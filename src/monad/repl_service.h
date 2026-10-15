/// repl_service.h — Engine-owned REPL client of Monad embedding
#ifndef OBSIDIAN_MONAD_REPL_SERVICE_H
#define OBSIDIAN_MONAD_REPL_SERVICE_H
#include "repl_model.h"
#include "runtime.h"

/* UI and compiler share a single owned model; no GPU work runs in callbacks. */
ObsReplModel *obs_monad_repl_model(void);
void obs_monad_repl_pump(ObsMonadRuntime *host);
void obs_monad_repl_reset(void);
#endif
