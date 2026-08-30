"""Contract for the safe CLI PMPK/PMAS v3 installation documentation.

`docs/offline-map-packs.md` is the user-facing operational contract for the
Linux CLI v3 writer. These tests pin the required wording so the concurrency
limitation and recovery steps cannot silently disappear.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
DOC = ROOT / "docs" / "offline-map-packs.md"


def _flat() -> str:
    """Collapse whitespace so prose wrapping across lines still matches."""
    return re.sub(r"\s+", " ", DOC.read_text(encoding="utf-8"))


def test_doc_documentation_is_present() -> None:
    assert "## Safe CLI installation with style and activation (PMPK/PMAS v3)" in _flat()


def test_doc_shows_exact_v3_cli_command() -> None:
    text = DOC.read_text(encoding="utf-8")
    assert "python3 tools/maps/build_map_pack.py" in text
    assert "--style osm-bright" in text
    assert "--activate" in text


def test_doc_explains_pmpk_and_pmas_v3_semantics() -> None:
    flat = _flat()
    assert "PMPK v3" in flat
    assert "PMAS v3" in flat
    assert "indexless" in flat
    # The complete-record property: the set record names every pack.
    assert "full ordered list of pack IDs" in flat
    assert "eight" in flat


def test_doc_states_safe_eject_requirement() -> None:
    flat = _flat()
    assert "Safe eject" in flat
    assert "safely unmount/eject" in flat


def test_doc_limits_cli_lock_to_cli_processes_and_warns_against_browser_concurrency() -> None:
    flat = _flat()
    assert "CLI processes only" in flat
    assert "Do not run the CLI and the browser installer against the same mounted card at the same time" in flat


def test_doc_documents_published_but_not_activated_recovery() -> None:
    flat = _flat()
    assert "published-but-not-activated" in flat
    assert "rerun the exact same command" in flat


def test_doc_does_not_instruct_deletion_of_marker_or_lease_files() -> None:
    flat = _flat()
    lower = flat.lower()
    # The v3 writer has no marker/lease/heartbeat files, and the doc must say so.
    assert "no marker, lease, or heartbeat files" in lower
    # No TTL-based marker protocol may be described anywhere in the doc.
    assert "INSTALL_MARKER_TTL" not in flat
    assert "stale-reclaim" not in lower
    assert "TTL" not in flat
    # The legacy section may mention the legacy `active-pack` marker, but the doc
    # must not tell users to delete any *.pyxis-installing* marker.
    assert ".pyxis-installing" not in flat
