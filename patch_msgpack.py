"""
PlatformIO pre-build script: Expose hideakitai/MsgPack internals used by microLXMF.

LXMessage's wire format includes a `dict[int, Any]` fields map. To splice
arbitrary msgpack values into the stream without LXMessage knowing each
value's type, the encoder uses Packer::packRawBytes (private upstream)
to write pre-encoded bytes; the decoder uses Unpacker::indices /
raw_data (also private upstream) to capture each key+value byte span as
an opaque slice.

This patch promotes the relevant access modifiers from private to
public. Idempotent: only rewrites on first apply, no-op afterward.

Mirrors the patch the microLXMF conformance-bridge applies via its
CMakeLists (microLXMF/conformance-bridge/CMakeLists.txt:106-125).
"""
Import("env")
import os, sys
sys.path.insert(0, env.get("PROJECT_DIR", "."))
from _build_helpers import env_libdeps_dir  # per-env libdeps path; never hardcode the env

MSGPACK_BASE = env_libdeps_dir(env, "MsgPack", "MsgPack")

def apply_patch(filepath, old, new, label):
    if not os.path.exists(filepath):
        print(f"PATCH: {os.path.basename(filepath)} not found, skipping {label}")
        return
    with open(filepath, "r") as f:
        content = f.read()
    if old in content:
        with open(filepath, "w") as f:
            f.write(content.replace(old, new))
        print(f"PATCH: {label}")
    elif new in content or "patched by pyxis" in content:
        print(f"PATCH: {label} (already applied)")
    else:
        print(f"PATCH: WARNING -- {label}: expected pattern not found")

apply_patch(
    os.path.join(MSGPACK_BASE, "Packer.h"),
    "    private:\n        void packRawByte",
    "    public:  // patched by pyxis (microLXMF needs packRawBytes)\n        void packRawByte",
    "Packer.h: expose packRawBytes/packRawByte as public",
)

apply_patch(
    os.path.join(MSGPACK_BASE, "Unpacker.h"),
    "    class Unpacker {\n        uint8_t* raw_data",
    "    class Unpacker {\n    public:  // patched by pyxis (microLXMF needs indices/raw_data)\n        uint8_t* raw_data",
    "Unpacker.h: expose indices/raw_data as public",
)
