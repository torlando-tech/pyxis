import json
import shutil
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
LXMF = ROOT / "lib" / "tdeck_ui" / "UI" / "LXMF"
WEBP = ROOT / "lib" / "webp"
FIXTURES = HERE / "fixtures" / "nomadnet_image"


def _cc():
    for name in ("gcc", "clang"):
        if shutil.which(name):
            return name
    pytest.skip("no C compiler found")


def _cxx():
    for name in ("g++", "clang++"):
        if shutil.which(name):
            return name
    pytest.skip("no C++ compiler found")


def test_vendor_manifest_is_complete():
    import json
    manifest = json.loads((WEBP / "VENDOR_MANIFEST.json").read_text())
    for rel in manifest["files"]:
        path = WEBP / rel
        assert path.is_file(), f"missing vendored file {rel}"
        import hashlib
        data = path.read_bytes()
        assert hashlib.sha256(data).hexdigest() == manifest["files"][rel]["sha256"], \
            f"vendored file {rel} hash mismatch (vendor drift)"
    assert manifest["version"] == "v1.3.2"


def test_nomadnet_image_decode_native(tmp_path):
    assert (FIXTURES / "demo.webp").is_file(), "authoritative fixture demo.webp missing"
    import hashlib
    demo = (FIXTURES / "demo.webp").read_bytes()
    assert hashlib.sha256(demo).hexdigest() == \
        "e7b8bd740d8fbf9d88480bc4e4fe42559373065ad6de2e8e114cce7c64740c06", \
        "demo.webp fixture hash drifted from the pinned reference bytes"
    # Compile the exact sources the firmware build compiles (library.json
    # srcFilter), so host evidence tracks the production set.
    library = json.loads((WEBP / "library.json").read_text())
    srcs = [s[2:-1] for s in library["build"]["srcFilter"]
            if s.startswith("+<") and s.endswith(">") and s[2:-1].endswith(".c")]
    webp_sources = [str(WEBP / "src" / s) for s in srcs]
    assert len(webp_sources) >= 25, "srcFilter unexpectedly shrank"
    binary = tmp_path / "test_nomadnet_image_decode"
    command = [
        _cxx(), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-DWEBP_DECODER_BUILD", "-DWEBP_BUILD", "-DWEBP_REDUCE_SIZE=1",
        "-DHAVE_CONFIG_H=1",
        f"-I{WEBP}", f"-I{LXMF}",
        str(HERE / "test_nomadnet_image_decode.cpp"),
        str(LXMF / "NomadNetImageDecoder.cpp"),
        *webp_sources,
        "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=180)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    result = subprocess.run([str(binary), str(FIXTURES)], capture_output=True,
                            text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "ALL IMAGE DECODER TESTS PASSED"
