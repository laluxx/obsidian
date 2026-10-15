/// command_catalog.c — native command inventory contract
#include "command_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct monad_value { int references; };

void monad_value_retain(monad_value_t *value) {
    assert(value && value->references > 0);
    ++value->references;
}

void monad_value_release(monad_value_t *value) {
    if (!value) return;
    assert(value->references > 1);
    --value->references;
}

int main(void) {
    ObsCommandCatalog catalog = {0};
    monad_value_t first = {.references = 1};
    monad_value_t replacement = {.references = 1};
    assert(obs_command_catalog_register(&catalog, "move-node", "p", &first));
    assert(first.references == 2 && catalog.count == 1);
    const ObsInteractiveCommand *entry = obs_command_catalog_find(
        &catalog, "move-node");
    assert(entry && strcmp(entry->interactive_spec, "p") == 0);
    assert(obs_command_catalog_contains(&catalog, entry));
    assert(!obs_command_catalog_find(&catalog, "missing"));
    assert(obs_command_catalog_register(&catalog, "move-node", "n: ", &replacement));
    assert(catalog.count == 1 && first.references == 1);
    entry = obs_command_catalog_at(&catalog, 0);
    assert(entry && entry->procedure == &replacement);
    assert(strcmp(entry->interactive_spec, "n: ") == 0);
    assert(!obs_command_catalog_register(&catalog, "", "", &first));
    assert(!obs_command_catalog_register(&catalog, "bad", "p", NULL));
    obs_command_catalog_clear(&catalog);
    assert(catalog.count == 0 && replacement.references == 1);
    puts("command_catalog: discovery, metadata replacement, and release PASS");
    return 0;
}
