"""Shared helpers for the PlatformIO pre-build scripts (patch_*, sync_file_libdeps).

Centralizes the per-environment libdeps path so it is computed exactly ONE way.
Each PlatformIO environment gets its OWN .pio/libdeps/<PIOENV> tree, so this path
MUST be resolved per-environment. Hardcoding the env component (e.g. "tdeck")
silently misdirects a patch when building any OTHER env -- that is exactly how the
`nimble_host_reset_reason` undefined-reference link error appeared in tdeck-ota.
Always go through env_libdeps_dir(); never re-hand-roll the path with a literal env.
"""
import os


def env_libdeps_dir(env, *parts):
    """Path inside THIS environment's libdeps tree: .pio/libdeps/<PIOENV>/<parts...>."""
    return os.path.join(
        env.get("PROJECT_DIR", "."),
        ".pio", "libdeps", env.get("PIOENV", "tdeck"),
        *parts,
    )
