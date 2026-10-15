/// semantic_preflight.h — Thread-isolated Monad compiler advisory analysis
#ifndef OBSIDIAN_MONAD_SEMANTIC_PREFLIGHT_H
#define OBSIDIAN_MONAD_SEMANTIC_PREFLIGHT_H
#include "source_stage.h"

/// Worker-owned compiler integration
/* No API invokes the live monad_t or its object space. Semantic preflight is
 * advisory: it cannot prove native emission, QTT publication or game imports. */
ObsStagePreflightHooks obs_semantic_preflight_hooks(void);
#endif
