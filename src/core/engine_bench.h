#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "gpu_profile.h"
#include "allocation_owner.h"

#define OBS_ENGINE_BENCH_MAX_GPU_HEAPS 16
#define OBS_ENGINE_BENCH_STARTUP_MILESTONE_COUNT 16

typedef enum {
    OBS_STARTUP_PROCESS_ENTRY,
    OBS_STARTUP_GLFW_INIT,
    OBS_STARTUP_GLFW_WINDOW,
    OBS_STARTUP_INPUT_READY,
    OBS_STARTUP_EDITOR_FONT_READY,
    OBS_STARTUP_AUDIO_READY,
    OBS_STARTUP_VULKAN_INSTANCE,
    OBS_STARTUP_PHYSICAL_DEVICE,
    OBS_STARTUP_LOGICAL_DEVICE,
    OBS_STARTUP_SWAPCHAIN,
    OBS_STARTUP_RENDERER_READY,
    OBS_STARTUP_MONAD_RUNTIME_READY,
    OBS_STARTUP_EDITOR_READY,
    OBS_STARTUP_MONAD_BOOT_READY,
    OBS_STARTUP_FIRST_SUBMIT,
    OBS_STARTUP_FIRST_PRESENT,
} ObsEngineStartupMilestone;

typedef struct {
    uint64_t total_ns;
    uint64_t app_update_ns;
    uint64_t app_draw_ns;
    uint64_t event_poll_ns;
    uint64_t begin_fence_wait_ns;
    uint64_t swapchain_maintenance_ns;
    uint64_t renderer_setup_ns;
    uint64_t uniform_update_ns;
    uint64_t frame_capability_ns;
    uint64_t monad_callback_ns;
    uint64_t scene_animation_ns;
    uint64_t physics_ns;
    uint64_t camera_ns;
    uint64_t mesh_sync_ns;
    uint64_t camera_animation_ns;
    uint64_t editor_ns;
    uint64_t geometry_upload_ns;
    uint64_t acquire_image_ns;
    uint64_t image_fence_wait_ns;
    uint64_t gpu_completion_wait_ns;
    uint64_t command_record_ns;
    uint64_t command_render_graph_ns;
    uint64_t command_transparency_ns;
    uint64_t command_ui_ns;
    uint64_t queue_submit_ns;
    uint64_t queue_present_ns;
    uint64_t vertex2d_data_bytes;
    uint32_t vertex2d_count;
    uint64_t gpu_ns;
    uint64_t gpu_stage_ns[OBS_GPU_PROFILE_STAGE_COUNT];
    uint32_t gpu_stage_mask;
    uint64_t draw_calls;
    uint64_t indirect_draws;
    uint64_t submitted_vertices;
    uint64_t submitted_indices;
    uint64_t mesh_task_workgroups;
    uint64_t compute_dispatches;
    uint64_t compute_workgroups;
    bool gpu_sampled;
    bool cpu_sampled;
    bool work_counters_sampled;
} ObsEngineBenchFrame;

typedef struct {
    uint64_t process_start_ns;
    uint64_t process_cpu_start_ns;
    uint64_t process_cpu_ns;
    uint64_t process_peak_rss_bytes;
    uint64_t process_final_rss_bytes;
    bool process_peak_rss_sampled;
    bool process_final_rss_sampled;
    uint64_t window_ready_ns;
    uint64_t window_init_ns;
    uint64_t app_setup_ns;
    uint64_t device_wait_idle_ns;
    uint64_t cleanup_ns;
    uint64_t process_end_ns;
    uint64_t startup_milestone_ns[OBS_ENGINE_BENCH_STARTUP_MILESTONE_COUNT];
    uint64_t scene_load_wall_ns;
    uint64_t scene_load_cpu_ns;
    uint64_t scene_geometry_upload_gpu_ns;
    uint64_t scene_source_file_bytes;
    uint64_t scene_vertex_count;
    uint64_t scene_index_count;
    uint64_t scene_meshlet_count;
    uint64_t gpu_vertex_buffer_capacity_bytes;
    uint64_t gpu_vertex_buffer_used_bytes;
    uint64_t gpu_index_buffer_capacity_bytes;
    uint64_t gpu_index_buffer_used_bytes;
    uint64_t scene_texture_count;
    uint32_t scene_ao_mesh_count;
    uint32_t scene_mesh_count;
    uint32_t scene_gltf_instance_count;
    float directional_sun_intensity;
    bool directional_shadows_enabled;
    uint32_t warmup_frames;
    uint32_t requested_frames;
    uint32_t captured_frames;
    uint32_t seen_frames;
    bool gpu_timestamps_supported;
    bool gpu_timestamp_capture_enabled;
    bool gpu_debug_labels_enabled;
    bool work_counters_enabled;
    bool gpu_memory_budget_supported;
    bool engine_gpu_allocations_sampled;
    bool scene_loaded;
    bool gpu_geometry_buffers_sampled;
    bool scene_geometry_upload_gpu_sampled;
    bool monad_boot_configured;
    bool monad_boot_published;
    char scene_cache_mode[16];
    char render_mode[16];
    char benchmark_name[64];
    char gpu_device_name[256];
    uint32_t render_width;
    uint32_t render_height;
    uint32_t gpu_vendor_id;
    uint32_t gpu_device_id;
    uint32_t gpu_driver_version;
    uint32_t gpu_api_version;
    uint32_t gpu_queue_family;
    uint32_t gpu_timestamp_valid_bits;
    double gpu_timestamp_period_ns;
    uint32_t gpu_memory_heap_count;
    uint32_t engine_gpu_heap_count;
    uint64_t gpu_memory_initial_usage[OBS_ENGINE_BENCH_MAX_GPU_HEAPS];
    uint64_t gpu_memory_final_usage[OBS_ENGINE_BENCH_MAX_GPU_HEAPS];
    uint64_t gpu_memory_final_budget[OBS_ENGINE_BENCH_MAX_GPU_HEAPS];
    uint64_t engine_gpu_initial_bytes[OBS_ENGINE_BENCH_MAX_GPU_HEAPS];
    uint64_t engine_gpu_final_bytes[OBS_ENGINE_BENCH_MAX_GPU_HEAPS];
    uint64_t engine_gpu_peak_bytes[OBS_ENGINE_BENCH_MAX_GPU_HEAPS];
    uint64_t engine_gpu_initial_allocations[OBS_ENGINE_BENCH_MAX_GPU_HEAPS];
    uint64_t engine_gpu_final_allocations[OBS_ENGINE_BENCH_MAX_GPU_HEAPS];
    uint64_t engine_gpu_peak_allocations[OBS_ENGINE_BENCH_MAX_GPU_HEAPS];
    uint32_t allocation_owner_count;
    ObsAllocationOwner allocation_owners[OBS_ALLOCATION_OWNER_LIMIT];
    uint64_t work_draw_calls_by_stage[OBS_GPU_PROFILE_STAGE_COUNT];
    uint64_t work_indirect_draws_by_stage[OBS_GPU_PROFILE_STAGE_COUNT];
    uint64_t work_vertices_by_stage[OBS_GPU_PROFILE_STAGE_COUNT];
    uint64_t work_indices_by_stage[OBS_GPU_PROFILE_STAGE_COUNT];
    uint64_t work_mesh_task_workgroups_by_stage[OBS_GPU_PROFILE_STAGE_COUNT];
    uint64_t work_dispatches_by_stage[OBS_GPU_PROFILE_STAGE_COUNT];
    uint64_t work_compute_workgroups_by_stage[OBS_GPU_PROFILE_STAGE_COUNT];
    uint32_t manifest_min_mesh_count;
    ObsEngineBenchFrame *frames;
    char output_path[1024];
    char scene_path[1024];
} ObsEngineBench;

uint64_t obs_engine_bench_now_ns(void);
uint64_t obs_engine_bench_cpu_now_ns(void);
void obs_engine_bench_profile_activate(ObsEngineBench *bench);
void obs_engine_bench_profile_mark(ObsEngineStartupMilestone milestone);
void obs_engine_bench_capture_memory(ObsEngineBench *bench);
bool obs_engine_bench_init(ObsEngineBench *bench, uint32_t warmup_frames,
                           uint32_t measured_frames, const char *output_path);
void obs_engine_bench_record_frame(ObsEngineBench *bench, uint64_t sequence,
                                   const ObsEngineBenchFrame *frame);
void obs_engine_bench_record_gpu(ObsEngineBench *bench, uint64_t sequence,
                                 const ObsGpuFrameProfile *profile);
bool obs_engine_bench_done(const ObsEngineBench *bench);
bool obs_engine_bench_write(const ObsEngineBench *bench);
void obs_engine_bench_destroy(ObsEngineBench *bench);
