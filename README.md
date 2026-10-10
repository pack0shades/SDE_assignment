# B+ Tree: memory-only vs disk-only vs memory + disk

One B+ tree implementation (C++17, no dependencies) running on three interchangeable
storage engines, benchmarked on 10,000,000 shuffled `key,payload` records.

**Results and analysis: [REPORT.md](REPORT.md).** Raw benchmark output: [results/](results/).

## Layout

| File | What it is |
|---|---|
| [src/common.h](src/common.h) | Record format, on-page node layout, capacities |
| [src/bptree.h](src/bptree.h) | The B+ tree: insert, point search, range scan, structural validation |
| [src/pager.h](src/pager.h) | The three storage engines: `MemPager`, `DiskPager`, `BufferPoolPager` |
| [src/bench.cpp](src/bench.cpp) | Benchmark driver |
| [src/test.cpp](src/test.cpp) | Correctness tests against `std::map` |
| [run_all.sh](run_all.sh) | Runs every benchmark configuration on the full dataset |

## Build and run

Needs Linux, `g++` (C++17) and `make`. Put `bplus_tree_data_10m.csv` in this directory
(it is not committed: 740 MB).

```sh
make            # builds build/bench, build/test, build/test_small
make test       # correctness tests, at 4096-byte and 256-byte pages

./build/bench --mode mem                      # memory-only
./build/bench --mode disk                     # disk-only, O_DIRECT, no cache at all
./build/bench --mode hybrid --pool-mb 128     # memory + disk, 128 MiB buffer pool

./run_all.sh    # everything, into results/ (about an hour: see the report)
```

Useful flags: `--n N` (use only the first N records), `--queries Q` (lookups to time,
default 1,000,000), `--no-direct` (let disk I/O go through the OS page cache),
`--csv FILE`, `--index FILE`, `--keep` (keep the index file), `--json FILE`.

The disk modes create a ~1 GiB index file in the current directory and delete it
afterwards.
