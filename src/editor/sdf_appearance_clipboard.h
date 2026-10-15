/// sdf_appearance_clipboard — value-only cross-primitive appearance transfer
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdf_inspector.h"

/* Engine-owner-thread operations. Never store a primitive pointer, scene
 * ticket, or mutable data in the clipboard. Copy works from the current SDF
 * registry; paste uses the existing property/undo bridge. */
ObsSdfEditStatus obs_sdf_appearance_copy(uint32_t source_index);
ObsSdfEditStatus obs_sdf_appearance_paste(uint32_t target_index);
bool obs_sdf_appearance_ready(void);
void obs_sdf_appearance_clear(void);

/* Durable, portable OBSLOOK v1 presets. A failed load preserves the current
 * sample; file operations refuse to overlap a speculative scene edit. */
bool obs_sdf_appearance_save_file(const char *path);
bool obs_sdf_appearance_load_file(const char *path);
/* OBSIDIAN_SDF_LOOK_PATH, or the default in the current working directory. */
const char *obs_sdf_appearance_preset_path(void);
