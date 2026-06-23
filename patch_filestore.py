"""
PlatformIO pre-build script: patches the libdeps copy of microStore
FileStore.h before each build.

Three purposes:

1. Adds diagnostic printfs to `exists()` and `put()` for tracking down
   the path-table-store bug where exists() returns false right after a
   successful put for the same key. Temporary; remove once that
   investigation is closed. Gated behind PYXIS_FILESTORE_DIAG=1.

2. Silences the spammy "[ustore] get: key not found in index" print
   that fires on every path-table miss. RNS hits path-store lookups
   constantly on every incoming packet — during an active LXST call
   that print floods USB CDC at hundreds of lines/sec, saturating the
   serial buffer and starving T:CALL_QOS responses (#75). The print
   is unconditional in upstream microStore, so we patch it out here.

3. Closes `active_file` before unlinking segment files in
   `finalize_compaction()` and `clear()`. Without this, on LittleFS
   / FAT (any FS that doesn't auto-close on unlink) the FD leaks —
   over enough compactions the device can't open new files. Local
   patch pending the upstream fix landing.
"""
Import("env")
import os, sys
sys.path.insert(0, env.get("PROJECT_DIR", "."))
from _build_helpers import env_libdeps_dir  # per-env libdeps path; never hardcode the env

FILESTORE_H = env_libdeps_dir(env, "microStore", "include", "microStore", "FileStore.h")

OLD = """\tbool exists(const uint8_t* key, uint8_t key_len)
\t{
        if (!isValid()) return false;
\t\tif(key_len > USTORE_MAX_KEY_LEN) return false;
\t\tIndexValue* e = index_find(key, key_len);
\t\tif (!e) return false;
\t\tif (is_ttl_expired_(e->timestamp, e->ttl)) { index_remove(key, key_len); return false; }
\t\treturn true;
\t}"""

NEW = """\t// pyxis-local diagnostic helper, only compiled under PYXIS_FILESTORE_DIAG
\t// and only used by the printfs below. Hex-encodes a binary key using just
\t// standard C so the diagnostic build is self-contained (no external bin_str).
\t// Static buffer is fine here: each printf evaluates it exactly once.
\tstatic const char* bin_str(const uint8_t* d, uint8_t n) {
\t\tstatic char b[2 * USTORE_MAX_KEY_LEN + 1];
\t\tstatic const char hex[] = "0123456789abcdef";
\t\tif (n > USTORE_MAX_KEY_LEN) n = USTORE_MAX_KEY_LEN;
\t\tfor (uint8_t i = 0; i < n; i++) { b[2 * i] = hex[(d[i] >> 4) & 0xF]; b[2 * i + 1] = hex[d[i] & 0xF]; }
\t\tb[2 * n] = '\\0';
\t\treturn b;
\t}
\tbool exists(const uint8_t* key, uint8_t key_len)
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

# FD-leak fix: close active_file before unlinking segments. Two sites.
FDLEAK_FINALIZE_OLD = """\tvoid finalize_compaction()
\t{
\t\tchar tmp_name[USTORE_MAX_FILENAME_LEN]; snprintf(tmp_name, sizeof(tmp_name), \"%s_compact.tmp\", base_prefix);
\t\tchar seg0[USTORE_MAX_FILENAME_LEN];     segment_name(0, seg0);

\t\t// Remove all existing segments, then rename tmp → seg0.
\t\tfor (uint32_t i = 0; i < _segment_count; i++) {
\t\t\tchar sname[USTORE_MAX_FILENAME_LEN]; segment_name(i, sname);
\t\t\t_filesystem.remove(sname);
\t\t}"""
FDLEAK_FINALIZE_NEW = """\tvoid finalize_compaction()
\t{
\t\tchar tmp_name[USTORE_MAX_FILENAME_LEN]; snprintf(tmp_name, sizeof(tmp_name), \"%s_compact.tmp\", base_prefix);
\t\tchar seg0[USTORE_MAX_FILENAME_LEN];     segment_name(0, seg0);

\t\t// pyxis-local: close active_file before unlinking, otherwise the FD
\t\t// leaks on LittleFS / FAT.
\t\tif (active_file) active_file.close();

\t\t// Remove all existing segments, then rename tmp → seg0.
\t\tfor (uint32_t i = 0; i < _segment_count; i++) {
\t\t\tchar sname[USTORE_MAX_FILENAME_LEN]; segment_name(i, sname);
\t\t\t_filesystem.remove(sname);
\t\t}"""

FDLEAK_CLEAR_OLD = """\t\tif (index_file) index_file.close();

\t\tfor(uint32_t i = 0; i < _segment_count; i++)
\t\t{
\t\t\tsegment_name(i,name);
\t\t\t_filesystem.remove(name);
\t\t}"""
FDLEAK_CLEAR_NEW = """\t\tif (index_file) index_file.close();
\t\t// pyxis-local: same active_file FD-leak fix as finalize_compaction().
\t\tif (active_file) active_file.close();

\t\tfor(uint32_t i = 0; i < _segment_count; i++)
\t\t{
\t\t\tsegment_name(i,name);
\t\t\t_filesystem.remove(name);
\t\t}"""

DIAG_ENABLED = os.environ.get("PYXIS_FILESTORE_DIAG", "0") == "1"

def patch(content):
    out = content
    # Silence patch always runs.
    if SPAMMY_OLD in out:
        out = out.replace(SPAMMY_OLD, SPAMMY_NEW)
        print("PATCH: FileStore.h: silenced 'key not found in index' spam")
    elif "silenced — fires on every path-store miss" in content:
        print("PATCH: FileStore.h: 'key not found' spam already silenced")
    # FD-leak fix always runs.
    if FDLEAK_FINALIZE_OLD in out:
        out = out.replace(FDLEAK_FINALIZE_OLD, FDLEAK_FINALIZE_NEW)
        print("PATCH: FileStore.h: closed active_file in finalize_compaction()")
    elif "pyxis-local: close active_file before unlinking" in content:
        print("PATCH: FileStore.h: finalize_compaction() FD-leak fix already applied")
    if FDLEAK_CLEAR_OLD in out:
        out = out.replace(FDLEAK_CLEAR_OLD, FDLEAK_CLEAR_NEW)
        print("PATCH: FileStore.h: closed active_file in clear()")
    elif "pyxis-local: same active_file FD-leak fix" in content:
        print("PATCH: FileStore.h: clear() FD-leak fix already applied")
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
