#define _POSIX_C_SOURCE 200809L
#include "live_editor.h"
#include "command_catalog.h"
#include "runtime.h"
#include "bindings.h"
#include "add_menu.h"
#include "editor.h"
#include "editor_frames.h"
#include "gizmo.h"
#include "repl_panel.h"
#include "window_api.h"
#include "vulkan_setup.h"
#include "vertico.h"
#include "window.h"

#include <GLFW/glfw3.h>
#include <monad/embed.h>
#include "live_eval.h"
#include "mimg/control.h"
#include "qtt/type_identity.h"
#include "types.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OBS_LIVE_MAP_CAP 16u
#define OBS_LIVE_ACTION_CAP 128u
#define OBS_LIVE_KEY_OP_CAP 128u
#define OBS_LIVE_NAME_CAP 96u
#define OBS_LIVE_DOC_CAP 192u
#define OBS_LIVE_SOURCE_LIMIT (16u * 1024u * 1024u)

#define OBS_KEYMAP_MAGIC UINT64_C(0x4f42534b45594d50)
#define OBS_PANEL_MAGIC UINT64_C(0x4f4253454450414e)
#define OBS_GIZMO_MODE_MAGIC UINT64_C(0x4f425347495a4d44)
#define OBS_RENDER_SETTING_MAGIC UINT64_C(0x4f425352454e4452)
#define OBS_ADD_MENU_MAGIC UINT64_C(0x4f42534144444d4e)
#define OBS_EDITOR_MAGIC UINT64_C(0x4f42534544495452)
#define OBS_TRANSFORM_MODE_MAGIC UINT64_C(0x4f42535452414e53)
#define OBS_TRANSFORM_AXIS_MAGIC UINT64_C(0x4f42534158495331)

typedef struct ObsLiveKeyOperation {
    KeyImageOperation image;
    char notation[OBS_LIVE_NAME_CAP];
    char command_name[OBS_LIVE_NAME_CAP];
    char description[OBS_LIVE_DOC_CAP];
} ObsLiveKeyOperation;

typedef struct ObsLiveKeymap {
    const char *name;
    KeyChordMap *map;
    KeyChordMap owned_map;
    uint64_t magic;
    bool owns_map;
    bool registered;
    ObsLiveKeyOperation operations[OBS_LIVE_KEY_OP_CAP];
    size_t operation_count;
    KeyChordPreparedImage prepared;
    uint64_t transaction_id;
    bool module_owned;
} ObsLiveKeymap;

typedef struct ObsLiveAction {
    const char *name;
    ObsLiveEditorAction action;
    uint64_t published_generation;
    char symbol[64];
} ObsLiveAction;

typedef struct ObsLiveEditorPanel {
    uint64_t magic;
    PanelSide side;
    bool repl;
} ObsLiveEditorPanel;

typedef struct ObsLiveGizmoMode {
    uint64_t magic;
    GizmoMode mode;
} ObsLiveGizmoMode;

typedef struct ObsLiveRenderSetting {
    uint64_t magic;
    bool *value;
} ObsLiveRenderSetting;

typedef struct ObsLiveAddMenu {
    uint64_t magic;
} ObsLiveAddMenu;

typedef struct ObsLiveEditor {
    uint64_t magic;
} ObsLiveEditor;

typedef struct ObsLiveTransformMode {
    uint64_t magic;
    EditorTransformMode mode;
} ObsLiveTransformMode;

typedef struct ObsLiveTransformAxis {
    uint64_t magic;
    EditorTransformAxis axis;
} ObsLiveTransformAxis;

static ObsLiveKeymap s_maps[OBS_LIVE_MAP_CAP];
static ObsLiveAction s_actions[OBS_LIVE_ACTION_CAP];
static ObsLiveEditorPanel s_panels[] = {
    {.magic = OBS_PANEL_MAGIC, .side = PANEL_LEFT},
    {.magic = OBS_PANEL_MAGIC, .side = PANEL_RIGHT},
    {.magic = OBS_PANEL_MAGIC, .side = PANEL_BOTTOM, .repl = true},
};
static ObsLiveGizmoMode s_gizmo_modes[] = {
    {.magic = OBS_GIZMO_MODE_MAGIC, .mode = GIZMO_MODE_TRANSLATE},
    {.magic = OBS_GIZMO_MODE_MAGIC, .mode = GIZMO_MODE_ROTATE},
    {.magic = OBS_GIZMO_MODE_MAGIC, .mode = GIZMO_MODE_SCALE},
};
static ObsLiveRenderSetting s_render_settings[] = {
    {.magic = OBS_RENDER_SETTING_MAGIC, .value = &skyboxEnabled},
    {.magic = OBS_RENDER_SETTING_MAGIC, .value = &iblLightingEnabled},
    {.magic = OBS_RENDER_SETTING_MAGIC, .value = &shadowsEnabled},
    {.magic = OBS_RENDER_SETTING_MAGIC, .value = &cullingFrozen},
};
static ObsLiveAddMenu s_add_menu = {.magic = OBS_ADD_MENU_MAGIC};
static ObsLiveEditor s_editor_api = {.magic = OBS_EDITOR_MAGIC};
static ObsLiveTransformMode s_transform_modes[] = {
    {.magic = OBS_TRANSFORM_MODE_MAGIC,
     .mode = EDITOR_TRANSFORM_TRANSLATE},
    {.magic = OBS_TRANSFORM_MODE_MAGIC, .mode = EDITOR_TRANSFORM_ROTATE},
};
static ObsLiveTransformAxis s_transform_axes[] = {
    {.magic = OBS_TRANSFORM_AXIS_MAGIC, .axis = EDITOR_TRANSFORM_AXIS_VIEW},
    {.magic = OBS_TRANSFORM_AXIS_MAGIC, .axis = EDITOR_TRANSFORM_AXIS_X},
    {.magic = OBS_TRANSFORM_AXIS_MAGIC, .axis = EDITOR_TRANSFORM_AXIS_Y},
    {.magic = OBS_TRANSFORM_AXIS_MAGIC, .axis = EDITOR_TRANSFORM_AXIS_Z},
};
static size_t s_action_count;
static ObsMonadRuntime *s_host;
static MonadLiveEvaluator *s_evaluator;
static uint64_t s_symbol_sequence;
static bool s_keymap_transaction;
static bool s_keymap_images_prepared;
static bool s_module_loaded;
static uint64_t s_transaction_id;
static _Thread_local unsigned s_callback_depth;
static _Thread_local unsigned s_completion_depth;
static char *s_current_source;
static const int64_t *s_vertico_width;
static const int64_t *s_vertico_count;
static const int64_t *s_vertico_position;
static const int64_t *s_vertico_scroll_margin;
static RuntimeValue *s_vertico_resize_fixelwise;
static RuntimeValue *s_completion_record;

static bool register_map_function(ObsLiveKeymap *map);
static bool register_action_function(ObsLiveAction *entry);
static bool prepare_keymap_images(void);
static RuntimeValue *map_define_key(void *context, int argc,
                                   RuntimeValue **argv);
static RuntimeValue *prepare_keymaps(void *context, int argc,
                                     RuntimeValue **argv);

static void set_failure(const char **error, const char *message) {
    if (error) *error = message;
}

static char *serialize_scheme(Type *type, int quantified_count) {
    char *body = qtt_type_serialize(type);
    if (!body) return NULL;
    uint64_t fingerprint = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)body; *p; ++p) {
        fingerprint ^= *p;
        fingerprint *= UINT64_C(1099511628211);
    }
    if (!fingerprint) fingerprint = 1;
    size_t size = strlen(body) + 96;
    char *scheme = malloc(size);
    if (scheme)
        snprintf(scheme, size, "monad-hm-scheme-v1|%016llx|%d|%s",
                 (unsigned long long)fingerprint, quantified_count, body);
    free(body);
    return scheme;
}

static Type *keymap_type(void) {
    return type_from_name("ObsidianKeymap");
}

static Type *editor_panel_type(void) {
    return type_from_name("ObsidianEditorPanel");
}

static Type *gizmo_mode_type(void) {
    return type_from_name("ObsidianGizmoMode");
}

static Type *render_setting_type(void) {
    return type_from_name("ObsidianRenderSetting");
}

static Type *add_menu_type(void) {
    return type_from_name("ObsidianAddMenu");
}

static Type *editor_type(void) {
    return type_from_name("ObsidianEditor");
}

static Type *transform_mode_type(void) {
    return type_from_name("ObsidianTransformMode");
}

static Type *transform_axis_type(void) {
    return type_from_name("ObsidianTransformAxis");
}

static char *nullary_scheme(Type *result_type) {
    Type *type = type_fn_builtin(0, 0, false);
    if (!type || !result_type) {
        type_free(type);
        type_free(result_type);
        return NULL;
    }
    type_free(type->return_type);
    type->return_type = result_type;
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static char *typed_function_scheme(
    size_t arity, Type *(*const parameter_types[])(void),
    Type *(*result_type)(void)) {
    Type *type = type_fn_builtin((int)arity, 0, false);
    if (!type || !parameter_types || !result_type) {
        type_free(type);
        return NULL;
    }
    for (size_t i = 0; i < arity; ++i) {
        Type *parameter = parameter_types[i]();
        if (!parameter) {
            type_free(type);
            return NULL;
        }
        type_free(type->params[i].type);
        type->params[i].type = parameter;
    }
    Type *result = result_type();
    if (!result) {
        type_free(type);
        return NULL;
    }
    type_free(type->return_type);
    type->return_type = result;
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static char *map_set_scheme(void) {
    Type *type = type_fn_builtin(1, 0, false);
    if (!type) return NULL;
    type_free(type->params[0].type);
    type->params[0].type = keymap_type();
    type_free(type->return_type);
    type->return_type = type_bool();
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static char *define_key_scheme(void) {
    Type *type = type_fn_builtin(5, 0, false);
    if (!type) return NULL;
    type_free(type->params[0].type);
    type->params[0].type = keymap_type();
    type_free(type->params[1].type);
    type->params[1].type = type_string();
    type_free(type->params[2].type);
    type->params[2].type = type_var(0);
    type_free(type->params[3].type);
    type->params[3].type = type_string();
    type_free(type->params[4].type);
    type->params[4].type = type_int();
    type_free(type->return_type);
    type->return_type = type_bool();
    char *scheme = serialize_scheme(type, 1);
    type_free(type);
    return scheme;
}

static char *panel_open_query_scheme(void) {
    Type *type = type_fn_builtin(1, 0, false);
    if (!type) return NULL;
    type_free(type->params[0].type);
    type->params[0].type = editor_panel_type();
    type_free(type->return_type);
    type->return_type = type_bool();
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static char *panel_set_open_scheme(void) {
    Type *type = type_fn_builtin(2, 0, false);
    if (!type) return NULL;
    type_free(type->params[0].type);
    type->params[0].type = editor_panel_type();
    type_free(type->params[1].type);
    type->params[1].type = type_bool();
    type_free(type->return_type);
    type->return_type = type_bool();
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static char *gizmo_mode_set_scheme(void) {
    Type *type = type_fn_builtin(1, 0, false);
    if (!type) return NULL;
    type_free(type->params[0].type);
    type->params[0].type = gizmo_mode_type();
    type_free(type->return_type);
    type->return_type = type_bool();
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static char *render_setting_query_scheme(void) {
    Type *type = type_fn_builtin(1, 0, false);
    if (!type) return NULL;
    type_free(type->params[0].type);
    type->params[0].type = render_setting_type();
    type_free(type->return_type);
    type->return_type = type_bool();
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static char *render_setting_set_scheme(void) {
    Type *type = type_fn_builtin(2, 0, false);
    if (!type) return NULL;
    type_free(type->params[0].type);
    type->params[0].type = render_setting_type();
    type_free(type->params[1].type);
    type->params[1].type = type_bool();
    type_free(type->return_type);
    type->return_type = type_bool();
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static char *add_menu_query_scheme(void) {
    Type *type = type_fn_builtin(1, 0, false);
    if (!type) return NULL;
    type_free(type->params[0].type);
    type->params[0].type = add_menu_type();
    type_free(type->return_type);
    type->return_type = type_int();
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static char *add_menu_set_scheme(void) {
    Type *type = type_fn_builtin(2, 0, false);
    if (!type) return NULL;
    type_free(type->params[0].type);
    type->params[0].type = add_menu_type();
    type_free(type->params[1].type);
    type->params[1].type = type_int();
    type_free(type->return_type);
    type->return_type = type_bool();
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static bool publish_function(const char *name, const char *scheme,
                             void *address, void *context) {
    if (!s_evaluator || !name || !scheme || !address) return false;
    char symbol[80];
    snprintf(symbol, sizeof(symbol), "__obs_live_%llu",
             (unsigned long long)++s_symbol_sequence);
    return monad_live_evaluator_function_with_context(
        s_evaluator, name, symbol, scheme, address, context, true);
}

typedef enum {
    LIVE_WINDOW_INT,
    LIVE_WINDOW_FLOAT,
    LIVE_WINDOW_STRING,
    LIVE_WINDOW_HANDLE,
    LIVE_WINDOW_TYPE0,
    LIVE_WINDOW_TYPE1,
    LIVE_WINDOW_BOOL
} LiveWindowType;

static Type *live_window_type(LiveWindowType kind) {
    switch (kind) {
    case LIVE_WINDOW_INT: return type_int();
    case LIVE_WINDOW_FLOAT: return type_float();
    case LIVE_WINDOW_STRING: return type_string();
    case LIVE_WINDOW_HANDLE: return type_from_name("ObsidianMonadWindow");
    case LIVE_WINDOW_TYPE0: return type_var(0);
    case LIVE_WINDOW_TYPE1: return type_var(1);
    case LIVE_WINDOW_BOOL: return type_bool();
    }
    return NULL;
}

static char *live_window_scheme(const LiveWindowType *parameters,
                               size_t parameter_count, LiveWindowType result,
                               int quantified_count) {
    Type *function = type_fn_builtin((int)parameter_count, 0, false);
    if (!function || !parameters) {
        type_free(function);
        return NULL;
    }
    for (size_t i = 0; i < parameter_count; ++i) {
        Type *parameter = live_window_type(parameters[i]);
        if (!parameter) {
            type_free(function);
            return NULL;
        }
        type_free(function->params[i].type);
        function->params[i].type = parameter;
    }
    Type *return_type = live_window_type(result);
    if (!return_type) {
        type_free(function);
        return NULL;
    }
    type_free(function->return_type);
    function->return_type = return_type;
    char *scheme = serialize_scheme(function, quantified_count);
    type_free(function);
    return scheme;
}

bool obs_monad_window_register_live_api(MonadLiveEvaluator *evaluator) {
    if (!evaluator || evaluator != s_evaluator) return false;
    static const LiveWindowType open_parameters[] = {
        LIVE_WINDOW_STRING, LIVE_WINDOW_INT, LIVE_WINDOW_INT,
        LIVE_WINDOW_TYPE0, LIVE_WINDOW_TYPE1
    };
    static const LiveWindowType close_parameters[] = {LIVE_WINDOW_HANDLE};
    static const LiveWindowType background_parameters[] = {
        LIVE_WINDOW_HANDLE, LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT,
        LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT
    };
    static const LiveWindowType rect_parameters[] = {
        LIVE_WINDOW_HANDLE, LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT,
        LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT,
        LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT
    };
    static const LiveWindowType circle_parameters[] = {
        LIVE_WINDOW_HANDLE, LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT,
        LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT,
        LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT
    };
    static const LiveWindowType line_parameters[] = {
        LIVE_WINDOW_HANDLE, LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT,
        LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT,
        LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT
    };
    static const LiveWindowType text_parameters[] = {
        LIVE_WINDOW_HANDLE, LIVE_WINDOW_STRING, LIVE_WINDOW_FLOAT,
        LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT,
        LIVE_WINDOW_FLOAT, LIVE_WINDOW_FLOAT
    };
    struct {
        const char *name;
        const LiveWindowType *parameters;
        size_t arity;
        LiveWindowType result;
        int quantified;
        void *function;
    } bindings[] = {
        {"window-open!", open_parameters, 5, LIVE_WINDOW_HANDLE, 2,
         (void *)obs_monad_live_window_open_value},
        {"window-close!", close_parameters, 1, LIVE_WINDOW_BOOL, 0,
         (void *)obs_monad_live_window_close_value},
        {"window-background!", background_parameters, 5, LIVE_WINDOW_BOOL, 0,
         (void *)obs_monad_live_window_background_value},
        {"window-rect!", rect_parameters, 9, LIVE_WINDOW_BOOL, 0,
         (void *)obs_monad_live_window_rect_value},
        {"window-circle!", circle_parameters, 8, LIVE_WINDOW_BOOL, 0,
         (void *)obs_monad_live_window_circle_value},
        {"window-line!", line_parameters, 9, LIVE_WINDOW_BOOL, 0,
         (void *)obs_monad_live_window_line_value},
        {"window-text!", text_parameters, 8, LIVE_WINDOW_BOOL, 0,
         (void *)obs_monad_live_window_text_value}
    };
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); ++i) {
        char *scheme = live_window_scheme(
            bindings[i].parameters, bindings[i].arity,
            bindings[i].result, bindings[i].quantified);
        if (!scheme) return false;
        const bool published = publish_function(
            bindings[i].name, scheme, bindings[i].function, NULL);
        free(scheme);
        if (!published) return false;
    }
    return true;
}

static RuntimeValue *editor_frame_selected_value(void *context, int argc,
                                                 RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    editor_frame_selected();
    return rt_value_bool(1);
}

static RuntimeValue *editor_scene_buffers_value(void *context, int argc,
                                                RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(vertico_show_scene_buffers(false));
}

static RuntimeValue *editor_frame_scene_buffers_value(void *context, int argc,
                                                      RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(vertico_show_scene_buffers(true));
}

static RuntimeValue *editor_scene_find_value(void *context, int argc,
                                            RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(vertico_show_scene_file(false));
}

static RuntimeValue *editor_frame_scene_find_value(void *context, int argc,
                                                   RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(vertico_show_scene_file(true));
}

static RuntimeValue *editor_frame_create_value(void *context, int argc,
                                              RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(obs_editor_frame_create(NULL, true));
}

static RuntimeValue *editor_frame_close_value(void *context, int argc,
                                             RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(obs_editor_frame_close_current());
}

static RuntimeValue *editor_frame_close_others_value(void *context, int argc,
                                                     RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    obs_editor_frame_close_others();
    return rt_value_bool(1);
}

static RuntimeValue *editor_frame_other_value(void *context, int argc,
                                              RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    obs_editor_frame_focus_next();
    return rt_value_bool(1);
}

static RuntimeValue *editor_variables_value(void *context, int argc,
                                            RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(vertico_show_variables());
}

static RuntimeValue *editor_materials_value(void *context, int argc,
                                            RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(vertico_show_materials());
}

static RuntimeValue *editor_image_frame_open_value(void *context, int argc,
                                                   RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(vertico_show_image_file());
}

static RuntimeValue *editor_image_zoom_in_value(void *context, int argc,
                                                RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(obs_editor_frame_image_zoom(1.25f));
}

static RuntimeValue *editor_image_zoom_out_value(void *context, int argc,
                                                 RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(obs_editor_frame_image_zoom(0.8f));
}

static RuntimeValue *editor_image_fit_value(void *context, int argc,
                                            RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_bool(0);
    return rt_value_bool(obs_editor_frame_image_fit());
}

static bool register_editor_commands_api(void) {
    char *scheme = nullary_scheme(type_bool());
    if (!scheme) return false;
    static const struct {
        const char *name;
        void *function;
    } commands[] = {
        {"editor-frame-selected", (void *)editor_frame_selected_value},
        {"editor-scene-buffer-list", (void *)editor_scene_buffers_value},
        {"editor-frame-scene-buffer-list",
         (void *)editor_frame_scene_buffers_value},
        {"editor-scene-buffer-find", (void *)editor_scene_find_value},
        {"editor-frame-scene-buffer-find",
         (void *)editor_frame_scene_find_value},
        {"editor-frame-create", (void *)editor_frame_create_value},
        {"editor-frame-close!", (void *)editor_frame_close_value},
        {"editor-frame-close-others!", (void *)editor_frame_close_others_value},
        {"editor-frame-other!", (void *)editor_frame_other_value},
        {"editor-variable-list", (void *)editor_variables_value},
        {"editor-material-list", (void *)editor_materials_value},
        {"editor-image-frame-open", (void *)editor_image_frame_open_value},
        {"image-editor-zoom-in", (void *)editor_image_zoom_in_value},
        {"image-editor-zoom-out", (void *)editor_image_zoom_out_value},
        {"image-editor-fit", (void *)editor_image_fit_value},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (!publish_function(commands[i].name, scheme, commands[i].function,
                              NULL)) {
            ok = false;
            break;
        }
    free(scheme);
    return ok;
}

static ObsLiveEditorPanel *editor_panel_from_value(RuntimeValue *value) {
    if (!value || value->type != RT_OPAQUE || !value->data.opaque_val)
        return NULL;
    for (size_t i = 0; i < sizeof(s_panels) / sizeof(s_panels[0]); ++i) {
        if (value->data.opaque_val == &s_panels[i] &&
            s_panels[i].magic == OBS_PANEL_MAGIC)
            return &s_panels[i];
    }
    return NULL;
}

static bool panel_target_is_open(const ObsLiveEditorPanel *panel) {
    return panel->repl ? obs_repl_panel_active()
                       : editor_panel_is_open(panel->side);
}

static RuntimeValue *make_editor_panel_value(void *context, int argc,
                                             RuntimeValue **argv) {
    (void)argv;
    ObsLiveEditorPanel *panel = context;
    if (argc != 0 || !panel || panel->magic != OBS_PANEL_MAGIC)
        return rt_value_nil();
    return rt_value_opaque(panel);
}

static RuntimeValue *editor_panel_open_query(void *context, int argc,
                                             RuntimeValue **argv) {
    (void)context;
    ObsLiveEditorPanel *panel =
        argc == 1 && argv ? editor_panel_from_value(argv[0]) : NULL;
    if (!panel ||
        !s_host || !s_host->active || !obs_monad_on_owner_thread()) {
        monad_live_evaluator_raise_language_error(
            "editor-panel-open? expects an editor-panel capability on the owner thread");
        return rt_value_nil();
    }
    return rt_value_bool(panel_target_is_open(panel));
}

static RuntimeValue *editor_panel_set_open(void *context, int argc,
                                           RuntimeValue **argv) {
    (void)context;
    ObsLiveEditorPanel *panel =
        argc == 2 && argv ? editor_panel_from_value(argv[0]) : NULL;
    if (!panel || argc != 2 || !argv[1] || argv[1]->type != RT_BOOL ||
        !s_host || !s_host->active || !obs_monad_on_owner_thread() ||
        !s_callback_depth || s_keymap_transaction ||
        (!panel->repl && !editor.initialized)) {
        monad_live_evaluator_raise_language_error(
            "editor-panel-open! requires a valid panel, Bool state, and live command dispatch");
        return rt_value_nil();
    }
    const bool open = argv[1]->data.int_val != 0;
    if (panel->repl) {
        if (obs_repl_panel_active() != open) obs_repl_panel_toggle();
    } else if (open) {
        editor_open_panel(panel->side);
    } else {
        editor_close_panel(panel->side);
    }
    return rt_value_bool(panel_target_is_open(panel) == open);
}

static ObsLiveEditor *editor_from_value(RuntimeValue *value) {
    if (!value || value->type != RT_OPAQUE ||
        value->data.opaque_val != &s_editor_api ||
        s_editor_api.magic != OBS_EDITOR_MAGIC)
        return NULL;
    return &s_editor_api;
}

static ObsLiveTransformMode *transform_mode_from_value(RuntimeValue *value) {
    if (!value || value->type != RT_OPAQUE || !value->data.opaque_val)
        return NULL;
    for (size_t i = 0;
         i < sizeof(s_transform_modes) / sizeof(s_transform_modes[0]); ++i)
        if (value->data.opaque_val == &s_transform_modes[i] &&
            s_transform_modes[i].magic == OBS_TRANSFORM_MODE_MAGIC)
            return &s_transform_modes[i];
    return NULL;
}

static ObsLiveTransformAxis *transform_axis_from_value(RuntimeValue *value) {
    if (!value || value->type != RT_OPAQUE || !value->data.opaque_val)
        return NULL;
    for (size_t i = 0;
         i < sizeof(s_transform_axes) / sizeof(s_transform_axes[0]); ++i)
        if (value->data.opaque_val == &s_transform_axes[i] &&
            s_transform_axes[i].magic == OBS_TRANSFORM_AXIS_MAGIC)
            return &s_transform_axes[i];
    return NULL;
}

static bool editor_api_mutation_allowed(const char *name) {
    if (s_host && s_host->active && obs_monad_on_owner_thread() &&
        s_callback_depth && !s_keymap_transaction &&
        obs_monad_runtime_mutation_allowed())
        return true;
    monad_live_evaluator_raise_language_error(
        name ? name : "editor operation requires a live editor command");
    return false;
}

static RuntimeValue *make_editor_value(void *context, int argc,
                                       RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0 || !s_host || !s_host->active ||
        !obs_monad_on_owner_thread())
        return rt_value_nil();
    return rt_value_opaque(&s_editor_api);
}

static RuntimeValue *make_transform_mode_value(void *context, int argc,
                                               RuntimeValue **argv) {
    (void)argv;
    ObsLiveTransformMode *mode = context;
    if (argc != 0 || !mode || mode->magic != OBS_TRANSFORM_MODE_MAGIC)
        return rt_value_nil();
    return rt_value_opaque(mode);
}

static RuntimeValue *make_transform_axis_value(void *context, int argc,
                                               RuntimeValue **argv) {
    (void)argv;
    ObsLiveTransformAxis *axis = context;
    if (argc != 0 || !axis || axis->magic != OBS_TRANSFORM_AXIS_MAGIC)
        return rt_value_nil();
    return rt_value_opaque(axis);
}

static RuntimeValue *editor_transform_begin_value(void *context, int argc,
                                                  RuntimeValue **argv) {
    (void)context;
    ObsLiveEditor *editor_cap =
        argc == 2 && argv ? editor_from_value(argv[0]) : NULL;
    ObsLiveTransformMode *mode =
        argc == 2 && argv ? transform_mode_from_value(argv[1]) : NULL;
    if (!editor_cap || !mode) {
        monad_live_evaluator_raise_language_error(
            "editor-transform-begin! expects an editor and transform mode");
        return rt_value_nil();
    }
    if (!editor_api_mutation_allowed(
            "editor-transform-begin! requires live command dispatch"))
        return rt_value_nil();
    return rt_value_bool(editor_transform_begin(mode->mode));
}

static RuntimeValue *editor_transform_axis_value(void *context, int argc,
                                                 RuntimeValue **argv) {
    (void)context;
    ObsLiveEditor *editor_cap =
        argc == 2 && argv ? editor_from_value(argv[0]) : NULL;
    ObsLiveTransformAxis *axis =
        argc == 2 && argv ? transform_axis_from_value(argv[1]) : NULL;
    if (!editor_cap || !axis) {
        monad_live_evaluator_raise_language_error(
            "editor-transform-axis! expects an editor and transform axis");
        return rt_value_nil();
    }
    if (!editor_api_mutation_allowed(
            "editor-transform-axis! requires live command dispatch"))
        return rt_value_nil();
    return rt_value_bool(editor_transform_set_axis(axis->axis));
}

static RuntimeValue *editor_transform_mode_value(void *context, int argc,
                                                 RuntimeValue **argv) {
    (void)context;
    ObsLiveEditor *editor_cap =
        argc == 2 && argv ? editor_from_value(argv[0]) : NULL;
    ObsLiveTransformMode *mode =
        argc == 2 && argv ? transform_mode_from_value(argv[1]) : NULL;
    if (!editor_cap || !mode) {
        monad_live_evaluator_raise_language_error(
            "editor-transform-mode! expects an editor and transform mode");
        return rt_value_nil();
    }
    if (!editor_api_mutation_allowed(
            "editor-transform-mode! requires live command dispatch"))
        return rt_value_nil();
    return rt_value_bool(editor_transform_set_mode(mode->mode));
}

static RuntimeValue *editor_transform_snap_value(void *context, int argc,
                                                 RuntimeValue **argv) {
    (void)context;
    ObsLiveEditor *editor_cap =
        argc == 2 && argv ? editor_from_value(argv[0]) : NULL;
    if (!editor_cap || argc != 2 || !argv[1] ||
        argv[1]->type != RT_FLOAT) {
        monad_live_evaluator_raise_language_error(
            "editor-transform-snap! expects an editor and Float step");
        return rt_value_nil();
    }
    if (!editor_api_mutation_allowed(
            "editor-transform-snap! requires live command dispatch"))
        return rt_value_nil();
    return rt_value_bool(
        editor_transform_set_snap((float)rt_unbox_float(argv[1])));
}

static RuntimeValue *editor_transform_origin_value(void *context, int argc,
                                                   RuntimeValue **argv) {
    (void)context;
    ObsLiveEditor *editor_cap =
        argc == 1 && argv ? editor_from_value(argv[0]) : NULL;
    if (!editor_cap) {
        monad_live_evaluator_raise_language_error(
            "editor-transform-origin! expects an editor capability");
        return rt_value_nil();
    }
    if (!editor_api_mutation_allowed(
            "editor-transform-origin! requires live command dispatch"))
        return rt_value_nil();
    return rt_value_bool(editor_transform_reset_origin());
}

static RuntimeValue *editor_transform_finish_value(void *context, int argc,
                                                   RuntimeValue **argv) {
    (void)context;
    ObsLiveEditor *editor_cap =
        argc == 2 && argv ? editor_from_value(argv[0]) : NULL;
    if (!editor_cap || argc != 2 || !argv[1] ||
        argv[1]->type != RT_BOOL) {
        monad_live_evaluator_raise_language_error(
            "editor-transform-finish! expects an editor and Boolean commit flag");
        return rt_value_nil();
    }
    if (!editor_api_mutation_allowed(
            "editor-transform-finish! requires live command dispatch"))
        return rt_value_nil();
    return rt_value_bool(
        editor_transform_finish(argv[1]->data.int_val != 0));
}

static RuntimeValue *make_gizmo_mode_value(void *context, int argc,
                                           RuntimeValue **argv) {
    (void)argv;
    ObsLiveGizmoMode *mode = context;
    if (argc != 0 || !mode || mode->magic != OBS_GIZMO_MODE_MAGIC)
        return rt_value_nil();
    return rt_value_opaque(mode);
}

static ObsLiveGizmoMode *gizmo_mode_from_value(RuntimeValue *value) {
    if (!value || value->type != RT_OPAQUE || !value->data.opaque_val)
        return NULL;
    for (size_t i = 0; i < sizeof(s_gizmo_modes) / sizeof(s_gizmo_modes[0]); ++i)
        if (value->data.opaque_val == &s_gizmo_modes[i] &&
            s_gizmo_modes[i].magic == OBS_GIZMO_MODE_MAGIC)
            return &s_gizmo_modes[i];
    return NULL;
}

static RuntimeValue *set_gizmo_mode(void *context, int argc,
                                    RuntimeValue **argv) {
    (void)context;
    ObsLiveGizmoMode *mode =
        argc == 1 && argv ? gizmo_mode_from_value(argv[0]) : NULL;
    if (!mode || !s_host || !s_host->active ||
        !obs_monad_on_owner_thread() || !s_callback_depth ||
        s_keymap_transaction) {
        monad_live_evaluator_raise_language_error(
            "gizmo-mode! requires a valid mode during live command dispatch");
        return rt_value_nil();
    }
    gizmo.mode = mode->mode;
    return rt_value_bool(1);
}

static RuntimeValue *make_render_setting_value(void *context, int argc,
                                               RuntimeValue **argv) {
    (void)argv;
    ObsLiveRenderSetting *setting = context;
    if (argc != 0 || !setting ||
        setting->magic != OBS_RENDER_SETTING_MAGIC || !setting->value)
        return rt_value_nil();
    return rt_value_opaque(setting);
}

static ObsLiveRenderSetting *render_setting_from_value(RuntimeValue *value) {
    if (!value || value->type != RT_OPAQUE || !value->data.opaque_val)
        return NULL;
    for (size_t i = 0;
         i < sizeof(s_render_settings) / sizeof(s_render_settings[0]); ++i)
        if (value->data.opaque_val == &s_render_settings[i] &&
            s_render_settings[i].magic == OBS_RENDER_SETTING_MAGIC &&
            s_render_settings[i].value)
            return &s_render_settings[i];
    return NULL;
}

static RuntimeValue *render_setting_query(void *context, int argc,
                                          RuntimeValue **argv) {
    (void)context;
    ObsLiveRenderSetting *setting =
        argc == 1 && argv ? render_setting_from_value(argv[0]) : NULL;
    if (!setting || !s_host || !s_host->active ||
        !obs_monad_on_owner_thread()) {
        monad_live_evaluator_raise_language_error(
            "render-setting? expects a live render-setting capability on the owner thread");
        return rt_value_nil();
    }
    return rt_value_bool(*setting->value);
}

static RuntimeValue *render_setting_set(void *context, int argc,
                                        RuntimeValue **argv) {
    (void)context;
    ObsLiveRenderSetting *setting =
        argc == 2 && argv ? render_setting_from_value(argv[0]) : NULL;
    if (!setting || !argv[1] || argv[1]->type != RT_BOOL ||
        !s_host || !s_host->active || !obs_monad_on_owner_thread() ||
        !s_callback_depth || s_keymap_transaction) {
        monad_live_evaluator_raise_language_error(
            "render-setting! requires a live setting, Bool value and live command dispatch");
        return rt_value_nil();
    }
    *setting->value = argv[1]->data.int_val != 0;
    return rt_value_bool(1);
}

static RuntimeValue *make_add_menu_value(void *context, int argc,
                                         RuntimeValue **argv) {
    (void)argv;
    ObsLiveAddMenu *menu = context;
    if (argc != 0 || menu != &s_add_menu ||
        menu->magic != OBS_ADD_MENU_MAGIC)
        return rt_value_nil();
    return rt_value_opaque(menu);
}

static bool add_menu_value_valid(RuntimeValue *value) {
    return value && value->type == RT_OPAQUE &&
           value->data.opaque_val == &s_add_menu &&
           s_add_menu.magic == OBS_ADD_MENU_MAGIC;
}

static RuntimeValue *add_menu_visible_count_value(void *context, int argc,
                                                  RuntimeValue **argv) {
    (void)context;
    if (argc != 1 || !argv || !add_menu_value_valid(argv[0]) ||
        !s_host || !s_host->active || !obs_monad_on_owner_thread()) {
        monad_live_evaluator_raise_language_error(
            "add-menu-visible-count expects a live Add Menu capability");
        return rt_value_nil();
    }
    return rt_value_int(add_menu_visible_count());
}

static RuntimeValue *add_menu_selection_index_value(void *context, int argc,
                                                    RuntimeValue **argv) {
    (void)context;
    if (argc != 1 || !argv || !add_menu_value_valid(argv[0]) ||
        !s_host || !s_host->active || !obs_monad_on_owner_thread()) {
        monad_live_evaluator_raise_language_error(
            "add-menu-selection-index expects a live Add Menu capability");
        return rt_value_nil();
    }
    return rt_value_int(add_menu_selection_index());
}

static RuntimeValue *add_menu_set_selection_index_value(void *context,
                                                        int argc,
                                                        RuntimeValue **argv) {
    (void)context;
    int64_t index = -1;
    if (argc != 2 || !argv || !add_menu_value_valid(argv[0]) ||
        !argv[1] || argv[1]->type != RT_INT ||
        !s_host || !s_host->active || !obs_monad_on_owner_thread() ||
        !s_callback_depth || s_keymap_transaction) {
        monad_live_evaluator_raise_language_error(
            "add-menu-set-selection-index! requires a bounded index during live command dispatch");
        return rt_value_nil();
    }
    index = argv[1]->data.int_val;
    if (index < 0 || index > INT32_MAX) {
        monad_live_evaluator_raise_language_error(
            "add-menu-set-selection-index! requires a bounded index");
        return rt_value_nil();
    }
    return rt_value_bool(add_menu_set_selection_index((int32_t)index));
}

static RuntimeValue *vertico_filtered_count_value(void *context, int argc,
                                                  RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0 || !s_host || !s_host->active ||
        !obs_monad_on_owner_thread()) {
        monad_live_evaluator_raise_language_error(
            "vertico-filtered-count requires the live editor owner thread");
        return rt_value_nil();
    }
    return rt_value_int((int64_t)vertico_filtered_count());
}

static RuntimeValue *vertico_selection_index_value(void *context, int argc,
                                                   RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0 || !s_host || !s_host->active ||
        !obs_monad_on_owner_thread()) {
        monad_live_evaluator_raise_language_error(
            "vertico-selection-index requires the live editor owner thread");
        return rt_value_nil();
    }
    return rt_value_int((int64_t)vertico_selection_index());
}

static RuntimeValue *vertico_scroll_offset_value(void *context, int argc,
                                                 RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0 || !s_host || !s_host->active ||
        !obs_monad_on_owner_thread()) {
        monad_live_evaluator_raise_language_error(
            "vertico-scroll-offset requires the live editor owner thread");
        return rt_value_nil();
    }
    return rt_value_int(vertico_scroll_offset());
}

static RuntimeValue *vertico_visible_count_value(void *context, int argc,
                                                 RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0 || !s_host || !s_host->active ||
        !obs_monad_on_owner_thread()) {
        monad_live_evaluator_raise_language_error(
            "vertico-visible-count requires the live editor owner thread");
        return rt_value_nil();
    }
    return rt_value_int(vertico_visible_count());
}

static RuntimeValue *vertico_orderless_score_value(void *context, int argc,
                                                   RuntimeValue **argv) {
    (void)context;
    if (argc != 2 || !argv || !argv[0] || !argv[1] ||
        argv[0]->type != RT_STRING || argv[1]->type != RT_STRING ||
        !s_host || !s_host->active || !obs_monad_on_owner_thread()) {
        monad_live_evaluator_raise_language_error(
            "vertico-orderless-score expects two strings on the editor owner thread");
        return rt_value_nil();
    }
    return rt_value_float(vertico_fuzzy_match(argv[0]->data.string_val,
                                              argv[1]->data.string_val));
}

static RuntimeValue *vertico_set_position_value(void *context, int argc,
                                               RuntimeValue **argv) {
    (void)context;
    if (argc != 2 || !argv || !argv[0] || !argv[1] ||
        argv[0]->type != RT_INT || argv[1]->type != RT_INT ||
        !s_host || !s_host->active || !obs_monad_on_owner_thread() ||
        !s_callback_depth || s_keymap_transaction) {
        monad_live_evaluator_raise_language_error(
            "vertico-set-position! requires two integer positions during live command dispatch");
        return rt_value_nil();
    }
    const int64_t selected = argv[0]->data.int_val;
    const int64_t offset = argv[1]->data.int_val;
    if (selected < 0 || selected > INT32_MAX || offset < 0 ||
        offset > INT32_MAX) {
        monad_live_evaluator_raise_language_error(
            "vertico-set-position! requires nonnegative bounded positions");
        return rt_value_nil();
    }
    return rt_value_bool(
        vertico_set_position((size_t)selected, (int32_t)offset));
}

static char *vertico_position_set_scheme(void) {
    Type *type = type_fn_builtin(2, 0, false);
    if (!type) return NULL;
    type_free(type->params[0].type);
    type->params[0].type = type_int();
    type_free(type->params[1].type);
    type->params[1].type = type_int();
    type_free(type->return_type);
    type->return_type = type_bool();
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    return scheme;
}

static bool capture_vertico_options(void) {
    const void *width = NULL;
    const void *count = NULL;
    const void *position = NULL;
    const void *scroll_margin = NULL;
    size_t width_size = 0, count_size = 0, position_size = 0;
    size_t scroll_margin_size = 0;
    MonadLiveScalarKind width_kind = MONAD_LIVE_SCALAR_NONE;
    MonadLiveScalarKind count_kind = MONAD_LIVE_SCALAR_NONE;
    MonadLiveScalarKind position_kind = MONAD_LIVE_SCALAR_NONE;
    MonadLiveScalarKind scroll_margin_kind = MONAD_LIVE_SCALAR_NONE;
    const bool width_found = s_evaluator && monad_live_evaluator_binding_scalar(
        s_evaluator, "vertico-width", &width, &width_size, &width_kind);
    const bool count_found = s_evaluator && monad_live_evaluator_binding_scalar(
        s_evaluator, "vertico-count", &count, &count_size, &count_kind);
    const bool position_found = s_evaluator &&
        monad_live_evaluator_binding_scalar(
            s_evaluator, "vertico-position", &position, &position_size,
            &position_kind);
    const bool scroll_margin_found = s_evaluator &&
        monad_live_evaluator_binding_scalar(
            s_evaluator, "vertico-scroll-margin", &scroll_margin,
            &scroll_margin_size, &scroll_margin_kind);
    if (!width_found || !count_found || !position_found ||
        !scroll_margin_found ||
        width_kind != MONAD_LIVE_SCALAR_SIGNED ||
        count_kind != MONAD_LIVE_SCALAR_SIGNED ||
        position_kind != MONAD_LIVE_SCALAR_SIGNED ||
        scroll_margin_kind != MONAD_LIVE_SCALAR_SIGNED ||
        width_size < sizeof(int64_t) || count_size < sizeof(int64_t) ||
        position_size < sizeof(int64_t) ||
        scroll_margin_size < sizeof(int64_t)) {
        fprintf(stderr,
            "Monad Vertico option reflection failed: width=%d/%d/%zu "
            "count=%d/%d/%zu position=%d/%d/%zu scroll-margin=%d/%d/%zu\n",
            width_found, (int)width_kind, width_size,
            count_found, (int)count_kind, count_size,
            position_found, (int)position_kind, position_size,
            scroll_margin_found, (int)scroll_margin_kind,
            scroll_margin_size);
        return false;
    }
    s_vertico_width = width;
    s_vertico_count = count;
    s_vertico_position = position;
    s_vertico_scroll_margin = scroll_margin;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "vertico-resize-fixelwise",
            &s_vertico_resize_fixelwise) ||
        !s_vertico_resize_fixelwise ||
        s_vertico_resize_fixelwise->type != RT_BOOL)
        return false;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "completion-history-record", &s_completion_record) ||
        !s_completion_record || s_completion_record->type != RT_CLOSURE ||
        !s_completion_record->data.closure_val ||
        s_completion_record->data.closure_val->arity != 3)
        return false;
    return true;
}

static bool capture_default_completion_strategy(void) {
    RuntimeValue *strategy = NULL;
    if (!s_evaluator ||
        !monad_live_evaluator_binding_value(
            s_evaluator, "obsidian-orderless-score", &strategy) ||
        !strategy || strategy->type != RT_CLOSURE ||
        !strategy->data.closure_val ||
        strategy->data.closure_val->arity != 2)
        return false;
    return true;
}

static RuntimeValue *make_keymap_value(void *context, int argc,
                                       RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_nil();
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        ObsLiveKeymap *slot = &s_maps[i];
        if (slot->registered || slot->magic == OBS_KEYMAP_MAGIC) continue;
        *slot = (ObsLiveKeymap){0};
        keymap_init(&slot->owned_map);
        slot->map = &slot->owned_map;
        slot->owns_map = true;
        slot->magic = OBS_KEYMAP_MAGIC;
        slot->module_owned = s_keymap_transaction;
        slot->transaction_id = s_keymap_transaction ? s_transaction_id : 0;
        return rt_value_opaque(slot);
    }
    monad_live_evaluator_raise_language_error(
        "Obsidian keymap capacity exhausted");
    return rt_value_nil();
}

static RuntimeValue *get_keymap_value(void *context, int argc,
                                      RuntimeValue **argv) {
    (void)argv;
    ObsLiveKeymap *map = context;
    if (argc != 0 || !map || map->magic != OBS_KEYMAP_MAGIC || !map->map)
        return rt_value_nil();
    return rt_value_opaque(map);
}

static ObsLiveKeymap *keymap_from_value(RuntimeValue *value) {
    if (!value || value->type != RT_OPAQUE || !value->data.opaque_val)
        return NULL;
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        if (value->data.opaque_val == &s_maps[i] &&
            s_maps[i].magic == OBS_KEYMAP_MAGIC && s_maps[i].map)
            return &s_maps[i];
    }
    return NULL;
}

static bool copy_runtime_string(RuntimeValue *value, char *out,
                                size_t capacity, bool allow_empty) {
    if (!value || value->type != RT_STRING || !value->data.string_val ||
        !out || capacity == 0)
        return false;
    size_t length = strlen(value->data.string_val);
    if ((!allow_empty && length == 0) || length >= capacity)
        return false;
    memcpy(out, value->data.string_val, length + 1);
    return true;
}

static monad_status_t call_live_closure(monad_object_space_t *space,
                                        size_t argc,
                                        monad_value_t *const *argv,
                                        monad_value_t **result,
                                        void *context,
                                        monad_error_t *error) {
    (void)argv;
    RuntimeValue *closure = context;
    if (argc != 0 || !result || !s_evaluator || !closure ||
        closure->type != RT_CLOSURE || !closure->data.closure_val)
        return MONAD_ERROR_INVALID_ARGUMENT;
    if (s_callback_depth >= 32) {
        if (error) {
            error->status = MONAD_ERROR_INTERNAL;
            error->message = "Monad editor command recursion limit exceeded";
        }
        return MONAD_ERROR_INTERNAL;
    }
    const int arity = closure->data.closure_val->arity;
    if (arity < 0 || arity > 1) {
        if (error) {
            error->status = MONAD_ERROR_INVALID_ARGUMENT;
            error->message = "editor key commands must take zero arguments";
        }
        return MONAD_ERROR_INVALID_ARGUMENT;
    }
    RuntimeValue *unit_arg = rt_value_int(0);
    RuntimeValue *arguments[] = {unit_arg};
    RuntimeValue *returned = NULL;
    ++s_callback_depth;
    bool ok = monad_live_evaluator_call_closure_guarded(
        s_evaluator, closure, arity, arity ? arguments : NULL, &returned);
    --s_callback_depth;
    if (!ok) {
        if (error) {
            error->status = MONAD_ERROR_INTERNAL;
            error->message = monad_live_evaluator_last_diagnostic(s_evaluator);
        }
        return MONAD_ERROR_INTERNAL;
    }
    *result = monad_value_nil(space);
    return *result ? MONAD_OK : MONAD_ERROR_OUT_OF_MEMORY;
}

static monad_value_t *wrap_live_command(RuntimeValue *closure,
                                        const char *description) {
    if (!closure || closure->type != RT_CLOSURE ||
        !closure->data.closure_val || closure->data.closure_val->arity < 0 ||
        closure->data.closure_val->arity > 1 || !s_host || !s_host->space)
        return NULL;
    monad_dynamic_procedure_descriptor_t descriptor;
    monad_dynamic_procedure_descriptor_init(&descriptor);
    descriptor.name = closure->data.closure_val->name
                          ? closure->data.closure_val->name
                          : "<monad-editor-command>";
    descriptor.docstring = description ? description : "";
    descriptor.minimum_arity = 0;
    descriptor.maximum_arity = 0;
    descriptor.function = call_live_closure;
    descriptor.context = closure;
    monad_value_t *procedure = NULL;
    monad_error_t error = {0};
    if (monad_value_make_procedure(s_host->space, &descriptor, &procedure,
                                   &error) != MONAD_OK)
        return NULL;
    return procedure;
}

static bool append_key_operation(ObsLiveKeymap *map, const char *notation,
                                 RuntimeValue *closure, const char *description,
                                 int action_type) {
    if (map->operation_count == OBS_LIVE_KEY_OP_CAP) return false;
    char name[OBS_LIVE_NAME_CAP];
    const char *closure_name = closure->data.closure_val->name;
    if (!closure_name || !*closure_name) closure_name = "monad-editor-command";
    if (strlen(closure_name) >= sizeof(name)) return false;
    memcpy(name, closure_name, strlen(closure_name) + 1);
    monad_value_t *procedure = wrap_live_command(closure, description);
    if (!procedure) return false;
    const size_t index = map->operation_count++;
    ObsLiveKeyOperation *operation = &map->operations[index];
    memcpy(operation->notation, notation, strlen(notation) + 1);
    memcpy(operation->command_name, name, strlen(name) + 1);
    snprintf(operation->description, sizeof(operation->description), "%s",
             description ? description : name);
    operation->image = (KeyImageOperation){
        .action = KEY_IMAGE_BIND,
        .notation = operation->notation,
        .procedure = procedure,
        .command_name = operation->command_name,
        .description = operation->description,
        .managed_monad = true,
        .action_type_flag = action_type
    };
    if (!obs_monad_live_command_register(operation->command_name, procedure)) {
        monad_value_release(procedure);
        --map->operation_count;
        return false;
    }
    return true;
}

static RuntimeValue *map_define_key(void *context, int argc,
                                    RuntimeValue **argv) {
    (void)context;
    if (argc != 5 || !argv || !s_host || !obs_monad_on_owner_thread()) {
        monad_live_evaluator_raise_language_error(
            "define-key expects a keymap, notation, command, description, and repeat flag");
        return rt_value_nil();
    }
    ObsLiveKeymap *map = keymap_from_value(argv[0]);
    char notation[OBS_LIVE_NAME_CAP];
    char description[OBS_LIVE_DOC_CAP];
    if (!map || !copy_runtime_string(argv[1], notation, sizeof(notation), false) ||
        !copy_runtime_string(argv[3], description, sizeof(description), true) ||
        !argv[2] || argv[2]->type != RT_CLOSURE ||
        !argv[4] || argv[4]->type != RT_INT ||
        (argv[4]->data.int_val != 0 && argv[4]->data.int_val != 1)) {
        monad_live_evaluator_raise_language_error(
            "define-key received an invalid foreign keymap or command");
        return rt_value_nil();
    }
    KeyChord parsed = {0};
    if (!parse_keychord_notation(notation, &parsed)) {
        monad_live_evaluator_raise_language_error("invalid key notation");
        return rt_value_nil();
    }
    const int action_type = GLFW_PRESS |
        (argv[4]->data.int_val ? GLFW_REPEAT : 0);
    if (s_keymap_transaction) {
        if (!append_key_operation(map, notation, argv[2], description,
                                  action_type)) {
            monad_live_evaluator_raise_language_error(
                "keymap transaction capacity or command catalog exhausted");
            return rt_value_nil();
        }
    } else {
        monad_value_t *procedure = wrap_live_command(argv[2], description);
        const char *name = argv[2]->data.closure_val->name;
        if (!procedure || !name ||
            !keychord_bind_managed_monad_named(
                map->map, notation, procedure, name, description, action_type) ||
            !obs_monad_live_command_register(name, procedure)) {
            monad_value_release(procedure);
            monad_live_evaluator_raise_language_error(
                "could not publish editor key binding");
            return rt_value_nil();
        }
        monad_value_release(procedure);
    }
    return rt_value_bool(1);
}

static RuntimeValue *set_overriding_map(void *context, int argc,
                                        RuntimeValue **argv) {
    (void)context;
    ObsLiveKeymap *map = argc == 1 && argv ? keymap_from_value(argv[0]) : NULL;
    if (!map) {
        monad_live_evaluator_raise_language_error(
            "keymap-set-overriding expects a live editor keymap");
        return rt_value_nil();
    }
    keymap_set_overriding(map->map);
    return rt_value_bool(1);
}

static RuntimeValue *clear_overriding_map(void *context, int argc,
                                          RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0) return rt_value_nil();
    keymap_clear_overriding();
    return rt_value_bool(1);
}

static RuntimeValue *prepare_keymaps(void *context, int argc,
                                     RuntimeValue **argv) {
    (void)context;
    (void)argv;
    if (argc != 0 || !s_keymap_transaction ||
        !prepare_keymap_images()) {
        monad_live_evaluator_raise_language_error(
            "could not prepare the complete editor keymap image");
        return rt_value_nil();
    }
    s_keymap_images_prepared = true;
    return rt_value_bool(1);
}

static RuntimeValue *invoke_registered_action(void *context, int argc,
                                              RuntimeValue **argv) {
    (void)argv;
    ObsLiveAction *entry = context;
    if (!entry || !entry->action || argc != 0) return rt_value_nil();
    entry->action();
    return rt_value_int(0);
}

static bool register_action_function(ObsLiveAction *entry) {
    if (!entry || !s_evaluator || !s_host) return false;
    if (entry->published_generation == s_host->generation) return true;
    Type *type = type_fn_builtin(0, 0, false);
    if (!type) return false;
    type_free(type->return_type);
    type->return_type = type_int();
    char *scheme = serialize_scheme(type, 0);
    type_free(type);
    if (!scheme) return false;
    snprintf(entry->symbol, sizeof(entry->symbol),
             "__obs_editor_action_%zu", (size_t)(entry - s_actions));
    bool ok = monad_live_evaluator_function_with_context(
        s_evaluator, entry->name, entry->symbol, scheme,
        (void *)invoke_registered_action, entry, true);
    free(scheme);
    if (ok) entry->published_generation = s_host->generation;
    return ok;
}

static bool register_editor_transform_api(void) {
    static Type *(*const begin_params[])(void) = {
        editor_type, transform_mode_type
    };
    static Type *(*const axis_params[])(void) = {
        editor_type, transform_axis_type
    };
    static Type *(*const mode_params[])(void) = {
        editor_type, transform_mode_type
    };
    static Type *(*const snap_params[])(void) = {
        editor_type, type_float
    };
    static Type *(*const origin_params[])(void) = {editor_type};
    static Type *(*const finish_params[])(void) = {
        editor_type, type_bool
    };
    char *editor_scheme = nullary_scheme(editor_type());
    char *begin_scheme =
        typed_function_scheme(2, begin_params, type_bool);
    char *axis_scheme =
        typed_function_scheme(2, axis_params, type_bool);
    char *mode_scheme =
        typed_function_scheme(2, mode_params, type_bool);
    char *snap_scheme =
        typed_function_scheme(2, snap_params, type_bool);
    char *origin_scheme =
        typed_function_scheme(1, origin_params, type_bool);
    char *finish_scheme =
        typed_function_scheme(2, finish_params, type_bool);
    if (!editor_scheme || !begin_scheme || !axis_scheme || !mode_scheme ||
        !snap_scheme || !origin_scheme || !finish_scheme) {
        free(editor_scheme);
        free(begin_scheme);
        free(axis_scheme);
        free(mode_scheme);
        free(snap_scheme);
        free(origin_scheme);
        free(finish_scheme);
        return false;
    }

    bool ok = publish_function("obsidian-editor", editor_scheme,
                               (void *)make_editor_value, NULL) &&
              publish_function("editor-transform-begin!", begin_scheme,
                               (void *)editor_transform_begin_value, NULL) &&
              publish_function("editor-transform-axis!", axis_scheme,
                               (void *)editor_transform_axis_value, NULL) &&
              publish_function("editor-transform-mode!", mode_scheme,
                               (void *)editor_transform_mode_value, NULL) &&
              publish_function("editor-transform-snap!", snap_scheme,
                               (void *)editor_transform_snap_value, NULL) &&
              publish_function("editor-transform-origin!", origin_scheme,
                               (void *)editor_transform_origin_value, NULL) &&
              publish_function("editor-transform-finish!", finish_scheme,
                               (void *)editor_transform_finish_value, NULL);
    free(editor_scheme);
    free(begin_scheme);
    free(axis_scheme);
    free(mode_scheme);
    free(snap_scheme);
    free(origin_scheme);
    free(finish_scheme);
    if (!ok) return false;

    static const char *const mode_names[] = {
        "obsidian-transform-translate",
        "obsidian-transform-rotate",
    };
    char *mode_constant_scheme = nullary_scheme(transform_mode_type());
    if (!mode_constant_scheme) return false;
    for (size_t i = 0; i < sizeof(mode_names) / sizeof(mode_names[0]); ++i) {
        if (!publish_function(mode_names[i], mode_constant_scheme,
                              (void *)make_transform_mode_value,
                              &s_transform_modes[i])) {
            free(mode_constant_scheme);
            return false;
        }
    }
    free(mode_constant_scheme);

    static const char *const axis_names[] = {
        "obsidian-transform-view-axis",
        "obsidian-transform-x-axis",
        "obsidian-transform-y-axis",
        "obsidian-transform-z-axis",
    };
    char *axis_constant_scheme = nullary_scheme(transform_axis_type());
    if (!axis_constant_scheme) return false;
    for (size_t i = 0; i < sizeof(axis_names) / sizeof(axis_names[0]); ++i) {
        if (!publish_function(axis_names[i], axis_constant_scheme,
                              (void *)make_transform_axis_value,
                              &s_transform_axes[i])) {
            free(axis_constant_scheme);
            return false;
        }
    }
    free(axis_constant_scheme);
    return true;
}

static bool register_map_function(ObsLiveKeymap *map) {
    if (!map || !s_evaluator || !s_host || !map->name || !map->map)
        return false;
    if (map->registered) return true;
    Type *result = keymap_type();
    char *scheme = nullary_scheme(result);
    if (!scheme) return false;
    char name[OBS_LIVE_NAME_CAP];
    int written = snprintf(name, sizeof(name), "obsidian-%s-keymap", map->name);
    if (written <= 0 || (size_t)written >= sizeof(name)) {
        free(scheme);
        return false;
    }
    char symbol[80];
    snprintf(symbol, sizeof(symbol), "__obs_keymap_%zu",
             (size_t)(map - s_maps));
    bool ok = monad_live_evaluator_function_with_context(
        s_evaluator, name, symbol, scheme, (void *)get_keymap_value, map, true);
    free(scheme);
    if (ok) map->registered = true;
    return ok;
}

static bool register_live_keymap_api(void) {
    char *keymap_scheme = nullary_scheme(keymap_type());
    char *define_scheme = define_key_scheme();
    char *set_scheme = map_set_scheme();
    if (!keymap_scheme || !define_scheme || !set_scheme) {
        free(keymap_scheme);
        free(define_scheme);
        free(set_scheme);
        return false;
    }
    bool ok = publish_function("make-sparse-keymap", keymap_scheme,
                               (void *)make_keymap_value, NULL) &&
              publish_function("define-key", define_scheme,
                               (void *)map_define_key, NULL) &&
              publish_function("keymap-set-overriding", set_scheme,
                               (void *)set_overriding_map, NULL);
    free(keymap_scheme);
    free(define_scheme);
    free(set_scheme);
    if (!ok) return false;
    if (!register_editor_transform_api()) return false;

    char *panel_value_scheme = nullary_scheme(editor_panel_type());
    if (!panel_value_scheme) return false;
    static const char *const panel_accessors[] = {
        "obsidian-hierarchy-panel",
        "obsidian-inspector-panel",
        "obsidian-repl-panel",
    };
    for (size_t i = 0; i < sizeof(s_panels) / sizeof(s_panels[0]); ++i) {
        if (!publish_function(panel_accessors[i], panel_value_scheme,
                              (void *)make_editor_panel_value, &s_panels[i])) {
            free(panel_value_scheme);
            return false;
        }
    }
    free(panel_value_scheme);

    char *gizmo_mode_value_scheme = nullary_scheme(gizmo_mode_type());
    if (!gizmo_mode_value_scheme) return false;
    static const char *const gizmo_mode_accessors[] = {
        "obsidian-gizmo-mode-translate",
        "obsidian-gizmo-mode-rotate",
        "obsidian-gizmo-mode-scale",
    };
    for (size_t i = 0;
         i < sizeof(s_gizmo_modes) / sizeof(s_gizmo_modes[0]); ++i) {
        if (!publish_function(gizmo_mode_accessors[i],
                              gizmo_mode_value_scheme,
                              (void *)make_gizmo_mode_value,
                              &s_gizmo_modes[i])) {
            free(gizmo_mode_value_scheme);
            return false;
        }
    }
    free(gizmo_mode_value_scheme);

    char *render_setting_value_scheme = nullary_scheme(render_setting_type());
    if (!render_setting_value_scheme) return false;
    static const char *const render_setting_accessors[] = {
        "obsidian-render-setting-skybox",
        "obsidian-render-setting-ibl",
        "obsidian-render-setting-shadows",
        "obsidian-render-setting-culling-frozen",
    };
    for (size_t i = 0;
         i < sizeof(s_render_settings) / sizeof(s_render_settings[0]); ++i) {
        if (!publish_function(render_setting_accessors[i],
                              render_setting_value_scheme,
                              (void *)make_render_setting_value,
                              &s_render_settings[i])) {
            free(render_setting_value_scheme);
            return false;
        }
    }
    free(render_setting_value_scheme);

    char *add_menu_value_scheme = nullary_scheme(add_menu_type());
    if (!add_menu_value_scheme) return false;
    ok = publish_function("obsidian-add-menu-state",
                          add_menu_value_scheme,
                          (void *)make_add_menu_value, &s_add_menu);
    free(add_menu_value_scheme);
    if (!ok) return false;

    Type *unit_bool = type_fn_builtin(0, 0, false);
    if (!unit_bool) return false;
    type_free(unit_bool->return_type);
    unit_bool->return_type = type_bool();
    char *clear_scheme = serialize_scheme(unit_bool, 0);
    type_free(unit_bool);
    if (!clear_scheme) return false;
    ok = publish_function("keymap-clear-overriding", clear_scheme,
                          (void *)clear_overriding_map, NULL);
    free(clear_scheme);
    if (!ok) return false;
    char *panel_open_scheme = panel_open_query_scheme();
    char *panel_set_scheme = panel_set_open_scheme();
    if (!panel_open_scheme || !panel_set_scheme) {
        free(panel_open_scheme);
        free(panel_set_scheme);
        return false;
    }
    ok = publish_function("editor-panel-open?", panel_open_scheme,
                          (void *)editor_panel_open_query, NULL) &&
         publish_function("editor-panel-open!", panel_set_scheme,
                          (void *)editor_panel_set_open, NULL);
    free(panel_open_scheme);
    free(panel_set_scheme);
    if (!ok) return false;
    char *gizmo_mode_scheme = gizmo_mode_set_scheme();
    if (!gizmo_mode_scheme) return false;
    ok = publish_function("gizmo-mode!", gizmo_mode_scheme,
                          (void *)set_gizmo_mode, NULL);
    free(gizmo_mode_scheme);
    if (!ok) return false;
    char *render_query_scheme = render_setting_query_scheme();
    char *render_set_scheme = render_setting_set_scheme();
    if (!render_query_scheme || !render_set_scheme) {
        free(render_query_scheme);
        free(render_set_scheme);
        return false;
    }
    ok = publish_function("render-setting?", render_query_scheme,
                          (void *)render_setting_query, NULL) &&
         publish_function("render-setting!", render_set_scheme,
                          (void *)render_setting_set, NULL);
    free(render_query_scheme);
    free(render_set_scheme);
    if (!ok) return false;
    char *add_menu_query = add_menu_query_scheme();
    char *add_menu_set = add_menu_set_scheme();
    if (!add_menu_query || !add_menu_set) {
        free(add_menu_query);
        free(add_menu_set);
        return false;
    }
    ok = publish_function("add-menu-visible-count", add_menu_query,
                          (void *)add_menu_visible_count_value, NULL) &&
         publish_function("add-menu-selection-index", add_menu_query,
                          (void *)add_menu_selection_index_value, NULL) &&
         publish_function("add-menu-set-selection-index!", add_menu_set,
                          (void *)add_menu_set_selection_index_value, NULL);
    free(add_menu_query);
    free(add_menu_set);
    if (!ok) return false;
    char *vertico_query = nullary_scheme(type_int());
    char *vertico_set = vertico_position_set_scheme();
    Type *orderless_type = type_fn_builtin(2, 0, false);
    if (orderless_type) {
        type_free(orderless_type->params[0].type);
        orderless_type->params[0].type = type_string();
        type_free(orderless_type->params[1].type);
        orderless_type->params[1].type = type_string();
        type_free(orderless_type->return_type);
        orderless_type->return_type = type_float();
    }
    char *orderless_scheme = orderless_type
        ? serialize_scheme(orderless_type, 0) : NULL;
    type_free(orderless_type);
    if (!vertico_query || !vertico_set || !orderless_scheme) {
        free(vertico_query);
        free(vertico_set);
        free(orderless_scheme);
        return false;
    }
    ok = publish_function("vertico-orderless-score", orderless_scheme,
                          (void *)vertico_orderless_score_value, NULL) &&
         publish_function("vertico-filtered-count", vertico_query,
                          (void *)vertico_filtered_count_value, NULL) &&
         publish_function("vertico-selection-index", vertico_query,
                          (void *)vertico_selection_index_value, NULL) &&
         publish_function("vertico-scroll-offset", vertico_query,
                          (void *)vertico_scroll_offset_value, NULL) &&
         publish_function("vertico-visible-count", vertico_query,
                          (void *)vertico_visible_count_value, NULL) &&
         publish_function("vertico-set-position!", vertico_set,
                          (void *)vertico_set_position_value, NULL);
    free(vertico_query);
    free(vertico_set);
    free(orderless_scheme);
    if (!ok) return false;
    char *prepare_scheme = nullary_scheme(type_bool());
    if (!prepare_scheme) return false;
    ok = publish_function("obsidian-editor-prepare-keymaps!",
                          prepare_scheme, (void *)prepare_keymaps, NULL);
    free(prepare_scheme);
    if (!ok) return false;
    if (!register_editor_commands_api()) return false;
    if (!obs_monad_window_register_live_api(s_evaluator)) return false;
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i)
        if (s_maps[i].magic == OBS_KEYMAP_MAGIC && s_maps[i].name &&
            !register_map_function(&s_maps[i]))
            return false;
    for (size_t i = 0; i < s_action_count; ++i)
        if (!register_action_function(&s_actions[i]))
            return false;
    return true;
}

bool obs_monad_live_editor_register_map(const char *name, KeyChordMap *map) {
    if (!name || !*name || !map || strlen(name) >= OBS_LIVE_NAME_CAP)
        return false;
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        ObsLiveKeymap *entry = &s_maps[i];
        if (entry->magic == OBS_KEYMAP_MAGIC && entry->name &&
            strcmp(entry->name, name) == 0)
            return entry->map == map;
    }
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        ObsLiveKeymap *entry = &s_maps[i];
        if (entry->magic != OBS_KEYMAP_MAGIC) {
            *entry = (ObsLiveKeymap){0};
            entry->name = name;
            entry->map = map;
            entry->magic = OBS_KEYMAP_MAGIC;
            if (s_evaluator && !register_map_function(entry)) return false;
            return true;
        }
    }
    return false;
}

bool obs_monad_live_editor_register_action(const char *name,
                                           ObsLiveEditorAction action) {
    if (!name || !*name || strlen(name) >= OBS_LIVE_NAME_CAP || !action)
        return false;
    for (size_t i = 0; i < s_action_count; ++i) {
        if (strcmp(s_actions[i].name, name) == 0) {
            s_actions[i].action = action;
            if (s_evaluator) {
                s_actions[i].published_generation = 0;
                return register_action_function(&s_actions[i]);
            }
            return true;
        }
    }
    if (s_action_count == OBS_LIVE_ACTION_CAP) return false;
    ObsLiveAction *entry = &s_actions[s_action_count++];
    *entry = (ObsLiveAction){.name = name, .action = action};
    if (s_evaluator && !register_action_function(entry)) {
        --s_action_count;
        *entry = (ObsLiveAction){0};
        return false;
    }
    return true;
}

bool obs_monad_live_editor_enabled(void) {
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i)
        if (s_maps[i].magic == OBS_KEYMAP_MAGIC && s_maps[i].name &&
            strcmp(s_maps[i].name, "global") == 0 && s_maps[i].map)
            return true;
    return false;
}

bool obs_monad_live_editor_vertico_options(int64_t *width, int64_t *count,
                                           int64_t *scroll_margin, bool *top,
                                           bool *resize_pixelwise) {
    if (!s_evaluator || !s_vertico_width || !s_vertico_count ||
        !s_vertico_position || !s_vertico_scroll_margin ||
        !s_vertico_resize_fixelwise || !width || !count || !scroll_margin ||
        !top || !resize_pixelwise ||
        !obs_monad_on_owner_thread())
        return false;
    *width = *s_vertico_width;
    *count = *s_vertico_count;
    *scroll_margin = *s_vertico_scroll_margin;
    *top = *s_vertico_position != 0;
    *resize_pixelwise =
        s_vertico_resize_fixelwise->data.int_val != 0;
    return true;
}

bool obs_monad_live_editor_completion_begin(
    const char *context, const char *query,
    ObsMonadCompletionContext *completion) {
    if (!context || !query || !completion || !s_evaluator ||
        !obs_monad_on_owner_thread())
        return false;
    *completion = (ObsMonadCompletionContext){
        .query = query, .context = context
    };
    RuntimeValue *strategy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "vertico-completion-strategy", &strategy) ||
        !strategy || strategy->type != RT_CLOSURE ||
        !strategy->data.closure_val ||
        strategy->data.closure_val->arity != 2) {
        completion->failed = true;
        fprintf(stderr,
                "Monad Vertico completion strategy must be a two-argument function\n");
        return true;
    }
    RuntimeValue *history = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "vertico-completion-history", &history) ||
        !history || history->type != RT_LIST) {
        completion->failed = true;
        fprintf(stderr, "Monad Vertico history must be a List\n");
        return true;
    }
    completion->strategy = strategy;
    completion->history = history;
    completion->query_value = rt_value_string(query);
    if (!completion->query_value) {
        completion->failed = true;
        fprintf(stderr,
                "Monad Vertico completion could not allocate query value\n");
    }
    return true;
}

bool obs_monad_live_editor_completion_record(const char *context,
                                              const char *candidate) {
    if (!context || !candidate || !s_evaluator ||
        !obs_monad_on_owner_thread())
        return false;
    RuntimeValue *context_value = rt_value_string(context);
    RuntimeValue *candidate_value = rt_value_string(candidate);
    if (!context_value || !candidate_value) {
        rt_value_free(context_value);
        rt_value_free(candidate_value);
        return false;
    }
    RuntimeValue *history = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "vertico-completion-history", &history) ||
        !history) {
        rt_value_free(context_value);
        rt_value_free(candidate_value);
        return false;
    }
    RuntimeValue *arguments[] = {context_value, candidate_value, history};
    RuntimeValue *result = NULL;
    const bool ok = monad_live_evaluator_call_closure_guarded(
        s_evaluator, s_completion_record, 3, arguments, &result);
    if (!ok || !result || result->type != RT_LIST) {
        rt_value_free(context_value);
        rt_value_free(candidate_value);
        return false;
    }
    RuntimeValue *persisted_history = rt_value_persist(result);
    if (!persisted_history) {
        rt_value_free(context_value);
        rt_value_free(candidate_value);
        return false;
    }
    if (!monad_live_evaluator_binding_set(
            s_evaluator, "vertico-completion-history", persisted_history)) {
        rt_value_free(persisted_history);
        rt_value_free(context_value);
        rt_value_free(candidate_value);
        return false;
    }
    rt_value_free(context_value);
    rt_value_free(candidate_value);
    return true;
}

bool obs_monad_live_editor_completion_score(
    ObsMonadCompletionContext *completion, const char *candidate,
    float *score) {
    if (!completion || !candidate || !score || completion->failed)
        return false;
    if (!s_evaluator || !completion->strategy ||
        !completion->query_value || s_completion_depth) {
        completion->failed = true;
        return false;
    }
    RuntimeValue *candidate_value = rt_value_string(candidate);
    if (!candidate_value) {
        completion->failed = true;
        fprintf(stderr,
                "Monad Vertico completion could not allocate candidate value\n");
        return false;
    }
    RuntimeValue *arguments[] = {
        completion->query_value, candidate_value
    };
    RuntimeValue *result = NULL;
    ++s_completion_depth;
    const bool ok = monad_live_evaluator_call_closure_guarded(
        s_evaluator, (RuntimeValue *)completion->strategy, 2, arguments,
        &result);
    --s_completion_depth;
    rt_value_free(candidate_value);
    if (!ok || !result || result->type != RT_FLOAT ||
        !isfinite(rt_unbox_float(result))) {
        completion->failed = true;
        fprintf(stderr, "Monad Vertico completion strategy failed: %s\n",
                monad_live_evaluator_last_diagnostic(s_evaluator)
                    ? monad_live_evaluator_last_diagnostic(s_evaluator)
                    : "expected a finite Float score");
        return false;
    }
    *score = (float)rt_unbox_float(result);
    if (*score > 0.0f && completion->context &&
        completion->history &&
        ((const RuntimeValue *)completion->history)->type == RT_LIST) {
        RuntimeList *history =
            ((const RuntimeValue *)completion->history)->data.list_val;
        size_t rank = 0;
        while (history && history->cell) {
            RuntimeValue *context_value = rt_list_car(history);
            history = rt_list_cdr(history);
            RuntimeValue *candidate_value = history && history->cell
                ? rt_list_car(history) : NULL;
            history = history ? rt_list_cdr(history) : NULL;
            if (context_value && context_value->type == RT_STRING &&
                candidate_value && candidate_value->type == RT_STRING &&
                strcmp(context_value->data.string_val, completion->context) == 0 &&
                strcmp(candidate_value->data.string_val, candidate) == 0) {
                *score += 1024.0f - (float)rank;
                break;
            }
            ++rank;
        }
    }
    return true;
}

void obs_monad_live_editor_completion_end(
    ObsMonadCompletionContext *completion) {
    if (!completion) return;
    rt_value_free(completion->query_value);
    *completion = (ObsMonadCompletionContext){0};
}

static bool marginalia_call(const char *name, const char *const *arguments,
                            size_t argument_count, char *out,
                            size_t capacity) {
    if (!s_evaluator || !obs_monad_on_owner_thread() || !name || !arguments ||
        !out || !capacity || argument_count > 3 || s_callback_depth >= 32)
        return false;
    RuntimeValue *closure = NULL;
    if (!monad_live_evaluator_binding_value(s_evaluator, name, &closure) ||
        !closure || closure->type != RT_CLOSURE ||
        !closure->data.closure_val ||
        closure->data.closure_val->arity != (int)argument_count)
        return false;
    RuntimeValue *values[3] = {0};
    for (size_t i = 0; i < argument_count; ++i) {
        values[i] = rt_value_string(arguments[i] ? arguments[i] : "");
        if (!values[i]) {
            for (size_t j = 0; j < i; ++j) rt_value_free(values[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool ok = monad_live_evaluator_call_closure_guarded(
        s_evaluator, closure, (int)argument_count, values, &result);
    --s_callback_depth;
    bool result_aliases_argument = false;
    for (size_t i = 0; i < argument_count; ++i)
        result_aliases_argument |= result == values[i];
    const bool valid_result = ok && result && result->type == RT_STRING &&
                              result->data.string_val;
    const size_t length =
        valid_result ? strlen(result->data.string_val) : SIZE_MAX;
    char leading[128];
    bool formatted = false;
    if (length < sizeof(leading)) {
        memcpy(leading, result->data.string_val, length + 1);
        int written = -1;
        if (strcmp(name, "marginalia-scene-buffer") == 0 &&
            argument_count == 2) {
            written = snprintf(out, capacity, "%s · %s", leading,
                               arguments[1] ? arguments[1] : "");
        } else if (strcmp(name, "marginalia-material") == 0 &&
                   argument_count == 2) {
            written = snprintf(out, capacity, "%s · %s", leading,
                               arguments[1] ? arguments[1] : "");
        } else if (strcmp(name, "marginalia-variable") == 0 &&
                   argument_count == 3) {
            written = snprintf(out, capacity, "%s · %s · %s", leading,
                               arguments[1] ? arguments[1] : "",
                               arguments[2] ? arguments[2] : "");
        } else {
            written = snprintf(out, capacity, "%s", leading);
        }
        formatted = written >= 0 && (size_t)written < capacity;
    }
    for (size_t i = 0; i < argument_count; ++i) rt_value_free(values[i]);
    if (result && !result_aliases_argument) rt_value_free(result);
    return formatted;
}

bool obs_monad_live_editor_marginalia_scene(
    const char *cached, const char *detail, char *out, size_t capacity) {
    const char *arguments[] = {cached, detail};
    return marginalia_call("marginalia-scene-buffer", arguments, 2, out,
                           capacity);
}

bool obs_monad_live_editor_marginalia_variable(
    const char *type, const char *value, const char *documentation,
    char *out, size_t capacity) {
    const char *arguments[] = {type, value, documentation};
    return marginalia_call("marginalia-variable", arguments, 3, out,
                           capacity);
}

bool obs_monad_live_editor_marginalia_material(
    const char *kind, const char *detail, char *out, size_t capacity) {
    const char *arguments[] = {kind, detail};
    return marginalia_call("marginalia-material", arguments, 2, out,
                           capacity);
}

static int filesystem_tree_path_index(const RuntimeValue *state,
                                     const char *path) {
    if (!state || state->type != RT_LIST || !state->data.list_val) return -1;
    RuntimeList *parts = state->data.list_val;
    RuntimeValue *paths = parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!paths || paths->type != RT_LIST || !paths->data.list_val) return -1;
    int index = 0;
    for (RuntimeList *item = paths->data.list_val; item && item->cell;
         item = rt_list_cdr(item), ++index) {
        RuntimeValue *value = rt_list_car(item);
        if (value && value->type == RT_STRING && value->data.string_val &&
            strcmp(value->data.string_val, path) == 0)
            return index;
    }
    return -1;
}

bool obs_monad_live_editor_filesystem_tree_command(const char *operation,
                                                    const char *path,
                                                    bool *expanded) {
    if (!operation || !path || !expanded || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;

    RuntimeValue *state = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "filesystem-tree-state", &state) ||
        !state || state->type != RT_LIST)
        return false;

    const int existing_index = filesystem_tree_path_index(state, path);
    if (strcmp(operation, "contains") == 0) {
        *expanded = existing_index >= 0;
        return true;
    }

    RuntimeValue *closure = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "filesystem-tree-transition", &closure) ||
        !closure || closure->type != RT_CLOSURE ||
        !closure->data.closure_val)
        return false;

    RuntimeValue *path_value = rt_value_string(path);
    RuntimeValue *index_value = rt_value_int(existing_index);
    RuntimeValue *state_value = rt_value_persist(state);
    if (!path_value || !index_value || !state_value) {
        rt_value_free(path_value);
        rt_value_free(index_value);
        rt_value_free(state_value);
        return false;
    }

    RuntimeValue *result = NULL;
    ++s_callback_depth;
    bool ok = false;
    RuntimeValue *operation_value = rt_value_string(operation);
    if (operation_value && closure->data.closure_val->arity == 4) {
        RuntimeValue *arguments[] = {
            operation_value, path_value, index_value, state_value
        };
        ok = monad_live_evaluator_call_closure_guarded(
            s_evaluator, closure, 4, arguments, &result);
    }
    --s_callback_depth;
    rt_value_free(operation_value);
    if (!ok || !result) {
        rt_value_free(path_value);
        rt_value_free(index_value);
        rt_value_free(state_value);
        if (result) rt_value_free(result);
        return false;
    }
    if (result->type != RT_LIST || !result->data.list_val ||
        !result->data.list_val->cell) {
        rt_value_free(path_value);
        rt_value_free(index_value);
        rt_value_free(state_value);
        rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *expanded_value = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *new_state = parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!expanded_value || expanded_value->type != RT_BOOL ||
        !new_state || new_state->type != RT_LIST) {
        rt_value_free(path_value);
        rt_value_free(index_value);
        rt_value_free(state_value);
        rt_value_free(result);
        return false;
    }
    RuntimeValue *persisted_state = rt_value_persist(new_state);
    rt_value_free(path_value);
    rt_value_free(index_value);
    rt_value_free(state_value);
    if (!persisted_state) {
        rt_value_free(result);
        return false;
    }
    ok = monad_live_evaluator_binding_set(
        s_evaluator, "filesystem-tree-state", persisted_state);
    if (!ok) rt_value_free(persisted_state);
    if (ok) *expanded = expanded_value->data.int_val != 0;
    rt_value_free(result);
    return ok;
}

bool obs_monad_live_editor_filesystem_tree_selection(int64_t *index,
                                                      bool set) {
    if (!index || !s_evaluator || !obs_monad_on_owner_thread()) return false;
    if (!set) {
        RuntimeValue *value = NULL;
        if (!monad_live_evaluator_binding_value(
                s_evaluator, "filesystem-tree-selected-index", &value) ||
            !value || value->type != RT_INT)
            return false;
        *index = value->data.int_val;
        return true;
    }
    RuntimeValue *value = rt_value_int(*index);
    if (!value) return false;
    if (!monad_live_evaluator_binding_set(
            s_evaluator, "filesystem-tree-selected-index", value)) {
        rt_value_free(value);
        return false;
    }
    return true;
}

bool obs_monad_live_editor_filesystem_activation(
    const char *extension, bool is_file, ObsMonadFilesystemActivation *action) {
    if (!extension || !action || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *transition = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "filesystem-file-activation", &transition) ||
        !transition || transition->type != RT_CLOSURE ||
        !transition->data.closure_val ||
        transition->data.closure_val->arity != 2)
        return false;
    RuntimeValue *extension_value = rt_value_string(extension);
    RuntimeValue *file_value = rt_value_bool(is_file);
    if (!extension_value || !file_value) {
        rt_value_free(extension_value);
        rt_value_free(file_value);
        return false;
    }
    RuntimeValue *arguments[] = {extension_value, file_value};
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, transition, 2, arguments, &result);
    --s_callback_depth;
    rt_value_free(extension_value);
    rt_value_free(file_value);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < OBS_MONAD_FILESYSTEM_ACTIVATE_NONE ||
        result->data.int_val > OBS_MONAD_FILESYSTEM_ACTIVATE_TEXT) {
        if (result) rt_value_free(result);
        return false;
    }
    *action = (ObsMonadFilesystemActivation)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_filesystem_double_click(
    int previous_index, int clicked_index, double previous_time,
    double current_time, bool *is_double_click) {
    if (!is_double_click || !isfinite(previous_time) ||
        !isfinite(current_time) || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "filesystem-double-click-action", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 4)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(previous_index), rt_value_int(clicked_index),
        rt_value_float(previous_time), rt_value_float(current_time)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 4, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        (result->data.int_val != 0 && result->data.int_val != 1)) {
        if (result) rt_value_free(result);
        return false;
    }
    *is_double_click = result->data.int_val != 0;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_filesystem_tree_wheel_scroll(
    float current_scroll, float wheel_delta, float item_count,
    float visible_rows, float row_height, float *next_scroll) {
    if (!next_scroll || !isfinite(current_scroll) || !isfinite(wheel_delta) ||
        !isfinite(visible_rows) || !isfinite(row_height) ||
        !isfinite(item_count) || item_count < 0.0f ||
        visible_rows < 0.0f || row_height <= 0.0f ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "filesystem-tree-wheel-scroll", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 5)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(current_scroll), rt_value_float(wheel_delta),
        rt_value_float(item_count), rt_value_float(visible_rows),
        rt_value_float(row_height)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 5, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_FLOAT ||
        !isfinite(result->data.float_val)) {
        if (result) rt_value_free(result);
        return false;
    }
    *next_scroll = (float)result->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_filesystem_search_next_index(
    int current_index, int match_count, int direction, int *next_index) {
    if (!next_index || match_count < 0 || (direction != -1 && direction != 1) ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "filesystem-search-next-index", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 3)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(current_index), rt_value_int(match_count),
        rt_value_int(direction)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 3, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < -1 || result->data.int_val >= match_count) {
        if (result) rt_value_free(result);
        return false;
    }
    *next_index = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_text_editor_wheel_scroll_target(
    float current_scroll, float wheel_delta, float row_height,
    float *target_scroll) {
    if (!target_scroll || !isfinite(current_scroll) ||
        !isfinite(wheel_delta) || !isfinite(row_height) || row_height < 0.0f ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "text-editor-wheel-scroll-target", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 3)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(current_scroll), rt_value_float(wheel_delta),
        rt_value_float(row_height)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 3, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_FLOAT ||
        !isfinite(result->data.float_val) || result->data.float_val < 0.0) {
        if (result) rt_value_free(result);
        return false;
    }
    *target_scroll = (float)result->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_text_pointer_transition(
    bool selecting, bool pressed, bool released, bool valid_position,
    int *action, bool *next_selecting) {
    if (!action || !next_selecting || !s_evaluator ||
        !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "text-editor-pointer-transition", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 4)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(selecting ? 1 : 0), rt_value_int(pressed ? 1 : 0),
        rt_value_int(released ? 1 : 0),
        rt_value_int(valid_position ? 1 : 0)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 4, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *action_value = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *selecting_value =
        parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!action_value || action_value->type != RT_INT ||
        action_value->data.int_val < 0 || action_value->data.int_val > 2 ||
        !selecting_value || selecting_value->type != RT_INT ||
        (selecting_value->data.int_val != 0 &&
         selecting_value->data.int_val != 1)) {
        rt_value_free(result);
        return false;
    }
    *action = (int)action_value->data.int_val;
    *next_selecting = selecting_value->data.int_val == 1;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_gizmo_rotation_hover_priority(
    int current_part, bool trackball_hit, int *hovered_part) {
    if (!hovered_part || current_part < -1 || current_part > 10 ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "gizmo-rotation-hover-priority", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 2)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(current_part), rt_value_int(trackball_hit ? 1 : 0)
    };
    if (!arguments[0] || !arguments[1]) {
        rt_value_free(arguments[0]);
        rt_value_free(arguments[1]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 2, arguments, &result);
    --s_callback_depth;
    rt_value_free(arguments[0]);
    rt_value_free(arguments[1]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < -1 || result->data.int_val > 10 ||
        (trackball_hit && result->data.int_val != 9) ||
        (!trackball_hit && result->data.int_val != current_part)) {
        if (result) rt_value_free(result);
        return false;
    }
    *hovered_part = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_gizmo_pointer_transition(
    int current_drag, int hovered_part, bool pressed, bool released,
    bool active, int *next_drag) {
    if (!next_drag || current_drag < -1 || current_drag > 10 ||
        hovered_part < -1 || hovered_part > 10 || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "gizmo-pointer-transition", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 5)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(current_drag), rt_value_int(hovered_part),
        rt_value_int(pressed ? 1 : 0), rt_value_int(released ? 1 : 0),
        rt_value_int(active ? 1 : 0)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 5, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < -1 || result->data.int_val > 10) {
        if (result) rt_value_free(result);
        return false;
    }
    *next_drag = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_gizmo_press_action(int drag_part,
                                               bool bone_hovered,
                                               int *action) {
    if (!action || drag_part < -1 || drag_part > 10 || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "gizmo-press-action", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 2)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(drag_part), rt_value_int(bone_hovered ? 1 : 0)
    };
    if (!arguments[0] || !arguments[1]) {
        rt_value_free(arguments[0]);
        rt_value_free(arguments[1]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 2, arguments, &result);
    --s_callback_depth;
    rt_value_free(arguments[0]);
    rt_value_free(arguments[1]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < 1 || result->data.int_val > 3 ||
        (drag_part >= 0 && result->data.int_val != 1) ||
        (drag_part < 0 && bone_hovered && result->data.int_val != 2)) {
        if (result) rt_value_free(result);
        return false;
    }
    *action = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_ui_window_pointer_action(
    int left_button, int action, bool close_hovered, bool in_title,
    bool inside_window, bool dragging, int *result_action) {
    if (!result_action || (left_button != 0 && left_button != 1) ||
        action < 0 || action > 2 || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "ui-window-pointer-action", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 6)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(left_button), rt_value_int(action),
        rt_value_int(close_hovered ? 1 : 0), rt_value_int(in_title ? 1 : 0),
        rt_value_int(inside_window ? 1 : 0), rt_value_int(dragging ? 1 : 0)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 6, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < 0 || result->data.int_val > 4) {
        if (result) rt_value_free(result);
        return false;
    }
    *result_action = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_ui_window_animation_time_step(
    float progress, float delta_time, bool closing, float *next_progress) {
    if (!next_progress || !isfinite(progress) || progress < 0.0f ||
        progress > 1.0f || !isfinite(delta_time) || delta_time < 0.0f ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "ui-window-animation-time-step", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 3)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(progress), rt_value_float(delta_time),
        rt_value_int(closing ? 1 : 0)
    };
    if (!arguments[0] || !arguments[1] || !arguments[2]) {
        rt_value_free(arguments[0]);
        rt_value_free(arguments[1]);
        rt_value_free(arguments[2]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 3, arguments, &result);
    --s_callback_depth;
    rt_value_free(arguments[0]);
    rt_value_free(arguments[1]);
    rt_value_free(arguments[2]);
    if (!called || !result || result->type != RT_FLOAT ||
        !isfinite(result->data.float_val) ||
        result->data.float_val < 0.0 || result->data.float_val > 1.0) {
        if (result) rt_value_free(result);
        return false;
    }
    *next_progress = (float)result->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_ui_window_clamp_axis_position(
    float position, float minimum, float maximum, float lower_extent,
    float upper_extent, float *next_position) {
    if (!next_position || !isfinite(position) || !isfinite(minimum) ||
        !isfinite(maximum) ||
        !isfinite(lower_extent) || lower_extent < 0.0f ||
        !isfinite(upper_extent) || upper_extent < 0.0f ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "ui-window-clamp-axis-position", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 5)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(position), rt_value_float(minimum),
        rt_value_float(maximum), rt_value_float(lower_extent),
        rt_value_float(upper_extent)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 5, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_FLOAT ||
        !isfinite(result->data.float_val)) {
        if (result) rt_value_free(result);
        return false;
    }
    *next_position = (float)result->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_bottom_panel_title_action(
    bool close_hit, bool title_hit, bool transport_hit, bool collapsed,
    bool double_click, int *action) {
    if (!action || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "bottom-panel-title-action", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 5)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(close_hit ? 1 : 0), rt_value_int(title_hit ? 1 : 0),
        rt_value_int(transport_hit ? 1 : 0),
        rt_value_int(collapsed ? 1 : 0),
        rt_value_int(double_click ? 1 : 0)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 5, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < 0 || result->data.int_val > 5) {
        if (result) rt_value_free(result);
        return false;
    }
    *action = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_animation_title_action(
    bool stop_hit, bool play_hit, bool close_hit, int *action) {
    if (!action || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "animation-title-action", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 3)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(stop_hit ? 1 : 0),
        rt_value_int(play_hit ? 1 : 0),
        rt_value_int(close_hit ? 1 : 0)
    };
    if (!arguments[0] || !arguments[1] || !arguments[2]) {
        for (size_t i = 0; i < 3; ++i) rt_value_free(arguments[i]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 3, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < 3; ++i) rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < 0 || result->data.int_val > 3) {
        if (result) rt_value_free(result);
        return false;
    }
    *action = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_animation_pointer_action(
    int title_action, bool ruler_hit, bool keyframe_hit, bool track_hit,
    int *action) {
    if (!action || title_action < 0 || title_action > 3 || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "animation-editor-pointer-action", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 4)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(title_action), rt_value_int(ruler_hit ? 1 : 0),
        rt_value_int(keyframe_hit ? 1 : 0), rt_value_int(track_hit ? 1 : 0)
    };
    for (size_t i = 0; i < 4; ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < 4; ++j) rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 4, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < 4; ++i) rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < 0 || result->data.int_val > 6) {
        if (result) rt_value_free(result);
        return false;
    }
    *action = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_message_drag_transition(
    bool dragging, bool pressed, bool released, bool inside_message,
    bool *next_dragging) {
    if (!next_dragging || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "editor-message-drag-transition", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 4)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(dragging ? 1 : 0),
        rt_value_int(pressed ? 1 : 0),
        rt_value_int(released ? 1 : 0),
        rt_value_int(inside_message ? 1 : 0)
    };
    for (size_t i = 0; i < 4; ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < 4; ++j) rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 4, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < 4; ++i) rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        (result->data.int_val != 0 && result->data.int_val != 1)) {
        if (result) rt_value_free(result);
        return false;
    }
    *next_dragging = result->data.int_val == 1;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_animation_track_wheel_scroll(
    float current_scroll, float wheel_delta, float track_count,
    float row_height, float *next_scroll) {
    if (!next_scroll || !isfinite(current_scroll) || !isfinite(wheel_delta) ||
        !isfinite(track_count) || !isfinite(row_height) ||
        track_count < 0.0f || row_height <= 0.0f || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "animation-track-wheel-scroll", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 4)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(current_scroll), rt_value_float(wheel_delta),
        rt_value_float(track_count), rt_value_float(row_height)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 4, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_FLOAT ||
        !isfinite(result->data.float_val)) {
        if (result) rt_value_free(result);
        return false;
    }
    *next_scroll = (float)result->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_animation_view_pan(
    float view_start, float view_end, float total_duration, float wheel_delta,
    float *next_start, float *next_end) {
    if (!next_start || !next_end || !isfinite(view_start) ||
        !isfinite(view_end) || !isfinite(total_duration) ||
        !isfinite(wheel_delta) || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "animation-view-pan", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 4)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(view_start), rt_value_float(view_end),
        rt_value_float(total_duration), rt_value_float(wheel_delta)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 4, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *start_value = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *end_value = parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!start_value || start_value->type != RT_FLOAT ||
        !end_value || end_value->type != RT_FLOAT ||
        !isfinite(start_value->data.float_val) ||
        !isfinite(end_value->data.float_val)) {
        rt_value_free(result);
        return false;
    }
    *next_start = (float)start_value->data.float_val;
    *next_end = (float)end_value->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_animation_keyframe_click_action(
    float delta_x, bool selected, int *action) {
    if (!action || !isfinite(delta_x) || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "animation-keyframe-click-action", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 2)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(delta_x), rt_value_int(selected ? 1 : 0)
    };
    if (!arguments[0] || !arguments[1]) {
        rt_value_free(arguments[0]);
        rt_value_free(arguments[1]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 2, arguments, &result);
    --s_callback_depth;
    rt_value_free(arguments[0]);
    rt_value_free(arguments[1]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < 0 || result->data.int_val > 2) {
        if (result) rt_value_free(result);
        return false;
    }
    *action = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_animation_transport_transition(
    int action, float current_time, float duration, bool playing,
    float *next_time, bool *next_playing) {
    if (!next_time || !next_playing || action < 0 || action > 3 ||
        !isfinite(current_time) || !isfinite(duration) || duration < 0.0f ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "animation-transport-transition", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 4)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(action), rt_value_float(current_time),
        rt_value_float(duration), rt_value_int(playing ? 1 : 0)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 4, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *time_value = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *playing_value =
        parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!time_value || time_value->type != RT_FLOAT ||
        !isfinite(time_value->data.float_val) ||
        !playing_value || playing_value->type != RT_FLOAT ||
        (playing_value->data.float_val != 0.0 &&
         playing_value->data.float_val != 1.0)) {
        rt_value_free(result);
        return false;
    }
    *next_time = (float)time_value->data.float_val;
    *next_playing = playing_value->data.float_val == 1.0;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_animation_playback_view_transition(
    float time, float total_duration, float view_start, float view_end,
    bool looped, bool visible, float *next_view_start, float *next_view_end) {
    if (!next_view_start || !next_view_end || !isfinite(time) ||
        !isfinite(total_duration) || !isfinite(view_start) ||
        !isfinite(view_end) || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "animation-playback-view-transition", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 6)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(time), rt_value_float(total_duration),
        rt_value_float(view_start), rt_value_float(view_end),
        rt_value_int(looped ? 1 : 0), rt_value_int(visible ? 1 : 0)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 6, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *start_value = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *end_value =
        parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!start_value || start_value->type != RT_FLOAT ||
        !isfinite(start_value->data.float_val) ||
        !end_value || end_value->type != RT_FLOAT ||
        !isfinite(end_value->data.float_val)) {
        rt_value_free(result);
        return false;
    }
    *next_view_start = (float)start_value->data.float_val;
    *next_view_end = (float)end_value->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_animation_pointer_time(
    float mouse_offset, float timeline_width, float view_start,
    float view_end, float duration, float *time) {
    if (!time || !isfinite(mouse_offset) || !isfinite(timeline_width) ||
        !isfinite(view_start) || !isfinite(view_end) || !isfinite(duration) ||
        duration < 0.0f || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "animation-pointer-time", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 5)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(mouse_offset), rt_value_float(timeline_width),
        rt_value_float(view_start), rt_value_float(view_end),
        rt_value_float(duration)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 5, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_FLOAT ||
        !isfinite(result->data.float_val) || result->data.float_val < 0.0 ||
        result->data.float_val > duration) {
        if (result) rt_value_free(result);
        return false;
    }
    *time = (float)result->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_animation_playhead_drag_transition(
    bool dragging, bool pressed, bool released, bool in_ruler, bool *next) {
    if (!next || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "animation-playhead-drag-transition", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 4)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(dragging ? 1 : 0), rt_value_int(pressed ? 1 : 0),
        rt_value_int(released ? 1 : 0), rt_value_int(in_ruler ? 1 : 0)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 4, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        (result->data.int_val != 0 && result->data.int_val != 1)) {
        if (result) rt_value_free(result);
        return false;
    }
    *next = result->data.int_val == 1;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_image_pointer_transition(
    int action, const ObsMonadImagePointerState *current,
    double x, double y, ObsMonadImagePointerState *next) {
    if (action < 1 || action > 3 || !current || !next ||
        !isfinite(current->pan_x) || !isfinite(current->pan_y) ||
        !isfinite(current->last_x) || !isfinite(current->last_y) ||
        !isfinite(x) || !isfinite(y) || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "image-editor-pointer-transition", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 8)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(action), rt_value_int(current->dragging ? 1 : 0),
        rt_value_float(current->pan_x), rt_value_float(current->pan_y),
        rt_value_float(current->last_x), rt_value_float(current->last_y),
        rt_value_float(x), rt_value_float(y)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 8, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *dragging_value = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *pan_x_value = parts && parts->cell ? rt_list_car(parts) : NULL;
    parts = parts && parts->cell ? rt_list_cdr(parts) : NULL;
    RuntimeValue *pan_y_value = parts && parts->cell ? rt_list_car(parts) : NULL;
    parts = parts && parts->cell ? rt_list_cdr(parts) : NULL;
    RuntimeValue *last_x_value = parts && parts->cell ? rt_list_car(parts) : NULL;
    parts = parts && parts->cell ? rt_list_cdr(parts) : NULL;
    RuntimeValue *last_y_value = parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!dragging_value || dragging_value->type != RT_INT ||
        (dragging_value->data.int_val != 0 &&
         dragging_value->data.int_val != 1) ||
        !pan_x_value || pan_x_value->type != RT_FLOAT ||
        !pan_y_value || pan_y_value->type != RT_FLOAT ||
        !last_x_value || last_x_value->type != RT_FLOAT ||
        !last_y_value || last_y_value->type != RT_FLOAT ||
        !isfinite(pan_x_value->data.float_val) ||
        !isfinite(pan_y_value->data.float_val) ||
        !isfinite(last_x_value->data.float_val) ||
        !isfinite(last_y_value->data.float_val)) {
        rt_value_free(result);
        return false;
    }
    *next = (ObsMonadImagePointerState){
        .dragging = dragging_value->data.int_val == 1,
        .pan_x = (float)pan_x_value->data.float_val,
        .pan_y = (float)pan_y_value->data.float_val,
        .last_x = last_x_value->data.float_val,
        .last_y = last_y_value->data.float_val
    };
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_image_zoom_level(float zoom, float factor,
                                             float *next_zoom) {
    if (!next_zoom || !isfinite(zoom) || zoom <= 0.0f ||
        !isfinite(factor) || factor <= 0.0f || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "image-editor-zoom-level", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 2)
        return false;
    RuntimeValue *arguments[] = {rt_value_float(zoom), rt_value_float(factor)};
    if (!arguments[0] || !arguments[1]) {
        rt_value_free(arguments[0]);
        rt_value_free(arguments[1]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 2, arguments, &result);
    --s_callback_depth;
    rt_value_free(arguments[0]);
    rt_value_free(arguments[1]);
    if (!called || !result || result->type != RT_FLOAT ||
        !isfinite(result->data.float_val) || result->data.float_val < 0.05 ||
        result->data.float_val > 32.0) {
        if (result) rt_value_free(result);
        return false;
    }
    *next_zoom = (float)result->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_image_zoom_pan(
    float pan_x, float pan_y, float old_zoom, float next_zoom,
    float center_x, float center_y, float pointer_x, float pointer_y,
    float *next_pan_x, float *next_pan_y) {
    if (!next_pan_x || !next_pan_y || !isfinite(pan_x) || !isfinite(pan_y) ||
        !isfinite(old_zoom) || old_zoom <= 0.0f || !isfinite(next_zoom) ||
        next_zoom <= 0.0f || !isfinite(center_x) || !isfinite(center_y) ||
        !isfinite(pointer_x) || !isfinite(pointer_y) || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "image-editor-zoom-pan", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 8)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(pan_x), rt_value_float(pan_y),
        rt_value_float(old_zoom), rt_value_float(next_zoom),
        rt_value_float(center_x), rt_value_float(center_y),
        rt_value_float(pointer_x), rt_value_float(pointer_y)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 8, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *x_value = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *y_value = parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!x_value || x_value->type != RT_FLOAT || !y_value ||
        y_value->type != RT_FLOAT || !isfinite(x_value->data.float_val) ||
        !isfinite(y_value->data.float_val)) {
        rt_value_free(result);
        return false;
    }
    *next_pan_x = (float)x_value->data.float_val;
    *next_pan_y = (float)y_value->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_color_picker_pointer_action(
    bool ring_hit, bool triangle_hit, int *action) {
    if (!action || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "color-picker-pointer-action", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 2)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(ring_hit ? 1 : 0),
        rt_value_int(triangle_hit ? 1 : 0)
    };
    if (!arguments[0] || !arguments[1]) {
        rt_value_free(arguments[0]);
        rt_value_free(arguments[1]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 2, arguments, &result);
    --s_callback_depth;
    rt_value_free(arguments[0]);
    rt_value_free(arguments[1]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < 0 || result->data.int_val > 2) {
        if (result) rt_value_free(result);
        return false;
    }
    *action = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_color_picker_drag_transition(
    int current_drag, int pointer_action, bool released, int *next_drag) {
    if (!next_drag || current_drag < 0 || current_drag > 2 ||
        pointer_action < 0 || pointer_action > 2 || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "color-picker-drag-transition", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 3)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(current_drag),
        rt_value_int(pointer_action),
        rt_value_int(released ? 1 : 0)
    };
    if (!arguments[0] || !arguments[1] || !arguments[2]) {
        for (size_t i = 0; i < 3; ++i) rt_value_free(arguments[i]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 3, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < 3; ++i) rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < 0 || result->data.int_val > 2) {
        if (result) rt_value_free(result);
        return false;
    }
    *next_drag = (int)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_hierarchy_selection(
    int32_t clicked_node, bool control, bool shift, int32_t anchor_row,
    ObsMonadHierarchySelectionAction *action) {
    if (!action || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32) {
        return false;
    }

    RuntimeValue *anchor_value = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "hierarchy-selection-anchor", &anchor_value) ||
        !anchor_value || anchor_value->type != RT_INT ||
        anchor_value->data.int_val < -1 ||
        anchor_value->data.int_val > INT32_MAX)
        return false;
    RuntimeValue *transition = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "hierarchy-selection-transition", &transition) ||
        !transition || transition->type != RT_CLOSURE ||
        !transition->data.closure_val ||
        transition->data.closure_val->arity != 5) {
        fprintf(stderr, "Monad hierarchy selection transition is unavailable\n");
        return false;
    }

    RuntimeValue *arguments[] = {
        rt_value_int(clicked_node), rt_value_bool(control), rt_value_bool(shift),
        rt_value_int(anchor_row), rt_value_int(anchor_value->data.int_val)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, transition, 5, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        fprintf(stderr, "Monad hierarchy selection transition returned no result\n");
        return false;
    }

    RuntimeList *parts = result->data.list_val;
    RuntimeValue *action_result = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *anchor_result = parts && parts->cell
        ? rt_list_car(parts) : NULL;
    if (!action_result || action_result->type != RT_INT ||
        action_result->data.int_val < OBS_MONAD_HIERARCHY_SELECT_SINGLE ||
        action_result->data.int_val > OBS_MONAD_HIERARCHY_SELECT_APPEND ||
        !anchor_result ||
        anchor_result->type != RT_INT ||
        anchor_result->data.int_val < -1 ||
        anchor_result->data.int_val > INT32_MAX) {
        rt_value_free(result);
        fprintf(stderr, "Monad hierarchy selection transition returned malformed data\n");
        return false;
    }
    RuntimeValue *next_anchor_value =
        rt_value_int(anchor_result->data.int_val);
    if (!next_anchor_value) {
        rt_value_free(result);
        return false;
    }
    const bool anchor_stored = monad_live_evaluator_binding_set(
        s_evaluator, "hierarchy-selection-anchor", next_anchor_value);
    if (!anchor_stored) {
        rt_value_free(next_anchor_value);
        rt_value_free(result);
        return false;
    }
    *action = (ObsMonadHierarchySelectionAction)action_result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_hierarchy_anchor(int32_t *anchor, bool set) {
    if (!anchor || !s_evaluator || !obs_monad_on_owner_thread())
        return false;
    if (!set) {
        RuntimeValue *value = NULL;
        if (!monad_live_evaluator_binding_value(
                s_evaluator, "hierarchy-selection-anchor", &value) ||
            !value || value->type != RT_INT || value->data.int_val < -1 ||
            value->data.int_val > INT32_MAX)
            return false;
        *anchor = (int32_t)value->data.int_val;
        return true;
    }
    if (*anchor < -1) return false;
    RuntimeValue *value = rt_value_int(*anchor);
    if (!value) return false;
    if (!monad_live_evaluator_binding_set(
            s_evaluator, "hierarchy-selection-anchor", value)) {
        rt_value_free(value);
        return false;
    }
    return true;
}

bool obs_monad_live_editor_hierarchy_range_bounds(
    int32_t anchor_row, int32_t target_row, int32_t item_count,
    int32_t *first_row, int32_t *last_row) {
    if (!first_row || !last_row || item_count < 0 || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "hierarchy-range-bounds", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 3)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(anchor_row), rt_value_int(target_row),
        rt_value_int(item_count)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 3, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *first = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *last = parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!first || first->type != RT_INT || !last || last->type != RT_INT) {
        rt_value_free(result);
        return false;
    }
    const int64_t first_value = first->data.int_val;
    const int64_t last_value = last->data.int_val;
    rt_value_free(result);
    if (first_value == -1 && last_value == -1) {
        *first_row = -1;
        *last_row = -1;
        return true;
    }
    if (first_value < 0 || last_value < first_value ||
        last_value >= item_count)
        return false;
    *first_row = (int32_t)first_value;
    *last_row = (int32_t)last_value;
    return true;
}

bool obs_monad_live_editor_hierarchy_node_selection(
    ObsMonadHierarchySelectionAction action, bool selected, bool is_group,
    bool *clear_others, bool *next_selected, bool *next_selected_group) {
    if (!clear_others || !next_selected || !next_selected_group ||
        (action != OBS_MONAD_HIERARCHY_SELECT_SINGLE &&
         action != OBS_MONAD_HIERARCHY_SELECT_TOGGLE &&
         action != OBS_MONAD_HIERARCHY_SELECT_APPEND) ||
        !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "hierarchy-node-selection-transition", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 3)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(action), rt_value_bool(selected), rt_value_bool(is_group)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 3, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *clear_value = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *selected_value = parts && parts->cell
        ? rt_list_car(parts) : NULL;
    parts = parts ? rt_list_cdr(parts) : NULL;
    RuntimeValue *group_value = parts && parts->cell
        ? rt_list_car(parts) : NULL;
    if (!clear_value || clear_value->type != RT_BOOL ||
        !selected_value || selected_value->type != RT_BOOL ||
        !group_value || group_value->type != RT_BOOL) {
        rt_value_free(result);
        return false;
    }
    *clear_others = clear_value->data.int_val != 0;
    *next_selected = selected_value->data.int_val != 0;
    *next_selected_group = group_value->data.int_val != 0;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_hierarchy_gltf_selection_transition(
    int selected_mesh_count, int mesh_count, bool group_selected,
    ObsMonadGltfSelectionTransition *transition) {
    if (!transition || selected_mesh_count < 0 || mesh_count < 0 ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "hierarchy-gltf-selection-transition", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 3)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(selected_mesh_count), rt_value_int(mesh_count),
        rt_value_int(group_selected ? 1 : 0)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 3, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < OBS_MONAD_GLTF_SELECTION_NONE ||
        result->data.int_val > OBS_MONAD_GLTF_SELECTION_PROMOTE) {
        if (result) rt_value_free(result);
        return false;
    }
    *transition = (ObsMonadGltfSelectionTransition)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_tree_selection_scroll_target(
    float row_top, float row_height, float view_height, float current_scroll,
    float *target_scroll) {
    if (!target_scroll || !isfinite(row_top) || !isfinite(row_height) ||
        !isfinite(view_height) || !isfinite(current_scroll) ||
        row_height < 0.0f || view_height < 0.0f || current_scroll < 0.0f ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "tree-selection-scroll-target", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 4)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(row_top), rt_value_float(row_height),
        rt_value_float(view_height), rt_value_float(current_scroll)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 4, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_FLOAT ||
        !isfinite(result->data.float_val) || result->data.float_val < 0.0) {
        if (result) rt_value_free(result);
        return false;
    }
    *target_scroll = (float)result->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_tree_scroll_center_target(
    float row_top, float view_height, float *target_scroll) {
    if (!target_scroll || !isfinite(row_top) || !isfinite(view_height) ||
        view_height < 0.0f || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "tree-scroll-center-target", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 2)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(row_top), rt_value_float(view_height)
    };
    if (!arguments[0] || !arguments[1]) {
        rt_value_free(arguments[0]);
        rt_value_free(arguments[1]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 2, arguments, &result);
    --s_callback_depth;
    rt_value_free(arguments[0]);
    rt_value_free(arguments[1]);
    if (!called || !result || result->type != RT_FLOAT ||
        !isfinite(result->data.float_val) || result->data.float_val < 0.0) {
        if (result) rt_value_free(result);
        return false;
    }
    *target_scroll = (float)result->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_tree_search_beam_row(
    float previous_row, float current_row, float animation_time,
    float *beam_row) {
    if (!beam_row || !isfinite(previous_row) || !isfinite(current_row) ||
        !isfinite(animation_time) || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "tree-search-beam-row", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 3)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(previous_row), rt_value_float(current_row),
        rt_value_float(animation_time)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 3, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_FLOAT ||
        !isfinite(result->data.float_val)) {
        if (result) rt_value_free(result);
        return false;
    }
    *beam_row = (float)result->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_tree_scroll_animation_step(
    float scroll_start, float scroll_target, float current_scroll,
    float scroll_time, float delta_time, float *next_scroll,
    float *next_scroll_time) {
    if (!next_scroll || !next_scroll_time || !isfinite(scroll_start) ||
        !isfinite(scroll_target) || !isfinite(current_scroll) ||
        !isfinite(scroll_time) || !isfinite(delta_time) ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "tree-scroll-animation-step", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 5)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(scroll_start), rt_value_float(scroll_target),
        rt_value_float(current_scroll), rt_value_float(scroll_time),
        rt_value_float(delta_time)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 5, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *scroll_value = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *time_value =
        parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!scroll_value || scroll_value->type != RT_FLOAT ||
        !isfinite(scroll_value->data.float_val) ||
        !time_value || time_value->type != RT_FLOAT ||
        !isfinite(time_value->data.float_val) ||
        time_value->data.float_val < 0.0 ||
        time_value->data.float_val > 1.0) {
        rt_value_free(result);
        return false;
    }
    *next_scroll = (float)scroll_value->data.float_val;
    *next_scroll_time = (float)time_value->data.float_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_tree_row_action(
    bool mouse_down, bool clicked, bool has_visibility, double mouse_x,
    double row_x, double visibility_x, double visibility_width,
    bool expandable, ObsMonadTreeRowAction *action) {
    if (!action || !isfinite(mouse_x) || !isfinite(row_x) ||
        !isfinite(visibility_x) || !isfinite(visibility_width) ||
        visibility_width < 0.0 || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "hierarchy-tree-row-action", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 8)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(mouse_down ? 1 : 0), rt_value_int(clicked ? 1 : 0),
        rt_value_int(has_visibility ? 1 : 0), rt_value_float(mouse_x),
        rt_value_float(row_x), rt_value_float(visibility_x),
        rt_value_float(visibility_width),
        rt_value_int(expandable ? 1 : 0)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 8, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < OBS_MONAD_TREE_ROW_NONE ||
        result->data.int_val > OBS_MONAD_TREE_ROW_DRAG) {
        if (result) rt_value_free(result);
        return false;
    }
    *action = (ObsMonadTreeRowAction)result->data.int_val;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_viewport_marquee_requires_containment(
    bool left_to_right, bool *requires_containment) {
    if (!requires_containment || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *transition = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "viewport-marquee-requires-containment", &transition) ||
        !transition || transition->type != RT_CLOSURE ||
        !transition->data.closure_val ||
        transition->data.closure_val->arity != 1)
        return false;

    RuntimeValue *arguments[] = {rt_value_bool(left_to_right)};
    if (!arguments[0]) return false;
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, transition, 1, arguments, &result);
    --s_callback_depth;
    rt_value_free(arguments[0]);
    if (!called || !result || result->type != RT_BOOL) {
        if (result) rt_value_free(result);
        return false;
    }
    *requires_containment = result->data.int_val != 0;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_viewport_marquee_drag_active(
    float delta_x, float delta_y, bool *active) {
    if (!active || !isfinite(delta_x) || !isfinite(delta_y) ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "viewport-marquee-drag-active", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 2)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(delta_x), rt_value_float(delta_y)
    };
    if (!arguments[0] || !arguments[1]) {
        rt_value_free(arguments[0]);
        rt_value_free(arguments[1]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 2, arguments, &result);
    --s_callback_depth;
    rt_value_free(arguments[0]);
    rt_value_free(arguments[1]);
    if (!called || !result || result->type != RT_INT ||
        (result->data.int_val != 0 && result->data.int_val != 1)) {
        if (result) rt_value_free(result);
        return false;
    }
    *active = result->data.int_val == 1;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_viewport_marquee_bounds_match(
    float min_x, float min_y, float max_x, float max_y,
    float selection_min_x, float selection_min_y,
    float selection_max_x, float selection_max_y,
    bool requires_containment, bool *matches) {
    if (!matches || !isfinite(min_x) || !isfinite(min_y) ||
        !isfinite(max_x) || !isfinite(max_y) ||
        !isfinite(selection_min_x) || !isfinite(selection_min_y) ||
        !isfinite(selection_max_x) || !isfinite(selection_max_y) ||
        !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "viewport-marquee-bounds-match", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 9)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_float(min_x), rt_value_float(min_y),
        rt_value_float(max_x), rt_value_float(max_y),
        rt_value_float(selection_min_x), rt_value_float(selection_min_y),
        rt_value_float(selection_max_x), rt_value_float(selection_max_y),
        rt_value_int(requires_containment ? 1 : 0)
    };
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!arguments[i]) {
            for (size_t j = 0; j < sizeof(arguments) / sizeof(arguments[0]); ++j)
                rt_value_free(arguments[j]);
            return false;
        }
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 9, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i)
        rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_INT ||
        (result->data.int_val != 0 && result->data.int_val != 1)) {
        if (result) rt_value_free(result);
        return false;
    }
    *matches = result->data.int_val == 1;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_viewport_marquee_focus(
    int selected_count, int first_mesh, int first_sdf,
    ObsMonadMarqueeTarget *target, int *target_index, bool *gizmo_active) {
    if (!target || !target_index || !gizmo_active || selected_count < 0 ||
        first_mesh < -1 || first_sdf < -1 || !s_evaluator ||
        !obs_monad_on_owner_thread() || s_callback_depth >= 32)
        return false;
    RuntimeValue *policy = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "viewport-marquee-focus", &policy) ||
        !policy || policy->type != RT_CLOSURE ||
        !policy->data.closure_val || policy->data.closure_val->arity != 3)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_int(selected_count), rt_value_int(first_mesh),
        rt_value_int(first_sdf)
    };
    if (!arguments[0] || !arguments[1] || !arguments[2]) {
        for (size_t i = 0; i < 3; ++i) rt_value_free(arguments[i]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, policy, 3, arguments, &result);
    --s_callback_depth;
    for (size_t i = 0; i < 3; ++i) rt_value_free(arguments[i]);
    if (!called || !result || result->type != RT_LIST ||
        !result->data.list_val || !result->data.list_val->cell) {
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeList *parts = result->data.list_val;
    RuntimeValue *target_value = rt_list_car(parts);
    parts = rt_list_cdr(parts);
    RuntimeValue *index_value =
        parts && parts->cell ? rt_list_car(parts) : NULL;
    parts = parts && parts->cell ? rt_list_cdr(parts) : NULL;
    RuntimeValue *gizmo_value =
        parts && parts->cell ? rt_list_car(parts) : NULL;
    if (!target_value || target_value->type != RT_INT ||
        target_value->data.int_val < OBS_MONAD_MARQUEE_TARGET_NONE ||
        target_value->data.int_val > OBS_MONAD_MARQUEE_TARGET_SDF ||
        !index_value || index_value->type != RT_INT ||
        !gizmo_value || gizmo_value->type != RT_INT ||
        (gizmo_value->data.int_val != 0 && gizmo_value->data.int_val != 1)) {
        rt_value_free(result);
        return false;
    }
    const int target_kind = (int)target_value->data.int_val;
    const int selected_index = (int)index_value->data.int_val;
    const bool single_selection = selected_count == 1;
    if ((target_kind == OBS_MONAD_MARQUEE_TARGET_NONE &&
         (selected_index != -1 || gizmo_value->data.int_val != 0)) ||
        (target_kind == OBS_MONAD_MARQUEE_TARGET_MESH &&
         (selected_count == 0 || first_mesh < 0 ||
          selected_index != first_mesh ||
          (gizmo_value->data.int_val == 1) != single_selection)) ||
        (target_kind == OBS_MONAD_MARQUEE_TARGET_SDF &&
         (selected_count == 0 || first_mesh >= 0 || first_sdf < 0 ||
          selected_index != first_sdf ||
          (gizmo_value->data.int_val == 1) != single_selection))) {
        rt_value_free(result);
        return false;
    }
    *target = (ObsMonadMarqueeTarget)target_kind;
    *target_index = selected_index;
    *gizmo_active = gizmo_value->data.int_val == 1;
    rt_value_free(result);
    return true;
}

bool obs_monad_live_editor_hierarchy_visibility(bool any_visible,
                                                bool *target_visible) {
    if (!target_visible || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *transition = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "hierarchy-visibility-target", &transition) ||
        !transition || transition->type != RT_CLOSURE ||
        !transition->data.closure_val ||
        transition->data.closure_val->arity != 1)
        return false;
    RuntimeValue *argument = rt_value_bool(any_visible);
    if (!argument) return false;
    RuntimeValue *arguments[] = {argument};
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, transition, 1, arguments, &result);
    --s_callback_depth;
    if (!called || !result || result->type != RT_BOOL)
        return false;
    *target_visible = result->data.int_val != 0;
    return true;
}

bool obs_monad_live_editor_hierarchy_expansion(bool expanded,
                                               bool *target_expanded) {
    if (!target_expanded || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *transition = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "hierarchy-expansion-target", &transition) ||
        !transition || transition->type != RT_CLOSURE ||
        !transition->data.closure_val ||
        transition->data.closure_val->arity != 1)
        return false;
    RuntimeValue *argument = rt_value_bool(expanded);
    if (!argument) return false;
    RuntimeValue *arguments[] = {argument};
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, transition, 1, arguments, &result);
    --s_callback_depth;
    if (!called || !result || result->type != RT_BOOL)
        return false;
    *target_expanded = result->data.int_val != 0;
    return true;
}

bool obs_monad_live_editor_inspector_mode(const char *subject,
                                          ObsMonadInspectorMode *mode) {
    if (!subject || !mode || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    RuntimeValue *transition = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "hierarchy-inspector-mode", &transition) ||
        !transition || transition->type != RT_CLOSURE ||
        !transition->data.closure_val ||
        transition->data.closure_val->arity != 1)
        return false;
    RuntimeValue *argument = rt_value_string(subject);
    if (!argument) return false;
    RuntimeValue *arguments[] = {argument};
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, transition, 1, arguments, &result);
    --s_callback_depth;
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < OBS_MONAD_INSPECTOR_MODE_DEFAULT ||
        result->data.int_val > OBS_MONAD_INSPECTOR_MODE_MATERIAL) {
        rt_value_free(argument);
        if (result) rt_value_free(result);
        return false;
    }
    RuntimeValue *state = rt_value_int(result->data.int_val);
    rt_value_free(argument);
    if (!state) {
        rt_value_free(result);
        return false;
    }
    const bool stored = monad_live_evaluator_binding_set(
        s_evaluator, "hierarchy-inspector-mode-state", state);
    if (!stored) {
        rt_value_free(state);
        rt_value_free(result);
    } else {
        *mode = (ObsMonadInspectorMode)result->data.int_val;
        rt_value_free(result);
    }
    return stored;
}

bool obs_monad_live_editor_inspector_current_mode(ObsMonadInspectorMode *mode) {
    if (!mode || !s_evaluator || !obs_monad_on_owner_thread())
        return false;
    RuntimeValue *value = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "hierarchy-inspector-mode-state", &value) ||
        !value || value->type != RT_INT ||
        value->data.int_val < OBS_MONAD_INSPECTOR_MODE_DEFAULT ||
        value->data.int_val > OBS_MONAD_INSPECTOR_MODE_MATERIAL)
        return false;
    *mode = (ObsMonadInspectorMode)value->data.int_val;
    return true;
}

bool obs_monad_live_editor_inspector_delete_action(
    bool has_mesh,
    ObsMonadInspectorDeleteAction *action) {
    if (!action || !s_evaluator || !obs_monad_on_owner_thread() ||
        s_callback_depth >= 32)
        return false;
    ObsMonadInspectorMode mode;
    if (!obs_monad_live_editor_inspector_current_mode(&mode)) return false;
    RuntimeValue *transition = NULL;
    if (!monad_live_evaluator_binding_value(
            s_evaluator, "inspector-delete-action", &transition) ||
        !transition || transition->type != RT_CLOSURE ||
        !transition->data.closure_val ||
        transition->data.closure_val->arity != 2)
        return false;
    RuntimeValue *arguments[] = {
        rt_value_bool(has_mesh), rt_value_int(mode)
    };
    if (!arguments[0] || !arguments[1]) {
        rt_value_free(arguments[0]);
        rt_value_free(arguments[1]);
        return false;
    }
    RuntimeValue *result = NULL;
    ++s_callback_depth;
    const bool called = monad_live_evaluator_call_closure_guarded(
        s_evaluator, transition, 2, arguments, &result);
    --s_callback_depth;
    rt_value_free(arguments[0]);
    rt_value_free(arguments[1]);
    if (!called || !result || result->type != RT_INT ||
        result->data.int_val < OBS_MONAD_INSPECTOR_DELETE_WHOLE_OBJECT ||
        result->data.int_val > OBS_MONAD_INSPECTOR_DELETE_SKELETON) {
        if (result) rt_value_free(result);
        return false;
    }
    *action = (ObsMonadInspectorDeleteAction)result->data.int_val;
    rt_value_free(result);
    return true;
}

static void variable_value_text(const RuntimeValue *value, char *out,
                                size_t capacity) {
    if (!capacity) return;
    if (!value) {
        snprintf(out, capacity, "unavailable");
        return;
    }
    switch (value->type) {
    case RT_INT:
        snprintf(out, capacity, "%lld",
                 (long long)value->data.int_val);
        break;
    case RT_BOOL:
        snprintf(out, capacity, "%s",
                 value->data.int_val ? "True" : "False");
        break;
    case RT_FLOAT:
        snprintf(out, capacity, "%.7g", value->data.float_val);
        break;
    case RT_CHAR:
        snprintf(out, capacity, "character %u",
                 (unsigned char)value->data.char_val);
        break;
    case RT_STRING:
        snprintf(out, capacity, "%s",
                 value->data.string_val ? value->data.string_val : "");
        break;
    case RT_SYMBOL:
        snprintf(out, capacity, "%s",
                 value->data.symbol_val ? value->data.symbol_val : "symbol");
        break;
    case RT_KEYWORD:
        snprintf(out, capacity, ":%s",
                 value->data.keyword_val ? value->data.keyword_val : "");
        break;
    case RT_RATIO:
        snprintf(out, capacity, "%lld/%lld",
                 (long long)value->data.ratio_val.numerator,
                 (long long)value->data.ratio_val.denominator);
        break;
    case RT_NIL:
        snprintf(out, capacity, "nil");
        break;
    case RT_CLOSURE:
        snprintf(out, capacity, "<function>");
        break;
    case RT_OPAQUE:
        snprintf(out, capacity, "<foreign object>");
        break;
    case RT_ARRAY:
        snprintf(out, capacity, "<array:%zu>", value->data.array_val.length);
        break;
    case RT_LIST:
        snprintf(out, capacity, "<list>");
        break;
    case RT_SET:
        snprintf(out, capacity, "<set>");
        break;
    case RT_MAP:
        snprintf(out, capacity, "<map>");
        break;
    case RT_THUNK:
        snprintf(out, capacity, "<thunk>");
        break;
    case RT_BIGNUM:
        snprintf(out, capacity, "<big integer>");
        break;
    case RT_PROOF:
        snprintf(out, capacity, "<proof>");
        break;
    }
}

bool obs_monad_live_editor_visit_variables(
    ObsMonadVariableVisitor visitor, void *context, bool *truncated) {
    if (truncated) *truncated = false;
    if (!visitor || !s_evaluator || !obs_monad_on_owner_thread()) return false;
    MonadControlBinding *records =
        calloc(MONAD_CONTROL_BINDING_MAX, sizeof(*records));
    if (!records) return false;
    uint32_t count = 0;
    bool source_truncated = false;
    bool ok = monad_live_evaluator_bindings(
        s_evaluator, records, MONAD_CONTROL_BINDING_MAX, &count,
        &source_truncated);
    bool visitor_truncated = false;
    for (uint32_t i = 0; ok && i < count; ++i) {
        const MonadControlBinding *binding = &records[i];
        if (binding->kind != MONAD_CONTROL_BINDING_VAR) continue;
        ObsMonadVariableView view = {0};
        snprintf(view.name, sizeof(view.name), "%s", binding->name);
        snprintf(view.type, sizeof(view.type), "%s", binding->type);
        snprintf(view.documentation, sizeof(view.documentation), "%s",
                 binding->doc);
        RuntimeValue *value = NULL;
        (void)monad_live_evaluator_binding_value(
            s_evaluator, binding->name, &value);
        variable_value_text(value, view.value, sizeof(view.value));
        if (!visitor(&view, context)) {
            visitor_truncated = true;
            break;
        }
    }
    free(records);
    if (truncated) *truncated = source_truncated || visitor_truncated;
    return ok;
}

KeyChordBinding *obs_monad_live_editor_find_binding(const char *map_name,
                                                     const char *notation) {
    if (!map_name || !notation) return NULL;
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        ObsLiveKeymap *map = &s_maps[i];
        if (map->magic == OBS_KEYMAP_MAGIC && map->name && map->map &&
            strcmp(map->name, map_name) == 0)
            return keychord_find_binding(map->map, notation);
    }
    return NULL;
}

bool obs_monad_live_editor_copy_map(const char *source_name,
                                    KeyChordMap *destination) {
    if (!source_name || !destination || !destination->bindings) return false;
    ObsLiveKeymap *source = NULL;
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        ObsLiveKeymap *candidate = &s_maps[i];
        if (candidate->magic == OBS_KEYMAP_MAGIC && candidate->name &&
            candidate->map && strcmp(candidate->name, source_name) == 0) {
            source = candidate;
            break;
        }
    }
    if (!source) return false;
    const size_t initial_count = destination->count;
    for (size_t i = 0; i < source->map->count; ++i) {
        const KeyChordBinding *binding = &source->map->bindings[i];
        if (binding->action_type != ACTION_MONAD_PROC ||
            !binding->action.monad_proc || !binding->command_name ||
            !binding->notation ||
            !keychord_bind_managed_monad_named(
                destination, binding->notation, binding->action.monad_proc,
                binding->command_name, binding->description,
                binding->action_type_flag))
            return false;
    }
    return destination->count > initial_count;
}

bool obs_monad_live_editor_open(ObsMonadRuntime *host, const char **error) {
    if (!host || !host->active || s_evaluator) {
        set_failure(error, "Monad live editor image cannot be opened");
        return false;
    }
    s_host = host;
    s_evaluator = monad_live_evaluator_create();
    if (!s_evaluator) {
        s_host = NULL;
        set_failure(error, "could not create Monad live editor image");
        return false;
    }
    host->live_evaluator = s_evaluator;
    if (!monad_live_evaluator_run_named_quiet(
            s_evaluator, "(data ObsidianKeymap ObsidianKeymapRef)",
            "<obsidian:editor-types>") ||
        !monad_live_evaluator_run_named_quiet(
            s_evaluator,
            "(data ObsidianMonadWindow ObsidianMonadWindowRef)",
            "<obsidian:editor-types>") ||
        !monad_live_evaluator_run_named_quiet(
            s_evaluator,
            "(data ObsidianEditorPanel ObsidianEditorPanelRef)",
            "<obsidian:editor-types>") ||
        !monad_live_evaluator_run_named_quiet(
            s_evaluator,
            "(data ObsidianGizmoMode ObsidianGizmoModeRef)",
            "<obsidian:editor-types>") ||
        !monad_live_evaluator_run_named_quiet(
            s_evaluator,
            "(data ObsidianRenderSetting ObsidianRenderSettingRef)",
            "<obsidian:editor-types>") ||
        !monad_live_evaluator_run_named_quiet(
            s_evaluator,
            "(data ObsidianAddMenu ObsidianAddMenuRef)",
            "<obsidian:editor-types>") ||
        !monad_live_evaluator_run_named_quiet(
            s_evaluator,
            "(data ObsidianEditor ObsidianEditorRef)",
            "<obsidian:editor-types>") ||
        !monad_live_evaluator_run_named_quiet(
            s_evaluator,
            "(data ObsidianTransformMode ObsidianTransformModeRef)",
            "<obsidian:editor-types>") ||
        !monad_live_evaluator_run_named_quiet(
            s_evaluator,
            "(data ObsidianTransformAxis ObsidianTransformAxisRef)",
            "<obsidian:editor-types>") ||
        !register_live_keymap_api()) {
        set_failure(error, monad_live_evaluator_last_diagnostic(s_evaluator));
        for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i)
            s_maps[i].registered = false;
        for (size_t i = 0; i < s_action_count; ++i)
            s_actions[i].published_generation = 0;
        monad_live_evaluator_destroy(s_evaluator);
        host->live_evaluator = NULL;
        s_evaluator = NULL;
        s_vertico_width = NULL;
        s_vertico_count = NULL;
        s_vertico_position = NULL;
        s_vertico_scroll_margin = NULL;
        s_vertico_resize_fixelwise = NULL;
        s_host = NULL;
        return false;
    }
    s_module_loaded = false;
    return true;
}

static void release_staged_operations(void) {
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        ObsLiveKeymap *map = &s_maps[i];
        for (size_t j = 0; j < map->operation_count; ++j) {
            monad_value_release(map->operations[j].image.procedure);
            map->operations[j] = (ObsLiveKeyOperation){0};
        }
        map->operation_count = 0;
    }
}

static bool prepare_keymap_images(void) {
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        ObsLiveKeymap *map = &s_maps[i];
        if (map->magic != OBS_KEYMAP_MAGIC || !map->map ||
            (!map->name && !map->module_owned))
            continue;
        KeyImageOperation operations[OBS_LIVE_KEY_OP_CAP];
        for (size_t j = 0; j < map->operation_count; ++j)
            operations[j] = map->operations[j].image;
        if (!keychord_prepare_monad_image(map->map, operations,
                                          map->operation_count,
                                          &map->prepared)) {
            for (size_t k = 0; k < OBS_LIVE_MAP_CAP; ++k)
                keychord_discard_prepared_image(&s_maps[k].prepared);
            return false;
        }
    }
    return true;
}

static void commit_keymap_images(void) {
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        ObsLiveKeymap *map = &s_maps[i];
        if (map->prepared.bindings &&
            !keychord_commit_monad_image(map->map, &map->prepared))
            abort();
    }
}

static bool read_live_source(const char *path, char **source, const char **error) {
    if (!path || !*path || !source) {
        set_failure(error, "invalid live editor module path");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        set_failure(error, "cannot open live editor module");
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        set_failure(error, "cannot seek live editor module");
        return false;
    }
    long length = ftell(file);
    if (length < 0 || (unsigned long)length > OBS_LIVE_SOURCE_LIMIT ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        set_failure(error, "live editor module exceeds its source limit");
        return false;
    }
    char *buffer = malloc((size_t)length + 1);
    if (!buffer) {
        fclose(file);
        set_failure(error, "out of memory reading live editor module");
        return false;
    }
    size_t bytes = fread(buffer, 1, (size_t)length, file);
    bool ok = bytes == (size_t)length && !ferror(file);
    fclose(file);
    if (!ok || memchr(buffer, '\0', bytes)) {
        free(buffer);
        set_failure(error, "live editor module read failed or contains NUL");
        return false;
    }
    buffer[bytes] = '\0';
    *source = buffer;
    return true;
}

static const char *live_editor_init_path(void) {
    const char *path = getenv("OBSIDIAN_MONAD_INIT");
    return path && *path ? path : "init.mon";
}

static bool append_live_source(char **base, char *suffix,
                               const char **error) {
    if (!base || !*base || !suffix) {
        free(suffix);
        set_failure(error, "invalid live editor source composition");
        return false;
    }
    const size_t base_length = strlen(*base);
    const size_t suffix_length = strlen(suffix);
    if (suffix_length > OBS_LIVE_SOURCE_LIMIT ||
        base_length > OBS_LIVE_SOURCE_LIMIT - suffix_length ||
        base_length + suffix_length > OBS_LIVE_SOURCE_LIMIT - 2) {
        free(suffix);
        set_failure(error, "combined live editor source exceeds its source limit");
        return false;
    }
    char *combined = malloc(base_length + suffix_length + 2);
    if (!combined) {
        free(suffix);
        set_failure(error, "out of memory composing live editor source");
        return false;
    }
    memcpy(combined, *base, base_length);
    combined[base_length] = '\n';
    memcpy(combined + base_length + 1, suffix, suffix_length + 1);
    free(*base);
    free(suffix);
    *base = combined;
    return true;
}

static bool read_live_editor_source(const char *path, char **source,
                                    const char **error) {
    char *bundle = NULL;
    if (!read_live_source("monad/obsidian/orderless.mon", &bundle, error))
        return false;
    char *marginalia = NULL;
    if (!read_live_source("monad/obsidian/marginalia.mon", &marginalia,
                          error) ||
        !append_live_source(&bundle, marginalia, error)) {
        free(bundle);
        return false;
    }
    char *widgets = NULL;
    if (!read_live_source("monad/obsidian/widgets.mon", &widgets, error) ||
        !append_live_source(&bundle, widgets, error)) {
        free(bundle);
        return false;
    }
    if (strcmp(path, "monad/obsidian/orderless.mon") != 0 &&
        strcmp(path, "monad/obsidian/marginalia.mon") != 0) {
        char *module = NULL;
        if (!read_live_source(path, &module, error) ||
            !append_live_source(&bundle, module, error)) {
            free(bundle);
            return false;
        }
    }
    const char *init_path = live_editor_init_path();
    if (strcmp(path, init_path) != 0 &&
        strcmp(path, "monad/obsidian/orderless.mon") != 0 &&
        strcmp(path, "monad/obsidian/marginalia.mon") != 0) {
        char *init = NULL;
        if (!read_live_source(init_path, &init, error) ||
            !append_live_source(&bundle, init, error)) {
            free(bundle);
            return false;
        }
    }
    *source = bundle;
    return true;
}

static char *next_live_form(const char **cursor, size_t *line_cursor) {
    const char *line = *cursor;
    const char *begin = NULL;
    const char *end = NULL;
    size_t current_line = *line_cursor;
    size_t begin_line = 0;
    size_t depth = 0;
    bool parenthesized = false;
    bool in_string = false;
    bool escaped = false;

    for (;;) {
        const char *line_end = strchr(line, '\n');
        if (!line_end) line_end = line + strlen(line);
        const char *first = line;
        while (first < line_end &&
               (*first == ' ' || *first == '\t' || *first == '\r'))
            ++first;
        const bool blank_or_comment =
            first == line_end || *first == ';';

        if (!begin) {
            if (!blank_or_comment) {
                begin = line;
                begin_line = current_line;
                parenthesized = *first == '(';
                depth = 0;
                in_string = false;
                escaped = false;
            }
        } else if (!parenthesized && blank_or_comment) {
            end = line;
            *cursor = line;
            *line_cursor = current_line;
            break;
        }

        if (begin && parenthesized) {
            for (const char *p = line; p < line_end; ++p) {
                if (in_string) {
                    if (escaped) escaped = false;
                    else if (*p == '\\') escaped = true;
                    else if (*p == '"') in_string = false;
                    continue;
                }
                if (*p == ';') break;
                if (*p == '"') in_string = true;
                else if (*p == '(') ++depth;
                else if (*p == ')' && depth > 0) --depth;
            }
            if (depth == 0 && !in_string) {
                end = line_end;
                if (*line_end) {
                    *cursor = line_end + 1;
                    *line_cursor = current_line + 1;
                } else {
                    *cursor = line_end;
                    *line_cursor = current_line;
                }
                break;
            }
        }
        if (*line_end == '\0') {
            if (!begin) {
                *cursor = line_end;
                *line_cursor = current_line;
                return NULL;
            }
            if (parenthesized && (depth != 0 || in_string))
                return NULL;
            end = line_end;
            *cursor = line_end;
            *line_cursor = current_line;
            break;
        }
        line = line_end + 1;
        ++current_line;
    }

    while (end > begin &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
            end[-1] == '\n'))
        --end;
    const size_t prefix = begin_line;
    const size_t length = (size_t)(end - begin);
    if (prefix > SIZE_MAX - length - 1) return NULL;
    char *form = malloc(prefix + length + 1);
    if (!form) return NULL;
    memset(form, '\n', prefix);
    memcpy(form + prefix, begin, length);
    form[prefix + length] = '\0';
    return form;
}

static bool live_editor_load_source(ObsMonadRuntime *host, const char *path,
                                    char *source, const char **error) {
    if (error) *error = NULL;
    if (!host || host != s_host || !s_evaluator ||
        !obs_monad_on_owner_thread()) {
        free(source);
        set_failure(error, "Monad live editor load requires its owner thread");
        return false;
    }
    s_keymap_transaction = true;
    s_keymap_images_prepared = false;
    if (++s_transaction_id == 0) ++s_transaction_id;
    release_staged_operations();
    if (!obs_monad_live_command_catalog_begin()) {
        s_keymap_transaction = false;
        free(source);
        set_failure(error, "could not stage live editor command catalog");
        return false;
    }
    bool batch_started = monad_live_evaluator_batch_begin(s_evaluator);
    bool ok = batch_started;
    const char *cursor = source;
    size_t line_cursor = 0;
    while (ok) {
        char *form = next_live_form(&cursor, &line_cursor);
        if (!form) {
            while (*cursor == ' ' || *cursor == '\t' ||
                   *cursor == '\r' || *cursor == '\n')
                ++cursor;
            if (*cursor) {
                set_failure(error,
                            "live editor module contains an invalid top-level form");
                ok = false;
            }
            break;
        }
        ok = monad_live_evaluator_run_named_quiet(s_evaluator, form, path);
        free(form);
    }
    if (ok) {
        ok = monad_live_evaluator_run_named_quiet(
            s_evaluator, "(obsidian-editor-prepare-keymaps!)", path);
    }
    if (batch_started && !monad_live_evaluator_batch_end(s_evaluator, ok)) {
        ok = false;
    }
    if (ok && !capture_vertico_options()) {
        fprintf(stderr,
                "Monad editor module must define typed Vertico layout variables\n");
        ok = false;
    }
    if (ok && !capture_default_completion_strategy()) {
        fprintf(stderr,
                "Monad editor module must define its default completion strategy\n");
        ok = false;
    }
    if (ok && !s_keymap_images_prepared) {
        fprintf(stderr, "Monad editor keymap images were not prepared\n");
        if (error)
            *error = "Monad editor keymap images were not prepared";
        ok = false;
    }
    if (ok) {
        commit_keymap_images();
        for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i)
            if (s_maps[i].transaction_id == s_transaction_id)
                s_maps[i].transaction_id = 0;
        obs_monad_live_command_catalog_finish(true);
        s_module_loaded = true;
    } else {
        for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i)
            keychord_discard_prepared_image(&s_maps[i].prepared);
        obs_monad_live_command_catalog_finish(false);
        if (error && !*error)
            *error = monad_live_evaluator_last_diagnostic(s_evaluator);
    }
    release_staged_operations();
    s_keymap_transaction = false;
    s_keymap_images_prepared = false;
    if (!ok) {
        for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
            ObsLiveKeymap *map = &s_maps[i];
            if (map->transaction_id != s_transaction_id) continue;
            keymap_free(&map->owned_map);
            *map = (ObsLiveKeymap){0};
        }
        if (error && !*error)
            *error = monad_live_evaluator_last_diagnostic(s_evaluator);
    }
    if (ok) {
        free(s_current_source);
        s_current_source = source;
    } else {
        free(source);
    }
    return ok;
}

bool obs_monad_live_editor_load_file(ObsMonadRuntime *host, const char *path,
                                     const char **error) {
    if (s_module_loaded)
        return obs_monad_live_editor_reload_file(host, path, error);
    if (error) *error = NULL;
    if (!host || host != s_host || !s_evaluator ||
        !obs_monad_on_owner_thread()) {
        set_failure(error, "Monad live editor load requires its owner thread");
        return false;
    }
    char *source = NULL;
    if (!read_live_editor_source(path, &source, error)) return false;
    return live_editor_load_source(host, path, source, error);
}

bool obs_monad_live_editor_reload_file(ObsMonadRuntime *host,
                                      const char *path, const char **error) {
    if (error) *error = NULL;
    if (!host || host != s_host || !s_evaluator ||
        !obs_monad_on_owner_thread()) {
        set_failure(error, "Monad live editor reload requires its owner thread");
        return false;
    }
    if (!s_module_loaded)
        return obs_monad_live_editor_load_file(host, path, error);
    if (!s_current_source) {
        set_failure(error, "the active live editor source is unavailable for rollback");
        return false;
    }
    char *candidate_source = NULL;
    if (!read_live_editor_source(path, &candidate_source, error)) return false;
    if (strcmp(candidate_source, s_current_source) == 0) {
        free(candidate_source);
        return true;
    }
    free(candidate_source);
    set_failure(error,
        "changed editor keymap source is not reloadable in the active evaluator; "
        "the current image remains active");
    return false;
}

void obs_monad_live_editor_clear_map_bindings(void) {
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        ObsLiveKeymap *map = &s_maps[i];
        if (map->magic == OBS_KEYMAP_MAGIC && map->map)
            (void)keychord_remove_managed_monad(map->map);
    }
}

void obs_monad_live_editor_close(ObsMonadRuntime *host) {
    if (!host || host != s_host) return;
    obs_monad_window_discard_live_callbacks();
    keymap_clear_overriding();
    obs_monad_live_command_catalog_finish(false);
    obs_monad_live_command_catalog_clear();
    obs_monad_live_editor_clear_map_bindings();
    release_staged_operations();
    for (size_t i = 0; i < OBS_LIVE_MAP_CAP; ++i) {
        ObsLiveKeymap *map = &s_maps[i];
        keychord_discard_prepared_image(&map->prepared);
        if (map->owns_map) {
            keymap_free(&map->owned_map);
            *map = (ObsLiveKeymap){0};
        } else {
            map->registered = false;
            map->operation_count = 0;
            map->transaction_id = 0;
            map->module_owned = false;
        }
    }
    if (s_evaluator) monad_live_evaluator_destroy(s_evaluator);
    s_evaluator = NULL;
    obs_monad_window_live_runtime_closed();
    s_vertico_width = NULL;
    s_vertico_count = NULL;
    s_vertico_position = NULL;
    s_vertico_scroll_margin = NULL;
    s_vertico_resize_fixelwise = NULL;
    s_completion_record = NULL;
    s_completion_depth = 0;
    host->live_evaluator = NULL;
    s_host = NULL;
    s_keymap_transaction = false;
    s_module_loaded = false;
    free(s_current_source);
    s_current_source = NULL;
    for (size_t i = 0; i < s_action_count; ++i)
        s_actions[i].published_generation = 0;
    s_symbol_sequence = 0;
}
