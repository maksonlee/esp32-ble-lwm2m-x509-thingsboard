#!/usr/bin/env python3
"""Validate credentials and prepare/flash this project's USB maintenance images.

The tool validates a local device bundle, generates a SPIFFS image and writes
firmware or certificates over USB after verifying the target board.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import ssl
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PEM = re.compile(rb"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----\s*", re.S)


def command(args, **kwargs):
    result = subprocess.run(args, capture_output=True, **kwargs)
    if result.returncode:
        # Raw subprocess output can contain sensitive data. Report only operation.
        raise ValueError(f"{Path(str(args[0])).name} {args[1]} failed (exit {result.returncode})")
    return result.stdout


def secure_write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def certificate_blocks(path):
    data = path.read_bytes()
    blocks = PEM.findall(data)
    if not blocks or PEM.sub(b"", data).strip():
        raise ValueError(f"{path.name}: expected only PEM certificates")
    return blocks


def check_credentials(directory, device_name, min_days=30):
    directory = Path(directory)
    for name in ("device.crt", "device.key", "root_ca.crt"):
        path = directory / name
        if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= 16384:
            raise ValueError(f"{name}: missing, symlink, empty, or larger than 16 KiB")
    if (directory / "device.key").stat().st_mode & 0o077:
        raise ValueError("device.key must be accessible only to its owner (chmod 600)")
    blocks = certificate_blocks(directory / "device.crt")
    if len(blocks) < 2:
        raise ValueError("device.crt must include its issuing CA chain")
    with tempfile.TemporaryDirectory() as temporary:
        files = []
        for index, block in enumerate(blocks):
            path = Path(temporary) / f"cert-{index}.pem"
            path.write_bytes(block)
            files.append(path)
            command(["openssl", "x509", "-in", str(path), "-noout", "-checkend", str(min_days * 86400)])
        decoded = [ssl._ssl._test_decode_cert(str(path)) for path in files]
        cn = [v for rdn in decoded[0]["subject"] for k, v in rdn if k == "commonName"]
        if cn != [device_name]:
            raise ValueError("Device certificate CN does not match --device-name")
        for index in range(len(files) - 1):
            if decoded[index]["issuer"] != decoded[index + 1]["subject"]:
                raise ValueError("Incorrect PEM order: expected device -> intermediate(s) -> root")
            command(["openssl", "verify", "-no-CAfile", "-no-CApath", "-no-CAstore",
                     "-partial_chain", "-trusted", str(files[index + 1]), str(files[index])])
        if decoded[-1]["subject"] != decoded[-1]["issuer"]:
            raise ValueError("Include the root last in the maintenance certificate bundle")
        command(["openssl", "verify", "-check_ss_sig", "-CAfile", str(files[-1]), str(files[-1])])
        intermediates = Path(temporary) / "intermediates.pem"
        intermediates.write_bytes(b"".join(blocks[1:]))
        command(["openssl", "verify", "-purpose", "sslclient", "-CAfile", str(files[-1]),
                 "-untrusted", str(intermediates), str(files[0])])
        eku = command(["openssl", "x509", "-in", str(files[0]), "-noout", "-ext", "extendedKeyUsage"])
        if b"TLS Web Client Authentication" not in eku:
            raise ValueError("Device certificate must explicitly allow TLS client authentication")
        public_cert = command(["openssl", "x509", "-in", str(files[0]), "-pubkey", "-noout"])
        public_key = command(["openssl", "pkey", "-in", str(directory / "device.key"),
                              "-passin", "pass:", "-pubout"])
        if public_cert != public_key:
            raise ValueError("Device certificate and private key do not match")
        # The server trust root can differ from the device issuer root.
        for index, block in enumerate(certificate_blocks(directory / "root_ca.crt")):
            path = Path(temporary) / f"server-root-{index}.pem"
            path.write_bytes(block)
            command(["openssl", "verify", "-check_ss_sig", "-CAfile", str(path), str(path)])
            command(["openssl", "x509", "-in", str(path), "-noout", "-checkend", str(min_days * 86400)])
    return {"device": device_name, "chain_certificates": len(blocks),
            "certificate_sha256": hashlib.sha256(ssl.PEM_cert_to_DER_cert(blocks[0].decode())).hexdigest()}


def partition_layout(build):
    data = (build / "partition_table/partition-table.bin").read_bytes()
    result = {}
    for pos in range(0, len(data), 32):
        record = data[pos:pos + 32]
        if record[:2] != b"\xaa\x50":
            break
        _, kind, subtype, offset, size, label, _ = struct.unpack("<HBBII16sI", record)
        result[label.split(b"\0", 1)[0].decode()] = (offset, size)
    if result.get("spiffs") != (0x3B0000, 0x50000):
        raise ValueError("Build partition table does not match the supported full-4-MiB layout")
    if result.get("ota_0") != (0x10000, 0x1D0000) or result.get("ota_1") != (0x1E0000, 0x1D0000):
        raise ValueError("Unexpected OTA layout")
    return result


def production_check(build):
    config = json.loads((build / "config/sdkconfig.json").read_text())
    missing = []
    if not config.get("SECURE_BOOT"):
        missing.append("Secure Boot disabled")
    if not config.get("SECURE_FLASH_ENC_ENABLED"):
        missing.append("Flash Encryption disabled")
    # Encrypted SPIFFS is unsupported by ESP-IDF. Fail closed until storage migrates.
    missing.append("device key remains in unencrypted SPIFFS; migrate secret storage first")
    raise ValueError("Not production ready: " + "; ".join(missing))


def make_image(args):
    metadata = check_credentials(args.certs_dir, args.device_name, args.min_days)
    layout = partition_layout(args.build)
    pop = args.certs_dir / "provisioning.pop"
    if args.generate_pop and not pop.exists():
        secure_write(pop, secrets.token_urlsafe(24).encode())
    if not pop.is_file() or pop.is_symlink() or not 16 <= pop.stat().st_size <= 128:
        raise ValueError("Missing/invalid provisioning.pop; use --generate-pop once")
    if pop.stat().st_mode & 0o077:
        raise ValueError("provisioning.pop must be accessible only to its owner")
    if not re.fullmatch(rb"[A-Za-z0-9_-]{16,128}", pop.read_bytes()):
        raise ValueError("provisioning.pop must be a 16-128 character URL-safe secret without a newline")
    allowed = {"device.crt", "device.key", "root_ca.crt", "provisioning.pop"}
    if {p.name for p in args.certs_dir.iterdir()} != allowed:
        raise ValueError("Certificate directory must contain only the four documented device files")
    idf = os.environ.get("IDF_PATH")
    if not idf:
        raise ValueError("Activate ESP-IDF before generating the image")
    config = json.loads((args.build / "config/sdkconfig.json").read_text())
    if config.get("LWM2M_ENDPOINT") != args.device_name:
        raise ValueError("Firmware LwM2M endpoint does not match --device-name")
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory) / "spiffs.bin"
        options = ["--page-size", str(config["SPIFFS_PAGE_SIZE"]),
                   "--obj-name-len", str(config["SPIFFS_OBJ_NAME_LEN"]),
                   "--meta-len", str(config["SPIFFS_META_LENGTH"])]
        if not config.get("SPIFFS_USE_MAGIC"): options.append("--no-magic")
        if not config.get("SPIFFS_USE_MAGIC_LENGTH"): options.append("--no-magic-len")
        command([sys.executable, str(Path(idf) / "components/spiffs/spiffsgen.py"),
                 str(layout["spiffs"][1]), str(args.certs_dir), str(temporary), *options])
        secure_write(args.image, temporary.read_bytes())
    metadata["spiffs_bytes"] = args.image.stat().st_size
    return metadata


def flash(args):
    # Regenerate from validated files every time; never flash a stale image.
    metadata = make_image(args)
    prefix = [sys.executable, "-m", "esptool", "--chip", "esp32", "--port", args.port]
    identity = command([*prefix, "read-mac"]).decode(errors="replace").lower()
    macs = re.findall(r"mac:\s*([0-9a-f:]{17})", identity)
    if not macs or set(macs) != {args.expected_mac.lower()}:
        raise ValueError("Connected board MAC does not match --expected-mac; flash refused")
    if args.cert_only:
        with tempfile.TemporaryDirectory() as directory:
            table = Path(directory) / "partition-table.bin"
            command([*prefix, "read-flash", "0x8000", "0x1000", str(table)])
            expected = (args.build / "partition_table/partition-table.bin").read_bytes()
            if table.read_bytes()[:len(expected)] != expected:
                raise ValueError("On-device partition table differs; full firmware migration required")
        files = ["0x3b0000", str(args.image.resolve())]
        settings = []
    else:
        flash_config = json.loads((args.build / "flasher_args.json").read_text())
        settings = flash_config["write_flash_args"]
        files = []
        allowed = {"0x1000", "0x8000", "0xd000", "0x10000"}
        if set(flash_config["flash_files"]) != allowed:
            raise ValueError("Unexpected flash files; refusing to overwrite other partitions")
        for offset, filename in flash_config["flash_files"].items():
            path = (args.build / filename).resolve()
            if not path.is_relative_to(args.build.resolve()):
                raise ValueError("Flash artifact escapes build directory")
            if offset == "0x10000" and path.stat().st_size > 0x1D0000:
                raise ValueError("Firmware exceeds OTA slot")
            files.extend([offset, str(path)])
        files.extend(["0x3b0000", str(args.image.resolve())])
    command([*prefix, "--baud", "460800", "write-flash", *settings, *files])
    metadata["flash"] = "verified by esptool; NVS preserved"
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["check", "image", "flash", "production-check"])
    parser.add_argument("--device-name", required=True)
    parser.add_argument("--certs-dir", type=Path, default=ROOT / "spiffs_root")
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--image", type=Path, default=ROOT / "spiffs_image/spiffs.bin")
    parser.add_argument("--min-days", type=int, default=30)
    parser.add_argument("--generate-pop", action="store_true")
    parser.add_argument("--port")
    parser.add_argument("--expected-mac")
    parser.add_argument("--cert-only", action="store_true")
    args = parser.parse_args()
    if args.min_days < 0: parser.error("--min-days must be nonnegative")
    if args.action == "flash" and (not args.port or not args.expected_mac):
        parser.error("flash requires --port and --expected-mac")
    try:
        if args.action == "production-check": result = production_check(args.build)
        elif args.action == "check": result = check_credentials(args.certs_dir, args.device_name, args.min_days)
        elif args.action == "image": result = make_image(args)
        else: result = flash(args)
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, ssl.SSLError) as error:
        print(f"Maintenance failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
