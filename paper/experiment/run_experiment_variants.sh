#!/usr/bin/env zsh
set -euo pipefail

# Run both experiment variants from tracked paper sources.
# This script synchronizes source files into scratch/, performs a clean
# reconfigure/rebuild, and then invokes ns-3 runs.

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT_DIR"

CATS_SRC="paper/experiment/experiments/experiment-core-prioritization.cc"
BASELINE_SRC="paper/experiment/experiments/experiment-baseline-tcp.cc"

CATS_SCRATCH="scratch/experiment-core-prioritization.cc"
BASELINE_SCRATCH="scratch/experiment-baseline-tcp.cc"

CATS_OUT="paper/experiment/results/cats"
BASELINE_OUT="paper/experiment/results/baseline"
ENABLE_PCAP_VALUE="${ENABLE_PCAP:-0}"
PCAP_ARG=""

if [[ "$ENABLE_PCAP_VALUE" == "1" || "$ENABLE_PCAP_VALUE" == "true" || "$ENABLE_PCAP_VALUE" == "TRUE" ]]; then
	PCAP_ARG=" --enablePcap=true"
fi

mkdir -p "$CATS_OUT" "$BASELINE_OUT"

cp -f "$CATS_SRC" "$CATS_SCRATCH"
cp -f "$BASELINE_SRC" "$BASELINE_SCRATCH"

# Clean and rebuild as requested for reliable runs.
cmake --build cmake-cache --target clean || true
cmake -S . -B cmake-cache -DCMAKE_BUILD_TYPE=debug -DNS3_ASSERT=ON -DNS3_LOG=ON -DNS3_WARNINGS_AS_ERRORS=ON -DNS3_NATIVE_OPTIMIZATIONS=OFF -DNS3_EXAMPLES=ON -DNS3_TESTS=ON -G "Unix Makefiles"
./ns3 build -j6

./ns3 run "experiment-core-prioritization --outputDir=$CATS_OUT$PCAP_ARG"
./ns3 run "experiment-baseline-tcp --outputDir=$BASELINE_OUT$PCAP_ARG"

echo "Experiment runs complete."
echo "  CATS results: $CATS_OUT/priority_completion.tsv"
echo "  Baseline results: $BASELINE_OUT/priority_completion.tsv"
if [[ -n "$PCAP_ARG" ]]; then
	echo "  PCAPs enabled: yes"
	echo "  CATS PCAP prefix: $CATS_OUT/experiment-core-prio-*.pcap"
	echo "  Baseline PCAP prefix: $BASELINE_OUT/experiment-baseline-tcp-*.pcap"
else
	echo "  PCAPs enabled: no (set ENABLE_PCAP=1 to enable)"
fi
