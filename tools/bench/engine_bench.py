#!/usr/bin/env python3
"""Repeatable Vulkan benchmark runner; project profiling targets use headless mode."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import statistics
import struct
import subprocess
import sys
import time
import shutil
from urllib.parse import unquote, urlsplit


def percentile(values: list[int], percent: int) -> int | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(percent * len(ordered) / 100) - 1)]


def summarize(values: list[int]) -> dict[str, int | float | None]:
    mean = statistics.fmean(values) if values else None
    stdev = statistics.pstdev(values) if values else None
    return {
        "samples": len(values),
        "median_ns": percentile(values, 50),
        "p95_ns": percentile(values, 95),
        "p99_ns": percentile(values, 99),
        "min_ns": min(values) if values else None,
        "max_ns": max(values) if values else None,
        "mean_ns": mean,
        "stdev_ns": stdev,
        "coefficient_of_variation_pct": (
            stdev / mean * 100 if mean else None
        ),
    }


def summarize_independent_runs(
    values: list[int | float],
) -> dict[str, int | float | list[int | float] | str | None]:
    """Summarize one per-process statistic; never treat frames as independent runs."""
    center = statistics.median(values) if values else None
    deviations = [abs(value - center) for value in values] if center is not None else []
    interval = None
    if len(values) >= 5:
        rng = random.Random(0)
        bootstrapped = sorted(
            statistics.median(rng.choices(values, k=len(values)))
            for _ in range(10_000)
        )
        interval = [
            bootstrapped[math.floor(0.025 * (len(bootstrapped) - 1))],
            bootstrapped[math.ceil(0.975 * (len(bootstrapped) - 1))],
        ]
    return {
        "independent_processes": len(values),
        "process_medians_ns": values,
        "median_ns": center,
        "median_absolute_deviation_ns": (
            statistics.median(deviations) if deviations else None
        ),
        "min_ns": min(values) if values else None,
        "max_ns": max(values) if values else None,
        "bootstrap_median_ci_95_ns": interval,
        "bootstrap_method": (
            "percentile bootstrap over independent process medians; "
            "10,000 deterministic resamples; available only with >=5 processes"
        ),
    }


def valid_gpu_stage_schema(schema: object) -> bool:
    if not isinstance(schema, list) or not schema:
        return False
    if any(
        not isinstance(stage, dict)
        or type(stage.get("id")) is not int
        or not isinstance(stage.get("name"), str)
        or not stage["name"]
        or stage.get("unit") != "ns"
        or stage.get("timing") != "inclusive"
        for stage in schema
    ):
        return False
    names = [stage["name"] for stage in schema]
    if len(names) != len(set(names)) or \
            [stage["id"] for stage in schema] != list(range(len(schema))):
        return False
    return all(stage.get("parent") is None or stage["parent"] in names
               for stage in schema)


def valid_vulkan_device(device: object) -> bool:
    return (
        isinstance(device, dict)
        and isinstance(device.get("name"), str)
        and bool(device["name"])
        and all(type(device.get(key)) is int and device[key] >= 0
                for key in ("vendor_id", "device_id", "driver_version",
                            "api_version"))
    )

def valid_process_memory(memory: object) -> bool:
    if not isinstance(memory, dict) or \
            memory.get("scope") != "whole_process" or \
            memory.get("peak_source") != "getrusage.ru_maxrss" or \
            memory.get("final_source") != "/proc/self/statm":
        return False
    return all(
        value is None or (type(value) is int and value >= 0)
        for value in (memory.get("peak_rss_bytes"),
                      memory.get("final_rss_bytes"))
    )


def valid_gpu_timestamp_metadata(metadata: object) -> bool:
    if not isinstance(metadata, dict) or \
            type(metadata.get("supported")) is not bool or \
            type(metadata.get("enabled")) is not bool or \
            type(metadata.get("queue_family_index")) is not int or \
            type(metadata.get("valid_bits")) is not int:
        return False
    period = metadata.get("period_ns")
    if not isinstance(period, (int, float)) or not math.isfinite(period):
        return False
    if metadata["supported"]:
        return (metadata["queue_family_index"] >= 0
                and 1 <= metadata["valid_bits"] <= 64
                and period > 0)
    return not metadata["enabled"] and metadata["valid_bits"] == 0


def valid_gpu_memory_budget(memory: object) -> bool:
    if not isinstance(memory, dict) or \
            memory.get("scope") != "current_process_estimate" or \
            memory.get("provider") != "VK_EXT_memory_budget" or \
            type(memory.get("supported")) is not bool or \
            not isinstance(memory.get("heaps"), list):
        return False
    heaps = memory["heaps"]
    if not memory["supported"]:
        return not heaps
    if not 1 <= len(heaps) <= 16:
        return False
    return all(
        isinstance(heap, dict)
        and type(heap.get("heap_index")) is int
        and heap["heap_index"] == index
        and all(type(heap.get(key)) is int and heap[key] >= 0
                for key in ("initial_usage_bytes", "final_usage_bytes",
                            "final_budget_bytes"))
        for index, heap in enumerate(heaps)
    )


def valid_engine_vulkan_memory(memory: object) -> bool:
    if not isinstance(memory, dict) or \
            memory.get("scope") != \
            "live_VkDeviceMemory_allocation_sizes_by_heap" or \
            type(memory.get("sampled")) is not bool or \
            not isinstance(memory.get("heaps"), list):
        return False
    heaps = memory["heaps"]
    if not memory["sampled"]:
        return not heaps
    if not 1 <= len(heaps) <= 16:
        return False
    fields = (
        "initial_live_bytes", "final_live_bytes", "peak_live_bytes",
        "initial_live_allocations", "final_live_allocations",
        "peak_live_allocations",
    )
    for index, heap in enumerate(heaps):
        if not isinstance(heap, dict) or \
                type(heap.get("heap_index")) is not int or \
                heap["heap_index"] != index or \
                not all(type(heap.get(field)) is int and heap[field] >= 0
                        for field in fields):
            return False
        if heap["peak_live_bytes"] < max(
                heap["initial_live_bytes"], heap["final_live_bytes"]) or \
                heap["peak_live_allocations"] < max(
                    heap["initial_live_allocations"],
                    heap["final_live_allocations"]):
            return False
    return True


def valid_gpu_geometry_buffers(buffers: object) -> bool:
    if not isinstance(buffers, dict) or \
            buffers.get("scope") != "logical_buffer_size_and_write_cursor" or \
            type(buffers.get("sampled")) is not bool:
        return False
    fields = (
        "vertex_capacity_bytes", "vertex_used_bytes",
        "index_capacity_bytes", "index_used_bytes",
    )
    if not buffers["sampled"]:
        return all(buffers.get(field) is None for field in fields)
    return all(
        type(buffers.get(capacity)) is int
        and buffers[capacity] > 0
        and type(buffers.get(used)) is int
        and 0 <= buffers[used] <= buffers[capacity]
        for capacity, used in (
            ("vertex_capacity_bytes", "vertex_used_bytes"),
            ("index_capacity_bytes", "index_used_bytes"),
        )
    )

def validation_error_lines(log_contents: str) -> list[str]:
    return [
        line for line in log_contents.splitlines()
        if "Validation Error:" in line
    ]

WORK_COUNTER_NAMES = (
    "draw_calls", "indirect_draws", "submitted_vertices",
    "submitted_indices", "mesh_task_workgroups", "compute_dispatches",
    "compute_workgroups",
)
STARTUP_MILESTONES = (
    "process_entry", "glfw_init", "glfw_window", "input_ready",
    "editor_font_ready", "audio_ready", "vulkan_instance",
    "physical_device", "logical_device", "swapchain", "renderer_ready",
    "monad_runtime_ready", "editor_ready", "monad_boot_ready",
    "first_submit", "first_present",
)

def valid_allocation_owners(report: dict) -> bool:
    data = report.get("allocation_owners")
    if not isinstance(data, dict) or \
            data.get("scope") != "VkDeviceMemory_constructor_ownership" or \
            type(data.get("sampled")) is not bool or \
            not isinstance(data.get("owners"), list):
        return False
    owners = data["owners"]
    names = []
    for owner in owners:
        if not isinstance(owner, dict) or \
                not isinstance(owner.get("name"), str) or not owner["name"]:
            return False
        names.append(owner["name"])
        for field in ("live_bytes", "peak_bytes", "live_allocations", "peak_allocations"):
            if type(owner.get(field)) is not int or owner[field] < 0:
                return False
        if owner["peak_bytes"] < owner["live_bytes"] or \
                owner["peak_allocations"] < owner["live_allocations"]:
            return False
    if len(names) != len(set(names)):
        return False
    heaps = report.get("engine_vulkan_memory", {})
    if data["sampled"] != heaps.get("sampled"):
        return False
    if not data["sampled"]:
        return not owners
    return all(
        sum(owner[field] for owner in owners) ==
        sum(heap[heap_field] for heap in heaps.get("heaps", []))
        for field, heap_field in (
            ("live_bytes", "final_live_bytes"),
            ("live_allocations", "final_live_allocations"))
    )


def valid_startup_milestones(milestones: object) -> bool:
    if not isinstance(milestones, dict) or \
            tuple(milestones) != STARTUP_MILESTONES:
        return False
    values = []
    for name in STARTUP_MILESTONES:
        value = milestones[name]
        if value is not None and (type(value) is not int or value < 0):
            return False
        if value is not None:
            values.append(value)
    return milestones["process_entry"] == 0 and \
        all(left <= right for left, right in zip(values, values[1:]))

def valid_work_counters(counters: object, enabled: bool) -> bool:
    if not isinstance(counters, dict) or \
            type(counters.get("sampled")) is not bool or \
            counters["sampled"] != enabled:
        return False
    if enabled:
        return all(type(counters.get(name)) is int and counters[name] >= 0
                   for name in WORK_COUNTER_NAMES)
    return all(counters.get(name) is None for name in WORK_COUNTER_NAMES)


def valid_work_counters_by_stage(
        data: object, enabled: bool, stage_names: list[str],
        sampled_frames: int) -> bool:
    if not enabled:
        return data is None
    if not isinstance(data, dict) or \
            data.get("sampled_frames") != sampled_frames or \
            data.get("unit") != "total_commands_over_measured_frames" or \
            not isinstance(data.get("stages"), dict) or \
            set(data["stages"]) != set(stage_names):
        return False
    return all(
        isinstance(data["stages"][stage], dict) and
        all(type(data["stages"][stage].get(name)) is int and
            data["stages"][stage][name] >= 0
            for name in WORK_COUNTER_NAMES)
        for stage in stage_names
    )


def consistent_work_counter_attribution(
        by_stage: dict, frames: list[dict],
        stage_schema: list[dict]) -> bool:
    stages = by_stage["stages"]
    for counter in WORK_COUNTER_NAMES:
        frame_total = sum(
            frame["work_counters"][counter]
            for frame in frames
            if frame["work_counters"]["sampled"])
        if stages["frame"][counter] != frame_total:
            return False
    for parent in ("shadows", "hzb"):
        children = [
            stage["name"] for stage in stage_schema
            if stage["parent"] == parent
        ]
        if children:
            for counter in WORK_COUNTER_NAMES:
                if stages[parent][counter] != sum(
                        stages[child][counter] for child in children):
                    return False
    return True


def summarize_count(values: list[int]) -> dict[str, int | float | None]:
    return {
        key.replace("_ns", "_count"): value
        for key, value in summarize(values).items()
    }

def use_color(setting: str) -> bool:
    if setting == "always":
        return True
    if setting == "never":
        return False
    return sys.stdout.isatty() and "NO_COLOR" not in os.environ


def paint(text: str, color: str, enabled: bool) -> str:
    return f"\033[{color}m{text}\033[0m" if enabled else text


def duration_ms(value: int | float | None) -> str:
    return "n/a" if value is None else f"{value / 1_000_000:.3f} ms"

def format_bytes(value: int | None) -> str:
    return "n/a" if value is None else f"{value / (1024 * 1024):.1f} MiB"


def source_provenance(root: Path, executable: Path,
                      compiler: str, flags: str) -> dict:
    try:
        revision = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=root, capture_output=True,
            text=True, check=True,
        ).stdout.strip()
        status = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=all"],
            cwd=root, capture_output=True, text=True, check=True,
        ).stdout
        dirty: bool | None = bool(status)
    except (OSError, subprocess.CalledProcessError):
        revision, dirty = None, None
    executable_sha256 = file_sha256(executable)
    source_sha256, source_file_count = benchmark_source_sha256(root)
    return {
        "source_revision": revision,
        "worktree_dirty": dirty,
        "benchmark_source_sha256": source_sha256,
        "benchmark_source_file_count": source_file_count,
        "executable_path": str(executable),
        "executable_sha256": executable_sha256,
        "compiler_command": compiler,
        "compiler_flags": flags,
    }


def file_sha256(path: Path) -> str | None:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as binary:
            for block in iter(lambda: binary.read(1024 * 1024), b""):
                digest.update(block)
    except OSError:
        return None
    return digest.hexdigest()


def benchmark_source_sha256(root: Path) -> tuple[str | None, int]:
    root = root.resolve()
    suffixes = {".c", ".h", ".cc", ".cpp", ".hpp", ".vert", ".frag",
                ".comp", ".task", ".mesh", ".glsl"}
    excluded = {".git", "build", "__pycache__"}
    candidates = {root / "Makefile", root / "obsidian.h",
                  root / "tools/bench/engine_bench.py"}
    for directory in ("src", "shaders", "vendor"):
        base = root / directory
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.is_file() and path.suffix in suffixes and not any(
                    part in excluded for part in path.relative_to(root).parts):
                candidates.add(path)
    candidates = {path for path in candidates if path.is_file()}
    digest = hashlib.sha256()
    for path in sorted(candidates, key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        file_digest = file_sha256(path)
        if file_digest is None:
            return None, len(candidates)
        digest.update(len(relative).to_bytes(8, "little"))
        digest.update(relative)
        digest.update(bytes.fromhex(file_digest))
    return digest.hexdigest(), len(candidates)


def asset_input_manifest(root: Path, scene_path: str) -> dict:
    root = root.resolve()
    scene = Path(scene_path)
    if not scene.is_absolute():
        scene = root / scene
    scene = scene.resolve()
    files: set[Path] = {scene}
    missing: list[str] = []

    try:
        if scene.suffix.lower() == ".gltf":
            with scene.open("r", encoding="utf-8") as source:
                document = json.load(source)
        elif scene.suffix.lower() == ".glb":
            with scene.open("rb") as source:
                header = source.read(12)
                if len(header) != 12 or header[:4] != b"glTF":
                    raise ValueError("invalid GLB header")
                version, declared_length = struct.unpack_from("<II", header, 4)
                chunk_header = source.read(8)
                if len(chunk_header) != 8:
                    raise ValueError("missing GLB JSON chunk")
                chunk_length, chunk_type = struct.unpack("<II", chunk_header)
                if version != 2 or declared_length != scene.stat().st_size or \
                        chunk_type != 0x4E4F534A or \
                        chunk_length > declared_length - 20:
                    raise ValueError("invalid GLB JSON chunk")
                document = json.loads(source.read(chunk_length))
        else:
            document = {}
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as error:
        missing.append(f"{scene_path}: {error}")
        document = {}

    for section in ("buffers", "images"):
        entries = document.get(section, [])
        if not isinstance(entries, list):
            missing.append(f"{scene_path}: malformed {section} array")
            continue
        for entry in entries:
            uri = entry.get("uri") if isinstance(entry, dict) else None
            if not isinstance(uri, str) or uri.startswith("data:"):
                continue
            parsed = urlsplit(uri)
            if parsed.scheme or parsed.netloc:
                missing.append(f"{scene_path}: non-local {section} URI {uri}")
                continue
            dependency = (scene.parent / unquote(parsed.path)).resolve()
            if not dependency.is_file():
                missing.append(f"{scene_path}: missing dependency {uri}")
            else:
                files.add(dependency)

    entries = []
    for path in sorted(files, key=lambda item: item.as_posix()):
        file_digest = file_sha256(path)
        if file_digest is None:
            missing.append(f"{path}: unreadable")
            continue
        try:
            display_path = path.relative_to(root).as_posix()
        except ValueError:
            display_path = path.as_posix()
        entries.append({
            "path": display_path,
            "sha256": file_digest,
            "bytes": path.stat().st_size,
        })
    manifest = hashlib.sha256()
    for entry in entries:
        encoded_path = entry["path"].encode("utf-8")
        manifest.update(len(encoded_path).to_bytes(8, "little"))
        manifest.update(encoded_path)
        manifest.update(bytes.fromhex(entry["sha256"]))
    return {
        "complete": not missing and len(entries) == len(files),
        "algorithm": "sha256",
        "sha256": manifest.hexdigest() if entries and not missing else None,
        "files": entries,
        "missing_inputs": missing,
    }


def comparison_identity(
        report: dict, build: dict, module: dict | None,
        assets: dict | None, validation: bool, labels: str,
        work_counters: str, timestamp_mode: str) -> dict:
    scene = report.get("scene")
    return {
        "host_platform": platform.platform(),
        "host_cpu": cpu_model(),
        "vulkan_device": report.get("vulkan_device"),
        "gpu_timestamp_metadata": report.get("gpu_timestamp_metadata"),
        "compiler_command": build.get("compiler_command"),
        "compiler_flags": build.get("compiler_flags"),
        "scenario": report.get("scenario"),
        "render_features": report.get("render_features"),
        "benchmark_name": report.get("benchmark_name"),
        "benchmark_module_sha256": module.get("sha256") if module else None,
        "scene_asset_sha256": assets.get("sha256") if assets else None,
        "scene_cache_mode": scene.get("cache_mode")
        if isinstance(scene, dict) else None,
        "render_extent": report.get("render_extent"),
        "warmup_frames": report.get("warmup_frames"),
        "measured_frames": report.get("requested_frames"),
        "validation": validation,
        "debug_labels": labels,
        "work_counters": work_counters,
        "gpu_timestamps": timestamp_mode,
    }


def comparison_identity_sha256(identity: dict) -> str:
    encoded = json.dumps(
        identity, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def compare_independent_summaries(
        current: dict, baseline: dict, metric: str,
        max_regression_pct: float | None) -> dict:
    current_contract = current.get("comparison_identity")
    baseline_contract = baseline.get("comparison_identity")
    if not isinstance(current_contract, dict) or \
            not isinstance(baseline_contract, dict):
        raise ValueError("summary is missing a comparison identity")
    current_identity = current_contract.get("sha256")
    baseline_identity = baseline_contract.get("sha256")
    if not current_identity or current_identity != baseline_identity:
        raise ValueError(
            "comparison identity mismatch: host/device/build/workload/"
            "instrumentation settings must match")
    current_runs = current.get("run_level_statistics")
    baseline_runs = baseline.get("run_level_statistics")
    if not isinstance(current_runs, dict) or not isinstance(baseline_runs, dict):
        raise ValueError("summary is missing independent-process statistics")
    current_stats = current_runs.get(metric, {})
    baseline_stats = baseline_runs.get(metric, {})
    current_values = current_stats.get("process_medians_ns")
    baseline_values = baseline_stats.get("process_medians_ns")
    for label, values in (("candidate", current_values),
                          ("baseline", baseline_values)):
        if not isinstance(values, list) or len(values) < 5 or any(
                type(value) not in (int, float) or
                not math.isfinite(value) or value <= 0 for value in values):
            raise ValueError(
                f"{label} needs at least five positive process medians "
                f"for {metric}")
    candidate_median = statistics.median(current_values)
    baseline_median = statistics.median(baseline_values)
    point_pct = (candidate_median / baseline_median - 1) * 100
    rng = random.Random(0)
    bootstrap = sorted(
        (statistics.median(rng.choices(current_values, k=len(current_values))) /
         statistics.median(rng.choices(baseline_values, k=len(baseline_values)))
         - 1) * 100
        for _ in range(10_000)
    )
    interval = [
        bootstrap[math.floor(0.025 * (len(bootstrap) - 1))],
        bootstrap[math.ceil(0.975 * (len(bootstrap) - 1))],
    ]
    status = "diagnostic_only"
    if max_regression_pct is not None:
        if interval[1] <= max_regression_pct:
            status = "pass"
        elif interval[0] > max_regression_pct:
            status = "regression"
        else:
            status = "inconclusive"
    return {
        "metric": metric,
        "status": status,
        "method": "independent-process medians; percentile bootstrap of median percent difference; 10000 resamples",
        "candidate_process_medians_ns": current_values,
        "baseline_process_medians_ns": baseline_values,
        "candidate_median_ns": candidate_median,
        "baseline_median_ns": baseline_median,
        "median_change_pct": point_pct,
        "difference_ci_95_pct": interval,
        "max_regression_pct": max_regression_pct,
        "paired": False,
        "caveat": "Independent runs do not control time-varying GPU/host state; this is not a paired test.",
    }

ANSI_SEQUENCE = re.compile(r"\x1b\[[0-9;]*m")


def terminal_width() -> int:
    return max(48, min(shutil.get_terminal_size((96, 24)).columns, 120))


def visible_width(text: str) -> int:
    return len(ANSI_SEQUENCE.sub("", text))


def glyph(preferred: str, fallback: str) -> str:
    try:
        preferred.encode(sys.stdout.encoding or "utf-8")
        return preferred
    except UnicodeEncodeError:
        return fallback


def render_cards(cards: list[tuple[str, str, str]], color_enabled: bool) -> list[str]:
    """Compact, width-aware cards for the handful of headline measurements."""
    if not cards:
        return []
    width = terminal_width()
    gap = 2
    columns = 3 if width >= 82 else 2
    card_width = (width - 4 - gap * (columns - 1)) // columns
    horizontal = glyph("─", "-")
    vertical = glyph("│", "|")
    tl, tr, bl, br = glyph("╭", "+"), glyph("╮", "+"), glyph("╰", "+"), glyph("╯", "+")
    result: list[str] = []
    for start in range(0, len(cards), columns):
        group = cards[start:start + columns]
        group_width = (width - 4 - gap * (len(group) - 1)) // len(group)
        inner = max(12, group_width - 2)
        result.append("  " + (" " * gap).join(tl + horizontal * inner + tr for _ in group))
        for row_index in range(3):
            cells = []
            for label, value, detail in group:
                raw = (label, value, detail)[row_index]
                limit = inner - 2
                plain = ANSI_SEQUENCE.sub("", raw)
                if len(plain) > limit:
                    plain = plain[:max(1, limit - 1)] + glyph("…", "~")
                    raw = plain
                code = ("1", "1;36", "2")[row_index]
                rendered = paint(raw, code, color_enabled)
                cells.append(vertical + " " + rendered +
                             " " * max(0, inner - visible_width(rendered) - 1) +
                             vertical)
            result.append("  " + (" " * gap).join(cells))
        result.append("  " + (" " * gap).join(bl + horizontal * inner + br for _ in group))
    return result


def report_terminal(summary: dict, summary_path: Path, color_enabled: bool) -> None:
    cyan, green, yellow, bold = "1;36", "1;32", "1;33", "1"
    width = terminal_width()
    rule = glyph("━", "=") * width
    scene = summary.get("scene") or {}
    gpu = summary["frames"].get("gpu_ns", {})
    wall = summary["frames"].get("total_ns", {})
    gpu_name = (summary["host"].get("gpu") or {}).get("name", "unknown GPU")
    run_name = summary.get("benchmark_name") or summary["scenario"]
    lines = [
        paint("  OBSIDIAN   /   ENGINE PERFORMANCE LAB", cyan, color_enabled),
        paint("  " + rule, cyan, color_enabled),
        f"  {paint(summary['workload']['render_mode'].upper(), green, color_enabled)}  "
        f"{run_name}  ·  {summary['workload']['runs']} run(s) × "
        f"{summary['workload']['measured_frames_per_run']} measured frames",
        f"  {summary['workload']['window_size']}  ·  {gpu_name}  ·  "
        f"{summary['host'].get('cpu') or 'unknown CPU'}",
        f"  Monad workload: {summary['workload'].get('monad_script') or 'explicit scene'}",
        "",
    ]
    startup = summary.get("startup_milestones", {})
    available_startup = [
        (name, duration_ms(metrics.get("median_ns")))
        for name, metrics in startup.items()
        if metrics.get("samples", 0) > 0
    ]
    if available_startup:
        lines.extend(["", "  STARTUP MILESTONES  · process-relative medians"])
        lines.extend(f"  {name:24} {duration}" for name, duration in available_startup)
    card_data = [
        ("FRAME WALL p50", duration_ms(wall.get("median_ns")),
         f"p95 {duration_ms(wall.get('p95_ns'))} · fence included"),
        ("GPU FRAME p50", duration_ms(gpu.get("median_ns")),
         f"p95 {duration_ms(gpu.get('p95_ns'))}"),
        ("PROCESS CPU", duration_ms(summary["process_cpu_ns"].get("median_ns")),
         "whole process · all threads"),
    ]
    lines.extend(render_cards(card_data, color_enabled))
    cv = wall.get("coefficient_of_variation_pct")
    if wall.get("samples", 0) < 100:
        lines.append(paint(
            f"  ▲ Only {wall.get('samples', 0)} frames; tail percentiles are smoke data.",
            yellow, color_enabled))
    if cv is not None and cv >= 10:
        lines.append(paint(
            f"  ▲ Frame CV {cv:.1f}%: noisy run; compare only with repeated controlled runs.",
            yellow, color_enabled))
    run_stats = summary.get("run_level_statistics", {})
    wall_runs = run_stats.get("total_ns", {})
    interval = wall_runs.get("bootstrap_median_ci_95_ns")
    interval_text = (
        f"[{duration_ms(interval[0])}, {duration_ms(interval[1])}]"
        if interval else "unavailable (requires ≥5 independent processes)"
    )
    lines.append(
        f"  Run-level wall p50 {duration_ms(wall_runs.get('median_ns'))} · "
        f"MAD {duration_ms(wall_runs.get('median_absolute_deviation_ns'))} · "
        f"95% bootstrap CI {interval_text}"
    )
    comparison = summary.get("performance_comparison")
    if comparison:
        delta_ci = comparison["difference_ci_95_pct"]
        ci_text = f"[{delta_ci[0]:+.2f}%, {delta_ci[1]:+.2f}%]"
        status_color = {
            "pass": "1;32",
            "regression": "1;31",
            "inconclusive": "1;33",
            "diagnostic_only": "1;36",
        }.get(comparison["status"], yellow)
        lines.extend([
            "",
            paint(
                f"  BASELINE COMPARISON  ·  {comparison['status'].upper()}",
                status_color, color_enabled),
            f"  {comparison['metric']} median change "
            f"{comparison['median_change_pct']:+.2f}% · independent-run "
            f"95% CI {ci_text}",
            "  Independent processes; not paired. Host/GPU drift may confound results.",
        ])
    lines.extend(["", paint("  GPU PASS BREAKDOWN", bold, color_enabled)])
    pass_metrics = summary.get("gpu_passes", {})
    stage_parents = {
        stage["name"]: stage["parent"]
        for stage in summary.get("gpu_stage_schema", [])
    }
    frame_p50 = gpu.get("median_ns") or 0
    bar_width = max(10, min(22, (width - 62) // 2))
    for stage, metric in pass_metrics.items():
        value = metric.get("median_ns")
        share = 100 * value / frame_p50 if value is not None and frame_p50 else None
        filled = (max(0, min(bar_width,
                              round(bar_width * value / frame_p50)))
                  if value is not None and frame_p50 else 0)
        bar = paint(glyph("█", "#") * filled, cyan, color_enabled) + \
              paint(glyph("░", "-") * (bar_width - filled), "2", color_enabled)
        share_text = f"{share:5.1f}%" if share is not None else "  n/a "
        prefix = "  └ " if stage_parents.get(stage) is not None else "  "
        lines.append(
            f"{prefix}{stage:<22} {duration_ms(value):>11}  {bar} "
            f"{share_text}  p95 {duration_ms(metric.get('p95_ns')):>11}"
        )
    lines.append(
        "  Scopes are inclusive and nested; pipeline overlap makes values non-additive."
    )
    unstable_stages = []
    for stage, metric in pass_metrics.items():
        median = metric.get("median_ns")
        p95 = metric.get("p95_ns")
        cv = metric.get("coefficient_of_variation_pct")
        if (metric.get("samples", 0) >= 100 and median and p95 is not None and
                p95 >= median * 3 and cv is not None and cv >= 50):
            unstable_stages.append(
                f"  {stage}: p50 {duration_ms(median)}, "
                f"p95 {duration_ms(p95)}, CV {cv:.1f}%"
            )
    if unstable_stages:
        lines.extend([
            paint("  ▲ GPU-stage variance warning; inspect raw frames/device state "
                  "(cause not inferred):", yellow, color_enabled),
            *unstable_stages,
        ])
    if scene:
        scene_upload = summary.get("scene_geometry_upload_gpu_ns")
        upload_text = (duration_ms(scene_upload.get("median_ns"))
                       if scene_upload else "n/a")
        buffers = summary["gpu_geometry_buffers"]
        vertex_utilization = (
            buffers["vertex_used_bytes"] * 100 /
            buffers["vertex_capacity_bytes"])
        index_utilization = (
            buffers["index_used_bytes"] * 100 /
            buffers["index_capacity_bytes"])
        lines.extend([
            "", paint("  WORKLOAD", bold, color_enabled),
            f"  {scene['path']}  ·  {scene['mesh_count']} meshes  ·  "
            f"{scene['texture_count']} textures",
            f"{scene['vertex_count']:,} vertices  ·  {scene['index_count']:,} indices  ·  "
            f"{scene['meshlet_count']:,} meshlets  ·  {scene['cache_mode']} OMDL cache",
            f"Renderer geometry buffers  ·  vertex "
            f"{format_bytes(buffers['vertex_used_bytes'])} / "
            f"{format_bytes(buffers['vertex_capacity_bytes'])} "
            f"({vertex_utilization:.1f}%)  ·  index "
            f"{format_bytes(buffers['index_used_bytes'])} / "
            f"{format_bytes(buffers['index_capacity_bytes'])} "
            f"({index_utilization:.1f}%)",
            f"  load {duration_ms(summary['scene_load_wall_ns'].get('median_ns'))} wall / "
            f"{duration_ms(summary['scene_load_cpu_ns'].get('median_ns'))} CPU  ·  "
            f"  upload {upload_text} GPU",
        ])
    work = summary["work_counters"]
    if work["enabled"]:
        counts = work["metrics"]
        lines.extend([
            "", paint("  CPU-RECORDED WORK", bold, color_enabled),
            "  Draw calls {draw_calls} · indirect draws {indirect_draws} · "
            "known vertices {submitted_vertices} · direct indices {submitted_indices}".format(
                **{key: value["median_count"] for key, value in counts.items()}
            ),
            "  Mesh-task workgroups {mesh_task_workgroups} · compute dispatches "
            "{compute_dispatches} · compute workgroups {compute_workgroups}".format(
                **{key: value["median_count"] for key, value in counts.items()}
            ),
            "  Input counts, not GPU-visible post-cull work; indirect index counts "
            "  are unavailable.",
        ])
        stage_work = work["by_stage"]
        active_stages = [
            (name, metrics) for name, metrics in stage_work.items()
            if name != "frame" and any(
                metrics[counter]["median_count"] not in (None, 0)
                for counter in (
                    "draw_calls", "mesh_task_workgroups",
                    "compute_dispatches"))
        ]
        if active_stages:
            lines.append("  Inclusive pass counts/frame · draw / mesh-task / dispatch:")
            lines.extend(
                f"    {name}: "
                f"{metrics['draw_calls']['median_count']} / "
                f"{metrics['mesh_task_workgroups']['median_count']} / "
                f"{metrics['compute_dispatches']['median_count']}"
                for name, metrics in active_stages
            )
    memory = summary.get("process_memory", {})
    owner_metrics = summary.get("allocation_owners", [])
    if owner_metrics:
        lines.extend(["", paint("  RESOURCE OWNERS  · constructor-attributed VkDeviceMemory",
                                bold, color_enabled)])
        for owner in owner_metrics:
            lines.append(
                f"  {owner['name']:32} live "
                f"{format_bytes(owner['live_bytes']['median_count'])} · peak "
                f"{format_bytes(owner['peak_bytes']['median_count'])}")
    peak_rss = memory.get("peak_rss_bytes", {}).get("median_count")
    final_rss = memory.get("final_rss_bytes", {}).get("median_count")
    lines.extend([
        "", paint("  PROCESS MEMORY  ·  whole process, after engine teardown",
                  bold, color_enabled),
        f"  peak RSS {format_bytes(peak_rss)}  ·  final RSS {format_bytes(final_rss)}",
    ])
    gpu_memory = summary.get("gpu_memory_budget", {})
    if gpu_memory.get("supported_all_runs"):
        lines.extend([
            "", paint("  VULKAN MEMORY  · per-process heap-use estimates; budget is dynamic",
                      bold, color_enabled),
        ])
        for heap in gpu_memory.get("heaps", []):
            usage = heap["final_usage_bytes"].get("median_count")
            budget = heap["final_budget_bytes"].get("median_count")
            lines.append(
                f"  Vulkan heap {heap['heap_index']} · current-process estimate: "
                f"{format_bytes(usage)} usage / {format_bytes(budget)} budget"
            )
    else:
        lines.extend([
            "", paint("  VULKAN MEMORY  · VK_EXT_memory_budget unavailable",
                      bold, color_enabled),
        ])
    engine_memory = summary.get("engine_vulkan_memory", {})
    lines.extend([
        "", paint("  ENGINE GPU ALLOCATIONS  · tracked VkDeviceMemory sizes",
                  bold, color_enabled),
    ])
    if engine_memory.get("sampled_all_runs"):
        for heap in engine_memory.get("heaps", []):
            live = heap["final_live_bytes"].get("median_count")
            peak = heap["peak_live_bytes"].get("median_count")
            allocations = heap["final_live_allocations"].get("median_count")
            lines.append(
                f"  Heap {heap['heap_index']} · engine live "
                f"{format_bytes(live)} / peak {format_bytes(peak)} · "
                f"{allocations} live allocations"
            )
    else:
        lines.append("  Engine allocation tracking unavailable")
    counters = summary.get("hardware_counters", {})
    event_metrics = counters.get("events", {})
    counter_text = "available" if counters.get("available") else \
                   "unavailable (kernel permission or perf provider)"
    lines.extend([
        "", paint(f"  CPU HARDWARE COUNTERS  ·  {counter_text}", bold, color_enabled),
    ])
    if counters.get("available"):
        cells = []
        for event in ("cycles", "instructions", "cache-misses"):
            metric = event_metrics.get(event, {})
            count = metric.get("median_count")
            if count is not None:
                cells.append(f"{event} {count:,}")
        lines.append("  " + "  ·  ".join(cells) +
                     "  (whole-process totals; not frame-attributed)")
    snapshots = summary.get("gpu_state_snapshots", [])
    if snapshots:
        lines.extend([
            "", paint("  GPU DEVICE STATE  ·  nvidia-smi snapshots outside timed processes",
                      bold, color_enabled),
        ])
        for sample in snapshots:
            before = format_gpu_state(sample.get("before"))
            after = format_gpu_state(sample.get("after"))
            lines.append(f"  run {sample['run']} before  {before}")
            lines.append(f"          after   {after}")
    build = summary.get("build", {})
    revision = build.get("source_revision")
    revision_label = revision[:12] if revision else "unavailable"
    revision_state = (
        "dirty" if build.get("worktree_dirty") is True else
        "clean" if build.get("worktree_dirty") is False else "unknown"
    )
    binary_hash = build.get("executable_sha256")
    lines.append(
        f"  Build provenance: {revision_label} ({revision_state}) · "
        f"binary SHA-256 {binary_hash[:12] if binary_hash else 'unavailable'}"
    )
    lines.extend([
        f"  GPU timestamp queries: "
        f"{paint('AVAILABLE' if summary['gpu_timestamp_capture_enabled_all_runs'] else 'DISABLED' if summary['gpu_timestamp_capture_mode'] == 'off' else 'UNAVAILABLE', green if summary['gpu_timestamp_capture_enabled_all_runs'] else yellow, color_enabled)}",
        f"  Vulkan command labels: "
        f"{paint('ENABLED' if summary['gpu_debug_labels_enabled_all_runs'] else 'DISABLED', green if summary['gpu_debug_labels_enabled_all_runs'] else yellow, color_enabled)}",
        f"  JSON  {summary_path}",
        f"  CSV   {summary_path.with_name('frames.csv')}",
        paint("  " + rule, cyan, color_enabled),
    ])
    print("\n".join(lines))


def format_gpu_state(state: dict | None) -> str:
    if not state:
        return "unavailable"
    temp = state.get("temperature_gpu")
    graphics = state.get("clocks_current_graphics")
    memory_clock = state.get("clocks_current_memory")
    power = state.get("power_draw")
    utilization = state.get("utilization_gpu")
    used = state.get("memory_used")
    total = state.get("memory_total")
    parts = [
        f"{temp}°C" if temp is not None else "temp n/a",
        f"{graphics}MHz core" if graphics is not None else "core n/a",
        f"{memory_clock}MHz memclk" if memory_clock is not None else "memclk n/a",
        f"{power:.1f}W" if power is not None else "power n/a",
        f"{utilization}% busy" if utilization is not None else "util n/a",
        f"{used}/{total}MiB VRAM" if used is not None and total is not None else "VRAM n/a",
    ]
    return " · ".join(parts)

def cpu_model() -> str | None:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or None


def gpu_metadata() -> dict[str, str] | None:
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,driver_version", "--format=csv,noheader"],
            check=True, capture_output=True, text=True, timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    first = result.stdout.splitlines()[0].strip() if result.stdout.splitlines() else ""
    if not first:
        return None
    name, _, driver = first.partition(",")
    return {"name": name.strip(), "driver": driver.strip()}


def gpu_state_snapshot() -> dict[str, int | float | None] | None:
    """Read device state outside the benchmark child; this is not frame telemetry."""
    fields = (
        "temperature.gpu", "clocks.current.graphics", "clocks.current.memory",
        "power.draw", "utilization.gpu", "memory.used", "memory.total",
    )
    try:
        result = subprocess.run(
            ["nvidia-smi", f"--query-gpu={','.join(fields)}",
             "--format=csv,noheader,nounits"],
            check=True, capture_output=True, text=True, timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    values = result.stdout.splitlines()[0].split(",") if result.stdout.splitlines() else []
    if len(values) != len(fields):
        return None

    parsed: dict[str, int | float | None] = {}
    for key, raw in zip(fields, values):
        value = raw.strip()
        try:
            parsed[key.replace(".", "_")] = (
                float(value) if key == "power.draw" else int(value)
            )
        except ValueError:
            parsed[key.replace(".", "_")] = None
    return parsed


def tail(path: Path, lines: int = 40) -> str:
    try:
        return "\n".join(path.read_text(errors="replace").splitlines()[-lines:])
    except OSError:
        return "<log unavailable>"

def parse_perf_stat(path: Path) -> dict[str, int | None] | None:
    """Read Linux perf's stable comma-separated stat format; never infer missing counters."""
    if not path.is_file():
        return None
    parsed: dict[str, int | None] = {}
    try:
        for line in path.read_text(errors="replace").splitlines():
            fields = line.split(",")
            if len(fields) < 3:
                continue
            event = fields[2].strip().partition(":")[0]
            if event not in {
                "cycles", "instructions", "cache-references", "cache-misses",
                "context-switches", "cpu-migrations",
            }:
                continue
            value = fields[0].strip().replace(" ", "")
            try:
                parsed[event] = int(float(value))
            except ValueError:
                parsed[event] = None
    except OSError:
        return None
    return parsed or None

def perf_counters_usable() -> bool:
    perf = shutil.which("perf")
    if not perf:
        return False
    try:
        probe = subprocess.run(
            [perf, "stat", "-e", "cycles", "--", "true"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=10, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=Path("./obsidian"))
    parser.add_argument("--build-compiler", default=os.environ.get("CC", "not-recorded"))
    parser.add_argument("--build-flags", default=os.environ.get("CFLAGS", "not-recorded"))
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup-frames", type=int, default=120)
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--timeout", type=float, default=180.0,
                        help="maximum seconds per run")
    parser.add_argument("--scene", type=Path,
                        help="load and profile this glTF scene before warm-up")
    parser.add_argument("--sun-intensity", type=float,
                        help="override the headless engine sun intensity for feature A/B runs")
    parser.add_argument("--headless", action="store_true",
                        help="render through the offscreen Vulkan target")
    parser.add_argument("--require-gpu-stage", action="append", default=[],
                        help="fail unless this GPU pass produced at least one timestamp sample")
    parser.add_argument("--gpu-timestamps", choices=("on", "off"), default="on",
                        help="enable GPU query instrumentation; off supports CPU-overhead A/B")
    parser.add_argument("--debug-labels", choices=("on", "off"), default="off",
                        help="enable VK_EXT_debug_utils command-buffer labels")
    parser.add_argument("--validation", action="store_true",
                        help="enable Khronos synchronization validation and fail on validation errors")
    parser.add_argument("--work-counters", choices=("on", "off"), default="on",
                        help="record CPU-known draw/dispatch counts; off for overhead A/B")
    parser.add_argument("--width", type=int, default=1280,
                        help="headless target width")
    parser.add_argument("--height", type=int, default=720,
                        help="headless target height")
    parser.add_argument("--scene-cache", choices=("cold", "warm"), default="cold",
                        help="disable the OMDL cache or prime and require its validated hit")
    parser.add_argument("--monad-boot", type=Path,
                        help="load this Monad module before measured benchmark frames")
    parser.add_argument("--monad-manifest", type=Path,
                        help="run an executable Monad-defined headless benchmark")
    parser.add_argument("--output", type=Path, default=Path("build/bench/engine"),
                        help="directory for raw reports, logs, and summary.json")
    parser.add_argument("--compare-to", type=Path,
                        help="compare against a compatible benchmark summary")
    parser.add_argument("--comparison-metric",
                        choices=("total_ns", "gpu_ns", "command_record_ns"),
                        default="total_ns",
                        help="per-process median metric used by --compare-to")
    parser.add_argument("--max-regression-pct", type=float,
                        help="maximum allowed upper 95%% CI regression; "
                             "requires --compare-to")
    parser.add_argument("--color", choices=("auto", "always", "never"),
                        default="auto", help="colored terminal report (default: auto)")
    args = parser.parse_args()
    if args.runs < 1 or args.frames < 1 or args.warmup_frames < 0:
        parser.error("runs/frames must be positive and warmup-frames nonnegative")
    if args.sun_intensity is not None and (
            not args.headless or not math.isfinite(args.sun_intensity) or
            args.sun_intensity < 0):
        parser.error("--sun-intensity requires --headless and a finite nonnegative value")
    if args.monad_manifest and (not args.headless or args.scene or args.monad_boot):
        parser.error("--monad-manifest requires --headless and excludes --scene/--monad-boot")
    if args.monad_manifest and not args.monad_manifest.is_file():
        parser.error(f"Monad benchmark module does not exist: {args.monad_manifest}")
    if args.scene_cache != "cold" and not args.scene and not args.monad_manifest:
        parser.error("--scene-cache requires --scene")
    if args.headless and not args.scene and not args.monad_manifest:
        parser.error("--headless requires --scene or --monad-manifest")
    if args.width < 1 or args.width > 8192 or args.height < 1 or args.height > 8192:
        parser.error("width and height must be in 1..8192")
    if args.warmup_frames + args.frames > 100000:
        parser.error("warmup plus measured frames must not exceed 100000")
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    if args.gpu_timestamps == "off" and args.require_gpu_stage:
        parser.error("--require-gpu-stage cannot be used with --gpu-timestamps off")
    if args.max_regression_pct is not None and (
            not math.isfinite(args.max_regression_pct) or
            args.max_regression_pct < 0 or not args.compare_to):
        parser.error("--max-regression-pct needs --compare-to and a finite nonnegative limit")
    return args


def main() -> int:
    args = parse_args()
    baseline_summary = None
    if args.compare_to:
        try:
            baseline_summary = json.loads(
                args.compare_to.resolve().read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"could not read comparison summary {args.compare_to}: {error}",
                  file=sys.stderr)
            return 2
        if not isinstance(baseline_summary, dict):
            print(f"comparison summary must be a JSON object: {args.compare_to}",
                  file=sys.stderr)
            return 2
    executable = args.executable.resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        print(f"benchmark executable is missing or not executable: {executable}",
              file=sys.stderr)
        return 2

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    repo_root = Path(__file__).resolve().parents[2]
    build_provenance = source_provenance(
        repo_root, args.executable.resolve(), args.build_compiler,
        args.build_flags)
    benchmark_module = None
    monad_module_path = args.monad_manifest or args.monad_boot
    if monad_module_path:
        module_path = monad_module_path.resolve()
        benchmark_module = {
            "path": module_path.relative_to(repo_root).as_posix()
            if module_path.is_relative_to(repo_root) else str(module_path),
            "sha256": file_sha256(module_path),
            "bytes": module_path.stat().st_size,
        }
        if benchmark_module["sha256"] is None:
            print(f"could not hash Monad benchmark module: {module_path}",
                  file=sys.stderr)
            return 1
    reports: list[dict] = []
    process_wall_ns: list[int] = []
    perf_reports: list[dict[str, int | None]] = []
    gpu_state_snapshots: list[dict] = []
    perf_path_command = shutil.which("perf") if perf_counters_usable() else None
    mode_args = [str(executable)]
    if args.headless:
        mode_args += ["--headless-benchmark"]
        if args.monad_manifest:
            mode_args += ["--monad-boot", str(args.monad_manifest)]
        else:
            mode_args += ["--scene", str(args.scene), "--width", str(args.width),
                          "--height", str(args.height)]
        if args.monad_boot:
            mode_args += ["--monad-boot", str(args.monad_boot)]
    if (args.scene and args.scene_cache == "warm") or args.monad_manifest:
        prepare_report = output / "scene-cache-prepare.json"
        prepare_log = output / "scene-cache-prepare.log"
        prepare_env = os.environ.copy()
        prepare_env.pop("VK_INSTANCE_LAYERS", None)
        prepare_env.pop("VK_LAYER_VALIDATE_SYNC", None)
        prepare_env.pop("VK_LAYER_ENABLES", None)
        prepare_env.update({
            "OBSIDIAN_BENCHMARK_FRAMES": "1",
            "OBSIDIAN_BENCHMARK_WARMUP": "0",
            "OBSIDIAN_BENCHMARK_REPORT": str(prepare_report),
            "OBSIDIAN_BENCH_GPU_TIMESTAMPS":
                "0" if args.gpu_timestamps == "off" else "1",
            "OBSIDIAN_VK_DEBUG_LABELS":
                "1" if args.debug_labels == "on" else "0",
            "OBSIDIAN_BENCH_WORK_COUNTERS":
                "1" if args.work_counters == "on" else "0",
        })
        if args.sun_intensity is not None:
            prepare_env["OBSIDIAN_BENCH_SUN_INTENSITY"] = format(
                args.sun_intensity, ".9g"
            )
        else:
            prepare_env.pop("OBSIDIAN_BENCH_SUN_INTENSITY", None)
        if args.validation:
            prepare_env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
            prepare_env["VK_LAYER_VALIDATE_SYNC"] = "1"
        if args.scene:
            prepare_env["OBSIDIAN_BENCH_SCENE"] = str(args.scene)
        if args.monad_manifest:
            prepare_env["OBSIDIAN_BENCHMARK_PREPARE"] = "1"
        if args.monad_boot:
            prepare_env["OBSIDIAN_MONAD_BOOT"] = str(args.monad_boot)
        prepare_env.pop("OBSIDIAN_BENCH_SCENE_CACHE", None)
        with prepare_log.open("w") as log:
            prepare_command = ([*mode_args[:2], str(prepare_report),
                                *mode_args[2:]] if args.headless else mode_args)
            prepare = subprocess.run(prepare_command, env=prepare_env, stdout=log,
                                     stderr=subprocess.STDOUT, timeout=args.timeout)
        if prepare.returncode != 0 or \
                (not args.monad_manifest and not prepare_report.is_file()):
            print(f"scene-cache preparation failed; log: {prepare_log}\n{tail(prepare_log)}",
                  file=sys.stderr)
            return 1
        prepare_errors = validation_error_lines(
            prepare_log.read_text(errors="replace"))
        if args.validation and prepare_errors:
            print(f"Vulkan validation error during scene preparation; "
                  f"log: {prepare_log}\n{tail(prepare_log)}", file=sys.stderr)
            return 1
    for run_index in range(args.runs):
        report_path = output / f"run-{run_index + 1:02d}.json"
        log_path = output / f"run-{run_index + 1:02d}.log"
        report_path.unlink(missing_ok=True)
        env = os.environ.copy()
        env.pop("VK_INSTANCE_LAYERS", None)
        env.pop("VK_LAYER_VALIDATE_SYNC", None)
        env.pop("VK_LAYER_ENABLES", None)
        env.update({
            "OBSIDIAN_BENCHMARK_FRAMES": str(args.frames),
            "OBSIDIAN_BENCHMARK_WARMUP": str(args.warmup_frames),
            "OBSIDIAN_BENCHMARK_REPORT": str(report_path),
            "OBSIDIAN_BENCH_GPU_TIMESTAMPS":
                "0" if args.gpu_timestamps == "off" else "1",
            "OBSIDIAN_VK_DEBUG_LABELS":
                "1" if args.debug_labels == "on" else "0",
            "OBSIDIAN_BENCH_WORK_COUNTERS":
                "1" if args.work_counters == "on" else "0",
        })
        if args.sun_intensity is not None:
            env["OBSIDIAN_BENCH_SUN_INTENSITY"] = format(
                args.sun_intensity, ".9g"
            )
        else:
            env.pop("OBSIDIAN_BENCH_SUN_INTENSITY", None)
        if args.validation:
            env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
            env["VK_LAYER_VALIDATE_SYNC"] = "1"
        if args.monad_boot:
            env["OBSIDIAN_MONAD_BOOT"] = str(args.monad_boot)
        else:
            env.pop("OBSIDIAN_MONAD_BOOT", None)
        if args.scene:
            env["OBSIDIAN_BENCH_SCENE"] = str(args.scene)
            if args.scene_cache == "cold":
                env["OBSIDIAN_BENCH_SCENE_CACHE"] = "off"
            else:
                env.pop("OBSIDIAN_BENCH_SCENE_CACHE", None)
        gpu_state_before = gpu_state_snapshot()
        start = time.perf_counter_ns()
        try:
            with log_path.open("w") as log:
                run_command = [*mode_args]
                if args.headless:
                    run_command.insert(2, str(report_path))
                perf_path = output / f"run-{run_index + 1:02d}.perf.csv"
                if perf_path_command:
                    run_command = [
                        perf_path_command, "stat", "-x,", "--no-big-num",
                        "-e", "cycles,instructions,cache-references,cache-misses,"
                              "context-switches,cpu-migrations",
                        "-o", str(perf_path), "--", *run_command,
                    ]
                result = subprocess.run(run_command, env=env, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            print(f"run {run_index + 1} timed out; log: {log_path}\n{tail(log_path)}",
                  file=sys.stderr)
            return 1
        elapsed = time.perf_counter_ns() - start
        process_wall_ns.append(elapsed)
        gpu_state_snapshots.append({
            "run": run_index + 1,
            "before": gpu_state_before,
            "after": gpu_state_snapshot(),
        })
        if result.returncode != 0 or not report_path.is_file():
            print(f"run {run_index + 1} failed (exit {result.returncode}); "
                  f"log: {log_path}\n{tail(log_path)}", file=sys.stderr)
            return 1
        run_errors = validation_error_lines(
            log_path.read_text(errors="replace"))
        if args.validation and run_errors:
            print(f"Vulkan validation error in run {run_index + 1}; "
                  f"log: {log_path}\n{tail(log_path)}", file=sys.stderr)
            return 1
        report = json.loads(report_path.read_text())
        report["vulkan_validation"] = {
            "enabled": args.validation,
            "layer": "VK_LAYER_KHRONOS_validation" if args.validation else None,
            "synchronization": args.validation,
            "errors": len(run_errors) if args.validation else None,
        }
        stage_schema = report.get("gpu_stage_schema")
        if not valid_gpu_stage_schema(stage_schema):
            print(f"run {run_index + 1} has an invalid GPU stage schema; "
                  f"log: {log_path}", file=sys.stderr)
            return 1
        if not valid_work_counters_by_stage(
                report.get("work_counters_by_stage"),
                args.work_counters == "on",
                [stage["name"] for stage in stage_schema],
                len(report.get("frames", []))):
            print(f"run {run_index + 1} has invalid per-stage work counters; "
                  f"log: {log_path}", file=sys.stderr)
            return 1
        if (not valid_vulkan_device(report.get("vulkan_device"))
                or not valid_process_memory(report.get("process_memory"))
                or not valid_startup_milestones(
                    report.get("startup_milestones_ns"))
                or not valid_gpu_memory_budget(
                    report.get("gpu_memory_budget"))
                or not valid_engine_vulkan_memory(
                    report.get("engine_vulkan_memory"))
                or not valid_allocation_owners(report)
                or not valid_gpu_geometry_buffers(
                    report.get("gpu_geometry_buffers"))
                or not valid_gpu_timestamp_metadata(
                    report.get("gpu_timestamp_metadata"))
                or type(report.get("gpu_debug_labels_enabled")) is not bool
                or type(report.get("work_counters_enabled")) is not bool
                or report["work_counters_enabled"] !=
                (args.work_counters == "on")
                or report["gpu_timestamps_supported"] !=
                report["gpu_timestamp_metadata"]["supported"]
                or report["gpu_timestamp_metadata"]["enabled"] !=
                (args.gpu_timestamps == "on"
                 and report["gpu_timestamp_metadata"]["supported"])):
            print(f"run {run_index + 1} has invalid Vulkan device/timestamp metadata; "
                  f"log: {log_path}", file=sys.stderr)
            return 1
        if args.headless and not report["engine_vulkan_memory"]["sampled"]:
            print(f"run {run_index + 1} lacks engine Vulkan allocation "
                  f"accounting; log: {log_path}", file=sys.stderr)
            return 1
        if not all(valid_work_counters(frame.get("work_counters"),
                                       args.work_counters == "on")
                   for frame in report.get("frames", [])):
            print(f"run {run_index + 1} has invalid work-counter data; "
                  f"log: {log_path}", file=sys.stderr)
            return 1
        if args.work_counters == "on" and not \
                consistent_work_counter_attribution(
                    report["work_counters_by_stage"],
                    report["frames"], stage_schema):
            print(f"run {run_index + 1} has inconsistent per-stage work "
                  f"attribution; log: {log_path}", file=sys.stderr)
            return 1
        if report["gpu_debug_labels_enabled"] != (args.debug_labels == "on"):
            print(f"run {run_index + 1} could not satisfy debug-label mode "
                  f"{args.debug_labels!r}; log: {log_path}", file=sys.stderr)
            return 1
        if reports:
            first_heaps = reports[0]["gpu_memory_budget"]["heaps"]
            current_heaps = report["gpu_memory_budget"]["heaps"]
            if (report["gpu_memory_budget"]["supported"] !=
                    reports[0]["gpu_memory_budget"]["supported"]
                    or len(current_heaps) != len(first_heaps)):
                print("GPU memory-budget availability changed between runs",
                      file=sys.stderr)
                return 1
            first_engine_memory = reports[0]["engine_vulkan_memory"]
            current_engine_memory = report["engine_vulkan_memory"]
            if (current_engine_memory["sampled"] !=
                    first_engine_memory["sampled"]
                    or len(current_engine_memory["heaps"]) !=
                    len(first_engine_memory["heaps"])):
                print("engine Vulkan memory-tracking availability changed "
                      "between runs", file=sys.stderr)
                return 1
        if reports and stage_schema != reports[0].get("gpu_stage_schema"):
            print("GPU stage schema changed between benchmark runs",
                  file=sys.stderr)
            return 1
        if reports and (
            report["vulkan_device"] != reports[0].get("vulkan_device")
            or report["gpu_timestamp_metadata"] !=
            reports[0].get("gpu_timestamp_metadata")
        ):
            print("Vulkan device/timestamp metadata changed between benchmark runs",
                  file=sys.stderr)
            return 1
        available_stages = {stage["name"] for stage in stage_schema}
        unknown_required_stages = set(args.require_gpu_stage) - available_stages
        if unknown_required_stages:
            print("unknown required GPU stage(s): " +
                  ", ".join(sorted(unknown_required_stages)), file=sys.stderr)
            return 2
        perf_sample = parse_perf_stat(output / f"run-{run_index + 1:02d}.perf.csv")
        if perf_sample is not None:
            perf_reports.append(perf_sample)
        expected_frames = report.get("requested_frames") if args.monad_manifest \
            else args.frames
        if report.get("captured_frames") != expected_frames:
            print(f"run {run_index + 1} captured "
                  f"{report.get('captured_frames')} of {expected_frames} measured frames; "
                  f"log: {log_path}", file=sys.stderr)
            return 1
        if report["gpu_timestamp_metadata"]["enabled"] and not any(
            frame.get("gpu_ns") is not None for frame in report.get("frames", [])
        ):
            print(f"run {run_index + 1} enabled GPU timestamps but captured "
                  f"no GPU samples; log: {log_path}", file=sys.stderr)
            return 1
        for stage in args.require_gpu_stage:
            if not any(
                frame.get("gpu_pass_ns", {}).get(stage) is not None
                for frame in report.get("frames", [])
            ):
                print(f"run {run_index + 1} did not execute required GPU stage "
                      f"{stage!r}; log: {log_path}", file=sys.stderr)
                return 1
        if args.scene or args.monad_manifest:
            scene = report.get("scene")
            geometry_buffers = report.get("gpu_geometry_buffers")
            if args.gpu_timestamps == "on" and (
                not report.get("gpu_timestamps_supported") or not any(
                    frame.get("gpu_ns") is not None
                    for frame in report.get("frames", [])
                )
            ):
                print(f"run {run_index + 1} did not produce required GPU timestamp samples; "
                      f"log: {log_path}", file=sys.stderr)
                return 1
            if (args.monad_boot or args.monad_manifest) and \
                    not report.get("monad_boot", {}).get("published"):
                print(f"run {run_index + 1} failed to publish Monad benchmark module; "
                      f"log: {log_path}", file=sys.stderr)
                return 1
            if args.monad_manifest and not report.get("benchmark_name"):
                print(f"run {run_index + 1} did not report its Monad benchmark name; "
                      f"log: {log_path}", file=sys.stderr)
                return 1
            if not scene or scene.get("mesh_count", 0) <= 0 or \
                    scene.get("vertex_count", 0) <= 0 or \
                    type(scene.get("meshlet_count")) is not int or \
                    scene["meshlet_count"] < 0:
                print(f"run {run_index + 1} loaded no measurable scene geometry; "
                      f"log: {log_path}", file=sys.stderr)
                return 1
            asset_inputs = asset_input_manifest(repo_root, scene["path"])
            report["asset_inputs"] = asset_inputs
            if not asset_inputs["complete"]:
                print(f"run {run_index + 1} has incomplete scene asset identity: "
                      f"{asset_inputs['missing_inputs']!r}; log: {log_path}",
                      file=sys.stderr)
                return 1
            report["benchmark_module"] = benchmark_module
            if not geometry_buffers["sampled"]:
                print(f"run {run_index + 1} did not report geometry buffer usage; "
                      f"log: {log_path}", file=sys.stderr)
                return 1
            if scene.get("mesh_count", 0) < report.get(
                    "manifest_min_mesh_count", 0):
                print(f"run {run_index + 1} failed its minimum mesh assertion; "
                      f"log: {log_path}", file=sys.stderr)
                return 1
            if args.gpu_timestamps == "on" and \
                    scene.get("geometry_upload_gpu_ns") is None:
                print(f"run {run_index + 1} did not capture GPU geometry upload time; "
                      f"log: {log_path}", file=sys.stderr)
                return 1
            expected_cache = args.scene_cache if args.scene else scene.get("cache_mode")
            if expected_cache not in ("cold", "warm") or \
                    scene.get("cache_mode") != expected_cache:
                print(f"run {run_index + 1} reported unexpected scene cache mode "
                      f"{scene.get('cache_mode')!r}", file=sys.stderr)
                return 1
            if expected_cache == "warm" and \
                    "[OMDL] validated cache hit:" not in log_path.read_text(errors="replace"):
                print(f"run {run_index + 1} did not use the primed validated OMDL cache; "
                      f"log: {log_path}", file=sys.stderr)
                return 1
        report["runner_provenance"] = build_provenance
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        reports.append(report)

    if args.scene or args.monad_manifest:
        scene_keys = ("path", "source_file_bytes", "mesh_count",
                      "gltf_instance_count", "texture_count",
                      "vertex_count", "index_count", "meshlet_count",
                      "cache_mode")
        expected_scene = reports[0].get("scene")
        if not expected_scene or any(
            any(report.get("scene", {}).get(key) != expected_scene.get(key)
                for key in scene_keys)
            for report in reports[1:]
        ):
            print("scene workload metadata changed between benchmark runs",
                  file=sys.stderr)
            return 1
        expected_buffers = reports[0]["gpu_geometry_buffers"]
        if any(report["gpu_geometry_buffers"] != expected_buffers
               for report in reports[1:]):
            print("geometry buffer capacities/cursors changed between runs",
                  file=sys.stderr)
            return 1
        expected_assets = reports[0].get("asset_inputs")
        if not expected_assets or any(
            report.get("asset_inputs") != expected_assets
            for report in reports[1:]
        ):
            print("scene asset hashes changed between benchmark runs",
                  file=sys.stderr)
            return 1
        expected_module = reports[0].get("benchmark_module")
        if any(report.get("benchmark_module") != expected_module
               for report in reports[1:]):
            print("Monad benchmark module identity changed between runs",
                  file=sys.stderr)
            return 1

    frame_keys = (
        "total_ns", "app_update_ns", "app_draw_ns", "event_poll_ns",
        "begin_fence_wait_ns", "swapchain_maintenance_ns", "renderer_setup_ns",
        "uniform_update_ns", "frame_capability_ns", "monad_callback_ns",
        "scene_animation_ns", "physics_ns", "camera_ns",
        "mesh_sync_ns", "camera_animation_ns", "editor_ns",
        "geometry_upload_ns", "acquire_image_ns", "image_fence_wait_ns",
        "gpu_completion_wait_ns", "gpu_ns",
        "command_record_ns", "command_render_graph_ns",
        "command_transparency_ns", "command_ui_ns",
        "queue_submit_ns", "queue_present_ns",
        "vertex2d_count", "vertex2d_data_bytes",
    )
    gpu_stage_schema = reports[0]["gpu_stage_schema"]
    work_counter_summary = {
        "enabled": args.work_counters == "on",
        "unit": "count_per_frame",
        "scope": "CPU-recorded Vulkan commands; not GPU-visible post-cull work",
        "metrics": {},
    }
    if args.work_counters == "on":
        work_counter_summary["metrics"] = {
            name: summarize_count([
                frame["work_counters"][name]
                for report in reports for frame in report["frames"]
            ])
            for name in WORK_COUNTER_NAMES
        }
        work_counter_summary["by_stage"] = {
            stage["name"]: {
                name: summarize_count([
                    report["work_counters_by_stage"]["stages"]
                    [stage["name"]][name] /
                    report["work_counters_by_stage"]["sampled_frames"]
                    for report in reports
                ])
                for name in WORK_COUNTER_NAMES
            }
            for stage in gpu_stage_schema
        }
    else:
        work_counter_summary["metrics"] = {
            name: summarize_count([]) for name in WORK_COUNTER_NAMES
        }
        work_counter_summary["by_stage"] = None
    all_frames = [frame for report in reports for frame in report["frames"]]
    frame_summary = {key: summarize([frame[key] for frame in all_frames
                                     if frame.get(key) is not None])
                     for key in frame_keys}
    gpu_stage_names = [stage["name"] for stage in gpu_stage_schema]
    gpu_memory_heap_count = len(
        reports[0]["gpu_memory_budget"]["heaps"])
    gpu_pass_summary = {
        stage: summarize([
            frame.get("gpu_pass_ns", {}).get(stage)
            for frame in all_frames
            if frame.get("gpu_pass_ns", {}).get(stage) is not None
        ])
        for stage in gpu_stage_names
    }
    run_level_statistics = {}
    for metric in ("total_ns", "gpu_ns", "command_record_ns"):
        per_process_medians = []
        for report in reports:
            samples = [frame[metric] for frame in report["frames"]
                       if frame.get(metric) is not None]
            if samples:
                per_process_medians.append(statistics.median(samples))
        run_level_statistics[metric] = summarize_independent_runs(
            per_process_medians)

    lifecycle_keys = (
        "process_to_window_ready", "window_init", "app_setup",
        "device_wait_idle", "engine_cleanup",
        "main_to_engine_shutdown_complete",
    )
    lifecycle_summary = {
        key: summarize([report["lifecycle_ns"][key] for report in reports])
        for key in lifecycle_keys
    }
    lifecycle_summary["external_process_wall"] = summarize(process_wall_ns)
    startup_summary = {
        name: summarize([
            report["startup_milestones_ns"][name]
            for report in reports
            if report["startup_milestones_ns"][name] is not None
        ])
        for name in STARTUP_MILESTONES
    }

    summary = {
        "schema_version": 1,
        "scenario": ("monad-headless-benchmark" if args.monad_manifest else
                     "headless-gltf-scene" if args.headless else
                     "gltf-scene" if args.scene else "default-editor-demo"),
        "benchmark_name": reports[0].get("benchmark_name"),
        "workload": {
            "runs": args.runs,
            "warmup_frames_per_run": (reports[0]["warmup_frames"]
                                      if args.monad_manifest
                                      else args.warmup_frames),
            "measured_frames_per_run": (reports[0]["requested_frames"]
                                        if args.monad_manifest else args.frames),
            "render_mode": "offscreen-headless" if args.headless else "window",
            "window_size": (f"{reports[0]['render_extent']['width']}x"
                            f"{reports[0]['render_extent']['height']}"
                            if args.monad_manifest else
                            f"{args.width}x{args.height}" if args.headless
                            else "800x600 default"),
            "assets": (reports[0]["scene"]["path"] if args.monad_manifest
                       else str(args.scene) if args.scene else
                       "default main.c startup assets"),
            "scene_cache_mode": (
                reports[0]["scene"]["cache_mode"] if args.monad_manifest
                else args.scene_cache if args.scene else None),
            "sun_intensity_override": args.sun_intensity,
            "monad_script": (str(args.monad_manifest) if args.monad_manifest
                             else str(args.monad_boot) if args.monad_boot else None),
            "benchmark_module": benchmark_module,
            "scene_asset_inputs": reports[0].get("asset_inputs"),
            "input_replay": False,
        },
        "host": {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu": cpu_model(),
            "gpu": gpu_metadata(),
        },
        "build": build_provenance,
        "comparison_identity": {
            "sha256": comparison_identity_sha256(comparison_identity(
                reports[0], build_provenance, benchmark_module,
                reports[0].get("asset_inputs"), args.validation,
                args.debug_labels, args.work_counters,
                args.gpu_timestamps)),
            "fields": comparison_identity(
                reports[0], build_provenance, benchmark_module,
                reports[0].get("asset_inputs"), args.validation,
                args.debug_labels, args.work_counters,
                args.gpu_timestamps),
            "note": "Configuration compatibility only; not a regression verdict.",
        },
        "gpu_state_snapshots": gpu_state_snapshots,
        "hardware_counters": {
            "provider": "Linux perf stat",
            "scope": "whole benchmark process, including initialization and teardown",
            "available": (
                perf_path_command is not None
                and len(perf_reports) == len(reports)
                and all(any(value is not None for value in sample.values())
                        for sample in perf_reports)
            ),
            "events": {
                event: summarize_count([
                    sample[event] for sample in perf_reports
                    if sample.get(event) is not None
                ])
                for event in (
                    "cycles", "instructions", "cache-references", "cache-misses",
                    "context-switches", "cpu-migrations",
                )
            },
            "note": (
                "Missing/unsupported events remain absent; counters are process-run "
                "totals, not frame-attributed samples."
            ),
        },
        "gpu_timestamps_supported_all_runs": all(
            report["gpu_timestamps_supported"] for report in reports),
        "gpu_timestamp_capture_enabled_all_runs": all(
            report["gpu_timestamp_metadata"]["enabled"] for report in reports),
        "gpu_timestamp_capture_mode": args.gpu_timestamps,
        "gpu_debug_labels_enabled_all_runs": all(
            report["gpu_debug_labels_enabled"] for report in reports),
        "gpu_debug_labels_mode": args.debug_labels,
        "vulkan_validation": {
            "enabled": args.validation,
            "layer": "VK_LAYER_KHRONOS_validation" if args.validation else None,
            "synchronization": args.validation,
            "errors": 0 if args.validation else None,
        },
        "gpu_memory_budget": {
            "scope": "current_process_estimate",
            "provider": "VK_EXT_memory_budget",
            "supported_all_runs": all(
                report["gpu_memory_budget"]["supported"]
                for report in reports),
            "heaps": [
                {
                    "heap_index": heap_index,
                    **{
                        field: summarize_count([
                            report["gpu_memory_budget"]["heaps"][heap_index][field]
                            for report in reports
                            if report["gpu_memory_budget"]["supported"]
                        ])
                        for field in (
                            "initial_usage_bytes", "final_usage_bytes",
                            "final_budget_bytes")
                    },
                }
                for heap_index in range(gpu_memory_heap_count)
            ],
        },
        "engine_vulkan_memory": {
            "scope": "live_VkDeviceMemory_allocation_sizes_by_heap",
            "sampled_all_runs": all(
                report["engine_vulkan_memory"]["sampled"]
                for report in reports),
            "heaps": [
                {
                    "heap_index": heap_index,
                    **{
                        field: summarize_count([
                            report["engine_vulkan_memory"]["heaps"][heap_index][field]
                            for report in reports
                            if report["engine_vulkan_memory"]["sampled"]
                        ])
                        for field in (
                            "initial_live_bytes", "final_live_bytes",
                            "peak_live_bytes", "initial_live_allocations",
                            "final_live_allocations", "peak_live_allocations")
                    },
                }
                for heap_index in range(
                    len(reports[0]["engine_vulkan_memory"]["heaps"]))
            ],
        },
        "allocation_owners": [
            {
                "name": name,
                **{
                    field: summarize_count([
                        next((owner[field] for owner in
                              report["allocation_owners"]["owners"]
                              if owner["name"] == name), 0)
                        for report in reports
                        if report["allocation_owners"]["sampled"]
                    ])
                    for field in ("live_bytes", "peak_bytes",
                                  "live_allocations", "peak_allocations")
                },
            }
            for name in sorted({
                owner["name"] for report in reports
                for owner in report["allocation_owners"]["owners"]
            })
        ],
        "gpu_geometry_buffers": reports[0]["gpu_geometry_buffers"],
        "work_counters": work_counter_summary,
        "gpu_stage_schema": gpu_stage_schema,
        "vulkan_device": reports[0]["vulkan_device"],
        "gpu_timestamp_metadata": reports[0]["gpu_timestamp_metadata"],
        "lifecycle": lifecycle_summary,
        "startup_milestones": startup_summary,
        "process_cpu_ns": summarize([
            report["process_cpu_ns"] for report in reports
        ]),
        "process_memory": {
            "scope": "whole_process",
            "peak_source": "getrusage.ru_maxrss",
            "final_source": "/proc/self/statm",
            "peak_rss_bytes": summarize_count([
                report["process_memory"]["peak_rss_bytes"]
                for report in reports
                if report["process_memory"]["peak_rss_bytes"] is not None
            ]),
            "final_rss_bytes": summarize_count([
                report["process_memory"]["final_rss_bytes"]
                for report in reports
                if report["process_memory"]["final_rss_bytes"] is not None
            ]),
        },
        "frames": frame_summary,
        "run_level_statistics": run_level_statistics,
        "gpu_passes": gpu_pass_summary,
        "scene": reports[0].get("scene"),
        "scene_load_wall_ns": summarize([
            report["scene"]["load_wall_ns"] for report in reports
        ]) if args.scene or args.monad_manifest else None,
        "scene_load_cpu_ns": summarize([
            report["scene"]["load_cpu_ns"] for report in reports
        ]) if args.scene or args.monad_manifest else None,
        "scene_geometry_upload_gpu_ns": summarize([
            report["scene"]["geometry_upload_gpu_ns"] for report in reports
            if report["scene"]["geometry_upload_gpu_ns"] is not None
        ]) if (args.scene or args.monad_manifest) and
              args.gpu_timestamps == "on" else None,
        "raw_reports": [f"run-{i + 1:02d}.json" for i in range(args.runs)],
        "logs": [f"run-{i + 1:02d}.log" for i in range(args.runs)],
    }
    comparison_exit = 0
    if baseline_summary is not None:
        try:
            comparison = compare_independent_summaries(
                summary, baseline_summary, args.comparison_metric,
                args.max_regression_pct)
        except ValueError as error:
            print(f"benchmark comparison rejected: {error}", file=sys.stderr)
            return 2
        summary["performance_comparison"] = comparison
        if comparison["status"] == "regression":
            comparison_exit = 1
        elif comparison["status"] == "inconclusive":
            comparison_exit = 2
    summary_path = output / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n")
    csv_path = output / "frames.csv"
    gpu_csv_keys = tuple(f"gpu_{stage}_ns" for stage in gpu_stage_names)
    work_csv_keys = tuple(f"work_{name}" for name in WORK_COUNTER_NAMES)
    with csv_path.open("w", newline="") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=("run", "sequence", *frame_keys, *work_csv_keys,
                                *gpu_csv_keys))
        writer.writeheader()
        for run_index, report in enumerate(reports, start=1):
            for frame in report["frames"]:
                writer.writerow({
                    "run": run_index,
                    "sequence": frame["sequence"],
                    **{key: frame.get(key) for key in frame_keys},
                    **{
                        f"work_{name}":
                            frame.get("work_counters", {}).get(name)
                        for name in WORK_COUNTER_NAMES
                    },
                    **{
                        f"gpu_{stage}_ns":
                            frame.get("gpu_pass_ns", {}).get(stage)
                        for stage in gpu_stage_names
                    },
                })
    report_terminal(summary, summary_path, use_color(args.color))
    return comparison_exit


if __name__ == "__main__":
    raise SystemExit(main())
