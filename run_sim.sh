#!/bin/bash

if [ "$#" -lt 3 ]; then
    echo "Illegal number of parameters"
    echo "Usage: ./run_champsim.sh [N_WARM] [N_SIM] [TRACE] [OPTION]"
    exit 1
fi

TRACE_DIR="/home/jalexandrou/Documents/Imperial/4Yr/ChampSim/SPEC_quick"
BINARY=$PWD/bin/champsim
N_WARM=${1}
N_SIM=${2}
TRACE=${3}
OPTION=${4}

# Sanity check
if [ -z $TRACE_DIR ] || [ ! -d "$TRACE_DIR" ] ; then
   echo "[ERROR] Cannot find a trace directory: $TRACE_DIR"
   exit 1
fi

if [ ! -f "$BINARY" ] ; then
   echo "[ERROR] Cannot find a ChampSim binary: $BINARY"
   exit 1
fi

re='^[0-9]+$'
if ! [[ $N_WARM =~ $re ]] || [ -z $N_WARM ] ; then
    echo "[ERROR]: Number of warmup instructions is NOT a number" >&2;
    exit 1
fi

re='^[0-9]+$'
if ! [[ $N_SIM =~ $re ]] || [ -z $N_SIM ] ; then
    echo "[ERROR]: Number of simulation instructions is NOT a number" >&2;
    exit 1
fi

if [ ! -f "$TRACE_DIR/$TRACE" ] ; then
   echo "[ERROR] Cannot find a trace file: $TRACE_DIR/$TRACE"
   exit 1
fi
echo "Configuring..."
$PWD/config.sh test_config.json

echo "Building Champsim"
make

echo "----------------------"
echo "Starting run of $TRACE"
echo "----------------------"
mkdir -p $PWD/results/BOP/${N_SIM}M
(${BINARY} --warmup-instructions ${N_WARM}000000 --simulation-instructions ${N_SIM}000000 ${OPTION} ${TRACE_DIR}/${TRACE}) > $PWD/results/BOP/${N_SIM}M/${TRACE}.txt
