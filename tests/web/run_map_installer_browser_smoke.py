"""B8: real Chromium OPFS smoke for the browser map installer.

Runs the public ``installMuiZip`` path against a synthetic one-tile ZIP on
a localhost secure context, reads the manifest/style/slot back through real
File System Access handles, reloads the page to verify OPFS persistence,
and guards against reintroducing the false ``exclusive``-create assumption.

This is a required pre-publication command, not a CI job: the repository's
CI does not provision a Chromium browser. Locates a Playwright-managed
Chromium build and drives it over the DevTools protocol.

Usage:
    python3 tests/web/run_map_installer_browser_smoke.py

Exit code 0 only when exactly one valid result record is emitted.
"""

import http.server
import json
import os
import shutil
import socket
import socketserver
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
HARNESS = "/tests/web/map_installer_browser_harness.html"
METADATA = {
    "packId": "smoke-pack",
    "mapSetId": "osm-bright",
    "name": "Smoke Pack",
    "attribution": "(c) OpenMapTiles (c) OpenStreetMap contributors",
    "source": "Oxed's Map Tile Downloader (OSM Bright)",
    "license": "OSM ODbL; style CC-BY-4.0/BSD-3-Clause",
}
PORT = None


def find_chromium() -> str:
    for env_name in ("PYXIS_SMOKE_CHROMIUM", "CHROME_PATH"):
        candidate = os.environ.get(env_name)
        if candidate and Path(candidate).is_file():
            return candidate
    cache = Path.home() / ".cache" / "ms-playwright"
    if cache.is_dir():
        for build in sorted(cache.glob("chromium-*"), reverse=True):
            for pattern in ("chrome-linux64/chrome", "chrome-linux/chrome"):
                candidate = build / pattern
                if candidate.is_file():
                    return str(candidate)
    for name in ("chromium", "chromium-browser", "google-chrome",
                 "google-chrome-stable"):
        candidate = shutil.which(name)
        if candidate:
            return candidate
    raise SystemExit(
        "No Chromium found. Install one (e.g. `python3 -m playwright install "
        "chromium`) or set PYXIS_SMOKE_CHROMIUM to a chrome binary."
    )


class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def make_handler(metadata_bytes):
    """Serve the repo tree, but answer the harness's metadata fetch from an
    in-memory payload so the smoke never writes a scratch file into the
    repository working tree."""

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(REPO_ROOT), **kwargs)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path.endswith("/smoke-metadata.json"):
                body = metadata_bytes
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            super().do_GET()

        def log_message(self, format, *args):
            # Keep the smoke output quiet; only the final JSON is emitted.
            pass

    return Handler


def run_phase(page, run_id, phase, timeout_s=90):
    """Run one harness phase and return window.__smokeResult.

    The phase and run id travel in the query string (with a nonce defeating
    the bfcache) so each phase is a real, independent load; the driver's
    in-memory metadata endpoint feeds the harness.
    """
    page.goto(
        f"http://127.0.0.1:{PORT}{HARNESS}"
        f"?phase={phase}&run={run_id}&nonce={phase}"
    )
    deadline = time.time() + timeout_s
    status = ""
    while time.time() < deadline:
        result = page.evaluate("window.__smokeResult")
        if result is not None:
            return result
        status = page.evaluate(
            "document.getElementById('status') ? "
            "document.getElementById('status').textContent : ''"
        )
        page.wait_for_timeout(100)
    raise SystemExit(
        f"phase {phase}: harness did not report in {timeout_s}s; status={status!r}"
    )


def main() -> None:
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        raise SystemExit(
            "The playwright Python package is required for this smoke. "
            "Install it in a scratch venv (browser binaries are picked up "
            "from ~/.cache/ms-playwright or PYXIS_SMOKE_CHROMIUM)."
        )

    global PORT
    PORT = free_port()
    metadata_bytes = json.dumps(METADATA).encode("utf-8")
    server = ThreadingHTTPServer(("127.0.0.1", PORT), make_handler(metadata_bytes))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        chromium = find_chromium()
        run_id = str(int(time.time() * 1000))
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(
                executable_path=chromium,
                args=[
                    "--no-sandbox",
                    "--disable-dev-shm-usage",
                ],
            )
            try:
                context = browser.new_context()
                page = context.new_page()
                result_install = run_phase(page, run_id, "install")
                if "error" in result_install:
                    raise SystemExit(
                        "install phase failed: "
                        f"{result_install['error']}\n"
                        f"{result_install.get('stack', '')}\n"
                        f"debug={result_install.get('debug', '')}"
                    )
                if not result_install.get("install") or \
                        not result_install.get("readback"):
                    raise SystemExit(f"install phase incomplete: {result_install}")
                if not result_install.get("exclusive_option_ignored"):
                    raise SystemExit(f"exclusive guard failed: {result_install}")
                result_reload = run_phase(page, run_id, "reload")
                if "error" in result_reload:
                    raise SystemExit(f"reload phase failed: {result_reload}")
            finally:
                browser.close()
    finally:
        server.shutdown()
        server.server_close()

    expected = {
        "install": True,
        "readback": True,
        "reload": True,
        "exclusive_option_ignored": True,
    }
    record = {
        # install/readback/exclusive come from the install phase (observed
        # there); only reload/tokenOk/composition come from the reload phase.
        "install": bool(result_install.get("install")),
        "readback": bool(result_install.get("readback")),
        "reload": bool(result_reload.get("reload")),
        "exclusive_option_ignored": bool(result_install.get("exclusive_option_ignored")),
    }
    if record != expected or result_reload.get("tokenOk") is not True \
            or not result_reload.get("composition", {}).get("ok"):
        json.dump({"ok": False, "result": result_reload}, sys.stdout)
        sys.stdout.write("\n")
        raise SystemExit(1)
    json.dump({"ok": True, "result": record}, sys.stdout)
    sys.stdout.write("\n")
    sys.exit(0)


if __name__ == "__main__":
    main()
