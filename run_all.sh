#!/bin/sh
# Runs every benchmark configuration on the full dataset, one after another, writing
# results/<name>.txt (human readable) and results/<name>.json.
#   usage: ./run_all.sh [csv] [extra bench args, e.g. --n 1000000]
set -e
CSV=${1:-bplus_tree_data_10m.csv}
[ $# -gt 0 ] && shift
mkdir -p results

run() {
  name=$1; shift
  echo ">>> $name"
  ./build/bench --csv "$CSV" --json "results/$name.json" "$@" | tee "results/$name.txt"
}

run memory            --mode mem "$@"
run hybrid_pool1100mb --mode hybrid --pool-mb 1100 "$@"   # pool larger than the index
run disk_oscache      --mode disk --no-direct "$@"        # disk-only, but through the OS page cache
run hybrid_pool512mb  --mode hybrid --pool-mb 512 "$@"
run hybrid_pool128mb  --mode hybrid --pool-mb 128 "$@"
run disk              --mode disk "$@"
