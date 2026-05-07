"""
PlatformIO pre-build script: TEMPORARY diagnostic patch for
microStore's FileStore::exists().

Adds a printf at the top of `bool exists(const uint8_t*, uint8_t)` so
we can see why the path-table store's exists returns false even when
the most recent put for the same key succeeded. Remove once the
investigation is done.
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

def patch(content):
    out = content
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
