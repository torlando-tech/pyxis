"""
PlatformIO pre-build: mirror file:// lib_deps from their source trees
into .pio/libdeps before each build.

PlatformIO copies a `file://...` lib_dep into `.pio/libdeps/<env>/<lib>`
once on first install and never re-syncs even when the source changes.
That silently masks fixes — `pio run` succeeds and the device flashes,
but the firmware contains the OLD version of the source. Bit us hard
on a microReticulum security fix that lived in source for a full test
session before anyone noticed it hadn't deployed.

For each registered file:// dep below, this script rsyncs the source
tree into the libdep cache, preserving timestamps so the linker sees
fresh source on every build.
"""
Import("env")
import os
import shutil
from pathlib import Path

# (source_dir, libdep_subdir_name)
FILE_DEPS = [
    ("~/repos/microReticulum", "microReticulum"),
]

PROJECT_DIR = Path(env.get("PROJECT_DIR", "."))
ENV_NAME = env.get("PIOENV", "tdeck")
LIBDEPS_BASE = PROJECT_DIR / ".pio" / "libdeps" / ENV_NAME


def mirror(src: Path, dst: Path):
    if not src.exists():
        print(f"SYNC: {src} missing, skipping")
        return False
    if not dst.exists():
        # Let PIO do the first install; we only refresh after that.
        print(f"SYNC: {dst} not yet installed, skipping (PIO will fetch)")
        return False
    src_files = 0
    copied = 0
    for root, dirs, files in os.walk(src):
        # Skip git, build artifacts.
        dirs[:] = [d for d in dirs if d not in (".git", ".pio", "__pycache__", "build")]
        rel = Path(root).relative_to(src)
        for fn in files:
            if fn.endswith((".pyc", ".o", ".a", ".elf", ".bin")):
                continue
            sp = Path(root) / fn
            dp = dst / rel / fn
            src_files += 1
            if not dp.exists() or sp.stat().st_mtime > dp.stat().st_mtime:
                dp.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(sp, dp)
                copied += 1
    if copied > 0:
        print(f"SYNC: {dst.name}: refreshed {copied}/{src_files} files from {src}")
    return True


for src, name in FILE_DEPS:
    mirror(Path(src), LIBDEPS_BASE / name)
