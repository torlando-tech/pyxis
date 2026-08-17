import os
import re
import subprocess
import shutil
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
    scenario_records = re.findall(
        r"^SCENARIO ([a-z-]+): (PASS|FAIL) server=(-?\d+) client=(-?\d+)$",
        result.stdout, flags=re.MULTILINE)
    assert len(scenario_records) == 10
    assert len({record[0] for record in scenario_records}) == 10
    assert all(record[1:] == ("PASS", "0", "0") for record in scenario_records)
    result_records = re.findall(r"^RESULT ([^\n]+)$", result.stdout, flags=re.MULTILINE)
    assert len(result_records) == 10
    parsed_results = {}
    for record in result_records:
        fields = record.split()
        values = {}
        for field in fields:
            assert field.count("=") == 1, record
            key, value = field.split("=", 1)
            assert key not in values, record
            values[key] = value
        assert "scenario" in values and values["scenario"] not in parsed_results
        assert values.get("passed") == "1"
        parsed_results[values["scenario"]] = values
    assert "anonymous=True" in result.stdout
    assert "EVENT oversized transfer=" in result.stdout
    assert "SCENARIO reuse: PASS server=0 client=0" in result.stdout
    assert "reuse_requests=2" in result.stdout
    assert "link_callbacks=1" in result.stdout
    assert "SCENARIO form-anonymous: PASS server=0 client=0" in result.stdout
    assert "SCENARIO form-identified: PASS server=0 client=0" in result.stdout
    assert "SCENARIO owner-form-history: PASS server=0 client=0" in result.stdout
    owner = parsed_results["owner-form-history"]
    assert owner["owner_submit"] == "1"
    assert owner["history_bytes"] == "1"
    assert owner["retained_link"] == "1"
    assert owner["back_restored"] == "1"
    assert owner["reload_reused"] == "1"
    assert "SERVER PASS exact form request data anonymous=True" in result.stdout
    assert "SERVER PASS exact form request data anonymous=False" in result.stdout
    assert "REFERENCE NomadNet Git 89e3eea10c60d8fe597d36d2e091d5aab86bdfb8 hash-pinned" in result.stdout
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
    assert '"nomadnet/Node.py"' in runner
    assert 'git", "rev-parse", "HEAD"' in runner
    assert 'NOMADNET_COMMIT' in runner
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


def test_owner_real_peer_scenario_uses_production_owner_seams():
    runner = (ROOT / "tests/native/nomadnet_x86_flow/run_flow.py").read_text()
    client = (ROOT / "tests/native/nomadnet_x86_flow/client.cpp").read_text()
    owner = (ROOT / "lib/tdeck_ui/UI/LXMF/NomadNetOwner.h").read_text()
    cmake = (ROOT / "tests/native/nomadnet_x86_flow/CMakeLists.txt").read_text()
    assert '"owner-form-history"' in runner
    assert "NomadNetOwner.h" in client
    assert "OwnerController" in client
    assert "owner.service" in client
    assert "prepare_owner_form_request" not in client
    assert "owner_service_submit" not in client
    assert "NomadNetOwner.cpp" in cmake
    assert "OwnerCommand service(" in owner
    assert "static bool retain_active_link(" in owner


def test_x86_runner_rejects_unattested_external_client(tmp_path):
    fake = tmp_path / "fake-client"
    fake.write_text("#!/bin/sh\nexit 0\n")
    fake.chmod(0o755)
    result = subprocess.run(
        ["python3", str(RUNNER), str(fake), "/bin/false", str(tmp_path)],
        cwd=ROOT, capture_output=True, text=True,
    )
    assert result.returncode != 0
    assert "client manifest" in result.stdout + result.stderr


def _configured_reference():
    value = os.environ.get("PYXIS_NOMADNET_REFERENCE_SOURCE")
    if not value:
        import pytest
        pytest.skip("configured NomadNet reference is required for executable provenance matrix")
    return Path(value)


def test_nomadnet_package_tree_provenance_accepts_exact_hashes_without_git(tmp_path):
    source = _configured_reference()
    package = tmp_path / "package"
    shutil.copytree(source / "nomadnet", package / "nomadnet")
    result = subprocess.run(
        ["python3", str(RUNNER), "--verify-reference", str(package)],
        cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "REFERENCE NomadNet package 1.2.8 hash-pinned" in result.stdout
    assert "REFERENCE PROVENANCE: PASS" in result.stdout


def test_nomadnet_package_tree_provenance_rejects_wrong_hash(tmp_path):
    source = _configured_reference()
    package = tmp_path / "package"
    shutil.copytree(source / "nomadnet", package / "nomadnet")
    with (package / "nomadnet/Node.py").open("a") as stream:
        stream.write("\n# stale\n")
    result = subprocess.run(
        ["python3", str(RUNNER), "--verify-reference", str(package)],
        cwd=ROOT, capture_output=True, text=True)
    assert result.returncode != 0
    assert "wrong NomadNet reference file hash for nomadnet/Node.py" in result.stdout + result.stderr


def test_nomadnet_git_tree_provenance_rejects_wrong_commit(tmp_path):
    source = _configured_reference()
    checkout = tmp_path / "checkout"
    shutil.copytree(source / "nomadnet", checkout / "nomadnet")
    subprocess.run(["git", "init", "-q"], cwd=checkout, check=True)
    subprocess.run(["git", "add", "nomadnet"], cwd=checkout, check=True)
    subprocess.run(
        ["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
         "commit", "-qm", "wrong provenance"], cwd=checkout, check=True)
    result = subprocess.run(
        ["python3", str(RUNNER), "--verify-reference", str(checkout)],
        cwd=ROOT, capture_output=True, text=True)
    assert result.returncode != 0
    assert "wrong NomadNet reference commit" in result.stdout + result.stderr


def test_x86_runner_rejects_stale_attested_client_when_configured(tmp_path):
    client_text = os.environ.get("PYXIS_NOMADNET_X86_CLIENT")
    if not client_text:
        import pytest
        pytest.skip("configured x86 client is required for stale attestation test")
    stale_root = tmp_path / "root"
    shutil.copytree(ROOT, stale_root, ignore=shutil.ignore_patterns(".git", ".pio"))
    with (stale_root / "lib/tdeck_ui/UI/LXMF/NomadNetOwner.cpp").open("a") as stream:
        stream.write("\n// stale source\n")
    stale_runner = stale_root / "tests/native/nomadnet_x86_flow/run_flow.py"
    result = subprocess.run(
        ["python3", str(stale_runner), client_text, "/bin/false", str(tmp_path)],
        cwd=stale_root, capture_output=True, text=True)
    assert result.returncode != 0
    assert "client manifest mismatch" in result.stdout + result.stderr


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
