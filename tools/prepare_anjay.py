#!/usr/bin/env python3
"""Fetch pinned dependencies and apply the shared ESP-IDF compatibility patches."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / ".deps"
ANJAY = "fdd70854c46f676acda179ad3a1b760eceada4db"
COMMONS = "e6c87eb58b3d605b1bd26eff816f3ad4b366b993"
PORT = "0c910a1aa0c812d1f4d1eb75b80fbce48241aef1"


def git(directory, *arguments, capture=False):
    return subprocess.run(["git", "-C", str(directory), *arguments], check=True,
                          text=True, stdout=subprocess.PIPE if capture else None,
                          timeout=600).stdout


def checkout(name, url, revision):
    directory = DEPS / name
    if not directory.exists():
        directory.mkdir(parents=True)
        git(directory, "init", "--quiet")
        git(directory, "remote", "add", "origin", url)
        git(directory, "fetch", "--quiet", "--depth=1", "origin", revision)
        git(directory, "checkout", "--quiet", "--detach", "FETCH_HEAD")
    if git(directory, "rev-parse", "HEAD", capture=True).strip() != revision:
        raise SystemExit(f"Unexpected revision in {directory}; preserve and inspect it manually")
    if git(directory, "diff", "--cached", capture=True):
        raise SystemExit(f"Staged dependency changes in {directory}; refusing to continue")
    return directory


def patch(directory, name):
    patch_file = ROOT / "components" / "anjay" / "patches" / name
    expected = patch_file.read_text()
    if git(directory, "diff", "--cached", capture=True):
        raise SystemExit(f"Staged dependency changes in {directory}; refusing to continue")
    if git(directory, "ls-files", "--others", "--exclude-standard", capture=True):
        raise SystemExit(f"Untracked dependency files in {directory}; refusing to continue")
    actual = git(directory, "diff", "--binary", "--ignore-submodules=all", capture=True)
    if actual == expected:
        return
    if actual:
        raise SystemExit(f"Unexpected local changes in {directory}; refusing to overwrite")
    git(directory, "apply", "--check", str(patch_file))
    git(directory, "apply", str(patch_file))
    assert git(directory, "diff", "--binary", "--ignore-submodules=all", capture=True) == expected


def main():
    DEPS.mkdir(exist_ok=True)
    core = checkout("anjay", "https://github.com/AVSystem/Anjay.git", ANJAY)
    if (git(core, "diff", "--ignore-submodules=all", capture=True)
            or git(core, "ls-files", "--others", "--exclude-standard", capture=True)):
        raise SystemExit("Unexpected Anjay core changes; refusing to continue")
    git(core, "submodule", "update", "--init", "--depth=1", "deps/avs_commons")
    commons = core / "deps/avs_commons"
    assert git(commons, "rev-parse", "HEAD", capture=True).strip() == COMMONS
    port = checkout("esp-port", "https://github.com/AVSystem/Anjay-esp-idf.git", PORT)
    patch(commons, "avs-commons-mbedtls4.patch")
    patch(port, "esp-port-config.patch")
    print("Pinned Anjay 3.15.0 sources and compatibility patches ready")


if __name__ == "__main__":
    main()
