"""Exercise the real OTA platform module with isolated SDK fault substitutes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from host_env import native_environment

OTA = Path(__file__).parent / "ota"


class OtaPlatformTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.executable = Path(cls.directory.name) / "ota_platform"
        subprocess.run([
            "cc", "-std=c11", "-g", "-fsanitize=address,undefined",
            "-I", str(OTA), str(OTA / "test_platform.c"),
            "-o", str(cls.executable),
        ], check=True, env=native_environment())

    def run_scenario(self, scenario):
        subprocess.run([str(self.executable), scenario], check=True)

    def test_split_headers_and_identity(self):
        self.run_scenario("headers")

    def test_size_and_complete_image_validation(self):
        self.run_scenario("image")

    def test_flash_failure_cleanup(self):
        self.run_scenario("flash")

    def test_reset_disconnect_and_install_order(self):
        self.run_scenario("lifecycle")

    def test_persistent_results_and_power_failures(self):
        self.run_scenario("journal")

    def test_offline_boot_deadline_and_confirmation(self):
        self.run_scenario("guard")


if __name__ == "__main__":
    unittest.main()
