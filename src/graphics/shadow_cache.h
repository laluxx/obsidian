#pragma once

#include "renderer.h"
#include "scene.h"
#include <stdbool.h>
#include <stdint.h>

bool obs_shadow_cache_build_key(const Scene *scene,
                                const LightingData *lighting,
                                bool shadows_enabled,
                                uint32_t shadow_draw_count,
                                uint64_t *out_key);
