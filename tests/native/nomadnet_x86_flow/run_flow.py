#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SERVER = Path(__file__).with_name("server.py")
CLIENT = Path(sys.argv[1])
PYTHON = Path(sys.argv[2])
SCENARIOS = ("immediate", "resource", "near-limit", "oversized", "timeout", "cancel")

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
    if scenario != "timeout":
        ok &= "anonymous=True" in server_text
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
    print(f"SCENARIO {scenario}: {'PASS' if ok else 'FAIL'} server={server_rc} client={client_rc}")
    failed |= not ok

sys.exit(1 if failed else 0)
