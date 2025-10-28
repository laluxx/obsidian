#include "scene.h"
#include "renderer.h"
#include "gltf_loader.h"

Scene scene = {0};

void scene_init(Scene *s) {
    meshes_init(&s->meshes);
    s->animations = NULL;
    s->animation_count = 0;
    s->gltf_data = NULL;
}

void scene_cleanup(Scene *s) {
    meshes_destroy(context.device, &s->meshes);
    free_gltf_animations(s);
}
