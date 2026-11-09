"""Helpers shared by the regression drivers.

Two drivers import this module and stay separate on purpose:
  - run_accelerator.py (repo root): the standalone accelerator flows
    (gold model, SystemC, Catapult scverify RTL, accuracy).
  - soc/run_soc.py: the SoC RTL flow (Chipyard simv, firmware on the
    Rocket core, DMA replay testbench).

Only genuinely flow-independent pieces live here: build-folder naming, the
network compile and its settings, the layers.txt parsing and skip rules, the
result table/utilization report, and the log-scrape patterns both flows grade
with.
"""

import datetime
import json
import os
import re
import shlex
import signal
import subprocess

import numpy as np
import pandas as pd

# Every flow's checker prints this exact line on a clean comparison.
PASS_PATTERN = r"Error\s+count:\s+0"

# Printed by the gold walk (Simulation.cc); both result parsers scrape it.
IDEAL_RUNTIME_PATTERN = (
    r"(matrix|vector)\s+unit\s+ideal\s+runtime:\s+(\d+)\s*cycles"
)


# The parameters a run falls back on when the environment leaves them unset,
# the drivers' one copy of the defaults the Makefile (buffers, features) and
# codegen.mk (scratchpad) assign with ``?=``.
BUFFER_DEFAULTS = {
    "INPUT_BUFFER_SIZE": "1024",
    "WEIGHT_BUFFER_SIZE": "1024",
    "ACCUM_BUFFER_SIZE": "1024",
    "DOUBLE_BUFFERED_ACCUM_BUFFER": "false",
    "SUPPORT_MVM": "false",
    "SUPPORT_SPMM": "false",
}
SCRATCHPAD_DEFAULTS = {
    "SCRATCHPAD_SIZE": "2097152",
    "SCRATCHPAD_OFFSET": "0",
    "NUM_BANKS": "16",
}
# Where the networks compile, relative to the repository (the Makefile's).
DEFAULT_CODEGEN_DIR = "build/codegen"
# A results folder's name; tools sort the folders by it.
RESULTS_FOLDER_FORMAT = "%Y_%m_%d_%H_%M_%S"


def set_default_env_vars(env_vars):
    """Fill the buffer and feature parameters ``env_vars`` leaves unset."""
    for name, value in BUFFER_DEFAULTS.items():
        env_vars.setdefault(name, value)


def build_params(env_vars):
    """The parameter set's name, e.g. ``64x64_512x576x256_true_true_true``:
    the array, the buffer sizes and the feature flags."""
    return (
        f"{env_vars['IC_DIMENSION']}x{env_vars['OC_DIMENSION']}_"
        f"{env_vars['INPUT_BUFFER_SIZE']}x{env_vars['WEIGHT_BUFFER_SIZE']}"
        f"x{env_vars['ACCUM_BUFFER_SIZE']}_"
        f"{env_vars['DOUBLE_BUFFERED_ACCUM_BUFFER']}_"
        f"{env_vars['SUPPORT_MVM']}_{env_vars['SUPPORT_SPMM']}"
    )


def get_build_folder(env_vars):
    """The build folder the Makefile uses: ``BUILD_DIR`` when set, else
    ``build/<DATATYPE>_<build_params>``, relative to the repository."""
    return env_vars.get("BUILD_DIR") or (
        f"build/{env_vars['DATATYPE']}_{build_params(env_vars)}"
    )


def get_catapult_folder(env_vars):
    """The Catapult build folder the Makefile uses: ``CATAPULT_BUILD_DIR``
    when set, else ``<build folder>/Catapult/<TECHNOLOGY>/clock_<CLOCK_PERIOD>``
    (``get_build_folder``)."""
    return env_vars.get("CATAPULT_BUILD_DIR") or (
        f"{get_build_folder(env_vars)}/Catapult/{env_vars['TECHNOLOGY']}"
        f"/clock_{env_vars['CLOCK_PERIOD']}"
    )


def run_logged(cmd, log_path, env, cwd=None):
    """Run ``cmd`` with its output in ``log_path``.

    Raises:
        RuntimeError: The command failed; the message names the log.
    """
    with open(log_path, "w") as log_file:
        result = subprocess.run(
            cmd, cwd=cwd, env=env, stdout=log_file, stderr=subprocess.STDOUT
        )
    if result.returncode != 0:
        raise RuntimeError(f"{shlex.join(cmd)} failed: see {log_path}")


def add_common_args(parser, num_processes=None):
    """Add the arguments both drivers take.

    Args:
        parser: The driver's ``argparse.ArgumentParser``.
        num_processes: ``--num_processes``'s default; None makes it
            required.
    """
    parser.add_argument(
        "--models",
        required=True,
        type=lambda text: [model.strip() for model in text.split(",")],
        help="Comma-separated networks to test (e.g. resnet18,llama_decode)",
    )
    parser.add_argument(
        "--tests",
        default=None,
        help="Comma separated list of tests to run (e.g. test1,test2)",
    )
    parser.add_argument(
        "--num_processes",
        type=int,
        default=num_processes,
        required=num_processes is None,
        help="Number of processes to run in parallel",
    )
    parser.add_argument(
        "--uniquify_layers",
        action="store_true",
        help="Run one layer of each group of structurally identical layers",
    )
    parser.add_argument(
        "--skip_layers",
        action="store_true",
        help="Skip the layers ci/skip_rules.json names",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Dump the RTL simulation's waveform",
    )


def make_results_folder(root="regression_results"):
    """Create ``<root>/<time>`` (``RESULTS_FOLDER_FORMAT``) and point
    ``<root>/latest`` at it.

    Returns:
        The new folder's path.
    """
    name = datetime.datetime.now().strftime(RESULTS_FOLDER_FORMAT)
    folder = os.path.join(root, name)
    os.makedirs(folder)
    latest = os.path.join(root, "latest")
    if os.path.lexists(latest):
        os.remove(latest)
    os.symlink(name, latest)
    return folder


def load_skip_rules(repo_dir, enabled):
    """The rules of ``<repo_dir>/ci/skip_rules.json``, or none when
    ``enabled`` is false."""
    if not enabled:
        return []
    with open(os.path.join(repo_dir, "ci", "skip_rules.json")) as f:
        return json.load(f)


def actual_tile_count(num_tiles):
    """Tiles a run actually executes: MAX_TILES bounds the tile loop; unset or
    0 means the whole loop runs (matching the interpreter, the C emitter, and
    the testbench)."""
    max_tiles = int(os.environ.get("MAX_TILES", "0"))
    return min(num_tiles, max_tiles) if max_tiles > 0 else num_tiles


def utilization(df):
    """Weighted ideal/actual ratio over the layer table. Callers must supply
    'Ideal' and 'Runtime' in the SAME unit (see the drivers' parsers)."""
    count = df["Count"].to_numpy()
    full_tiles = df["L2 Tiles"].to_numpy()
    actual_tiles = df["Actual Tiles"].to_numpy()
    ideal = df["Ideal"].to_numpy()
    runtime = df["Runtime"].to_numpy()

    # Precompute common factor
    weight = (full_tiles * count) / actual_tiles

    numerator = np.sum(ideal * weight)
    denominator = np.sum(runtime * weight)

    return numerator / denominator if denominator != 0 else np.nan


def print_test_results(test_results, layers, output_folder):
    columns = [
        "Model",
        "Layer",
        "Status",
        "Runtime",
        "Ideal",
        "RuntimeType",
        "Count",
        "L2 Tiles",
        "Actual Tiles",
    ]
    if len(test_results[0]) == 3:
        columns = columns[:3]

    # convert list of tuples to DataFrame
    df = pd.DataFrame(test_results, columns=columns)
    sorted_df = []

    # get models
    models = df["Model"].unique()

    for model in models:
        print("=" * 10 + f" {model} " + "=" * 10)

        # Create an explicit copy of the DataFrame
        model_df = df[df["Model"] == model].copy()

        # sort according to order in layers
        model_df["Layer"] = pd.Categorical(model_df["Layer"], layers[model])
        model_df.sort_values("Layer", inplace=True)
        # turn categorial back to string
        model_df["Layer"] = model_df["Layer"].astype(str)
        sorted_df.append(model_df)

        passed = model_df[model_df["Status"] == True]
        failed = model_df[model_df["Status"] == False]

        print("Passed:")
        print(passed["Layer"].to_string(index=False) if not passed.empty else "None")
        print("Failed:")
        print(failed["Layer"].to_string(index=False) if not failed.empty else "None")

        # if runtime column exists, print runtime of each layer
        if "Runtime" in model_df.columns:
            print("Runtime:")
            print(model_df[columns[1:]].to_string(index=False), flush=True)

            utilization_all    = utilization(model_df)
            utilization_matrix = utilization(model_df[model_df["RuntimeType"] == "matrix"])

            print(f"Utilization: {utilization_all:.3f}")
            print(f"Matrix Utilization: {utilization_matrix:.3f}")

    # concatentate all DataFrames into a single DataFrame and save to pickle and excel
    combined_df = pd.concat(sorted_df)
    combined_df.to_pickle(f"{output_folder}/test_results.pkl")
    combined_df.to_excel(f"{output_folder}/test_results.xlsx", index=False)

    # return True if all tests passed
    return len(df[df["Status"] == False]) == 0


def check_environment_vars(required_vars):
    unset_vars = [var for var in required_vars if var not in os.environ]
    if len(unset_vars) > 0:
        raise ValueError(f"Please set {', '.join(unset_vars)} environment variables")


def run_with_timeout(cmd, env, stdout_file, timeout, cwd=None):
    """Run `cmd`, and on timeout kill every process it started.

    `subprocess.run(timeout=...)` kills only the direct child. These tests
    launch the simulator through `make`, so killing `make` leaves the
    simulator running -- orphaned, holding a core, and invisible to the
    sweep, which has already moved on. Giving the child its own session puts
    the whole tree in one process group the timeout can signal.

    Returns True if the command finished, False if it timed out.
    """
    with subprocess.Popen(
        cmd,
        cwd=cwd,
        env=env,
        stdout=stdout_file,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    ) as process:
        try:
            process.communicate(timeout=timeout)
            return True
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                process.kill()
            process.communicate()
            return False


def network_settings(network, repo_dir):
    """``network``'s settings as make's ``VAR=value`` arguments, for every
    make call about the network: ``NETWORK`` and each ``KEY=VALUE`` line of
    ``<repo_dir>/ci/networks/<network>.env``, the file a network compiled
    with settings of its own keeps them in (``codegen.mk`` checks them
    against the compiled folder on every call)."""
    settings = [f"NETWORK={network}"]
    path = os.path.join(repo_dir, "ci", "networks", f"{network}.env")
    if os.path.exists(path):
        with open(path) as f:
            settings += [line.strip() for line in f if line.strip()]
    return settings


def compile_network(network, *, repo_dir, env):
    """Compile ``network``: ``make network-proto`` in ``repo_dir`` with the
    network's settings on the command line (``network_settings``), the
    command printed first.

    Args:
        network: The network's name.
        repo_dir: The voyager repository, whose ``codegen.mk`` compiles.
        env: The environment make runs in: the hardware config
            (``DATATYPE``, ``IC_DIMENSION``, ``OC_DIMENSION``, ...).

    Raises:
        subprocess.CalledProcessError: The compile failed.
    """
    cmd = [
        "make",
        "-C",
        repo_dir,
        "network-proto",
        *network_settings(network, repo_dir),
    ]
    print(shlex.join(cmd), flush=True)
    subprocess.run(cmd, env=env, check=True)


def network_layers(
    network,
    layers,
    layer_counts,
    tile_counts,
    *,
    env,
    sim_type,
    codegen_root,
    skip_rules,
    uniquify,
):
    """Add a compiled ``network``'s layers to the test lists, less those
    ``skip_rules`` drop for ``sim_type`` and the ``env``'s datatype and
    block size.

    ``layers.txt`` has one line per bufferized layer: its name, the first
    and last emitted op of its extent, its equivalence group from the
    compiler's bufferization cache, and its tile loop's trip count.
    ``uniquify`` keeps the first layer of each group and counts the rest
    into it.

    Args:
        network: The network's name.
        layers: The test list by network, filled in place, as are
            ``layer_counts`` and ``tile_counts``.
        layer_counts: Per network, how many layers each test stands for.
        tile_counts: Per network, each test's tile-loop trip count.
        env: The run's environment, for ``DATATYPE`` and the array size.
        sim_type: The flow the skip rules are matched against.
        codegen_root: The folder holding ``networks/``.
        skip_rules: The rules of ``ci/skip_rules.json``.
        uniquify: Run one layer of each structurally identical group.
    """
    datatype = env["DATATYPE"]
    block_size = max(int(env["OC_DIMENSION"]), int(env["IC_DIMENSION"]))
    skip_layers = [
        re.compile(pattern)
        for pattern in get_skip_layers(
            skip_rules, network, datatype, sim_type, block_size
        )
    ]
    layers[network] = []
    layer_counts[network] = {}
    tile_counts[network] = {}
    seen_groups = {}
    path = f"{codegen_root}/networks/{network}/{datatype}/layers.txt"
    with open(path) as f:
        for line in f:
            fields = line.split()
            if not fields:
                continue
            name, _start_op, _end_op, group, tiles = fields
            if any(p.fullmatch(name) for p in skip_layers):
                print(f"Skipping layer {name}")
                continue
            if uniquify and group in seen_groups:
                layer_counts[network][seen_groups[group]] += 1
                continue
            seen_groups[group] = name
            layers[network].append(name)
            layer_counts[network][name] = 1
            tile_counts[network][name] = int(tiles)


def matches(value, rule_value):
    if isinstance(rule_value, list):
        return value in rule_value
    else:
        if rule_value == "*":
            return True
        else:
            return value == rule_value


def get_skip_layers(skip_rules, model, datatype, sim_type, block_size):
    best_rule = None
    best_specificity = -1

    for rule in skip_rules:
        if (
            matches(model, rule["model"])
            and matches(datatype, rule["datatype"])
            and matches(sim_type, rule["sim_type"])
            and matches(block_size, rule["block_size"])
        ):
            # Calculate specificity score (higher is more specific)
            # Exact match = 2, list match = 1, wildcard = 0
            specificity = 0
            specificity += 2 if rule["datatype"] != "*" else 0
            specificity += 1 if isinstance(rule["model"], list) else (2 if rule["model"] != "*" else 0)
            specificity += 1 if isinstance(rule["sim_type"], list) else (2 if rule["sim_type"] != "*" else 0)
            specificity += 2 if rule["block_size"] != "*" else 0

            if specificity > best_specificity:
                best_specificity = specificity
                best_rule = rule

    return set(best_rule["layers"]) if best_rule else set()
