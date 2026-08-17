import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
LVGL = ROOT / ".pio" / "libdeps" / "tdeck" / "lvgl"


def test_actual_nomadnet_screen_320x240_acceptance(tmp_path):
    resolved = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "resolve_lvgl.py"), "--root", str(ROOT)],
        capture_output=True,
        text=True,
        timeout=180,
    )
    assert resolved.returncode == 0, resolved.stdout + resolved.stderr
    assert LVGL.is_dir()
    cmake = shutil.which("cmake")
    assert cmake
    build = tmp_path / "build"
    configured = subprocess.run([
        cmake, "-S", str(HERE / "nomadnet_lvgl_acceptance"), "-B", str(build),
        f"-DPYXIS_ROOT={ROOT}", f"-DLVGL_SOURCE={LVGL}",
    ], capture_output=True, text=True)
    assert configured.returncode == 0, configured.stdout + configured.stderr
    compiled = subprocess.run([cmake, "--build", str(build), "-j2"],
                              capture_output=True, text=True, timeout=180)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    result = subprocess.run([str(build / "nomadnet_lvgl_acceptance")],
                            capture_output=True, text=True, timeout=30, env=env)
    assert result.returncode == 0, result.stdout + result.stderr
    output = result.stdout.strip()
    assert "fit_tier=1 fit_columns=1" in output
    assert "reflow_tier=1 reflow_cards=1" in output
    assert "eight_column_tier=1 eight_column_preserved=1 eight_column_pixels=1 table_link_focus=1 eight_column_objects=1" in output
    assert "focus_events=1 edge_scroll=1" in output
    assert "ready=1 cancel=1 enter=1 escape=1 focus_restore=1" in output
    assert "teardown=1 cached_status_transient=1 cached_status_oom_collapses=1 stale_group=0" in output
    assert "background_pixels=1 table_pixels=1 form_pixels=1 focus_pixels=1 glyph_pixels=1" in output
    assert "exact_fonts=1" in output
    assert output.endswith("objects=0")
