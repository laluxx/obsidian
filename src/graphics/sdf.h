/// sdf — graphics subsystem
#pragma once
#include <stdbool.h>
#include <stdint.h>

void flushSdfSSBO(void);
/* Caller guarantees frame-slot fence completion; no device-wide idle. */
bool obs_sdf_sync_frame(uint32_t slot);
void sdfs_add(Scene* s, const char* name, int type, vec3 pos, vec4 size, vec4 color);

/// Public interface
