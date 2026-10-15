/// glTF material field semantics shared by synchronous and asynchronous import.
#include "cgltf.h"
#include "gltf_material.h"

float obs_gltf_occlusion_strength(const cgltf_material *material) {
    return material && material->occlusion_texture.texture
               ? material->occlusion_texture.scale
               : 1.0f;
}
