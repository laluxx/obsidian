/// scene_api — monad subsystem
#ifndef OBSIDIAN_MONAD_SCENE_API_H
#define OBSIDIAN_MONAD_SCENE_API_H
#include "runtime.h"

/// Public interface
/* Registration is transactional at object-space lifetime granularity: a failed
 * registration aborts startup and destroys the space. */
bool obs_monad_register_scene_api(ObsMonadRuntime *host, monad_error_t *error);
#endif
