/// keymap_image.c — Real keymap prepared-image and parser regression
#define _POSIX_C_SOURCE 200809L
#include "keychords.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

TextCallback currentTextCallback = NULL;

/// Monad object-space test double (no runtime/JIT claims)
struct monad_value {
  int references;
};
static long retains, releases;
static uint64_t glfw_time_calls;
static int watch_allocations;
static size_t watched_allocations;
static size_t watched_allocation_bytes;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
char *__real_strdup(const char *text);
void *__wrap_malloc(size_t size) {
  if (watch_allocations) {
    ++watched_allocations;
    watched_allocation_bytes += size;
  }
  return __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
  if (watch_allocations) {
    ++watched_allocations;
    watched_allocation_bytes += count * size;
  }
  return __real_calloc(count, size);
}
void *__wrap_realloc(void *ptr, size_t size) {
  if (watch_allocations) {
    ++watched_allocations;
    watched_allocation_bytes += size;
  }
  return __real_realloc(ptr, size);
}
char *__wrap_strdup(const char *text) {
  if (watch_allocations) {
    ++watched_allocations;
    watched_allocation_bytes += strlen(text) + 1;
  }
  return __real_strdup(text);
}
void monad_value_retain(monad_value_t *p) {
  assert(p && p->references > 0);
  ++p->references;
  ++retains;
}
void monad_value_release(monad_value_t *p) {
  assert(p && p->references > 1);
  --p->references;
  ++releases;
}
double glfwGetTime(void) {
  ++glfw_time_calls;
  return 0.0;
}
static void native_action(void) {}

/// Parser must handle complete chords without truncation
static void parser(void) {
  KeyChord chord = {0};
  assert(parse_keychord_notation("C-x C-s", &chord) && chord.length == 2);
  assert(parse_keychord_notation("C-x M-z <up> SPC", &chord) &&
         chord.length == 4);
  assert(!parse_keychord_notation("C-x M-z <up> SPC x", &chord));
  assert(!parse_keychord_notation("", &chord));
  assert(!parse_keychord_notation("C-x ???", &chord));
  assert(parse_keychord_notation("  C-x   C-s  ", &chord) && chord.length == 2);
  char long_token[256];
  memset(long_token, 'a', sizeof(long_token));
  long_token[255] = 0;
  assert(!parse_keychord_notation(long_token, &chord));
}

/// Active key help must reflect override/local precedence, without shadowed
/// chords.
static void active_bindings(void) {
  keymap_init(&keymap);
  KeyChordMap local = {0}, overriding = {0};
  keymap_init(&local);
  keymap_init(&overriding);
  assert(keychord_bind(&keymap, "C-x", native_action, "global x", GLFW_PRESS));
  assert(
      keychord_bind(&keymap, "C-g", native_action, "global quit", GLFW_PRESS));
  assert(keychord_bind(&local, "C-x", native_action, "local x", GLFW_PRESS));
  assert(keychord_bind(&local, "C-l", native_action, "local help", GLFW_PRESS));
  keymap_stack_push(&local);

  assert(keychord_process_key_with_stack(GLFW_KEY_U, GLFW_PRESS,
                                         GLFW_MOD_CONTROL));
  assert(keychord_prefix_arg_is_set() && keychord_prefix_arg_value() == 4);
  assert(keychord_process_key_with_stack(GLFW_KEY_3, GLFW_PRESS, 0));
  assert(keychord_prefix_arg_value() == 3);
  keychord_clear_prefix_arg();
  assert(keychord_process_key_with_stack(GLFW_KEY_MINUS, GLFW_PRESS,
                                         GLFW_MOD_ALT));
  assert(keychord_process_key_with_stack(GLFW_KEY_2, GLFW_PRESS, 0));
  assert(keychord_prefix_arg_value() == -2);
  keychord_clear_prefix_arg();
  keychord_set_prefix_capture_enabled(false);
  assert(!keychord_process_key_with_stack(GLFW_KEY_U, GLFW_PRESS,
                                          GLFW_MOD_CONTROL));
  assert(!keychord_prefix_arg_is_set());
  keychord_set_prefix_capture_enabled(true);
  for (int i = 0; i < 20; ++i)
    assert(keychord_process_key_with_stack(GLFW_KEY_9, GLFW_PRESS,
                                           i == 0 ? GLFW_MOD_ALT : 0));
  assert(keychord_prefix_arg_value() == INT64_MAX);
  keychord_clear_prefix_arg();
  assert(keychord_process_key_with_stack(GLFW_KEY_MINUS, GLFW_PRESS,
                                         GLFW_MOD_ALT));
  for (int i = 0; i < 20; ++i)
    assert(keychord_process_key_with_stack(GLFW_KEY_9, GLFW_PRESS, 0));
  assert(keychord_prefix_arg_value() == INT64_MIN);
  keychord_clear_prefix_arg();

  KeyChordBinding *visible[8] = {0};
  size_t count = keychord_collect_active_bindings(visible, 8);
  assert(count == 3);
  assert(strcmp(visible[0]->description, "local x") == 0);
  assert(strcmp(visible[1]->description, "local help") == 0);
  assert(strcmp(visible[2]->description, "global quit") == 0);

  assert(keychord_bind(&overriding, "C-g", native_action, "override quit",
                       GLFW_PRESS));
  keymap_set_overriding(&overriding);
  count = keychord_collect_active_bindings(visible, 8);
  assert(count == 3);
  assert(strcmp(visible[0]->description, "override quit") == 0);
  assert(strcmp(visible[1]->description, "local x") == 0);
  assert(strcmp(visible[2]->description, "local help") == 0);

  keymap_clear_overriding();
  keymap_stack_pop();
  keymap_free(&overriding);
  keymap_free(&local);
  keymap_free(&keymap);
}

/// Bound keys skip clock reads; prefixes and subsequent chord keys still
/// timestamp.
static void dispatch_timer_contract(void) {
  KeyChordMap map = {0};
  KeyChord chord = {0};
  keymap_init(&map);
  assert(keychord_bind(&map, "a", native_action, "single", GLFW_PRESS));
  assert(parse_keychord_notation("a", &chord));
  glfw_time_calls = 0;
  assert(keychord_process_key(&map, chord.keys[0], GLFW_PRESS, chord.mods[0]));
  assert(glfw_time_calls == 0);
  keymap_free(&map);

  keymap_stack_clear();
  keymap_init(&keymap);
  keymap_init(&map);
  assert(keychord_bind(&map, "a", native_action, "single", GLFW_PRESS));
  assert(parse_keychord_notation("a", &chord));
  keymap_stack_push(&map);
  glfw_time_calls = 0;
  assert(keychord_process_key_with_stack(chord.keys[0], GLFW_PRESS,
                                         chord.mods[0]));
  assert(glfw_time_calls == 0);

  keymap_stack_clear();
  keymap_free(&map);
  keymap_init(&map);
  assert(keychord_bind(&map, "a b", native_action, "chord", GLFW_PRESS));
  assert(parse_keychord_notation("a b", &chord) && chord.length == 2);
  keymap_stack_push(&map);
  glfw_time_calls = 0;
  assert(keychord_process_key_with_stack(chord.keys[0], GLFW_PRESS,
                                         chord.mods[0]));
  assert(glfw_time_calls == 1);
  assert(keychord_process_key_with_stack(chord.keys[1], GLFW_PRESS,
                                         chord.mods[1]));
  assert(glfw_time_calls == 2);
  keymap_stack_clear();
  keymap_free(&map);
  keymap_free(&keymap);
}

/// Prepared allocations, duplicate bindings, abort, generation guards
static void image_test(void) {
  KeyChordMap live = {0};
  keymap_init(&live);
  assert(live.bindings);
  monad_value_t a = {.references = 1}, b = {.references = 1};
  assert(keychord_bind(&live, "C-n", native_action, "native", GLFW_PRESS));
  assert(keychord_bind_monad_named(&live, "C-a", &a, "obsidian.monad", "old",
                                   GLFW_PRESS));
  assert(keychord_bind_monad_named(&live, "C-b", &a, "obsidian.monad", "old",
                                   GLFW_PRESS));
  assert(a.references == 3);
  const uint64_t epoch = live.lookup_generation;
  KeyImageOperation bad[] = {
      {.action = KEY_IMAGE_BIND, .notation = "C-n", .procedure = &b}};
  KeyChordPreparedImage image = {0};
  assert(!keychord_prepare_monad_image(&live, bad, 1, &image));
  assert(!image.bindings && live.lookup_generation == epoch &&
         a.references == 3);
  KeyImageOperation ops[] = {
      {.action = KEY_IMAGE_BIND, .notation = "C-a", .procedure = &b},
      {.action = KEY_IMAGE_BIND, .notation = "C-x", .procedure = &b},
      {.action = KEY_IMAGE_UNBIND, .notation = "C-x"},
      {.action = KEY_IMAGE_BIND, .notation = "C-z", .procedure = &a},
      {.action = KEY_IMAGE_BIND, .notation = "C-z", .procedure = &b},
  };
  assert(keychord_prepare_monad_image(&live, ops, 5, &image));
  assert(a.references == 3 && b.references == 3); /* b in a, z */
  assert(keychord_find_binding(&live, "C-b"));    /* live state unchanged */
  keychord_discard_prepared_image(&image);
  assert(!image.bindings && a.references == 3 && b.references == 1);
  assert(keychord_prepare_monad_image(&live, ops, 5, &image));
  const KeyChordBinding *native_before = keychord_find_binding(&live, "C-n");
  assert(native_before && native_before->action_type == ACTION_C_FUNCTION);
  watched_allocations = 0;
  watch_allocations = 1;
  assert(keychord_commit_monad_image(&live, &image));
  watch_allocations = 0;
  assert(watched_allocations == 0);
  assert(!image.bindings && a.references == 1 && b.references == 3);
  assert(keychord_find_binding(&live, "C-n"));
  assert(keychord_find_binding(&live, "C-a"));
  assert(!keychord_find_binding(&live, "C-b"));
  assert(!keychord_find_binding(&live, "C-x"));
  assert(keychord_find_binding(&live, "C-z"));
  assert(!keychord_prepare_monad_image(&live, bad, 1, &image));
  KeyImageOperation one[] = {
      {.action = KEY_IMAGE_BIND, .notation = "C-c", .procedure = &a}};
  assert(keychord_prepare_monad_image(&live, one, 1, &image));
  assert(keychord_bind(&live, "C-d", native_action, "new", GLFW_PRESS));
  assert(!keychord_commit_monad_image(&live, &image));
  keychord_discard_prepared_image(&image);
  assert(keychord_find_binding(&live, "C-a"));
  assert(a.references == 1 && b.references == 3);
  assert(keychord_remove_managed_monad(&live) == 2);
  assert(keychord_remove_managed_monad(&live) == 0);
  assert(live.count == 2 && b.references == 1);
  keymap_free(&live);
  assert(retains == releases);
}

/// 10k replace/reject cycles, release accounting, no stale references
static void stress(void) {
  KeyChordMap map = {0};
  keymap_init(&map);
  monad_value_t v = {.references = 1};
  for (unsigned i = 0; i < 10000; ++i) {
    KeyImageOperation ops[3] = {
        {.action = KEY_IMAGE_BIND, .notation = "C-a", .procedure = &v},
        {.action = KEY_IMAGE_BIND, .notation = "C-b", .procedure = &v},
        {.action = KEY_IMAGE_BIND, .notation = "C-c", .procedure = &v},
    };
    KeyChordPreparedImage image = {0};
    assert(keychord_prepare_monad_image(&map, ops, 3, &image));
    if (i % 7 == 0)
      keychord_discard_prepared_image(&image);
    else
      assert(keychord_commit_monad_image(&map, &image));
    assert(v.references == (int)(map.count + 1));
  }
  keymap_free(&map);
  assert(v.references == 1 && retains == releases);
}

/// Optional local microbenchmark: result is environment-specific
static uint64_t nanos(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int dispatch_calls;
static void dispatch_action(void) { ++dispatch_calls; }

static int compare_u64(const void *left, const void *right) {
  uint64_t a = *(const uint64_t *)left;
  uint64_t b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

/// Profile batched samples to amortize timer overhead; measures production
/// dispatch.
static void bench_dispatch(void) {
  enum { batches = 101, events_per_batch = 10000 };
  uint64_t samples[batches];
  KeyChordMap local = {0};
  keymap_stack_clear();
  keymap_init(&keymap);
  keymap_init(&local);
  assert(keychord_bind(&keymap, "a", dispatch_action, "global", GLFW_PRESS));
  assert(keychord_bind(&local, "a", dispatch_action, "local", GLFW_PRESS));
  KeyChord test_chord = {0};
  assert(parse_keychord_notation("a", &test_chord));
  assert(keychord_lookup_binding(&local, &test_chord, GLFW_PRESS));
  keymap_stack_push(&local);

  watched_allocations = 0;
  watched_allocation_bytes = 0;
  glfw_time_calls = 0;
  keychord_reset_lookup_profile();
  watch_allocations = 1;
  for (int batch = 0; batch < batches; ++batch) {
    uint64_t start = nanos();
    for (int event = 0; event < events_per_batch; ++event) {
      if (!keychord_process_key_with_stack(test_chord.keys[0], GLFW_PRESS, 0)) {
        fprintf(stderr, "dispatch rejected at batch=%d event=%d\n", batch,
                event);
        abort();
      }
    }
    samples[batch] = (nanos() - start) / events_per_batch;
  }
  watch_allocations = 0;
  qsort(samples, batches, sizeof(samples[0]), compare_u64);
  assert(dispatch_calls == batches * events_per_batch);
#ifdef OBS_KEYCHORD_PROFILE
  const char *profile_mode = "profiled";
#else
  const char *profile_mode = "unprofiled";
#endif
  printf("keychord dispatch (%s): local native PRESS, "
         "%d batches x %d events; "
         "p50=%llu ns/event p95=%llu ns/event; "
         "allocations=%zu requested_bytes=%zu; timer_reads=%llu; "
         "lookup_cache_hits=%llu\n",
         profile_mode, batches, events_per_batch,
         (unsigned long long)samples[batches / 2],
         (unsigned long long)samples[(batches * 95) / 100], watched_allocations,
         watched_allocation_bytes, (unsigned long long)glfw_time_calls,
         (unsigned long long)keychord_lookup_cache_hits());
  keymap_stack_clear();
  keymap_free(&local);
  keymap_free(&keymap);
}

static void benchmark(void) {
  KeyChordMap map = {0};
  keymap_init(&map);
  monad_value_t v = {.references = 1};
  const char *keys[] = {"C-a", "C-b", "C-c", "C-d", "C-e", "C-f", "C-g", "C-h",
                        "C-i", "C-j", "C-k", "C-l", "C-m", "C-n", "C-o", "C-p"};
  uint64_t start = nanos();
  for (unsigned i = 0; i < 3000; ++i) {
    KeyImageOperation ops[16];
    for (unsigned j = 0; j < 16; ++j)
      ops[j] = (KeyImageOperation){
          .action = KEY_IMAGE_BIND, .notation = keys[j], .procedure = &v};
    KeyChordPreparedImage image = {0};
    assert(keychord_prepare_monad_image(&map, ops, 16, &image));
    assert(keychord_commit_monad_image(&map, &image));
  }
  uint64_t elapsed = nanos() - start;
  printf(
      "keymap image: 3000 x 16 replacements: %.2f us/cycle (headless stub)\n",
      elapsed / 3000.0 / 1000.0);
  keymap_free(&map);
  assert(v.references == 1);
}
/// Benchmark old one-at-a-time removal against one-pass ownership cleanup
static void bench_cleanup(void) {
  const char *keys[] = {"C-a", "C-b", "C-c", "C-d", "C-e", "C-f", "C-g", "C-h",
                        "C-i", "C-j", "C-k", "C-l", "C-m", "C-n", "C-o", "C-p"};
  uint64_t slow_ns = 0, fast_ns = 0;
  monad_value_t value = {.references = 1};
  for (unsigned cycle = 0; cycle < 2000; ++cycle) {
    KeyChordMap slow = {0}, fast = {0};
    keymap_init(&slow);
    keymap_init(&fast);
    for (size_t i = 0; i < 16; ++i) {
      assert(keychord_bind_monad_named(&slow, keys[i], &value, "obsidian.monad",
                                       "script", GLFW_PRESS));
      assert(keychord_bind_monad_named(&fast, keys[i], &value, "obsidian.monad",
                                       "script", GLFW_PRESS));
    }
    uint64_t start = nanos();
    for (size_t i = 0; i < 16; ++i)
      assert(keychord_unbind(&slow, keys[i]));
    slow_ns += nanos() - start;
    start = nanos();
    assert(keychord_remove_managed_monad(&fast) == 16);
    fast_ns += nanos() - start;
    keymap_free(&slow);
    keymap_free(&fast);
  }
  assert(value.references == 1);
  printf("keymap cleanup: repeated unbind %.3f us, bulk %.3f us per 16 "
         "bindings (stub)\n",
         slow_ns / 2000.0 / 1000.0, fast_ns / 2000.0 / 1000.0);
}
int main(void) {
  parser();
  image_test();
  stress();
  active_bindings();
  dispatch_timer_contract();
  puts("keymap image/parser ownership, abort, generation, 10k cycles PASS");
  if (getenv("OBSIDIAN_BENCH_KEYMAP")) {
    bench_dispatch();
    benchmark();
    bench_cleanup();
  }
  return 0;
}
