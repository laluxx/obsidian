#include "obsidian.h"
#include "editor.h"
#include "image_editor.h"
#include "material_browser.h"
#include "live_editor.h"
#include "easing.h"
#include "add_menu.h"
#include "gizmo.h"
#include "gltf_loader.h"
#include "hierarchy_selection_internal.h"
#include "scene_buffers.h"
#include "editor_frames.h"
#include "gizmo.h"
#include "text_editor.h"
#include "../../src/monad/runtime.h"
#include "../../src/platform/window.h"
#include "live_eval.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int g_selected_sdf_index;

static ObsMaterialBrowserEntry *preview;
static ObsImageEditor image = {.texture_index = -1};

static void draw_preview(void *unused) {
    (void)unused;
    quad2D((vec2){0, 0}, (vec2){256, 256},
           (Color){0.05f, 0.05f, 0.05f, 1});
    obs_material_browser_render(preview, 128, 128, 128);
}

static void draw_editor_surface(void *unused) {
    (void)unused;
    editor.panels[PANEL_TOP].t = 1.0f;
    editor.panels[PANEL_TOP].target_t = 1.0f;
    editor_render();
}

static void draw_image(void *unused) {
    (void)unused;
    obs_image_editor_render(&image);
}

static void draw_editor_frame(ObsWindow *window) {
    ObsEditorFrame *frame = obs_editor_frame_current();
    obs_begin_frame(window);
    if (obs_editor_frame_is_2d_only(frame))
        obs_editor_frame_render_surface(frame);
    else {
        editor_update();
        editor_render();
    }
    obs_end_frame(window);
}

static void draw_headless_editor_frame(void *unused) {
    (void)unused;
    editor_update();
    editor_render();
}

static bool wait_for_headless_scene_mesh_growth(size_t previous_count) {
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        if (scene->meshes.count > previous_count) return true;
        if (!obs_headless_render_frame(draw_headless_editor_frame, NULL))
            return false;
        usleep(10000);
    }
    return scene->meshes.count > previous_count;
}

static int64_t live_window_smoke_integer(const char *name) {
    RuntimeValue *value = NULL;
    assert(monad_live_evaluator_binding_value(
        obs_monad_engine()->live_evaluator, name, &value));
    return rt_unbox_int(value);
}

static void click_editor_point(GLFWwindow *window, ObsWindow *surface,
                               double x, double y) {
    glfwSetCursorPos(window, x, y);
    internal_cursor_position_callback(window, x, y);
    editor_mouse_move(x, y);
    editor_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
    draw_editor_frame(surface);
    editor_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
}

static unsigned char *read_pixels(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    unsigned char *pixels = malloc(256 * 256 * 4);
    assert(pixels);
    assert(fread(pixels, 4, 256 * 256, file) == 256 * 256);
    assert(fgetc(file) == EOF);
    fclose(file);
    return pixels;
}

int main(int argc, char **argv) {
    bool windows = argc == 2 && strcmp(argv[1], "--windows") == 0;
    if (windows) {
        /* X11 supports deterministic pointer warps; Wayland deliberately
         * forbids them, so synthetic native mouse tests use Xwayland. */
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        assert(setenv("OBSIDIAN_MONAD_INIT",
                      "tests/native/monad-window-smoke.mon", 1) == 0);
        assert(initWindow(640, 480, "Obsidian frame regression"));
    }
    else {
        obs_set_editor_enabled(true);
        assert(obs_headless_init(256, 256, true));
        init_free_type();
        keymap_init(&keymap);
        initThemes();
        ease_init();
        editor_init();
        gizmo_init();
        vertico_init();
        add_menu_init();
        assert(obs_register_editor_commands());
        assert(obs_monad_engine_start());
        obs_monad_engine_boot_from_environment();
    }
    ObsMonadReloadReport report = {0};
    obs_monad_engine_reload_report(&report);
    if (report.state != OBS_MONAD_RELOAD_SUCCEEDED)
        fprintf(stderr, "Monad smoke boot failed: %s\n", report.message);
    assert(report.state == OBS_MONAD_RELOAD_SUCCEEDED);
    if (access("./assets", R_OK | X_OK) != 0) {
        assert(strcmp(editor.file_manager.current_path, ".") == 0);
        assert(editor.file_manager.item_count > 0);
    }
    {
        ObsMonadCompletionContext completion = {0};
        float before = 0.0f, after = 0.0f, other_context = 0.0f;
        float cancelled = 0.0f;
        assert(obs_monad_live_editor_completion_begin(
            "history-contract", "alpha", &completion));
        assert(obs_monad_live_editor_completion_score(
            &completion, "alpha", &before));
        obs_monad_live_editor_completion_end(&completion);
        assert(obs_monad_live_editor_completion_begin(
            "history-contract", "alpha", &completion));
        assert(obs_monad_live_editor_completion_score(
            &completion, "alpha", &cancelled));
        obs_monad_live_editor_completion_end(&completion);
        assert(cancelled == before);
        assert(obs_monad_live_editor_completion_record(
            "history-contract", "alpha"));
        assert(obs_monad_live_editor_completion_begin(
            "history-contract", "alpha", &completion));
        assert(obs_monad_live_editor_completion_score(
            &completion, "alpha", &after));
        obs_monad_live_editor_completion_end(&completion);
        assert(after > before);
        assert(obs_monad_live_editor_completion_begin(
            "other-contract", "alpha", &completion));
        assert(obs_monad_live_editor_completion_score(
            &completion, "alpha", &other_context));
        obs_monad_live_editor_completion_end(&completion);
        assert(other_context == before);
        float older_pick = 0.0f, newer_pick = 0.0f, unmatched = 0.0f;
        assert(obs_monad_live_editor_completion_record(
            "history-order-contract", "alpha"));
        assert(obs_monad_live_editor_completion_record(
            "history-order-contract", "alphabet"));
        assert(obs_monad_live_editor_completion_begin(
            "history-order-contract", "alpha", &completion));
        assert(obs_monad_live_editor_completion_score(
            &completion, "alpha", &older_pick));
        assert(obs_monad_live_editor_completion_score(
            &completion, "alphabet", &newer_pick));
        assert(obs_monad_live_editor_completion_score(
            &completion, "omega", &unmatched));
        obs_monad_live_editor_completion_end(&completion);
        assert(newer_pick > older_pick);
        assert(unmatched <= 0.0f);
        assert(obs_monad_live_editor_completion_record(
            "history-order-contract", "alpha"));
        assert(obs_monad_live_editor_completion_begin(
            "history-order-contract", "alpha", &completion));
        assert(obs_monad_live_editor_completion_score(
            &completion, "alpha", &older_pick));
        assert(obs_monad_live_editor_completion_score(
            &completion, "alphabet", &newer_pick));
        obs_monad_live_editor_completion_end(&completion);
        assert(older_pick > newer_pick);
        puts("Monad Vertico accepted completion history and context isolation PASS");
    }
    {
        ObsMonadFilesystemActivation activation;
        assert(obs_monad_live_editor_filesystem_activation(
            ".obj", true, &activation));
        assert(activation == OBS_MONAD_FILESYSTEM_ACTIVATE_SCENE_PREVIEW);
        assert(obs_monad_live_editor_filesystem_activation(
            ".gltf", true, &activation));
        assert(activation == OBS_MONAD_FILESYSTEM_ACTIVATE_SCENE_PREVIEW);
        assert(obs_monad_live_editor_filesystem_activation(
            ".glb", true, &activation));
        assert(activation == OBS_MONAD_FILESYSTEM_ACTIVATE_SCENE_PREVIEW);
        static const char *const image_extensions[] = {
            ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr", ".exr"};
        for (size_t i = 0;
             i < sizeof(image_extensions) / sizeof(image_extensions[0]); ++i) {
            assert(obs_monad_live_editor_filesystem_activation(
                image_extensions[i], true, &activation));
            assert(activation ==
                   OBS_MONAD_FILESYSTEM_ACTIVATE_IMAGE_PREVIEW);
        }
        assert(obs_monad_live_editor_filesystem_activation(
            ".mon", true, &activation));
        assert(activation == OBS_MONAD_FILESYSTEM_ACTIVATE_TEXT);
        assert(obs_monad_live_editor_filesystem_activation(
            ".obj", false, &activation));
        assert(activation == OBS_MONAD_FILESYSTEM_ACTIVATE_NONE);
        assert(obs_monad_live_editor_filesystem_activation(
            "", true, &activation));
        assert(activation == OBS_MONAD_FILESYSTEM_ACTIVATE_NONE);
        bool is_double_click = false;
        assert(obs_monad_live_editor_filesystem_double_click(
            4, 4, 1.0, 1.299, &is_double_click));
        assert(is_double_click);
        assert(obs_monad_live_editor_filesystem_double_click(
            4, 4, 1.0, 1.301, &is_double_click));
        assert(!is_double_click);
        assert(obs_monad_live_editor_filesystem_double_click(
            4, 5, 1.0, 1.1, &is_double_click));
        assert(!is_double_click);
        puts("Monad filesystem activation and double-click policies PASS");
    }
    assert(obs_scene_buffers_init(scene, "Primary", NULL));
    assert(obs_editor_frames_init(context.primary));
    if (windows) {
        const double center_x = context.drawExtent.width * 0.5;
        const double top_y = context.drawExtent.height * 0.2;
        const double bottom_y = context.drawExtent.height * 0.8;
        vec3 top_origin, top_direction, bottom_origin, bottom_direction;
        assert(gizmo_screen_ray(center_x, top_y, top_origin, top_direction));
        assert(gizmo_screen_ray(center_x, bottom_y, bottom_origin,
                                bottom_direction));
        assert(glm_vec3_dot(top_direction, camera.up) >
               glm_vec3_dot(bottom_direction, camera.up));
        ObsEditorFrame *primary = obs_editor_frame_current();
        cube((vec3){0, 0, 0}, 2, RED);
        inspector_select_mesh(0);
        GLFWwindow *native = obs_editor_frame_window(primary)->window;
        int logical_width = 0, logical_height = 0;
        glfwGetWindowSize(native, &logical_width, &logical_height);
        const double pointer_x = logical_width * 0.5;
        const double pointer_y = logical_height * 0.5;
        glfwSetCursorPos(native, pointer_x, pointer_y);
        internal_cursor_position_callback(native, pointer_x, pointer_y);
        draw_editor_frame(obs_editor_frame_window(primary));
        gizmo.mode = GIZMO_MODE_ROTATE;
        gizmo_mouse_move(center_x, context.drawExtent.height * 0.5 - 36.0);
        assert(gizmo.hovered == GIZMO_PART_SPHERE);
        gizmo_mouse_move(center_x, context.drawExtent.height * 0.5 + 36.0);
        assert(gizmo.hovered == GIZMO_PART_SPHERE);
        gizmo.mode = GIZMO_MODE_TRANSLATE;
        glfwPollEvents();
        glfwFocusWindow(native);
        usleep(50000);
        glfwPollEvents();
        glfwSetCursorPos(native, 420, 240);
        internal_cursor_position_callback(native, 420, 240);
        KeyChordBinding *open_color = keychord_find_binding(&keymap, "c");
        assert(open_color && open_color->managed_monad);
        keychord_invoke_binding(open_color);
        ColorPickerState *picker = editor_color_picker();
        assert(picker->visible);
        float cx = ui_window_content_x(&picker->window) + picker->radius;
        float cy = ui_window_content_y(&picker->window) +
                   picker->window.content_h - picker->radius;
        double px = cx;
        double py = context.drawExtent.height -
                    (cy + picker->radius * 0.88f);
        glfwSetCursorPos(native, px, py);
        internal_cursor_position_callback(native, px, py);
        internal_mouse_button_callback(native, GLFW_MOUSE_BUTTON_LEFT,
                                       GLFW_PRESS, 0);
        assert(picker->drag == CP_DRAG_HUE);
        assert(scene->meshes.items[0].baseColorFactor[1] > 0.8f);
        internal_mouse_button_callback(native, GLFW_MOUSE_BUTTON_LEFT,
                                       GLFW_RELEASE, 0);
        assert(picker->drag == CP_DRAG_NONE);
        float close_x = ui_window_panel_x(&picker->window) +
                        ui_window_panel_w(&picker->window) - UI_PANEL_PAD - 6;
        float close_y = context.drawExtent.height -
                        (ui_window_panel_y(&picker->window) +
                         ui_window_panel_h(&picker->window) -
                         UI_TITLE_H * 0.5f);
        glfwSetCursorPos(native, close_x, close_y);
        internal_cursor_position_callback(native, close_x, close_y);
        internal_mouse_button_callback(native, GLFW_MOUSE_BUTTON_LEFT,
                                       GLFW_PRESS, 0);
        assert(picker->window.closing);
        keychord_invoke_binding(open_color);
        KeyChordBinding *cancel = keychord_find_binding(&keymap, "C-g");
        assert(cancel && cancel->managed_monad);
        keychord_invoke_binding(cancel);
        assert(picker->window.closing && picker->target_color == NULL);
        puts("Actual GLFW color-picker hue click/release, close button and Monad C-g PASS");
        for (unsigned i = 0; i < 30 && picker->visible; ++i) {
            draw_editor_frame(obs_editor_frame_window(primary));
            usleep(20000);
        }
        assert(!picker->visible);
        assert(obs_monad_engine_start());
        bool tree_expanded = false;
        assert(obs_monad_live_editor_filesystem_tree_command(
            "clear", "", &tree_expanded));

        char tree_root[] = "/tmp/obsidian-tree-XXXXXX";
        assert(mkdtemp(tree_root));
        assert(obs_monad_live_editor_filesystem_tree_command(
            "toggle", tree_root, &tree_expanded));
        assert(tree_expanded);
        assert(obs_monad_live_editor_filesystem_tree_command(
            "contains", tree_root, &tree_expanded));
        assert(tree_expanded);
        assert(obs_monad_live_editor_filesystem_tree_command(
            "toggle", tree_root, &tree_expanded));
        assert(!tree_expanded);
        assert(obs_monad_live_editor_filesystem_tree_command(
            "contains", tree_root, &tree_expanded));
        assert(!tree_expanded);
        char child_dir[256], child_file[256];
        snprintf(child_dir, sizeof(child_dir), "%s/child", tree_root);
        snprintf(child_file, sizeof(child_file), "%s/child/item.txt", tree_root);
        assert(mkdir(child_dir, 0700) == 0);
        FILE *tree_file = fopen(child_file, "wb");
        assert(tree_file);
        assert(fputs("tree activation\n", tree_file) >= 0);
        assert(fclose(tree_file) == 0);
        assert(file_manager_navigate(tree_root));
        assert(editor.file_manager.item_count == 1);
        assert(editor.file_manager.items[0].type == FILE_ITEM_DIR);
        editor.panels[PANEL_RIGHT].open = true;
        editor.panels[PANEL_RIGHT].t = 1.0f;
        editor.panels[PANEL_RIGHT].target_t = 1.0f;
        editor.fs_collapsed = false;
        editor.inspector_fs_split = 0.5f;
        ObsWindow *primary_surface = obs_editor_frame_window(primary);
        draw_editor_frame(primary_surface);
        const float panel_cx =
            (float)context.drawExtent.width -
            editor.panels[PANEL_RIGHT].size + 14.0f;
        const float content_height =
            (float)context.drawExtent.height - 32.0f - 14.0f;
        const float first_row_y =
            14.0f + content_height * 0.5f - 32.0f - 14.0f -
            (editor.font->ascent - editor.font->descent + 11.0f) * 0.5f;
        click_editor_point(
            native, primary_surface, panel_cx + 2.0f,
            context.drawExtent.height - first_row_y);
        assert(editor.file_manager.item_count == 2);
        assert(editor.file_manager.items[0].expanded);
        const float child_row_y = first_row_y -
            (editor.font->ascent - editor.font->descent + 11.0f);
        click_editor_point(
            native, primary_surface, panel_cx + 64.0f,
            context.drawExtent.height - child_row_y);
        assert(editor.file_manager.selected_index == 1);
        int64_t monad_selected = -1;
        assert(obs_monad_live_editor_filesystem_tree_selection(
            &monad_selected, false));
        assert(monad_selected == 1);
        click_editor_point(
            native, primary_surface, panel_cx + 64.0f,
            context.drawExtent.height - child_row_y);
        assert(editor.panels[PANEL_BOTTOM].open);
        text_editor_close();
        assert(unlink(child_file) == 0);
        assert(rmdir(child_dir) == 0);
        assert(rmdir(tree_root) == 0);
        puts("Actual GLFW filesystem tree expansion, selection and double-click activation PASS");

        assert(file_manager_navigate("tests/native"));
        assert(editor.file_manager.item_count > 0);
        editor.fs_collapsed = true;
        editor.fs_anim_t = 1.0f;
        GLFWwindow *primary_native = obs_editor_frame_window(primary)->window;
        internal_key_callback(primary_native, GLFW_KEY_S, 0, GLFW_PRESS,
                              GLFW_MOD_CONTROL);
        assert(editor_text_input_is_active());
        assert(!editor.fs_collapsed);
        internal_key_callback(primary_native, GLFW_KEY_Z, 0, GLFW_PRESS, 0);
        internal_key_callback(primary_native, GLFW_KEY_Z, 0, GLFW_PRESS, 0);
        assert(editor_text_input_is_active());
        assert(editor.file_manager.item_count > 0);
        obs_frame_profile_enable(true);
        draw_editor_frame(primary_surface);
        ObsFrameProfile search_tree_profile = {0};
        obs_frame_profile_get(&search_tree_profile);
        obs_frame_profile_enable(false);
        assert(search_tree_profile.vertex2d_count > 0);
        internal_key_callback(primary_native, GLFW_KEY_G, 0, GLFW_PRESS,
                              GLFW_MOD_CONTROL);
        assert(!editor_text_input_is_active());
        puts("Actual GLFW C-s search expands/renders file tree, "
             "typing and C-g cancellation PASS");

        assert(monad_live_evaluator_run_quiet(
            obs_monad_engine()->live_evaluator,
            "(obsidian_window_smoke_open 0)"));
        ObsEditorFrame *monad_tool = obs_editor_frame_current();
        assert(monad_tool != primary);
        assert(obs_editor_frame_is_monad_window(monad_tool));
        assert(obs_editor_frame_is_2d_only(monad_tool));
        context.profileWorkCountersEnabled = true;
        obs_frame_profile_enable(true);
        draw_editor_frame(obs_editor_frame_window(monad_tool));
        ObsFrameProfile window_profile = {0};
        obs_frame_profile_get(&window_profile);
        assert(window_profile.vertex2d_count > 0);
        assert(live_window_smoke_integer(
                   "obsidian-window-smoke-draw-count") > 0);
        GLFWwindow *monad_native = obs_editor_frame_window(monad_tool)->window;
        internal_key_callback(monad_native, GLFW_KEY_A, 0, GLFW_PRESS, 0);
        assert(live_window_smoke_integer(
                   "obsidian-window-smoke-key-count") == 1);
        internal_mouse_button_callback(monad_native, GLFW_MOUSE_BUTTON_LEFT,
                                       GLFW_PRESS, 0);
        assert(live_window_smoke_integer(
                   "obsidian-window-smoke-mouse-count") == 1);
        internal_key_callback(obs_editor_frame_window(primary)->window,
                              GLFW_KEY_F13, 0, GLFW_PRESS, 0);
        assert(live_window_smoke_integer(
                   "obsidian-window-smoke-key-count") == 1);
        internal_key_callback(monad_native, GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);
        assert(obs_editor_frame_is_closing(monad_tool));
        obs_frame_profile_enable(false);
        for (unsigned i = 0; i < 20 && obs_editor_frame_next(primary); ++i) {
            glfwPollEvents();
            obs_editor_frames_begin_iteration();
            obs_editor_frames_end_iteration();
            usleep(20000);
        }
        assert(obs_editor_frame_next(primary) == NULL);
        puts("Monad window API draw, focused keyboard/mouse input and close PASS");

        assert(monad_live_evaluator_run_quiet(
            obs_monad_engine()->live_evaluator,
            "(obsidian_widget_smoke_open 0)"));
        ObsEditorFrame *widget_frame = obs_editor_frame_current();
        assert(widget_frame != primary &&
               obs_editor_frame_is_monad_window(widget_frame));
        draw_editor_frame(obs_editor_frame_window(widget_frame));
        GLFWwindow *widget_native =
            obs_editor_frame_window(widget_frame)->window;
        internal_key_callback(widget_native, GLFW_KEY_UP, 0, GLFW_PRESS, 0);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_key_count") == 1);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_last_key") == GLFW_KEY_UP);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_selected") == 0);
        internal_key_callback(widget_native, GLFW_KEY_UP, 0, GLFW_REPEAT, 0);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_key_count") == 1);
        internal_key_callback(widget_native, GLFW_KEY_DOWN, 0, GLFW_PRESS, 0);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_key_count") == 2);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_selected") == 1);
        internal_key_callback(widget_native, GLFW_KEY_DOWN, 0, GLFW_PRESS, 0);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_selected") == 1);
        internal_key_callback(widget_native, GLFW_KEY_ENTER, 0, GLFW_PRESS, 0);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_key_count") == 4);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_last_key") == GLFW_KEY_ENTER);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_candidate") == 42);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_activation_count") == 1);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_last_action") == 42);
        glfwSetCursorPos(widget_native, 20.0, 20.0);
        internal_cursor_position_callback(widget_native, 20.0, 20.0);
        internal_mouse_button_callback(widget_native, GLFW_MOUSE_BUTTON_LEFT,
                                       GLFW_PRESS, 0);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_mouse_count") == 1);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_mouse_hit") == -1);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_activation_count") == 1);
        glfwSetCursorPos(widget_native, 308.0, 56.0);
        internal_cursor_position_callback(widget_native, 308.0, 56.0);
        internal_mouse_button_callback(widget_native, GLFW_MOUSE_BUTTON_LEFT,
                                       GLFW_PRESS, 0);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_mouse_count") == 2);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_mouse_hit") == -1);
        glfwSetCursorPos(widget_native, 20.0, 56.0);
        internal_cursor_position_callback(widget_native, 20.0, 56.0);
        internal_mouse_button_callback(widget_native, GLFW_MOUSE_BUTTON_LEFT,
                                       GLFW_PRESS, 0);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_mouse_count") == 3);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_mouse_hit") == 0);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_activation_count") == 2);
        assert(live_window_smoke_integer(
                   "obsidian_widget_smoke_last_action") == 41);
        internal_key_callback(widget_native, GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);
        assert(obs_editor_frame_is_closing(widget_frame));
        for (unsigned i = 0; i < 20 && obs_editor_frame_next(primary); ++i) {
            glfwPollEvents();
            obs_editor_frames_begin_iteration();
            obs_editor_frames_end_iteration();
            usleep(20000);
        }
        assert(obs_editor_frame_next(primary) == NULL);
        puts("Monad widget vbox focus, keyboard activation and mouse hit-testing PASS");

        assert(obs_editor_frame_create_image("assets/textures/pengu.png", true));
        ObsEditorFrame *tool = obs_editor_frame_current();
        assert(tool != primary && obs_editor_frame_is_2d_only(tool));
        context.profileWorkCountersEnabled = true;
        texture_pool_wait_for_uploads(&context);
        for (unsigned i = 0; i < 12; ++i) {
            glfwPollEvents();
            obs_editor_frames_begin_iteration();
            obs_editor_frame_activate(tool);
            ObsWindow *window = obs_editor_frame_window(tool);
            obs_begin_frame(window);
            obs_editor_frame_render_surface(tool);
            obs_end_frame(window);
            assert(context.profileComputeDispatches == 0);
            assert(context.profileIndirectDraws == 0);
            obs_editor_frames_end_iteration();
            usleep(20000);
        }
        obs_editor_frame_activate(tool);
        KeyChordMap *map = keychord_get_frame_local_map();
        KeyChordBinding *close = keychord_find_binding(map, "C-g");
        assert(close && close->managed_monad);
        keychord_invoke_binding(close);
        assert(obs_editor_frame_is_closing(tool));
        for (unsigned i = 0; i < 20; ++i) {
            glfwPollEvents();
            obs_editor_frames_begin_iteration();
            obs_editor_frames_end_iteration();
            usleep(20000);
        }
        assert(obs_editor_frame_next(primary) == NULL);
        assert(keychord_get_frame_local_map() == NULL);
        obs_editor_frame_activate(primary);
        assert(load_gltf("tests/fixtures/async-static-hierarchy.gltf", scene));
        assert(scene->gltf_instance_count > 0);
        GLTFInstance *instance =
            &scene->gltf_instances[scene->gltf_instance_count - 1];
        assert(instance->mesh_count >= 3);
        OmdlSceneGraph *graph = instance->gltf_data;
        int32_t group_idx = -1;
        int32_t instance_nodes[SCENE_TREE_MAX_NODES];
        int instance_node_count = 0;
        int32_t unrelated_node = -1;
        for (int32_t i = 0; i < scene->tree.count; ++i) {
            SceneNode *node = &scene->tree.nodes[i];
            if (node->mesh_index == 0)
                unrelated_node = i;
            if (node->mesh_index >= (int32_t)instance->mesh_start_index &&
                node->mesh_index <
                    (int32_t)(instance->mesh_start_index +
                              instance->mesh_count))
                instance_nodes[instance_node_count++] = i;
            for (int32_t c = node->first_child; c >= 0;
                 c = scene->tree.nodes[c].next_sibling) {
                if (scene->tree.nodes[c].mesh_index ==
                    (int32_t)instance->mesh_start_index)
                    group_idx = i;
            }
        }
        assert(unrelated_node >= 0 && group_idx >= 0);
        assert(instance_node_count == (int)instance->mesh_count);
        scene->tree.nodes[unrelated_node].selected = true;
        editor_hierarchy_add_range(&scene->tree, instance_nodes,
                                   instance_node_count, 0, 1);
        assert(scene->tree.nodes[unrelated_node].selected);
        assert(scene->tree.nodes[instance_nodes[0]].selected);
        assert(scene->tree.nodes[instance_nodes[1]].selected);
        assert(!scene->tree.nodes[instance_nodes[2]].selected);
        for (int i = 0; i < instance_node_count; ++i)
            scene->tree.nodes[instance_nodes[i]].selected = true;
        editor_hierarchy_promote_complete_gltfs(scene);
        assert(scene->tree.nodes[group_idx].selected &&
               scene->tree.nodes[group_idx].selected_group);
        for (int i = 0; i < instance_node_count; ++i)
            assert(!scene->tree.nodes[instance_nodes[i]].selected);

        const int child_mesh = (int)instance->mesh_start_index + 2;
        Mesh *child = &scene->meshes.items[child_mesh];
        const int32_t child_node = (int32_t)(uintptr_t)child->node;
        assert(child_node >= 0 && child_node < (int32_t)graph->node_count);
        mat4 original_node_local, original_instance_transform;
        glm_mat4_copy(graph->nodes[child_node].base_matrix,
                      original_node_local);
        glm_mat4_copy(instance->instance_transform,
                      original_instance_transform);
        inspector_select_mesh(child_mesh);
        editor_mouse_move(320.0, 240.0);
        assert(editor_transform_begin(EDITOR_TRANSFORM_ROTATE));
        editor_mouse_move(350.0, 240.0);
        editor_update();
        assert(memcmp(graph->nodes[child_node].base_matrix,
                      original_node_local, sizeof(mat4)) != 0);
        assert(memcmp(instance->instance_transform,
                      original_instance_transform, sizeof(mat4)) == 0);
        assert(editor_transform_finish(false));
        assert(memcmp(graph->nodes[child_node].base_matrix,
                      original_node_local, sizeof(mat4)) == 0);

        for (int32_t i = 0; i < scene->tree.count; ++i)
            scene->tree.nodes[i].selected = false;
        scene->tree.nodes[group_idx].selected = true;
        scene->tree.nodes[group_idx].selected_group = true;
        inspector_select_mesh_internal((int)instance->mesh_start_index);
        editor_mouse_move(320.0, 240.0);
        assert(editor_transform_begin(EDITOR_TRANSFORM_ROTATE));
        editor_mouse_move(350.0, 240.0);
        editor_update();
        assert(memcmp(instance->instance_transform,
                      original_instance_transform, sizeof(mat4)) != 0);
        assert(editor_transform_finish(false));
        assert(memcmp(instance->instance_transform,
                      original_instance_transform, sizeof(mat4)) == 0);
        puts("GLFW trackball halves, additive hierarchy range/promotion and glTF child/group transforms PASS");
        obs_editor_frames_shutdown();
        obs_scene_buffers_cleanup();
        obs_monad_engine_stop();
        add_menu_cleanup();
        vertico_cleanup();
        editor_cleanup();
        keymap_free(&keymap);
        cleanup(&context);
        puts("Actual image window, 2D-only submissions, Monad C-g and owned keymap teardown PASS");
        return 0;
    }

    Mesh mesh = {.name = "Red cube", .materialIndex = -1,
                 .textureIndex = -1, .normalMapIndex = -1,
                 .metallicRoughIndex = -1, .aoIndex = -1,
                 .emissiveIndex = -1, .roughnessFactor = 0.5f,
                 .baseColorFactor = {1, 0, 0, 1}};
    glm_mat4_identity(mesh.model);
    glm_mat4_identity(mesh.local_transform);
    meshes_add(&scene->meshes, mesh);
    meshes_add(&scene->meshes, mesh);
    assert(scene->meshes.count == 2);
    KeyChordBinding *duplicate = keychord_find_binding(&keymap, "M-d");
    assert(duplicate && duplicate->managed_monad);

    const int32_t sdf_index = obs_sdf_add_checked(
        scene, "Blue SDF", SDF_TYPE_SPHERE, (vec3){0, 0, 0},
        (vec4){1, 0, 0, 0}, (vec4){0.05f, 0.15f, 0.95f, 1.0f});
    assert(sdf_index == 0);
    obs_material_browser_begin();
    preview = (ObsMaterialBrowserEntry *)obs_material_browser_add_mesh(0);
    assert(preview);
    assert(!obs_material_browser_add_mesh(1));
    assert(!obs_material_browser_add_mesh(2));
    const ObsMaterialBrowserEntry *sdf_preview =
        obs_material_browser_add_sdf((size_t)sdf_index);
    assert(sdf_preview);
    assert(strcmp(obs_material_browser_kind(sdf_preview), "sdf") == 0);
    assert(strstr(obs_material_browser_name(sdf_preview), "Blue SDF"));
    assert(!obs_material_browser_add_sdf((size_t)sdf_index));

    KeyChordBinding *binding = keychord_find_binding(&keymap, "C-h C-f");
    assert(binding && binding->managed_monad);
    keychord_invoke_binding(binding);
    assert(vertico.is_active && vertico.material_preview_mode);
    assert(vertico.candidate_count == 2);
    assert(strstr(vertico.candidates[0].annotation, "#FF0000"));
    assert(strstr(vertico.candidates[1].annotation, "#0D26F2"));
    assert(vertico.scroll_margin == 4);
    preview = vertico.candidates[0].data;
    sdf_preview = vertico.candidates[1].data;
    vertico_quit();
    g_selected_sdf_index = -1;
    obs_material_browser_select(sdf_preview);
    assert(g_selected_sdf_index == sdf_index);
    g_selected_sdf_index = -1;

    vertico_clear_candidates();
    for (int i = 0; i < 24; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "candidate-%02d", i);
        vertico_add_candidate(name, NULL, NULL);
    }
    vertico_activate("scroll smoke", NULL);
    assert(vertico.count > 5);
    KeyChordBinding *next = keychord_find_binding(
        &vertico.vertico_keymap, "C-n");
    assert(next);
    for (int i = 0; i < 6; ++i) keychord_invoke_binding(next);
    assert(vertico_selection_index() == 6);
    assert(vertico_scroll_offset() == 1);

    const float header_x = (float)context.drawExtent.width * 0.5f;
    const float row_height = editor.font->ascent - editor.font->descent + 5.0f;
    const int original_count = vertico.count;
    assert(original_count > 1);
    vertico.count = original_count - 1;
    editor.panels[PANEL_TOP].size =
        40.0f + row_height * (float)vertico.count;
    const float header_y = editor.panels[PANEL_TOP].size - 16.0f;
    assert(vertico_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS,
                                header_x, header_y));
    assert(vertico_mouse_move(header_x, header_y + row_height - 1.0f));
    assert(vertico.count == original_count - 1);
    assert(vertico_mouse_move(header_x, header_y + row_height + 1.0f));
    assert(vertico.count == original_count);
    assert(vertico_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE,
                                header_x, header_y + row_height + 1.0f));

    vertico.resize_pixelwise = true;
    vertico.count = original_count - 1;
    editor.panels[PANEL_TOP].size =
        40.0f + row_height * (float)vertico.count;
    const float pixel_height = editor.panels[PANEL_TOP].size;
    const float pixel_header_y = editor.panels[PANEL_TOP].size - 16.0f;
    assert(vertico_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS,
                                header_x, pixel_header_y));
    assert(vertico_mouse_move(header_x, pixel_header_y + 3.5f));
    assert(editor.panels[PANEL_TOP].size > pixel_height + 2.0f);
    assert(vertico_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE,
                                header_x, pixel_header_y + 3.5f));
    assert(obs_headless_render_raw("build/tests/vertico-bottom-row.rgba",
                                   draw_editor_surface, NULL));
    assert(vertico.row_bottom >= 7.0f);
    unsigned char *vertico_pixels =
        read_pixels("build/tests/vertico-bottom-row.rgba");
    size_t bottom_row_ink = 0;
    const size_t bottom_y =
        (size_t)((float)context.drawExtent.height -
                 (vertico.row_bottom + vertico.row_height * 0.5f));
    const size_t y0 = bottom_y > 4 ? bottom_y - 4 : 0;
    const size_t y1 = bottom_y + 4 < 256 ? bottom_y + 4 : 255;
    for (size_t y = y0; y <= y1; ++y)
        for (size_t x = 12; x < 244; ++x) {
            const unsigned char *pixel = &vertico_pixels[4 * (y * 256 + x)];
            if (pixel[0] > 80 || pixel[1] > 80 || pixel[2] > 80)
                ++bottom_row_ink;
        }
    assert(bottom_row_ink > 5);
    free(vertico_pixels);
    vertico_quit();

    context.primary->render2DOnly = true;
    context.profileWorkCountersEnabled = true;
    assert(obs_headless_render_raw("build/tests/material-before.rgba",
                                   draw_preview, NULL));
    unsigned char *before = read_pixels("build/tests/material-before.rgba");
    obs_material_browser_drag(preview, 50, 20);
    assert(obs_headless_render_raw("build/tests/material-after.rgba",
                                   draw_preview, NULL));
    unsigned char *after = read_pixels("build/tests/material-after.rgba");
    assert(memcmp(before, after, 256 * 256 * 4) != 0);
    size_t red_pixels = 0;
    size_t min_red_x = 256, max_red_x = 0;
    size_t min_red_y = 256, max_red_y = 0;
    for (size_t i = 0; i < 256 * 256; ++i) {
        if (after[4*i] > after[4*i+1] + 30 &&
            after[4*i] > after[4*i+2] + 30) {
            ++red_pixels;
            const size_t x = i % 256, y = i / 256;
            if (x < min_red_x) min_red_x = x;
            if (x > max_red_x) max_red_x = x;
            if (y < min_red_y) min_red_y = y;
            if (y > max_red_y) max_red_y = y;
        }
    }
    assert(red_pixels > 1000);
    assert(max_red_x - min_red_x < 130 && max_red_y - min_red_y < 130);
    assert(max_red_x - min_red_x > max_red_y - min_red_y + 5);
    assert(after[4 * (128 * 256 + 140)] >
           after[4 * (128 * 256 + 140) + 1] + 30);
    assert(!(after[4 * (176 * 256 + 176)] >
             after[4 * (176 * 256 + 176) + 1] + 30));
    assert(context.profileComputeDispatches == 0);
    assert(context.profileIndirectDraws == 0);
    assert(context.profileMeshTaskWorkgroups == 0);
    free(before);
    free(after);

    assert(obs_image_editor_open(&image, "assets/textures/pengu.png"));
    texture_pool_wait_for_uploads(&context);
    assert(obs_headless_render_raw("build/tests/image-fit.rgba",
                                   draw_image, NULL));
    before = read_pixels("build/tests/image-fit.rgba");
    obs_image_editor_zoom(&image, 2);
    assert(obs_headless_render_raw("build/tests/image-zoom.rgba",
                                   draw_image, NULL));
    after = read_pixels("build/tests/image-zoom.rgba");
    assert(memcmp(before, after, 256 * 256 * 4) != 0);
    assert(context.profileComputeDispatches == 0);
    assert(context.profileIndirectDraws == 0);
    assert(context.profileMeshTaskWorkgroups == 0);
    free(before);
    free(after);
    obs_image_editor_close(&image);
    scene->meshes.count = 0;
    if (!windows) {
        context.primary->render2DOnly = false;
        camera_init(&camera, (vec3){0.0f, 0.0f, 5.0f}, -90.0f, 0.0f,
                    (float)context.drawExtent.width /
                        (float)context.drawExtent.height);
        cube((vec3){0, 0, 0}, 2, RED);
        const int mesh_index = (int)scene->meshes.count - 1;
        inspector_select_mesh(mesh_index);
        int32_t hierarchy_anchor = -1;
        assert(obs_monad_live_editor_hierarchy_anchor(
            &hierarchy_anchor, false));
        assert(hierarchy_anchor >= 0 &&
               hierarchy_anchor < scene->tree.count);
        assert(scene->tree.nodes[hierarchy_anchor].mesh_index == mesh_index);
        assert(scene->tree.nodes[hierarchy_anchor].selected);
        assert(obs_headless_render_frame(draw_headless_editor_frame, NULL));
        const double center_x = context.drawExtent.width * 0.5;
        const double center_y = context.drawExtent.height * 0.5;
        assert(gizmo_pick_mesh(center_x, center_y) == mesh_index);

        const char *jpg_drop[] = {"assets/textures/puta.jpg"};
        internal_drop_paths_at(center_x, center_y, 1, jpg_drop);
        assert(scene->meshes.items[mesh_index].textureIndex >= 0);
        assert(scene->meshes.items[mesh_index].texture ==
               texture_pool_get(scene->meshes.items[mesh_index].textureIndex));
        const int32_t jpg_texture_index =
            scene->meshes.items[mesh_index].textureIndex;
        const char *png_drop[] = {
            "assets/textures/prototype/Red/texture_01.png"};
        internal_drop_paths_at(center_x, center_y, 1, png_drop);
        assert(scene->meshes.items[mesh_index].textureIndex >= 0);
        assert(scene->meshes.items[mesh_index].textureIndex !=
               jpg_texture_index);
        assert(scene->meshes.items[mesh_index].texture ==
               texture_pool_get(scene->meshes.items[mesh_index].textureIndex));
        Mesh *missing_material = &scene->meshes.items[mesh_index];
        missing_material->materialIndex = -1;
        missing_material->textureIndex = -1;
        missing_material->normalMapIndex = -1;
        missing_material->metallicRoughIndex = -1;
        missing_material->aoIndex = -1;
        missing_material->emissiveIndex = -1;
        missing_material->transmissionIndex = -1;
        missing_material->thicknessIndex = -1;
        missing_material->wireframe = true;
        markMeshesSSBODirty(&context);
        assert(obs_headless_render_frame(draw_headless_editor_frame, NULL));
        assert(gizmo_pick_mesh(center_x, center_y) == mesh_index);

        const char *glb_drop[] = {"assets/gltf/Box.glb"};
        size_t before_drop = scene->meshes.count;
        internal_drop_paths_at(-1.0, -1.0, 1, glb_drop);
        assert(wait_for_headless_scene_mesh_growth(before_drop));
        const char *gltf_drop[] = {"tests/fixtures/async-meshlet-growth.gltf"};
        before_drop = scene->meshes.count;
        internal_drop_paths_at(-1.0, -1.0, 1, gltf_drop);
        assert(wait_for_headless_scene_mesh_growth(before_drop));
        const char *obj_drop[] = {"tests/fixtures/async_triangle.obj"};
        before_drop = scene->meshes.count;
        internal_drop_paths_at(-1.0, -1.0, 1, obj_drop);
        assert(wait_for_headless_scene_mesh_growth(before_drop));
        assert(load_gltf("tests/fixtures/async-static-hierarchy.gltf", scene));
        GLTFInstance *group_instance =
            &scene->gltf_instances[scene->gltf_instance_count - 1];
        assert(group_instance->mesh_count >= 3);
        const int32_t group_first =
            (int32_t)group_instance->mesh_start_index;
        const int32_t group_end =
            group_first + (int32_t)group_instance->mesh_count;
        int32_t group_idx = -1;
        int32_t instance_nodes[SCENE_TREE_MAX_NODES];
        int instance_node_count = 0;
        for (int32_t i = 0; i < scene->tree.count; ++i) {
            SceneNode *node = &scene->tree.nodes[i];
            if (node->mesh_index >= group_first && node->mesh_index < group_end)
                instance_nodes[instance_node_count++] = i;
            for (int32_t child = node->first_child; child >= 0;
                 child = scene->tree.nodes[child].next_sibling) {
                if (scene->tree.nodes[child].mesh_index == group_first)
                    group_idx = i;
            }
        }
        assert(group_idx >= 0);
        assert(instance_node_count == (int)group_instance->mesh_count);
        for (int i = 0; i < instance_node_count; ++i)
            scene->tree.nodes[instance_nodes[i]].selected = false;
        scene->tree.nodes[instance_nodes[0]].selected = true;
        scene->tree.nodes[instance_nodes[1]].selected = true;
        editor_hierarchy_promote_complete_gltfs(scene);
        assert(!scene->tree.nodes[group_idx].selected_group);
        assert(scene->tree.nodes[instance_nodes[0]].selected);
        assert(scene->tree.nodes[instance_nodes[1]].selected);
        for (int i = 0; i < instance_node_count; ++i)
            scene->tree.nodes[instance_nodes[i]].selected = true;
        editor_hierarchy_promote_complete_gltfs(scene);
        assert(scene->tree.nodes[group_idx].selected);
        assert(scene->tree.nodes[group_idx].selected_group);
        for (int i = 0; i < instance_node_count; ++i)
            assert(!scene->tree.nodes[instance_nodes[i]].selected);
        scene->tree.nodes[group_idx].selected = true;
        scene->tree.nodes[group_idx].selected_group = true;
        scene->tree.nodes[instance_nodes[0]].selected = true;
        editor_hierarchy_promote_complete_gltfs(scene);
        assert(scene->tree.nodes[group_idx].selected);
        assert(scene->tree.nodes[group_idx].selected_group);
        for (int i = 0; i < instance_node_count; ++i)
            assert(!scene->tree.nodes[instance_nodes[i]].selected);
        puts("Headless glTF complete-group promotion and group-selection demotion PASS");

        const size_t source_group_mesh_count = group_instance->mesh_count;
        mat4 source_instance_transform, source_first_mesh_transform;
        glm_mat4_copy(group_instance->instance_transform,
                      source_instance_transform);
        glm_mat4_copy(scene->meshes.items[group_first].model,
                      source_first_mesh_transform);
        const size_t gltf_count_before_copy = scene->gltf_instance_count;
        const size_t meshes_before_copy = scene->meshes.count;
        editor.inspector.selected_mesh_index = group_first;
        scene->tree.nodes[group_idx].selected = true;
        scene->tree.nodes[group_idx].selected_group = true;
        keychord_invoke_binding(duplicate);
        assert(wait_for_headless_scene_mesh_growth(meshes_before_copy));
        assert(scene->gltf_instance_count == gltf_count_before_copy + 1);
        GLTFInstance *group_copy =
            &scene->gltf_instances[scene->gltf_instance_count - 1];
        assert(group_copy->mesh_count == source_group_mesh_count);
        assert(group_copy->source_path &&
               strcmp(group_copy->source_path,
                      "tests/fixtures/async-static-hierarchy.gltf") == 0);
        assert(memcmp(group_copy->instance_transform,
                      source_instance_transform,
                      sizeof(source_instance_transform)) == 0);
        assert(memcmp(scene->meshes.items[group_copy->mesh_start_index].model,
                      source_first_mesh_transform,
                      sizeof(source_first_mesh_transform)) == 0);
        assert(editor_transform_active());
        assert(editor_transform_finish(true));
        puts("Headless M-d duplicates and places complete glTF instances PASS");

        const size_t one_mesh_source_index = scene->gltf_instance_count - 2;
        const size_t one_mesh_instance_count = scene->gltf_instance_count;
        const size_t one_mesh_scene_count = scene->meshes.count;
        mat4 selected_mesh_transform;
        glm_mat4_copy(scene->meshes.items[group_first].model,
                      selected_mesh_transform);
        inspector_select_mesh(group_first);
        assert(!scene->tree.nodes[group_idx].selected_group);
        keychord_invoke_binding(duplicate);
        assert(wait_for_headless_scene_mesh_growth(one_mesh_scene_count));
        assert(scene->gltf_instance_count == one_mesh_instance_count + 1);
        GLTFInstance *one_mesh_copy =
            &scene->gltf_instances[scene->gltf_instance_count - 1];
        assert(one_mesh_copy->mesh_count == 1);
        assert(scene->meshes.count == one_mesh_scene_count + 1);
        assert(memcmp(scene->meshes.items[one_mesh_copy->mesh_start_index].model,
                      selected_mesh_transform,
                      sizeof(selected_mesh_transform)) == 0);
        assert(scene->gltf_instances[one_mesh_source_index].mesh_count ==
               source_group_mesh_count);
        assert(editor_transform_active());
        assert(editor_transform_finish(true));
        puts("Headless M-d duplicates only a selected glTF mesh PASS");

        inspector_deselect();
        hierarchy_anchor = 0;
        assert(obs_monad_live_editor_hierarchy_anchor(
            &hierarchy_anchor, false));
        assert(hierarchy_anchor == -1);
        for (int32_t i = 1; i < scene->tree.count; ++i)
            assert(!scene->tree.nodes[i].selected);
        puts("Headless JPG/PNG texture drops and GLB/glTF/OBJ scene drops PASS");
    }

    inspector_select_mesh(0);
    editor_mouse_move(128.0, 128.0);
    const size_t standalone_base_count = scene->meshes.count;
    keychord_invoke_binding(duplicate);
    assert(scene->meshes.count == standalone_base_count + 1 &&
           editor_transform_active());
    assert(keychord_process_key_with_stack(GLFW_KEY_3, GLFW_PRESS, 0));
    assert(keychord_process_key_with_stack(GLFW_KEY_X, GLFW_PRESS, 0));
    editor_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
    assert(!editor_transform_active());
    assert(fabsf(scene->meshes.items[standalone_base_count].model[3][0] -
                 3.0f) < 0.001f);

    keychord_invoke_binding(duplicate);
    assert(scene->meshes.count == standalone_base_count + 2 &&
           editor_transform_active());
    assert(keychord_process_key_with_stack(GLFW_KEY_MINUS, GLFW_PRESS, 0));
    assert(keychord_process_key_with_stack(GLFW_KEY_3, GLFW_PRESS, 0));
    assert(keychord_process_key_with_stack(GLFW_KEY_X, GLFW_PRESS, 0));
    editor_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
    assert(!editor_transform_active());
    assert(fabsf(scene->meshes.items[standalone_base_count + 1]
                     .model[3][0]) < 0.001f);

    keychord_invoke_binding(duplicate);
    assert(scene->meshes.count == standalone_base_count + 3 &&
           editor_transform_active());
    assert(keychord_process_key_with_stack(GLFW_KEY_4, GLFW_PRESS, 0));
    assert(keychord_process_key_with_stack(GLFW_KEY_Y, GLFW_PRESS, 0));
    assert(keychord_process_key_with_stack(GLFW_KEY_ENTER, GLFW_PRESS, 0));
    assert(fabsf(scene->meshes.items[standalone_base_count + 2]
                     .model[3][1] - 4.0f) < 0.001f);

    keychord_invoke_binding(duplicate);
    assert(scene->meshes.count == standalone_base_count + 4 &&
           editor_transform_active());
    assert(keychord_process_key_with_stack(GLFW_KEY_MINUS, GLFW_PRESS, 0));
    assert(keychord_process_key_with_stack(GLFW_KEY_2, GLFW_PRESS, 0));
    assert(keychord_process_key_with_stack(GLFW_KEY_Z, GLFW_PRESS, 0));
    assert(keychord_process_key_with_stack(GLFW_KEY_ENTER, GLFW_PRESS, 0));
    assert(fabsf(scene->meshes.items[standalone_base_count + 3]
                     .model[3][2] + 2.0f) < 0.001f);

    keychord_invoke_binding(duplicate);
    assert(scene->meshes.count == standalone_base_count + 5 &&
           editor_transform_active());
    assert(keychord_process_key_with_stack(GLFW_KEY_3, GLFW_PRESS, 0));
    assert(keychord_process_key_with_stack(GLFW_KEY_X, GLFW_PRESS, 0));
    vec3 original_camera_position;
    mat4 original_projection;
    glm_vec3_copy(camera.position, original_camera_position);
    glm_mat4_copy(camera.projection_matrix, original_projection);
    camera.position[0] = 0.0f;
    camera.position[1] = 0.0f;
    camera.position[2] = 10.0f;
    glm_mat4_identity(camera.projection_matrix);
    const float pixels_per_meter =
        (2.0f * glm_vec3_distance(
                    camera.position,
                    scene->meshes.items[standalone_base_count + 4].model[3])) /
        ((float)context.drawExtent.height *
         camera.projection_matrix[1][1]);
    assert(pixels_per_meter > 0.0f);
    const double drag_x = 128.0 + ceil(2.0f / pixels_per_meter);
    editor_mouse_move(drag_x, 128.0);
    editor_update();
    const float snapped_delta =
        scene->meshes.items[standalone_base_count + 4].model[3][0] -
        scene->meshes.items[standalone_base_count + 3].model[3][0];
    assert(fabsf(snapped_delta) >= 3.0f);
    assert(fabsf(snapped_delta / 3.0f -
                 roundf(snapped_delta / 3.0f)) < 0.001f);
    editor_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
    assert(!editor_transform_active());
    glm_vec3_copy(original_camera_position, camera.position);
    glm_mat4_copy(original_projection, camera.projection_matrix);

    keychord_invoke_binding(duplicate);
    assert(scene->meshes.count == standalone_base_count + 6 &&
           editor_transform_active());
    assert(keychord_process_key_with_stack(
        GLFW_KEY_G, GLFW_PRESS, GLFW_MOD_CONTROL));
    assert(!editor_transform_active() &&
           scene->meshes.count == standalone_base_count + 5);
    assert(editor.inspector.selected_mesh_index ==
           (int)standalone_base_count + 4);
    keychord_invoke_binding(duplicate);
    assert(scene->meshes.count == standalone_base_count + 6 &&
           editor_transform_active());
    assert(keychord_process_key_with_stack(GLFW_KEY_ESCAPE, GLFW_PRESS, 0));
    assert(!editor_transform_active() &&
           scene->meshes.count == standalone_base_count + 5);
    assert(editor.inspector.selected_mesh_index ==
           (int)standalone_base_count + 4);
    puts("Monad M-d standalone clone, signed XYZ, cursor-step, click commit and cancellation PASS");

    Scene *primary_scene = scene;
    ObsSceneBuffer *second =
        obs_scene_buffer_open("tests/fixtures/async-triangle.gltf");
    assert(second && obs_scene_buffer_scene(second) != primary_scene);
    binding = keychord_find_binding(&keymap, "C-x b");
    assert(binding && binding->managed_monad);
    keychord_invoke_binding(binding);
    assert(vertico.is_active);
    bool selected = false;
    for (size_t i = 0; i < vertico.filtered_count; ++i) {
        if (vertico.filtered[i].data == second) {
            assert(vertico_set_position(i, 0));
            vertico_select();
            selected = true;
            break;
        }
    }
    assert(selected && scene == obs_scene_buffer_scene(second));
    assert(obs_editor_frame_set_buffer(obs_editor_frame_current(),
                                       obs_scene_buffer_first()));
    assert(scene == primary_scene);
    obs_editor_frames_shutdown();
    obs_scene_buffers_cleanup();
    obs_monad_engine_stop();
    add_menu_cleanup();
    vertico_cleanup();
    editor_cleanup();
    keymap_free(&keymap);
    cleanup(&context);
    puts("Material discovery, Monad command, drag pixels, image zoom, 2D-only GPU work and C-x b scene switching PASS");
    return 0;
}
