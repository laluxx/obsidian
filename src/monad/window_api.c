#include "window_api.h"

#include "bindings.h"
#include "editor.h"
#include "foreign.h"
#include "live_binding_txn.h"
#include "renderer.h"
#include "window.h"
#include "live_eval.h"
#include "types.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MONAD_WINDOW_TYPE "obsidian.monad-window"
#define MONAD_WINDOW_CAPACITY 32u
#define MONAD_WINDOW_TITLE_MAX 191u

typedef struct {
    bool active;
    bool closing;
    uint64_t generation;
    ObsEditorFrame *frame;
    monad_value_t *draw;
    monad_value_t *input;
} ObsMonadWindow;

typedef struct {
    struct ObsLiveWindowCapability *capability;
    bool active;
    bool closing;
    ObsEditorFrame *frame;
    RuntimeValue *draw;
    RuntimeValue *input;
} ObsLiveMonadWindow;

typedef struct ObsLiveWindowCapability {
    struct ObsLiveWindowCapability *next;
} ObsLiveWindowCapability;

static ObsMonadWindow s_windows[MONAD_WINDOW_CAPACITY];
static ObsLiveMonadWindow s_live_windows[MONAD_WINDOW_CAPACITY];
static ObsLiveWindowCapability *s_live_capabilities;
static uint64_t s_next_generation;
static ObsMonadWindow *s_drawing_window;
static ObsLiveMonadWindow *s_live_drawing_window;
static unsigned s_window_callback_depth;
static unsigned s_live_callback_depth;

static void free_live_callback_boxes(ObsLiveMonadWindow *window) {
    free(window->draw);
    free(window->input);
    window->draw = NULL;
    window->input = NULL;
}

static RuntimeValue *live_capability(ObsLiveMonadWindow *window) {
    return window && window->active
        ? rt_value_opaque(window->capability) : rt_value_nil();
}

static ObsLiveMonadWindow *live_window_from_value(RuntimeValue *value) {
    void *capability = rt_unbox_opaque(value);
    if (!capability) return NULL;
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_live_windows[i].active && !s_live_windows[i].closing &&
            capability == s_live_windows[i].capability)
            return &s_live_windows[i];
    return NULL;
}

static bool live_window_call(ObsLiveMonadWindow *window,
                             RuntimeValue *procedure, int argc,
                             RuntimeValue **arguments) {
    if (!window || !window->active || window->closing || !procedure ||
        procedure->type != RT_CLOSURE || s_live_callback_depth >= 8 ||
        !obs_monad_on_owner_thread())
        return false;
    ++s_live_callback_depth;
    RuntimeValue *result = NULL;
    const bool ok = monad_live_evaluator_call_closure_guarded(
        obs_monad_engine()->live_evaluator, procedure, argc, arguments,
        &result);
    --s_live_callback_depth;
    arena_reset(&g_eval_arena);
    return ok;
}

static void close_live_window(ObsLiveMonadWindow *window) {
    if (!window || !window->active || window->closing) return;
    window->closing = true;
    obs_editor_frame_close(window->frame);
}

RuntimeValue *obs_monad_live_window_open_value(void *unused, int argc,
                                               RuntimeValue **argv) {
    (void)unused;
    if (argc != 5 || !argv || !obs_monad_on_owner_thread() ||
        !obs_monad_runtime_mutation_allowed() ||
        obs_monad_binding_staging() ||
        argv[0]->type != RT_STRING || argv[1]->type != RT_INT ||
        argv[2]->type != RT_INT || argv[3]->type != RT_CLOSURE ||
        argv[4]->type != RT_CLOSURE ||
        argv[3]->data.closure_val->arity != 1 ||
        argv[4]->data.closure_val->arity != 7)
        return rt_value_nil();
    const size_t title_length = strlen(argv[0]->data.string_val);
    const int64_t width = argv[1]->data.int_val;
    const int64_t height = argv[2]->data.int_val;
    if (!title_length || title_length > MONAD_WINDOW_TITLE_MAX ||
        width < 64 || width > 4096 || height < 64 || height > 4096)
        return rt_value_nil();
    ObsLiveMonadWindow *window = NULL;
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (!s_live_windows[i].active) {
            window = &s_live_windows[i];
            break;
        }
    if (!window) return rt_value_nil();
    RuntimeValue *draw = rt_value_persist(argv[3]);
    RuntimeValue *input = rt_value_persist(argv[4]);
    if (!draw || !input) {
        free(draw);
        free(input);
        return rt_value_nil();
    }
    ObsLiveWindowCapability *capability = calloc(1, sizeof(*capability));
    if (!capability) {
        free(draw);
        free(input);
        return rt_value_nil();
    }
    capability->next = s_live_capabilities;
    s_live_capabilities = capability;
    *window = (ObsLiveMonadWindow){
        .active = true,
        .capability = capability,
        .draw = draw,
        .input = input
    };
    window->frame = obs_editor_frame_create_monad_window(
        argv[0]->data.string_val, (int)width, (int)height, true);
    if (!window->frame) {
        free_live_callback_boxes(window);
        *window = (ObsLiveMonadWindow){0};
        return rt_value_nil();
    }
    return live_capability(window);
}

RuntimeValue *obs_monad_live_window_close_value(void *unused, int argc,
                                                RuntimeValue **argv) {
    (void)unused;
    if (argc != 1 || !argv || !obs_monad_runtime_mutation_allowed() ||
        obs_monad_binding_staging() || !live_window_from_value(argv[0]))
        return rt_value_bool(0);
    close_live_window(live_window_from_value(argv[0]));
    return rt_value_bool(1);
}

static bool live_draw_values(int argc, RuntimeValue **argv, size_t count,
                             double *values,
                             ObsLiveMonadWindow **window) {
    if (!argv || argc != (int)(count + 1) || !s_live_drawing_window ||
        !(*window = live_window_from_value(argv[0])) ||
        *window != s_live_drawing_window ||
        obs_editor_frame_current() != (*window)->frame)
        return false;
    for (size_t i = 0; i < count; ++i) {
        RuntimeValue *value = argv[i + 1];
        if (value->type != RT_INT && value->type != RT_FLOAT) return false;
        values[i] = value->type == RT_INT
            ? (double)value->data.int_val : value->data.float_val;
        if (!isfinite(values[i]) || fabs(values[i]) > 1000000.0)
            return false;
    }
    return true;
}

static bool live_draw_color(const double *values, size_t start, Color *color) {
    for (size_t i = 0; i < 4; ++i)
        if (values[start + i] < 0.0 || values[start + i] > 1.0)
            return false;
    *color = (Color){(float)values[start], (float)values[start + 1],
                     (float)values[start + 2], (float)values[start + 3]};
    return true;
}

RuntimeValue *obs_monad_live_window_background_value(void *unused, int argc,
                                                      RuntimeValue **argv) {
    (void)unused;
    double values[4];
    Color color;
    ObsLiveMonadWindow *window = NULL;
    if (!live_draw_values(argc, argv, 4, values, &window) ||
        !live_draw_color(values, 0, &color))
        return rt_value_bool(0);
    quad2D((vec2){0.0f, 0.0f},
           (vec2){(float)context.drawExtent.width,
                  (float)context.drawExtent.height}, color);
    return rt_value_bool(1);
}

RuntimeValue *obs_monad_live_window_rect_value(void *unused, int argc,
                                               RuntimeValue **argv) {
    (void)unused;
    double values[8];
    Color color;
    ObsLiveMonadWindow *window = NULL;
    if (!live_draw_values(argc, argv, 8, values, &window) ||
        values[2] < 0.0 || values[3] < 0.0 ||
        !live_draw_color(values, 4, &color))
        return rt_value_bool(0);
    const float height = (float)context.drawExtent.height;
    quad2D((vec2){(float)values[0],
                  height - (float)values[1] - (float)values[3]},
           (vec2){(float)values[2], (float)values[3]}, color);
    return rt_value_bool(1);
}

RuntimeValue *obs_monad_live_window_circle_value(void *unused, int argc,
                                                 RuntimeValue **argv) {
    (void)unused;
    double values[7];
    Color color;
    ObsLiveMonadWindow *window = NULL;
    if (!live_draw_values(argc, argv, 7, values, &window) ||
        values[2] < 0.0 || !live_draw_color(values, 3, &color))
        return rt_value_bool(0);
    circle2D((vec2){(float)values[0],
                    (float)context.drawExtent.height - (float)values[1]},
             (float)values[2], color);
    return rt_value_bool(1);
}

RuntimeValue *obs_monad_live_window_line_value(void *unused, int argc,
                                               RuntimeValue **argv) {
    (void)unused;
    double values[8];
    Color color;
    ObsLiveMonadWindow *window = NULL;
    if (!live_draw_values(argc, argv, 8, values, &window) ||
        !live_draw_color(values, 4, &color))
        return rt_value_bool(0);
    const float height = (float)context.drawExtent.height;
    line2D((vec2){(float)values[0], height - (float)values[1]},
           (vec2){(float)values[2], height - (float)values[3]}, color);
    return rt_value_bool(1);
}

RuntimeValue *obs_monad_live_window_text_value(void *unused, int argc,
                                               RuntimeValue **argv) {
    (void)unused;
    double values[6];
    Color color;
    ObsLiveMonadWindow *window = NULL;
    if (argc != 8 || !argv || argv[1]->type != RT_STRING ||
        strlen(argv[1]->data.string_val) > 4096 ||
        !live_window_from_value(argv[0]) ||
        (window = live_window_from_value(argv[0])) != s_live_drawing_window ||
        obs_editor_frame_current() != window->frame)
        return rt_value_bool(0);
    for (size_t i = 0; i < 6; ++i) {
        RuntimeValue *value = argv[i + 2];
        if (value->type != RT_INT && value->type != RT_FLOAT)
            return rt_value_bool(0);
        values[i] = value->type == RT_INT
            ? (double)value->data.int_val : value->data.float_val;
        if (!isfinite(values[i]) || fabs(values[i]) > 1000000.0)
            return rt_value_bool(0);
    }
    if (!live_draw_color(values, 2, &color) || !editor.font)
        return rt_value_bool(0);
    text(editor.font, argv[1]->data.string_val, (float)values[0],
         (float)context.drawExtent.height - (float)values[1], color);
    return rt_value_bool(1);
}

static void release_window(ObsMonadWindow *window) {
    if (!window) return;
    monad_value_release(window->draw);
    monad_value_release(window->input);
    *window = (ObsMonadWindow){0};
}

static bool window_from_value(monad_value_t *value, ObsMonadRuntime *host,
                              ObsMonadWindow **out, monad_error_t *error) {
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i) {
        ObsMonadWindow *window = &s_windows[i];
        if (!window->active || window->closing) continue;
        if (obs_host_require(value, host, MONAD_WINDOW_TYPE, window,
                             obs_host_epoch(host, window->generation), NULL)) {
            if (out) *out = window;
            return true;
        }
    }
    return obs_host_invalid(error, "expected an open Monad window capability");
}

static monad_value_t *window_ref(ObsMonadWindow *window,
                                 ObsMonadRuntime *host,
                                 monad_error_t *error) {
    monad_value_t *value = NULL;
    if (!window || !window->active || window->closing ||
        !obs_host_borrow(host->space, host, MONAD_WINDOW_TYPE, window,
                         obs_host_epoch(host, window->generation),
                         &value, error))
        return NULL;
    return value;
}

static bool callback_call(ObsMonadWindow *window, monad_value_t *procedure,
                          size_t argc, monad_value_t *const *arguments,
                          monad_error_t *error) {
    ObsMonadRuntime *host = obs_monad_engine();
    if (!window || !window->active || !procedure ||
        s_window_callback_depth >= 8 || !obs_host_owner(host))
        return false;
    ObsCodeLease lease = {0};
    if (!obs_monad_code_enter(&lease)) return false;
    monad_value_retain(procedure);
    monad_value_t *result = NULL;
    ++s_window_callback_depth;
    monad_status_t status = monad_value_call(
        host->space, procedure, argc, arguments, &result, error);
    --s_window_callback_depth;
    monad_value_release(result);
    monad_value_release(procedure);
    (void)obs_monad_code_leave(&lease);
    return status == MONAD_OK;
}

static void close_window(ObsMonadWindow *window) {
    if (!window || !window->active || window->closing) return;
    window->closing = true;
    obs_editor_frame_close(window->frame);
}

static monad_status_t open_window(monad_object_space_t *space, size_t argc,
                                  monad_value_t *const *args,
                                  monad_value_t **out, void *context,
                                  monad_error_t *error) {
    (void)space;
    ObsMonadRuntime *host = context;
    const char *title = NULL;
    size_t title_length = 0;
    int64_t width = 0, height = 0;
    size_t draw_min = 0, draw_max = 0, input_min = 0, input_max = 0;
    if (argc != 5 || !args || !out || !obs_host_owner(host) ||
        !obs_monad_runtime_mutation_allowed() || obs_monad_binding_staging() ||
        monad_value_bytes(args[0], &title, &title_length, error) != MONAD_OK ||
        !title || !title_length || title_length > MONAD_WINDOW_TITLE_MAX ||
        monad_value_as_i64(args[1], &width, error) != MONAD_OK ||
        monad_value_as_i64(args[2], &height, error) != MONAD_OK ||
        width < 64 || width > 4096 || height < 64 || height > 4096 ||
        monad_value_kind(args[3]) != MONAD_VALUE_PROCEDURE ||
        monad_value_kind(args[4]) != MONAD_VALUE_PROCEDURE ||
        monad_value_procedure_info(args[3], NULL, &draw_min, &draw_max,
                                   error) != MONAD_OK ||
        monad_value_procedure_info(args[4], NULL, &input_min, &input_max,
                                   error) != MONAD_OK ||
        draw_min != 1 || draw_max != 1 || input_min != 7 || input_max != 7)
        return obs_host_invalid(error,
            "window-open! expects title, 64..4096 dimensions, a one-argument draw procedure, and a seven-argument input procedure");

    ObsMonadWindow *window = NULL;
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (!s_windows[i].active) {
            window = &s_windows[i];
            break;
        }
    if (!window)
        return obs_host_invalid(error, "Monad window capacity is exhausted");

    if (++s_next_generation == 0) ++s_next_generation;
    *window = (ObsMonadWindow){
        .active = true,
        .generation = s_next_generation,
        .draw = args[3],
        .input = args[4]
    };
    monad_value_retain(window->draw);
    monad_value_retain(window->input);
    char title_copy[MONAD_WINDOW_TITLE_MAX + 1];
    memcpy(title_copy, title, title_length);
    title_copy[title_length] = '\0';
    window->frame = obs_editor_frame_create_monad_window(
        title_copy, (int)width, (int)height, true);
    if (!window->frame) {
        release_window(window);
        return obs_host_invalid(error, "could not create the Monad tool window");
    }
    *out = window_ref(window, host, error);
    if (!*out) {
        close_window(window);
        return error ? error->status : MONAD_ERROR_OUT_OF_MEMORY;
    }
    return MONAD_OK;
}

static monad_status_t close_window_binding(monad_object_space_t *space,
                                           size_t argc,
                                           monad_value_t *const *args,
                                           monad_value_t **out,
                                           void *context,
                                           monad_error_t *error) {
    (void)space;
    ObsMonadRuntime *host = context;
    ObsMonadWindow *window = NULL;
    if (argc != 1 || !args || !out || !obs_host_owner(host) ||
        !obs_monad_runtime_mutation_allowed() ||
        !window_from_value(args[0], host, &window, error))
        return obs_host_invalid(error, "window-close! expects an open window capability");
    close_window(window);
    *out = monad_value_nil(host->space);
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static bool read_draw_args(size_t argc, monad_value_t *const *args,
                           ObsMonadRuntime *host, ObsMonadWindow **window,
                           double *values, size_t value_count,
                           monad_error_t *error) {
    if (!args || argc != value_count + 1 || !s_drawing_window ||
        !window_from_value(args[0], host, window, error) ||
        *window != s_drawing_window ||
        obs_editor_frame_current() != (*window)->frame)
        return false;
    for (size_t i = 0; i < value_count; ++i)
        if (monad_value_as_f64(args[i + 1], &values[i], error) != MONAD_OK ||
            !isfinite(values[i]) || fabs(values[i]) > 1000000.0)
            return false;
    return true;
}

static bool draw_color(const double *values, size_t start, Color *color) {
    for (size_t i = 0; i < 4; ++i)
        if (values[start + i] < 0.0 || values[start + i] > 1.0)
            return false;
    *color = (Color){(float)values[start], (float)values[start + 1],
                     (float)values[start + 2], (float)values[start + 3]};
    return true;
}

static monad_status_t draw_background(monad_object_space_t *space,
                                     size_t argc,
                                     monad_value_t *const *args,
                                     monad_value_t **out, void *ctx,
                                     monad_error_t *error) {
    (void)space;
    ObsMonadRuntime *host = ctx;
    ObsMonadWindow *window = NULL;
    double values[4];
    Color color;
    if (!out || !read_draw_args(argc, args, host, &window, values, 4, error) ||
        !draw_color(values, 0, &color))
        return obs_host_invalid(error, "window-background! requires RGBA values in [0,1] during that window's draw callback");
    quad2D((vec2){0.0f, 0.0f},
           (vec2){(float)context.drawExtent.width,
                  (float)context.drawExtent.height}, color);
    *out = monad_value_nil(host->space);
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t draw_rect(monad_object_space_t *space, size_t argc,
                                monad_value_t *const *args,
                                monad_value_t **out, void *ctx,
                                monad_error_t *error) {
    (void)space;
    ObsMonadRuntime *host = ctx;
    ObsMonadWindow *window = NULL;
    double values[8];
    Color color;
    if (!out || !read_draw_args(argc, args, host, &window, values, 8, error) ||
        values[2] < 0.0 || values[3] < 0.0 ||
        !draw_color(values, 4, &color))
        return obs_host_invalid(error, "window-rect! requires x, y, nonnegative width/height, and RGBA values in [0,1] during drawing");
    const float height = (float)context.drawExtent.height;
    quad2D((vec2){(float)values[0], height - (float)values[1] - (float)values[3]},
           (vec2){(float)values[2], (float)values[3]}, color);
    *out = monad_value_nil(host->space);
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t draw_circle(monad_object_space_t *space, size_t argc,
                                  monad_value_t *const *args,
                                  monad_value_t **out, void *ctx,
                                  monad_error_t *error) {
    (void)space;
    ObsMonadRuntime *host = ctx;
    ObsMonadWindow *window = NULL;
    double values[7];
    Color color;
    if (!out || !read_draw_args(argc, args, host, &window, values, 7, error) ||
        values[2] < 0.0 || !draw_color(values, 3, &color))
        return obs_host_invalid(error, "window-circle! requires x, y, nonnegative radius, and RGBA values in [0,1] during drawing");
    circle2D((vec2){(float)values[0],
                     (float)context.drawExtent.height - (float)values[1]},
             (float)values[2], color);
    *out = monad_value_nil(host->space);
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t draw_line(monad_object_space_t *space, size_t argc,
                                monad_value_t *const *args,
                                monad_value_t **out, void *ctx,
                                monad_error_t *error) {
    (void)space;
    ObsMonadRuntime *host = ctx;
    ObsMonadWindow *window = NULL;
    double values[8];
    Color color;
    if (!out || !read_draw_args(argc, args, host, &window, values, 8, error) ||
        !draw_color(values, 4, &color))
        return obs_host_invalid(error, "window-line! requires two points and RGBA values in [0,1] during drawing");
    const float height = (float)context.drawExtent.height;
    line2D((vec2){(float)values[0], height - (float)values[1]},
           (vec2){(float)values[2], height - (float)values[3]}, color);
    *out = monad_value_nil(host->space);
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_status_t draw_text(monad_object_space_t *space, size_t argc,
                                monad_value_t *const *args,
                                monad_value_t **out, void *ctx,
                                monad_error_t *error) {
    (void)space;
    ObsMonadRuntime *host = ctx;
    ObsMonadWindow *window = NULL;
    const char *value = NULL;
    size_t length = 0;
    double values[6];
    Color color;
    if (argc != 8 || !args || !out ||
        monad_value_bytes(args[1], &value, &length, error) != MONAD_OK ||
        !value || length > 4096 ||
        !window_from_value(args[0], host, &window, error) ||
        window != s_drawing_window ||
        obs_editor_frame_current() != window->frame)
        return obs_host_invalid(error, "window-text! expects a window capability and UTF-8 text during drawing");
    for (size_t i = 0; i < 6; ++i)
        if (monad_value_as_f64(args[i + 2], &values[i], error) != MONAD_OK ||
            !isfinite(values[i]) || fabs(values[i]) > 1000000.0)
            return obs_host_invalid(error, "window-text! expects finite coordinates and RGBA values in [0,1]");
    if (
        !draw_color(values, 2, &color))
        return obs_host_invalid(error, "window-text! expects text, x, y, RGBA during drawing");
    if (!editor.font)
        return obs_host_invalid(error, "editor font is unavailable");
    const float baseline = (float)context.drawExtent.height - (float)values[1];
    text(editor.font, value, (float)values[0], baseline, color);
    *out = monad_value_nil(host->space);
    return *out ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static bool input_call(ObsMonadWindow *window, const char *kind, int code,
                       int action, int mods, double x, double y) {
    if (!window || !window->active || window->closing || !window->input ||
        obs_editor_frame_is_closing(window->frame) ||
        obs_editor_frame_current() != window->frame)
        return false;
    ObsMonadRuntime *host = obs_monad_engine();
    if (!obs_host_owner(host)) return false;
    monad_error_t error = {0};
    monad_value_t *capability = window_ref(window, host, &error);
    monad_value_t *event = NULL, *code_value = NULL, *action_value = NULL;
    monad_value_t *mods_value = NULL, *x_value = NULL, *y_value = NULL;
    if (!capability ||
        monad_value_make_string(host->space, kind, strlen(kind), &event,
                                &error) != MONAD_OK ||
        monad_value_make_i64(host->space, code, &code_value, &error) != MONAD_OK ||
        monad_value_make_i64(host->space, action, &action_value, &error) != MONAD_OK ||
        monad_value_make_i64(host->space, mods, &mods_value, &error) != MONAD_OK ||
        monad_value_make_f64(host->space, x, &x_value, &error) != MONAD_OK ||
        monad_value_make_f64(host->space, y, &y_value, &error) != MONAD_OK) {
        monad_value_release(capability);
        monad_value_release(event);
        monad_value_release(code_value);
        monad_value_release(action_value);
        monad_value_release(mods_value);
        monad_value_release(x_value);
        monad_value_release(y_value);
        return false;
    }
    monad_value_t *arguments[] = {
        capability, event, code_value, action_value, mods_value, x_value, y_value
    };
    const bool ok = callback_call(window, window->input, 7, arguments, &error);
    monad_value_release(capability);
    monad_value_release(event);
    monad_value_release(code_value);
    monad_value_release(action_value);
    monad_value_release(mods_value);
    monad_value_release(x_value);
    monad_value_release(y_value);
    if (!ok) {
        fprintf(stderr, "Obsidian Monad window input: %s\n",
                error.message ? error.message : "callback failed");
        close_window(window);
    }
    return true;
}

bool obs_monad_window_is_surface(ObsEditorFrame *frame) {
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_windows[i].active && s_windows[i].frame == frame)
            return true;
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_live_windows[i].active && s_live_windows[i].frame == frame)
            return true;
    return false;
}

void obs_monad_window_render(ObsEditorFrame *frame) {
    ObsMonadWindow *window = NULL;
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_windows[i].active && !s_windows[i].closing &&
            s_windows[i].frame == frame) {
            window = &s_windows[i];
            break;
        }
    if (!window) {
        for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i) {
            ObsLiveMonadWindow *live = &s_live_windows[i];
            if (!live->active || live->closing || live->frame != frame ||
                !live->draw || obs_editor_frame_is_closing(frame))
                continue;
            RuntimeValue *capability = live_capability(live);
            RuntimeValue *arguments[] = {capability};
            if (obs_editor_frame_current() == frame) {
                s_live_drawing_window = live;
                if (!live_window_call(live, live->draw, 1, arguments))
                    close_live_window(live);
                s_live_drawing_window = NULL;
            }
            return;
        }
        return;
    }
    if (obs_editor_frame_current() != frame || !window->draw) return;
    ObsMonadRuntime *host = obs_monad_engine();
    monad_error_t error = {0};
    monad_value_t *capability = window_ref(window, host, &error);
    if (!capability) return;
    monad_value_t *arguments[] = {capability};
    s_drawing_window = window;
    const bool ok = callback_call(window, window->draw, 1, arguments, &error);
    s_drawing_window = NULL;
    monad_value_release(capability);
    if (!ok) {
        fprintf(stderr, "Obsidian Monad window draw: %s\n",
                error.message ? error.message : "callback failed");
        close_window(window);
    }
}

void obs_monad_window_frame_destroyed(ObsEditorFrame *frame) {
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_windows[i].active && s_windows[i].frame == frame)
            release_window(&s_windows[i]);
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i) {
        ObsLiveMonadWindow *window = &s_live_windows[i];
        if (!window->active || window->frame != frame) continue;
        free_live_callback_boxes(window);
        *window = (ObsLiveMonadWindow){0};
    }
}

static bool live_window_input(ObsLiveMonadWindow *window, const char *kind,
                              int code, int action, int mods,
                              double x, double y) {
    if (!window || !window->active || window->closing || !window->input ||
        obs_editor_frame_is_closing(window->frame) ||
        obs_editor_frame_current() != window->frame)
        return false;
    static RuntimeValue *event_values[5];
    static const char *event_names[5] = {
        "key", "char", "mouse-button", "mouse-move", "scroll"
    };
    size_t event_index = 0;
    while (event_index < 5 && strcmp(kind, event_names[event_index]) != 0)
        ++event_index;
    if (event_index == 5) return false;
    if (!event_values[event_index])
        event_values[event_index] = rt_value_string(kind);
    RuntimeValue *arguments[] = {
        live_capability(window), event_values[event_index], rt_value_int(code),
        rt_value_int(action), rt_value_int(mods), rt_value_float(x),
        rt_value_float(y)
    };
    const bool ok = live_window_call(window, window->input, 7, arguments);
    if (!ok) close_live_window(window);
    return ok;
}

bool obs_monad_window_key(ObsEditorFrame *frame, int key, int action,
                          int mods) {
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_windows[i].active && s_windows[i].frame == frame)
            return input_call(&s_windows[i], "key", key, action, mods, 0, 0);
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_live_windows[i].active && s_live_windows[i].frame == frame)
            return live_window_input(&s_live_windows[i], "key", key, action,
                                     mods, 0, 0);
    return false;
}

bool obs_monad_window_char(ObsEditorFrame *frame, unsigned int codepoint) {
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_windows[i].active && s_windows[i].frame == frame)
            return input_call(&s_windows[i], "char", (int)codepoint, 0, 0, 0, 0);
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_live_windows[i].active && s_live_windows[i].frame == frame)
            return live_window_input(&s_live_windows[i], "char",
                                     (int)codepoint, 0, 0, 0, 0);
    return false;
}

bool obs_monad_window_mouse_button(ObsEditorFrame *frame, int button,
                                   int action, int mods, double x, double y) {
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_windows[i].active && s_windows[i].frame == frame)
            return input_call(&s_windows[i], "mouse-button", button, action,
                              mods, x, y);
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_live_windows[i].active && s_live_windows[i].frame == frame)
            return live_window_input(&s_live_windows[i], "mouse-button",
                                     button, action, mods, x, y);
    return false;
}

bool obs_monad_window_mouse_move(ObsEditorFrame *frame, double x, double y) {
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_windows[i].active && s_windows[i].frame == frame)
            return input_call(&s_windows[i], "mouse-move", 0, 0, 0, x, y);
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_live_windows[i].active && s_live_windows[i].frame == frame)
            return live_window_input(&s_live_windows[i], "mouse-move",
                                     0, 0, 0, x, y);
    return false;
}

bool obs_monad_window_scroll(ObsEditorFrame *frame, double x_offset,
                             double y_offset) {
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_windows[i].active && s_windows[i].frame == frame)
            return input_call(&s_windows[i], "scroll", 0, 0, 0,
                              x_offset, y_offset);
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_live_windows[i].active && s_live_windows[i].frame == frame)
            return live_window_input(&s_live_windows[i], "scroll",
                                     0, 0, 0, x_offset, y_offset);
    return false;
}

void obs_monad_window_discard_live_callbacks(void) {
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i)
        if (s_live_windows[i].active)
            close_live_window(&s_live_windows[i]);
}

void obs_monad_window_live_runtime_closed(void) {
    while (s_live_capabilities) {
        ObsLiveWindowCapability *next = s_live_capabilities->next;
        free(s_live_capabilities);
        s_live_capabilities = next;
    }
}

void obs_monad_window_discard_callbacks(void) {
    obs_monad_window_discard_live_callbacks();
    for (size_t i = 0; i < MONAD_WINDOW_CAPACITY; ++i) {
        ObsMonadWindow *window = &s_windows[i];
        if (!window->active) continue;
        close_window(window);
        monad_value_release(window->draw);
        monad_value_release(window->input);
        window->draw = NULL;
        window->input = NULL;
    }
}

bool obs_monad_register_window_api(ObsMonadRuntime *host,
                                   monad_error_t *error) {
    static const ObsHostBinding bindings[] = {
        {"window-open!", "Open a focused tool window with Monad draw and input callbacks.", 5, open_window, NULL},
        {"window-close!", "Close a Monad tool window.", 1, close_window_binding, NULL},
        {"window-background!", "Paint the active tool window RGBA background.", 5, draw_background, NULL},
        {"window-rect!", "Draw a top-left-coordinate rectangle with RGBA color.", 9, draw_rect, NULL},
        {"window-circle!", "Draw a circle at top-left-coordinate center.", 8, draw_circle, NULL},
        {"window-line!", "Draw a line between top-left-coordinate points.", 9, draw_line, NULL},
        {"window-text!", "Draw UTF-8 text at a top-left-coordinate baseline.", 8, draw_text, NULL},
    };
    return obs_host_register(host, bindings,
                             sizeof(bindings) / sizeof(bindings[0]), error);
}
