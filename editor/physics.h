#pragma once
#include "renderer.h"

void physics_init(void);
void physics_cleanup(void);
void physics_step(float dt);
void physics_rebuild_mesh(Mesh* m);
void physics_destroy_body(Mesh* m);
void physics_set_transform(Mesh* m);



