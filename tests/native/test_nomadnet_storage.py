from pathlib import Path
from native_test import compile_and_run
ROOT=Path(__file__).resolve().parents[2]
def test_nomadnet_storage_contract(tmp_path):
    compile_and_run(tmp_path,name="nomadnet_storage",sources=[ROOT/"tests/native/test_nomadnet_storage.cpp"],include_dirs=[ROOT/"lib/tdeck_ui/UI/LXMF"])
