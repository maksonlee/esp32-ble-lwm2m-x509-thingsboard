"""Compile and run native C fault-injection tests with ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from host_env import native_environment

NATIVE = Path(__file__).parent / "native"


class NativeTests(unittest.TestCase):
    def test_spiffs_faults(self):
        self.run_native("test_spiffs.c")

    def test_dht11_frames(self):
        self.run_native("test_dht11.c")

    def test_reprovision_button(self):
        self.run_native("test_button.c")

    def run_native(self, source):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "test"
            subprocess.run([
                "cc", "-std=c11", "-g", "-fsanitize=address,undefined",
                "-I", str(NATIVE), str(NATIVE / source), "-o", str(executable),
            ], check=True, env=native_environment())
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
