# TMV development tools

Two standalone utilities from the TMV work on this fork. **Neither is library code**, neither is
built by the project's CMake, and neither is part of any upstream proposal — this branch exists
only so the tools can be handed around without polluting a review branch.

Both are single-file C, link nothing (not even `liboapv`), and build with one command.

---

## `apvreprofile` — promote published assets to the UNCONST profiles

### Why it exists

12K/16K TMV assets published before the UNCONST profile extensions carry a **constrained**
profile (`422-10` = 33, and so on) together with a tile grid beyond the RFC 9924 20x20 limit. Any
decoder that honours the limit — upstream OpenAPV and anything built from it — rejects those
frames outright, so the coarse mip levels decode and the fine ones come back **black**.

Re-encoding is the clean answer, but a published 16K sequence takes a very long time to
regenerate. This patches the profile instead.

### What it does

`profile_idc` is the first byte of `frame_info()`, byte-aligned at `pbu_start + 8` (the 4-byte
`pbu_size`, then the 4-byte PBU header). Promoting it is therefore a **one-byte patch per frame
header**. Nothing else in the bitstream depends on the value, and the assets already satisfy the
UNCONST profiles' chroma and bit-depth constraints — the extension only removes the tile-grid
limit.

Mapping applied: 33->43, 44->54, 55->65, 66->76, 77->87, 88->98, 99->109.

### Safety

It edits media in place, so:

- **dry run unless `--apply`** is given;
- each file is fully parsed and validated *before* any byte of it is written;
- only files with at least one oversized level are touched; already-UNCONST files are skipped, so
  it is **idempotent**;
- all frame PBUs in a file are promoted together, so an access unit never ends up with mixed
  profiles;
- `--apply --revert` applies the inverse mapping and restores the original bytes exactly
  (verified by md5).

### Usage

```
apvreprofile <file-or-directory> [--apply] [--revert] [--ext .apv1] [--verbose] [--no-verify]

apvreprofile /media/seq                  # dry run: report what would change
apvreprofile /media/seq --apply          # write, then re-scan to verify
apvreprofile /media/seq --apply --revert # undo
```

---

## `auprobe` — access unit structure dump

Walks an access unit and reports, per PBU: type, and for frame PBUs the profile, geometry, tile
grid, whether `tile_size_present_in_fh_flag` is set, and the tile size table. It parses the bit
layout directly rather than calling the library, so it can inspect assets the library **rejects**
— which is exactly what you need when diagnosing the problem above.

It also cross-checks the frame header's tile size table against the real tile chain by walking
the 4-byte size prefixes, and flags grids that exceed the RFC 9924 limit.

```
auprobe <file.apv> [--dump-tile-sizes N] [--dump-chain]
```

`--dump-chain` emits machine-readable `TILE <mip> <idx> <abs_offset> <size>` lines derived from
the chain itself, so they can be diffed against whatever a decoder reports.

---

## Build

```
gcc -O2 -o apvreprofile apvreprofile.c
gcc -O2 -o auprobe      auprobe.c
```

Tested with gcc on Windows (mingw) and portable to POSIX: the only platform-specific calls are a
case-insensitive compare and 64-bit file seeks, behind a shim at the top of `apvreprofile.c`.

## Licensing

These are internal development utilities, shared here for review convenience. They carry no
license header deliberately — confirm licensing before reusing them anywhere, and certainly
before proposing them upstream.
