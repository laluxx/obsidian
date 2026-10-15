#include "engine_bench.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(obs_engine_bench_cpu_now_ns() > 0);
    ObsEngineBench bench = {0};
    assert(obs_engine_bench_init(&bench, 2, 3, "/tmp/obsidian-engine-bench.json"));
    obs_engine_bench_profile_activate(&bench);
    assert(bench.startup_milestone_ns[OBS_STARTUP_PROCESS_ENTRY] ==
           bench.process_start_ns);
    obs_engine_bench_profile_mark(OBS_STARTUP_RENDERER_READY);
    const uint64_t ready = bench.startup_milestone_ns[OBS_STARTUP_RENDERER_READY];
    assert(ready >= bench.process_start_ns);
    obs_engine_bench_profile_mark(OBS_STARTUP_RENDERER_READY);
    assert(bench.startup_milestone_ns[OBS_STARTUP_RENDERER_READY] == ready);
    obs_engine_bench_profile_mark(OBS_STARTUP_FIRST_SUBMIT);
    assert(bench.startup_milestone_ns[OBS_STARTUP_FIRST_SUBMIT] >= ready);
    obs_engine_bench_profile_activate(NULL);
    obs_engine_bench_profile_mark(OBS_STARTUP_FIRST_PRESENT);
    assert(bench.startup_milestone_ns[OBS_STARTUP_FIRST_PRESENT] == 0);
    assert(!obs_engine_bench_done(&bench));
    snprintf(bench.gpu_device_name, sizeof(bench.gpu_device_name), "%s",
             "Unit Test GPU");
    bench.gpu_timestamps_supported = true;
    bench.gpu_timestamp_capture_enabled = true;
    bench.gpu_debug_labels_enabled = true;
    bench.work_counters_enabled = true;
    bench.gpu_memory_budget_supported = true;
    bench.gpu_memory_heap_count = 1;
    bench.gpu_memory_initial_usage[0] = 1000;
    bench.gpu_memory_final_usage[0] = 2000;
    bench.gpu_memory_final_budget[0] = 4096;
    bench.work_draw_calls_by_stage[OBS_GPU_PROFILE_MAIN_FORWARD] = 51;
    bench.work_mesh_task_workgroups_by_stage[OBS_GPU_PROFILE_MAIN_FORWARD] = 27;
    bench.gpu_vendor_id = 4318;
    bench.gpu_device_id = 9999;
    bench.gpu_driver_version = 42;
    bench.gpu_api_version = 1;
    bench.gpu_queue_family = 2;
    bench.gpu_timestamp_valid_bits = 36;
    bench.gpu_timestamp_period_ns = 1.5;
    bench.process_peak_rss_sampled = true;
    bench.process_peak_rss_bytes = 123456;
    bench.process_final_rss_sampled = true;
    bench.process_final_rss_bytes = 65432;
    bench.scene_loaded = true;
    bench.scene_mesh_count = 10;
    bench.scene_vertex_count = 1000;
    bench.scene_index_count = 3000;
    bench.scene_meshlet_count = 125;
    bench.gpu_geometry_buffers_sampled = true;
    bench.gpu_vertex_buffer_capacity_bytes = 4096;
    bench.gpu_vertex_buffer_used_bytes = 1024;
    bench.gpu_index_buffer_capacity_bytes = 2048;
    bench.gpu_index_buffer_used_bytes = 512;

    ObsEngineBenchFrame frame = {
        .total_ns = 100,
        .command_render_graph_ns = 40,
        .command_transparency_ns = 12,
        .command_ui_ns = 4,
        .work_counters_sampled = true,
        .draw_calls = 17,
        .indirect_draws = 3,
        .submitted_vertices = 256,
        .submitted_indices = 512,
        .mesh_task_workgroups = 9,
        .compute_dispatches = 4,
        .compute_workgroups = 300,
    };
    obs_engine_bench_record_frame(&bench, 0, &frame);
    obs_engine_bench_record_frame(&bench, 1, &frame);
    assert(bench.captured_frames == 0);
    assert(!obs_engine_bench_done(&bench));

    for (uint64_t sequence = 2; sequence < 5; sequence++) {
        frame.total_ns = sequence * 100;
        obs_engine_bench_record_frame(&bench, sequence, &frame);
        ObsGpuFrameProfile gpu = {
            .gpu_ns = sequence * 10,
            .stage_ns = {[OBS_GPU_PROFILE_FRAME] = sequence * 10,
                         [OBS_GPU_PROFILE_SHADOWS] = sequence * 3,
                         [OBS_GPU_PROFILE_SHADOW_CASCADE_0] = sequence * 2},
            .stage_mask = (UINT32_C(1) << OBS_GPU_PROFILE_FRAME) |
                          (UINT32_C(1) << OBS_GPU_PROFILE_SHADOWS) |
                          (UINT32_C(1) << OBS_GPU_PROFILE_SHADOW_CASCADE_0)
        };
        obs_engine_bench_record_gpu(&bench, sequence, &gpu);
    }
    assert(obs_engine_bench_done(&bench));
    assert(bench.captured_frames == 3);
    assert(bench.frames[0].total_ns == 200);
    assert(bench.frames[2].total_ns == 400);
    assert(bench.frames[0].gpu_sampled && bench.frames[0].gpu_ns == 20);
    assert(bench.frames[2].gpu_sampled && bench.frames[2].gpu_ns == 40);
    assert(bench.frames[0].gpu_stage_ns[OBS_GPU_PROFILE_SHADOWS] == 6);
    assert(bench.frames[0].gpu_stage_mask ==
           ((UINT32_C(1) << OBS_GPU_PROFILE_FRAME) |
            (UINT32_C(1) << OBS_GPU_PROFILE_SHADOWS) |
            (UINT32_C(1) << OBS_GPU_PROFILE_SHADOW_CASCADE_0)));
    assert(obs_engine_bench_write(&bench));
    FILE *report = fopen("/tmp/obsidian-engine-bench.json", "r");
    assert(report);
    char contents[32768];
    size_t size = fread(contents, 1, sizeof(contents) - 1, report);
    contents[size] = '\0';
    fclose(report);
    assert(strstr(contents, "\"gpu_pass_ns\"") != NULL);
    assert(strstr(contents, "\"work_counters_enabled\": true") != NULL);
    assert(strstr(contents,
        "\"work_counters\": {\"sampled\": true, \"draw_calls\": 17, "
        "\"indirect_draws\": 3, \"submitted_vertices\": 256, "
        "\"submitted_indices\": 512, \"mesh_task_workgroups\": 9, "
        "\"compute_dispatches\": 4, \"compute_workgroups\": 300}") != NULL);
    assert(strstr(contents,
        "\"work_counters_by_stage\": {\"sampled_frames\": 3, "
        "\"unit\": \"total_commands_over_measured_frames\", \"stages\": {")
        != NULL);
    assert(strstr(contents,
        "\"main_forward\": {\"draw_calls\": 51, \"indirect_draws\": 0, "
        "\"submitted_vertices\": 0, \"submitted_indices\": 0, "
        "\"mesh_task_workgroups\": 27, \"compute_dispatches\": 0, "
        "\"compute_workgroups\": 0}") != NULL);
    assert(strstr(contents,
        "\"gpu_geometry_buffers\": {\"scope\": "
        "\"logical_buffer_size_and_write_cursor\", \"sampled\": true, "
        "\"vertex_capacity_bytes\": 4096, \"vertex_used_bytes\": 1024, "
        "\"index_capacity_bytes\": 2048, \"index_used_bytes\": 512}")
        != NULL);
    assert(strstr(contents,
                  "\"vertex_count\": 1000, \"index_count\": 3000, "
                  "\"meshlet_count\": 125") != NULL);
    assert(strstr(contents, "\"command_render_graph_ns\": 40") != NULL);
    assert(strstr(contents, "\"command_transparency_ns\": 12") != NULL);
    assert(strstr(contents, "\"command_ui_ns\": 4") != NULL);
    assert(strstr(contents,
                  "\"name\": \"Unit Test GPU\", \"vendor_id\": 4318") != NULL);
    assert(strstr(contents,
                  "\"supported\": true, \"queue_family_index\": 2, "
                  "\"valid_bits\": 36, \"period_ns\": 1.5") != NULL);
    assert(strstr(contents, "\"enabled\": true") != NULL);
    assert(strstr(contents, "\"gpu_debug_labels_enabled\": true") != NULL);
    assert(strstr(contents,
        "\"gpu_memory_budget\": {\"scope\": \"current_process_estimate\", "
        "\"provider\": \"VK_EXT_memory_budget\", \"supported\": true, "
        "\"heaps\": [{\"heap_index\": 0, \"initial_usage_bytes\": 1000, "
        "\"final_usage_bytes\": 2000, \"final_budget_bytes\": 4096}]}")
        != NULL);
    assert(strstr(contents,
                  "\"process_memory\": {\"scope\": \"whole_process\", "
                  "\"peak_source\": \"getrusage.ru_maxrss\", "
                  "\"final_source\": \"/proc/self/statm\", "
                  "\"peak_rss_bytes\": 123456, \"final_rss_bytes\": 65432}")
           != NULL);
    assert(strstr(contents, "\"shadows\": 6") != NULL);
    assert(strstr(contents, "\"hzb.mip.15\": null") != NULL);
    assert(strstr(contents,
                  "\"name\": \"shadow.cascade.0\", \"parent\": \"shadows\"")
           != NULL);
    assert(strstr(contents, "\"shadow.cascade.0\": 4") != NULL);
    assert(strstr(contents,
                  "\"name\": \"hzb.mip.15\", \"parent\": \"hzb\"") != NULL);

    bench.gpu_memory_budget_supported = false;
    bench.gpu_memory_heap_count = 0;
    bench.scene_loaded = false;
    bench.gpu_geometry_buffers_sampled = false;
    bench.work_counters_enabled = false;
    for (uint32_t i = 0; i < bench.requested_frames; ++i)
        bench.frames[i].work_counters_sampled = false;
    snprintf(bench.output_path, sizeof(bench.output_path), "%s",
             "/tmp/obsidian-engine-bench-no-gpu-budget.json");
    assert(obs_engine_bench_write(&bench));
    report = fopen(bench.output_path, "r");
    assert(report);
    size = fread(contents, 1, sizeof(contents) - 1, report);
    contents[size] = '\0';
    fclose(report);
    assert(strstr(contents,
        "\"gpu_geometry_buffers\": {\"scope\": "
        "\"logical_buffer_size_and_write_cursor\", \"sampled\": false, "
        "\"vertex_capacity_bytes\": null, \"vertex_used_bytes\": null, "
        "\"index_capacity_bytes\": null, \"index_used_bytes\": null}")
        != NULL);
    assert(strstr(contents, "\"work_counters_enabled\": false") != NULL);
    assert(strstr(contents, "\"work_counters_by_stage\": null") != NULL);
    assert(strstr(contents,
        "\"work_counters\": {\"sampled\": false, \"draw_calls\": null, "
        "\"indirect_draws\": null, \"submitted_vertices\": null, "
        "\"submitted_indices\": null, \"mesh_task_workgroups\": null, "
        "\"compute_dispatches\": null, \"compute_workgroups\": null}")
        != NULL);
    assert(strstr(contents,
        "\"gpu_memory_budget\": {\"scope\": \"current_process_estimate\", "
        "\"provider\": \"VK_EXT_memory_budget\", \"supported\": false, "
        "\"heaps\": []}") != NULL);

    obs_engine_bench_record_frame(&bench, 4, &frame);
    ObsGpuFrameProfile duplicate = {.gpu_ns = 999};
    obs_engine_bench_record_gpu(&bench, 4, &duplicate);
    assert(bench.captured_frames == 3);
    assert(bench.frames[2].gpu_ns == 40);
    obs_engine_bench_record_gpu(&bench, 99, &duplicate);
    assert(bench.frames[2].gpu_ns == 40);

    obs_engine_bench_destroy(&bench);
    assert(bench.frames == NULL);
    puts("engine benchmark capture: warmup, bounds, duplicate, and GPU sequence PASS");
    return 0;
}
