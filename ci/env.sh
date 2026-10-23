# Environment for a CI job, sourced by every step that builds or runs
# anything: the CAD tools and this run's conda env, which the setup job
# creates under $CI_ENV_ROOT (set by the runner, see .github/RUNNER.md).

if [ -f /etc/profile.d/modules.sh ]; then
    source /etc/profile.d/modules.sh
elif [ -f /etc/profile.d/lmod.sh ]; then
    source /etc/profile.d/lmod.sh
fi

eval `modulecmd bash load base catapult/2024.2_2 vcs/T-2022.06-SP2 verdi/S-2021.09-SP1 dc_shell/U-2022.12-SP1`
export VG_GNU_PACKAGE=/cad/synopsys/vcs_gnu_package/S-2021.09/gnu_9/linux
source $VG_GNU_PACKAGE/source_me.sh
export LD_LIBRARY_PATH=/cad/mentor/2024.2_2/Mgc_home/lib:/cad/mentor/2024.2_2/Mgc_home/shared/lib:$LD_LIBRARY_PATH

export PATH=$HOME/miniforge3/condabin:$PATH
source $(conda info --base)/etc/profile.d/conda.sh
conda activate ${CI_ENV_ROOT:?}/ci-env-${GITHUB_RUN_ID:?}

export PROJECT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export CODEGEN_DIR=build/codegen
