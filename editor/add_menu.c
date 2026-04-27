#include "add_menu.h"
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

static void spawn_mesh(const char* name) {
    Mesh m;
    memset(&m, 0, sizeof(Mesh));

    m.megaBaseVertex = UINT32_MAX;
    m.megaBaseIndex = UINT32_MAX;
    m.dynamicBaseVertex = UINT32_MAX;
    m.megaBaseMeshlet = UINT32_MAX;
    m.megaBaseMeshletVertex = UINT32_MAX;
    m.megaBaseMeshletTriangle = UINT32_MAX;

    glm_mat4_identity(m.local_transform);
    glm_mat4_identity(m.model);

    glm_vec3_copy((vec3){-1.0f, -1.0f, -1.0f}, m.aabbMin);
    glm_vec3_copy((vec3){ 1.0f,  1.0f,  1.0f}, m.aabbMax);

    m.name = strdup(name);
    m.visible = true;
    m.alpha_mode = 0; // Opaque
    m.baseColorFactor[0] = 1.0f; m.baseColorFactor[1] = 1.0f;
    m.baseColorFactor[2] = 1.0f; m.baseColorFactor[3] = 1.0f;
    m.roughnessFactor = 0.5f;
    m.metallicFactor = 0.0f;
    m.emissiveStrength = 1.0f;

    m.textureIndex = -1;
    m.normalMapIndex = -1;
    m.metallicRoughIndex = -1;
    m.aoIndex = -1;
    m.emissiveIndex = -1;
    m.transmissionIndex = -1;
    m.thicknessIndex = -1;
    m.jointOffset = -1;

    m.ior = 1.5f;
    m.attenuationDistance = 100000.0f;
    m.attenuationColor[0] = 1.0f;
    m.attenuationColor[1] = 1.0f;
    m.attenuationColor[2] = 1.0f;

    Vertex verts[1024];
    memset(verts, 0, sizeof(verts));
    uint32_t indices[6144];
    uint32_t v_count = 0, i_count = 0;

    if (strcmp(name, "Plane") == 0) {
        verts[0] = (Vertex){ .pos={-1,0, 1}, .normal={0,1,0}, .texCoord={0,1}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        verts[1] = (Vertex){ .pos={ 1,0, 1}, .normal={0,1,0}, .texCoord={1,1}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        verts[2] = (Vertex){ .pos={ 1,0,-1}, .normal={0,1,0}, .texCoord={1,0}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        verts[3] = (Vertex){ .pos={-1,0,-1}, .normal={0,1,0}, .texCoord={0,0}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        uint32_t ind[] = {0, 1, 2, 0, 2, 3};
        memcpy(indices, ind, sizeof(ind));
        v_count = 4; i_count = 6;
    } else if (strcmp(name, "Cube") == 0) {
        vec3 norms[6] = {{0,0,1}, {0,0,-1}, {-1,0,0}, {1,0,0}, {0,1,0}, {0,-1,0}};
        vec4 tangs[6] = {{1,0,0,1}, {-1,0,0,1}, {0,0,1,1}, {0,0,-1,1}, {1,0,0,1}, {1,0,0,1}};
        vec3 pos[6][4] = {
            {{-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}},
            {{ 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1}},
            {{-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1}},
            {{ 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1}},
            {{-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1}},
            {{-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1}}
        };
        vec2 uvs[4] = {{0,1}, {1,1}, {1,0}, {0,0}};
        for (int f = 0; f < 6; f++) {
            for (int v = 0; v < 4; v++) {
                verts[v_count] = (Vertex){ .color={1,1,1,1} };
                glm_vec3_copy(pos[f][v], verts[v_count].pos);
                glm_vec3_copy(norms[f], verts[v_count].normal);
                glm_vec2_copy(uvs[v], verts[v_count].texCoord);
                glm_vec4_copy(tangs[f], verts[v_count].tangent);
                v_count++;
            }
            indices[i_count++] = f*4 + 0; indices[i_count++] = f*4 + 1; indices[i_count++] = f*4 + 2;
            indices[i_count++] = f*4 + 0; indices[i_count++] = f*4 + 2; indices[i_count++] = f*4 + 3;
        }
    } else if (strcmp(name, "Circle") == 0) {
        int segs = 32;
        verts[0] = (Vertex){ .pos={0,0,0}, .normal={0,1,0}, .texCoord={0.5f,0.5f}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        v_count = 1;
        for(int i=0; i<=segs; i++) {
            float a = (float)i / segs * 6.2831853f;
            float px = cosf(a), pz = sinf(a);
            verts[v_count] = (Vertex){ .pos={px, 0, pz}, .normal={0,1,0}, .texCoord={(px+1.0f)*0.5f, (pz+1.0f)*0.5f}, .color={1,1,1,1}, .tangent={1,0,0,1} };
            v_count++;
        }
        for(int i=1; i<=segs; i++) {
            indices[i_count++] = 0; indices[i_count++] = i + 1; indices[i_count++] = i;
        }
    } else if (strcmp(name, "Cylinder") == 0) {
        int segs = 32;
        for(int i=0; i<=segs; i++) {
            float u = (float)i / segs;
            float a = u * 6.2831853f;
            float px = cosf(a), pz = sinf(a);
            verts[v_count] = (Vertex){ .pos={px, -1, pz}, .normal={px,0,pz}, .texCoord={u, 1}, .color={1,1,1,1}, .tangent={-pz,0,px,1} };
            verts[v_count+1] = (Vertex){ .pos={px,  1, pz}, .normal={px,0,pz}, .texCoord={u, 0}, .color={1,1,1,1}, .tangent={-pz,0,px,1} };
            v_count += 2;
        }
        for(int i=0; i<segs; i++) {
            uint32_t b = i * 2;
            indices[i_count++] = b; indices[i_count++] = b+1; indices[i_count++] = b+2;
            indices[i_count++] = b+1; indices[i_count++] = b+3; indices[i_count++] = b+2;
        }
        uint32_t t_center = v_count;
        verts[v_count++] = (Vertex){ .pos={0,1,0}, .normal={0,1,0}, .texCoord={0.5f,0.5f}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        for(int i=0; i<=segs; i++) {
            float a = (float)i / segs * 6.2831853f;
            float px = cosf(a), pz = sinf(a);
            verts[v_count++] = (Vertex){ .pos={px, 1, pz}, .normal={0,1,0}, .texCoord={(px+1)*0.5f, (pz+1)*0.5f}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        }
        for(int i=0; i<segs; i++) {
            indices[i_count++] = t_center; indices[i_count++] = t_center + 2 + i; indices[i_count++] = t_center + 1 + i;
        }
        uint32_t b_center = v_count;
        verts[v_count++] = (Vertex){ .pos={0,-1,0}, .normal={0,-1,0}, .texCoord={0.5f,0.5f}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        for(int i=0; i<=segs; i++) {
            float a = (float)i / segs * 6.2831853f;
            float px = cosf(a), pz = sinf(a);
            verts[v_count++] = (Vertex){ .pos={px, -1, pz}, .normal={0,-1,0}, .texCoord={(px+1)*0.5f, (1-pz)*0.5f}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        }
        for(int i=0; i<segs; i++) {
            indices[i_count++] = b_center; indices[i_count++] = b_center + 1 + i; indices[i_count++] = b_center + 2 + i;
        }
    } else if (strcmp(name, "Cone") == 0) {
        int segs = 32;
        for(int i=0; i<=segs; i++) {
            float u = (float)i / segs;
            float a = u * 6.2831853f;
            float px = cosf(a), pz = sinf(a);
            vec3 n = {px, 0.5f, pz}; glm_vec3_normalize(n);
            verts[v_count] = (Vertex){ .pos={px, -1, pz}, .normal={n[0],n[1],n[2]}, .texCoord={u, 1}, .color={1,1,1,1}, .tangent={-pz,0,px,1} };
            verts[v_count+1] = (Vertex){ .pos={0, 1, 0}, .normal={n[0],n[1],n[2]}, .texCoord={u, 0}, .color={1,1,1,1}, .tangent={-pz,0,px,1} };
            v_count += 2;
        }
        for(int i=0; i<segs; i++) {
            uint32_t b = i * 2;
            indices[i_count++] = b; indices[i_count++] = b+1; indices[i_count++] = b+2;
        }
        uint32_t b_center = v_count;
        verts[v_count++] = (Vertex){ .pos={0,-1,0}, .normal={0,-1,0}, .texCoord={0.5f,0.5f}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        for(int i=0; i<=segs; i++) {
            float a = (float)i / segs * 6.2831853f;
            float px = cosf(a), pz = sinf(a);
            verts[v_count++] = (Vertex){ .pos={px, -1, pz}, .normal={0,-1,0}, .texCoord={(px+1)*0.5f, (1-pz)*0.5f}, .color={1,1,1,1}, .tangent={1,0,0,1} };
        }
        for(int i=0; i<segs; i++) {
            indices[i_count++] = b_center; indices[i_count++] = b_center + 2 + i; indices[i_count++] = b_center + 1 + i;
        }
    } else if (strcmp(name, "UV Sphere") == 0 || strcmp(name, "Ico Sphere") == 0) {
        int lats = 12, lons = 12;
        for(int i=0; i<=lats; i++) {
            float v = (float)i / lats;
            float phi = v * 3.14159265f;
            for(int j=0; j<=lons; j++) {
                float u = (float)j / lons;
                float theta = u * 6.2831853f;
                float px = sinf(phi) * cosf(theta);
                float py = cosf(phi);
                float pz = sinf(phi) * sinf(theta);
                verts[v_count] = (Vertex){ .pos={px,py,pz}, .normal={px,py,pz}, .texCoord={u, v}, .color={1,1,1,1}, .tangent={-sinf(theta),0,cosf(theta),1} };
                v_count++;
            }
        }
        for(int i=0; i<lats; i++) {
            for(int j=0; j<lons; j++) {
                uint32_t p0 = i*(lons+1) + j;
                uint32_t p1 = p0 + 1;
                uint32_t p2 = (i+1)*(lons+1) + j;
                uint32_t p3 = p2 + 1;
                indices[i_count++] = p0; indices[i_count++] = p1; indices[i_count++] = p2;
                indices[i_count++] = p1; indices[i_count++] = p3; indices[i_count++] = p2;
            }
        }
    } else if (strcmp(name, "Torus") == 0) {
        int segs1 = 16, segs2 = 12;
        for(int i=0; i<=segs1; i++) {
            float u = (float)i / segs1;
            float a1 = u * 6.2831853f;
            float cos1 = cosf(a1), sin1 = sinf(a1);
            for(int j=0; j<=segs2; j++) {
                float v = (float)j / segs2;
                float a2 = v * 6.2831853f;
                float cos2 = cosf(a2), sin2 = sinf(a2);
                float r = 1.0f + 0.3f * cos2;
                float px = r * cos1, py = 0.3f * sin2, pz = r * sin1;
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

    // We no longer generate fake meshlets for spawned primitives!
    // They will seamlessly fall back to the legacy pipeline (pbr.vert),
    // which is perfectly designed for dynamic modeling and geometry editing.
    m.megaBaseMeshlet = UINT32_MAX;
    m.megaBaseMeshletVertex = UINT32_MAX;
    m.megaBaseMeshletTriangle = UINT32_MAX;
    m.meshletCount = 0;

    m.megaBaseVertex = megaBufferAllocate(&context, verts, v_count);
    m.megaBaseIndex = megaIndexBufferAllocate(&context, indices, i_count);
    m.vertexCount = v_count;
    m.indexCount = i_count;

    // CRITICAL: Force the CPU staging buffer to actually DMA copy to the GPU VRAM instantly!
    flushUploadStagingBuffer(&context);

    int new_idx = (int)scene.meshes.count;

    // Initialize root if missing
    if (scene.tree.count == 0) {
        memset(&scene.tree.nodes[0], 0, sizeof(SceneNode));
        strcpy((char*)scene.tree.nodes[0].name, "World");
        scene.tree.nodes[0].first_child = -1;
        scene.tree.nodes[0].next_sibling = -1;
        scene.tree.nodes[0].parent = -1;
        scene.tree.nodes[0].visible = true;
        scene.tree.nodes[0].expanded = true;
        scene.tree.count = 1;
        scene.tree.root = 0;
    }

    // Use the official API to attach the node safely
    int node_idx = scene_tree_add_node(&scene.tree, name, 0, new_idx);
    m.node = (void*)(uintptr_t)node_idx;

    meshes_add(&scene.meshes, m);

    // Add a Material child node so the inspector can isolate material view,
    // identical to what the GLTF loader does for every mesh it imports.
    scene_tree_add_node(&scene.tree, "Material", node_idx, new_idx);

    // Auto-select in the inspector to update SSBO/indirect buffers cleanly
    inspector_select_mesh(new_idx);
    markMeshesSSBODirty(&context);

    extern bool scene_topology_dirty;
    scene_topology_dirty = true;
}

static void action_plane    () { spawn_mesh("Plane"     ); }
static void action_cube     () { spawn_mesh("Cube"      ); }
static void action_circle   () { spawn_mesh("Circle"    ); }
static void action_uvsphere () { spawn_mesh("UV Sphere" ); }
static void action_icosphere() { spawn_mesh("Ico Sphere"); }
static void action_cylinder () { spawn_mesh("Cylinder"  ); }
static void action_cone     () { spawn_mesh("Cone"      ); }

extern void physics_rebuild_mesh(Mesh* m);

static void add_collider_to_selected(int type, const char* name) {
    (void)name;
    if (editor.inspector.selected_mesh_index >= 0 && editor.inspector.selected_mesh_index < (int)scene.meshes.count) {
        Mesh* m = &scene.meshes.items[editor.inspector.selected_mesh_index];
        m->collider_type = type;
        physics_rebuild_mesh(m);
        // The Collider node is synthetic — generated at DFS render time from
        // m->collider_type > 0, exactly like Skeleton. We never create a real
        // scene tree node for it; doing so causes duplicate display and
        // multi-selection corruption.
        extern bool scene_topology_dirty;
        scene_topology_dirty = true;
    }
}

static void action_col_cube()    { add_collider_to_selected(1, "Cube Collider"); }
static void action_col_sphere()  { add_collider_to_selected(2, "Sphere Collider"); }
static void action_col_capsule() { add_collider_to_selected(3, "Capsule Collider"); }

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
    spawn_mesh("Torus");

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

static MenuItem root_children[] = {
    {"Mesh", -1, true, mesh_children, sizeof(mesh_children)/sizeof(MenuItem), NULL},
    {"Collider", -1, true, collider_children, sizeof(collider_children)/sizeof(MenuItem), NULL}
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
static void flatten_search_recursive(MenuItem* node, const char* query) {
    if (add_menu.visible_count >= ADD_MENU_MAX_RESULTS) return;

    if (node == &root_children[1] && editor.inspector.selected_mesh_index == -1) return;

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

    if (add_menu.input_length == 0) {
        // Show current directory in hierarchy
        MenuItem* current = add_menu.menu_stack[add_menu.stack_depth];
        for (int i = 0; i < current->child_count && i < ADD_MENU_MAX_RESULTS; i++) {
            if (&current->children[i] == &root_children[1] && editor.inspector.selected_mesh_index == -1) continue;
            add_menu.visible_items[add_menu.visible_count++] = &current->children[i];
        }
    } else {
        // Deep search all leaves
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
