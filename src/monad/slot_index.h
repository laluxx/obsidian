/// slot_index.h — Constant-time validation of densely stored engine foreign slots
#ifndef OBSIDIAN_MONAD_SLOT_INDEX_H
#define OBSIDIAN_MONAD_SLOT_INDEX_H
#include <stdbool.h>
#include <stddef.h>

/// Checked slot resolution
/* ONLY call after MonadC has checked nominal type and structural epoch, and
 * after the owning subsystem has checked count <= capacity. This answers
 * whether an opaque address identifies the *start* of a live array element.
 * It never dereferences the candidate or performs out-of-array C subtraction.
 * Engine storage remains C-owned and must stay stable during this call. */
bool obs_slot_index(const void *base, size_t element_size, size_t count,
                    const void *candidate, size_t *index);
#endif
