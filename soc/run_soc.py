# Script to run Voyager simulations
import os
import argparse
import multiprocessing as mp
import subprocess
import re
import sys
import time

# This file lives at <voyager repo>/soc/run_soc.py; resolve through
# the symlink (sims/vcs/run_soc.py) so both invocation paths work.
VOYAGER_DIR = os.path.abspath(
    os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")
)
JTAG_DIR = os.path.join(VOYAGER_DIR, "soc", "jtag")

# Shared regression helpers live at the voyager repo root, next to
# run_accelerator.py (the standalone-accelerator driver).
sys.path.insert(0, VOYAGER_DIR)
from common import (
    DEFAULT_CODEGEN_DIR,
    IDEAL_RUNTIME_PATTERN,
    PASS_PATTERN,
    SCRATCHPAD_DEFAULTS,
    actual_tile_count,
    add_common_args,
    build_params,
    compile_network,
    get_build_folder,
    load_skip_rules,
    make_results_folder,
    network_layers,
    network_settings,
    print_test_results,
)

# Element width in bits per DATATYPE, which sets the SoC SRAM row width.
DATATYPE_WIDTH = {"E4M3": 8, "INT8": 8, "MXINT8": 8, "MXNF4": 4}


def _wait_for_pattern_in_file(path, pattern, timeout=1800):
    """Poll a file until a regex pattern is found, then return the match object."""
    compiled = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r") as f:
                for line in f:
                    m = compiled.search(line)
                    if m:
                        return m
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    raise TimeoutError(f"Pattern '{pattern}' not found in {path} within {timeout}s")


def _terminate_process(proc: subprocess.Popen, timeout: float = 5) -> int:
    if proc.poll() is None:
        proc.terminate()

    try:
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        return proc.wait()


def elf_symbol_address(elf, symbol):
    """The address of `symbol` in `elf`, from the RISC-V nm."""
    listing = subprocess.run(
        ["riscv64-unknown-elf-nm", elf], capture_output=True, text=True, check=True
    ).stdout
    for entry in listing.splitlines():
        fields = entry.split()
        if len(fields) == 3 and fields[2] == symbol:
            return int(fields[0], 16)
    raise RuntimeError(f"{elf}: no symbol {symbol}")


def run_rtl_simulation(
    model,
    layer,
    layer_count,
    tile_count,
    output_folder,
    env_vars,
    config_path,
    debug,
    jtag_sim=None,
):
    riscv_binary = (
        f"../../tests/voyager/networks/{model}/{config_path}/{layer}.riscv"
    )
    # What this run sets on top of the shared environment; simv reads it
    # from its environment, and the log records it first.
    overrides = {
        "NETWORK": model,
        "TESTS": layer,
        "RISCV_BINARY": riscv_binary,
    }

    cmd = [
        (
            f"./simv-chipyard.harness-{env_vars['CONFIG']}" if not debug
            else f"./simv-chipyard.harness-{env_vars['CONFIG']}-debug"
        ),
        "+permissive",
        "+dramsim",
        "+dramsim_ini_dir=../../generators/testchipip/src/main/resources/dramsim2_ini",
        "+ntb_random_seed_automatic",
    ]

    # +verbose adds the core's commit trace to the .err file: gigabytes per
    # JTAG run (the core spends hours in the debug module's program buffer)
    # and nothing the driver reads. Opt in with SIM_VERBOSE=1.
    if env_vars.get("SIM_VERBOSE"):
        cmd.append("+verbose")

    # Extra runtime args, e.g. EXTRA_SIM_FLAGS="+vcs+initreg+0" to zero-init
    # all state (mirrors the SystemC harness) instead of the default random.
    if env_vars.get("EXTRA_SIM_FLAGS"):
        cmd.extend(env_vars["EXTRA_SIM_FLAGS"].split())

    if debug:
        cmd.append("+fsdbfile=dump.fsdb")

    vcs_err = f"{output_folder}/{model}_{layer}.err"
    vcs_log = f"{output_folder}/{model}_{layer}.log"
    openocd_log = f"{output_folder}/{model}_{layer}_openocd.log"
    gdb_log = f"{output_folder}/{model}_{layer}_gdb.log"
    scratchpad_bin = f"{output_folder}/{model}_{layer}_scratchpad.bin"
    checker_log = f"{output_folder}/{model}_{layer}_checker.log"

    # Full JTAG mode: the emitter lists the scratchpad regions the grade
    # needs; GDB reads back only those and the checker loads them.
    if jtag_sim == "full":
        overrides["SCRATCHPAD_DUMP_LIST"] = (
            f"{VOYAGER_DIR}/soc/firmware/networks/{model}/{config_path}"
            f"/{layer}_scratchpad_dump.txt"
        )
    else:
        # The testbench services the firmware's requests out of a mailbox in
        # the firmware's own memory; its address is wherever the linker put
        # it in this layer's ELF.
        overrides["HOST_MAILBOX"] = hex(
            elf_symbol_address(riscv_binary, "host_mailbox")
        )
    env_vars = {**env_vars, **overrides}
    run_header = "".join(
        f"# {name}={value}\n" for name, value in overrides.items()
    )

    if jtag_sim is None:
        cmd.extend([
            # 10M cycles is 9 ms at CLOCK_PERIOD=0.9, which a large MAX_TILES
            # can outrun -- a layer that hits the cap reports no error count at
            # all, which reads like a failure rather than a truncated run.
            "+max-cycles=" + env_vars.get("MAX_CYCLES", "10000000"),
            "+permissive-off",
            riscv_binary,
        ])

        with open(vcs_log, "w") as log_file, open(vcs_err, "w") as err_file:
            log_file.write(run_header)
            log_file.flush()
            subprocess.run(
                cmd,
                env=env_vars,
                stdout=log_file,
                stderr=err_file,
                timeout=48 * 60 * 60,
            )
    else:
        cmd.extend([
            "+max-cycles=0",
            "+jtag_rbb_enable=1",
            "--rbb-port=9823",  # This is ignored by the simulator
            "+permissive-off",
            "none"
        ])

        with open(vcs_log, "w") as log_file, open(vcs_err, "w") as err_file:
            log_file.write(run_header)
            log_file.flush()
            vcs_proc = subprocess.Popen(
                cmd, env=env_vars, stdout=log_file, stderr=err_file
            )

        try:
            # Step 1: Wait for VCS to announce its JTAG bitbang port
            m = _wait_for_pattern_in_file(vcs_err, r"Listening on port (\d+)")
            vcs_jtag_port = m.group(1)
            print(f"[JTAG] VCS listening on port {vcs_jtag_port}")

            # Step 2: Launch OpenOCD with VCS_JTAG_PORT in env
            openocd_env = {**env_vars, "VCS_JTAG_PORT": vcs_jtag_port}
            with open(openocd_log, "w") as ocd_log:
                openocd_proc = subprocess.Popen(
                    ["openocd", "-f", os.path.join(JTAG_DIR, "cemulator.cfg")],
                    env=openocd_env,
                    stdout=ocd_log,
                    stderr=subprocess.STDOUT,
                )

            try:
                # Step 3: Wait for OpenOCD to announce its GDB port
                m = _wait_for_pattern_in_file(
                    openocd_log,
                    r"Listening on port (\d+) for gdb connections",
                    timeout=4 * 60 * 60,
                )
                gdb_port = m.group(1)
                print(f"[JTAG] OpenOCD GDB server on port {gdb_port}")

                # Step 4: Run GDB (drives the simulation to completion)
                gdb_env = {
                    **env_vars,
                    "OPENOCD_GDB_PORT": gdb_port,
                    "SCRATCHPAD_DUMP_PATH": scratchpad_bin
                }
                with open(gdb_log, "w") as gdb_log_file:
                    subprocess.run(
                        [
                            "riscv64-unknown-elf-gdb",
                            "--batch",
                            riscv_binary,
                            "-x", os.path.join(JTAG_DIR, "run_jtag.gdb"),
                        ],
                        env=gdb_env,
                        stdout=gdb_log_file,
                        stderr=subprocess.STDOUT,
                    )
            except Exception as e:
                print(f"Error during GDB execution: {e}")
            finally:
                _terminate_process(openocd_proc)
        except Exception as e:
            print(f"Error starting OpenOCD initialization: {e}")
        finally:
            _terminate_process(vcs_proc)

        # Step 5: Run jtag_sim_checker on the scratchpad dump
        if jtag_sim == "full":
            checker_bin = os.path.join(
                VOYAGER_DIR, get_build_folder(env_vars), "cc", "jtag_sim_checker"
            )
            with open(checker_log, "w") as clf:
                subprocess.run(
                    [checker_bin, scratchpad_bin],
                    env=env_vars,
                    stdout=clf,
                    stderr=subprocess.STDOUT,
                )

    return grade_simulation(
        model,
        layer,
        layer_count,
        tile_count,
        output_folder,
        jtag_sim,
    )


def grade_simulation(
    model,
    layer,
    layer_count,
    tile_count,
    results_folder,
    jtag_sim=None,
):
    success = False
    runtime = 0
    ideal = 0
    runtime_type = ""

    vcs_log = f"{results_folder}/{model}_{layer}.log"
    gdb_log = f"{results_folder}/{model}_{layer}_gdb.log"
    checker_log = f"{results_folder}/{model}_{layer}_checker.log"
    sim_log = checker_log if jtag_sim == "full" else vcs_log

    if os.path.exists(sim_log):
        with open(sim_log, "r") as file:
            content = file.read()

        # Check if test passed
        success = bool(re.search(PASS_PATTERN, content))

        # Capture runtime type (Matrix or Vector) and ideal runtime. Both the
        # compiler's ideal and the firmware's measurement are in accelerator
        # cycles, so the utilization report needs no conversion.
        match = re.search(IDEAL_RUNTIME_PATTERN, content, flags=re.IGNORECASE)
        if match:
            runtime_type = match.group(1).lower()
            ideal = int(match.group(2))

        # Capture actual runtime
        runtime_source = content
        if jtag_sim is not None and os.path.exists(gdb_log):
            with open(gdb_log, "r") as gdb_file:
                runtime_source = gdb_file.read()

        runtime_match = re.search(
            r"Accelerator\s+Runtime\s*:\s*(\d+)\s*cycles",
            runtime_source,
            flags=re.IGNORECASE,
        )
        if runtime_match:
            runtime = int(runtime_match.group(1))

    return (
        model,
        layer,
        success,
        runtime,
        ideal,
        runtime_type,
        layer_count,
        tile_count,
        actual_tile_count(tile_count),
    )


def run_rtl_simulations(
    layers,
    layer_counts,
    tile_counts,
    num_processes,
    results_folder,
    env_vars,
    config_path,
    debug,
    jtag_sim=None,
):
    pool = mp.Pool(num_processes)

    test_results = []

    for model, tests in layers.items():
        for test in tests:
            pool.apply_async(
                run_rtl_simulation,
                args=(
                    model,
                    test,
                    layer_counts[model][test],
                    tile_counts[model][test],
                    results_folder,
                    env_vars,
                    config_path,
                    debug,
                    jtag_sim,
                ),
                callback=test_results.append,
            )
    pool.close()
    pool.join()

    return print_test_results(test_results, layers, results_folder)


def regrade_results_folder(
    layers,
    layer_counts,
    tile_counts,
    results_folder,
    jtag_sim=None,
):
    test_results = []

    for model, tests in layers.items():
        for test in tests:
            test_results.append(
                grade_simulation(
                    model,
                    test,
                    layer_counts[model][test],
                    tile_counts[model][test],
                    results_folder,
                    jtag_sim,
                )
            )

    return print_test_results(test_results, layers, results_folder)


def inject_power_pins(testharness_sv_path):
    INJECT_SENTINEL = "dut_VSS"
    CHIPTOP_PATTERN = "ChipTop chiptop0 ("
    CLOSING_PREFIX = "  );"

    with open(testharness_sv_path, "r") as f:
        lines = f.readlines()

    if any(INJECT_SENTINEL in line for line in lines):
        print(f"Power pins already injected in {testharness_sv_path}, skipping.")
        return

    chiptop_idx = next(
        (i for i, line in enumerate(lines) if CHIPTOP_PATTERN in line), None
    )
    assert chiptop_idx is not None, (
        f"Could not find ChipTop instantiation in {testharness_sv_path}"
    )

    closing_idx = next(
        (
            i
            for i in range(chiptop_idx + 1, len(lines))
            if lines[i].startswith(CLOSING_PREFIX)
        ),
        None,
    )
    assert closing_idx is not None, (
        f"Could not find closing '); for ChipTop in {testharness_sv_path}"
    )

    supply_block = [
        "  // BEGIN: POWER PIN INJECT\n",
        "`ifdef GL_PWR_PINS\n",
        "  supply0 dut_VSS;\n",
        "  supply1 dut_VDDPST;\n",
        "  supply1 dut_VDD;\n",
        "`endif // GL_PWR_PINS\n",
        "  // END: POWER PIN INJECT\n",
    ]

    port_block = [
        "    // BEGIN: POWER PIN INJECT\n",
        "`ifdef GL_PWR_PINS\n",
        "   ,.VSS       (dut_VSS)\n",
        "   ,.VDDPST    (dut_VDDPST)\n",
        "   ,.VDD       (dut_VDD)\n",
        "`endif // GL_PWR_PINS\n",
        "    // END: POWER PIN INJECT\n",
    ]

    # Insert port_block first (higher index) to keep chiptop_idx valid
    new_lines = lines[:closing_idx] + port_block + lines[closing_idx:]
    new_lines = new_lines[:chiptop_idx] + supply_block + new_lines[chiptop_idx:]

    bak_path = testharness_sv_path + ".bak"
    with open(bak_path, "w") as f:
        f.writelines(lines)

    with open(testharness_sv_path, "w") as f:
        f.writelines(new_lines)

    print(f"Power pins injected into {testharness_sv_path}.")


def base_environment(args):
    """The run's environment before the SoC config is elaborated: the
    config, the gold-vs-DUT grade, the compile folders, the scratchpad
    geometry and the JTAG mode's settings."""
    env_vars = os.environ.copy()
    env_vars["CONFIG"] = args.config
    # The SoC flow always grades the DUT against the gold walk; force it so a
    # stale exported SIMS cannot leak into the simv process.
    env_vars["SIMS"] = "gold,accelerator"
    env_vars["PROJECT_ROOT"] = VOYAGER_DIR
    env_vars["CODEGEN_DIR"] = DEFAULT_CODEGEN_DIR

    # SCRATCHPAD_SIZE is the whole physical L2 SRAM the compiler plans into
    # (software-pipelined buffers get slots of it); it must equal the SoC
    # scratchpad the Chipyard config instantiates.
    for name, value in SCRATCHPAD_DEFAULTS.items():
        env_vars.setdefault(name, value)
    scratchpad_size = int(env_vars["SCRATCHPAD_SIZE"])
    scratchpad_offset = int(env_vars["SCRATCHPAD_OFFSET"])

    # The SoC targets require the IC=3 first conv lowered via im2col; the
    # channel-replication conv path is not supported here (see
    # README-soc-bringup.md). Overridable by exporting CONV2D_IM2COL=0.
    env_vars.setdefault("CONV2D_IM2COL", "1")

    # Shield load_memory and check_outputs functions from full JTAG mode
    # since we use GDB for data loading and output checking there
    if args.jtag_mode == "full":
        env_vars["JTAG_SIM"] = "1"
        # The image holds two ping-pong tiles and nothing restages the slots
        # afterwards, so the firmware may not run further than that.
        max_tiles = min(int(env_vars.get("MAX_TILES", "2")), 2)
        env_vars["MAX_TILES"] = str(max_tiles)
        print(f"[JTAG] full mode: MAX_TILES={max_tiles}")
        # run_jtag.gdb dumps [SRAM_BASE + SOC_MEM_OFFSET, + 2 * CACHE_SIZE)
        # and jtag_sim_checker reads it back at the same offset: everything
        # above the program's reservation, as two halves.
        env_vars.setdefault("SOC_MEM_OFFSET", str(scratchpad_offset))
        env_vars.setdefault(
            "CACHE_SIZE", str((scratchpad_size - scratchpad_offset) // 2)
        )
    if args.jtag_mode == "vpi":
        env_vars["SKIP_GDB_LOAD"] = "1"
    if args.jtag_mode is not None and args.semihosting:
        env_vars["SEMIHOSTING"] = "1"

    return env_vars


def generate_verilog(env_vars, results_folder):
    """Elaborate the SoC config (``make verilog``), which also writes its
    ``config.sh``."""
    # Generate SoC verilog
    print("Generating verilog...")
    with open(f"{results_folder}/verilog_generation.log", "w") as log_file:
        subprocess.run(
            ["make", "verilog"],
            env=env_vars,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            check=True,
        )


def add_config_environment(args, env_vars):
    """Add the SoC config's parameters (its ``config.sh``) to
    ``env_vars``, with the simulator's defines and the SRAM row width they
    imply."""
    # Set the environment variables from the sh file
    sim_cxxflags = []
    with open(f"{VOYAGER_DIR}/build/configs/{args.config}/config.sh", "r") as f:
        for line in f:
            if (match := re.match(r"export\s+(\w+)=(.*)", line.strip())) is None:
                continue
            var, val = match.groups()
            val = val.strip("'\"")
            env_vars[var] = val
            sim_cxxflags.append(f"-D{val}" if var == "DATATYPE" else f"-D{var}={val}")
    if args.gl_netlist is not None:
        sim_cxxflags.append("-DGL_SIM")
    # The DPI-compiled gold model sizes its clock-dependent accumulator
    # depths (Pooling.h ACCUMULATOR_SUM_N, SpMM.h FEEDBACK_DELAY) from the
    # CLOCK_PERIOD macro, defaulting to 5.0 when it is absent; the RTL is
    # scheduled at the run's clock, so the two must agree.
    if (clock_period := env_vars.get("CLOCK_PERIOD")) is not None:
        sim_cxxflags.append(f"-DCLOCK_PERIOD={clock_period}")
    env_vars.setdefault("EXTRA_SIM_CXXFLAGS", " ".join(sim_cxxflags))

    # We need to pass these preprocessor defines to the simulator during compilation
    preproc_defines = []
    if env_vars.get("SUPPORT_MVM") == "true":
        preproc_defines.append("+define+SUPPORT_MVM")
    if env_vars.get("SUPPORT_SPMM") == "true":
        preproc_defines.append("+define+SUPPORT_SPMM")
    # Overwrite the default Chipyard Verilog module definitions with the GL netlist if provided
    if args.gl_netlist is not None:
        preproc_defines.append("+define+GL_SIM")
        # TODO: replace with technology-specific preprocessor defines
        preproc_defines.append("+define+TSMC_PWR_AWARE +define+TSMC_CM_UNIT_DELAY")
        preproc_defines.append(f"-f {args.gl_netlist}")
    if args.gl_pwr_pins:
        preproc_defines.append("+define+GL_PWR_PINS")
    env_vars["EXTRA_SIM_SOURCES"] = " ".join(preproc_defines)

    # Set the memory banking configuration for the compiler
    # accesses need to be aligned to accelerator's data width
    # width of datatype * 2 * max(IC,OC)
    if env_vars["DATATYPE"] not in DATATYPE_WIDTH:
        raise ValueError(
            f"Unknown width for DATATYPE: {env_vars['DATATYPE']}. Please add it to run_soc.py"
        )
    datatype_width = DATATYPE_WIDTH[env_vars["DATATYPE"]]

    # BANK_WIDTH here describes the SoC SRAM row width for the testbench's VPI
    # backdoor addressing (SoCMemory) — the compiler-side
    # --scratchpad_word_bytes is now computed by the accelerator's codegen.mk
    # and no longer injected from here.
    min_unroll = min(int(env_vars["IC_DIMENSION"]), int(env_vars["OC_DIMENSION"]))
    bank_width = int(datatype_width * min_unroll / 8)

    env_vars.setdefault("BANK_WIDTH", str(bank_width))
    env_vars.setdefault("SPDLOG_LEVEL", "DEBUG")


def list_layers(args, env_vars, compile_first):
    """The layers to run: ``--tests``, else each network's ``layers.txt``,
    compiled first when ``compile_first``.

    Returns:
        ``(layers, layer_counts, tile_counts)``, by network.
    """
    layers = {}
    layer_counts = {}
    tile_counts = {}
    if args.tests is not None:
        assert (
            len(args.models) == 1
        ), "Only one model can be specified when using --tests"
        layers[args.models[0]] = args.tests.split(",")
        layer_counts[args.models[0]] = {layer: 1 for layer in layers[args.models[0]]}
        tile_counts[args.models[0]] = {test: 1 for test in layers[args.models[0]]}
        return layers, layer_counts, tile_counts
    options = {
        "env": env_vars,
        "sim_type": "rtl",
        "codegen_root": os.path.join(VOYAGER_DIR, env_vars["CODEGEN_DIR"]),
        "skip_rules": load_skip_rules(VOYAGER_DIR, args.skip_layers),
        "uniquify": args.uniquify_layers,
    }
    for network in args.models:
        if compile_first:
            compile_network(network, repo_dir=VOYAGER_DIR, env=env_vars)
        network_layers(network, layers, layer_counts, tile_counts, **options)
    return layers, layer_counts, tile_counts


def build_simulator(args, env_vars, results_folder):
    """Build the simv (``make default``, or ``debug`` for waveforms) and, in
    full JTAG mode, the scratchpad checker."""
    # Build the simulation binary
    print("Building the simulation binary...")
    with open(f"{results_folder}/sim_build.log", "w") as log_file:
        subprocess.run(
            ["make", "default" if not args.debug else "debug"],
            env=env_vars,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            check=True,
        )

    # Build the JTAG sim checker binary
    if args.jtag_mode == "full":
        print("Building jtag_sim_checker...")
        with open(f"{results_folder}/jtag_sim_checker_build.log", "w") as log_file:
            subprocess.run(
                ["make", "-C", f"{VOYAGER_DIR}/soc/", "jtag_sim_checker"],
                env=env_vars,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                check=True,
            )


def build_firmware(args, env_vars, layers, config_path, results_folder):
    """Emit each network's firmware (``GenerateSoCBinaries``), keep the
    layers that emitted in ``layers``, and link each layer's ELF."""
    # Generate the SoC C driver code
    print("Generating the SoC C driver code...")
    with open(f"{results_folder}/driver_code_gen.log", "w") as log_file:
        for network in args.models:
            # The network and its layers go on make's command line, which
            # make exports to the emitter; the log records them first.
            overrides = [
                *network_settings(network, VOYAGER_DIR),
                f"TESTS={','.join(layers[network])}",
            ]
            log_file.write("".join(f"# {o}\n" for o in overrides))
            log_file.flush()

            gen_start = time.time() - 1
            subprocess.run(
                ["make", "-C", f"{VOYAGER_DIR}/soc/", "GenerateSoCBinaries"]
                + overrides,
                env=env_vars,
                stdout=log_file,
                stderr=subprocess.STDOUT,
            )

            # A layer the emitter cannot map is reported as "Skipping ..." and
            # makes GenerateSoCBinaries exit non-zero. Failing the whole network
            # on that costs every other layer its coverage, so keep the ones
            # that actually emitted -- freshly, so a stale .c from an earlier
            # run cannot stand in for a layer that just failed.
            firmware_dir = (
                f"{VOYAGER_DIR}/soc/firmware/networks/{network}/{config_path}"
            )
            emitted, dropped = [], []
            for layer in layers[network]:
                path = f"{firmware_dir}/{layer}.c"
                if os.path.exists(path) and os.path.getmtime(path) >= gen_start:
                    emitted.append(layer)
                else:
                    dropped.append(layer)
            if dropped:
                print(
                    f"{network}: {len(dropped)} layer(s) did not emit, excluded: "
                    + ", ".join(dropped)
                )
            if not emitted:
                raise RuntimeError(
                    f"{network}: no layer emitted; see {log_file.name}"
                )
            layers[network] = emitted

            # Compile the SoC C code
            for layer in layers[network]:
                cmd = ["make", "-C", "../../tests/voyager/"]

                extra_cflags = []
                # Suppress printf which stalls without a frontend server (all JTAG modes).
                if args.jtag_mode is not None and not args.semihosting:
                    extra_cflags.append("-DSUPPRESS_PRINTF")
                # Full JTAG mode has no testbench: the image is preloaded, so
                # the firmware performs its copies' counter adds itself.
                if args.jtag_mode == "full":
                    extra_cflags.append("-DNO_TESTBENCH")
                # A DRAM config's firmware moves its copies with the DMA
                # engine, unless --mailbox keeps them on the testbench.
                if env_vars.get("HW_DMA") == "true" and not args.mailbox:
                    extra_cflags.append("-DHW_DMA")
                # PLIC ids run in device order, so a config without the UART
                # (which takes id 1) moves Voyager down to it. The firmware
                # defaults to the id the UART-bearing configs give it.
                if (int_id := env_vars.get("VOYAGER_INT_ID")) is not None:
                    extra_cflags.append(f"-DVOYAGER_INT_ID={int_id}")
                # A caller's EXTRA_CFLAGS must extend this list, not be
                # clobbered by it: the command-line assignment below overrides
                # the environment for make.
                if (env_cflags := env_vars.get("EXTRA_CFLAGS")):
                    extra_cflags.append(env_cflags)
                if extra_cflags:
                    cmd.append(f"EXTRA_CFLAGS={' '.join(extra_cflags)}")

                cmd.append(f"networks/{network}/{config_path}/{layer}.riscv")

                subprocess.run(
                    cmd,
                    env=env_vars,
                    stdout=log_file,
                    stderr=subprocess.STDOUT,
                    check=True,
                )


def main():
    parser = argparse.ArgumentParser(description="Run Voyager simulations")
    add_common_args(parser, num_processes=16)
    parser.add_argument("--config", required=True, help="SoC config name")
    parser.add_argument(
        "--jtag-mode",
        choices=["full", "program", "vpi"],
        default=None,
        help=(
            "Run via JTAG interface. "
            "'full': program and data both loaded via JTAG. "
            "'program': program loaded via JTAG, data via VPI. "
            "'vpi': program and data both via VPI, JTAG for startup control only."
        ),
    )
    parser.add_argument(
        "--gl_netlist",
        default=None,
        help="Path to the GL netlist to use for GL simulation.",
    )
    parser.add_argument(
        "--gl_pwr_pins",
        action="store_true",
        help=(
            "Inject VSS/VDDPST/VDD supply declarations and power pin connections into "
            "TestHarness.sv for GL simulation with a power-aware netlist. "
            "Adds +define+GL_PWR_PINS to VCS compilation."
        ),
    )
    parser.add_argument(
        "--results_folder",
        default=None,
        help="Existing regression results folder to parse.",
    )
    parser.add_argument(
        "--semihosting",
        action="store_true",
        help=(
            "Route printf through RISC-V semihosting instead of suppressing it. "
            "Requires --jtag-mode. Output appears in the OpenOCD log."
        ),
    )
    parser.add_argument(
        "--mailbox",
        action="store_true",
        help=(
            "On a DRAM config, serve the copies from the testbench mailbox "
            "instead of the DMA engine."
        ),
    )
    parser.add_argument(
        "--build-only",
        action="store_true",
        help=(
            "Stop once the firmware is linked: compile the networks that have "
            "no model.txt, emit and link every layer, run nothing. No Verilog "
            "is generated and no simulator is built."
        ),
    )
    args = parser.parse_args()

    if args.results_folder is None:
        results_folder = make_results_folder()
    else:
        results_folder = args.results_folder

    env_vars = base_environment(args)
    if args.results_folder is not None:
        # A re-parse only grades the logs already there.
        add_config_environment(args, env_vars)
        layers, layer_counts, tile_counts = list_layers(
            args, env_vars, compile_first=False
        )
        success = regrade_results_folder(
            layers, layer_counts, tile_counts, results_folder, args.jtag_mode
        )
        exit(0 if success else 1)

    if not args.build_only:
        generate_verilog(env_vars, results_folder)
    if args.gl_pwr_pins:
        inject_power_pins(
            os.path.join(
                "generated-src",
                f"chipyard.harness.TestHarness.{args.config}",
                "gen-collateral",
                "TestHarness.sv",
            )
        )
    add_config_environment(args, env_vars)
    layers, layer_counts, tile_counts = list_layers(
        args, env_vars, compile_first=True
    )
    config_path = f"{env_vars['DATATYPE']}/{build_params(env_vars)}"

    build_simulator(args, env_vars, results_folder)
    build_firmware(args, env_vars, layers, config_path, results_folder)
    if args.build_only:
        total = sum(len(v) for v in layers.values())
        print(f"--build-only: {total} layer(s) linked under tests/voyager/networks/, nothing run.")
        exit(0)

    success = run_rtl_simulations(
        layers,
        layer_counts,
        tile_counts,
        args.num_processes,
        results_folder,
        env_vars,
        config_path,
        args.debug,
        args.jtag_mode,
    )

    exit(0 if success else 1)


if __name__ == "__main__":
    main()
