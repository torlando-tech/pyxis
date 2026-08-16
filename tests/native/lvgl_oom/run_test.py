#!/usr/bin/env python3
"""Build patched generated LVGL with a failing allocator and test password mode."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import tempfile


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lvgl-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    lvgl = args.lvgl_dir.resolve()
    template = (lvgl / "lv_conf_template.h").read_text()

    with tempfile.TemporaryDirectory(prefix="pyxis-lvgl-oom-") as temporary:
        root = pathlib.Path(temporary)
        config = root / "lv_conf.h"
        allocator = root / "fail_alloc.h"
        build = root / "build"
        executable = root / "test_password_mode_oom"

        config.write_text(
            template.replace(
                '#if 0 /*Set it to "1" to enable content*/',
                '#if 1 /* password-mode OOM test configuration */',
                1,
            )
            .replace("#define LV_MEM_CUSTOM 0", "#define LV_MEM_CUSTOM 1", 1)
            .replace(
                "#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>",
                f'#define LV_MEM_CUSTOM_INCLUDE "{allocator}"',
                1,
            )
            .replace("#define LV_MEM_CUSTOM_ALLOC   malloc", "#define LV_MEM_CUSTOM_ALLOC   test_malloc", 1)
            .replace("#define LV_MEM_CUSTOM_FREE    free", "#define LV_MEM_CUSTOM_FREE    test_free", 1)
            .replace("#define LV_MEM_CUSTOM_REALLOC realloc", "#define LV_MEM_CUSTOM_REALLOC test_realloc", 1)
            .replace("#define LV_USE_ASSERT_MALLOC        1", "#define LV_USE_ASSERT_MALLOC        0", 1)
        )
        allocator.write_text(
            "#pragma once\n#include <stddef.h>\n"
            "void *test_malloc(size_t);\n"
            "void *test_realloc(void *, size_t);\n"
            "void test_free(void *);\n"
        )

        run([
            "cmake", "-S", str(lvgl), "-B", str(build),
            f'-DCMAKE_C_FLAGS=-DLV_CONF_PATH="{config}"',
        ])
        run(["cmake", "--build", str(build), "--target", "lvgl", "-j2"])
        run([
            shutil.which("cc") or "cc", "-std=c11",
            f"-DLV_CONF_PATH={config}", f"-I{lvgl}",
            str(pathlib.Path(__file__).with_name("test_password_mode_oom.c")),
            str(build / "lib" / "liblvgl.a"), "-lm", "-o", str(executable),
        ])
        run([str(executable)])

    print("generated LVGL password-mode allocation failure: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
