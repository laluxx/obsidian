/// bindings — monad subsystem
#ifndef OBSIDIAN_MONAD_BINDINGS_H
#define OBSIDIAN_MONAD_BINDINGS_H
#include <stdbool.h>
#include <monad/embed.h>
typedef struct ObsMonadRuntime ObsMonadRuntime;
/* Runtime defined in runtime.h, keep this header private to the subsystem. */
#include "runtime.h"
#include "command_catalog.h"

/// Public interface
bool obs_monad_on_owner_thread(void);
void obs_monad_unregister_key_dispatch(void);
/* Invalidates script-owned key closures while preserving native dispatcher. */
void obs_monad_discard_live_bindings(void);
bool obs_monad_register_bindings(ObsMonadRuntime *host, monad_error_t *error);
/* Module load transaction; never mutate existing callbacks until publication. */
bool obs_monad_binding_begin_publication(void);
void obs_monad_binding_finish_publication(bool published);
void obs_monad_binding_abort_publication(void);
bool obs_monad_binding_staging(void);
bool obs_monad_binding_stage_frame(monad_value_t *procedure);
size_t obs_monad_interactive_command_count(void);
const ObsInteractiveCommand *obs_monad_interactive_command_at(size_t index);
bool obs_monad_interactive_command_is(const void *entry);
void obs_monad_invoke_interactive_command(const ObsInteractiveCommand *entry);
void obs_monad_invoke_interactive_command_text(
    const ObsInteractiveCommand *entry, const char *const *text_args,
    size_t text_count);
bool obs_monad_live_command_catalog_begin(void);
bool obs_monad_live_command_register(const char *name,
                                     monad_value_t *procedure);
void obs_monad_live_command_catalog_finish(bool commit);
void obs_monad_live_command_catalog_clear(void);
#endif
