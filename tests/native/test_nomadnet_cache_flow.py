from pathlib import Path
from native_test import compile_and_run
ROOT=Path(__file__).resolve().parents[2];INC=ROOT/"lib/tdeck_ui/UI/LXMF"
def test_incremental_cache_owner_flow(tmp_path):
 compile_and_run(tmp_path,name="cache_flow",sources=[ROOT/"tests/native/test_nomadnet_cache_flow.cpp",INC/"NomadNetCache.cpp",INC/"NomadNetCacheFlow.cpp"],include_dirs=[INC],sanitize=True)
