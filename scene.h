#pragma once
#include "renderer.h"
#include "cgltf.h"

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

typedef struct {
    Animation* animations;
    size_t animation_count;
    cgltf_data* gltf_data;
    size_t mesh_start_index;  // First mesh from this glTF
    size_t mesh_count;        // Number of meshes from this glTF
} GLTFInstance;

// A node in the scene tree (UI-facing hierarchy, not the GPU mesh array)
// Nodes are stored in a flat array; children/siblings are linked by index.
// -1 means "no entry".
#define SCENE_TREE_MAX_NODES 4096

typedef struct {
    char     name[128];       // Display name (glTF filename or mesh name)
    int32_t  parent;          // Index into SceneTree.nodes, -1 = root
    int32_t  first_child;     // Index of first child node, -1 = leaf
    int32_t  next_sibling;    // Index of next sibling, -1 = last child
    int32_t  mesh_index;      // Index into scene.meshes.items, -1 = group node
    bool     expanded;        // UI state: is this node expanded in the tree?
    bool     visible;         // UI state: is this node visible in viewport?
} SceneNode;

typedef struct {
    SceneNode nodes[SCENE_TREE_MAX_NODES];
    int32_t   count;
    int32_t   root;           // Index of the virtual root node (always index 0)
} SceneTree;

typedef struct {
    Meshes meshes;
    GLTFInstance* gltf_instances;
    size_t gltf_instance_count;
    size_t gltf_instance_capacity;
    SceneTree tree;           // Parallel hierarchy for UI rendering
} Scene;

extern Scene scene;
void scene_init(Scene *s);
void scene_cleanup(Scene *s);
void print_scene_meshes();

/// Scene tree API

// Adds a node and returns its index. parent_idx = -1 attaches to the virtual root.
int32_t scene_tree_add_node(SceneTree* tree, const char* name, int32_t parent_idx, int32_t mesh_index);
// Called once per load_gltf() — creates one group node for the file and one leaf per mesh.
void    scene_tree_register_gltf(Scene* s, GLTFInstance* inst, const char* filepath);
