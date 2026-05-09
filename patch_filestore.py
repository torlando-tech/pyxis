"""
PlatformIO pre-build script: patches the libdeps copy of microStore
FileStore.h before each build.

Two purposes:

1. Adds diagnostic printfs to `exists()` and `put()` for tracking down
   the path-table-store bug where exists() returns false right after a
   successful put for the same key. Temporary; remove once that
   investigation is closed.

2. Silences the spammy "[ustore] get: key not found in index" print
   that fires on every path-table miss. RNS hits path-store lookups
   constantly on every incoming packet — during an active LXST call
   that print floods USB CDC at hundreds of lines/sec, saturating the
   serial buffer and starving T:CALL_QOS responses (#75). The print
   is unconditional in upstream microStore, so we patch it out here.
"""
Import("env")
import os

FILESTORE_H = os.path.join(
    env.get("PROJECT_DIR", "."),
    ".pio", "libdeps", "tdeck", "microStore", "include", "microStore", "FileStore.h",
)

OLD = """\tbool exists(const uint8_t* key, uint8_t key_len)
\t{
        if (!isValid()) return false;
\t\tif(key_len > USTORE_MAX_KEY_LEN) return false;
\t\tIndexValue* e = index_find(key, key_len);
\t\tif (!e) return false;
\t\tif (is_ttl_expired_(e->timestamp, e->ttl)) { index_remove(key, key_len); return false; }
\t\treturn true;
\t}"""

NEW = """\tbool exists(const uint8_t* key, uint8_t key_len)
\t{
\t\tif (!isValid()) { printf("[ustore] exists: !isValid len=%u idx_size=%zu store=%p\\n", (unsigned)key_len, _index.size(), (void*)this); return false; }
\t\tif(key_len > USTORE_MAX_KEY_LEN) { printf("[ustore] exists: key too long\\n"); return false; }
\t\tIndexValue* e = index_find(key, key_len);
\t\tif (!e) { printf("[ustore] exists: not_in_index len=%u key=%s idx_size=%zu store=%p\\n", (unsigned)key_len, bin_str(key, key_len), _index.size(), (void*)this); return false; }
\t\tif (is_ttl_expired_(e->timestamp, e->ttl)) { printf("[ustore] exists: ttl_expired ts=%u ttl=%u now=%u\\n", e->timestamp, e->ttl, microStore::time()); index_remove(key, key_len); return false; }
\t\tprintf("[ustore] exists: found key=%s store=%p\\n", bin_str(key, key_len), (void*)this);
\t\treturn true;
\t}"""

PUT_OLD = """\t\tindex_insert(key, key_len, current_segment, offset, ts, ttl);

\t\t// Enforce max_recs:"""

PUT_NEW = """\t\tindex_insert(key, key_len, current_segment, offset, ts, ttl);
\t\tprintf("[ustore] put: index_insert done key=%s idx_size=%zu store=%p\\n", bin_str(key, key_len), _index.size(), (void*)this);

\t\t// Enforce max_recs:"""

SPAMMY_OLD = '\t\t\tprintf("[ustore] get: key not found in index\\n");'
SPAMMY_NEW = '\t\t\t/* silenced — fires on every path-store miss, floods USB CDC */'

DIAG_ENABLED = os.environ.get("PYXIS_FILESTORE_DIAG", "0") == "1"

def patch(content):
    out = content
    # Silence patch always runs.
    if SPAMMY_OLD in out:
        out = out.replace(SPAMMY_OLD, SPAMMY_NEW)
        print("PATCH: FileStore.h: silenced 'key not found in index' spam")
    elif "silenced — fires on every path-store miss" in content:
        print("PATCH: FileStore.h: 'key not found' spam already silenced")
    # Diagnostic patches only when explicitly requested.
    if not DIAG_ENABLED:
        return out
    if OLD in out:
        out = out.replace(OLD, NEW)
        print("PATCH: FileStore.h: exists() diagnostics added")
    elif "store=%p" in content and "exists:" in content:
        print("PATCH: FileStore.h: exists() already patched")
    if PUT_OLD in out:
        out = out.replace(PUT_OLD, PUT_NEW)
        print("PATCH: FileStore.h: put-after-insert diagnostics added")
    elif "put: index_insert done" in content:
        print("PATCH: FileStore.h: put-after-insert already patched")
    return out

if os.path.exists(FILESTORE_H):
    with open(FILESTORE_H) as f:
        content = f.read()
    new = patch(content)
    if new != content:
        with open(FILESTORE_H, "w") as f:
            f.write(new)
else:
    print("PATCH: FileStore.h not found, skipping")
