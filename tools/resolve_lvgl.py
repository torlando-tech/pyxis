#!/usr/bin/env python3
"""Resolve and attest the exact LVGL tree used by the native acceptance test.

The digest input is the lexicographically sorted POSIX-relative path set made of
all ``src/**/*.c`` and ``src/**/*.h`` files (every source/header compiled or
included by LVGL's CMake target), plus the root build/public headers and library
metadata listed in ``ROOT_FILES``. Each record is ``relative_path + NUL +
file_bytes`` with no extra delimiter. Generated build files, examples, demos,
documentation and VCS metadata are deliberately excluded from this stable set.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

LVGL_VERSION = "8.4.0"
LVGL_VANILLA_TREE_SHA256 = "9a6ca0d597f4a17792c04a19104482c65e5af20401414a3c393c8d8a6a175a47"
LVGL_TREE_SHA256 = "be9f52b7aa9ca3fe379569cdf59fe43ebd9e676bc775b72b86cde91c59808ec2"
DEFAULT_PLATFORMIO = Path("/tmp/pyxis-platformio/bin/pio")
ROOT_FILES = (
    "CMakeLists.txt",
    "library.json",
    "library.properties",
    "lv_conf_template.h",
    "lvgl.h",
)


class ResolutionError(RuntimeError):
    pass


def stable_source_files(lvgl: Path) -> list[Path]:
    files = [lvgl / relative for relative in ROOT_FILES]
    src = lvgl / "src"
    files.extend(path for path in src.rglob("*") if path.is_file() and path.suffix in {".c", ".h"})
    missing = [path for path in files if not path.is_file()]
    if missing:
        raise ResolutionError(f"LVGL attested source set is incomplete: {missing[0]}")
    return sorted(files, key=lambda path: path.relative_to(lvgl).as_posix())


def source_tree_digest(lvgl: Path) -> str:
    digest = hashlib.sha256()
    for path in stable_source_files(lvgl):
        relative = path.relative_to(lvgl).as_posix().encode("utf-8")
        digest.update(relative)
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()


def verify_version(lvgl: Path) -> None:
    metadata_path = lvgl / "library.json"
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ResolutionError(f"cannot read LVGL library metadata: {error}") from error
    version = metadata.get("version")
    if version != LVGL_VERSION:
        raise ResolutionError(f"expected LVGL version {LVGL_VERSION}, found {version}")


def apply_repository_patch(root: Path, lvgl: Path) -> None:
    patch_script = root / "patch_lvgl_textarea.py"
    if not patch_script.is_file():
        raise ResolutionError(f"repository LVGL patch script is missing: {patch_script}")
    completed = subprocess.run(
        [sys.executable, str(patch_script), "--lvgl-root", str(lvgl)],
        cwd=root, check=False,
    )
    if completed.returncode != 0:
        raise ResolutionError(
            f"repository LVGL patch failed with exit {completed.returncode}"
        )


def resolve(root: Path) -> tuple[Path, str]:
    lvgl = root / ".pio" / "libdeps" / "tdeck" / "lvgl"
    if not lvgl.is_dir():
        pio = Path(os.environ.get("PYXIS_PLATFORMIO_BIN", str(DEFAULT_PLATFORMIO)))
        if not pio.is_file() or not os.access(pio, os.X_OK):
            raise ResolutionError(
                f"LVGL is missing and PlatformIO is not executable at {pio}; "
                "set PYXIS_PLATFORMIO_BIN to the CI PlatformIO executable"
            )
        completed = subprocess.run(
            [str(pio), "pkg", "install", "-e", "tdeck"], cwd=root, check=False
        )
        if completed.returncode != 0:
            raise ResolutionError(
                f"PlatformIO dependency resolution failed with exit {completed.returncode}"
            )
        if not lvgl.is_dir():
            raise ResolutionError(f"PlatformIO completed but did not create {lvgl}")
    verify_version(lvgl)
    before = source_tree_digest(lvgl)
    if before not in {LVGL_VANILLA_TREE_SHA256, LVGL_TREE_SHA256}:
        raise ResolutionError(
            "LVGL source digest mismatch before patch: expected vanilla "
            f"{LVGL_VANILLA_TREE_SHA256} or patched {LVGL_TREE_SHA256}, found {before}"
        )
    # The only executable used after package resolution is the repository's
    # narrowly-scoped LVGL patcher in standalone mode. PlatformIO build hooks
    # and unrelated dependency patches are never invoked by this resolver.
    apply_repository_patch(root, lvgl)
    actual = source_tree_digest(lvgl)
    if actual != LVGL_TREE_SHA256:
        raise ResolutionError(
            f"LVGL source digest mismatch after patch: expected {LVGL_TREE_SHA256}, found {actual}"
        )
    return lvgl, actual


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        lvgl, digest = resolve(args.root.resolve())
    except ResolutionError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"LVGL {LVGL_VERSION} {digest} {lvgl}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
