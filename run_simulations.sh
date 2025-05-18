#!/bin/bash

mkdir -p logs

SCRIPT="simulate.py"

# List your benchmarks and configs here
BENCHMARKS=("bwaves603")  # Add more benchmarks
CONFIGS=("" "berti.json" "multi_bop.json" "bop_stride.json" "kairios.json")  # Add more configs

MAX_JOBS=1  # Number of parallel jobs allowed
TOTAL_JOBS=$(( ${#BENCHMARKS[@]} * ${#CONFIGS[@]} ))

run_job() {
  local benchmark="$1"
  local config="$2"
  local logfile="logs/${benchmark}_${config}.log"

  echo "[START] Benchmark: $benchmark | Config: $config"

  if [ -z "$config" ]; then
    python simulate.py --benchmark "$benchmark" > "$logfile" 2>&1
  else
    python simulate.py --benchmark "$benchmark" --config "$config" > "$logfile" 2>&1
  fi

  echo "[Done] Benchmark: $benchmark | Config: $config"
}

# Iterate over all combinations
for benchmark in "${BENCHMARKS[@]}"; do
  for config in "${CONFIGS[@]}"; do
    run_job "$benchmark" "$config" &

    # Limit the number of concurrent jobs
    while [ "$(jobs -r | wc -l)" -ge "$MAX_JOBS" ]; do
      sleep 1
    done
  done
done

# Wait for all background jobs
wait

echo "✅ All $TOTAL_JOBS jobs completed."
