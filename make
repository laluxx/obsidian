#!/usr/bin/env python3
"""Small, headless-first developer entrypoint for Obsidian."""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
COLORS = {"cyan": "36;1", "green": "32;1", "yellow": "33;1", "red": "31;1"}

CPU_BENCHMARKS = (
    "font",
    "dispatch",
    "keymap",
    "node-topology",
    "object-identity",
    "foreign-slots",
    "omdl-digest",
    "texture-refs",
    "sdf-undo",
    "skin-overrides",
    "animation-channels",
    "transparent-sort",
)
BENCHMARK_TARGETS = {
    "font": ("bench-font", "CPU UTF-8 scan; no GPU or FreeType rendering"),
    "dispatch": ("bench-dispatch", "C recorder overhead; no Monad/JIT"),
    "keymap": ("bench-keymap", "Production key dispatch and keymap operations"),
    "node-topology": ("bench-node-topology", "CPU scene topology microbenchmark"),
    "object-identity": ("bench-object-identity", "CPU object identity registry"),
    "foreign-slots": ("bench-foreign-slots", "C foreign-slot lookup comparison"),
    "omdl-digest": ("bench-omdl-digest", "CPU asset digest comparison"),
    "texture-refs": ("bench-texture-refs", "CPU texture-reference lookup"),
    "sdf-undo": (
        "bench-sdf-undo",
        "Production SDF undo/redo journal; CPU-only, no Vulkan timing",
    ),
    "skin-overrides": (
        "bench-skin-overrides",
        "Production bone-override routing; indexed vs nested CPU scan",
    ),
    "animation-channels": (
        "bench-animation-channels",
        "Production animate_scene channel scans and hierarchy; CPU-only",
    ),
    "transparent-sort": (
        "bench-transparent-sort",
        "Production transparent-mesh sorting; CPU-only, no Vulkan startup",
    ),
    "keychords-performance": (
        "test-keychords-performance",
        "Native/Monad keychord component; requires MonadC and GLFW",
    ),
    "engine": (
        "bench-monad",
        "Headless Vulkan engine workload; requires assets, GPU and MonadC",
    ),
    "engine-validation": (
        "bench-monad-validation",
        "One-run Vulkan validation diagnostic; requires validation layers",
    ),
}
BENCHMARK_GROUPS = {
    "cpu": CPU_BENCHMARKS,
    "host": ("keychords-performance",),
    "validation": ("engine-validation",),
    "all": (*CPU_BENCHMARKS, "keychords-performance", "engine", "engine-validation"),
}

BUILD_TARGETS = {
    "default": ("all", "Build the optimized engine (default target)"),
    "libs": ("libs", "Build static and shared engine libraries"),
    "debug": ("debug", "Build with debug flags"),
    "run": ("run", "Build and launch the graphical sample"),
    "msdf-tool": ("msdf-tool", "Build the optional MSDF utility"),
}

TEST_TARGETS = {
    "default": ("test", "Default Make suite; unavailable SDK/live suites report SKIP"),
    "all": ("test-all", "Strict SDK/Monad/frame integration gate"),
    "headless": (
        "test-headless",
        "Native tests + ASan/UBSan/LSan + CPU benchmarks; optional Vulkan smokes",
    ),
    "native": ("test-native", "Headless production-C regressions"),
    "tooling": ("test-tooling", "Build workflow, benchmark schema, and CLI contracts"),
    "monad": ("test-monad", "Real embedded Monad integration fixtures"),
    "frame": ("test-frame", "Live renderer and active-frame integration"),
    "native-profile": ("test-native-profile", "Native CPU/GPU profiler contracts"),
    "agent-api": ("test-agent-api-compile", "External API compile contracts"),
    "agent-runtime": ("test-agent-runtime", "Linked engine/Monad agent runtime"),
    "keychords-monad": ("test-keychords-monad", "Real Monad keychord integration"),
    "keychords-performance": (
        "test-keychords-performance",
        "Monad keychord microbenchmark; requires MonadC and GLFW",
    ),
    "renderer2d-capacity": ("test-renderer2d-capacity", "Renderer capacity C contract"),
    "emacs-reference": ("test-emacs-reference", "Optional Emacs ERT oracle"),
    "emacs-scene-undo": ("test-emacs-scene-undo", "Native scene undo contract"),
    "sculpture-runtime": ("test-sculpture-runtime", "Linked Vulkan/Monad sculpture runtime"),
    "semantic-preflight": ("test-semantic-preflight", "MonadC semantic preflight contract"),
    "catalog-transition": ("test-catalog-transition", "Requires public MonadC SDK headers"),
    "export-catalog": ("test-export-catalog", "Requires public MonadC SDK headers"),
    "native-read": ("test-native-read", "Requires public MonadC SDK headers"),
    "headless-startup-render": ("test-headless-startup-render", "Requires engine, Vulkan and assets"),
    "headless-sponza": ("test-headless-sponza", "Requires engine, Vulkan and Sponza assets"),
    "vulkan-validation": (
        "test-vulkan-validation-headless",
        "Requires Vulkan validation layer and a compatible GPU",
    ),
    "vulkan-dynamic-headless": (
        "test-vulkan-dynamic-headless",
        "Dynamic vertex capacity/frame slicing with Vulkan validation",
    ),
    "vulkan-dynamic-window": (
        "test-vulkan-dynamic-window",
        "Two-window dynamic frame ownership and swapchain validation",
    ),
    "middle-orbit": ("test-headless-middle-orbit", "Requires Monad CLI, engine and Vulkan"),
    "editor-keymaps": ("test-editor-keymaps", "Monad keymap language contract"),
    "headless-editor-keymaps": (
        "test-headless-editor-keymaps",
        "Requires the engine, assets and Vulkan",
    ),
    "transparent-sort": (
        "test-transparent-sort",
        "Production renderer transparent sort and allocation-failure contract",
    ),
    "repl-abi": ("test-repl-abi", "Requires public MonadC SDK headers"),
    "font-unicode": ("test-font-unicode", "Requires FreeType, Vulkan and MonadC"),
    "scene-gesture": ("test-scene-gesture", "Native scene gesture contract"),
    "sdf-viewport": ("test-sdf-viewport", "Native SDF viewport contract"),
    "sdf-typed-viewport": ("test-sdf-typed-viewport", "Typed SDF viewport contract"),
    "sdf-viewport-runtime": (
        "test-sdf-viewport-runtime",
        "Linked Vulkan/Monad viewport runtime",
    ),
    "viewport-coordinates": ("test-viewport-coordinates", "Native viewport coordinates"),
    "sdf-property-bridge": ("test-sdf-property-bridge", "Native SDF property/undo contract"),
    "sdf-property-gesture": ("test-sdf-property-gesture", "Native SDF gesture/undo contract"),
    "sdf-appearance-clipboard": (
        "test-sdf-appearance-clipboard",
        "Native SDF appearance clipboard contract",
    ),
}
TEST_GROUPS = {
    "host": ("native", "tooling"),
    "integration": ("monad", "frame"),
}

PROFILE_TARGETS = {
    "engine": ("bench-monad", "Headless Vulkan/Monad workload"),
    "validation": ("bench-monad-validation", "Synchronization-validation diagnostic"),
    "check": ("profile-check", "Profiler contracts plus GPU/render benchmark gates"),
}


def color_enabled(mode: str) -> bool:
    return mode == "always" or (mode == "auto" and sys.stdout.isatty()
                                and "NO_COLOR" not in os.environ)


def paint(text: str, color: str, enabled: bool) -> str:
    return f"\033[{COLORS[color]}m{text}\033[0m" if enabled else text


def glyph(value: str, fallback: str) -> str:
    try:
        value.encode(sys.stdout.encoding or "utf-8")
        return value
    except UnicodeEncodeError:
        return fallback


def dashboard(title: str, subtitle: str, enabled: bool) -> None:
    width = min(96, max(50, shutil.get_terminal_size((88, 24)).columns))
    horizontal = glyph("─", "-")
    print()
    print(paint("  " + glyph("╭", "+") + horizontal * (width - 4) +
                glyph("╮", "+"), "cyan", enabled))
    print(paint("  │", "cyan", enabled) + "  " +
          paint(title, "cyan", enabled))
    print(paint("  │", "cyan", enabled) + "  " + subtitle)
    print(paint("  " + glyph("╰", "+") + horizontal * (width - 4) +
                glyph("╯", "+"), "cyan", enabled))


def run_step(label: str, command: list[str], env: dict[str, str],
             enabled: bool) -> int:
    print(f"\n  {paint('▶', 'cyan', enabled)} {label}")
    print("    " + " ".join(command))
    started = time.perf_counter()
    result = subprocess.run(command, cwd=ROOT, env=env, check=False)
    elapsed = time.perf_counter() - started
    if result.returncode == 0:
        status = paint("PASS", "green", enabled)
    else:
        status = paint(f"FAIL ({result.returncode})", "red", enabled)
    print(f"\n  {status} · {label} · {elapsed:.2f}s")
    return result.returncode


def parser() -> argparse.ArgumentParser:
    cli = argparse.ArgumentParser(
        description="Obsidian build and quality frontend; GNU Make remains the build backend."
    )
    cli.add_argument(
        "command",
        nargs="?",
        choices=("build", "test", "profile", "bench", "clean", "help"),
        default="help",
    )
    cli.add_argument("selection", nargs="?", help="target, suite, or named workload")
    cli.add_argument("--color", choices=("auto", "always", "never"), default="auto")
    cli.add_argument("--asset-root", default=os.environ.get(
        "BENCH_ASSET_ROOT", "assets"),
        help="root containing gltf/ scenes (default: assets)")
    cli.add_argument("--runs", type=int)
    cli.add_argument("--jobs", type=int, help="parallel build jobs")
    cli.add_argument("--iterations", type=int, help="iterations for CPU microbenchmarks")
    cli.add_argument("--nodes", type=int, help="node count for node-topology")
    cli.add_argument("--slots", type=int, help="slot count for object-identity")
    cli.add_argument("--frames", type=int, help="measured engine frames / topology frames")
    cli.add_argument("--warmup-frames", type=int, help="engine warm-up frames")
    cli.add_argument("--manifest", help="Monad benchmark manifest")
    cli.add_argument("--output", help="benchmark artifact directory")
    cli.add_argument("--gpu-timestamps", choices=("on", "off"))
    cli.add_argument("--debug-labels", choices=("on", "off"))
    cli.add_argument("--work-counters", choices=("on", "off"))
    cli.add_argument("--compare-to", help="compatible summary.json baseline")
    cli.add_argument(
        "--comparison-metric",
        choices=("total_ns", "gpu_ns", "command_record_ns"),
    )
    cli.add_argument("--max-regression-pct", type=float)
    cli.add_argument("--list", action="store_true", help="list command names and requirements")
    return cli


def benchmark_names(selection: str) -> tuple[str, ...]:
    if selection in BENCHMARK_GROUPS:
        return BENCHMARK_GROUPS[selection]
    return (selection,)


def print_catalog(
    title: str,
    subtitle: str,
    targets: dict[str, tuple[str, str]],
    groups: dict[str, tuple[str, ...]],
    enabled: bool,
) -> None:
    dashboard(title, subtitle, enabled)
    name_width = max((len(name) for name in targets), default=4)
    target_width = max((len(target) for target, _ in targets.values()), default=11)
    print(f"\n  {'NAME':{name_width}}  {'MAKE TARGET':{target_width}}  REQUIREMENTS / SCOPE")
    print(f"  {'-' * name_width}  {'-' * target_width}  {'-' * 44}")
    for name, (target, description) in targets.items():
        print(f"  {name:{name_width}}  {target:{target_width}}  {description}")
    if groups:
        print("\n  Groups:")
        for name, members in groups.items():
            print(f"    {name}: {', '.join(members)}")


def list_benchmarks(enabled: bool) -> None:
    print_catalog(
        "OBSIDIAN  /  BENCHMARKS",
        "'all' runs every registered benchmark; use 'cpu' to avoid GPU/SDK workloads",
        BENCHMARK_TARGETS,
        BENCHMARK_GROUPS,
        enabled,
    )


def list_tests(enabled: bool) -> None:
    print_catalog(
        "OBSIDIAN  /  TEST TARGETS",
        "'default' runs the supported test gate; 'all' is the strict integration gate",
        TEST_TARGETS,
        TEST_GROUPS,
        enabled,
    )


def list_build_targets(enabled: bool) -> None:
    print_catalog(
        "OBSIDIAN  /  BUILD TARGETS",
        "Python frontend routes to existing GNU Make build targets",
        BUILD_TARGETS,
        {},
        enabled,
    )


def list_profile_targets(enabled: bool) -> None:
    print_catalog(
        "OBSIDIAN  /  PROFILE TARGETS",
        "Repeatable engine measurements and full GPU profile validation",
        PROFILE_TARGETS,
        {},
        enabled,
    )


def bench(args: argparse.Namespace, env: dict[str, str], enabled: bool) -> int:
    if args.list:
        list_benchmarks(enabled)
        return 0
    runs = args.runs if args.runs is not None else 5
    if runs < 1:
        print(paint("\n  --runs must be at least 1.", "red", enabled))
        return 2
    if args.jobs is not None and args.jobs < 1:
        print(paint("\n  --jobs must be at least 1.", "red", enabled))
        return 2
    if args.max_regression_pct is not None and (
        not math.isfinite(args.max_regression_pct)
        or args.max_regression_pct < 0
        or not args.compare_to
    ):
        print(
            paint(
                "\n  --max-regression-pct needs --compare-to and a finite nonnegative value.",
                "red",
                enabled,
            )
        )
        return 2
    for option in ("iterations", "nodes", "slots", "frames"):
        value = getattr(args, option)
        if value is not None and value < 1:
            print(paint(f"\n  --{option.replace('_', '-')} must be at least 1.", "red", enabled))
            return 2
    if args.warmup_frames is not None and args.warmup_frames < 0:
        print(paint("\n  --warmup-frames cannot be negative.", "red", enabled))
        return 2

    env["BENCH_RUNS"] = str(runs)
    if args.jobs is not None:
        env["JOBS"] = str(args.jobs)
    if args.iterations is not None:
        env["BENCH_ITERATIONS"] = str(args.iterations)
    if args.nodes is not None:
        env["NODES"] = str(args.nodes)
    if args.slots is not None:
        env["BENCH_SLOTS"] = str(args.slots)
    if args.frames is not None:
        env["BENCH_FRAMES"] = str(args.frames)
        env["FRAMES"] = str(args.frames)
    if args.warmup_frames is not None:
        env["BENCH_WARMUP_FRAMES"] = str(args.warmup_frames)
    if args.manifest is not None:
        env["BENCH_MONAD_MANIFEST"] = args.manifest
    if args.output is not None:
        env["BENCH_OUTPUT"] = args.output
    for option, variable in (
        ("gpu_timestamps", "BENCH_GPU_TIMESTAMPS"),
        ("debug_labels", "BENCH_DEBUG_LABELS"),
        ("work_counters", "BENCH_WORK_COUNTERS"),
        ("compare_to", "BENCH_COMPARE_TO"),
        ("comparison_metric", "BENCH_COMPARISON_METRIC"),
        ("max_regression_pct", "BENCH_MAX_REGRESSION_PCT"),
    ):
        value = getattr(args, option)
        if value is not None:
            env[variable] = str(value)

    selection = args.selection or "all"
    if selection not in BENCHMARK_GROUPS and selection not in BENCHMARK_TARGETS:
        print(paint(f"\n  Unknown benchmark '{selection}'. Use './make bench --list'.", "red", enabled))
        return 2
    names = benchmark_names(selection)
    dashboard(
        "OBSIDIAN  /  BENCHMARK LAB",
        f"{selection} · {len(names)} workload(s) · sequential, isolated targets",
        enabled,
    )
    failures = 0
    for name in names:
        target, requirements = BENCHMARK_TARGETS[name]
        print(f"\n  {paint('SCOPE', 'cyan', enabled)} {name}: {requirements}")
        failures += run_step(name, ["make", target], env, enabled) != 0
    if failures:
        print(paint(f"\n  {failures} benchmark target(s) failed.", "red", enabled))
        return 1
    print(paint(f"\n  All {len(names)} benchmark target(s) passed.", "green", enabled))
    return 0


def selected_names(selection: str, targets: dict[str, tuple[str, str]],
                   groups: dict[str, tuple[str, ...]], default: str) -> tuple[str, ...] | None:
    choice = selection or default
    if choice in groups:
        return groups[choice]
    if choice in targets:
        return (choice,)
    return None


def reject_options(args: argparse.Namespace, allowed: set[str], enabled: bool) -> bool:
    option_fields = {
        "runs": args.runs,
        "iterations": args.iterations,
        "nodes": args.nodes,
        "slots": args.slots,
        "frames": args.frames,
        "warmup-frames": args.warmup_frames,
        "manifest": args.manifest,
        "output": args.output,
        "gpu-timestamps": args.gpu_timestamps,
        "debug-labels": args.debug_labels,
        "work-counters": args.work_counters,
        "compare-to": args.compare_to,
        "comparison-metric": args.comparison_metric,
        "max-regression-pct": args.max_regression_pct,
    }
    unsupported = [name for name, value in option_fields.items()
                   if value is not None and name not in allowed]
    if unsupported:
        print(paint(
            f"\n  Option(s) --{', --'.join(unsupported)} do not apply to '{args.command}'.",
            "red", enabled,
        ))
        return True
    return False


def help_text(enabled: bool) -> None:
    dashboard("OBSIDIAN  /  DEVELOPER CLI",
              "One discoverable frontend; existing GNU Make targets and recipes remain available",
              enabled)
    print("""
  Usage:
    ./make build   {default libs debug run, msdf-tool}
    ./make test    {default all native tooling monad frame <named suite>}
    ./make bench   {all cpu host validation <named workload>}
    ./make profile {engine validation check}
    ./make clean
    ./make help    {build test bench profile clean}

  Lists:
    ./make build --list       ./make test --list
    ./make bench --list       ./make profile --list

  Tests:
    'default' runs make test; gated integration suites report SKIP when
    prerequisites are absent. 'all' maps to strict make test-all.
    Use './make test monad' or './make test frame' for explicit integration.

  Backend:
    This frontend does not replace GNU Make. Every selection invokes the
    corresponding existing Make target; direct Make targets remain supported.
""")


def main() -> int:
    args = parser().parse_args()
    enabled = color_enabled(args.color)
    env = os.environ.copy()
    env["BENCH_COLOR"] = args.color
    env["BENCH_ASSET_ROOT"] = args.asset_root

    if args.command == "help":
        if args.selection is None:
            help_text(enabled)
            return 0
        command = args.selection
        if command == "build":
            list_build_targets(enabled)
        elif command == "test":
            list_tests(enabled)
        elif command == "bench":
            list_benchmarks(enabled)
        elif command == "profile":
            list_profile_targets(enabled)
        elif command == "clean":
            print("  ./make clean  Remove project build, test, vendor, shader, and object outputs.")
        else:
            print(paint(f"\n  Unknown help topic '{command}'.", "red", enabled))
            return 2
        return 0

    if args.list:
        if args.command == "build":
            list_build_targets(enabled)
        elif args.command == "test":
            list_tests(enabled)
        elif args.command == "bench":
            list_benchmarks(enabled)
        elif args.command == "clean":
            print("  ./make clean  Remove project build, test, vendor, shader, and object outputs.")
        else:
            list_profile_targets(enabled)
        return 0

    if args.command == "clean":
        if args.selection is not None:
            print(paint("\n  'clean' does not accept a selection.", "red", enabled))
            return 2
        return run_step("clean", ["make", "clean"], env, enabled)

    if args.jobs is not None:
        if args.jobs < 1:
            print(paint("\n  --jobs must be at least 1.", "red", enabled))
            return 2
        env["JOBS"] = str(args.jobs)

    if args.command == "bench":
        return bench(args, env, enabled)

    if args.command == "build":
        if reject_options(args, set(), enabled):
            return 2
        names = selected_names(args.selection, BUILD_TARGETS, {}, "default")
        if names is None:
            print(paint("\n  Unknown build target. Use './make build --list'.", "red", enabled))
            return 2
        title, subtitle = "OBSIDIAN  /  BUILD", "Existing GNU Make build target"
        targets = BUILD_TARGETS
    elif args.command == "test":
        if reject_options(args, set(), enabled):
            return 2
        names = selected_names(args.selection, TEST_TARGETS, TEST_GROUPS, "default")
        if names is None:
            print(paint("\n  Unknown test suite. Use './make test --list'.", "red", enabled))
            return 2
        title, subtitle = "OBSIDIAN  /  TEST LAB", "Existing GNU Make test target(s)"
        targets = TEST_TARGETS
    else:
        if reject_options(args, {"runs", "manifest", "output", "gpu-timestamps",
                                 "debug-labels", "work-counters", "compare-to",
                                 "comparison-metric", "max-regression-pct"}, enabled):
            return 2
        names = selected_names(args.selection, PROFILE_TARGETS, {}, "engine")
        if names is None:
            print(paint("\n  Unknown profile target. Use './make profile --list'.", "red", enabled))
            return 2
        runs = args.runs if args.runs is not None else 5
        if runs < 1:
            print(paint("\n  --runs must be at least 1.", "red", enabled))
            return 2
        env["BENCH_RUNS"] = str(runs)
        if args.manifest is not None:
            env["BENCH_MONAD_MANIFEST"] = args.manifest
        if args.output is not None:
            env["BENCH_OUTPUT"] = args.output
        for option, variable in (
            ("gpu_timestamps", "BENCH_GPU_TIMESTAMPS"),
            ("debug_labels", "BENCH_DEBUG_LABELS"),
            ("work_counters", "BENCH_WORK_COUNTERS"),
            ("compare_to", "BENCH_COMPARE_TO"),
            ("comparison_metric", "BENCH_COMPARISON_METRIC"),
            ("max_regression_pct", "BENCH_MAX_REGRESSION_PCT"),
        ):
            value = getattr(args, option)
            if value is not None:
                env[variable] = str(value)
        title, subtitle = "OBSIDIAN  /  PERFORMANCE LAB", "Existing GNU Make profiler target"
        targets = PROFILE_TARGETS

    dashboard(title, f"{len(names)} selected target(s) · GNU Make backend", enabled)
    failures = 0
    for name in names:
        target, description = targets[name]
        print(f"\n  {paint('SCOPE', 'cyan', enabled)} {name}: {description}")
        failures += run_step(name, ["make", target], env, enabled) != 0
    if failures:
        print(paint(f"\n  {failures} target(s) failed.", "red", enabled))
        return 1
    print(paint(f"\n  All {len(names)} target(s) passed.", "green", enabled))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
