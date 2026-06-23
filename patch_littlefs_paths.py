"""
PlatformIO pre-build script: patches the libdeps copy of microStore's
LittleFSFileSystem adapter so every file path is normalized to a leading "/".

Why: ESP32's Arduino LittleFS/VFS rejects any path that does not start with
"/" (it logs nothing and returns an invalid File). But microStore's own
adapter init-test uses "./__init_test__" and microReticulum's Transport path
store uses "./path_store" -> "./path_store_0.dat". Both are relative ("./")
paths that work on native POSIX but fail on-device: the LittleFS check
"fails" and reformats every boot, and `open_segment` can never open
"./path_store_0.dat", so the path table never persists and pyxis never learns
any peer's path ("Failed to add destination ... to path table!"). That blocks
all LXMF messaging.

This is a platform-correctness concern that belongs in the ESP32 adapter, so
we normalize "./x" / "x" -> "/x" at each LittleFS.* call site there. Native
builds (PosixFileSystem) are unaffected — they don't use this adapter.

Idempotent: detects the already-patched state via the _pyxis_norm_path marker.

TODO(upstream): attermann/microStore's LittleFSFileSystem adapter should
normalize paths to a leading "/" itself (file an issue / PR on the fork).
"""
Import("env")  # noqa: F821
import os
import sys
sys.path.insert(0, env.get("PROJECT_DIR", "."))  # noqa: F821
from _build_helpers import env_libdeps_dir  # per-env libdeps path; never hardcode the env

ADAPTER_H = env_libdeps_dir(env, "microStore", "include", "microStore", "Adapters", "LittleFSFileSystem.h")

MARKER = "_pyxis_norm_path"

HELPER = """namespace microStore { namespace Adapters {

// patched-by-pyxis: ESP32 Arduino LittleFS requires a leading "/". microStore
// and microReticulum use "./"-prefixed paths that work on POSIX but fail here.
static inline std::string _pyxis_norm_path(const char* p) {
\tstd::string s(p ? p : "");
\tif (s.rfind("./", 0) == 0) s.erase(0, 2);
\tif (s.empty() || s.front() != '/') s.insert(s.begin(), '/');
\treturn s;
}
"""

# (old, new) — order matters so the more-specific open() forms are rewritten
# before the bare open(path) form (which is a non-overlapping distinct string).
REPLACEMENTS = [
    ("LittleFS.open(path, pmode)",        "LittleFS.open(_pyxis_norm_path(path).c_str(), pmode)"),
    ("LittleFS.open(path, FILE_READ)",    "LittleFS.open(_pyxis_norm_path(path).c_str(), FILE_READ)"),
    ("LittleFS.open(path)",               "LittleFS.open(_pyxis_norm_path(path).c_str())"),
    ("LittleFS.exists(path)",             "LittleFS.exists(_pyxis_norm_path(path).c_str())"),
    ("LittleFS.remove(path)",             "LittleFS.remove(_pyxis_norm_path(path).c_str())"),
    ("LittleFS.rename(from_path, to_path)","LittleFS.rename(_pyxis_norm_path(from_path).c_str(), _pyxis_norm_path(to_path).c_str())"),
    ("LittleFS.mkdir(path)",              "LittleFS.mkdir(_pyxis_norm_path(path).c_str())"),
    ("LittleFS.rmdir(path)",              "LittleFS.rmdir(_pyxis_norm_path(path).c_str())"),
]


def main():
    if not os.path.exists(ADAPTER_H):
        # libdeps not fetched yet on a clean tree — PIO runs this again post-fetch.
        print(f"[patch_littlefs_paths] {ADAPTER_H} not present yet; skipping")
        return
    with open(ADAPTER_H) as f:
        src = f.read()
    if MARKER in src:
        print("[patch_littlefs_paths] already patched; skipping")
        return

    if "#include <string>" not in src:
        src = src.replace("#include <LittleFS.h>",
                          "#include <LittleFS.h>\n#include <string>", 1)

    # Inject the helper by expanding the namespace-open line.
    anchor = "namespace microStore { namespace Adapters {"
    if anchor not in src:
        print(f"[patch_littlefs_paths] FATAL: namespace anchor not found in {ADAPTER_H}")
        sys.exit(1)
    src = src.replace(anchor, HELPER, 1)

    for old, new in REPLACEMENTS:
        if old not in src:
            print(f"[patch_littlefs_paths] FATAL: expected call site not found: {old!r}")
            sys.exit(1)
        src = src.replace(old, new)

    with open(ADAPTER_H, "w") as f:
        f.write(src)
    print("[patch_littlefs_paths] normalized LittleFS adapter paths to leading '/'")


main()
