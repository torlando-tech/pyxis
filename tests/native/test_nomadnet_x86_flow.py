import os
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNNER = HERE / "nomadnet_x86_flow" / "run_flow.py"
ROOT = HERE.parent.parent


def test_nomadnet_x86_real_peer_flow():
    client_text = os.environ.get("PYXIS_NOMADNET_X86_CLIENT")
    python_text = os.environ.get("PYXIS_NOMADNET_RNS_PYTHON")
    nomadnet_text = os.environ.get("PYXIS_NOMADNET_REFERENCE_SOURCE")
    configured = (client_text, python_text, nomadnet_text)
    assert all(configured), (
        "the mandatory real-peer gate requires PYXIS_NOMADNET_X86_CLIENT, "
        "PYXIS_NOMADNET_RNS_PYTHON, and PYXIS_NOMADNET_REFERENCE_SOURCE"
    )
    assert client_text is not None
    assert python_text is not None
    assert nomadnet_text is not None
    client = Path(client_text)
    python = Path(python_text)
    nomadnet = Path(nomadnet_text)
    assert client.is_file(), client
    assert python.is_file(), python
    assert nomadnet.is_dir(), nomadnet
    result = subprocess.run(
        ["python3", str(RUNNER), str(client), str(python), str(nomadnet)],
        cwd=HERE.parent.parent,
        capture_output=True,
        text=True,
        timeout=180,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.count("SCENARIO ") == 9
    assert result.stdout.count(": PASS server=0 client=0") == 9
    assert "anonymous=True" in result.stdout
    assert "EVENT oversized transfer=" in result.stdout
    assert "SCENARIO reuse: PASS server=0 client=0" in result.stdout
    assert "reuse_requests=2" in result.stdout
    assert "link_callbacks=1" in result.stdout
    assert "SCENARIO form-anonymous: PASS server=0 client=0" in result.stdout
    assert "SCENARIO form-identified: PASS server=0 client=0" in result.stdout
    assert "SERVER PASS exact form request data anonymous=True" in result.stdout
    assert "SERVER PASS exact form request data anonymous=False" in result.stdout
    assert "REFERENCE NomadNet 89e3eea10c60d8fe597d36d2e091d5aab86bdfb8" in result.stdout
    assert "REFERENCE RNS 1.4.2" in result.stdout
    assert "REFERENCE RNS tree b5398e7bae0cdd47212e0c6bff3f3a51b21012db0c23cb20d43b5103612f6c5e" in result.stdout
    assert "REFERENCE Browser.handle_link oracle: PASS" in result.stdout


def test_x86_flow_has_real_lan_tcp_nomadnet_mode():
    cmake = (ROOT / "tests/native/nomadnet_x86_flow/CMakeLists.txt").read_text()
    client = (ROOT / "tests/native/nomadnet_x86_flow/client.cpp").read_text()

    assert '"${PYXIS_ROOT}/src/TCPClientInterface.cpp"' in cmake
    assert '"${PYXIS_ROOT}/src"' in cmake
    assert 'scenario == "lan"' in client
    assert 'path = "/page/index.mu"' in client
    assert "set_target_host" in client
    assert "set_target_port" in client


def test_x86_flow_proves_two_pages_reuse_one_encrypted_link():
    runner = (ROOT / "tests/native/nomadnet_x86_flow/run_flow.py").read_text()
    server = (ROOT / "tests/native/nomadnet_x86_flow/server.py").read_text()
    client = (ROOT / "tests/native/nomadnet_x86_flow/client.cpp").read_text()

    assert '"reuse"' in runner
    assert '"/page/reuse-first.mu"' in server
    assert '"/page/reuse-second.mu"' in server
    assert 'scenario == "reuse"' in client
    assert 'link_callbacks != 1' in client
    assert 'reuse_requests == 2' in client


def test_x86_flow_proves_exact_form_maps_for_anonymous_and_identified_links():
    cmake = (ROOT / "tests/native/nomadnet_x86_flow/CMakeLists.txt").read_text()
    runner = (ROOT / "tests/native/nomadnet_x86_flow/run_flow.py").read_text()
    server = (ROOT / "tests/native/nomadnet_x86_flow/server.py").read_text()
    client = (ROOT / "tests/native/nomadnet_x86_flow/client.cpp").read_text()

    assert '"form-anonymous"' in runner and '"form-identified"' in runner
    assert '89e3eea10c60d8fe597d36d2e091d5aab86bdfb8' in runner
    assert 'nomadnet/ui/textui/Browser.py' in runner
    assert 'RNS_VERSION = "1.4.2"' in server
    assert '"/page/form.mu"' in server
    assert '"var_fixed": "yes"' in server
    assert '"field_name": "Example User"' in server
    assert '"field_password": "example-pass"' in server
    assert '"field_color": "red,blue"' in server
    assert 'scenario == "form-identified"' in client
    assert "established_link.identify(local_identity)" in client
    assert "FormState" in client and "encode(" in client
    assert '"${PYXIS_ROOT}/lib/tdeck_ui/UI/LXMF/NomadNetForm.cpp"' in cmake


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
