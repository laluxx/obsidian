/// scene — scene subsystem
#include "scene.h"
#include "renderer.h"

/// Implementation

static Scene s_default_scene;
Scene *scene = &s_default_scene;

static uint64_t s_scene_structure_epoch;
void scene_tree_bump_revision(SceneTree *tree) {
    if (++s_scene_structure_epoch == 0) ++s_scene_structure_epoch;
    tree->revision = s_scene_structure_epoch;
}

static uint64_t s_mesh_epoch, s_sdf_epoch, s_gltf_epoch, s_sdf_lifetime_epoch;
static void bump_sdf_lifetime(Scene *s) {
    if (++s_sdf_lifetime_epoch == 0) ++s_sdf_lifetime_epoch;
    s->sdf_lifetime_epoch = s_sdf_lifetime_epoch;
}
void scene_bump_mesh_revision(Scene *s) { if (++s_mesh_epoch == 0) ++s_mesh_epoch; s->mesh_revision = s_mesh_epoch; }
void scene_bump_sdf_revision(Scene *s) { if (++s_sdf_epoch == 0) ++s_sdf_epoch; s->sdf_revision = s_sdf_epoch; }
void scene_bump_gltf_revision(Scene *s) { if (++s_gltf_epoch == 0) ++s_gltf_epoch; s->gltf_revision = s_gltf_epoch; }

void scene_init(Scene *s) {
    bump_sdf_lifetime(s);
    scene_bump_mesh_revision(s);
    scene_bump_sdf_revision(s);
    scene_bump_gltf_revision(s);
    // Initialize the virtual root node at index 0
    scene_tree_bump_revision(&s->tree);
    s->tree.count = 0;
    s->tree.root  = 0;
    scene_tree_add_node(&s->tree, "Scene", -1, -1);
    meshes_init(&s->meshes);
    s->materials = (ObsMaterialLibrary){0};
    s->gltf_instances = NULL;
    s->gltf_instance_count = 0;
    s->gltf_instance_capacity = 0;

    s->sdfs = NULL;
    s->sdf_count = 0;
    s->sdf_capacity = 0;
    if (s == scene) flushSdfSSBO();
}

void omdl_scene_graph_destroy(OmdlSceneGraph *g) {
    if (!g) return;
    free(g->nodes);
    free(g->world_transforms);
    free(g->local_transforms);
    free(g->script_translations);
    free(g->topology_order);
    free(g->skins);
    free(g->skin_joints);
    free(g->skin_ibms);
    free(g->anims);
    free(g->channels);
    free(g->anim_floats);
    obs_skin_override_index_destroy(&g->bone_override_index);
    obs_animation_channel_index_destroy(&g->animation_channel_index);
    free(g);
}

void scene_release_texture_leases(Scene *s) {
    if (!s) return;
    for (size_t i = 0; i < s->gltf_instance_count; ++i)
        obs_cache_texture_lease_release(&s->gltf_instances[i].texture_lease);
}

void scene_gltf_instance_destroy(GLTFInstance *instance) {
    if (!instance) return;
    obs_cache_texture_lease_release(&instance->texture_lease);
    if (instance->animations) {
        for (size_t a = 0; a < instance->animation_count; ++a) {
            Animation *anim = &instance->animations[a];
            free(anim->name);
            for (size_t c = 0; c < anim->channel_count; ++c) {
                AnimationChannel *channel = &anim->channels[c];
                free(channel->times);
                free(channel->translations);
                free(channel->rotations);
                free(channel->scales);
                free(channel->weights);
            }
            free(anim->channels);
        }
        free(instance->animations);
    }
    omdl_scene_graph_destroy(instance->gltf_data);
    free(instance->source_path);
    *instance = (GLTFInstance){0};
}

void scene_cleanup(Scene *s) {
    bump_sdf_lifetime(s);
    scene_bump_mesh_revision(s);
    scene_bump_sdf_revision(s);
    scene_bump_gltf_revision(s);
    scene_tree_bump_revision(&s->tree);
    meshes_destroy(context.device, &s->meshes);
    obs_material_library_destroy(&s->materials);

    if (s->sdfs) {
        free(s->sdfs);
        s->sdfs = NULL;
    }
    s->sdf_count = 0;
    s->sdf_capacity = 0;
    if (s == scene) flushSdfSSBO();

    for (size_t i = 0; i < s->gltf_instance_count; i++) {
        scene_gltf_instance_destroy(&s->gltf_instances[i]);
    }

    if (s->gltf_instances) {
        free(s->gltf_instances);
        s->gltf_instances = NULL;
    }

    s->gltf_instance_count = 0;
    s->gltf_instance_capacity = 0;
}
