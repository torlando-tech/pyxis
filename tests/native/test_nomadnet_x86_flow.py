import os
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
RUNNER = HERE / "nomadnet_x86_flow" / "run_flow.py"
ROOT = HERE.parent.parent


def test_nomadnet_x86_real_peer_flow():
    client_text = os.environ.get("PYXIS_NOMADNET_X86_CLIENT")
    python_text = os.environ.get("PYXIS_NOMADNET_RNS_PYTHON")
    if not client_text or not python_text:
        pytest.skip(
            "set PYXIS_NOMADNET_X86_CLIENT and PYXIS_NOMADNET_RNS_PYTHON "
            "to run the real two-process Reticulum flow"
        )
    assert client_text is not None
    assert python_text is not None
    client = Path(client_text)
    python = Path(python_text)
    assert client.is_file(), client
    assert python.is_file(), python
    result = subprocess.run(
        ["python3", str(RUNNER), str(client), str(python)],
        cwd=HERE.parent.parent,
        capture_output=True,
        text=True,
        timeout=180,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.count("SCENARIO ") == 6
    assert result.stdout.count(": PASS server=0 client=0") == 6
    assert "anonymous=True" in result.stdout
    assert "EVENT oversized transfer=" in result.stdout


def test_x86_flow_has_real_lan_tcp_nomadnet_mode():
    cmake = (ROOT / "tests/native/nomadnet_x86_flow/CMakeLists.txt").read_text()
    client = (ROOT / "tests/native/nomadnet_x86_flow/client.cpp").read_text()

    assert '"${PYXIS_ROOT}/src/TCPClientInterface.cpp"' in cmake
    assert '"${PYXIS_ROOT}/src"' in cmake
    assert 'scenario == "lan"' in client
    assert 'path = "/page/index.mu"' in client
    assert "set_target_host" in client
    assert "set_target_port" in client


def test_physical_lan_flow_captures_both_link_wire_boundaries_and_crypto_vector():
    server = (ROOT / "tests/native/nomadnet_x86_flow/lan_server.py").read_text()
    harness = (ROOT / "tests/hardware/nomadnet_tdeck_harness.py").read_text()

    assert 'LAN WIRE RX kind={kind}' in server
    assert 'LAN WIRE TX kind={kind}' in server
    assert "RNS.Transport.inbound = staticmethod(captured_inbound)" in server
    assert "RNS.Transport.transmit = staticmethod(captured_transmit)" in server
    assert 'capture.command(\n            "T:CRYPTO_VECTOR"' in harness
    assert 'choices=("NORMAL", "SUSPEND")' in harness
    assert 'capture.command("T:NOMAD_MODE " + args.mode)' in harness
