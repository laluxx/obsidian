#!/usr/bin/env python3
"""GPU stage-schema invariants for the headless benchmark report contract."""
import importlib.util
import contextlib
import io
import json
from pathlib import Path
import sys
import struct
import tempfile

spec = importlib.util.spec_from_file_location(
    "engine_bench", Path(__file__).resolve().parents[2] / "tools/bench/engine_bench.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
paired_spec = importlib.util.spec_from_file_location(
    "startup_compare", Path(__file__).resolve().parents[2] /
    "tools/bench/startup_compare.py")
paired_module = importlib.util.module_from_spec(paired_spec)
paired_spec.loader.exec_module(paired_module)
# Large paired host drift must cancel instead of producing an unpaired CI.
paired = paired_module.paired_statistics(
    [100, 1000, 10000, 100000, 1000000],
    [110, 1010, 10010, 100010, 1000010])
assert paired["paired_median_difference_ns"] == 10
assert paired["paired_difference_ci_95_ns"] == [10, 10]
try:
    paired_module.paired_statistics([1] * 5, [2] * 4)
except ValueError:
    pass
else:
    raise AssertionError("unequal pair counts accepted")

schema = [
    {"id": 0, "name": "shadows", "parent": None,
     "unit": "ns", "timing": "inclusive"},
    {"id": 1, "name": "shadow.cascade.0", "parent": "shadows",
     "unit": "ns", "timing": "inclusive"},
    {"id": 2, "name": "hzb.mip.15", "parent": "hzb",
     "unit": "ns", "timing": "inclusive"},
    {"id": 3, "name": "hzb", "parent": None,
     "unit": "ns", "timing": "inclusive"},
]
assert module.valid_gpu_stage_schema(schema)
assert module.valid_vulkan_device({
    "name": "Test GPU", "vendor_id": 1, "device_id": 2,
    "driver_version": 3, "api_version": 4})
assert not module.valid_vulkan_device({"name": "Test GPU"})
owner_report = {
    "allocation_owners": {
        "sampled": True, "scope": "VkDeviceMemory_constructor_ownership",
        "owners": [{"name": "geometry", "live_bytes": 100,
                    "peak_bytes": 200, "live_allocations": 1,
                    "peak_allocations": 2}]},
    "engine_vulkan_memory": {
        "sampled": True,
        "heaps": [{"final_live_bytes": 100, "final_live_allocations": 1}]},
}
assert module.valid_allocation_owners(owner_report)
assert not module.valid_allocation_owners({
    **owner_report, "engine_vulkan_memory": {
        "sampled": True,
        "heaps": [{"final_live_bytes": 99, "final_live_allocations": 1}]}})
startup_milestones = {
    "process_entry": 0, "glfw_init": 5, "glfw_window": 10,
    "input_ready": 11, "editor_font_ready": 12, "audio_ready": 13,
    "vulkan_instance": 20, "physical_device": 21, "logical_device": 40,
    "swapchain": 50, "renderer_ready": 60, "monad_runtime_ready": 61,
    "editor_ready": 62, "monad_boot_ready": 63, "first_submit": 70,
    "first_present": 80,
};
assert module.valid_startup_milestones(startup_milestones)
assert module.valid_startup_milestones({
    **startup_milestones, "glfw_window": None, "audio_ready": None})
assert not module.valid_startup_milestones({
    **startup_milestones, "logical_device": 19})
assert not module.valid_startup_milestones({
    **startup_milestones, "process_entry": 1})
assert module.valid_process_memory({
    "scope": "whole_process", "peak_source": "getrusage.ru_maxrss",
    "final_source": "/proc/self/statm", "peak_rss_bytes": 1234,
    "final_rss_bytes": None})
assert not module.valid_process_memory({
    "scope": "per_frame", "peak_source": "getrusage.ru_maxrss",
    "final_source": "/proc/self/statm", "peak_rss_bytes": 1234,
    "final_rss_bytes": 1000})
assert not module.valid_process_memory({
    "scope": "whole_process", "peak_source": "getrusage.ru_maxrss",
    "final_source": "/proc/self/statm", "peak_rss_bytes": -1,
    "final_rss_bytes": 1000})
assert module.valid_gpu_timestamp_metadata({
    "supported": True, "enabled": True, "queue_family_index": 0,
    "valid_bits": 36, "period_ns": 1.0})
assert module.valid_gpu_timestamp_metadata({
    "supported": True, "enabled": False, "queue_family_index": 0,
    "valid_bits": 36, "period_ns": 1.0})
assert module.valid_gpu_timestamp_metadata({
    "supported": False, "enabled": False, "queue_family_index": 0,
    "valid_bits": 0, "period_ns": 1.0})
assert not module.valid_gpu_timestamp_metadata({
    "supported": True, "enabled": True, "queue_family_index": 0,
    "valid_bits": 0, "period_ns": 1.0})
assert not module.valid_gpu_timestamp_metadata({
    "supported": True, "enabled": True, "queue_family_index": 0,
    "valid_bits": 36, "period_ns": float("nan")})
assert module.valid_gpu_memory_budget({
    "scope": "current_process_estimate", "provider": "VK_EXT_memory_budget",
    "supported": False, "heaps": []})
assert module.valid_gpu_memory_budget({
    "scope": "current_process_estimate", "provider": "VK_EXT_memory_budget",
    "supported": True, "heaps": [{
        "heap_index": 0, "initial_usage_bytes": 10,
        "final_usage_bytes": 20, "final_budget_bytes": 30}]})
assert not module.valid_gpu_memory_budget({
    "scope": "device_global", "provider": "VK_EXT_memory_budget",
    "supported": True, "heaps": []})
assert module.valid_engine_vulkan_memory({
    "scope": "live_VkDeviceMemory_allocation_sizes_by_heap",
    "sampled": False, "heaps": []})
assert module.valid_engine_vulkan_memory({
    "scope": "live_VkDeviceMemory_allocation_sizes_by_heap",
    "sampled": True, "heaps": [{
        "heap_index": 0, "initial_live_bytes": 100,
        "final_live_bytes": 200, "peak_live_bytes": 250,
        "initial_live_allocations": 2, "final_live_allocations": 3,
        "peak_live_allocations": 4}]})
assert not module.valid_engine_vulkan_memory({
    "scope": "live_VkDeviceMemory_allocation_sizes_by_heap",
    "sampled": True, "heaps": [{
        "heap_index": 0, "initial_live_bytes": 100,
        "final_live_bytes": 200, "peak_live_bytes": 199,
        "initial_live_allocations": 2, "final_live_allocations": 3,
        "peak_live_allocations": 4}]})
identity = {"device": 42, "asset_sha256": "a" * 64}
identity_hash = module.comparison_identity_sha256(identity)
assert identity_hash == module.comparison_identity_sha256(
    {"asset_sha256": "a" * 64, "device": 42})
assert identity_hash != module.comparison_identity_sha256(
    {"device": 42, "asset_sha256": "b" * 64})

def comparison_fixture(values):
    return {
        "comparison_identity": {"sha256": "same-config"},
        "run_level_statistics": {
            "total_ns": {"process_medians_ns": values},
        },
    }

baseline = comparison_fixture([1000] * 5)
same_run = module.compare_independent_summaries(
    comparison_fixture([1000] * 5), baseline, "total_ns", 0.0)
assert same_run["status"] == "pass"
assert same_run["difference_ci_95_pct"] == [0.0, 0.0]
regression = module.compare_independent_summaries(
    comparison_fixture([1200] * 5), baseline, "total_ns", 10.0)
assert regression["status"] == "regression"
inconclusive = module.compare_independent_summaries(
    comparison_fixture([900, 1000, 1000, 1000, 1100]),
    baseline, "total_ns", 5.0)
assert inconclusive["status"] == "inconclusive"
assert not regression["paired"]
try:
    module.compare_independent_summaries(
        {"comparison_identity": {"sha256": "different"}},
        baseline, "total_ns", None)
except ValueError:
    pass
else:
    raise AssertionError("incompatible comparison was accepted")
try:
    module.compare_independent_summaries(
        comparison_fixture([1000] * 5), {}, "total_ns", None)
except ValueError:
    pass
else:
    raise AssertionError("summary without identity was accepted")
try:
    module.compare_independent_summaries(
        comparison_fixture([1000] * 4), baseline, "total_ns", None)
except ValueError:
    pass
else:
    raise AssertionError("four-run comparison was accepted")
assert not module.valid_engine_vulkan_memory({
    "scope": "live_VkDeviceMemory_allocation_sizes_by_heap",
    "sampled": True, "heaps": [{
        "heap_index": 1, "initial_live_bytes": 100,
        "final_live_bytes": 200, "peak_live_bytes": 250,
        "initial_live_allocations": 2, "final_live_allocations": 3,
        "peak_live_allocations": 4}]})
assert module.valid_gpu_geometry_buffers({
    "scope": "logical_buffer_size_and_write_cursor", "sampled": True,
    "vertex_capacity_bytes": 4096, "vertex_used_bytes": 1024,
    "index_capacity_bytes": 2048, "index_used_bytes": 512})
assert module.valid_gpu_geometry_buffers({
    "scope": "logical_buffer_size_and_write_cursor", "sampled": False,
    "vertex_capacity_bytes": None, "vertex_used_bytes": None,
    "index_capacity_bytes": None, "index_used_bytes": None})
assert not module.valid_gpu_geometry_buffers({
    "scope": "logical_buffer_size_and_write_cursor", "sampled": True,
    "vertex_capacity_bytes": 10, "vertex_used_bytes": 11,
    "index_capacity_bytes": 10, "index_used_bytes": 5})
assert module.validation_error_lines(
    "Vulkan command labels enabled\nNo errors") == []
assert module.validation_error_lines(
    "Validation Error: [VUID-a] first\n"
    "Validation Error: [VUID-b] second") == [
        "Validation Error: [VUID-a] first",
        "Validation Error: [VUID-b] second",
    ]
assert module.valid_work_counters({
    "sampled": True, "draw_calls": 1, "indirect_draws": 0,
    "submitted_vertices": 3, "submitted_indices": 0,
    "mesh_task_workgroups": 4, "compute_dispatches": 2,
    "compute_workgroups": 16}, True)
assert module.valid_work_counters({
    "sampled": False, "draw_calls": None, "indirect_draws": None,
    "submitted_vertices": None, "submitted_indices": None,
    "mesh_task_workgroups": None, "compute_dispatches": None,
    "compute_workgroups": None}, False)
assert not module.valid_work_counters({
    "sampled": True, "draw_calls": -1, "indirect_draws": 0,
    "submitted_vertices": 3, "submitted_indices": 0,
    "mesh_task_workgroups": 4, "compute_dispatches": 2,
    "compute_workgroups": 16}, True)
stage_counts = {
    name: 1 for name in module.WORK_COUNTER_NAMES
}
assert module.valid_work_counters_by_stage({
    "sampled_frames": 3,
    "unit": "total_commands_over_measured_frames",
    "stages": {"frame": stage_counts, "main": stage_counts},
}, True, ["frame", "main"], 3)
assert module.valid_work_counters_by_stage(
    None, False, ["frame", "main"], 3)
assert not module.valid_work_counters_by_stage(
    {"sampled_frames": 2,
     "unit": "total_commands_over_measured_frames",
     "stages": {"frame": stage_counts, "main": stage_counts}},
    True, ["frame", "main"], 3)
assert not module.valid_work_counters_by_stage(
    {"sampled_frames": 3,
     "unit": "total_commands_over_measured_frames",
     "stages": {"frame": stage_counts}},
    True, ["frame", "main"], 3)
def zero_counts():
    return {name: 0 for name in module.WORK_COUNTER_NAMES}


frame_work = zero_counts()
frame_work["draw_calls"] = 1
stage_work = {
    name: zero_counts()
    for name in ("frame", "shadows", "shadow.cascade.0", "hzb", "hzb.mip.0")
}
stage_work["frame"]["draw_calls"] = 1
stage_work["shadows"]["draw_calls"] = 1
stage_work["shadow.cascade.0"]["draw_calls"] = 1
work_stages = [{"name": "frame", "parent": None},
               {"name": "shadows", "parent": None},
               {"name": "shadow.cascade.0", "parent": "shadows"},
               {"name": "hzb", "parent": None},
               {"name": "hzb.mip.0", "parent": "hzb"}]
assert module.consistent_work_counter_attribution(
    {"stages": stage_work},
    [{"work_counters": {"sampled": True, **frame_work}}],
    work_stages)
stage_work["shadows"]["draw_calls"] = 2
assert not module.consistent_work_counter_attribution(
    {"stages": stage_work},
    [{"work_counters": {"sampled": True, **frame_work}}],
    work_stages)
stage_work["shadows"]["draw_calls"] = 1
frame_work["draw_calls"] = 2
assert not module.consistent_work_counter_attribution(
    {"stages": stage_work},
    [{"work_counters": {"sampled": True, **frame_work}}],
    work_stages)
assert not module.valid_gpu_stage_schema([])
assert not module.valid_gpu_stage_schema({"stages": schema})

invalid = [dict(schema[0]), dict(schema[1]), dict(schema[2]), dict(schema[3])]
invalid[1]["id"] = 0
assert not module.valid_gpu_stage_schema(invalid)  # duplicate/non-contiguous IDs
invalid = [dict(stage) for stage in schema]
invalid[1]["name"] = "shadows"
assert not module.valid_gpu_stage_schema(invalid)  # duplicate key
invalid = [dict(stage) for stage in schema]
invalid[1]["parent"] = "missing"
assert not module.valid_gpu_stage_schema(invalid)  # dangling parent
invalid = [dict(stage) for stage in schema]
invalid[1]["unit"] = "ticks"
assert not module.valid_gpu_stage_schema(invalid)  # ambiguous units
invalid = [dict(stage) for stage in schema]
invalid[1]["timing"] = "exclusive"
assert not module.valid_gpu_stage_schema(invalid)  # contract requires inclusive scopes

few_runs = module.summarize_independent_runs([10, 20, 30, 40])
assert few_runs["independent_processes"] == 4
assert few_runs["process_medians_ns"] == [10, 20, 30, 40]
assert few_runs["median_ns"] == 25.0
assert few_runs["median_absolute_deviation_ns"] == 10.0
assert few_runs["bootstrap_median_ci_95_ns"] is None
replicated = module.summarize_independent_runs([10, 20, 30, 40, 50])
assert replicated["independent_processes"] == 5
interval = replicated["bootstrap_median_ci_95_ns"]
assert interval == module.summarize_independent_runs(
    [10, 20, 30, 40, 50])["bootstrap_median_ci_95_ns"]
assert interval[0] <= replicated["median_ns"] <= interval[1]
provenance = module.source_provenance(
    Path(__file__).resolve().parents[2],
    Path(module.__file__).resolve(),
    "cc", "-O2",
)
assert provenance["source_revision"] is None or isinstance(
    provenance["source_revision"], str)
assert provenance["worktree_dirty"] is None or type(
    provenance["worktree_dirty"]) is bool
assert len(provenance["executable_sha256"]) == 64
assert provenance["compiler_command"] == "cc"
assert len(provenance["benchmark_source_sha256"]) == 64
assert provenance["benchmark_source_file_count"] > 0

with tempfile.TemporaryDirectory() as temporary:
    asset_root = Path(temporary)
    scene_dir = asset_root / "assets"
    scene_dir.mkdir()
    (scene_dir / "mesh.bin").write_bytes(b"mesh-a")
    (scene_dir / "texture map.png").write_bytes(b"texture")
    scene_file = scene_dir / "scene.gltf"
    scene_file.write_text(json.dumps({
        "buffers": [{"uri": "mesh.bin", "byteLength": 6},
                    {"uri": "data:application/octet-stream;base64,AAAA",
                     "byteLength": 3}],
        "images": [{"uri": "texture%20map.png"}],
    }), encoding="utf-8")
    assets = module.asset_input_manifest(asset_root, "assets/scene.gltf")
    assert assets["complete"]
    assert len(assets["files"]) == 3
    first_asset_hash = assets["sha256"]
    (scene_dir / "mesh.bin").write_bytes(b"mesh-b")
    changed_assets = module.asset_input_manifest(
        asset_root, "assets/scene.gltf")
    assert changed_assets["complete"]
    assert changed_assets["sha256"] != first_asset_hash
    (scene_dir / "texture map.png").unlink()
    missing_asset = module.asset_input_manifest(
        asset_root, "assets/scene.gltf")
    assert not missing_asset["complete"]
    assert missing_asset["sha256"] is None
    (scene_dir / "texture map.png").write_bytes(b"texture")
    json_chunk = json.dumps({
        "buffers": [{"uri": "mesh.bin", "byteLength": 6}],
        "images": [],
    }).encode("utf-8")
    json_chunk += b" " * (-len(json_chunk) % 4)
    binary_chunk = b"BIN!"
    glb_length = 12 + 8 + len(json_chunk) + 8 + len(binary_chunk)
    glb_file = scene_dir / "scene.glb"
    glb_file.write_bytes(
        b"glTF" + struct.pack("<II", 2, glb_length)
        + struct.pack("<II", len(json_chunk), 0x4E4F534A) + json_chunk
        + struct.pack("<II", len(binary_chunk), 0x004E4942) + binary_chunk
    )
    glb_assets = module.asset_input_manifest(asset_root, "assets/scene.glb")
    assert glb_assets["complete"]
    assert len(glb_assets["files"]) == 2

original_argv = sys.argv
try:
    sys.argv = ["engine_bench.py"]
    assert module.parse_args().debug_labels == "off"
    assert module.parse_args().work_counters == "on"
    sys.argv = ["engine_bench.py", "--require-gpu-stage", "hzb.mip.15"]
    assert module.parse_args().require_gpu_stage == ["hzb.mip.15"]
finally:
    sys.argv = original_argv

try:
    sys.argv = ["engine_bench.py", "--gpu-timestamps", "off"]
    assert module.parse_args().gpu_timestamps == "off"
    sys.argv = ["engine_bench.py", "--debug-labels", "on"]
    assert module.parse_args().debug_labels == "on"
    sys.argv = ["engine_bench.py", "--work-counters", "off"]
    assert module.parse_args().work_counters == "off"
    sys.argv = ["engine_bench.py", "--gpu-timestamps", "off",
                "--require-gpu-stage", "frame"]
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            module.parse_args()
            raise AssertionError("timestamp-disabled stage requirement accepted")
        except SystemExit as error:
            assert error.code == 2
finally:
    sys.argv = original_argv

print("engine benchmark contract: GPU schema, metadata and instrumentation modes PASS")
