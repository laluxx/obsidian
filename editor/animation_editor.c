#include "animation_editor.h"
#include "theme.h"


///  Globals

AnimEditorState g_anim_editor = {0};

static KeyChordMap s_ae_keymap;

// Lazy-loaded icon handles
static int32_t s_icon_play    = -1;
static int32_t s_icon_pause   = -1;
static int32_t s_icon_stop    = -1;
static int32_t s_icon_key_bez = -1;   // KeyBezierSelected.svg  (unselected keyframe)
static int32_t s_icon_key_sel = -1;   // KeySelected.svg        (selected keyframe)
static int32_t s_icon_close   = -1;

extern KeyChordMap keymap;
extern void set_active_text_input(void (*cb)(char));


///  Helpers

static void ae_load_icons(void) {
    if (s_icon_play >= 0) return;
    extern VulkanContext context;
    s_icon_play    = texture_pool_add_svg(&context, "./assets/icons/Play.svg",                16, 16);
    s_icon_pause   = texture_pool_add_svg(&context, "./assets/icons/Pause.svg",               16, 16);
    s_icon_stop    = texture_pool_add_svg(&context, "./assets/icons/Stop.svg",                16, 16);
    s_icon_key_bez = texture_pool_add_svg(&context, "./assets/icons/KeyBezierSelected.svg",   16, 16);
    s_icon_key_sel = texture_pool_add_svg(&context, "./assets/icons/KeySelected.svg",         16, 16);
    s_icon_close   = texture_pool_add_svg(&context, "./assets/icons/Close.svg",               16, 16);
}

// Map a time value to an X pixel coordinate within the timeline content area.
// cx      = left edge of timeline (after the header column)
// cw      = width of the timeline area
// t       = time in seconds to convert
static float ae_time_to_x(float cx, float cw, float t) {
    AnimEditorState* ae = &g_anim_editor;
    float range = ae->view_end - ae->view_start;
    if (range <= 0.0f) return cx;
    return cx + ((t - ae->view_start) / range) * cw;
}

// Inverse: screen X → time in seconds.
static float ae_x_to_time(float cx, float cw, float x) {
    AnimEditorState* ae = &g_anim_editor;
    float range = ae->view_end - ae->view_start;
    if (cw <= 0.0f) return ae->view_start;
    return ae->view_start + ((x - cx) / cw) * range;
}

// Smooth scroll helpers — same 333ms cubic-out used everywhere else in the editor.
static void ae_smooth_scroll_x(float target) {
    AnimEditorState* ae = &g_anim_editor;
    ae->scroll_x_start  = ae->scroll_x;
    ae->scroll_x_target = target;
    ae->scroll_x_t      = 0.0f;
}

static void ae_smooth_scroll_y(float target) {
    AnimEditorState* ae = &g_anim_editor;
    ae->scroll_y_start  = ae->scroll_y;
    ae->scroll_y_target = target;
    ae->scroll_y_t      = 0.0f;
}

// Clamp view_start / view_end so we never scroll past the animation bounds.
static void ae_clamp_view(void) {
    AnimEditorState* ae = &g_anim_editor;
    float dur = ae->total_duration > 0.0f ? ae->total_duration : 1.0f;
    float range = ae->view_end - ae->view_start;
    if (range < 0.05f) range = 0.05f;
    if (ae->view_start < 0.0f) {
        ae->view_start = 0.0f;
        ae->view_end   = range;
    }
    if (ae->view_end > dur * 1.05f) {   // 5% overshoot for breathing room
        ae->view_end   = dur * 1.05f;
        ae->view_start = ae->view_end - range;
        if (ae->view_start < 0.0f) ae->view_start = 0.0f;
    }
}


///  Lifecycle

void anim_editor_init(void) {
    AnimEditorState* ae = &g_anim_editor;
    memset(ae, 0, sizeof(*ae));

    ae->visible        = false;
    ae->playing        = false;
    ae->time           = 0.0f;
    ae->selected_track = -1;
    ae->view_start     = 0.0f;
    ae->view_end       = 5.0f;
    ae->scroll_x_t     = 1.0f;
    ae->scroll_y_t     = 1.0f;

    keymap_init(&s_ae_keymap);

    // Playback
    keychord_bind(&s_ae_keymap, "SPC",   anim_editor_play_pause,    "Play / Pause",      PRESS);
    keychord_bind(&s_ae_keymap, "C-SPC", anim_editor_stop,          "Stop & rewind",     PRESS);

    // Navigation
    keychord_bind(&s_ae_keymap, "C-f",    anim_editor_next_keyframe, "Next keyframe",     PRESS | REPEAT);
    keychord_bind(&s_ae_keymap, "C-b",    anim_editor_prev_keyframe, "Prev keyframe",     PRESS | REPEAT);
    keychord_bind(&s_ae_keymap, "C-a",    anim_editor_goto_start,    "Go to start",       PRESS);
    keychord_bind(&s_ae_keymap, "C-e",    anim_editor_goto_end,      "Go to end",         PRESS);
    keychord_bind(&s_ae_keymap, "<right>",anim_editor_next_keyframe, "Next keyframe",     PRESS | REPEAT);
    keychord_bind(&s_ae_keymap, "<left>", anim_editor_prev_keyframe, "Prev keyframe",     PRESS | REPEAT);

    // Close
    keychord_bind(&s_ae_keymap, "C-g",   anim_editor_close,         "Close editor",      PRESS);
}

void anim_editor_goto_start(void) {
    if (g_anim_editor.visible) g_anim_editor.time = 0.0f;
}

void anim_editor_goto_end(void) {
    if (g_anim_editor.visible) g_anim_editor.time = g_anim_editor.total_duration;
}

void anim_editor_cleanup(void) {
    // Nothing heap-allocated yet; keyframe pool is inline.
    memset(&g_anim_editor, 0, sizeof(g_anim_editor));
}

bool anim_editor_open(int mesh_index) {
    if (mesh_index < 0 || mesh_index >= (int)scene.meshes.count) return false;

    ae_load_icons();

    AnimEditorState* ae = &g_anim_editor;

    // ── Find the GLTFInstance that owns this mesh ─────────────────────────
    GLTFInstance* inst = NULL;
    for (size_t i = 0; i < scene.gltf_instance_count; i++) {
        if (mesh_index >= (int)scene.gltf_instances[i].mesh_start_index &&
            mesh_index  <  (int)(scene.gltf_instances[i].mesh_start_index +
                                  scene.gltf_instances[i].mesh_count)) {
            inst = &scene.gltf_instances[i];
            break;
        }
    }

    if (!inst || !inst->gltf_data) return false;

    OmdlSceneGraph* osg = (OmdlSceneGraph*)inst->gltf_data;
    if (osg->anim_count == 0) return false;

    // ── Populate tracks from OmdlAnimation ───────────────────────────────
    ae->track_count   = 0;
    ae->kf_pool_used  = 0;
    ae->total_duration = 0.0f;

    // Load the first animation sequence as the active one
    OmdlAnimation* anim = &osg->anims[0];
    ae->total_duration = anim->duration;

    // Create one track per animation channel (e.g. Bone Translation)
    for (uint32_t c = 0; c < anim->channel_count && ae->track_count < 64; c++) {
        OmdlChannel* ch = &osg->channels[anim->channel_offset + c];
        AnimTrack* track = &ae->tracks[ae->track_count];

        const char* node_name = "Root";
        if (ch->target_node != UINT32_MAX && ch->target_node < osg->node_count) {
            node_name = osg->nodes[ch->target_node].name;
        }

        const char* path_type = "Unk";
        if (ch->path_type == 1) path_type = "Pos"; // translation
        else if (ch->path_type == 2) path_type = "Rot"; // rotation
        else if (ch->path_type == 3) path_type = "Scl"; // scale
        else if (ch->path_type == 4) path_type = "Wgt"; // weights

        snprintf(track->name, sizeof(track->name), "%s : %s", node_name, path_type);
        track->duration  = anim->duration;
        track->keyframes = ae->kf_pool + ae->kf_pool_used;
        track->kf_count  = 0;

        for (uint32_t k = 0; k < ch->keyframe_count && ae->kf_pool_used < ANIM_EDITOR_MAX_KEYFRAMES; k++) {
            float t = osg->anim_floats[ch->times_offset + k];
            ae->kf_pool[ae->kf_pool_used++] = (AnimKeyframe){ .time = t, .selected = false };
            track->kf_count++;
        }

        ae->track_count++;
    }

    if (ae->track_count == 0) return false;

    // ── Reset view ────────────────────────────────────────────────────────
    // Do NOT reset playing state or time, so reopening continues seamlessly!
    if (ae->time > ae->total_duration) ae->time = 0.0f;
    ae->selected_track = 0;
    ae->view_start     = 0.0f;
    ae->view_end       = ae->total_duration > 0.0f ? ae->total_duration : 5.0f;
    ae->scroll_y       = 0.0f;
    ae->scroll_y_target= 0.0f;
    ae->scroll_y_t     = 1.0f;

    ae->visible = true;

    // ── Configure panel ───────────────────────────────────────────────────
    Panel* p = &editor.panels[PANEL_BOTTOM];
    p->title = "";    // Titlebar drawn by anim_editor_draw_titlebar
    p->render_content = render_anim_editor_panel;

    // Make the panel wider than the text editor to give the timeline room.
    // We override the panel width by adjusting the global percentage; the
    // panel_get_rect for PANEL_BOTTOM uses sw * 0.65 when size >= 400 — see
    // the note in editor.c. We just set a taller min height here.
    float sh = (float)context.swapChainExtent.height;
    float desired = ANIM_EDITOR_DEFAULT_SIZE;
    if (desired < p->min_size) desired = p->min_size;
    if (desired > sh - 40.0f)  desired = sh - 40.0f;
    p->size = desired;

    // ── Swap keymaps ──────────────────────────────────────────────────────
    KeyChordMap tmp = keymap;
    keymap    = s_ae_keymap;
    s_ae_keymap = tmp;
    set_active_text_input(NULL);   // Animation editor has no text input

    return true;
}

void anim_editor_close(void) {
    AnimEditorState* ae = &g_anim_editor;
    if (!ae->visible) return;

    // Trigger panel slide down; do NOT instantly hide content or stop playback!
    editor_close_panel(PANEL_BOTTOM);

    // Swap keymap back
    KeyChordMap tmp = keymap;
    keymap    = s_ae_keymap;
    s_ae_keymap = tmp;
    set_active_text_input(NULL);
}

///  Keybinding callbacks

void anim_editor_play_pause(void) {
    AnimEditorState* ae = &g_anim_editor;
    if (!ae->visible) return;
    ae->playing = !ae->playing;
}

void anim_editor_stop(void) {
    AnimEditorState* ae = &g_anim_editor;
    if (!ae->visible) return;
    ae->playing = false;
    ae->time    = 0.0f;
}

void anim_editor_next_keyframe(void) {
    AnimEditorState* ae = &g_anim_editor;
    if (!ae->visible || ae->selected_track < 0) return;
    AnimTrack* track = &ae->tracks[ae->selected_track];
    for (int i = 0; i < track->kf_count; i++) {
        if (track->keyframes[i].time > ae->time + 0.0005f) {
            ae->time = track->keyframes[i].time;
            return;
        }
    }
    // Wrap to first
    if (track->kf_count > 0)
        ae->time = track->keyframes[0].time;
}

void anim_editor_prev_keyframe(void) {
    AnimEditorState* ae = &g_anim_editor;
    if (!ae->visible || ae->selected_track < 0) return;
    AnimTrack* track = &ae->tracks[ae->selected_track];
    for (int i = track->kf_count - 1; i >= 0; i--) {
        if (track->keyframes[i].time < ae->time - 0.0005f) {
            ae->time = track->keyframes[i].time;
            return;
        }
    }
    // Wrap to last
    if (track->kf_count > 0)
        ae->time = track->keyframes[track->kf_count - 1].time;
}


///  Per-frame

void anim_editor_update(float dt, double mx, double my) {
    AnimEditorState* ae = &g_anim_editor;

    // Once the panel finishes sliding out of view, we cleanly restore the Text Editor state
    if (ae->visible && editor.panels[PANEL_BOTTOM].target_t == 0.0f && editor.panels[PANEL_BOTTOM].t == 0.0f) {
        ae->visible = false;
        editor.panels[PANEL_BOTTOM].title = "Text Editor";
        editor.panels[PANEL_BOTTOM].render_content = NULL;
    }

    if (ae->dragging_playhead && ae->visible) {
        float header_w = ANIM_EDITOR_HEADER_W;
        float tl_x     = ae->panel_x + ANIM_EDITOR_PAD_X + header_w;
        float tl_w     = ae->panel_w - ANIM_EDITOR_PAD_X * 2.0f - header_w;
        ae->time = ae_x_to_time(tl_x, tl_w, (float)mx);
        if (ae->time < 0.0f) ae->time = 0.0f;
        if (ae->time > ae->total_duration) ae->time = ae->total_duration;
    }

    // ── Advance playback ──────────────────────────────────────────────────
    if (ae->playing && ae->total_duration > 0.0f) {
        ae->time += dt;

        bool looped = false;
        if (ae->time > ae->total_duration) {
            ae->time = fmodf(ae->time, ae->total_duration);
            looped = true;
        }

        // Auto-scroll the view window to keep the playhead visible
        // ONLY scroll if we are zoomed in (range < total_duration).
        float range = ae->view_end - ae->view_start;
        bool is_zoomed = range < ae->total_duration * 0.99f;

        if (ae->visible && is_zoomed) {
            if (looped) {
                // Snap back to the beginning if we looped
                ae->view_start = 0.0f;
                ae->view_end   = range;
            } else if (ae->time > ae->view_end - range * 0.1f) {
                ae->view_start = ae->time - range * 0.1f;
                ae->view_end   = ae->view_start + range;
                ae_clamp_view();
            }
        }
    }

    if (!ae->visible) return;

    // ── Smooth scroll integration ─────────────────────────────────────────
    if (ae->scroll_x_t < 1.0f) {
        ae->scroll_x_t += 3.0f * dt;
        if (ae->scroll_x_t >= 1.0f) {
            ae->scroll_x_t = 1.0f;
            ae->scroll_x   = ae->scroll_x_target;
        } else {
            ae->scroll_x = ae->scroll_x_start +
                (ae->scroll_x_target - ae->scroll_x_start) *
                ease_cubic_out(ae->scroll_x_t);
        }
    }

    if (ae->scroll_y_t < 1.0f) {
        ae->scroll_y_t += 3.0f * dt;
        if (ae->scroll_y_t >= 1.0f) {
            ae->scroll_y_t = 1.0f;
            ae->scroll_y   = ae->scroll_y_target;
        } else {
            ae->scroll_y = ae->scroll_y_start +
                (ae->scroll_y_target - ae->scroll_y_start) *
                ease_cubic_out(ae->scroll_y_t);
        }
    }
}


///  Titlebar

void anim_editor_draw_titlebar(float x, float y, float w, float h,
                               float mx, float my) {
    AnimEditorState* ae = &g_anim_editor;
    Font* font = editor.font;
    if (!font || !ae->visible) return;

    ae_load_icons();

    float cx   = x + ANIM_EDITOR_PAD_X;
    float cy   = y + h * 0.5f;
    float icon_size = 16.0f;
    float icon_y    = cy - icon_size * 0.5f - 1.0f;

    // ── Animation name label ──────────────────────────────────────────────
    const char* label = "Animation";
    if (ae->selected_track >= 0 && ae->selected_track < ae->track_count)
        label = ae->tracks[ae->selected_track].name;
    text(font, label, cx, cy - 2.0f, CT.text);
    cx += measure_text_width(font, label, 1.0f) + 16.0f;

    // ── Transport buttons: Stop · Play/Pause ──────────────────────────────
    // Stop button
    {
        float bx = cx;
        bool hov = (mx >= bx - 4.0f && mx <= bx + icon_size + 4.0f &&
                    my >= icon_y - 4.0f && my <= icon_y + icon_size + 4.0f);
        if (hov) exQuad2D((vec2){bx - 4.0f, icon_y - 4.0f}, (vec2){icon_size + 8.0f, icon_size + 8.0f}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.button, CT.button);

        Texture2D* tex = texture_pool_get(s_icon_stop);
        if (tex)
            texture2D((vec2){bx, icon_y}, (vec2){icon_size, icon_size},
                      tex, hov ? CT.text : CT.text_dim);
        cx += icon_size + 8.0f;
    }

    // Play / Pause button
    {
        float bx = cx;
        bool hov = (mx >= bx - 4.0f && mx <= bx + icon_size + 4.0f &&
                    my >= icon_y - 4.0f && my <= icon_y + icon_size + 4.0f);
        if (hov) exQuad2D((vec2){bx - 4.0f, icon_y - 4.0f}, (vec2){icon_size + 8.0f, icon_size + 8.0f}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.button, CT.button);

        int32_t icon = ae->playing ? s_icon_pause : s_icon_play;
        Texture2D* tex = texture_pool_get(icon);
        Color col = ae->playing ? CT.success : (hov ? CT.text : CT.text_dim);
        if (tex)
            texture2D((vec2){bx, icon_y}, (vec2){icon_size, icon_size}, tex, col);
        cx += icon_size + 16.0f;
    }

    // ── Playhead time readout ─────────────────────────────────────────────
    char time_buf[32];
    int mins = (int)(ae->time / 60.0f);
    float secs = ae->time - mins * 60.0f;
    snprintf(time_buf, sizeof(time_buf), "%d:%06.3f", mins, secs);
    text(font, time_buf, cx, cy - 2.0f, CT.text_dim);

    // ── Close button ─────────────────────────────────────────────────────
    float close_x = x + w - ANIM_EDITOR_PAD_X - icon_size;
    float close_y = y + (h - icon_size) * 0.5f - 1.0f;
    bool close_hov = (mx >= close_x - 8.0f && mx <= close_x + icon_size + 8.0f &&
                      my >= y && my <= y + h);
    Texture2D* tex_close = texture_pool_get(s_icon_close);
    if (tex_close)
        texture2D((vec2){close_x, close_y}, (vec2){icon_size, icon_size},
                  tex_close, close_hov ? CT.error : CT.border);
}


///  Render — timeline content

void render_anim_editor_panel(Panel* panel, float px, float py, float pw, float ph) {
    (void)panel;
    if (!editor.font) return;

    AnimEditorState* ae = &g_anim_editor;
    if (!ae->visible) return;

    ae->panel_x = px;
    ae->panel_y = py;
    ae->panel_w = pw;
    ae->panel_h = ph;

    Font* font = editor.font;
    float lh   = font->ascent - font->descent + 4.0f;

    // ── Geometry ──────────────────────────────────────────────────────────
    //   px,py are the bottom-left of the full panel (Y-up).
    //   The titlebar sits at the top (y + ph - TITLE_H in screen coords but
    //   since Y grows up in our coordinate system, top = py + ph - TITLE_H).
    //   Content sits below the titlebar.

    float content_x = px + ANIM_EDITOR_PAD_X;
    float content_y = py + ANIM_EDITOR_PAD_Y;
    float content_w = pw - ANIM_EDITOR_PAD_X * 2.0f;
    float content_h = ph - ANIM_EDITOR_TITLE_H - ANIM_EDITOR_PAD_Y;

    if (content_h < 10.0f) return;

    // Header column (track labels on the left)
    float header_w  = ANIM_EDITOR_HEADER_W;
    // Timeline area (keyframes on the right)
    float tl_x      = content_x + header_w;
    float tl_w      = content_w - header_w;
    float tl_top    = content_y + content_h;   // Y-up: top of content area
    float tl_bot    = content_y;               // Y-up: bottom

    // ── Background ────────────────────────────────────────────────────────
    exQuad2D((vec2){content_x, content_y}, (vec2){content_w, content_h},
             (vec4){0,0,0,0}, 0.0f, CT.bg_deep, CT.bg_deep);

    // Header column slightly lighter
    exQuad2D((vec2){content_x, content_y}, (vec2){header_w, content_h},
             (vec4){0,0,0,0}, 0.0f, CT.bg_alt, CT.bg_alt);

    // Thin separator between header and timeline
    quad2D((vec2){tl_x - 1.0f, content_y}, (vec2){1.0f, content_h}, CT.border);

    // ── Ruler ─────────────────────────────────────────────────────────────
    // Draw tick marks and time labels across the top of the timeline.
    float ruler_h = 20.0f;
    float ruler_y = tl_top - ruler_h;  // Y-up bottom edge of the ruler strip

    exQuad2D((vec2){tl_x, ruler_y}, (vec2){tl_w, ruler_h},
             (vec4){0,0,0,0}, 0.0f, CT.bg_deepest, CT.bg_deepest);

    // Choose a sensible tick interval based on the visible range.
    // We target roughly 60–120 px between major ticks.
    float range  = ae->view_end - ae->view_start;
    if (range <= 0.0f) range = 1.0f;

    float px_per_sec = tl_w / range;
    float intervals[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f};
    float major_interval = 1.0f;
    for (int ii = 0; ii < (int)(sizeof(intervals)/sizeof(intervals[0])); ii++) {
        if (intervals[ii] * px_per_sec >= 80.0f) { major_interval = intervals[ii]; break; }
    }
    float minor_interval = major_interval / 5.0f;

    // Start slightly before view_start so ticks don't pop in/out
    float t_start = floorf(ae->view_start / minor_interval) * minor_interval;
    for (float t = t_start; t <= ae->view_end + minor_interval; t += minor_interval) {
        float tx = ae_time_to_x(tl_x, tl_w, t);
        if (tx < tl_x || tx > tl_x + tl_w) continue;

        bool is_major = (fabsf(fmodf(t + major_interval * 0.0001f, major_interval)) < minor_interval * 0.5f);

        float tick_h = is_major ? ruler_h * 0.55f : ruler_h * 0.3f;
        float tick_y = ruler_y;  // ticks hang down from top of ruler in Y-up = bottom edge of ruler rect
        quad2D((vec2){tx, tick_y}, (vec2){1.0f, tick_h},
               is_major ? CT.text_dim : CT.border);

        if (is_major) {
            char label[16];
            if (major_interval < 1.0f)
                snprintf(label, sizeof(label), "%.2fs", t);
            else {
                int m = (int)(t / 60.0f);
                float s = t - m * 60.0f;
                if (m > 0) snprintf(label, sizeof(label), "%d:%04.1f", m, s);
                else       snprintf(label, sizeof(label), "%.0fs", s);
            }
            float label_w = measure_text_width(font, label, 1.0f);
            float label_x = tx - label_w * 0.5f;
            if (label_x < tl_x) label_x = tl_x;
            if (label_x + label_w > tl_x + tl_w) label_x = tl_x + tl_w - label_w;
            float label_y = ruler_y + ruler_h * 0.5f + 2.0f;
            text(font, label, label_x, label_y, CT.text_dim);
        }
    }

    // ── Tracks ────────────────────────────────────────────────────────────
    float track_h   = ANIM_EDITOR_TRACK_H;
    float tracks_top = ruler_y;   // Perfectly flush with the ruler now that the upward-drawing bug is fixed!

    for (int ti = 0; ti < ae->track_count; ti++) {
        AnimTrack* track = &ae->tracks[ti];

        float row_top = tracks_top - (float)(ti + 1) * track_h - ae->scroll_y;
        float row_bot = row_top + track_h;

        // Clip to content area
        if (row_bot < content_y || row_top > tracks_top) continue;

        bool is_selected = (ae->selected_track == ti);

        // ── Track row background ──────────────────────────────────────────
        Color row_bg = is_selected ? CT.fs_hovered : (ti % 2 == 0 ? CT.bg_deep : CT.bg_alt);
        // FIX: Y-up rects must start at row_top (the bottom edge), NOT row_bot!
        exQuad2D((vec2){content_x, row_top}, (vec2){content_w, track_h},
                 (vec4){0,0,0,0}, 0.0f, row_bg, row_bg);

        // Thin bottom border for each row
        quad2D((vec2){content_x, row_top}, (vec2){content_w, 1.0f}, CT.border);

        // ── Label (left column) ───────────────────────────────────────────
        float label_y = row_top + track_h * 0.5f - 2.0f;
        Color label_col = is_selected ? CT.text : CT.text_dim;

        // Truncate label if too long
        char display_name[64];
        strncpy(display_name, track->name, sizeof(display_name) - 1);
        display_name[sizeof(display_name) - 1] = '\0';

        float max_label_w = header_w - ANIM_EDITOR_PAD_X * 2.0f;
        float dots_w = measure_text_width(font, "..", 1.0f);
        int len = strlen(display_name);
        while (len > 0 && measure_text_width(font, display_name, 1.0f) > max_label_w - dots_w) {
            display_name[--len] = '\0';
        }
        if (len < (int)strlen(track->name)) strcat(display_name, "..");

        text(font, display_name, content_x + ANIM_EDITOR_PAD_X, label_y, label_col);

        // ── Keyframe diamonds ─────────────────────────────────────────────
        // Clip draws to the timeline column
        float kf_center_y = row_top + track_h * 0.5f;

        for (int ki = 0; ki < track->kf_count; ki++) {
            AnimKeyframe* kf = &track->keyframes[ki];
            float kx = ae_time_to_x(tl_x, tl_w, kf->time);
            if (kx < tl_x - 8.0f || kx > tl_x + tl_w + 8.0f) continue;

            // Vertical guide line across all tracks from this keyframe position
            if (ti == 0) {
                quad2D((vec2){kx, content_y}, (vec2){1.0f, content_h - ruler_h},
                       (Color){CT.border.r, CT.border.g, CT.border.b, 0.35f});
            }

            int32_t icon_idx = kf->selected ? s_icon_key_sel : s_icon_key_bez;
            Texture2D* tex   = texture_pool_get(icon_idx);
            if (tex && tex->loaded) {
                float icon_size = 14.0f;
                Color tint = kf->selected
                    ? CT.text
                    : (is_selected ? CT.text : CT.text_dim);
                texture2D((vec2){kx - icon_size * 0.5f, kf_center_y - icon_size * 0.5f},
                          (vec2){icon_size, icon_size}, tex, tint);
            }
        }
    }

    // ── Playhead ──────────────────────────────────────────────────────────
    {
        float ph_x = ae_time_to_x(tl_x, tl_w, ae->time);
        if (ph_x >= tl_x && ph_x <= tl_x + tl_w) {
            // Vertical line
            quad2D((vec2){ph_x, content_y}, (vec2){2.0f, content_h}, CT.accent);

            // Flat-top triangle head pointing down, exactly like Godot
            float head_h = 12.0f;
            float head_w = 14.0f;
            float top_y = ruler_y + ruler_h;
            float bot_y = top_y - head_h;
            vec2 p0 = {ph_x - head_w * 0.5f, top_y};
            vec2 p1 = {ph_x + head_w * 0.5f, top_y};
            vec2 p2 = {ph_x, bot_y};
            // Draw both windings to bypass any GPU backface culling!
            triangle_col(p0, CT.accent, p1, CT.accent, p2, CT.accent);
            triangle_col(p0, CT.accent, p2, CT.accent, p1, CT.accent);
        }
    }

    // ── Empty state ───────────────────────────────────────────────────────
    if (ae->track_count == 0) {
        const char* msg = "No animations";
        float mw = measure_text_width(font, msg, 1.0f);
        text(font, msg,
             content_x + (content_w - mw) * 0.5f,
             content_y + content_h * 0.5f,
             CT.text_dim);
    }
}


///  Input

bool anim_editor_wants_mouse(double mx, double my) {
    AnimEditorState* ae = &g_anim_editor;
    if (!ae->visible) return false;
    float sh = (float)context.swapChainExtent.height;
    float ry = sh - (float)my;
    return ((float)mx >= ae->panel_x && (float)mx <= ae->panel_x + ae->panel_w &&
            ry >= ae->panel_y && ry <= ae->panel_y + ae->panel_h);
}

void anim_editor_scroll(double dx, double dy) {
    AnimEditorState* ae = &g_anim_editor;
    if (!ae->visible) return;

    // Horizontal scroll (or Shift+vertical) pans the timeline view window.
    float range   = ae->view_end - ae->view_start;
    float pan_sec = (float)dx * range * 0.05f;

    // Vertical scroll moves through tracks.
    if (dy != 0.0) {
        float new_scroll = ae->scroll_y_target - (float)dy * ANIM_EDITOR_TRACK_H * 2.0f;
        if (new_scroll < 0.0f) new_scroll = 0.0f;
        float max_scroll = ae->track_count * ANIM_EDITOR_TRACK_H;
        if (new_scroll > max_scroll) new_scroll = max_scroll;
        ae_smooth_scroll_y(new_scroll);
    }

    if (pan_sec != 0.0f) {
        ae->view_start += pan_sec;
        ae->view_end   += pan_sec;
        ae_clamp_view();
    }
}

void anim_editor_mouse_button(int button, int action, double mx, double my) {
    AnimEditorState* ae = &g_anim_editor;
    if (!ae->visible) return;
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;

    float sh = (float)context.swapChainExtent.height;
    float ry = sh - (float)my;   // Y-up

    float header_w = ANIM_EDITOR_HEADER_W;
    float tl_x     = ae->panel_x + ANIM_EDITOR_PAD_X + header_w;
    float tl_w     = ae->panel_w - ANIM_EDITOR_PAD_X * 2.0f - header_w;
    float content_h = ae->panel_h - ANIM_EDITOR_TITLE_H - ANIM_EDITOR_PAD_Y;
    float ruler_h   = 20.0f;
    float ruler_y   = ae->panel_y + ANIM_EDITOR_PAD_Y + content_h - ruler_h;
    float tracks_top = ruler_y;

    if (action == GLFW_PRESS) {
        // ── Transport buttons in the titlebar ─────────────────────────────
        float title_y     = ae->panel_y + ae->panel_h - ANIM_EDITOR_TITLE_H;
        float title_h     = ANIM_EDITOR_TITLE_H;
        float label_w     = 0.0f;
        if (ae->selected_track >= 0 && ae->selected_track < ae->track_count)
            label_w = measure_text_width(editor.font, ae->tracks[ae->selected_track].name, 1.0f);

        float btn_x     = ae->panel_x + ANIM_EDITOR_PAD_X + label_w + 16.0f;
        float icon_size = 16.0f;

        bool in_title = (ry >= title_y && ry <= title_y + title_h);
        if (in_title) {
            // Stop
            if ((float)mx >= btn_x - 4.0f && (float)mx <= btn_x + icon_size + 4.0f) {
                anim_editor_stop();
                return;
            }
            btn_x += icon_size + 8.0f;
            // Play/Pause
            if ((float)mx >= btn_x - 4.0f && (float)mx <= btn_x + icon_size + 4.0f) {
                anim_editor_play_pause();
                return;
            }

            // Close
            float close_x = ae->panel_x + ae->panel_w - ANIM_EDITOR_PAD_X - icon_size;
            if ((float)mx >= close_x - 8.0f && (float)mx <= close_x + icon_size + 8.0f) {
                anim_editor_close();
                return;
            }
        }

        // ── Click on ruler → move playhead ────────────────────────────────
        float ruler_y_bottom = ruler_y;
        float ruler_y_top    = ruler_y + ruler_h;
        if ((float)mx >= tl_x && (float)mx <= tl_x + tl_w &&
            ry >= ruler_y_bottom && ry <= ruler_y_top) {
            ae->time = ae_x_to_time(tl_x, tl_w, (float)mx);
            if (ae->time < 0.0f) ae->time = 0.0f;
            if (ae->time > ae->total_duration) ae->time = ae->total_duration;
            ae->dragging_playhead = true;
            return;
        }

        // ── Click on a track row → select track / toggle keyframe ─────────
        for (int ti = 0; ti < ae->track_count; ti++) {
            float row_top = tracks_top - (float)(ti + 1) * ANIM_EDITOR_TRACK_H - ae->scroll_y;

            if (ry >= row_top && ry <= row_top + ANIM_EDITOR_TRACK_H) {
                ae->selected_track = ti;

                // Check if a keyframe icon was hit (8px radius around center)
                AnimTrack* track = &ae->tracks[ti];
                for (int ki = 0; ki < track->kf_count; ki++) {
                    float kx = ae_time_to_x(tl_x, tl_w, track->keyframes[ki].time);
                    if (fabsf((float)mx - kx) <= 8.0f) {
                        // Toggle selection; deselect all others in this track
                        bool was_selected = track->keyframes[ki].selected;
                        for (int kj = 0; kj < track->kf_count; kj++)
                            track->keyframes[kj].selected = false;
                        track->keyframes[ki].selected = !was_selected;
                        // Snap playhead to this keyframe
                        ae->time = track->keyframes[ki].time;
                        return;
                    }
                }
                return;
            }
        }

    } else if (action == GLFW_RELEASE) {
        ae->dragging_playhead = false;
    }
}
