/// window — platform subsystem
#pragma once

#include "context.h"
#include "camera.h"
#include <stdbool.h>
#include <stdint.h>
#include "gpu_profile.h"

/// Public interface

#define WIDTH 800
#define HEIGHT 600

#define CURSOR_NORMAL   GLFW_CURSOR_NORMAL
#define CURSOR_HIDDEN   GLFW_CURSOR_HIDDEN
#define CURSOR_DISABLED GLFW_CURSOR_DISABLED


#define ARROW_CURSOR     GLFW_ARROW_CURSOR
#define IBEAM_CURSOR     GLFW_IBEAM_CURSOR
#define CROSSHAIR_CURSOR GLFW_CROSSHAIR_CURSOR
#define HAND_CURSOR      GLFW_HAND_CURSOR
#define HRESIZE_CURSOR   GLFW_HRESIZE_CURSOR
#define VRESIZE_CURSOR   GLFW_VRESIZE_CURSOR

GLFWcursor* createStandardCursor(int shape);
void setCursor(GLFWcursor* cursor);


extern float delta_time;
extern float last_frame;


GLFWwindow* initWindow(int width, int height, const char* title);
typedef void (*ObsHeadlessDrawCallback)(void *userdata);
bool obs_headless_init(uint32_t width, uint32_t height,
                       bool readback_enabled);
bool obs_headless_render_frame(ObsHeadlessDrawCallback draw, void *userdata);
bool obs_headless_render_raw(const char *path, ObsHeadlessDrawCallback draw,
                             void *userdata);
int windowShouldClose();

/* Set before initWindow() when the host application supplies its own editor.
 * The disabled path does not initialize, draw, or route input to Obsidian's
 * built-in editor. */
void obs_set_editor_enabled(bool enabled);
bool obs_editor_enabled(void);
bool obs_register_editor_commands(void);

void beginFrame();
void endFrame();

/* ── Multi-window API ────────────────────────────────────────────────────
 * obs_window_create_glfw + obs_window_create_surface + obs_window_init_
 * swapchain build the FIRST window around instance/device creation; after
 * that obs_window_create() is a one-call constructor for extra windows. */
ObsWindow *obs_window_create_glfw(int width, int height, const char *title);
bool       obs_window_create_surface(ObsWindow *w, VulkanContext *ctx);
bool       obs_window_init_swapchain(ObsWindow *w, VulkanContext *ctx);
bool       obs_window_init_command_buffers(ObsWindow *w, VulkanContext *ctx);
ObsWindow *obs_window_create(int width, int height, const char *title);
ObsWindow *obs_window_create_hidden(int width, int height, const char *title);
void       obs_window_destroy(ObsWindow *w);
void       obs_window_collect_retired_swapchains(ObsWindow *w);
void       obs_window_drain_retired_swapchains(ObsWindow *w);
void       obs_window_wait_swapchain_presentations(ObsWindow *w);

GLFWwindow *obs_window_glfw(ObsWindow *w);
ObsWindow  *obs_window_from_glfw(GLFWwindow *win);
uint32_t    obs_window_extent_width(ObsWindow *w);
uint32_t    obs_window_extent_height(ObsWindow *w);
bool        obs_window_should_close(ObsWindow *w);
void        obs_window_focus(ObsWindow *w);
void        obs_window_set_current(ObsWindow *w);
void        obs_window_set_resize_increments(ObsWindow *w, int cw, int ch, int minw, int minh);

/* Per-window frame loop.  obs_begin_frame resets renderer CPU state and the
 * frame slot; between it and obs_end_frame the app issues draw primitives;
 * obs_end_frame uploads, records, submits and presents that window. */
void obs_begin_frame(ObsWindow *w);
void obs_end_frame(ObsWindow *w);
/* Optional CPU-side frame breakdown for embedding hosts.  Instrumentation is
 * dormant unless enabled. Getter returns last COMPLETED begin/end pair;
 * mid-frame reads never observe a partially filled active sample. */
typedef struct {
    uint64_t frame_sequence;
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
    uint64_t command_record_ns;
    uint64_t command_render_graph_ns;
    uint64_t command_transparency_ns;
    uint64_t command_ui_ns;
    uint64_t queue_submit_ns;
    uint64_t queue_present_ns;
    uint64_t vertex2d_data_bytes;
    uint32_t vertex2d_count;
} ObsFrameProfile;
typedef enum {
    OBS_FRAME_COMMAND_RENDER_GRAPH,
    OBS_FRAME_COMMAND_TRANSPARENCY,
    OBS_FRAME_COMMAND_UI
} ObsFrameCommandScope;
void obs_frame_profile_enable(bool enabled);
uint64_t obs_frame_profile_command_scope_begin(void);
void obs_frame_profile_command_scope_end(ObsFrameCommandScope scope,
                                         uint64_t start_ns);
void obs_frame_profile_get(ObsFrameProfile *out);
bool obs_gpu_frame_profile_supported(uint32_t *valid_bits,
                                     double *timestamp_period_ns);
bool obs_gpu_frame_profile_enable(void);
bool obs_gpu_frame_profile_take(ObsGpuFrameProfile *out);
void obs_gpu_frame_profile_flush(void);
void obs_gpu_frame_profile_destroy(void);
void obs_poll_events(void);
/* Block until an OS event arrives or timeout_seconds elapses.  Hosts that
 * own the editor loop can use this to pace uncapped immediate-present
 * renderers without adding input latency while idle. */
void obs_wait_events_timeout(double timeout_seconds);

/* The window currently processing an input event (valid inside callbacks
 * registered through the register*Callback functions). */
ObsWindow *obs_current_event_window(void);

double getTime();

void setWindowPos(GLFWwindow* window, int x, int y);
void getWindowPos(GLFWwindow* window, int* x, int* y);

void setWindowSize(GLFWwindow* window, int width, int height);
void getWindowSize(GLFWwindow* window, int* width, int* height);

void pollEvents(void);

const char* getClipboardString();
void setClipboardString(const char *text);

void setInputMode(GLFWwindow* window, int mode, int value);
void getCursorPos(GLFWwindow* window, double* xpos, double* ypos);


void setCursorMode(int mode);
int getCursorMode();
void showCursor();
void hideCursor();
void disableCursor();

/* void setWindowResizeIncrements(int char_width, int char_height); */
void setWindowResizeIncrements(int char_width, int char_height, int min_width, int min_height);


void toggle_editor_mode();
void process_editor_movement(Camera* cam, float deltaTime);
void editor_frame_selected(void);
