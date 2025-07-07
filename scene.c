#include "scene.h"
#include "renderer.h"

Scene scene = {0};

void scene_init(Scene *s) {
    meshes_init(&s->meshes);
}
