#!/bin/python3  

import subprocess
import argparse 
import sys 
import os 
import json 
from concurrent.futures import ThreadPoolExecutor

from _SPEC2017_def import SPEC2017_SHORTCODE, SPEC2017_PATH

# Define the warmup and instructions to run (in millions)
WARMUP_INSTRUCTIONS = 0
SIMULATION_INSTRUCTIONS = 2

# Parse arguments
parser = argparse.ArgumentParser(description='Run ChampSim on SPEC2017 benchmarks')

parser.add_argument('--benchmark', type=str, help='Benchmark to run', required=True)
parser.add_argument('--config', type=str, help='Configuration file', required=False)
parser.add_argument('--name', type=str, help='Optional name for result directory', required=False)
parser.add_argument('--clean', action='store_true', help='Clean the build', required=False)
parser.add_argument('--no_conf', action='store_true', help='Skip configuration (Still Make)', required=False) 
parser.add_argument('--cppflags', type=str, help='Extra CPPFLAGS to pass to make (e.g. "-DKAIROS_DBUG -DTEST_DBUG")', required=False)

args = parser.parse_args()

# See if input benchmark is valid
if args.benchmark not in SPEC2017_SHORTCODE:
    print("Invalid benchmark: ", args.benchmark)
    sys.exit(1)

prefetcher = "misc" # Default 

config_path = args.config if args.config else "no_prefetch.json"
with open(config_path) as config_file:
    config = json.load(config_file)
    prefetcher_selected_L1 = config["L1D"]["prefetcher"]
    prefetcher_selected_L2 = config["L2C"]["prefetcher"]

if args.config:
  if not args.name: # Auto parse the name of the prefetchers
      prefetcher = f"{prefetcher_selected_L1}-{prefetcher_selected_L2}"
  else: 
      prefetcher = args.name
else: prefetcher = "no"

if not args.no_conf:
    # Update configuration
    print("======================") 
    print("Updating Configuration")
    print("======================")
    result = subprocess.run(["./config.sh", config_path])
    if result.returncode != 0:
        print("Configuration failed")
        sys.exit(1)
        
print("**********************") 
print(f"Prefetchers Selected:  L1={prefetcher_selected_L1}  L2={prefetcher_selected_L2}")
print(f"Name: {prefetcher}")
print("**********************")

# Make
print("=================")
print("Building ChampSim")
print("=================")

if args.clean:
    result = subprocess.run(["make", "clean"])
    if result.returncode != 0:
        print("Clean failed")
        sys.exit(1)

make_cmd = ["make"]
if args.cppflags:
    make_cmd.append(f"EXTRA_CPPFLAGS={args.cppflags}")

result = subprocess.run(make_cmd)
if result.returncode != 0:
    print("Build failed")
    sys.exit(1)

# Run simulation for all benchmarks in parallel 
print("==================")
print("Running Simulation")
print("==================")

# Print selected prefetcher
print(f"Prefetcher: {prefetcher}")

# Assign job numbers to the bechmarks
kernel_args = []
for i, benchmark in enumerate(SPEC2017_SHORTCODE[args.benchmark]):
    kernel_args.append((benchmark, i, len(SPEC2017_SHORTCODE[args.benchmark]) - 1))

# Make output directory 
if not os.path.exists(f"results/{prefetcher}/{args.benchmark}"):
        os.makedirs(f"results/{prefetcher}/{args.benchmark}")

def champsim_kernel(kernel_arg):

    (benchmark, job_number, total) = kernel_arg

    benchmark_split = benchmark.split('.')
    benchmark_num = benchmark_split[0]
    benchmark_name = benchmark_split[1]

    with open(f"results/{prefetcher}/{args.benchmark}/{benchmark_num}.{benchmark_name}.txt", "w+") as output_file:
        print(f"Dispatching {benchmark} ... [{job_number+1} / {total+1}]")
        result = subprocess.run([
            "./bin/champsim", 
            "--warmup-instructions", f"{WARMUP_INSTRUCTIONS}000000",
            "--simulation-instructions", f"{SIMULATION_INSTRUCTIONS}000000",
            SPEC2017_PATH + benchmark
        ], stdout=output_file, stderr=subprocess.STDOUT)
        print(f"Completed {benchmark}. [{job_number+1} / {total+1}] with status {result.returncode}")
    
    return 

with ThreadPoolExecutor() as executor:
    executor.map(champsim_kernel, kernel_args)

# Print completion message
print("===================")
print("Simulation Complete")
print("===================")


