/// atomic_file — fail-closed, same-directory byte publication
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Replace PATH only after its complete bytes are written and file-synced.
 * Previous PATH remains untouched on error before rename. Creates a private
 * (0600) temporary sibling; does not promise atomicity across multiple paths
 * or power-loss durability of the containing directory entry. */
bool obs_atomic_write_bytes(const char *path, const void *data, size_t size);
