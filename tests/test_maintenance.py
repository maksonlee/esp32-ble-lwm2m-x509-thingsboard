import importlib.util
import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("maintenance_device", Path(__file__).parents[1] / "tools/device.py")
device = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device)


class MaintenanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        (root / "partition_table").mkdir()
        (root / "partition_table/partition-table.bin").write_bytes(b"expected table")
        self.args = SimpleNamespace(build=root, cert_only=True, port="FAKE",
                                    expected_mac="02:00:00:00:00:01", image=root / "image.bin")

    def test_wrong_board_never_writes(self):
        with patch.object(device, "make_image", return_value={}), \
             patch.object(device, "command", return_value=b"MAC: 00:00:00:00:00:00") as command:
            with self.assertRaisesRegex(ValueError, "MAC"):
                device.flash(self.args)
            self.assertEqual(command.call_count, 1)

    def test_certificate_update_rejects_old_partition_table(self):
        def command(args):
            if "read-mac" in args: return b"MAC: 02:00:00:00:00:01"
            self.assertIn("read-flash", args)
            Path(args[-1]).write_bytes(b"old table")
            return b""
        with patch.object(device, "make_image", return_value={}), patch.object(device, "command", side_effect=command):
            with self.assertRaisesRegex(ValueError, "partition table differs"):
                device.flash(self.args)

    def test_certificate_update_writes_only_spiffs(self):
        writes = []
        def command(args):
            if "read-mac" in args: return b"MAC: 02:00:00:00:00:01"
            if "read-flash" in args:
                Path(args[-1]).write_bytes(b"expected table" + b"\xff" * 16)
            else: writes.append(args)
            return b""
        with patch.object(device, "make_image", return_value={}), patch.object(device, "command", side_effect=command):
            device.flash(self.args)
        self.assertEqual(len(writes), 1)
        self.assertEqual(writes[0][-3:], ["write-flash", "0x3b0000", str(self.args.image)])

    def test_production_gate_rejects_unprotected_secret_storage(self):
        config = self.args.build / "config"
        config.mkdir()
        (config / "sdkconfig.json").write_text(json.dumps({"SECURE_BOOT": True, "SECURE_FLASH_ENC_ENABLED": True}))
        with self.assertRaisesRegex(ValueError, "SPIFFS"):
            device.production_check(self.args.build)

    def test_image_rejects_wrong_lwm2m_endpoint(self):
        directory = self.args.build / "bundle"
        directory.mkdir()
        for name in ("device.crt", "device.key", "root_ca.crt", "provisioning.pop"):
            (directory / name).write_bytes(b"test-placeholder-1234")
            (directory / name).chmod(0o600)
        config = self.args.build / "config"
        config.mkdir()
        (config / "sdkconfig.json").write_text(json.dumps({"LWM2M_ENDPOINT": "Wrong Device"}))
        self.args.certs_dir = directory
        self.args.device_name = "Test Device"
        self.args.min_days = 30
        self.args.generate_pop = False
        with patch.object(device, "check_credentials", return_value={}), \
             patch.object(device, "partition_layout", return_value={}), \
             patch.dict(os.environ, {"IDF_PATH": "/unused"}), \
             patch.object(device, "command") as command:
            with self.assertRaisesRegex(ValueError, "endpoint"):
                device.make_image(self.args)
            command.assert_not_called()
