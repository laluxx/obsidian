#include "add_menu.h"
#include "renderer.h"
#include "editor.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern void set_active_text_input(void (*cb)(char));

#define ADD_MENU_MAX_RESULTS 256
#define ADD_MENU_INPUT_SIZE 64
#define ADD_MENU_MAX_DEPTH 16

typedef void (*MenuAction)(void);

typedef struct MenuItem {
    const char* name;
    int32_t icon_id;
    bool is_menu;
    struct MenuItem* children;
    int child_count;
    MenuAction action;
} MenuItem;

typedef struct {
    bool is_active;
    float x, y;
    float width;

    char input[ADD_MENU_INPUT_SIZE];
    size_t input_length;
    size_t cursor;
    double last_key_time;

    // Hierarchy Stack
    MenuItem* menu_stack[ADD_MENU_MAX_DEPTH];
    int stack_depth; // 0 = root menu

    // View State
    MenuItem* visible_items[ADD_MENU_MAX_RESULTS];
    int visible_count;
    int selected_index;
    int scroll_offset;

    // Rendered Bounding Box (for mouse hit-testing)
    float box_x, box_y, box_w, box_h;
    float title_h, item_h;

    // Slide animation
    float anim_t;
    float anim_target;
    bool  closing;

    KeyChordMap saved_keymap;
    KeyChordMap menu_keymap;

    // Icons
    int32_t icon_mesh;
    int32_t icon_plane;
    int32_t icon_cube;
    int32_t icon_circle;
    int32_t icon_uvsphere;
    int32_t icon_icosphere;
    int32_t icon_cylinder;
    int32_t icon_cone;
    int32_t icon_torus;
    int32_t icon_arrow_right;

    int32_t icon_collider_cat;
    int32_t icon_col_cube;
    int32_t icon_col_sphere;
    int32_t icon_col_capsule;
} AddMenuState;

static AddMenuState add_menu = {0};

#include "scene.h"

// --- Mesh Spawning Actions ---
extern void markMeshesSSBODirty(VulkanContext* ctx);
extern VulkanContext context;
extern uint32_t megaBufferAllocate(VulkanContext* ctx, Vertex* vertices, uint32_t vertexCount);
extern uint32_t megaIndexBufferAllocate(VulkanContext* ctx, uint32_t* indices, uint32_t indexCount);
extern uint32_t megaMeshletBufferAllocate(VulkanContext* ctx, Meshlet* meshlets, MeshletBounds* bounds, MeshletSkinData* skins, uint32_t count);
extern uint32_t megaMeshletVertexBufferAllocate(VulkanContext* ctx, uint32_t* vertices, uint32_t count);
extern uint32_t megaMeshletTriangleBufferAllocate(VulkanContext* ctx, uint8_t* triangles, uint32_t count);
extern void flushUploadStagingBuffer(VulkanContext* ctx);

static void action_plane    () { plane    ((vec3){0,0,0}, (vec2){2,2},          (Color){1,1,1,1}); }
static void action_cube     () { cube     ((vec3){0,0,0}, 2.0f,                 (Color){1,1,1,1}); }
static void action_circle   () { circle   ((vec3){0,0,0}, 1.0f,                 (Color){1,1,1,1}); }
static void action_uvsphere () { uv_sphere((vec3){0,0,0}, 1.0f, 12, 12,         (Color){1,1,1,1}); }
static void action_icosphere() { uv_sphere((vec3){0,0,0}, 1.0f, 12, 12,         (Color){1,1,1,1}); }
static void action_cylinder () { cylinder ((vec3){0,0,0}, 2.0f, 1.0f,           (Color){1,1,1,1}); }
static void action_cone     () { cone     ((vec3){0,0,0}, 2.0f, 1.0f,           (Color){1,1,1,1}); }

extern void physics_rebuild_mesh(Mesh* m);

static void add_collider_to_selected(int type, const char* name) {
    (void)name;
    int mi = editor.inspector.selected_mesh_index;
    if (mi >= 0 && mi < (int)scene.meshes.count) {
        Mesh* m = &scene.meshes.items[mi];
        m->collider_type = type;
        physics_rebuild_mesh(m);
        extern bool scene_topology_dirty;
        scene_topology_dirty = true;
        inspector_show_collider();
        inspector_select_mesh_internal(mi);
    }
}

static void action_col_cube()    { add_collider_to_selected(1, "Cube Collider"); }
static void action_col_sphere()  { add_collider_to_selected(2, "Sphere Collider"); }
static void action_col_capsule() { add_collider_to_selected(3, "Capsule Collider"); }

static void action_sdf_sphere() {
    sdfs_add(&scene, "SDF Sphere", SDF_TYPE_SPHERE, (vec3){0, 0, 0}, (vec4){1.0f, 0, 0, 0}, (vec4){0.8f, 0.2f, 0.2f, 1.0f});
    message(MSG_SUCCESS, "SDF Sphere added!");
}
static void action_sdf_cube()   {
    sdfs_add(&scene, "SDF Cube", SDF_TYPE_BOX, (vec3){2, 0, 0}, (vec4){1.0f, 1.0f, 1.0f, 0}, (vec4){0.2f, 0.8f, 0.2f, 1.0f});
    message(MSG_SUCCESS, "SDF Cube added!");
}

static void action_add_material(void) {
    int mi = editor.inspector.selected_mesh_index;
    if (mi < 0 || mi >= (int)scene.meshes.count) return;
    Mesh* m = &scene.meshes.items[mi];
    m->wireframe          = false;
    m->baseColorFactor[0] = 1.0f;
    m->baseColorFactor[1] = 1.0f;
    m->baseColorFactor[2] = 1.0f;
    m->baseColorFactor[3] = 1.0f;
    m->metallicFactor     = 0.0f;
    m->roughnessFactor    = 0.5f;
    m->emissiveStrength   = 1.0f;
    if (m->node) {
        int32_t mesh_node_idx = (int32_t)(uintptr_t)m->node;
        bool has_material_node = false;
        int32_t c = scene.tree.nodes[mesh_node_idx].first_child;
        while (c >= 0) {
            if (strcmp(scene.tree.nodes[c].name, "Material") == 0) {
                has_material_node = true; break;
            }
            c = scene.tree.nodes[c].next_sibling;
        }
        if (!has_material_node)
            scene_tree_add_node(&scene.tree, "Material", mesh_node_idx, -1);
    }
    extern bool scene_topology_dirty;
    scene_topology_dirty = true;
    markMeshesSSBODirty(&context);
    inspector_show_material();
    inspector_select_mesh_internal(mi);
    message(MSG_SUCCESS, "Material added");
    add_menu_close();
}

void regenerate_active_primitive(void) {
    if (!adjust_state.active || adjust_state.mesh_index < 0 || adjust_state.mesh_index >= (int)scene.meshes.count) return;
    Mesh* m = &scene.meshes.items[adjust_state.mesh_index];

    // Heap allocation to completely eliminate stack overflow risk for high-segment geometry
    Vertex* verts = malloc(16384 * sizeof(Vertex));
    uint32_t* indices = malloc(98304 * sizeof(uint32_t));
    uint32_t v_count = 0, i_count = 0;

    // MEMORY LEAK PREVENTION:
    // If this mesh was the absolute last thing added to the mega buffers,
    // we can safely rewind the allocators to overwrite the old geometry!
    extern VulkanContext context;
    if (m->megaBaseVertex != UINT32_MAX && m->megaBaseVertex + m->vertexCount == context.megaVertexBufferOffset) {
        context.megaVertexBufferOffset -= m->vertexCount;
    }
    if (m->megaBaseIndex != UINT32_MAX && m->megaBaseIndex + m->indexCount == context.megaIndexBufferOffset) {
        context.megaIndexBufferOffset -= m->indexCount;
    }

    if (adjust_state.type == ADJUST_PRIMITIVE_TORUS) {
        int segs1 = adjust_state.torus_major_segs;
        int segs2 = adjust_state.torus_minor_segs;
        float r_maj = adjust_state.torus_major_rad;
        float r_min = adjust_state.torus_minor_rad;

        // Safety clamps to prevent buffer overflow
        if (segs1 > 128) segs1 = 128;
        if (segs2 > 64) segs2 = 64;

        for(int i=0; i<=segs1; i++) {
            float u = (float)i / segs1;
            float a1 = u * 6.2831853f;
            float cos1 = cosf(a1), sin1 = sinf(a1);
            for(int j=0; j<=segs2; j++) {
                float v = (float)j / segs2;
                float a2 = v * 6.2831853f;
                float cos2 = cosf(a2), sin2 = sinf(a2);

                float r = r_maj + r_min * cos2;
                float px = r * cos1, py = r_min * sin2, pz = r * sin1;
                float nx = cos2 * cos1, ny = sin2, nz = cos2 * sin1;

                verts[v_count] = (Vertex){ .pos={px,py,pz}, .normal={nx,ny,nz}, .texCoord={u, v}, .color={1,1,1,1}, .tangent={-sin1,0,cos1,1} };
                v_count++;
            }
        }
        for(int i=0; i<segs1; i++) {
            for(int j=0; j<segs2; j++) {
                uint32_t p0 = i*(segs2+1) + j;
                uint32_t p1 = p0 + 1;
                uint32_t p2 = (i+1)*(segs2+1) + j;
                uint32_t p3 = p2 + 1;
                indices[i_count++] = p0; indices[i_count++] = p1; indices[i_count++] = p2;
                indices[i_count++] = p1; indices[i_count++] = p3; indices[i_count++] = p2;
            }
        }
    }

    // Default tangents for shader compatibility
    for(uint32_t i=0; i<v_count; i++) {
        if (verts[i].tangent[3] == 0.0f) {
            verts[i].tangent[0] = 1.0f; verts[i].tangent[3] = 1.0f;
        }
    }

    m->megaBaseVertex = megaBufferAllocate(&context, verts, v_count);
    m->megaBaseIndex = megaIndexBufferAllocate(&context, indices, i_count);
    m->vertexCount = v_count;
    m->indexCount = i_count;

    glm_mat4_identity(m->local_transform);
    glm_translate(m->local_transform, adjust_state.position);
    glm_rotate_z(m->local_transform, glm_rad(adjust_state.rotation[2]), m->local_transform);
    glm_rotate_y(m->local_transform, glm_rad(adjust_state.rotation[1]), m->local_transform);
    glm_rotate_x(m->local_transform, glm_rad(adjust_state.rotation[0]), m->local_transform);
    glm_mat4_copy(m->local_transform, m->model);

    free(verts);
    free(indices);

    extern bool scene_topology_dirty;
    scene_topology_dirty = true; // Force the indirect buffer to rebuild with new vertex counts

    markMeshesSSBODirty(&context);
    flushUploadStagingBuffer(&context);
}

static void action_torus() {
    torus((vec3){0,0,0}, 1.0f, 0.3f, 16, 12, (Color){1,1,1,1});

    adjust_state.active = true;
    adjust_state.type = ADJUST_PRIMITIVE_TORUS;
    adjust_state.mesh_index = scene.meshes.count - 1;
    adjust_state.torus_major_segs = 16;
    adjust_state.torus_minor_segs = 12;
    adjust_state.torus_major_rad = 1.0f;
    adjust_state.torus_minor_rad = 0.3f;
    glm_vec3_zero(adjust_state.position);
    glm_vec3_zero(adjust_state.rotation);

    extern void adjust_panel_reset_anim(void);
    adjust_panel_reset_anim();

    // Force an immediate regeneration to synchronize the CPU state with the GPU parameters
    regenerate_active_primitive();
}

// --- Menu Hierarchy Definition ---
static MenuItem mesh_children[] = {
    {"Plane",      -1, false, NULL, 0, action_plane},
    {"Cube",       -1, false, NULL, 0, action_cube},
    {"Circle",     -1, false, NULL, 0, action_circle},
    {"UV Sphere",  -1, false, NULL, 0, action_uvsphere},
    {"Ico Sphere", -1, false, NULL, 0, action_icosphere},
    {"Cylinder",   -1, false, NULL, 0, action_cylinder},
    {"Cone",       -1, false, NULL, 0, action_cone},
    {"Torus",      -1, false, NULL, 0, action_torus}
};

static MenuItem collider_children[] = {
    {"Cube", -1, false, NULL, 0, action_col_cube},
    {"Sphere", -1, false, NULL, 0, action_col_sphere},
    {"Capsule", -1, false, NULL, 0, action_col_capsule}
};

static MenuItem sdf_children[] = {
    {"Sphere", -1, false, NULL, 0, action_sdf_sphere},
    {"Cube",   -1, false, NULL, 0, action_sdf_cube}
};

static MenuItem root_children[] = {
    {"Mesh",     -1, true,  mesh_children,     sizeof(mesh_children)/sizeof(MenuItem),      NULL},
    {"Collider", -1, true,  collider_children, sizeof(collider_children)/sizeof(MenuItem), NULL},
    {"SDF",      -1, true,  sdf_children,      sizeof(sdf_children)/sizeof(MenuItem),      NULL},
    {"Material", -1, false, NULL, 0, NULL}
};

static MenuItem root_menu = {
    "Add", -1, true, root_children, sizeof(root_children)/sizeof(MenuItem), NULL
};

// --- Fast Case-Insensitive Substring Match ---
static bool str_contains_ci(const char* haystack, const char* needle) {
    if (!*needle) return true;
    char nc = (*needle >= 'A' && *needle <= 'Z') ? *needle + 32 : *needle;
    for (const char* h = haystack; *h; h++) {
        char hc = (*h >= 'A' && *h <= 'Z') ? *h + 32 : *h;
        if (hc == nc) {
            const char* h1 = h + 1;
            const char* n1 = needle + 1;
            while (*n1) {
                char h1c = (*h1 >= 'A' && *h1 <= 'Z') ? *h1 + 32 : *h1;
                char n1c = (*n1 >= 'A' && *n1 <= 'Z') ? *n1 + 32 : *n1;
                if (h1c != n1c) break;
                h1++; n1++;
            }
            if (!*n1) return true;
        }
    }
    return false;
}

// --- Filtering & View Management ---
static bool mesh_has_material(void) {
    int mi = editor.inspector.selected_mesh_index;
    if (mi < 0 || mi >= (int)scene.meshes.count) return true;
    Mesh* m = &scene.meshes.items[mi];
    return !m->wireframe;
}

static void flatten_search_recursive(MenuItem* node, const char* query) {
    if (add_menu.visible_count >= ADD_MENU_MAX_RESULTS) return;

    if (node == &root_children[1] && editor.inspector.selected_mesh_index == -1) return;
    if (node == &root_children[3] && mesh_has_material()) return;

    if (!node->is_menu) {
        if (str_contains_ci(node->name, query)) {
            add_menu.visible_items[add_menu.visible_count++] = node;
        }
    } else {
        for (int i = 0; i < node->child_count; i++) {
            flatten_search_recursive(&node->children[i], query);
        }
    }
}

static void add_menu_filter(void) {
    add_menu.visible_count = 0;

    // Wire the Material action dynamically
    root_children[3].action = action_add_material;

    if (add_menu.input_length == 0) {
        MenuItem* current = add_menu.menu_stack[add_menu.stack_depth];
        for (int i = 0; i < current->child_count && i < ADD_MENU_MAX_RESULTS; i++) {
            // Collider: only show if a mesh is selected
            if (&current->children[i] == &root_children[1] && editor.inspector.selected_mesh_index == -1) continue;
            // Material: only show if selected mesh has no material
            if (&current->children[i] == &root_children[3] && mesh_has_material()) continue;
            add_menu.visible_items[add_menu.visible_count++] = &current->children[i];
        }
    } else {
        for (int i = 0; i < root_menu.child_count; i++) {
            flatten_search_recursive(&root_menu.children[i], add_menu.input);
        }
    }

    if (add_menu.selected_index >= add_menu.visible_count) {
        add_menu.selected_index = add_menu.visible_count > 0 ? add_menu.visible_count - 1 : 0;
    }
}

// --- Navigation Callbacks ---
void add_menu_next(void) {
    if (add_menu.visible_count == 0) return;
    if (add_menu.selected_index < add_menu.visible_count - 1) {
        add_menu.selected_index++;
    }
}

void add_menu_prev(void) {
    if (add_menu.visible_count == 0) return;
    if (add_menu.selected_index > 0) {
        add_menu.selected_index--;
    }
}

void add_menu_first(void) { add_menu.selected_index = 0; }
void add_menu_last(void)  { if (add_menu.visible_count > 0) add_menu.selected_index = add_menu.visible_count - 1; }

void add_menu_enter(void) {
    if (add_menu.visible_count == 0) return;
    MenuItem* selected = add_menu.visible_items[add_menu.selected_index];

    if (selected->is_menu) {
        if (add_menu.stack_depth < ADD_MENU_MAX_DEPTH - 1) {
            add_menu.stack_depth++;
            add_menu.menu_stack[add_menu.stack_depth] = selected;
            add_menu.selected_index = 0;
            add_menu.input[0] = '\0'; // Clear search when diving into a menu
            add_menu.input_length = 0;
            add_menu.cursor = 0;
            add_menu_filter();
        }
    } else {
        if (selected->action) selected->action();
        add_menu_close();
    }
}

void add_menu_back(void) {
    if (add_menu.input_length > 0) {
        // Clear search first
        add_menu.input[0] = '\0';
        add_menu.input_length = 0;
        add_menu.cursor = 0;
        add_menu_filter();
    } else if (add_menu.stack_depth > 0) {
        add_menu.stack_depth--;
        add_menu.selected_index = 0;
        add_menu_filter();
    }
}

// --- Input Callbacks ---
void add_menu_insert_char(char c) {
    if (add_menu.input_length < ADD_MENU_INPUT_SIZE - 1) {
        memmove(&add_menu.input[add_menu.cursor + 1], &add_menu.input[add_menu.cursor], add_menu.input_length - add_menu.cursor + 1);
        add_menu.input[add_menu.cursor] = c;
        add_menu.cursor++;
        add_menu.input_length++;
        add_menu_filter();
        add_menu.last_key_time = glfwGetTime();
    }
}

void add_menu_backspace(void) {
    if (add_menu.cursor > 0) {
        memmove(&add_menu.input[add_menu.cursor - 1], &add_menu.input[add_menu.cursor], add_menu.input_length - add_menu.cursor + 1);
        add_menu.cursor--;
        add_menu.input_length--;
        add_menu_filter();
        add_menu.last_key_time = glfwGetTime();
    } else if (add_menu.stack_depth > 0) {
        // Backspace on empty input pops the menu stack naturally!
        add_menu_back();
    }
}

void add_menu_cursor_left(void)  { if (add_menu.cursor > 0) add_menu.cursor--; add_menu.last_key_time = glfwGetTime(); }
void add_menu_cursor_right(void) { if (add_menu.cursor < add_menu.input_length) add_menu.cursor++; add_menu.last_key_time = glfwGetTime(); }

void add_menu_smart_left(void) {
    if (add_menu.input_length > 0) {
        add_menu_cursor_left();
    } else {
        add_menu_back();
    }
}

void add_menu_smart_right(void) {
    if (add_menu.input_length > 0 && add_menu.cursor < add_menu.input_length) {
        add_menu_cursor_right();
    } else {
        add_menu_enter();
    }
}

// --- Mouse Handling ---
void add_menu_mouse_move(double mx, double my) {
    if (!add_menu.is_active) return;

    // Hit test against the items list (Y grows up in the UI coordinate space)
    if (mx >= add_menu.box_x && mx <= add_menu.box_x + add_menu.box_w) {
        float row_start_y = add_menu.box_y + add_menu.box_h - add_menu.title_h;
        if (my < row_start_y && my >= add_menu.box_y) {
            float local_y = row_start_y - my;
            int index = (int)(local_y / add_menu.item_h);
            if (index >= 0 && index < add_menu.visible_count) {
                add_menu.selected_index = index;
            }
        }
    }
}

bool add_menu_mouse_button(int button, int action, double mx, double my) {
    if (!add_menu.is_active) return false;

    if (action == 1 /* GLFW_PRESS */) {
        bool inside = (mx >= add_menu.box_x && mx <= add_menu.box_x + add_menu.box_w &&
                       my >= add_menu.box_y && my <= add_menu.box_y + add_menu.box_h);

        if (!inside) {
            add_menu_close();
            return true; // Consumed click outside to close
        }

        // Clicked inside. If clicked on an item, select it.
        float row_start_y = add_menu.box_y + add_menu.box_h - add_menu.title_h;
        if (my < row_start_y && my >= add_menu.box_y) {
            add_menu_enter();
        }

        return true; // Modal interception
    }
    return true; // Block other mouse events while open
}

// --- Lifecycle ---
void add_menu_init(void) {
    add_menu.width = 200.0f;
    keymap_init(&add_menu.menu_keymap);

    keychord_bind(&add_menu.menu_keymap, "C-n",    add_menu_next,       "Next candidate",     PRESS | REPEAT);
    keychord_bind(&add_menu.menu_keymap, "C-p",    add_menu_prev,       "Previous candidate", PRESS | REPEAT);
    keychord_bind(&add_menu.menu_keymap, "<down>", add_menu_next,       "Next candidate",     PRESS | REPEAT);
    keychord_bind(&add_menu.menu_keymap, "<up>",   add_menu_prev,       "Previous candidate", PRESS | REPEAT);
    keychord_bind(&add_menu.menu_keymap, "C-c n",  add_menu_last,       "Last candidate",     PRESS);
    keychord_bind(&add_menu.menu_keymap, "C-c p",  add_menu_first,      "First candidate",    PRESS);
    keychord_bind(&add_menu.menu_keymap, "RET",    add_menu_enter,       "Select",             PRESS);
    keychord_bind(&add_menu.menu_keymap, "C-f",    add_menu_smart_right, "Forward/Enter",      PRESS | REPEAT);
    keychord_bind(&add_menu.menu_keymap, "C-b",    add_menu_smart_left,  "Back/Left",          PRESS | REPEAT);
    keychord_bind(&add_menu.menu_keymap, "<right>",add_menu_smart_right, "Forward/Enter",      PRESS | REPEAT);
    keychord_bind(&add_menu.menu_keymap, "<left>", add_menu_smart_left,  "Back/Left",          PRESS | REPEAT);
    keychord_bind(&add_menu.menu_keymap, "C-g",    add_menu_close,       "Cancel",             PRESS);
    keychord_bind(&add_menu.menu_keymap, "ESC",    add_menu_close,       "Cancel",             PRESS);
    keychord_bind(&add_menu.menu_keymap, "DEL",    add_menu_backspace,   "Backspace",          PRESS | REPEAT);

    // Load Icons
    add_menu.icon_mesh        = texture_pool_add_svg(&context, "./assets/blender-icons/outliner_ob_mesh.svg", 20, 20);
    add_menu.icon_plane       = texture_pool_add_svg(&context, "./assets/blender-icons/mesh_plane.svg",       20, 20);
    add_menu.icon_cube        = texture_pool_add_svg(&context, "./assets/blender-icons/mesh_cube.svg",        20, 20);
    add_menu.icon_circle      = texture_pool_add_svg(&context, "./assets/blender-icons/mesh_circle.svg",      20, 20);
    add_menu.icon_uvsphere    = texture_pool_add_svg(&context, "./assets/blender-icons/mesh_uvsphere.svg",    20, 20);
    add_menu.icon_icosphere   = texture_pool_add_svg(&context, "./assets/blender-icons/mesh_icosphere.svg",   20, 20);
    add_menu.icon_cylinder    = texture_pool_add_svg(&context, "./assets/blender-icons/mesh_cylinder.svg",    20, 20);
    add_menu.icon_cone        = texture_pool_add_svg(&context, "./assets/blender-icons/mesh_cone.svg",        20, 20);
    add_menu.icon_torus       = texture_pool_add_svg(&context, "./assets/blender-icons/mesh_torus.svg",       20, 20);
    add_menu.icon_arrow_right = texture_pool_add_svg(&context, "./assets/icons/GuiTreeArrowRight.svg",        20, 20);

    add_menu.icon_collider_cat= texture_pool_add_svg(&context, "./assets/icons/KinematicCollision3D.svg",     20, 20);
    add_menu.icon_col_cube    = texture_pool_add_svg(&context, "./assets/blender-icons/cube.svg",             20, 20);
    add_menu.icon_col_sphere  = texture_pool_add_svg(&context, "./assets/blender-icons/sphere.svg",           20, 20);
    add_menu.icon_col_capsule = texture_pool_add_svg(&context, "./assets/blender-icons/mesh_capsule.svg",     20, 20);

    // Assign icons to the structure
    root_children[0].icon_id  = add_menu.icon_mesh;
    root_children[1].icon_id  = add_menu.icon_collider_cat;
    root_children[2].icon_id  = add_menu.icon_uvsphere; // Reusing uvsphere icon for SDF for now

    collider_children[0].icon_id = add_menu.icon_col_cube;
    collider_children[1].icon_id = add_menu.icon_col_sphere;
    collider_children[2].icon_id = add_menu.icon_col_capsule;
    mesh_children[0].icon_id  = add_menu.icon_plane;
    mesh_children[1].icon_id  = add_menu.icon_cube;
    mesh_children[2].icon_id  = add_menu.icon_circle;
    mesh_children[3].icon_id  = add_menu.icon_uvsphere;
    mesh_children[4].icon_id  = add_menu.icon_icosphere;
    mesh_children[5].icon_id  = add_menu.icon_cylinder;
    mesh_children[6].icon_id  = add_menu.icon_cone;
    mesh_children[7].icon_id  = add_menu.icon_torus;
}

void add_menu_cleanup(void) {
    keymap_free(&add_menu.menu_keymap);
}

void add_menu_open(double mx, double my) {
    if (add_menu.is_active) return;

    add_menu.is_active = true;
    add_menu.x = (float)mx;
    add_menu.y = (float)context.swapChainExtent.height - (float)my; // Vulkan coord flip

    add_menu.input[0] = '\0';
    add_menu.input_length = 0;
    add_menu.cursor = 0;

    add_menu.stack_depth = 0;
    add_menu.menu_stack[0] = &root_menu;
    add_menu.selected_index = 0;
    add_menu.scroll_offset = 0;

    // Swap Keymap
    extern KeyChordMap keymap;
    add_menu.saved_keymap = keymap;
    keymap = add_menu.menu_keymap;

    set_active_text_input(add_menu_insert_char);
    add_menu_filter();
}

void add_menu_close(void) {
    if (!add_menu.is_active) return;
    add_menu.is_active = false;

    // Restore Keymap
    extern KeyChordMap keymap;
    keymap = add_menu.saved_keymap;
    set_active_text_input(NULL);
}

bool add_menu_is_open(void) {
    return add_menu.is_active;
}

void add_menu_render(void) {
    if (!add_menu.is_active || !editor.font) return;

    float title_h = 32.0f;
    float item_h = editor.font->ascent - editor.font->descent + 12.0f;
    int render_count = add_menu.visible_count > 0 ? add_menu.visible_count : 1;
    float h = title_h + (render_count * item_h) + 8.0f;
    float w = add_menu.width;

    float sw = (float)context.swapChainExtent.width;
    float sh = (float)context.swapChainExtent.height;

    float px = add_menu.x;
    float py = add_menu.y - h;

    if (px + w > sw) px = sw - w - 10.0f;
    if (py < 10.0f) py = 10.0f;

    // Save rendered bounds for mouse hit-testing
    add_menu.box_x = px;
    add_menu.box_y = py;
    add_menu.box_w = w;
    add_menu.box_h = h;
    add_menu.title_h = title_h;
    add_menu.item_h = item_h;

    vec4 radii = {8.0f, 8.0f, 8.0f, 8.0f};

    // Shadow & Background
    Color shadow = {0.0f, 0.0f, 0.0f, 0.4f};
    exQuad2D((vec2){px + 6.0f, py - 6.0f}, (vec2){w, h}, radii, 0.0f, shadow, shadow);
    exQuad2D((vec2){px, py}, (vec2){w, h}, radii, 1.0f, CT.border, CT.bg);

    // Titlebar
    exQuad2D((vec2){px, py + h - title_h}, (vec2){w, title_h}, (vec4){radii[0], radii[1], 0.0f, 0.0f}, 0.0f, CT.bg_alt, CT.bg_alt);

    float text_y = py + h - title_h * 0.5f;

    // Dynamic Titlebar (Breadcrumbs or Search)
    if (add_menu.input_length == 0) {
        text(editor.font, add_menu.menu_stack[add_menu.stack_depth]->name, px + 14.0f, text_y, CT.text);
    } else {
        // Emacs-style input rendering
        float cx = px + 14.0f;
        float cursor_w = font_width(editor.font);
        double time_since_key = glfwGetTime() - add_menu.last_key_time;
        bool cursor_visible = (time_since_key < 0.5) || (fmod(time_since_key, 1.0) < 0.5);

        for (size_t i = 0; i <= add_menu.input_length; i++) {
            bool is_cursor = (i == add_menu.cursor);
            char c = (i < add_menu.input_length) ? add_menu.input[i] : '\0';

            if (is_cursor && cursor_visible) {
                exQuad2D((vec2){cx, py + h - title_h + 4.0f}, (vec2){cursor_w, title_h - 8.0f}, (vec4){2.0f, 2.0f, 2.0f, 2.0f}, 0.0f, CT.accent, CT.accent);
            }

            if (c != '\0') {
                Color col = (is_cursor && cursor_visible) ? CT.bg_deep : CT.prompt;
                float adv = character(editor.font, (uint32_t)c, cx, text_y, col);
                cx += adv > 0.0f ? adv : cursor_w;
            } else if (is_cursor) {
                break;
            }
        }
    }

    // Items
    float row_y = py + h - title_h - item_h * 0.5f - 4.0f;

    if (add_menu.visible_count == 0) {
        text(editor.font, "No matches", px + 14.0f, row_y, CT.text_dim);
        return;
    }

    for (int i = 0; i < add_menu.visible_count; i++) {
        MenuItem* item = add_menu.visible_items[i];
        bool is_selected = (i == add_menu.selected_index);

        if (is_selected) {
            exQuad2D((vec2){px + 4.0f, row_y - item_h * 0.5f}, (vec2){w - 8.0f, item_h}, (vec4){4.0f, 4.0f, 4.0f, 4.0f}, 0.0f, CT.vertico_current, CT.vertico_current);
        }

        bool is_collider = (item == &root_children[1] || item->action == action_col_cube || item->action == action_col_sphere || item->action == action_col_capsule);
        Color target_col = is_collider ? CT.collision : (is_selected ? CT.text : CT.text_dim);

        // Draw Icon
        float current_x = px + 14.0f;
        float icon_size = 20.0f;
        if (item->icon_id >= 0) {
            Texture2D* icon_tex = texture_pool_get(item->icon_id);
            if (icon_tex && icon_tex->loaded) {
                texture2D((vec2){current_x, row_y - 12.0f}, (vec2){icon_size, icon_size}, icon_tex, target_col);
            }
            current_x += icon_size + 8.0f;
        }

        // Draw Name
        text(editor.font, item->name, current_x, row_y, target_col);

        // Draw Submenu Arrow

        if (item->is_menu) {
            Texture2D* arrow_tex = texture_pool_get(add_menu.icon_arrow_right);
            if (arrow_tex && arrow_tex->loaded) {
                texture2D((vec2){px + w - 28.0f, row_y - 12.0f}, (vec2){icon_size, icon_size}, arrow_tex, is_selected ? CT.text : CT.text_dim);
            }
        }

        row_y -= item_h;
    }
}
