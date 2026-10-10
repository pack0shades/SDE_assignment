# B+ Tree on 10M records: memory-only vs disk-only vs memory + disk

## 1. What was built

A single B+ tree implementation ([src/bptree.h](src/bptree.h)) that runs unchanged on three
storage engines ([src/pager.h](src/pager.h)). Because the tree code, node layout and order
are identical in all three, every difference measured below comes from the storage layer.

| Variant | Engine | Where nodes live |
|---|---|---|
| Memory-only | `MemPager` | Heap. Nothing is written to disk, nothing survives the process. |
| Disk-only | `DiskPager` | A file. **No cache at all**: every node visit is a `pread`, every node change a `pwrite`. |
| Memory + disk | `BufferPoolPager` | A file, behind a fixed-size LRU buffer pool. Dirty pages are written back on eviction and on flush. |

Operations: insert (overwrites on duplicate key), point search, ordered range scan over the
linked leaves. Deletion was not required and is not implemented.

## 2. Assumptions

**Data.** Every row is `key,payload` with an 8-character key and a 64-character payload
(checked for all 10M rows; the loader rejects anything else). Keys are unique. Records are
inserted **one at a time in file order** (shuffled); there is no sorting or bulk loading,
since that would sidestep the insertion cost being measured.

**Page / node size: 4096 bytes.** One node = one page = one filesystem block = one unit
of disk I/O.

**Node layout and order** (derived from the page size, not chosen separately):

| Node | Layout | Capacity |
|---|---|---|
| Leaf | 8 B header, 56 × 8 B keys, 56 × 64 B payloads | **56 records** |
| Internal | 8 B header, 340 × 8 B keys, 341 × 4 B child page ids | **order 341** (340 keys, 341 children) |

- Keys are stored as a 64-bit integer holding the 8 characters big-endian, so integer
  comparison equals string comparison.
- Payloads are stored **inside the leaves** (clustered index), so a lookup needs no extra
  hop to fetch the record.
- Children are 32-bit page ids in every variant (in memory too), which is what gives
  internal nodes a fan-out of 341.
- A full node splits in half; the middle key is copied up (leaf) or moved up (internal).
  Leaves are chained left to right.

**Storage structure (disk variants).** One file of 4 KiB pages. Page 0 is a metadata page
(root, height, counts); page ids are file offsets / 4096. Files are opened with `O_DIRECT`,
so the OS page cache is bypassed and every counted read/write is a real device I/O. There
is no write-ahead log: durability is a flush + `fsync` at the end (included in insert
time), so a crash mid-build loses the index. Single-threaded.

**Environment.** Intel i7-13700HX, 16 GB RAM, Micron 3400 NVMe SSD, ext4, Linux 6.18,
g++ 16 `-O2`. The machine was in normal desktop use (and swapping) during the runs, so
treat timings as ±10%.

**Measurement.** Insert time excludes CSV parsing. Search time is 1,000,000 lookups of
randomly sampled existing keys, each verified against the CSV. Memory is the exact size of
the engine's buffers (cross-checked against process RSS growth). Disk I/O is counted in
4 KiB pages inside the engine.

RESULTS_PLACEHOLDER
