/// command_catalog.c — monad subsystem
#include "command_catalog.h"
#include <string.h>

bool obs_command_catalog_register(ObsCommandCatalog *catalog,
                                  const char *name, const char *spec,
                                  monad_value_t *procedure) {
    if (!catalog || !name || !*name || !spec || !procedure ||
        strlen(name) >= OBS_COMMAND_NAME_MAX ||
        strlen(spec) >= OBS_COMMAND_SPEC_MAX)
        return false;
    ObsInteractiveCommand *entry = NULL;
    for (size_t i = 0; i < catalog->count; ++i) {
        if (strcmp(catalog->entries[i].name, name) == 0) {
            entry = &catalog->entries[i];
            break;
        }
    }
    if (!entry) {
        if (catalog->count == OBS_COMMAND_MAX) return false;
        entry = &catalog->entries[catalog->count++];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->name, name, strlen(name) + 1);
    }
    monad_value_retain(procedure);
    monad_value_t *previous = entry->procedure;
    memcpy(entry->interactive_spec, spec, strlen(spec) + 1);
    entry->procedure = procedure;
    monad_value_release(previous);
    return true;
}

void obs_command_catalog_clear(ObsCommandCatalog *catalog) {
    if (!catalog) return;
    for (size_t i = 0; i < catalog->count; ++i) {
        monad_value_release(catalog->entries[i].procedure);
        memset(&catalog->entries[i], 0, sizeof(catalog->entries[i]));
    }
    catalog->count = 0;
}

const ObsInteractiveCommand *obs_command_catalog_at(
    const ObsCommandCatalog *catalog, size_t index) {
    return catalog && index < catalog->count ? &catalog->entries[index] : NULL;
}

const ObsInteractiveCommand *obs_command_catalog_find(
    const ObsCommandCatalog *catalog, const char *name) {
    if (!catalog || !name) return NULL;
    for (size_t i = 0; i < catalog->count; ++i)
        if (strcmp(catalog->entries[i].name, name) == 0)
            return &catalog->entries[i];
    return NULL;
}

bool obs_command_catalog_contains(const ObsCommandCatalog *catalog,
                                  const void *entry) {
    if (!catalog || !entry) return false;
    for (size_t i = 0; i < catalog->count; ++i)
        if (&catalog->entries[i] == entry)
            return true;
    return false;
}
