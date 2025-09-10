#!/bin/python3 

# This is a script to Launch ChampSim simulations on a cluster connected via ssh 

# Update config -> Build ChampSim -> Copy binary to machines -> Execute -> Copy back results 

# This works in a master - slave way. Jobs are prioritised to the slaves 

import argparse
import sys 
import os
import subprocess
import json
import time
import uuid
import glob
import re
from concurrent.futures import ThreadPoolExecutor
import paramiko
from scp import SCPClient
import getpass
import threading
from collections import defaultdict
import shutil

from _SPEC2017_def import SPEC2017_shortcode, SPEC2017_path
from _server_def import HOST_NAMES as RAW_HOSTS
HOST_CAPACITIES = {}
HOST_NAMES = []

for entry in RAW_HOSTS:
    if isinstance(entry, (list, tuple)) and len(entry) >= 2:
        host, cap = entry[0], int(entry[1])
    else:
        host, cap = entry, 1  # default capacity if not provided
    HOST_NAMES.append(host)
    HOST_CAPACITIES[host] = cap
# Define the warmup and instructions to run 
WARMUP_INSTRUCTIONS = 50000000
SIMULATION_INSTRUCTIONS = 200000000

# Maximum number of concurrent jobs to run across all hosts
MAX_CPU = sum(HOST_CAPACITIES.values())  # Adjust this value based on your cluster capacity

# Parse arguments
parser = argparse.ArgumentParser(description='Run ChampSim on SPEC2017 benchmarks')

parser.add_argument('--benchmark', type=str, help='Benchmark to run (use "SPEC_ALL" to run all benchmarks, "SPEC_2017_ONES" for SPEC2017 subset)', required=True)
parser.add_argument('--name', type=str, help='Directoy name to store the result', required=False)
parser.add_argument('--config', type=str, help='Configuration file', required=False)
parser.add_argument('--clean', action='store_true', help='Clean the build', required=False)
parser.add_argument('--run', action='store_true', help='Skip configuration and build process', required=False) 
parser.add_argument('--host', type=str, help='Specify a single host to run on', required=False)
parser.add_argument('--max-cpu', type=int, help=f'Maximum number of concurrent jobs (default: {MAX_CPU})', required=False)

args = parser.parse_args()

# Override MAX_CPU if specified
if args.max_cpu:
    MAX_CPU = args.max_cpu
    print(f"Maximum concurrent jobs set to: {MAX_CPU}")

# Generate a unique ID for this run to avoid conflicts with simultaneous runs
run_id = str(uuid.uuid4())[:8]
remote_run_dir = f"~/champsim/run_{run_id}"
remote_bin_path = f"{remote_run_dir}/bin/champsim"
remote_log_base = f"{remote_run_dir}/results"

# Create a local copy of the binary for this specific run to avoid interference
local_bin_copy = f"./bin/champsim_run_{run_id}"

# Handle SPEC_ALL special case
if args.benchmark == "SPEC_ALL":
    print("Running ALL SPEC 2006 + 2017 benchmarks")
    matching_benchmarks = SPEC2017_shortcode.copy()
    print(f"Found {len(matching_benchmarks)} benchmark categories with {sum(len(traces) for traces in matching_benchmarks.values())} total traces")
elif args.benchmark == "SPEC_2017":
    print("Running SPEC2017 benchmarks")
    # Import the spec2017_ones list
    from _SPEC2017_def import spec2017_ones
    matching_benchmarks = {}
    for key in spec2017_ones:
        if key in SPEC2017_shortcode:
            matching_benchmarks[key] = SPEC2017_shortcode[key]
    print(f"Found {len(matching_benchmarks)} benchmark categories with {sum(len(traces) for traces in matching_benchmarks.values())} total traces")
elif args.benchmark == "SPEC_2006":
    print("Running SPEC2006 benchmarks")
    # Import the spec2006_ones list
    from _SPEC2017_def import spec2006_ones
    matching_benchmarks = {}
    for key in spec2006_ones:   
        if key in SPEC2017_shortcode:
            matching_benchmarks[key] = SPEC2017_shortcode[key]
    print(f"Found {len(matching_benchmarks)} benchmark categories with {sum(len(traces) for traces in matching_benchmarks.values())} total traces")
else:
    # See if input benchmark is valid
    matching_benchmarks = {}
    for key in SPEC2017_shortcode:
        if args.benchmark in key:
            matching_benchmarks[key] = SPEC2017_shortcode[key]

    if not matching_benchmarks:
        print("No benchmarks found matching: ", args.benchmark)
        print("Available benchmarks: ", ", ".join(SPEC2017_shortcode.keys()))
        print("Use 'SPEC_ALL' to run all benchmarks or 'SPEC_2017_ONES' for SPEC2017 subset")
        sys.exit(1)

    print(f"Found {len(matching_benchmarks)} matching benchmark categories:")
    for key in matching_benchmarks:
        print(f"  - {key} ({len(matching_benchmarks[key])} traces)")

class SSHConnectionPool:
    """Thread-safe SSH connection pool to reuse connections"""
    
    def __init__(self, password=None):
        self.password = password
        self.connections = defaultdict(list)  # host -> list of available connections
        self.active_connections = defaultdict(int)  # host -> count of active connections
        self.lock = threading.Lock()
        self.max_connections_per_host = HOST_CAPACITIES # Limit connections per host
    
    def get_connection(self, host):
        """Get an available connection or create a new one"""
        with self.lock:
            # Try to reuse an existing connection
            if self.connections[host]:
                client = self.connections[host].pop()
                # Test if connection is still alive
                try:
                    client.exec_command("echo test", timeout=5)
                    self.active_connections[host] += 1
                    return client
                except:
                    # Connection is dead, close it
                    try:
                        client.close()
                    except:
                        pass
            
                        # Create new connection if under limit for this host
            if self.active_connections[host] < self.max_connections_per_host.get(host, 1):
                try:
                    client = paramiko.SSHClient()
                    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                    # Use key-based (passwordless) authentication when no password is provided
                    if self.password:
                        client.connect(host, password=self.password, timeout=30)
                    else:
                        client.connect(host, timeout=30, allow_agent=True, look_for_keys=True)
                    self.active_connections[host] += 1
                    return client
                except Exception as e:
                    print(f"Failed to create connection to {host}: {str(e)}")
                    raise
            
            # If we reach here, we've hit the connection limit
            raise Exception(
                f"Connection pool exhausted for {host} "
                f"(max: {self.max_connections_per_host.get(host, 1)})"
            )
    def return_connection(self, host, client):
        """Return a connection to the pool"""
        with self.lock:
            try:
                # Test if connection is still alive
                client.exec_command("echo test", timeout=5)
                self.connections[host].append(client)
                self.active_connections[host] -= 1
            except:
                # Connection is dead, close it and decrease counter
                try:
                    client.close()
                except:
                    pass
                self.active_connections[host] -= 1
    
    def close_all(self):
        """Close all connections in the pool"""
        with self.lock:
            for host_connections in self.connections.values():
                for client in host_connections:
                    try:
                        client.close()
                    except:
                        pass
            self.connections.clear()
            self.active_connections.clear()

# Global connection pool
ssh_pool = None

def setup_ssh_client(host):
    """Setup SSH client for a host using connection pool"""
    global ssh_pool
    if ssh_pool is None:
        raise Exception("SSH connection pool not initialized")
    return ssh_pool.get_connection(host)

def return_ssh_client(host, client):
    """Return SSH client to connection pool"""
    global ssh_pool
    if ssh_pool is not None:
        ssh_pool.return_connection(host, client)

def test_ssh_connections():
    """Test SSH connections to all hosts"""
    print("Testing SSH connections...")
    for host in HOST_NAMES:
        print(f"Testing connection to {host}...")
        try:
            client = setup_ssh_client(host)
            # Test a simple command
            stdin, stdout, stderr = client.exec_command("echo 'Connection successful'")
            output = stdout.read().decode().strip()
            error = stderr.read().decode().strip()
            
            if output == "Connection successful":
                print(f"✓ Successfully connected to {host}")
                # Get load average
                stdin, stdout, stderr = client.exec_command("uptime")
                uptime_output = stdout.read().decode().strip()
                load_avg = uptime_output.split("load average:")[1].strip() if "load average:" in uptime_output else "N/A"
                print(f"  Load Average: {load_avg}")
            else:
                print(f"✗ Connection to {host} failed: {error}")
                sys.exit(1)
                
            return_ssh_client(host, client)
        except Exception as e:
            print(f"✗ Connection to {host} failed: {str(e)}")
            sys.exit(1)
    print("All SSH connections successful!")

def copy_to_remote(host, local_path, remote_path):
    """Copy file to remote host using connection pool"""
    client = setup_ssh_client(host)
    try:
        scp = SCPClient(client.get_transport())
        
        # Create remote directory structure
        remote_dir = os.path.dirname(remote_path)
        stdin, stdout, stderr = client.exec_command(f"mkdir -p {remote_dir}")
        stdout.channel.recv_exit_status()  # Wait for command to complete
        
        scp.put(local_path, remote_path)
        scp.close()
    finally:
        return_ssh_client(host, client)

def copy_from_remote(host, remote_path, local_path):
    """Copy file from remote host using connection pool"""
    client = setup_ssh_client(host)
    try:
        scp = SCPClient(client.get_transport())
        
        # Create local directory structure
        local_dir = os.path.dirname(local_path)
        os.makedirs(local_dir, exist_ok=True)
        
        scp.get(remote_path, local_path)
        scp.close()
    finally:
        return_ssh_client(host, client)

def run_remote_command(host, command):
    """Run command on remote host using connection pool"""
    client = setup_ssh_client(host)
    try:
        stdin, stdout, stderr = client.exec_command(command)
        output = stdout.read().decode()
        error = stderr.read().decode()
        return output, error
    finally:
        return_ssh_client(host, client)

# Initialize SSH connection pool
print("Initializing SSH connection pool...")
ssh_pool = SSHConnectionPool()

# Test connections before proceeding
test_ssh_connections()

# Filter hosts if --host is specified
if args.host:
    print(f"Running only on host: {args.host}")
    HOST_NAMES = [args.host]
    HOST_CAPACITIES = {args.host: HOST_CAPACITIES.get(args.host, MAX_CPU)}
    MAX_CPU = HOST_CAPACITIES[args.host]

prefetcher = "misc" # Default 

config_path = args.config if args.config else "champsim_config.json"
with open(config_path) as config_file:
    config = json.load(config_file)
    
dir_name = config["L1D"]["prefetcher"] + '-' + config["L2C"]["prefetcher"]

if not args.name: # Auto parse the name of the L1D prefetcher
    prefetcher = dir_name
else: 
    prefetcher = args.name

if not args.run:
    # Update configuration
    print("======================") 
    print("Updating Configuration")
    print("======================")
    if args.config:
        result = subprocess.run(["./config.sh", args.config])
    else: 
        result = subprocess.run(["./config.sh", "champsim_config.json"])
    if result.returncode != 0:
        print("Configuration failed")
        sys.exit(1)
        
    # Get the prefetcher from the config file again
    with open(config_path) as config_file:
        config = json.load(config_file)
        prefetcher_l1 = config["L1D"]["prefetcher"]
        prefetcher_l2 = config["L2C"]["prefetcher"]

    print("**********************") 
    print(f"L1D : {prefetcher_l1}")
    print(f"L2C : {prefetcher_l2}")
    print(f"Name: {prefetcher}")
    print("**********************")
    
    confirm = input("Continue? [Y/n]: ") 
    
    if not (confirm.lower() in ["y","yes"] or confirm == ""):
        exit(0)

    # Make
    print("=================")
    print("Building ChampSim")
    print("=================")
    if args.clean:
        result = subprocess.run(["make", "clean"])
        if result.returncode != 0:
            print("Clean failed")
            sys.exit(1)
        result = subprocess.run(["make"])
    else: 
        result = subprocess.run(["make"])
    if result.returncode != 0:
        print("Build failed")
        sys.exit(1)

    # Create a local copy of the binary for this specific run
    print("========================")
    print("Creating Binary Copy")
    print(f"Run ID: {run_id}")
    print("========================")
    
    try:
        shutil.copy2("./bin/champsim", local_bin_copy)
        print(f"Created local binary copy: {local_bin_copy}")
    except Exception as e:
        print(f"Failed to create binary copy: {str(e)}")
        sys.exit(1)
else:
    # When using --run, still create a local copy to ensure isolation
    print("========================")
    print("Creating Binary Copy")
    print(f"Run ID: {run_id}")
    print("========================")
    
    if not os.path.exists("./bin/champsim"):
        print("Error: No existing binary found. Please build first or run without --run flag.")
        sys.exit(1)
    
    try:
        shutil.copy2("./bin/champsim", local_bin_copy)
        print(f"Created local binary copy: {local_bin_copy}")
    except Exception as e:
        print(f"Failed to create binary copy: {str(e)}")
        sys.exit(1)

# Copy binary to all hosts
print("=================")
print("Copying Binary ")
print(f"Run ID: {run_id}")
print("=================")
for host in HOST_NAMES:
    print(f"Copying to {host}...")
    # Create a unique directory for this run on the remote host
    run_remote_command(host, f"mkdir -p {os.path.dirname(remote_bin_path)}")
    copy_to_remote(host, local_bin_copy, remote_bin_path)

start_time = time.time()

# Run simulation for all benchmarks in parallel 
print("==================")
print("Running Simulation")
print("==================")

# Print selected prefetcher
print("Configuration Summary:")
print(f"  Prefetcher Directory: {prefetcher}")
print(f"  Total Benchmarks: {sum(len(benchmarks) for benchmarks in matching_benchmarks.values())}")
print(f"  Hosts: {', '.join(HOST_NAMES)}")
print(f"  Max Concurrent Jobs: {MAX_CPU}")
print()

# Collect all matching benchmarks with their category names
all_benchmarks = []
for category, benchmarks in matching_benchmarks.items():
    # Make output directory using the actual category name from SPEC2017_shortcode
    if not os.path.exists(f"results/{prefetcher}/{category}"):
        os.makedirs(f"results/{prefetcher}/{category}")
    
    for benchmark in benchmarks:
        all_benchmarks.append((benchmark, category))

# ------------------------------
# Progress tracking
# ------------------------------
total_simulations = len(all_benchmarks)
completed_simulations = 0
progress_lock = threading.Lock()

# NEW: track how many simulation jobs are currently running on each host (separate from
# ssh_pool.active_connections which counts **open SSH channels**).
# This prevents double-counting when a job opens several SSH sessions during its lifetime.
running_jobs = defaultdict(int)

def get_load_averages():
    """Get load average of all hosts."""
    
    host_loads = {}
    
    def fetch_load(host):
        try:
            # run_remote_command handles the connection pooling
            output, error = run_remote_command(host, "uptime")
            if "load average:" in output:
                # Parse the 1-minute load average
                load_avg = float(output.split('load average:')[1].split(',')[0].strip())
                return host, load_avg
            else:
                print(f"Warning: Could not parse load average for {host}. Error: {error}")
                return host, float('inf')
        except Exception as e:
            print(f"Warning: Could not get load average for {host}: {e}")
            return host, float('inf')

    # Fetch loads from all hosts in parallel
    with ThreadPoolExecutor(max_workers=len(HOST_NAMES)) as executor:
        futures = [executor.submit(fetch_load, host) for host in HOST_NAMES]
        for future in futures:
            host, load = future.result()
            host_loads[host] = load
            
    return host_loads

def run_benchmark_on_host(benchmark_tuple, job_number, total):
    """Run a benchmark on a specific host"""
    benchmark, category = benchmark_tuple
    global completed_simulations, total_simulations

    # Wait until at least one host has free capacity
    while True:
        with ssh_pool.lock:
            # Find a host with spare capacity
            free_hosts = [h for h in HOST_NAMES
                          if running_jobs[h] < HOST_CAPACITIES[h]]
            if free_hosts:
                host = min(
                    free_hosts,
                    key=lambda h: running_jobs[h] / HOST_CAPACITIES[h]
                )
                # Reserve a slot for this job **before** leaving the lock so no other thread can grab it.
                running_jobs[host] += 1
                break
        time.sleep(0.5)

    try:
        print(f"Dispatching {benchmark} to {host}... ")
        
        # Create remote log directory with unique run ID - use actual category name
        remote_log_dir = f"{remote_run_dir}/results/{prefetcher}/{category}"
        run_remote_command(host, f"mkdir -p {remote_log_dir}")
        
        # Run simulation using the binary in unique directory
        output, error = run_remote_command(
            host,
            f"cd {remote_run_dir} && {remote_bin_path} --warmup-instructions {WARMUP_INSTRUCTIONS} "
            f"--simulation-instructions {SIMULATION_INSTRUCTIONS} {SPEC2017_path}{benchmark} > "
            f"{remote_log_dir}/{benchmark.split('.')[1]}.txt 2>&1",
        )
        
        # Copy results back - use actual category name for directory, trace name for file
        copy_from_remote(
            host,
            f"{remote_log_dir}/{benchmark.split('.')[1]}.txt",
            f"results/{prefetcher}/{category}/{benchmark.split('.')[1]}.txt",
        )
        
        # Update progress counters safely
        with progress_lock:
            completed_simulations += 1
            finished = completed_simulations

        print(f"Completed {benchmark} on {host}. [Progress: {finished} / {total_simulations}]")
        return output, error
    finally:
        # Always release the job slot, even if the simulation failed
        with ssh_pool.lock:
            running_jobs[host] -= 1

# Distribute benchmarks across hosts
host_index = 0
futures = []

with ThreadPoolExecutor(max_workers=MAX_CPU) as executor:
    for i, benchmark_tuple in enumerate(all_benchmarks):
        futures.append(executor.submit(run_benchmark_on_host, benchmark_tuple, i, len(all_benchmarks) - 1))
        

# Wait for all simulations to complete
for future in futures:
    future.result()

# Clean up remote directories after completion (optional)
print("============")
print("Cleaning Up ")
print("============")

print("Deleting remote files...")
for host in HOST_NAMES:

    try:
        run_remote_command(host, f"rm -rf {remote_run_dir}")
        print(f"Cleaned up run directory on {host}")
    except:
        print(f"Failed to clean up run directory on {host}")

# Close all SSH connections
print("Closing SSH connections...")
ssh_pool.close_all()

# Clean up local binary copy
print("Cleaning up local binary copy...")
try:
    if os.path.exists(local_bin_copy):
        os.remove(local_bin_copy)
        print(f"Removed local binary copy: {local_bin_copy}")
except Exception as e:
    print(f"Failed to remove local binary copy: {str(e)}")

# Print completion message
print("===================")
print("Simulation Complete")
print("===================")

end_time = time.time()

# Print elapsed time in minutes
elapsed_time = end_time - start_time
elapsed_time_minutes = elapsed_time / 60
# Print test name 
print(f"Simulated: {prefetcher}")
print(f"Simulation time: {elapsed_time_minutes:.2f} minutes")





