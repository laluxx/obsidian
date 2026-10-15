/// frame_api.h — Monad-facing frame capability and live frame commands
#ifndef OBSIDIAN_MONAD_FRAME_API_H
#define OBSIDIAN_MONAD_FRAME_API_H
#include "runtime.h"
/// Public interface
bool obs_monad_register_frame_api(ObsMonadRuntime *host, monad_error_t *error);
void obs_monad_frame_tick(void);
void obs_monad_frame_discard_callback(void);
/* Installs a retained callback after compiler publication. */
void obs_monad_frame_set_callback(monad_value_t *procedure);
#endif
