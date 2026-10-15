#include "material_browser.h"

#include "editor.h"
#include "gizmo.h"
#include "live_editor.h"
#include "mesh_properties.h"
#include "renderer.h"
#include "scene.h"
#include "theme.h"
#include "vertico.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int g_selected_sdf_index;

#define OBS_MATERIAL_ENTRY_MAGIC UINT32_C(0x4f424d41)
#define OBS_MATERIAL_ROW_HEIGHT 96.0f

typedef enum {
  OBS_MATERIAL_MESH,
  OBS_MATERIAL_PRESET,
  OBS_MATERIAL_SDF
} ObsMaterialBrowserSource;

struct ObsMaterialBrowserEntry {
  uint32_t magic;
  ObsMaterialBrowserSource source;
  size_t index;
  int material_index;
  int32_t albedo_index;
  int32_t normal_index;
  int32_t metallic_rough_index;
  int32_t ao_index;
  int32_t emissive_index;
  float rgba[4];
  float emissive[3];
  float emissive_strength;
  float roughness;
  float metallic;
  float yaw;
  float pitch;
  char name[128];
};

static ObsMaterialBrowserEntry s_entries[VERTICO_MAX_CANDIDATES];
static size_t s_entry_count;

static void copy_name(char *out, size_t capacity, const char *name) {
  if (!capacity) return;
  if (!name) name = "";
  size_t length = 0;
  while (length + 1 < capacity && name[length]) ++length;
  memcpy(out, name, length);
  out[length] = '\0';
}

static bool mesh_material_equal(const ObsMaterialBrowserEntry *entry,
                                const Mesh *mesh) {
  if (!entry || !mesh || entry->source != OBS_MATERIAL_MESH) return false;
  return entry->material_index == mesh->materialIndex &&
      entry->albedo_index == mesh->textureIndex &&
      entry->normal_index == mesh->normalMapIndex &&
      entry->metallic_rough_index == mesh->metallicRoughIndex &&
      entry->ao_index == mesh->aoIndex &&
      entry->emissive_index == mesh->emissiveIndex &&
      entry->roughness == mesh->roughnessFactor &&
      entry->metallic == mesh->metallicFactor &&
      memcmp(entry->rgba, mesh->baseColorFactor, sizeof(entry->rgba)) == 0;
}

static bool sdf_material_equal(const ObsMaterialBrowserEntry *entry,
                               const SdfPrimitive *sdf) {
  return entry && sdf && entry->source == OBS_MATERIAL_SDF &&
      entry->roughness == sdf->roughness &&
      entry->metallic == sdf->metallic &&
      entry->emissive_strength == sdf->emissiveStrength &&
      memcmp(entry->rgba, sdf->color, sizeof(entry->rgba)) == 0 &&
      memcmp(entry->emissive, sdf->emissive, sizeof(entry->emissive)) == 0;
}

void obs_material_browser_begin(void) { s_entry_count = 0; }

static ObsMaterialBrowserEntry *entry_new(void) {
  if (s_entry_count >= VERTICO_MAX_CANDIDATES) return NULL;
  ObsMaterialBrowserEntry *entry = &s_entries[s_entry_count++];
  *entry = (ObsMaterialBrowserEntry){
      .magic = OBS_MATERIAL_ENTRY_MAGIC,
      .yaw = 0.785398163f,
      .pitch = 0.48f,
  };
  return entry;
}

const ObsMaterialBrowserEntry *obs_material_browser_add_mesh(size_t index) {
  if (!scene || index >= scene->meshes.count) return NULL;
  const Mesh *mesh = &scene->meshes.items[index];
  for (size_t i = 0; i < s_entry_count; ++i)
    if (mesh_material_equal(&s_entries[i], mesh))
      return NULL;

  ObsMaterialBrowserEntry *entry = entry_new();
  if (!entry) return NULL;
  entry->source = OBS_MATERIAL_MESH;
  entry->index = index;
  entry->material_index = mesh->materialIndex;
  entry->albedo_index = mesh->textureIndex;
  entry->normal_index = mesh->normalMapIndex;
  entry->metallic_rough_index = mesh->metallicRoughIndex;
  entry->ao_index = mesh->aoIndex;
  entry->emissive_index = mesh->emissiveIndex;
  memcpy(entry->rgba, mesh->baseColorFactor, sizeof(entry->rgba));
  entry->roughness = mesh->roughnessFactor;
  entry->metallic = mesh->metallicFactor;
  char mesh_name[96];
  copy_name(mesh_name, sizeof(mesh_name), mesh->name);
  if (!mesh_name[0]) snprintf(mesh_name, sizeof(mesh_name), "Mesh %zu", index);
  if (mesh->materialIndex >= 0)
    snprintf(entry->name, sizeof(entry->name), "Material %d · %.88s",
             mesh->materialIndex, mesh_name);
  else
    snprintf(entry->name, sizeof(entry->name), "Material · %.96s", mesh_name);
  return entry;
}

const ObsMaterialBrowserEntry *obs_material_browser_add_asset(size_t index) {
  if (!scene || index >= scene->materials.count) return NULL;
  const ObsMaterialValues *values = &scene->materials.items[index].values;
  ObsMaterialBrowserEntry *entry = entry_new();
  if (!entry) return NULL;
  entry->source = OBS_MATERIAL_PRESET;
  entry->index = index;
  entry->material_index = -1;
  entry->albedo_index = -1;
  entry->normal_index = -1;
  entry->metallic_rough_index = -1;
  entry->ao_index = -1;
  entry->emissive_index = -1;
  memcpy(entry->rgba, values->rgba, sizeof(entry->rgba));
  entry->roughness = values->roughness;
  entry->metallic = values->metallic;
  snprintf(entry->name, sizeof(entry->name), "Material preset %zu", index);
  return entry;
}

const ObsMaterialBrowserEntry *obs_material_browser_add_sdf(size_t index) {
  if (!scene || index >= scene->sdf_count) return NULL;
  const SdfPrimitive *sdf = &scene->sdfs[index];
  for (size_t i = 0; i < s_entry_count; ++i)
    if (sdf_material_equal(&s_entries[i], sdf))
      return NULL;

  ObsMaterialBrowserEntry *entry = entry_new();
  if (!entry) return NULL;
  entry->source = OBS_MATERIAL_SDF;
  entry->index = index;
  entry->material_index = -1;
  entry->albedo_index = -1;
  entry->normal_index = -1;
  entry->metallic_rough_index = -1;
  entry->ao_index = -1;
  entry->emissive_index = -1;
  memcpy(entry->rgba, sdf->color, sizeof(entry->rgba));
  memcpy(entry->emissive, sdf->emissive, sizeof(entry->emissive));
  entry->roughness = sdf->roughness;
  entry->metallic = sdf->metallic;
  entry->emissive_strength = sdf->emissiveStrength;

  const char *name = "SDF";
  for (int32_t node = 1; node < scene->tree.count; ++node) {
    if (scene->tree.nodes[node].sdf_index == (int32_t)index) {
      name = scene->tree.nodes[node].name;
      break;
    }
  }
  snprintf(entry->name, sizeof(entry->name), "SDF Material · %.100s", name);
  return entry;
}

bool obs_material_browser_contains(const void *data) {
  const uintptr_t address = (uintptr_t)data;
  const uintptr_t begin = (uintptr_t)&s_entries[0];
  const uintptr_t end = (uintptr_t)&s_entries[s_entry_count];
  return data && address >= begin && address < end &&
         (address - begin) % sizeof(s_entries[0]) == 0 &&
         ((const ObsMaterialBrowserEntry *)data)->magic ==
             OBS_MATERIAL_ENTRY_MAGIC;
}

const char *obs_material_browser_name(const ObsMaterialBrowserEntry *entry) {
  return entry && entry->magic == OBS_MATERIAL_ENTRY_MAGIC ? entry->name : "";
}

const char *obs_material_browser_kind(const ObsMaterialBrowserEntry *entry) {
  if (!entry || entry->magic != OBS_MATERIAL_ENTRY_MAGIC) return "unknown";
  switch (entry->source) {
  case OBS_MATERIAL_MESH: return "mesh";
  case OBS_MATERIAL_PRESET: return "preset";
  case OBS_MATERIAL_SDF: return "sdf";
  }
  return "unknown";
}

void obs_material_browser_annotation(const ObsMaterialBrowserEntry *entry,
                                     char *out, size_t capacity) {
  if (!out || !capacity) return;
  out[0] = '\0';
  if (!entry || entry->magic != OBS_MATERIAL_ENTRY_MAGIC) return;
  char detail[128];
  const unsigned red = (unsigned)lrintf(fminf(fmaxf(entry->rgba[0], 0.0f), 1.0f) * 255.0f);
  const unsigned green = (unsigned)lrintf(fminf(fmaxf(entry->rgba[1], 0.0f), 1.0f) * 255.0f);
  const unsigned blue = (unsigned)lrintf(fminf(fmaxf(entry->rgba[2], 0.0f), 1.0f) * 255.0f);
  snprintf(detail, sizeof(detail), "R %.2f · M %.2f · E %.2f · #%02X%02X%02X",
           (double)entry->roughness, (double)entry->metallic,
           (double)entry->emissive_strength,
           red, green, blue);
  if (!obs_monad_live_editor_marginalia_material(
          obs_material_browser_kind(entry), detail, out, capacity))
    snprintf(out, capacity, "%s · %s",
             obs_material_browser_kind(entry), detail);
}

void obs_material_browser_render(const ObsMaterialBrowserEntry *entry,
                                float center_x, float center_y, float size) {
  if (!entry || entry->magic != OBS_MATERIAL_ENTRY_MAGIC || size <= 0.0f)
    return;
  typedef struct {
    float x, y, z;
  } PreviewVertex;
  static const PreviewVertex cube[8] = {
      {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
      {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1}};
  static const unsigned faces[6][4] = {
      {4,5,6,7}, {1,0,3,2}, {0,4,7,3},
      {5,1,2,6}, {7,6,2,3}, {0,1,5,4}};
  const float cy = cosf(entry->yaw), sy = sinf(entry->yaw);
  const float cp = cosf(entry->pitch), sp = sinf(entry->pitch);
  const float radius = size * 0.22f;
  PreviewVertex projected[8];
  for (unsigned i = 0; i < 8; ++i) {
    const float x = cy * cube[i].x + sy * cube[i].z;
    const float z0 = -sy * cube[i].x + cy * cube[i].z;
    const float y = cp * cube[i].y - sp * z0;
    const float z = sp * cube[i].y + cp * z0;
    projected[i] = (PreviewVertex){
        center_x + radius * (x + 0.42f * z),
        center_y + radius * (y + 0.24f * z), z};
  }

  typedef struct { unsigned face; float depth; } FaceOrder;
  FaceOrder visible[6];
  size_t visible_count = 0;
  for (unsigned f = 0; f < 6; ++f) {
    const unsigned a = faces[f][0], b = faces[f][1], c = faces[f][2];
    const float ux = projected[b].x - projected[a].x;
    const float uy = projected[b].y - projected[a].y;
    const float vx = projected[c].x - projected[a].x;
    const float vy = projected[c].y - projected[a].y;
    const float winding = ux * vy - uy * vx;
    if (winding <= 0.0f) continue;
    float depth = 0.0f;
    for (unsigned j = 0; j < 4; ++j)
      depth += projected[faces[f][j]].z * 0.25f;
    visible[visible_count++] = (FaceOrder){f, depth};
  }
  for (size_t i = 1; i < visible_count; ++i) {
    FaceOrder face = visible[i];
    size_t j = i;
    while (j && visible[j - 1].depth > face.depth) {
      visible[j] = visible[j - 1];
      --j;
    }
    visible[j] = face;
  }

  const float alpha = fminf(fmaxf(entry->rgba[3], 0.45f), 1.0f);
  const float material_light =
      0.82f + 0.28f * entry->metallic + 0.18f * (1.0f - entry->roughness);
  for (size_t i = 0; i < visible_count; ++i) {
    const unsigned *face = faces[visible[i].face];
    const float light = 0.55f + 0.13f * (float)visible[i].face;
    const float emission = fminf(entry->emissive_strength, 4.0f) * 0.15f;
    Color color = {
        fminf(entry->rgba[0] * light * material_light +
                  entry->emissive[0] * emission, 1.0f),
        fminf(entry->rgba[1] * light * material_light +
                  entry->emissive[1] * emission, 1.0f),
        fminf(entry->rgba[2] * light * material_light +
                  entry->emissive[2] * emission, 1.0f),
        alpha};
    vec2 p0 = {projected[face[0]].x, projected[face[0]].y};
    vec2 p1 = {projected[face[1]].x, projected[face[1]].y};
    vec2 p2 = {projected[face[2]].x, projected[face[2]].y};
    vec2 p3 = {projected[face[3]].x, projected[face[3]].y};
    triangle_col(p0, color, p1, color, p2, color);
    triangle_col(p0, color, p2, color, p3, color);
    line2D(p0, p1, (Color){0.0f, 0.0f, 0.0f, alpha * 0.28f});
    line2D(p1, p2, (Color){0.0f, 0.0f, 0.0f, alpha * 0.28f});
    line2D(p2, p3, (Color){0.0f, 0.0f, 0.0f, alpha * 0.28f});
    line2D(p3, p0, (Color){0.0f, 0.0f, 0.0f, alpha * 0.28f});
  }
}

void obs_material_browser_drag(ObsMaterialBrowserEntry *entry, float dx,
                               float dy) {
  if (!entry || entry->magic != OBS_MATERIAL_ENTRY_MAGIC ||
      !isfinite(dx) || !isfinite(dy))
    return;
  entry->yaw -= dx * 0.012f;
  entry->pitch = fminf(fmaxf(entry->pitch - dy * 0.012f, -1.35f), 1.35f);
}

void obs_material_browser_select(const ObsMaterialBrowserEntry *entry) {
  if (!entry || entry->magic != OBS_MATERIAL_ENTRY_MAGIC) return;
  if (entry->source == OBS_MATERIAL_MESH) {
    if (!scene || entry->index >= scene->meshes.count) return;
    inspector_select_mesh((int)entry->index);
    inspector_show_material();
    return;
  }
  if (entry->source == OBS_MATERIAL_SDF) {
    if (!scene || entry->index >= scene->sdf_count) return;
    editor.inspector.selected_mesh_index = -1;
    g_selected_sdf_index = (int)entry->index;
    gizmo.active = true;
    return;
  }
  char report[128];
  snprintf(report, sizeof(report), "Preset · roughness %.2f · metallic %.2f",
           (double)entry->roughness, (double)entry->metallic);
  message(MSG_INFO, report);
}

float obs_material_browser_row_height(void) {
  return OBS_MATERIAL_ROW_HEIGHT;
}
