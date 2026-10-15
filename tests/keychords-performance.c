/// keychords-performance — tests subsystem
#define _POSIX_C_SOURCE 200809L
#include "keychords.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/// Implementation

static monad_object_space_t *bench_space;
static int native_calls;

static void native_command(void) { native_calls++; }
static void native_guard(KeyChordAction action) { action(); }

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

static monad_value_t *dynamic_command;

static void call_interactively(monad_value_t *procedure) {
    monad_value_t *result = NULL;
    monad_error_t error = {0};
    assert(procedure == dynamic_command);
    assert(monad_value_call(bench_space, procedure, 0, NULL, &result,
                            &error) == MONAD_OK);
    monad_value_release(result);
}

static uint64_t now_ns(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

int main(void) {
    enum { iterations = 100000 };
    monad_error_t error = {0};
    monad_runtime_config_t config;
    monad_runtime_t *runtime = NULL;
    monad_thread_t *thread = NULL;
    monad_value_t *procedure = NULL;

    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    assert(monad_object_space_create(thread, &bench_space, &error) == MONAD_OK);

    monad_dynamic_procedure_descriptor_t descriptor;
    monad_dynamic_procedure_descriptor_init(&descriptor);
    descriptor.name = "keychord-performance";
    descriptor.minimum_arity = 0;
    descriptor.maximum_arity = 0;
    descriptor.function = return_nil;
    assert(monad_value_make_procedure(
        bench_space, &descriptor, &procedure, &error) == MONAD_OK);
    dynamic_command = procedure;

    KeyChordMap native_map;
    keymap_init(&native_map);
    assert(keychord_bind_native_named(
        &native_map, "a", native_command, procedure,
        "keychord-performance", "native", GLFW_PRESS));
    register_keychord_native_action_guard(native_guard);

    uint64_t start = now_ns();
    for (int i = 0; i < iterations; ++i)
        assert(keychord_process_key(&native_map, GLFW_KEY_A, GLFW_PRESS, 0));
    uint64_t native_ns = now_ns() - start;
    assert(native_calls == iterations);

    KeyChordMap dynamic_map;
    keymap_init(&dynamic_map);
    assert(keychord_bind_monad(&dynamic_map, "a", procedure, "dynamic",
                               GLFW_PRESS));
    register_call_interactively(call_interactively);

    start = now_ns();
    for (int i = 0; i < iterations; ++i)
        assert(keychord_process_key(&dynamic_map, GLFW_KEY_A, GLFW_PRESS, 0));
    uint64_t dynamic_ns = now_ns() - start;

    printf("native=%llu ns dynamic=%llu ns ratio=%.2fx\n",
           (unsigned long long)native_ns,
           (unsigned long long)dynamic_ns,
           native_ns ? (double)dynamic_ns / (double)native_ns : 0.0);

    keymap_free(&dynamic_map);
    keymap_free(&native_map);
    monad_value_release(procedure);
    assert(monad_object_space_destroy(bench_space, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    return 0;
}
