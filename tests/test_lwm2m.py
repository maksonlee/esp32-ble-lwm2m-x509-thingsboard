"""Build real application fault tests against pinned Anjay public interfaces."""
from pathlib import Path
import socket
import struct
import subprocess
import unittest
from host_env import native_environment

ROOT = Path(__file__).resolve().parents[1]


class Lwm2mTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build = ROOT / "build/native-lwm2m"
        environment = native_environment()
        subprocess.run(["cmake", "-S", str(ROOT / "tests/lwm2m"), "-B", str(build), "-G", "Ninja"],
                       check=True, stdout=subprocess.DEVNULL, env=environment)
        subprocess.run(["cmake", "--build", str(build), "--target", "test_lwm2m", "observe_probe", "--parallel", "2"],
                       check=True, stdout=subprocess.DEVNULL, env=environment)
        cls.build = build

    def test_startup_and_sample_freshness(self):
        subprocess.run([str(self.build / "test_lwm2m")], check=True)

    def test_observe_identical_samples_and_failed_read(self):
        # An independent UDP peer checks the actual CoAP/TLV notification bytes.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0))
            server.settimeout(5)
            client = subprocess.Popen([str(self.build / "observe_probe"),
                                       f"coap://127.0.0.1:{server.getsockname()[1]}"],
                                      stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL, text=True)
            self.addCleanup(self.stop_probe, client)
            register, peer = server.recvfrom(8192)
            code, mid, token, options, payload = decode_coap(register)
            self.assertEqual(code, 2)
            self.assertIn((15, b"lwm2m=1.1"), options)
            # Leshan rejects quoted object versions even for LwM2M 1.0 clients.
            self.assertIn(b"</3303>;ver=1.1,</3303/0>", payload)
            self.assertIn(b"</3304>;ver=1.1,</3304/0>", payload)
            self.assertNotIn(b'ver="', payload)
            server.sendto(encode_coap(2, 65, mid, token, [(8, b"rd"), (8, b"test")]), peer)
            server.sendto(encode_coap(0, 1, 300, b"T", [(6, b""), (11, b"3303"),
                                                      (11, b"0"), (17, (11542).to_bytes(2, "big"))]), peer)

            def receive():
                packet, sender = server.recvfrom(8192)
                self.assertEqual(sender, peer)
                code, mid, token, options, payload = decode_coap(packet)
                self.assertEqual(code, 69)
                self.assertEqual(token, b"T")
                self.assertIn(6, dict(options))
                if (packet[0] >> 4) & 3 == 0:
                    server.sendto(encode_coap(2, 0, mid, b"", []), peer)
                return decode_tlv(payload)

            initial = receive()
            self.assertNotIn(5700, initial)  # No fabricated zero before first sample.
            for timestamp in (1900000000, 1900000005):
                client.stdin.write(f"sample {timestamp}\n"); client.stdin.flush()
                values = receive()
                self.assertEqual(struct.unpack(">d" if len(values[5700]) == 8 else ">f", values[5700])[0], 23)
                self.assertEqual(int.from_bytes(values[5518], "big", signed=True), timestamp)
            client.stdin.write("fail\n"); client.stdin.flush()
            server.settimeout(0.4)
            with self.assertRaises(socket.timeout):
                server.recvfrom(8192)
            server.settimeout(5)
            client.stdin.write("sample 1900000015\n"); client.stdin.flush()
            self.assertEqual(int.from_bytes(receive()[5518], "big", signed=True), 1900000015)
            client.stdin.write("quit\n"); client.stdin.flush()
            deregister, _ = server.recvfrom(8192)
            code, mid, token, _, _ = decode_coap(deregister)
            self.assertEqual(code, 4)
            server.sendto(encode_coap(2, 66, mid, token, []), peer)
            self.assertEqual(client.wait(timeout=5), 0)

    @staticmethod
    def stop_probe(client):
        if client.poll() is None:
            client.terminate()
            client.wait(timeout=5)
        client.stdin.close()


def encode_coap(kind, code, mid, token, options):
    result = bytearray([0x40 | kind << 4 | len(token), code]) + mid.to_bytes(2, "big") + token
    last = 0
    for number, value in options:
        delta = number - last
        assert 0 <= delta < 13 and len(value) < 13
        result.append(delta << 4 | len(value))
        result.extend(value)
        last = number
    return result


def decode_coap(data):
    position = 4 + (data[0] & 15)
    token, options, number = data[4:position], [], 0
    while position < len(data) and data[position] != 255:
        header = data[position]; position += 1
        parts = []
        for nibble in (header >> 4, header & 15):
            if nibble == 13:
                nibble = 13 + data[position]; position += 1
            elif nibble == 14:
                nibble = 269 + int.from_bytes(data[position:position + 2], "big"); position += 2
            assert nibble != 15
            parts.append(nibble)
        number += parts[0]
        options.append((number, data[position:position + parts[1]]))
        position += parts[1]
    return data[1], int.from_bytes(data[2:4], "big"), token, options, data[position + 1:]


def decode_tlv(data):
    result, position = {}, 0
    while position < len(data):
        header = data[position]; position += 1
        assert header >> 6 == 3  # Single-instance Resource with Value.
        id_size = 2 if header & 32 else 1
        rid = int.from_bytes(data[position:position + id_size], "big"); position += id_size
        length_size = (header >> 3) & 3
        if length_size:
            length = int.from_bytes(data[position:position + length_size], "big"); position += length_size
        else:
            length = header & 7
        result[rid] = data[position:position + length]; position += length
    return result
