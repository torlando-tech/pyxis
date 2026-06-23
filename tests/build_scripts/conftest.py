import sys
from pathlib import Path

# The pre-build scripts (patch_*, sync_file_libdeps) import their sibling
# `_build_helpers` from the repo root. Put the repo root on sys.path so the tests
# can load those scripts regardless of how pytest was invoked -- CI runs bare
# `pytest ...`, which (unlike `python -m pytest`) does NOT add the cwd to sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
