#define _POSIX_C_SOURCE 200809L
#include "renderer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SAMPLE_COUNT = 5 };

static uint64_t now_ns(void) {
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
    perror("clock_gettime");
    exit(EXIT_FAILURE);
  }
  return (uint64_t)time.tv_sec * UINT64_C(1000000000) +
         (uint64_t)time.tv_nsec;
}

static int compare_u64(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

static bool validate_order(const Meshes *meshes, const uint8_t *opaque,
                           size_t opaque_count, uint8_t *seen) {
  memset(seen, 0, meshes->count);
  float previous_distance = 0.0f;
  for (size_t i = 0; i < meshes->count; ++i) {
    const uint32_t index = meshes->draw_indices[i];
    if (index >= meshes->count || seen[index])
      return false;
    seen[index] = 1;
    if ((i < opaque_count) != (opaque[index] != 0))
      return false;
    if (i >= opaque_count) {
      const Mesh *mesh = &meshes->items[index];
      const float z = mesh->model[3][2];
      const float distance = z * z;
      if (i > opaque_count && previous_distance < distance)
        return false;
      previous_distance = distance;
    }
  }
  return true;
}

static bool run_case(size_t count, bool mixed, size_t iterations) {
  Mesh *items = calloc(count, sizeof(*items));
  uint32_t *indices = malloc(count * sizeof(*indices));
  uint32_t *original = malloc(count * sizeof(*original));
  uint8_t *opaque = calloc(count, sizeof(*opaque));
  uint8_t *seen = malloc(count);
  uint64_t *times = malloc(iterations * sizeof(*times));
  if (!items || !indices || !original || !opaque || !seen || !times) {
    fprintf(stderr, "transparent sort benchmark allocation failed\n");
    free(items);
    free(indices);
    free(original);
    free(opaque);
    free(seen);
    free(times);
    return false;
  }

  size_t expected_opaque = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t hash = (uint32_t)i + UINT32_C(0x9e3779b9);
    hash ^= hash >> 16;
    hash *= UINT32_C(0x7feb352d);
    hash ^= hash >> 15;
    hash *= UINT32_C(0x846ca68b);
    hash ^= hash >> 16;
    items[i].model[3][2] = (float)(hash % (uint32_t)(count * 13 + 1) + 1);
    opaque[i] = (uint8_t)(mixed && i % 4 == 0);
    items[i].alpha_mode = opaque[i] ? 0 : 2;
    expected_opaque += opaque[i] != 0;
    indices[i] = original[i] = (uint32_t)i;
  }
  Meshes meshes = {
      .items = items,
      .draw_indices = indices,
      .count = count,
      .capacity = count,
  };
  vec3 camera = {0.0f, 0.0f, 0.0f};

  sort_meshes_by_alpha(&meshes, camera);
  if (!validate_order(&meshes, opaque, expected_opaque, seen)) {
    fprintf(stderr, "transparent sort correctness failed (%zu meshes, %s)\n",
            count, mixed ? "mixed" : "all blend");
    goto fail;
  }

  uint64_t sample_medians[SAMPLE_COUNT];
  for (size_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
    for (size_t i = 0; i < iterations; ++i) {
      memcpy(indices, original, count * sizeof(*indices));
      const uint64_t start = now_ns();
      sort_meshes_by_alpha(&meshes, camera);
      times[i] = now_ns() - start;
      if (!validate_order(&meshes, opaque, expected_opaque, seen)) {
        fprintf(stderr,
                "transparent sort correctness failed during measurement "
                "(%zu meshes, %s)\n",
                count, mixed ? "mixed" : "all blend");
        goto fail;
      }
    }
    qsort(times, iterations, sizeof(*times), compare_u64);
    sample_medians[sample] = times[iterations / 2];
  }
  qsort(sample_medians, SAMPLE_COUNT, sizeof(*sample_medians), compare_u64);
  printf("transparent sort meshes=%zu blend=%zu case=%s median=%" PRIu64
         " ns/sort (%d samples x %zu sorts)\n",
         count, count - expected_opaque, mixed ? "mixed" : "all-blend",
         sample_medians[SAMPLE_COUNT / 2], SAMPLE_COUNT, iterations);
  free(items);
  free(indices);
  free(original);
  free(opaque);
  free(seen);
  free(times);
  return true;

fail:
  free(items);
  free(indices);
  free(original);
  free(opaque);
  free(seen);
  free(times);
  return false;
}

int main(int argc, char **argv) {
  size_t iterations = 16;
  if (argc > 2) {
    fprintf(stderr, "usage: %s [sorts-per-sample]\n", argv[0]);
    return EXIT_FAILURE;
  }
  if (argc == 2) {
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(argv[1], &end, 10);
    if (errno || end == argv[1] || *end || parsed == 0 ||
        parsed > UINT32_MAX) {
      fprintf(stderr, "sorts-per-sample must be in 1..%" PRIu32 "\n",
              UINT32_MAX);
      return EXIT_FAILURE;
    }
    iterations = (size_t)parsed;
  }

  static const size_t counts[] = {128, 512, 2048, 8192};
  puts("Production transparent mesh sort; CPU-only, no renderer/GPU startup");
  for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
    if (!run_case(counts[i], false, iterations) ||
        !run_case(counts[i], true, iterations))
      return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
