#!/usr/bin/env python3
"""
Generate compressed branch traces (.bz2) for each ChampSim simpoint on a cluster.

This is a streamlined derivative of `simulate_cluster.py` that:
  • Builds/ships ChampSim as before (unless --run is specified).
  • Executes each benchmark with the environment variable `BRANCH_TRACE_FILE` set
    to `<simpoint>.bz2` so the runtime tracer emits a compressed branch trace.
  • Retrieves every generated trace back to the local `results/<prefetcher>/<category>/` dir.

The majority of cluster-orchestration (SSH pooling, file copy, job scheduling)
code is borrowed verbatim from `simulate_cluster.py` to stay consistent.
"""
import os, sys, argparse, uuid, time, json, shutil, subprocess, threading, re
from concurrent.futures import ThreadPoolExecutor
from collections import defaultdict
from pathlib import Path

# --- BEGIN: shared imports from original script ---
from _SPEC2017_def import SPEC2017_shortcode, SPEC2017_path
from _server_def import HOST_NAMES as RAW_HOSTS
import paramiko
from scp import SCPClient
# --- END: shared imports ---

# -----------------------------
# Configuration
# -----------------------------
WARMUP_INSTRUCTIONS = 20_000_000
SIMULATION_INSTRUCTIONS = 500_000_000

HOST_CAPACITIES = {}
HOST_NAMES = []
for entry in RAW_HOSTS:
    if isinstance(entry, (list, tuple)) and len(entry) >= 2:
        host, cap = entry[0], int(entry[1])
    else:
        host, cap = entry, 1
    HOST_NAMES.append(host)
    HOST_CAPACITIES[host] = cap
MAX_CPU = sum(HOST_CAPACITIES.values())

# -----------------------------
# CLI parsing
# -----------------------------
parser = argparse.ArgumentParser(description="Generate branch traces for ChampSim benchmarks on a cluster")
parser.add_argument("--benchmark", required=True, help="Benchmark to run (SPEC_ALL, SPEC_2017, SPEC_2006, or substring match)")
parser.add_argument("--name", help="Result subdirectory name")
parser.add_argument("--config", default="champsim_config.json", help="ChampSim config JSON path")
parser.add_argument("--clean", action="store_true", help="Run 'make clean' before build")
parser.add_argument("--run", action="store_true", help="Skip configuration/build")
parser.add_argument("--host", help="Restrict to a single host")
parser.add_argument("--max-cpu", type=int, help="Override total parallel jobs")
args = parser.parse_args()

if args.max_cpu:
    MAX_CPU = args.max_cpu

# Unique ID for isolation across runs
run_id = str(uuid.uuid4())[:8]
remote_run_dir = f"~/champsim/run_{run_id}"
remote_bin_path = f"{remote_run_dir}/bin/champsim"
remote_result_base = f"{remote_run_dir}/results"
local_bin_copy = f"./bin/champsim_run_{run_id}"

# -----------------------------
# Determine benchmarks
# -----------------------------
if args.benchmark == "SPEC_ALL":
    matching_benchmarks = SPEC2017_shortcode.copy()
elif args.benchmark == "SPEC_2017":
    from _SPEC2017_def import spec2017_ones
    matching_benchmarks = {k: SPEC2017_shortcode[k] for k in spec2017_ones if k in SPEC2017_shortcode}
elif args.benchmark == "SPEC_2006":
    from _SPEC2017_def import spec2006_ones
    matching_benchmarks = {k: SPEC2017_shortcode[k] for k in spec2006_ones if k in SPEC2017_shortcode}
else:
    matching_benchmarks = {k: v for k, v in SPEC2017_shortcode.items() if args.benchmark in k}
    if not matching_benchmarks:
        print("No benchmarks found matching", args.benchmark)
        sys.exit(1)

# -----------------------------
# SSH Pooling utilities (unchanged)
# -----------------------------
class SSHConnectionPool:
    def __init__(self):
        self.connections = defaultdict(list)
        self.active = defaultdict(int)
        self.lock = threading.Lock()

    def _new_client(self, host):
        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        client.connect(host, allow_agent=True, look_for_keys=True)
        return client

    def get(self, host):
        with self.lock:
            if self.connections[host]:
                cli = self.connections[host].pop()
            elif self.active[host] < HOST_CAPACITIES[host]:
                cli = self._new_client(host)
            else:
                return None  # caller will retry
            self.active[host] += 1
            return cli

    def put(self, host, cli):
        with self.lock:
            try:
                cli.exec_command("echo test", timeout=3)
                self.connections[host].append(cli)
            except:
                try: cli.close()
                except: pass
            self.active[host] -= 1

    def close_all(self):
        with self.lock:
            for pool in self.connections.values():
                for c in pool:
                    try: c.close()
                    except: pass
            self.connections.clear()
            self.active.clear()

ssh_pool = SSHConnectionPool()

# -----------------------------
# Helper functions (copy minimal)
# -----------------------------

def copy_to_remote(host, local, remote):
    c = None
    while c is None:
        c = ssh_pool.get(host)
    try:
        scp = SCPClient(c.get_transport())
        remote_dir = os.path.dirname(remote)
        c.exec_command(f"mkdir -p {remote_dir}")
        scp.put(local, remote)
        scp.close()
    finally:
        ssh_pool.put(host, c)

def copy_from_remote(host, remote, local):
    c = None
    while c is None:
        c = ssh_pool.get(host)
    try:
        scp = SCPClient(c.get_transport())
        Path(local).parent.mkdir(parents=True, exist_ok=True)
        scp.get(remote, local)
        scp.close()
    finally:
        ssh_pool.put(host, c)

def run_remote(host, cmd):
    c = None
    while c is None:
        c = ssh_pool.get(host)
    try:
        stdin, stdout, stderr = c.exec_command(cmd)
        out, err = stdout.read().decode(), stderr.read().decode()
        return out, err
    finally:
        ssh_pool.put(host, c)

# -----------------------------
# Build ChampSim locally (reuse simulate_cluster behaviour)
# -----------------------------
if not args.run:
    if args.clean:
        subprocess.run(["make", "clean"], check=True)
    # re-configure
    subprocess.run(["./config.sh", args.config], check=True)
    subprocess.run(["make"], check=True)
    shutil.copy2("./bin/champsim", local_bin_copy)
else:
    if not os.path.exists("./bin/champsim"):
        print("No ChampSim binary found. Build first or omit --run flag.")
        sys.exit(1)
    shutil.copy2("./bin/champsim", local_bin_copy)

# -----------------------------
# Ship binary to hosts
# -----------------------------
for h in HOST_NAMES:
    run_remote(h, f"mkdir -p {remote_run_dir}/bin")
    copy_to_remote(h, local_bin_copy, remote_bin_path)

# -----------------------------
# Flatten benchmarks list with category
# -----------------------------
all_jobs = [(bench, cat) for cat, lst in matching_benchmarks.items() for bench in lst]

total_jobs = len(all_jobs)
completed = 0
completed_lock = threading.Lock()

running_jobs = defaultdict(int)

# -----------------------------
# Job executor
# -----------------------------

def job_thread(job):
    global completed
    bench, category = job

    # Wait for slot
    while True:
        with ssh_pool.lock:
            free_hosts = [h for h in HOST_NAMES if running_jobs[h] < HOST_CAPACITIES[h]]
            if free_hosts:
                host = min(free_hosts, key=lambda h: running_jobs[h])
                running_jobs[host] += 1
                break
        time.sleep(0.5)

    try:
        simpoint_name = Path(bench).stem  # e.g., 436.cactusADM-1804B.champsimtrace.xz -> 436.cactusADM-1804B
        remote_cat_dir = f"{remote_result_base}/{category}"
        run_remote(host, f"mkdir -p {remote_cat_dir}")

        count_path_remote = f"{remote_cat_dir}/{simpoint_name}_counts.txt"

        cmd = (
            f"cd {remote_run_dir} && "
            f"env BRANCH_TRACE_FILE={remote_cat_dir}/{simpoint_name}.bz2 "
            f"BRANCH_COUNT_FILE={count_path_remote} "
            f"WARMUP_INSTR={WARMUP_INSTRUCTIONS} "
            f"{remote_bin_path} --warmup-instructions {WARMUP_INSTRUCTIONS} "
            f"--simulation-instructions {SIMULATION_INSTRUCTIONS} "
            f"{SPEC2017_path}{bench} > /dev/null 2>&1"
        )
        run_remote(host, cmd)

        # Retrieve trace and count file
        local_base_dir = f"results/branch_traces/{category}"
        local_trace = f"{local_base_dir}/{simpoint_name}.bz2"
        local_count = f"{local_base_dir}/{simpoint_name}_counts.txt"
        copy_from_remote(host, f"{remote_cat_dir}/{simpoint_name}.bz2", local_trace)
        copy_from_remote(host, count_path_remote, local_count)

        with completed_lock:
            completed += 1
            print(f"[{completed}/{total_jobs}] Done {bench} -> {local_trace}")
    finally:
        with ssh_pool.lock:
            running_jobs[host] -= 1

# -----------------------------
# Launch jobs
# -----------------------------
print(f"Dispatching {total_jobs} jobs across {len(HOST_NAMES)} hosts...")
with ThreadPoolExecutor(max_workers=MAX_CPU) as ex:
    for j in all_jobs:
        ex.submit(job_thread, j)

print("All branch traces generated!")

# -----------------------------
# Cleanup
# -----------------------------
for h in HOST_NAMES:
    run_remote(h, f"rm -rf {remote_run_dir}")
ssh_pool.close_all()
try:
    os.remove(local_bin_copy)
except: pass
