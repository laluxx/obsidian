/// keychords-monad — tests subsystem
#include "keychords.h"

#include <assert.h>
#include <string.h>

/// Implementation

static void c_command(void) {}
static int native_calls;
static void native_command(void) { native_calls++; }
static int native_guard_calls;
static void native_guard(KeyChordAction action) {
    native_guard_calls++;
    action();
}
static void callback_command(void *userdata) { (void)userdata; }

static monad_status_t return_nil(
    monad_object_space_t *space, size_t argc, monad_value_t *const *argv,
    monad_value_t **result, void *context, monad_error_t *error) {
    (void)argc;
    (void)argv;
    (void)context;
    (void)error;
    *result = monad_value_nil(space);
    return MONAD_OK;
}

int main(void) {
    monad_error_t error = {0};
    monad_runtime_config_t config;
    monad_runtime_t *runtime = NULL;
    monad_thread_t *thread = NULL;
    monad_object_space_t *space = NULL;

    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    assert(monad_object_space_create(thread, &space, &error) == MONAD_OK);

    monad_dynamic_procedure_descriptor_t descriptor;
    monad_dynamic_procedure_descriptor_init(&descriptor);
    descriptor.name = "keychord-test";
    descriptor.minimum_arity = 0;
    descriptor.maximum_arity = 0;
    descriptor.function = return_nil;

    monad_value_t *procedure = NULL;
    assert(monad_value_make_procedure(
        space, &descriptor, &procedure, &error) == MONAD_OK);

    KeyChordMap map;
    keymap_init(&map);

    assert(keychord_bind(&map, "a", c_command, "c", GLFW_PRESS));
    assert(keychord_bind_monad(
        &map, "a", procedure, "monad", GLFW_PRESS));
    assert(map.count == 1);
    KeyChordBinding *binding = keychord_find_binding(&map, "a");
    assert(binding && binding->action_type == ACTION_MONAD_PROC);
    assert(binding->action.monad_proc == procedure);

    assert(keychord_bind_native_named(
        &map, "a", native_command, procedure, "native-command",
        "native", GLFW_PRESS));
    assert(map.count == 1);
    binding = keychord_find_binding(&map, "a");
    assert(binding && binding->action_type == ACTION_C_FUNCTION);
    assert(binding->action.c_action == native_command);
    assert(binding->command_proc == procedure);
    assert(strcmp(keychord_binding_command_name(binding), "native-command") == 0);
    register_keychord_native_action_guard(native_guard);
    assert(keychord_process_key(&map, GLFW_KEY_A, GLFW_PRESS, 0));
    assert(native_calls == 1);
    assert(native_guard_calls == 1);

    assert(keychord_bind_callback(
        &map, "a", callback_command, NULL, NULL, "callback", GLFW_PRESS));
    assert(map.count == 1);
    binding = keychord_find_binding(&map, "a");
    assert(binding && binding->action_type == ACTION_CALLBACK);

    assert(keychord_bind_monad_named(
        &map, "a", procedure, "monad-again", "monad-again",
        GLFW_PRESS));
    assert(map.count == 1);
    binding = keychord_find_binding(&map, "a");
    assert(binding && binding->action_type == ACTION_MONAD_PROC);
    assert(strcmp(keychord_binding_command_name(binding), "monad-again") == 0);

    assert(keychord_unbind(&map, "a"));
    assert(map.count == 0);
    keymap_free(&map);

    monad_value_release(procedure);
    assert(monad_object_space_destroy(space, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    return 0;
}
