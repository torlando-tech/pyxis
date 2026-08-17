#!/usr/bin/env python3
import ast
import hashlib
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
SERVER = Path(__file__).with_name("server.py")
CLIENT = Path(sys.argv[1])
PYTHON = Path(sys.argv[2])
NOMADNET_SOURCE = Path(sys.argv[3])
NOMADNET_COMMIT = "89e3eea10c60d8fe597d36d2e091d5aab86bdfb8"
NOMADNET_VERSION = "1.2.8"
REFERENCE_FILES = {
    "nomadnet/_version.py":
        "09f5579b4c3094a3d6d2484e730856c3ad53a60b5988b8a309e3ec00838adda5",
    "nomadnet/ui/textui/MicronParser.py":
        "c4b40918fe813a7cfbb696f33df8a08451fd0156a6919a185b75225f52402ffb",
    "nomadnet/ui/textui/Browser.py":
        "b7bc37e0fd4e72261703a037ab1967ea4cc43b837dc1cd74f92a835bacab40a1",
}
SCENARIOS = ("immediate", "resource", "near-limit", "oversized", "timeout", "cancel", "reuse",
             "form-anonymous", "form-identified")

for relative, expected_hash in REFERENCE_FILES.items():
    path = NOMADNET_SOURCE / relative
    if not path.is_file():
        raise SystemExit(f"missing NomadNet reference file: {relative}")
    actual_hash = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual_hash != expected_hash:
        raise SystemExit(f"wrong NomadNet reference file hash for {relative}: {actual_hash}")
version_tree = ast.parse((NOMADNET_SOURCE / "nomadnet/_version.py").read_text())
version = next(
    ast.literal_eval(node.value)
    for node in version_tree.body
    if isinstance(node, ast.Assign)
    and any(isinstance(target, ast.Name) and target.id == "__version__" for target in node.targets)
)
if version != NOMADNET_VERSION:
    raise SystemExit(f"wrong NomadNet reference version: {version}")
print(f"REFERENCE NomadNet {NOMADNET_COMMIT}")


def run_browser_oracle(source: Path) -> None:
    tree = ast.parse(source.read_text(), filename=str(source))
    browser_class = next(
        node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == "Browser"
    )
    method = next(
        node for node in browser_class.body
        if isinstance(node, ast.FunctionDef) and node.name == "handle_link"
    )
    method.name = "canonical_handle_link"
    method.decorator_list = []
    method = ast.fix_missing_locations(method)

    class Edit:
        def __init__(self, name, value):
            self.field_name = name
            self.edit_text = value

    class RadioButton:
        def __init__(self, name, value, state):
            self.field_name = name
            self.field_value = value
            self.state = state

    class CheckBox(RadioButton):
        pass

    urwid = SimpleNamespace(
        Edit=Edit, RadioButton=RadioButton, CheckBox=CheckBox,
        Text=lambda value: value,
    )

    class Rns:
        LOG_DEBUG = 0

        @staticmethod
        def log(*_args):
            return None

    class BrowserSentinel:
        DISCONECTED = 0

    class Subject:
        status = 0

        def __init__(self):
            self.attr_maps = [
                Edit("name", "alice"),
                RadioButton("mode", "fast", True),
                CheckBox("tags", "", True),
                CheckBox("tags", "blue", True),
                CheckBox("ignored", "no", False),
            ]
            self.observed = None

        def retrieve_url(self, target, request_data):
            self.observed = (target, request_data)

    namespace = {
        "Browser": BrowserSentinel,
        "RNS": Rns,
        "urwid": urwid,
        "nomadnet": object(),
    }
    exec(compile(ast.Module(body=[method], type_ignores=[]), str(source), "exec"), namespace)
    subject = Subject()
    namespace["canonical_handle_link"](
        subject, "destination:/page/index.mu",
        ["x=first", "x=last", "name", "mode", "tags"],
    )
    expected = {
        "var_x": "last",
        "field_name": "alice",
        "field_mode": "fast",
        "field_tags": "blue",
    }
    if subject.observed != ("destination:/page/index.mu", expected):
        raise SystemExit(f"canonical Browser oracle mismatch: {subject.observed!r}")
    print("REFERENCE Browser.handle_link oracle: PASS")


run_browser_oracle(NOMADNET_SOURCE / "nomadnet/ui/textui/Browser.py")

failed = False
for scenario in SCENARIOS:
    run_dir = Path(tempfile.mkdtemp(prefix=f"pyxis-nomadnet-flow-{scenario}-"))
    server_log = run_dir / "server.log"
    client_log = run_dir / "client.log"
    with server_log.open("w") as server_output, client_log.open("w") as client_output:
        server = subprocess.Popen(
            [str(PYTHON), str(SERVER), scenario, "--timeout", "20"],
            cwd=ROOT, stdout=server_output, stderr=subprocess.STDOUT,
        )
        time.sleep(1.0)
        client = subprocess.Popen(
            [str(CLIENT), scenario], cwd=run_dir,
            stdout=client_output, stderr=subprocess.STDOUT,
        )
        try:
            client_rc = client.wait(timeout=22)
            server_rc = server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            client.kill()
            server.kill()
            client_rc = client.wait()
            server_rc = server.wait()

    server_text = server_log.read_text()
    client_text = client_log.read_text()
    print(f"=== {scenario.upper()} SERVER ===\n{server_text}", end="")
    print(f"=== {scenario.upper()} CLIENT ===\n{client_text}", end="")
    ok = server_rc == 0 and client_rc == 0
    if scenario not in ("timeout", "form-identified"):
        ok &= "anonymous=True" in server_text
    if scenario == "form-identified":
        ok &= "anonymous=False" in server_text
    if scenario in ("form-anonymous", "form-identified"):
        ok &= "SERVER PASS exact form request data" in server_text
    if scenario == "timeout":
        ok &= all(marker in client_text for marker in (
            "deadline=1", "receipt_failed=1", "pending=0", "link_closed=1",
        ))
        ok &= "cancel=0" in client_text
    if scenario == "cancel":
        ok &= all(marker in client_text for marker in (
            "resource_started=1", "resource_progress=1", "receipt_failed=1",
            "pending=0", "link_closed=1",
        ))
        ok &= "SERVER PASS cancellation observed" in server_text
    if scenario == "reuse":
        ok &= all(marker in client_text for marker in (
            "reuse_requests=2", "link_callbacks=1", "pending=0",
        ))
        ok &= server_text.count("SERVER request count=") == 2
        ok &= "SERVER PASS reused one Link for two anonymous requests" in server_text
    print(f"SCENARIO {scenario}: {'PASS' if ok else 'FAIL'} server={server_rc} client={client_rc}")
    failed |= not ok

sys.exit(1 if failed else 0)
