import os
import matplotlib.pyplot as plt
from collections import defaultdict
import numpy as np
import math
import re
import json

plt.style.use('default')

from _SPEC2017_def import SPEC2017_shortcode, spec2017_ones, spec2006_ones, memint2017_ones
from _SPEC_WEIGHTS import SPEC2017_SHORTCODE_WEIGHTS

# --- CONFIGURABLE ---
LOG_DIR = 'results'
GRAPH_DIR = 'graphs'
OUTPUT = "pdf"


BENCHMARKS = ['bwaves603', 'cactuBSSN607', 'fotonik3d649', 'gcc602', 'roms654', 'xalancbmk623', 'xz657']    

BENCHMARKS = memint2017_ones
BENCHMARKS = spec2017_ones
# BENCHMARKS = spec2006_ones
# BENCHMARKS = list(SPEC2017_shortcode.keys())

BENCHMARKS = [bm for bm in BENCHMARKS if bm != 'milc433']
BENCHMARKS = [bm for bm in BENCHMARKS if bm != 'gromacs435']
# BENCHMARKS = [bm for bm in BENCHMARKS if bm != 'mcf429']

BASELINE = 'no-no'

# PREFETCHERS = ['berti_stride-no', 'stride-bop', 'caerus']

# PREFETCHERS = ['ip_stride-no', 'no-caerus', 'caerus_4_train_8_offsets', 'caerus_8_train_8_offsets', 'caerus_8_train_16_offsets','caerus_16_train_16_offsets']
# PREFETCHERS = ['ip_stride-no', 'bop-no','mlop-no', 'bertihp-no','caerus-no-101621', 'caerus-no-108621']
PREFETCHERS = ['ip_stride-no', 'no-bop','mlop-no', 'berti-no', 'no-caerus_84821_retrainlast', 'no-caerus_old']
# PREFETCHERS = ['no-caerus', 'no-caerus_delay_60', 'no-caerus_delay_120', 'no-caerus_delay_200']

# PREFETCHERS = ['bertihp-no','mlop-no', 'no-caerus', 'caerus-no', 'caerus-no-41811', 'caerus-no-421011']
# PREFETCHERS = ['caerus_single_offset','caerus_8_offset','caerus_8_offset_no_overlap','caerus_8_offset_acc_621_train4']
# PREFETCHERS = ['no-bop','caerus_single_offset', 'no-caerus-11621', 'caerus_8_offset_acc_621']


# PREFETCHERS = ['stride','berti-no', 'no-bop', 'mlop-no', 'amlopd-no', 'amlop-no']
# PREFETCHERS = ['mlop-no', 'berti-no', 'no-bop', 'caerus-no', 'caerus-no_81811_31_100', 'amlop-no']
# PREFETCHERS = ['no-bop', 'berti-no', 'mlop-no', 'no-caerus-84821', 'no-caerus_84821_badenabled', 'no-caerus_84821_retrain', 'no-caerus_84821_pfhitretrain', 'no-caerus_84821_retrainlast']

BASELINE = 'ip_stride-no'
PREFETCHERS = ['ip_stride-bop', 'berti_stride-no', 'mlop_stride-no','ip_stride-caerus', 'ip_stride-caerus_old']

if BASELINE not in PREFETCHERS:
    PREFETCHERS.append(BASELINE)

PLOT_NAME = 'MLOP'

INCLUDE_GEOMEAN = True 
PLOT_TRACE_IPC = False  # Plot raw IPC values from simpoints instead of geomean
ONLY_GEOMEAN_BAR = False
HIGHLIGHT_LAST = False

ONLY_GEOMEAN_LINE = False # BROKEN! 


PLOT_WIDTH = 20
PLOT_HEIGHT = 6

if ONLY_GEOMEAN_BAR:
    PLOT_WIDTH = 4
    PLOT_HEIGHT = 5

BLUES = False

blues = ['#84a1f5', '#4e70d4', '#3052b8', '#0A2472']
blues = ['#0A114A', '#1A4FDB', '#3A7BFF', '#6EC8F7']
blues = ['#A3D1FF', '#4E8EFF', '#2A5CFF', '#0A2472']
blues = ['#FE9D52', '#FFCEA9', '#BBBBBB', '#9ECBED', '#3C97DA'] # Emphasize last color


# --- PARSING FUNCTIONS ---

def _determine_cpu_str(path):
    parts = path.split(os.sep)

    # Extract the prefetcher directory that immediately follows LOG_DIR
    prefetcher_dir = None
    if LOG_DIR in parts:
        idx = parts.index(LOG_DIR)
        if idx + 1 < len(parts):
            prefetcher_dir = parts[idx + 1]

    if not prefetcher_dir:
        return 'cpu0_L2C'  # Fallback to L2C if we cannot infer correctly

    # If it's the double-"no" baseline, skip this entry entirely
    if prefetcher_dir == 'no-no':
        return None

    # Split into L1 / L2 components if possible
    comps = prefetcher_dir.split('-')
    if len(comps) >= 2:
        l1_pref, l2_pref = comps[0], comps[1]
        if l2_pref != 'no':
            return 'cpu0_L2C'
        elif l1_pref != 'no':
            return 'cpu0_L1D'
        else:
            return None  # Both are "no"
    else:
        # Single component – assume L2C prefetcher
        return 'cpu0_L2C'

def parse_ipc_from_file(filepath):
    in_roi_section = False
    with open(filepath) as f:
        for line in f:
            line = line.strip()
            if "Region of Interest Statistics" in line:
                in_roi_section = True
            elif in_roi_section and line.startswith("CPU 0 cumulative IPC:"):
                parts = line.split()
                try:
                    return float(parts[4]) 
                except (IndexError, ValueError):
                    continue
    return None

def parse_dram_from_file(filepath):
    with open(filepath) as f:
        for line in f:
            line = line.strip()
            if line.startswith("LLC TOTAL"):
                parts = line.split()
                try:
                    return float(parts[7]) 
                except (IndexError, ValueError):
                    continue
    return None

def parse_cov_from_file(filepath):
    # Determine which cache level to use based on prefetcher configuration in the path

    cpu_str = _determine_cpu_str(filepath)

    # Skip paths where there is no prefetcher (e.g., no-no)
    if cpu_str is None:
        return None

    in_roi_section = False
    useful = None
    demand_misses = None
    
    with open(filepath) as f:
        for line in f:
            line = line.strip()
            if "Region of Interest Statistics" in line:
                in_roi_section = True
            elif in_roi_section and f"{cpu_str} PREFETCH REQUESTED:" in line:
                parts = line.split()
                useful = float(parts[7])  # USEFUL value
            elif in_roi_section and f"{cpu_str} LOAD" in line:
                parts = line.split()
                demand_misses = float(parts[7])  # MISS value
    
    if useful is not None and demand_misses is not None:
        return useful, demand_misses
    
    return None

def parse_acc_from_file(filepath):
    # Re-use the same helper logic as in parse_cov_from_file

    cpu_str = _determine_cpu_str(filepath)

    if cpu_str is None:
        return None

    in_roi_section = False
    with open(filepath) as f:
        for line in f:
            line = line.strip()
            if "Region of Interest Statistics" in line:
                in_roi_section = True
            elif in_roi_section and f"{cpu_str} PREFETCH REQUESTED:" in line:
                parts = line.split()
                return float(parts[7]), float(parts[9])  # USEFUL, USELESS
        
    return None

# --- DATA GATHERING FUNCTIONS ---
def gather_data(parse_func, metric_name):
    """Generic function to gather data using the specified parsing function"""
    data = defaultdict(dict)
    
    for benchmark in BENCHMARKS:
        for prefetcher in PREFETCHERS:
            path = os.path.join(LOG_DIR, prefetcher, benchmark)
            if not os.path.isdir(path):
                print(f"Missing directory: {path}")
                continue

            for filename in os.listdir(path):
                if filename.endswith('.txt'):
                    simpoint = filename.replace('.txt', '')
                    # Quick and dirty hack, sorry Jacob :(
                    if re.match(r'^\d+\.', simpoint):
                        simpoint = simpoint[len(re.match(r'^\d+\.', simpoint).group(0)):]
                    filepath = os.path.join(path, filename)
                    result = parse_func(filepath)
                    if result is not None:
                        label = f"{benchmark}/{simpoint}"
                        # Standardize prefetcher names for display
                        display_name = prefetcher
                        if prefetcher == 'bop':
                            display_name = 'BOP'
                        elif prefetcher == 'berti_stride':
                            display_name = 'Berti'
                        data[display_name][label] = result
    
    return data

# --- COMPUTE WEIGHTED GEOMEAN ---
def weighted_geomean(values, weights):
    log_sum = 0
    for v, w in zip(values, weights):
        if v <= 0:
            return 0.0  # invalid speedup
        log_sum += math.log(v) * w
    return math.exp(log_sum)

def weighted_arithmetic_mean(values, weights):
    """Computes the weighted arithmetic mean."""
    if not weights:
        return sum(values) / len(values) if values else 0.0
    
    total_weight = sum(weights)
    if total_weight == 0:
        return sum(values) / len(values) if values else 0.0
        
    weighted_sum = sum(v * w for v, w in zip(values, weights))
    return weighted_sum / total_weight

def compute_geomean_speedups(data, metric_type, baseline_name=None):
    """Compute weighted geometric mean speedups for different metric types"""
    geomean_speedups = defaultdict(dict)
    
    for benchmark in BENCHMARKS:
        weight_map = SPEC2017_SHORTCODE_WEIGHTS.get(benchmark, {})
        simpoints = list(weight_map.keys())
        
        # Get display names for prefetchers. For coverage/accuracy we skip the baseline ("no-no").
        display_prefetchers = []
        for p in PREFETCHERS:
            # Optionally skip baseline for non-relative metrics
            if p == baseline_name and metric_type in ['coverage', 'accuracy']:
                continue
            else:
                display_prefetchers.append(p)
        
        for prefetcher in display_prefetchers:
            speedups = []
            weights = []
            
            for sp in simpoints:
                # Quick and dirty hack, sorry Jacob :(
                if re.match(r'^\d+\.', sp):
                    sp_name = sp[len(re.match(r'^\d+\.', sp).group(0)):]
                
                label = f"{benchmark}/{sp_name}"
                
                if metric_type == 'ipc':
                    base_value = data[baseline_name].get(label) if baseline_name else None
                    test_value = data[prefetcher].get(label)
                    if base_value and test_value:
                        speedup = test_value / base_value
                        weight = weight_map[sp]
                        speedups.append(speedup)
                        weights.append(weight)
                
                elif metric_type == 'dram':
                    base_value = data[baseline_name].get(label) if baseline_name else None
                    test_value = data[prefetcher].get(label)
                    if base_value and test_value:
                        speedup = test_value / base_value
                        weight = weight_map[sp]
                        speedups.append(speedup)
                        weights.append(weight)
                
                elif metric_type == 'coverage':
                    cov_data = data[prefetcher].get(label)
                    if cov_data:
                        useful, demand_misses = cov_data
                        if useful == 0:
                            continue  # Skip simpoints with no prefetches
                        coverage = useful / (useful + demand_misses)
                        weight = weight_map[sp]
                        speedups.append(coverage)
                        weights.append(weight)
                
                elif metric_type == 'accuracy':
                    acc_data = data[prefetcher].get(label)
                    if acc_data:
                        useful, useless = acc_data
                        if useful == 0:
                            accuracy = 1.0  # No prefetches issued
                        else:
                            accuracy = useful / (useful + useless)
                        weight = weight_map[sp]
                        speedups.append(accuracy)
                        weights.append(weight)
            
            if speedups and weights:
                
                if metric_type in ['dram', 'coverage', 'accuracy']:
                    geo = weighted_arithmetic_mean(speedups, weights)
                else:
                    geo = weighted_geomean(speedups, weights)
                geomean_speedups[prefetcher][benchmark] = geo
            else:
                print(f"Incomplete data for benchmark: {benchmark} prefetcher: {prefetcher}")
                geomean_speedups[prefetcher][benchmark] = 0.0
    
    # Compute overall geomean across benchmarks
    plot_prefetchers = [p for p in display_prefetchers if p != baseline_name] if baseline_name else display_prefetchers
    
    for prefetcher in plot_prefetchers:
        values = [geomean_speedups[prefetcher][bm] for bm in BENCHMARKS if geomean_speedups[prefetcher][bm] > 0]
        if values:
            log_sum = sum(math.log(v) for v in values)
            overall_geo = math.exp(log_sum / len(values))
        else:
            overall_geo = 0.0
        geomean_speedups[prefetcher]["geomean"] = overall_geo
    
    return geomean_speedups, plot_prefetchers

def compute_trace_speedups(data, metric_type, baseline_name=None):
    """Compute speedups for individual simpoints (traces) instead of geomean across benchmarks"""
    trace_speedups = defaultdict(dict)
    all_trace_labels = []
    
    for benchmark in BENCHMARKS:
        weight_map = SPEC2017_SHORTCODE_WEIGHTS.get(benchmark, {})
        simpoints = list(weight_map.keys())
        
        # Get display names for prefetchers
        display_prefetchers = []
        for p in PREFETCHERS:
            if p == baseline_name and metric_type in ['coverage', 'accuracy']:
                continue
            else:
                display_prefetchers.append(p)
        
        for sp in simpoints:
            # Quick and dirty hack, sorry Jacob :(
            if re.match(r'^\d+\.', sp):
                sp_name = sp[len(re.match(r'^\d+\.', sp).group(0)):]
            else:
                sp_name = sp
            
            label = f"{benchmark}/{sp_name}"
            all_trace_labels.append(label)
            
            for prefetcher in display_prefetchers:
                if metric_type == 'ipc':
                    base_value = data[baseline_name].get(label) if baseline_name else None
                    test_value = data[prefetcher].get(label)
                    if base_value and test_value:
                        speedup = test_value / base_value
                        trace_speedups[prefetcher][label] = speedup
                    else:
                        trace_speedups[prefetcher][label] = 0.0
    
    # Remove baseline from plot prefetchers
    plot_prefetchers = [p for p in display_prefetchers if p != baseline_name] if baseline_name else display_prefetchers
    
    return trace_speedups, plot_prefetchers, all_trace_labels

# --- PLOTTING FUNCTIONS ---
def setup_plot_style():
    plt.style.use('seaborn-v0_8')
    if BLUES:
        plt.rcParams['axes.prop_cycle'] = plt.cycler(color=blues)
    plt.rcParams.update({
        'font.size': 14,
        'axes.labelsize': 14,
        'xtick.labelsize': 14,
        'ytick.labelsize': 14,
        'legend.fontsize': 12
    })

def create_plot(geomean_speedups, plot_prefetchers, metric_name, ylabel, filename, 
                include_geomean=True, include_baseline=True, ylim_bottom=None, ylim_top=None, 
                legend_position='right', only_geomean_bar=False, only_geomean_line=False, 
                plot_traces=False, trace_labels=None):
    """Generic plotting function"""
    setup_plot_style()

    # Create a new list of prefetchers with the display names
    display_prefetchers = []
    # Change the names of the prefetchers to the display names
    for prefetcher in plot_prefetchers:
        if prefetcher == 'no-bop':
            display_prefetchers.append('BOP')
        elif prefetcher == 'bop-no':
            display_prefetchers.append('BOP')
        elif prefetcher == 'bertihp-no':
            display_prefetchers.append('Berti')
        elif prefetcher == 'ip_stride-no':
            display_prefetchers.append('Stride')
        elif prefetcher == 'no-caerus':
            display_prefetchers.append('Caerus')
        elif prefetcher == 'mlop-no':
            display_prefetchers.append('MLOP')
        elif prefetcher == 'caerus_single_offset':
            display_prefetchers.append('Single Offset')
        elif prefetcher == 'caerus_8_offset':
            display_prefetchers.append('+ Multiple Offsets')
        elif prefetcher == 'caerus_8_offset_no_overlap':
            display_prefetchers.append('+ No Overlap')
        elif prefetcher == 'caerus_8_offset_acc_621_train4':
            display_prefetchers.append('+ PC-Local Filtering')
        elif prefetcher == 'caerus_8_offset_acc_621':
            display_prefetchers.append('Caerus')
        else:
            display_prefetchers.append(prefetcher)
    
    if plot_traces and trace_labels:
        # Create trace plot showing individual simpoint results
        fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))
        
        if include_baseline:
            ax.axhline(1.0, linestyle='-', color='black', linewidth=1, label='Baseline')
        
        ax.grid(True, linestyle='--', alpha=0.7, axis='y', zorder=0)
        
        x = np.arange(len(trace_labels))
        bar_width = 0.5 / len(plot_prefetchers)
        
        for i, prefetcher in enumerate(plot_prefetchers):
            heights = [geomean_speedups[prefetcher].get(label, 0.0) for label in trace_labels]
            offsets = x + i * bar_width
            
            # Determine edge color based on HIGHLIGHT_LAST setting
            edge_color = 'black'
            line_width = 1
            if HIGHLIGHT_LAST and i == len(plot_prefetchers) - 1:
                edge_color = '#0A2472'
                line_width = 2
            
            ax.bar(offsets, heights, width=bar_width, label=display_prefetchers[i], 
                   edgecolor=edge_color, linewidth=line_width, zorder=1)
        
        ax.set_xticks(x + bar_width * (len(plot_prefetchers) - 1) / 2)
        # Rotate trace labels for better readability
        ax.set_xticklabels(trace_labels, rotation=90, ha='center', fontsize=10)
        
        if ylim_bottom is not None:
            ax.set_ylim(bottom=ylim_bottom)
        if ylim_top is not None:
            ax.set_ylim(top=ylim_top)
        
        ax.yaxis.set_major_locator(plt.MaxNLocator(nbins=10))
        ax.set_ylabel(ylabel, fontweight='bold')
        ax.set_xlabel("Simpoint Trace", fontweight='bold')
        
        # Adjust legend position
        if legend_position == 'left':
            ax.legend(loc='upper left', bbox_to_anchor=(0.05, 1), ncol=1, 
                     frameon=True, edgecolor='black', prop={'weight': 'bold'})
        elif legend_position == 'top':
            ax.legend(loc='upper center', bbox_to_anchor=(0.5, 1.07), ncol=len(plot_prefetchers), 
                     frameon=True, edgecolor='black', prop={'weight': 'bold'})
        else:  # right
            ax.legend(loc='upper left', bbox_to_anchor=(1, 1), ncol=1, 
                     frameon=True, edgecolor='black', prop={'weight': 'bold'})
    
    elif only_geomean_line:
        # Create line plot for geomean only
        fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))
        
        if include_baseline:
            ax.axhline(1.0, linestyle='-', color='black', linewidth=1, label='baseline')
            # Add legend only for baseline
            loc = 'upper left' if legend_position == 'left' else 'upper right'
            ax.legend(loc=loc, frameon=True, edgecolor='black', prop={'weight': 'bold'})
        
        ax.grid(True, linestyle='--', alpha=0.7, axis='y', zorder=0)
        
        # Plot each prefetcher as a point
        x_positions = np.arange(len(plot_prefetchers))
        geomean_values = [geomean_speedups[prefetcher].get("geomean", 0.0) for prefetcher in plot_prefetchers]
        
        ax.plot(x_positions, geomean_values, 'x-', markersize=10, linewidth=1.5, markeredgewidth=1.5, color='purple')
        
        # If name contains stride, extract the number
        for i, prefetcher in enumerate(plot_prefetchers):
            if 'stride' in prefetcher:
                plot_prefetchers[i] = prefetcher.split('_')[1]
        
        # Set x-axis labels to prefetcher names
        ax.set_xticks(x_positions)
        ax.set_xticklabels(plot_prefetchers, rotation=45, ha='right')
        
        if ylim_bottom is not None:
            ax.set_ylim(bottom=ylim_bottom)
        if ylim_top is not None:
            ax.set_ylim(top=ylim_top)
        
        ax.yaxis.set_major_locator(plt.MaxNLocator(nbins=10))
        ax.set_ylabel(ylabel, fontweight='bold')
        # ax.set_xlabel("Prefetcher", fontweight='bold')
        ax.set_xlabel("Degree", fontweight='bold')

    elif only_geomean_bar:
        # Create bar plot for geomean only
        fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

        if include_baseline:
            ax.axhline(1.0, linestyle='-', color='black', linewidth=1, label='Baseline')
        
        ax.grid(True, linestyle='--', alpha=0.7, axis='y', zorder=0)
        
        # Plot each prefetcher as a point
        x_positions = np.arange(len(plot_prefetchers))

        geomean_values = [geomean_speedups[prefetcher].get("geomean", 0.0) for prefetcher in plot_prefetchers]

        for i, (x, val) in enumerate(zip(x_positions, geomean_values)):
            # Determine edge color based on HIGHLIGHT_LAST setting
            edge_color = 'black'
            line_width = 1
            if HIGHLIGHT_LAST and i == len(plot_prefetchers) - 1:
                edge_color = 'red'
                line_width = 2
            
            # ax.bar(x, val, width=0.5, label='Geomean', color=blues[i % len(blues)])
            ax.bar(x, val, width=0.5, label='Geomean', edgecolor=edge_color, linewidth=line_width)
        
        ax.set_xticks(x_positions)
        ax.set_xticklabels(display_prefetchers, rotation=45, ha='right')

        if ylim_bottom is not None:
            ax.set_ylim(bottom=ylim_bottom)
        if ylim_top is not None:
            ax.set_ylim(top=ylim_top)
        
        ax.yaxis.set_major_locator(plt.MaxNLocator(nbins=10))
        ax.set_ylabel(ylabel, fontweight='bold')
        ax.set_xlabel("Prefetcher", fontweight='bold')

    else:
        # Original bar plot code
        all_labels = BENCHMARKS + (["geomean"] if include_geomean else [])
        x = np.arange(len(all_labels))
        bar_width = 0.5 / len(plot_prefetchers)
        
        fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))
        
        if include_baseline:
            ax.axhline(1.0, linestyle='-', color='black', linewidth=1, label='Baseline')
        
        ax.grid(True, linestyle='--', alpha=0.7, axis='y', zorder=0)
        
        for i, prefetcher in enumerate(plot_prefetchers):
            heights = [geomean_speedups[prefetcher].get(bm, 0.0) for bm in all_labels]
            offsets = x + i * bar_width
            
            # Determine edge color based on HIGHLIGHT_LAST setting
            edge_color = 'black'
            line_width = 1
            if HIGHLIGHT_LAST and i == len(plot_prefetchers) - 1:
                edge_color = '#0A2472'
                line_width = 2
            
            ax.bar(offsets, heights, width=bar_width, label=display_prefetchers[i], 
                   edgecolor=edge_color, linewidth=line_width, zorder=1)
        
        ax.set_xticks(x + bar_width * (len(plot_prefetchers) - 1) / 2)
        ax.set_xticklabels(all_labels, rotation=45, ha='right')
        
        if ylim_bottom is not None:
            ax.set_ylim(bottom=ylim_bottom)
        if ylim_top is not None:
            ax.set_ylim(top=ylim_top)
        
        ax.yaxis.set_major_locator(plt.MaxNLocator(nbins=10))
        ax.set_ylabel(ylabel, fontweight='bold')
        ax.set_xlabel("Benchmark", fontweight='bold')
        
        # Adjust legend position based on legend_position parameter
        if legend_position == 'left':
            ax.legend(loc='upper left', bbox_to_anchor=(0.05, 1), ncol=1, 
                     frameon=True, edgecolor='black', prop={'weight': 'bold'})
        elif legend_position == 'top':
            ax.legend(loc='upper center', bbox_to_anchor=(0.5, 1.07), ncol=6, 
                     frameon=True, edgecolor='black', prop={'weight': 'bold'})
        else:  # right
            if metric_name in ['coverage', 'accuracy']:
                ax.legend(loc='upper left', bbox_to_anchor=(0.8, 1), ncol=1, 
                         frameon=True, edgecolor='black', prop={'weight': 'bold'})
            else:
                ax.legend(loc='upper left', bbox_to_anchor=(1, 1), ncol=1, 
                         frameon=True, edgecolor='black', prop={'weight': 'bold'})
    
    plt.tight_layout(pad=0.1)  # Reduce padding around the plot
    if not os.path.exists(GRAPH_DIR):
        os.makedirs(GRAPH_DIR)
    # increase dpi
    if OUTPUT == "pdf":
        plt.savefig(os.path.join(GRAPH_DIR, filename), format='pdf', bbox_inches='tight')  # Trim excess whitespace
    elif OUTPUT == "png":
        plt.savefig(os.path.join(GRAPH_DIR, filename), format='png', dpi=300, bbox_inches='tight')  # Trim excess whitespace
    plt.close()

# --- MAIN EXECUTION ---
def main():
    # Gather all data
    print("Gathering IPC data...")
    ipc_data = gather_data(parse_ipc_from_file, 'ipc')
    
    print("Gathering DRAM data...")
    dram_data = gather_data(parse_dram_from_file, 'dram')
    
    print("Gathering coverage data...")
    cov_data = gather_data(parse_cov_from_file, 'coverage')
    
    print("Gathering accuracy data...")
    acc_data = gather_data(parse_acc_from_file, 'accuracy')
    
    # Compute geomean speedups for each metric
    print("--------------------------------")
    if PLOT_TRACE_IPC:
        print("Computing IPC trace speedups...")
        ipc_speedups, ipc_plot_prefetchers, ipc_trace_labels = compute_trace_speedups(ipc_data, 'ipc', BASELINE)
        print("IPC trace speedups computed for individual simpoints")
    else:
        print("Computing IPC speedups...")
        ipc_speedups, ipc_plot_prefetchers = compute_geomean_speedups(ipc_data, 'ipc', BASELINE)
        ipc_trace_labels = None
        # print("IPC speedups:", dict(ipc_speedups))\
        print("Geomean IPC speedups:")
        for prefetcher in ipc_plot_prefetchers:
            print(f"> {prefetcher}: {ipc_speedups[prefetcher].get('geomean', 0.0)}")
    print("--------------------------------")

    print("Computing DRAM traffic...")
    dram_speedups, dram_plot_prefetchers = compute_geomean_speedups(dram_data, 'dram', BASELINE)
    # print("DRAM speedups:", dict(dram_speedups))
    print("Geomean DRAM traffic:")
    for prefetcher in dram_plot_prefetchers:
        print(f"> {prefetcher}: {dram_speedups[prefetcher].get('geomean', 0.0)}")
    print("--------------------------------")
    
    print("Computing coverage...")
    cov_speedups, cov_plot_prefetchers = compute_geomean_speedups(cov_data, 'coverage', BASELINE)
    # print("Coverage values:", dict(cov_speedups))
    print("Geomean coverage:")
    for prefetcher in cov_plot_prefetchers:
        print(f"> {prefetcher}: {cov_speedups[prefetcher].get('geomean', 0.0)}")
    print("--------------------------------")
    
    print("Computing accuracy...")
    acc_speedups, acc_plot_prefetchers = compute_geomean_speedups(acc_data, 'accuracy', BASELINE)
    # print("Accuracy values:", dict(acc_speedups))
    print("Geomean accuracy:")
    for prefetcher in acc_plot_prefetchers:
        print(f"> {prefetcher}: {acc_speedups[prefetcher].get('geomean', 0.0)}")
    print("--------------------------------")

    # Create all plots
    print("Creating IPC plot...")
    if PLOT_TRACE_IPC:
        create_plot(ipc_speedups, ipc_plot_prefetchers, 'ipc', 'IPC Speedup', 
                    f'{PLOT_NAME}_ipc_traces.{OUTPUT}', include_geomean=False, include_baseline=True, 
                    ylim_bottom=0.9, legend_position='top', plot_traces=True, trace_labels=ipc_trace_labels)
    else:
        create_plot(ipc_speedups, ipc_plot_prefetchers, 'ipc', 'IPC Speedup', 
                    f'{PLOT_NAME}_ipc.{OUTPUT}', include_geomean=INCLUDE_GEOMEAN, include_baseline=True, 
                    ylim_bottom=0.9, only_geomean_bar=ONLY_GEOMEAN_BAR, legend_position='top')
    
    print("Creating DRAM plot...")
    create_plot(dram_speedups, dram_plot_prefetchers, 'dram', 'Normalized DRAM Traffic', 
                f'{PLOT_NAME}_dram.{OUTPUT}', include_geomean=INCLUDE_GEOMEAN, include_baseline=True, ylim_bottom=0.9, only_geomean_bar=ONLY_GEOMEAN_BAR, only_geomean_line=ONLY_GEOMEAN_LINE, legend_position='top')
    
    print("Creating coverage plot...")
    create_plot(cov_speedups, cov_plot_prefetchers, 'coverage', 'Coverage', 
                f'{PLOT_NAME}_coverage.{OUTPUT}', include_geomean=INCLUDE_GEOMEAN, include_baseline=False, 
                ylim_bottom=0.0, ylim_top=1.0, only_geomean_bar=ONLY_GEOMEAN_BAR, legend_position='top')
    
    print("Creating accuracy plot...")
    create_plot(acc_speedups, acc_plot_prefetchers, 'accuracy', 'Accuracy', 
                f'{PLOT_NAME}_accuracy.{OUTPUT}', include_geomean=INCLUDE_GEOMEAN, include_baseline=False, 
                ylim_bottom=0.0, ylim_top=1.0, only_geomean_bar=ONLY_GEOMEAN_BAR, legend_position='top')
    
    print("All plots created successfully!")

if __name__ == "__main__":
    main() 