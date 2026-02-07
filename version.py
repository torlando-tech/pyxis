Import("env")
import subprocess

try:
    version = subprocess.check_output(
        ["git", "describe", "--tags", "--always"],
        stderr=subprocess.DEVNULL
    ).decode().strip().lstrip("v")
except Exception:
    version = "dev"

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(version))])
