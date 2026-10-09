#!/bin/sh
# Runs complete.mjs N times (default 5); keeps each log as out/complete-run-<i>.txt.
n=${1:-5}
for i in $(seq 1 $n); do
  node "$(dirname "$0")/complete.mjs" > "$(dirname "$0")/out/complete-run-$i.txt" 2>&1
  echo "run $i: $(grep -c PAGEERROR "$(dirname "$0")/out/complete-run-$i.txt") errors, $(grep -c '\[hang\] main thread stuck' "$(dirname "$0")/out/complete-run-$i.txt") hangs, next mission $(grep -c 'next mission: playable' "$(dirname "$0")/out/complete-run-$i.txt")"
done
