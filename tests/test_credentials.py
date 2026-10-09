"""Use disposable PKI fixtures, never the board's certificate or private key."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("device", Path(__file__).parents[1] / "tools/device.py")
device = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device)


class CredentialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.base = Path(cls.temp.name)

        def run(*args):
            subprocess.run(["openssl", *args], cwd=cls.base, check=True, capture_output=True)

        run("req", "-x509", "-newkey", "rsa:2048", "-noenc", "-keyout", "root.key",
            "-out", "root.pem", "-subj", "/CN=Test Root", "-days", "365",
            "-addext", "basicConstraints=critical,CA:TRUE")
        for name, cn in [("issuer", "Test Issuer"), ("leaf", "Test Device")]:
            run("req", "-new", "-newkey", "rsa:2048", "-noenc", "-keyout", name + ".key",
                "-out", name + ".csr", "-subj", "/CN=" + cn)
            issuer = "root" if name == "issuer" else "issuer"
            extension = "basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n" if name == "issuer" else "basicConstraints=CA:FALSE\nextendedKeyUsage=clientAuth\nkeyUsage=digitalSignature,keyEncipherment\n"
            (cls.base / "ext.cnf").write_text(extension)
            run("x509", "-req", "-in", name + ".csr", "-CA", issuer + ".pem",
                "-CAkey", issuer + ".key", "-CAcreateserial", "-days", "365",
                "-extfile", "ext.cnf", "-out", name + ".pem")

    def setUp(self):
        self.dir = Path(self.temp.name) / self._testMethodName
        self.dir.mkdir()
        self.blocks = [(self.base / (name + ".pem")).read_bytes() for name in ("leaf", "issuer", "root")]
        (self.dir / "device.crt").write_bytes(b"".join(self.blocks))
        shutil.copyfile(self.base / "root.pem", self.dir / "root_ca.crt")
        shutil.copyfile(self.base / "leaf.key", self.dir / "device.key")
        (self.dir / "device.key").chmod(0o600)

    def check(self):
        return device.check_credentials(self.dir, "Test Device")

    def test_valid_bundle(self):
        self.assertEqual(self.check()["chain_certificates"], 3)

    def test_server_trust_can_differ_from_device_issuer(self):
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec",
                        "-pkeyopt", "ec_paramgen_curve:P-256", "-noenc", "-days", "365",
                        "-subj", "/CN=Independent Server Root", "-addext", "basicConstraints=critical,CA:TRUE",
                        "-keyout", str(self.base / "independent.key"),
                        "-out", str(self.dir / "root_ca.crt")], check=True, capture_output=True)
        self.assertEqual(self.check()["chain_certificates"], 3)

    def test_wrong_chain_order(self):
        (self.dir / "device.crt").write_bytes(self.blocks[0] + self.blocks[2] + self.blocks[1])
        with self.assertRaisesRegex(ValueError, "order"):
            self.check()

    def test_mismatched_private_key(self):
        shutil.copyfile(self.base / "issuer.key", self.dir / "device.key")
        with self.assertRaisesRegex(ValueError, "do not match"):
            self.check()

    def test_wrong_device(self):
        with self.assertRaisesRegex(ValueError, "CN"):
            device.check_credentials(self.dir, "Another Device")

    def test_expiry_margin(self):
        with self.assertRaises(ValueError):
            device.check_credentials(self.dir, "Test Device", min_days=400)

    def test_private_key_permissions(self):
        (self.dir / "device.key").chmod(0o644)
        with self.assertRaisesRegex(ValueError, "owner"):
            self.check()

    def test_reject_mixed_pem(self):
        with (self.dir / "device.crt").open("ab") as f:
            f.write(b"not a certificate")
        with self.assertRaisesRegex(ValueError, "only PEM"):
            self.check()

    def test_reject_symlinks(self):
        (self.dir / "device.key").unlink()
        (self.dir / "device.key").symlink_to(self.base / "leaf.key")
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.check()
