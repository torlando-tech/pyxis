Import("env")
import os
import subprocess

version = os.environ.get("PYXIS_VERSION_OVERRIDE")
if not version:
    try:
        version = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            stderr=subprocess.DEVNULL
        ).decode().strip().lstrip("v")
    except Exception:
        version = "dev"

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(version))])
