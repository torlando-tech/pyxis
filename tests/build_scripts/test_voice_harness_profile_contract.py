import sys
from pathlib import Path

import pytest

VOICE_TEST_DIR = Path(__file__).resolve().parents[2] / "tools/voice_test"
sys.path.insert(0, str(VOICE_TEST_DIR))

from voice_profile_contract import require_ulbw_confirmation, validate_requested_profiles


def test_harness_accepts_only_ulbw_request():
    assert validate_requested_profiles("ULBW") == ["ULBW"]


@pytest.mark.parametrize("requested", ["VLBW", "LBW", "ULBW,VLBW", ""])
def test_harness_rejects_non_ulbw_profile_sets(requested):
    with pytest.raises(ValueError):
        validate_requested_profiles(requested)


def test_harness_requires_exact_ulbw_firmware_confirmation():
    assert require_ulbw_confirmation("noise\nT:OK profile=0x10\n") == 0x10


@pytest.mark.parametrize(
    "response",
    ["T:ERR unknown profile", "T:OK profile=0x20", "", "T:OK profile=0x1"],
)
def test_harness_aborts_on_rejection_or_mismatched_confirmation(response):
    with pytest.raises(RuntimeError):
        require_ulbw_confirmation(response)
