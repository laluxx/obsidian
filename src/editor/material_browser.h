#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct ObsMaterialBrowserEntry ObsMaterialBrowserEntry;

void obs_material_browser_begin(void);
const ObsMaterialBrowserEntry *obs_material_browser_add_mesh(size_t index);
const ObsMaterialBrowserEntry *obs_material_browser_add_asset(size_t index);
const ObsMaterialBrowserEntry *obs_material_browser_add_sdf(size_t index);
bool obs_material_browser_contains(const void *data);
const char *obs_material_browser_name(const ObsMaterialBrowserEntry *entry);
const char *obs_material_browser_kind(const ObsMaterialBrowserEntry *entry);
void obs_material_browser_annotation(const ObsMaterialBrowserEntry *entry,
                                     char *out, size_t capacity);
void obs_material_browser_render(const ObsMaterialBrowserEntry *entry,
                                float center_x, float center_y, float size);
void obs_material_browser_drag(ObsMaterialBrowserEntry *entry, float dx,
                               float dy);
void obs_material_browser_select(const ObsMaterialBrowserEntry *entry);
float obs_material_browser_row_height(void);
