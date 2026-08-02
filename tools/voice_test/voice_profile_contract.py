"""Dependency-free production voice-profile checks for the physical harness."""

import re

ULBW_PROFILE_NAME = "ULBW"
ULBW_PROFILE_ID = 0x10
_ULBW_CONFIRMATION = re.compile(r"(?:^|\r?\n)T:OK profile=0x10(?:\r?\n|$)")


def validate_requested_profiles(value):
    profiles = [item.strip().upper() for item in value.split(",") if item.strip()]
    if profiles != [ULBW_PROFILE_NAME]:
        raise ValueError("Pyxis production voice testing supports only ULBW")
    return profiles


def require_ulbw_confirmation(response):
    if not _ULBW_CONFIRMATION.search(response):
        raise RuntimeError(
            "firmware did not confirm the required ULBW profile (T:OK profile=0x10)"
        )
    return ULBW_PROFILE_ID
