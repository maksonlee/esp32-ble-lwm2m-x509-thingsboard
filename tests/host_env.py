"""Select host binutils even when ESP-IDF's ULP tools precede them in PATH."""
import os
from pathlib import Path
import shutil


def native_environment():
    compiler = shutil.which("cc")
    if not compiler:
        raise RuntimeError("Host C compiler 'cc' is required for native tests")
    environment = os.environ.copy()
    # GCC may find ld through PATH; the ULP SDK also provides an unprefixed ld.
    directory = str(Path(compiler).resolve().parent)
    environment["PATH"] = directory + os.pathsep + environment.get("PATH", "")
    environment["CC"] = compiler
    return environment
