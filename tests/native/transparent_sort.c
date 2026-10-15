#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>

extern uint32_t opaqueMeshCount;
extern uint32_t transparentMeshCount;

static bool fail_next_realloc;

void *__real_realloc(void *pointer, size_t size);
void *__wrap_realloc(void *pointer, size_t size) {
  if (fail_next_realloc) {
    fail_next_realloc = false;
    return NULL;
  }
  return __real_realloc(pointer, size);
}

static bool check_partition_and_order(const Meshes *meshes,
                                      size_t expected_opaque,
                                      bool check_depth_order) {
  bool seen[8] = {false};
  if (meshes->count > sizeof(seen) / sizeof(seen[0]) ||
      opaqueMeshCount != expected_opaque ||
      transparentMeshCount != meshes->count - expected_opaque)
    return false;
  float previous_distance = 0.0f;
  for (size_t i = 0; i < meshes->count; ++i) {
    const uint32_t index = meshes->draw_indices[i];
    if (index >= meshes->count || seen[index])
      return false;
    seen[index] = true;
    const bool opaque = meshes->items[index].alpha_mode != 2;
    if ((i < expected_opaque) != opaque)
      return false;
    if (i >= expected_opaque) {
      const float z = meshes->items[index].model[3][2];
      const float distance = z * z;
      if (check_depth_order && i > expected_opaque &&
          previous_distance < distance)
        return false;
      previous_distance = distance;
    }
  }
  return true;
}

static bool test_sort_order(void) {
  Mesh items[8] = {0};
  uint32_t indices[8];
  static const float depths[8] = {1, 8, 3, 6, 4, 7, 2, 5};
  static const int alpha_modes[8] = {2, 0, 2, 2, 0, 2, 2, 0};
  for (uint32_t i = 0; i < 8; ++i) {
    indices[i] = i;
    items[i].model[3][2] = depths[i];
    items[i].alpha_mode = alpha_modes[i];
  }
  Meshes meshes = {
      .items = items, .draw_indices = indices, .count = 8, .capacity = 8};
  vec3 camera = {0.0f, 0.0f, 0.0f};
  sort_meshes_by_alpha(&meshes, camera);
  return check_partition_and_order(&meshes, 3, true);
}

static bool test_heap_boundaries(void) {
  enum { MESH_COUNT = 3072, OPAQUE_COUNT = 1024 };
  Mesh *items = calloc(MESH_COUNT, sizeof(*items));
  uint32_t *indices = malloc(MESH_COUNT * sizeof(*indices));
  bool *seen = calloc(MESH_COUNT, sizeof(*seen));
  if (!items || !indices || !seen) {
    free(items);
    free(indices);
    free(seen);
    return false;
  }
  size_t opaque_count = 0;
  for (uint32_t i = 0; i < MESH_COUNT; ++i) {
    indices[i] = MESH_COUNT - 1 - i;
    items[i].model[3][2] = (float)((i * 37u) % 32u);
    items[i].alpha_mode = i % 3u == 0u ? 0 : 2;
    opaque_count += items[i].alpha_mode != 2;
  }
  Meshes meshes = {
      .items = items,
      .draw_indices = indices,
      .count = MESH_COUNT,
      .capacity = MESH_COUNT,
  };
  vec3 camera = {0.0f, 0.0f, 0.0f};
  sort_meshes_by_alpha(&meshes, camera);
  if (opaque_count != OPAQUE_COUNT || opaqueMeshCount != OPAQUE_COUNT ||
      transparentMeshCount != MESH_COUNT - OPAQUE_COUNT) {
    free(items);
    free(indices);
    free(seen);
    return false;
  }

  float previous_distance = 0.0f;
  bool valid = true;
  for (size_t i = 0; i < MESH_COUNT; ++i) {
    const uint32_t index = indices[i];
    if (index >= MESH_COUNT || seen[index]) {
      valid = false;
      break;
    }
    seen[index] = true;
    if ((i < OPAQUE_COUNT) != (items[index].alpha_mode != 2)) {
      valid = false;
      break;
    }
    if (i >= OPAQUE_COUNT) {
      const float z = items[index].model[3][2];
      const float distance = z * z;
      if (i > OPAQUE_COUNT && previous_distance < distance) {
        valid = false;
        break;
      }
      previous_distance = distance;
    }
  }
  free(items);
  free(indices);
  free(seen);
  return valid;
}

static bool test_realloc_failure_and_retry(void) {
  Mesh small_items[2] = {0};
  uint32_t small_indices[2] = {0, 1};
  small_items[0].model[3][2] = 1.0f;
  small_items[1].model[3][2] = 2.0f;
  small_items[0].alpha_mode = 2;
  small_items[1].alpha_mode = 2;
  Meshes small = {
      .items = small_items,
      .draw_indices = small_indices,
      .count = 2,
      .capacity = 2,
  };
  vec3 camera = {0.0f, 0.0f, 0.0f};
  sort_meshes_by_alpha(&small, camera);

  Mesh items[4] = {0};
  uint32_t indices[4] = {0, 1, 2, 3};
  items[0].alpha_mode = 2;
  items[1].alpha_mode = 0;
  items[2].alpha_mode = 2;
  items[3].alpha_mode = 2;
  items[0].model[3][2] = 1.0f;
  items[1].model[3][2] = 4.0f;
  items[2].model[3][2] = 3.0f;
  items[3].model[3][2] = 2.0f;
  Meshes larger = {
      .items = items, .draw_indices = indices, .count = 4, .capacity = 4};

  fail_next_realloc = true;
  sort_meshes_by_alpha(&larger, camera);
  if (fail_next_realloc || !check_partition_and_order(&larger, 1, false))
    return false;

  sort_meshes_by_alpha(&larger, camera);
  return check_partition_and_order(&larger, 1, true);
}

int main(void) {
  if (!test_realloc_failure_and_retry()) {
    fprintf(stderr, "transparent sort allocation failure/retry failed\n");
    return EXIT_FAILURE;
  }
  if (!test_sort_order()) {
    fprintf(stderr, "transparent sort ordering/partition failed\n");
    return EXIT_FAILURE;
  }
  if (!test_heap_boundaries()) {
    fprintf(stderr, "transparent sort heap boundary/order failed\n");
    return EXIT_FAILURE;
  }
  puts("transparent sort: back-to-front order, opaque partition, "
       "allocation failure/retry, heap boundaries PASS");
  return EXIT_SUCCESS;
}
