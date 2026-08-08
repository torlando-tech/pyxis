# Web map-installer fixtures

`downloader-rootless-stored-descriptor.zip` is a sanitized, minimal archive shaped like output from the downloader recommended by the web installer. Its ZIP envelope was derived from an authorized sample, but none of that sample's tile payloads or identifying coverage are included.

The fixture intentionally preserves the compatibility-relevant traits observed in that output:

- rootless canonical XYZ file names with no directory entries;
- stored (uncompressed) PNG entries;
- general-purpose flag bit 3 with zero CRC and sizes in each local header;
- signed trailing data descriptors; and
- DOS-created central-directory records with no extra fields or external attributes.

Its three generic 256×256 PNGs are generated test images at canonical zoom 0–2 coordinates. The small coverage is sufficient to exercise the downloader's archive format without retaining geographic content.
