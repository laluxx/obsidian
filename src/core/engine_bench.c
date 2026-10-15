#define _POSIX_C_SOURCE 200809L
#include "engine_bench.h"

#include <inttypes.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool json_write_string(FILE *out, const char *text) {
    if (fputc('"', out) == EOF) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', out) == EOF || fputc(*p, out) == EOF) return false;
        } else if (*p < 0x20) {
            if (fprintf(out, "\\u%04x", *p) < 0) return false;
        } else if (fputc(*p, out) == EOF) {
            return false;
        }
    }
    return fputc('"', out) != EOF;
}


#define OBS_ENGINE_BENCH_MAX_FRAMES 100000u

static ObsEngineBench *startup_profile_bench;
static const char *const startup_milestone_names[
    OBS_ENGINE_BENCH_STARTUP_MILESTONE_COUNT] = {
    "process_entry", "glfw_init", "glfw_window", "input_ready",
    "editor_font_ready", "audio_ready", "vulkan_instance",
    "physical_device", "logical_device", "swapchain", "renderer_ready",
    "monad_runtime_ready", "editor_ready", "monad_boot_ready",
    "first_submit", "first_present"
};

uint64_t obs_engine_bench_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

uint64_t obs_engine_bench_cpu_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

void obs_engine_bench_profile_activate(ObsEngineBench *bench) {
    startup_profile_bench = bench;
    if (bench)
        bench->startup_milestone_ns[OBS_STARTUP_PROCESS_ENTRY] =
            bench->process_start_ns;
}

void obs_engine_bench_profile_mark(ObsEngineStartupMilestone milestone) {
    if (!startup_profile_bench ||
        (unsigned)milestone >= OBS_ENGINE_BENCH_STARTUP_MILESTONE_COUNT ||
        startup_profile_bench->startup_milestone_ns[milestone] != 0)
        return;
    startup_profile_bench->startup_milestone_ns[milestone] =
        obs_engine_bench_now_ns();
}

void obs_engine_bench_capture_memory(ObsEngineBench *bench) {
    if (!bench) return;
    struct rusage usage = {0};
    if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0 &&
        (uint64_t)usage.ru_maxrss <= UINT64_MAX / UINT64_C(1024)) {
        bench->process_peak_rss_bytes =
            (uint64_t)usage.ru_maxrss * UINT64_C(1024);
        bench->process_peak_rss_sampled = true;
    }

    FILE *statm = fopen("/proc/self/statm", "r");
    if (!statm) return;
    unsigned long long total_pages = 0, resident_pages = 0;
    int fields = fscanf(statm, "%llu %llu", &total_pages, &resident_pages);
    fclose(statm);
    long page_size = sysconf(_SC_PAGESIZE);
    if (fields == 2 && page_size > 0 &&
        resident_pages <= UINT64_MAX / (uint64_t)page_size) {
        bench->process_final_rss_bytes =
            (uint64_t)resident_pages * (uint64_t)page_size;
        bench->process_final_rss_sampled = true;
    }
}

bool obs_engine_bench_init(ObsEngineBench *bench, uint32_t warmup_frames,
                           uint32_t measured_frames, const char *output_path) {
    if (!bench || !output_path || !output_path[0] ||
        measured_frames == 0 || measured_frames > OBS_ENGINE_BENCH_MAX_FRAMES ||
        warmup_frames > OBS_ENGINE_BENCH_MAX_FRAMES - measured_frames ||
        strlen(output_path) >= sizeof(bench->output_path))
        return false;

    memset(bench, 0, sizeof(*bench));
    bench->frames = calloc(measured_frames, sizeof(*bench->frames));
    if (!bench->frames) return false;
    bench->warmup_frames = warmup_frames;
    bench->process_cpu_start_ns = obs_engine_bench_cpu_now_ns();
    bench->requested_frames = measured_frames;
    memcpy(bench->output_path, output_path, strlen(output_path) + 1);
    bench->process_start_ns = obs_engine_bench_now_ns();
    return true;
}

void obs_engine_bench_record_frame(ObsEngineBench *bench, uint64_t sequence,
                                   const ObsEngineBenchFrame *frame) {
    if (!bench || !bench->frames || !frame) return;
    if (sequence < UINT32_MAX && sequence + 1 > bench->seen_frames)
        bench->seen_frames = (uint32_t)sequence + 1;
    if (sequence < bench->warmup_frames) return;

    uint64_t index = sequence - bench->warmup_frames;
    if (index >= bench->requested_frames || bench->frames[index].cpu_sampled)
        return;
    bench->frames[index] = *frame;
    bench->frames[index].cpu_sampled = true;
    bench->captured_frames++;
}

void obs_engine_bench_record_gpu(ObsEngineBench *bench, uint64_t sequence,
                                 const ObsGpuFrameProfile *profile) {
    if (!bench || !bench->frames || sequence < bench->warmup_frames) return;
    uint64_t index = sequence - bench->warmup_frames;
    if (!profile || index >= bench->requested_frames ||
        bench->frames[index].gpu_sampled)
        return;
    bench->frames[index].gpu_ns = profile->gpu_ns;
    memcpy(bench->frames[index].gpu_stage_ns, profile->stage_ns,
           sizeof(profile->stage_ns));
    bench->frames[index].gpu_stage_mask = profile->stage_mask;
    bench->frames[index].gpu_sampled = true;
}

bool obs_engine_bench_done(const ObsEngineBench *bench) {
    return bench && bench->frames &&
           bench->seen_frames >= bench->warmup_frames + bench->requested_frames;
}

bool obs_engine_bench_write(const ObsEngineBench *bench) {
    if (!bench || !bench->frames || !bench->output_path[0]) return false;
    FILE *out = fopen(bench->output_path, "w");
    if (!out) return false;

    fprintf(out,
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"scenario\": \"%s\",\n"
        "  \"warmup_frames\": %u,\n"
        "  \"requested_frames\": %u,\n"
        "  \"captured_frames\": %u,\n"
        "  \"render_mode\": \"%s\",\n"
        "  \"render_extent\": {\"width\": %u, \"height\": %u},\n"
        "  \"benchmark_name\": ",
        bench->scene_loaded ? "gltf-scene" : "default-editor-demo",
        bench->warmup_frames, bench->requested_frames, bench->captured_frames,
        bench->render_mode, bench->render_width, bench->render_height);
    if (bench->benchmark_name[0])
        json_write_string(out, bench->benchmark_name);
    else
        fputs("null", out);
    fprintf(out,
        ",\n"
        "  \"manifest_min_mesh_count\": %u,\n"
        "  \"gpu_timestamps_supported\": %s,\n"
        "  \"gpu_debug_labels_enabled\": %s,\n"
        "  \"work_counters_enabled\": %s,\n"
        "  \"vulkan_device\": {\"name\": ",
        bench->manifest_min_mesh_count,
        bench->gpu_timestamps_supported ? "true" : "false",
        bench->gpu_debug_labels_enabled ? "true" : "false",
        bench->work_counters_enabled ? "true" : "false");
    json_write_string(out, bench->gpu_device_name);
    fprintf(out,
        ", \"vendor_id\": %u, \"device_id\": %u, \"driver_version\": %u,"
        " \"api_version\": %u},\n"
        "  \"gpu_timestamp_metadata\": {\"supported\": %s,"
        " \"queue_family_index\": %u, \"valid_bits\": %u,"
        " \"period_ns\": %.17g, \"enabled\": %s},\n"
        "  \"process_cpu_ns\": %" PRIu64 ",\n"
        "  \"process_memory\": {\"scope\": \"whole_process\","
        " \"peak_source\": \"getrusage.ru_maxrss\","
        " \"final_source\": \"/proc/self/statm\", \"peak_rss_bytes\": ",
        bench->gpu_vendor_id, bench->gpu_device_id,
        bench->gpu_driver_version, bench->gpu_api_version,
        bench->gpu_timestamps_supported ? "true" : "false",
        bench->gpu_queue_family, bench->gpu_timestamp_valid_bits,
        bench->gpu_timestamp_period_ns,
        bench->gpu_timestamp_capture_enabled ? "true" : "false",
        bench->process_cpu_ns);
    if (bench->process_peak_rss_sampled)
        fprintf(out, "%" PRIu64, bench->process_peak_rss_bytes);
    else
        fputs("null", out);
    fputs(", \"final_rss_bytes\": ", out);
    if (bench->process_final_rss_sampled)
        fprintf(out, "%" PRIu64, bench->process_final_rss_bytes);
    else
        fputs("null", out);
    fprintf(out,
        "},\n"
        "  \"gpu_memory_budget\": {\"scope\": \"current_process_estimate\","
        " \"provider\": \"VK_EXT_memory_budget\", \"supported\": %s,"
        " \"heaps\": [",
        bench->gpu_memory_budget_supported ? "true" : "false");
    for (uint32_t i = 0; bench->gpu_memory_budget_supported &&
                         i < bench->gpu_memory_heap_count; ++i) {
        fprintf(out,
            "%s{\"heap_index\": %u, \"initial_usage_bytes\": %" PRIu64
            ", \"final_usage_bytes\": %" PRIu64
            ", \"final_budget_bytes\": %" PRIu64 "}",
            i ? ", " : "", i, bench->gpu_memory_initial_usage[i],
            bench->gpu_memory_final_usage[i],
            bench->gpu_memory_final_budget[i]);
    }
    fprintf(out,
        "]},\n"
        "  \"engine_vulkan_memory\": {"
        "\"scope\": \"live_VkDeviceMemory_allocation_sizes_by_heap\","
        "\"sampled\": %s, \"heaps\": [",
        bench->engine_gpu_allocations_sampled ? "true" : "false");
    for (uint32_t i = 0; bench->engine_gpu_allocations_sampled &&
                         i < bench->engine_gpu_heap_count; ++i) {
        fprintf(out,
            "%s{\"heap_index\": %u, \"initial_live_bytes\": %" PRIu64
            ", \"final_live_bytes\": %" PRIu64
            ", \"peak_live_bytes\": %" PRIu64
            ", \"initial_live_allocations\": %" PRIu64
            ", \"final_live_allocations\": %" PRIu64
            ", \"peak_live_allocations\": %" PRIu64 "}",
            i ? ", " : "", i, bench->engine_gpu_initial_bytes[i],
            bench->engine_gpu_final_bytes[i], bench->engine_gpu_peak_bytes[i],
            bench->engine_gpu_initial_allocations[i],
            bench->engine_gpu_final_allocations[i],
            bench->engine_gpu_peak_allocations[i]);
    }
    fputs("]},\n  \"gpu_geometry_buffers\": {"
          "\"scope\": \"logical_buffer_size_and_write_cursor\", "
          "\"sampled\": ", out);
    fputs(bench->gpu_geometry_buffers_sampled ? "true" : "false", out);
    fputs(", \"vertex_capacity_bytes\": ", out);
    if (bench->gpu_geometry_buffers_sampled)
        fprintf(out, "%" PRIu64, bench->gpu_vertex_buffer_capacity_bytes);
    else
        fputs("null", out);
    fputs(", \"vertex_used_bytes\": ", out);
    if (bench->gpu_geometry_buffers_sampled)
        fprintf(out, "%" PRIu64, bench->gpu_vertex_buffer_used_bytes);
    else
        fputs("null", out);
    fputs(", \"index_capacity_bytes\": ", out);
    if (bench->gpu_geometry_buffers_sampled)
        fprintf(out, "%" PRIu64, bench->gpu_index_buffer_capacity_bytes);
    else
        fputs("null", out);
    fputs(", \"index_used_bytes\": ", out);
    if (bench->gpu_geometry_buffers_sampled)
        fprintf(out, "%" PRIu64, bench->gpu_index_buffer_used_bytes);
    else
        fputs("null", out);
    fprintf(out,
        "},\n"
        "  \"lifecycle_ns\": {\n"
        "    \"process_to_window_ready\": %" PRIu64 ",\n"
        "    \"window_init\": %" PRIu64 ",\n"
        "    \"app_setup\": %" PRIu64 ",\n"
        "    \"device_wait_idle\": %" PRIu64 ",\n"
        "    \"engine_cleanup\": %" PRIu64 ",\n"
        "    \"main_to_engine_shutdown_complete\": %" PRIu64 "\n"
        "  },\n"
        "  \"startup_milestones_ns\": {",
        bench->window_ready_ns >= bench->process_start_ns
            ? bench->window_ready_ns - bench->process_start_ns : 0,
        bench->window_init_ns, bench->app_setup_ns,
        bench->device_wait_idle_ns, bench->cleanup_ns,
        bench->process_end_ns >= bench->process_start_ns
            ? bench->process_end_ns - bench->process_start_ns : 0);

    for (size_t i = 0; i < OBS_ENGINE_BENCH_STARTUP_MILESTONE_COUNT; ++i) {
        fprintf(out, "%s\n    \"%s\": ",
                i == 0 ? "" : ",", startup_milestone_names[i]);
        if (bench->startup_milestone_ns[i] >= bench->process_start_ns &&
            bench->startup_milestone_ns[i] != 0)
            fprintf(out, "%" PRIu64,
                    bench->startup_milestone_ns[i] - bench->process_start_ns);
        else
            fputs("null", out);
    }
    fputs("\n  },\n", out);
    fprintf(out, "  \"allocation_owners\": {\"sampled\": %s, "
            "\"scope\": \"VkDeviceMemory_constructor_ownership\", \"owners\": [",
            bench->engine_gpu_allocations_sampled ? "true" : "false");
    for (uint32_t i = 0; i < bench->allocation_owner_count; ++i) {
        const ObsAllocationOwner *owner = &bench->allocation_owners[i];
        fprintf(out, "%s{\"name\": ", i ? "," : "");
        json_write_string(out, owner->name);
        fprintf(out, ", \"live_bytes\": %" PRIu64
                ", \"peak_bytes\": %" PRIu64
                ", \"live_allocations\": %" PRIu64
                ", \"peak_allocations\": %" PRIu64 "}",
                owner->live_bytes, owner->peak_bytes,
                owner->live_allocations, owner->peak_allocations);
    }
    fputs("]},\n", out);

    fprintf(out,
        "  \"render_features\": {\"directional_sun_intensity\": %.9g, "
        "\"directional_shadows_enabled\": %s},\n",
        bench->directional_sun_intensity,
        bench->directional_shadows_enabled ? "true" : "false");
    fputs("  \"scene\": ", out);
    if (bench->scene_loaded) {
        fputs("{\"path\": ", out);
        json_write_string(out, bench->scene_path);
        fprintf(out,
            ", \"load_wall_ns\": %" PRIu64
            ", \"load_cpu_ns\": %" PRIu64
            ", \"geometry_upload_gpu_ns\": ",
            bench->scene_load_wall_ns, bench->scene_load_cpu_ns);
        if (bench->scene_geometry_upload_gpu_sampled)
            fprintf(out, "%" PRIu64, bench->scene_geometry_upload_gpu_ns);
        else
            fputs("null", out);
        fputs(", \"cache_mode\": ", out);
        json_write_string(out, bench->scene_cache_mode);
        fprintf(out,
            ", \"source_file_bytes\": %" PRIu64
            ", \"mesh_count\": %u, \"ao_mesh_count\": %u"
            ", \"gltf_instance_count\": %u"
            ", \"texture_count\": %" PRIu64
            ", \"vertex_count\": %" PRIu64
            ", \"index_count\": %" PRIu64
            ", \"meshlet_count\": %" PRIu64 "},\n",
            bench->scene_source_file_bytes,
            bench->scene_mesh_count, bench->scene_ao_mesh_count,
            bench->scene_gltf_instance_count,
            bench->scene_texture_count,
            bench->scene_vertex_count, bench->scene_index_count,
            bench->scene_meshlet_count);
    } else {
        fputs("null,\n", out);
    }
    if (bench->monad_boot_configured)
        fprintf(out, "  \"monad_boot\": {\"published\": %s},\n",
                bench->monad_boot_published ? "true" : "false");
    else
        fputs("  \"monad_boot\": null,\n", out);

    fputs("  \"gpu_stage_schema\": [\n", out);
    for (uint32_t stage = 0; stage < OBS_GPU_PROFILE_STAGE_COUNT; ++stage) {
        const char *name =
            obs_gpu_profile_stage_name((ObsGpuProfileStage)stage);
        int parent = obs_gpu_profile_stage_parent((ObsGpuProfileStage)stage);
        fprintf(out, "    {\"id\": %u, \"name\": ", stage);
        json_write_string(out, name);
        fputs(", \"parent\": ", out);
        if (parent >= 0)
            json_write_string(out, obs_gpu_profile_stage_name(
                                      (ObsGpuProfileStage)parent));
        else
            fputs("null", out);
        fprintf(out, ", \"unit\": \"ns\", \"timing\": \"inclusive\"}%s\n",
                stage + 1 == OBS_GPU_PROFILE_STAGE_COUNT ? "" : ",");
    }
    fputs("  ],\n", out);
    fputs("  \"frames\": [\n", out);
    for (uint32_t i = 0; i < bench->requested_frames; i++) {
        const ObsEngineBenchFrame *f = &bench->frames[i];
        fprintf(out,
            "    {\"sequence\": %u, \"cpu_sampled\": %s, \"total_ns\": %" PRIu64
            ", \"app_update_ns\": %" PRIu64 ", \"app_draw_ns\": %" PRIu64
            ", \"event_poll_ns\": %" PRIu64
            ", \"begin_fence_wait_ns\": %" PRIu64
            ", \"swapchain_maintenance_ns\": %" PRIu64
            ", \"renderer_setup_ns\": %" PRIu64
            ", \"uniform_update_ns\": %" PRIu64
            ", \"frame_capability_ns\": %" PRIu64
            ", \"monad_callback_ns\": %" PRIu64
            ", \"scene_animation_ns\": %" PRIu64
            ", \"physics_ns\": %" PRIu64
            ", \"camera_ns\": %" PRIu64
            ", \"mesh_sync_ns\": %" PRIu64
            ", \"camera_animation_ns\": %" PRIu64
            ", \"editor_ns\": %" PRIu64
            ", \"geometry_upload_ns\": %" PRIu64
            ", \"acquire_image_ns\": %" PRIu64
            ", \"image_fence_wait_ns\": %" PRIu64
            ", \"gpu_completion_wait_ns\": %" PRIu64
            ", \"command_record_ns\": %" PRIu64
            ", \"command_render_graph_ns\": %" PRIu64
            ", \"command_transparency_ns\": %" PRIu64
            ", \"command_ui_ns\": %" PRIu64
            ", \"queue_submit_ns\": %" PRIu64
            ", \"queue_present_ns\": %" PRIu64
            ", \"vertex2d_data_bytes\": %" PRIu64
            ", \"vertex2d_count\": %u, \"gpu_ns\": ",
            i + bench->warmup_frames, f->cpu_sampled ? "true" : "false",
            f->total_ns, f->app_update_ns,
            f->app_draw_ns, f->event_poll_ns, f->begin_fence_wait_ns,
            f->swapchain_maintenance_ns, f->renderer_setup_ns,
            f->uniform_update_ns, f->frame_capability_ns, f->monad_callback_ns,
            f->scene_animation_ns, f->physics_ns, f->camera_ns,
            f->mesh_sync_ns, f->camera_animation_ns, f->editor_ns,
            f->geometry_upload_ns,
            f->acquire_image_ns, f->image_fence_wait_ns,
            f->gpu_completion_wait_ns, f->command_record_ns,
            f->command_render_graph_ns, f->command_transparency_ns,
            f->command_ui_ns,
            f->queue_submit_ns, f->queue_present_ns, f->vertex2d_data_bytes,
            f->vertex2d_count);
        if (f->gpu_sampled) fprintf(out, "%" PRIu64, f->gpu_ns);
        else fputs("null", out);
        fputs(", \"gpu_pass_ns\": {", out);
        for (uint32_t stage = 0; stage < OBS_GPU_PROFILE_STAGE_COUNT; ++stage) {
            if (stage) fputs(", ", out);
            json_write_string(
                out, obs_gpu_profile_stage_name((ObsGpuProfileStage)stage));
            fputs(": ", out);
            if (f->gpu_stage_mask & (UINT32_C(1) << stage))
                fprintf(out, "%" PRIu64, f->gpu_stage_ns[stage]);
            else
                fputs("null", out);
        }
        fputs("}, \"work_counters\": {\"sampled\": ", out);
        fputs(f->work_counters_sampled ? "true" : "false", out);
        const struct {
            const char *name;
            uint64_t value;
        } counters[] = {
            {"draw_calls", f->draw_calls},
            {"indirect_draws", f->indirect_draws},
            {"submitted_vertices", f->submitted_vertices},
            {"submitted_indices", f->submitted_indices},
            {"mesh_task_workgroups", f->mesh_task_workgroups},
            {"compute_dispatches", f->compute_dispatches},
            {"compute_workgroups", f->compute_workgroups},
        };
        for (size_t counter = 0;
             counter < sizeof(counters) / sizeof(counters[0]); ++counter) {
            fprintf(out, ", ");
            json_write_string(out, counters[counter].name);
            fputs(": ", out);
            if (f->work_counters_sampled)
                fprintf(out, "%" PRIu64, counters[counter].value);
            else
                fputs("null", out);
        }
        fprintf(out, "}}%s\n",
                i + 1 == bench->requested_frames ? "" : ",");
    }
    fputs("  ],\n  \"work_counters_by_stage\": ", out);
    if (!bench->work_counters_enabled) {
        fputs("null", out);
    } else {
        fprintf(out,
                "{\"sampled_frames\": %u, "
                "\"unit\": \"total_commands_over_measured_frames\", "
                "\"stages\": {",
                bench->captured_frames);
        for (uint32_t stage = 0; stage < OBS_GPU_PROFILE_STAGE_COUNT; ++stage) {
            if (stage) fputs(", ", out);
            json_write_string(
                out, obs_gpu_profile_stage_name((ObsGpuProfileStage)stage));
            fprintf(out,
                    ": {\"draw_calls\": %" PRIu64
                    ", \"indirect_draws\": %" PRIu64
                    ", \"submitted_vertices\": %" PRIu64
                    ", \"submitted_indices\": %" PRIu64
                    ", \"mesh_task_workgroups\": %" PRIu64
                    ", \"compute_dispatches\": %" PRIu64
                    ", \"compute_workgroups\": %" PRIu64 "}",
                    bench->work_draw_calls_by_stage[stage],
                    bench->work_indirect_draws_by_stage[stage],
                    bench->work_vertices_by_stage[stage],
                    bench->work_indices_by_stage[stage],
                    bench->work_mesh_task_workgroups_by_stage[stage],
                    bench->work_dispatches_by_stage[stage],
                    bench->work_compute_workgroups_by_stage[stage]);
        }
        fputs("}}", out);
    }
    fputs("\n}\n", out);
    bool ok = !ferror(out);
    if (fclose(out) != 0) ok = false;
    return ok;
}

void obs_engine_bench_destroy(ObsEngineBench *bench) {
    if (!bench) return;
    free(bench->frames);
    memset(bench, 0, sizeof(*bench));
}
