/// glTF material field semantics shared by synchronous and asynchronous import.
#pragma once

typedef struct cgltf_material cgltf_material;

float obs_gltf_occlusion_strength(const cgltf_material *material);
