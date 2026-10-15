/// obj — assets subsystem
#pragma once

#include "renderer.h"
#include "scene.h"
#include <cglm/cglm.h>
#include <vulkan/vulkan.h>

/// Public interface

/* Mesh load_obj(const char* path, vec4 color); */
Mesh load_obj(const char* path, char* name, vec4 color);

typedef struct ObsObjAsync ObsObjAsync;
typedef enum {
    OBS_OBJ_ASYNC_PENDING,
    OBS_OBJ_ASYNC_PUBLISHED,
    OBS_OBJ_ASYNC_FAILED
} ObsObjAsyncStatus;

/* CPU parsing runs on a worker; poll commits prepared vertices on the
 * renderer-owning thread. One request may be in flight per owner. */
ObsObjAsync *obs_obj_async_create(void);
bool obs_obj_async_request(ObsObjAsync *owner, Scene *target,
                           const char *path, const vec4 color);
ObsObjAsyncStatus obs_obj_async_poll(ObsObjAsync *owner,
                                     int32_t *mesh_index);
void obs_obj_async_destroy(ObsObjAsync *owner);
