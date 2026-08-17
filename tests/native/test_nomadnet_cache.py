from pathlib import Path
from native_test import compile_and_run
ROOT=Path(__file__).resolve().parents[2]; INC=ROOT/"lib/tdeck_ui/UI/LXMF"
def test_nomadnet_cache_fault_matrix(tmp_path):
 compile_and_run(tmp_path,name="nomadnet_cache",sources=[ROOT/"tests/native/test_nomadnet_cache.cpp",INC/"NomadNetCache.cpp"],include_dirs=[INC],sanitize=True)
