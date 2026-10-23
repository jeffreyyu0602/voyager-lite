#!/bin/bash
# Runs one CI test row: ci/run.sh <config> <sims> <networks>
#
# <config> names a hardware config, ci/configs/<config>.env. <networks> is a
# comma-separated list for run_regression.py --models. A network compiled
# with its own settings (ci/networks/<network>.env, see codegen.mk) must be
# alone in its row. Run from the repository root with the CAD tools loaded.
set -euo pipefail

config=$1
sims=$2
networks=$3
ci_dir=$(dirname "$0")

for network in ${networks//,/ }; do
    if [ "$network" != "$networks" ] && [ -f "$ci_dir/networks/$network.env" ]; then
        echo "$network has its own compile settings: give it its own row" >&2
        exit 1
    fi
done

set -a
source "$ci_dir/configs/$config.env"
if [ -f "$ci_dir/networks/$networks.env" ]; then
    source "$ci_dir/networks/$networks.env"
fi
set +a

python run_regression.py --models "$networks" --sims "$sims" \
    --num_processes 16 --uniquify_layers --skip_layers
