"""Exercise the application Firmware Update bridge over loopback CoAP."""
from contextlib import contextmanager
import json
from pathlib import Path
import select
import socket
import subprocess
import unittest

from host_env import native_environment
from test_lwm2m import decode_coap

ROOT = Path(__file__).resolve().parents[1]


def encode_coap(kind, code, mid, token, options):
    result = bytearray([0x40 | kind << 4 | len(token), code]) + mid.to_bytes(2, "big") + token

    def option_integer(value):
        if value < 13:
            return value, b""
        if value < 269:
            return 13, bytes([value - 13])
        return 14, (value - 269).to_bytes(2, "big")

    previous = 0
    for number, value in options:
        delta, delta_extra = option_integer(number - previous)
        length, length_extra = option_integer(len(value))
        result.append(delta << 4 | length)
        result.extend(delta_extra + length_extra + value)
        previous = number
    return result


class FirmwarePeer:
    """A separate peer; no Anjay internals or production credentials are used."""

    def __init__(self, case, server, client, address):
        self.case = case
        self.server = server
        self.client = client
        self.address = address
        self.mid = 500

    def packet(self, method, resource, payload=None, *, block=None, token=None, object_id=5):
        self.mid += 1
        token = token if token is not None else self.mid.to_bytes(2, "big")
        options = [(11, str(object_id).encode()), (11, b"0")]
        options.extend((11, part.encode()) for part in str(resource).split("/"))
        if payload is not None:
            options.append((12, b"*" if resource == 0 else b""))
        if block is not None:
            options.append((27, block.to_bytes((block.bit_length() + 7) // 8, "big")))
        packet = encode_coap(0, method, self.mid, token, options)
        if payload:
            packet.extend(b"\xff" + payload)
        return packet

    def exchange(self, packet):
        self.server.sendto(packet, self.address)
        expected_token = decode_coap(packet)[2]
        while True:
            response, address = self.server.recvfrom(8192)
            self.case.assertEqual(address, self.address)
            decoded = decode_coap(response)
            code, mid, token, _, _ = decoded
            if code == 0:  # Separate CoAP response may follow an empty ACK.
                continue
            self.case.assertEqual(token, expected_token)
            if (response[0] >> 4) & 3 == 0:
                self.server.sendto(encode_coap(2, 0, mid, b"", []), self.address)
            return decoded

    def read(self, resource, object_id=5):
        code, _, _, _, payload = self.exchange(self.packet(1, resource, object_id=object_id))
        self.case.assertEqual(code, 69)
        return payload

    def state_and_result(self):
        return int(self.read(3)), int(self.read(5))

    def stats(self):
        self.client.stdin.write("stats\n")
        self.client.stdin.flush()
        self.case.assertTrue(select.select([self.client.stdout], [], [], 5)[0],
                             "Firmware probe did not report its platform calls")
        return json.loads(self.client.stdout.readline())

    def push(self, payload, *, duplicate=False):
        # Block1 SZX=4 is 256 bytes. A stable token identifies one transfer.
        blocks = [payload[offset:offset + 256] for offset in range(0, len(payload), 256)]
        for number, data in enumerate(blocks):
            more = number + 1 < len(blocks)
            packet = self.packet(3, 0, data, block=(number << 4) | (8 if more else 0) | 4,
                                 token=b"FW")
            response = self.exchange(packet)
            if more:
                self.case.assertEqual(response[0], 95)  # 2.31 Continue
                self.case.assertIn(27, dict(response[3]))
            if duplicate and number in (0, len(blocks) - 1):
                repeated = self.exchange(packet)
                self.case.assertEqual(repeated, response)
        return response[0]


class OtaWireTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = ROOT / "build/native-lwm2m"
        environment = native_environment()
        subprocess.run(["cmake", "-S", str(ROOT / "tests/lwm2m"), "-B", str(cls.build),
                        "-G", "Ninja"], check=True, stdout=subprocess.DEVNULL, env=environment)
        subprocess.run(["cmake", "--build", str(cls.build), "--target", "firmware_probe",
                        "--parallel", "2"], check=True, stdout=subprocess.DEVNULL,
                       env=environment)

    @contextmanager
    def peer(self, initial=0, mode="good"):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0))
            server.settimeout(5)
            client = subprocess.Popen(
                [str(self.build / "firmware_probe"),
                 f"coap://127.0.0.1:{server.getsockname()[1]}", str(initial), mode],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                text=True)
            try:
                register, address = server.recvfrom(8192)
                code, mid, token, options, payload = decode_coap(register)
                self.assertEqual(code, 2)
                self.assertIn((15, b"lwm2m=1.1"), options)
                self.assertIn(b"</5/0>", payload)
                self.assertIn(b"</3/0>", payload)
                server.sendto(encode_coap(2, 65, mid, token,
                                          [(8, b"rd"), (8, b"firmware-test")]), address)
                yield FirmwarePeer(self, server, client, address)
            finally:
                if client.poll() is None:
                    client.terminate()
                    client.wait(timeout=5)
                client.stdin.close()
                client.stdout.close()

    def test_push_only_and_execute_requires_download(self):
        with self.peer() as peer:
            self.assertEqual(peer.read(9), b"1")  # Push only.
            self.assertEqual(peer.read(3, object_id=3), b"v01")
            self.assertEqual(peer.read("11/0", object_id=3), b"0")
            self.assertEqual(peer.read(16, object_id=3), b"U")
            self.assertEqual(peer.state_and_result(), (0, 0))
            self.assertEqual(peer.exchange(peer.packet(2, 2))[0], 133)  # 4.05
            self.assertEqual(peer.stats()["upgrades"], 0)
            self.assertEqual(peer.exchange(peer.packet(
                3, 1, b"https://example.com/firmware.bin"))[0], 128)  # 4.00
            self.assertEqual(peer.state_and_result(), (0, 9))
            self.assertEqual(peer.stats()["opens"], 0)

    def test_blockwise_push_duplicate_and_execute(self):
        package = b"\xe9" + bytes(range(256)) * 3
        with self.peer() as peer:
            self.assertEqual(peer.push(package, duplicate=True), 68)  # 2.04
            self.assertEqual(peer.state_and_result(), (2, 0))
            self.assertEqual(peer.read(6), b"isolated-firmware-test")
            self.assertEqual(peer.read(7), b"v02")
            stats = peer.stats()
            self.assertEqual(stats["opens"], 1)
            self.assertEqual(stats["finishes"], 1)
            self.assertEqual(stats["size"], len(package))
            self.assertEqual(bytes.fromhex(stats["hex"]), package)
            self.assertEqual(stats["upgrades"], 0)
            self.assertEqual(peer.exchange(peer.packet(2, 2))[0], 68)
            self.assertEqual(peer.state_and_result(), (3, 0))
            self.assertEqual(peer.stats()["upgrades"], 1)
            self.assertEqual(peer.exchange(peer.packet(2, 2))[0], 133)
            self.assertEqual(peer.stats()["upgrades"], 1)

    def test_invalid_and_oversize_images_report_results(self):
        package = b"\xe9" + bytes(range(256)) * 2
        failures = (("bad", 68, 5), ("unsupported", 68, 6),
                    ("space", 160, 2), ("memory", 160, 3))
        for mode, expected_code, expected_result in failures:
            with self.subTest(mode=mode), self.peer(mode=mode) as peer:
                self.assertEqual(peer.push(package), expected_code)
                self.assertEqual(peer.state_and_result(), (0, expected_result))
                self.assertEqual(peer.stats()["upgrades"], 0)
                self.assertEqual(peer.stats()["status"], "failure")
                peer.client.stdin.write("good\n")
                peer.client.stdin.flush()
                peer.stats()
                self.assertEqual(peer.push(package), 68)
                self.assertEqual(peer.state_and_result(), (2, 0))

    def test_cancel_downloaded_package(self):
        for resource, payload in ((1, b""), (0, b"\x00")):
            with self.subTest(resource=resource), self.peer() as peer:
                self.assertEqual(peer.push(b"\xe9" + bytes(range(256))), 68)
                self.assertEqual(peer.state_and_result(), (2, 0))
                # Standard empty Package URI and one-zero-byte Package reset.
                self.assertEqual(peer.exchange(peer.packet(3, resource, payload))[0], 68)
                self.assertEqual(peer.state_and_result(), (0, 0))
                self.assertGreater(peer.stats()["resets"], 0)
                self.assertEqual(peer.exchange(peer.packet(2, 2))[0], 133)
                # Idle-state one-byte cancellation passes through stream_open/write.
                self.assertEqual(peer.exchange(peer.packet(3, 0, b"\x00"))[0], 68)
                self.assertEqual(peer.state_and_result(), (0, 0))
                self.assertEqual(peer.stats()["status"], "idle")

    def test_interrupted_blockwise_transfer_retains_failure(self):
        with self.peer() as peer:
            peer.client.stdin.write("short-timeout\n")
            peer.client.stdin.flush()
            peer.stats()
            first_block = peer.packet(3, 0, b"\xe9" + bytes(range(255)),
                                      block=12, token=b"FW")
            self.assertEqual(peer.exchange(first_block)[0], 95)
            # No subsequent block: Anjay's input reader expires and resets the stream.
            stats = peer.stats()
            self.assertEqual(stats["status"], "failure")
            self.assertEqual(stats["size"], 0)
            self.assertEqual(peer.state_and_result(), (0, 4))
            self.assertEqual(peer.push(b"\xe9" + bytes(range(256))), 68)
            self.assertEqual(peer.state_and_result(), (2, 0))

    def test_persisted_results_restore_after_client_creation(self):
        for initial, state, result in ((1, 0, 1), (8, 0, 8), (-2, 2, 0), (-3, 3, 0)):
            with self.subTest(initial=initial), self.peer(initial=initial) as peer:
                self.assertEqual(peer.state_and_result(), (state, result))
                self.assertEqual(peer.stats()["opens"], 0)
                if initial == -3:
                    peer.client.stdin.write("registered\n")
                    peer.client.stdin.flush()
                    peer.stats()  # Synchronize after the registration callback.
                    self.assertEqual(peer.state_and_result(), (0, 1))
                    self.assertEqual(peer.stats()["status"], "success")

    def test_failed_apply_or_confirmation_never_report_success(self):
        with self.peer(mode="upgrade-failure") as peer:
            self.assertEqual(peer.push(b"\xe9" + bytes(range(256))), 68)
            self.assertEqual(peer.exchange(peer.packet(2, 2))[0], 68)
            peer.read(3)  # Let the deferred upgrade handler run.
            self.assertEqual(peer.state_and_result(), (2, 8))
            self.assertEqual(peer.stats()["upgrades"], 1)
        with self.peer(initial=-3, mode="confirm-failure") as peer:
            peer.client.stdin.write("registered\n")
            peer.client.stdin.flush()
            peer.stats()
            self.assertEqual(peer.state_and_result(), (3, 0))


if __name__ == "__main__":
    unittest.main()
