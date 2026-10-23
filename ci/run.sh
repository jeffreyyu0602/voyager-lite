#!/bin/bash
# Runs one CI test row: ci/run.sh <config> <sims> <networks>
#
# <config> names a hardware config, ci/configs/<config>.env. <networks> is a
# comma-separated list for run_regression.py --models; a network compiled
# with its own settings has them in ci/networks/<network>.env, which
# run_regression.py applies to that network only. Run from the repository
# root with the CAD tools loaded.
set -euo pipefail

config=$1
sims=$2
networks=$3

set -a
source "$(dirname "$0")/configs/$config.env"
set +a

python run_regression.py --models "$networks" --sims "$sims" \
    --num_processes 16 --uniquify_layers --skip_layers
