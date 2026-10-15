/// command_catalog — monad subsystem
#ifndef OBSIDIAN_COMMAND_CATALOG_H
#define OBSIDIAN_COMMAND_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <monad/embed.h>

#define OBS_COMMAND_MAX 1024
#define OBS_COMMAND_NAME_MAX 96
#define OBS_COMMAND_SPEC_MAX 128

typedef struct {
    char name[OBS_COMMAND_NAME_MAX];
    char interactive_spec[OBS_COMMAND_SPEC_MAX];
    monad_value_t *procedure;
} ObsInteractiveCommand;

typedef struct {
    ObsInteractiveCommand entries[OBS_COMMAND_MAX];
    size_t count;
} ObsCommandCatalog;

bool obs_command_catalog_register(ObsCommandCatalog *catalog,
                                  const char *name, const char *spec,
                                  monad_value_t *procedure);
void obs_command_catalog_clear(ObsCommandCatalog *catalog);
const ObsInteractiveCommand *obs_command_catalog_at(
    const ObsCommandCatalog *catalog, size_t index);
const ObsInteractiveCommand *obs_command_catalog_find(
    const ObsCommandCatalog *catalog, const char *name);
bool obs_command_catalog_contains(const ObsCommandCatalog *catalog,
                                  const void *entry);

#endif
