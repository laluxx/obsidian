#pragma once
#include "renderer.h"
#include "cgltf.h"
#include "object_identity.h"
#include "material_assets.h"
#include "cache_textures.h"
#include "skin_override_index.h"
#include "animation_channel_index.h"

/// Public interface

typedef struct {
    cgltf_node* target_node;
    cgltf_animation_path_type path;
    size_t keyframe_count;
    float* times;
    vec3* translations;
    versor* rotations;
    vec3* scales;
    float* weights;   // For morph
} AnimationChannel;

typedef struct {
    char* name;
    float duration;
    size_t channel_count;
    AnimationChannel* channels;
} Animation;

// AAA Data-Oriented Flat Node Hierarchy
typedef struct {
    char name[64];
    int32_t parent;     // -1 if root
    vec3 translation;
    versor rotation;
    vec3 scale;
    mat4 base_matrix;   // Pre-computed fallback for static/matrix-only nodes
    int32_t mesh_idx;   // Relative to instance->mesh_start_index
    int32_t skin_idx;
    bool expanded;      // UI State
} OmdlNode;

typedef struct {
    uint32_t joints_count;
    uint32_t joints_offset; // Index into a flat uint32_t array
    uint32_t ibm_offset;    // Index into a flat mat4 array
} OmdlSkin;

typedef struct OmdlChannel {
    uint32_t target_node; // Index into OmdlNode array
    uint32_t path_type;   // 0=T, 1=R, 2=S, 3=W
    uint32_t keyframe_count;
    uint32_t times_offset;
    uint32_t values_offset;
} OmdlChannel;

typedef struct OmdlAnimation {
    char name[64];
    float duration;
    uint32_t channel_count;
    uint32_t channel_offset;
} OmdlAnimation;

// The monolithic runtime struct that replaces cgltf_data
typedef struct OmdlSceneGraph {
    uint32_t node_count;
    OmdlNode* nodes;
    mat4* world_transforms; // Pre-allocated scratchpad for fast hierarchy evaluation
    mat4* local_transforms; // Pre-allocated to avoid frame allocation overhead
    vec3* script_translations; // Runtime-only additive Monad node-local offsets
    uint32_t* topology_order; // Verified parent-first order, generated once per load

    uint32_t skin_count;
    OmdlSkin* skins;
    uint32_t* skin_joints;
    mat4* skin_ibms;

    uint32_t anim_count;
    OmdlAnimation* anims;
    OmdlChannel* channels;
    float* anim_floats; // Massive flat array holding ALL keyframes and times

    /* Runtime-only inverse map for mesh/joint overrides; never serialized. */
    ObsSkinOverrideIndex bone_override_index;
    bool bone_override_index_attempted;
    bool bone_override_index_ready;
    /* Runtime-only node-to-channel map; built lazily for large animations. */
    ObsAnimationChannelIndex animation_channel_index;
} OmdlSceneGraph;

typedef struct {
    Animation* animations;
    size_t animation_count;
    OmdlSceneGraph* gltf_data;
    char *source_path;          // owned source path for editor duplication
    size_t mesh_start_index;  // First mesh from this glTF
    size_t mesh_count;        // Number of meshes from this glTF
    mat4 instance_transform;  // Transform applied above every glTF scene root
    size_t texture_count;     // glTF texture resources retained by this instance
    /* Retained cache-hit texture references; released at scene teardown. */
    ObsCacheTextureLease texture_lease;
} GLTFInstance;

// A node in the scene tree (UI-facing hierarchy, not the GPU mesh array)
// Nodes are stored in a flat array; children/siblings are linked by index.
// -1 means "no entry".
#define SCENE_TREE_MAX_NODES 4096

typedef struct {
    uint64_t foreign_identity; /* preserved by compacting/moving nodes */
    char     name[128];       // Display name (glTF filename or mesh name)
    int32_t  parent;          // Index into SceneTree.nodes, -1 = root
    int32_t  first_child;     // Index of first child node, -1 = leaf
    int32_t  next_sibling;    // Index of next sibling, -1 = last child
    int32_t  mesh_index;      // Index into scene.meshes.items, -1 = group node
    int32_t  sdf_index;       // Index into scene.sdfs, -1 = not an SDF
    bool     expanded;        // UI state: is this node expanded in the tree?
    bool     visible;         // UI state: is this node visible in viewport?
    bool     selected_group;
    bool     selected;
} SceneNode;

typedef struct {
    SceneNode nodes[SCENE_TREE_MAX_NODES];
    /* Structural revision: lazily remaps stable node handles on mutation. */
    uint64_t revision;
    int32_t   count;
    int32_t   root;           // Index of the virtual root node (always index 0)
} SceneTree;

typedef struct {
    /* Registry pages persist across scene resets while Monad handles exist. */
    ObsIdentityRegistry foreign_objects;
    Meshes meshes;
    ObsMaterialLibrary materials;
    GLTFInstance* gltf_instances;
    size_t gltf_instance_count;
    size_t gltf_instance_capacity;
    SceneTree tree;           // Parallel hierarchy for UI rendering
    /* Foreign-object structural epochs; never reset to zero on scene reload. */
    uint64_t mesh_revision;
    uint64_t sdf_revision;
    /* Changes on scene init/cleanup, NOT ordinary SDF append/reallocation. */
    uint64_t sdf_lifetime_epoch;
    uint64_t gltf_revision;

    SdfPrimitive* sdfs;       // CPU-side flat array of SDFs
    uint32_t sdf_count;
    uint32_t sdf_capacity;
} Scene;

/* The active scene changes when an editor frame selects another scene buffer. */
extern Scene *scene;
/* Bump on structural mutations. Monotone for the running process, including
 * scene resets; do not derive revisions from uninitialized Scene memory. */
void scene_tree_bump_revision(SceneTree *tree);
void scene_bump_mesh_revision(Scene *s);
void scene_bump_sdf_revision(Scene *s);
void scene_bump_gltf_revision(Scene *s);
void scene_init(Scene *s);
void scene_cleanup(Scene *s);
/* Frees the native OMDL graph, not the temporary cgltf parser object. */
void scene_release_texture_leases(Scene *s);
void scene_gltf_instance_destroy(GLTFInstance *instance);
void omdl_scene_graph_destroy(OmdlSceneGraph *graph);

/* Checked append; returns assigned index or -1 without partial scene mutation. */
int32_t obs_sdf_add_checked(Scene *s, const char *name, int type,
                            vec3 pos, vec4 size, vec4 color);
void sdfs_add(Scene* s, const char* name, int type, vec3 pos, vec4 size, vec4 color);
void flushSdfSSBO(void);

/// Scene tree API

// Adds a node and returns its index. parent_idx = -1 attaches to the virtual root.
int32_t scene_tree_add_node(SceneTree* tree, const char* name, int32_t parent_idx, int32_t mesh_index);
bool    scene_tree_set_selected_visibility(SceneTree* tree, bool visible);
bool    scene_tree_collect_sdf_visibility(const SceneTree* tree,
                                          uint32_t sdf_count,
                                          bool visible[MAX_SDF_PRIMITIVES]);
// Called once per load_gltf() — creates one group node for the file and one leaf per mesh.
void    scene_tree_register_gltf(Scene* s, GLTFInstance* inst, const char* filepath);
