/// omdl_cache.h — validated OMDL on-disk section layout
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/// On-disk header (legacy native ABI; changes require a magic/version bump)
typedef struct {
    uint32_t magic;
    uint32_t mesh_count;
    uint32_t total_vertices;
    uint32_t total_indices;
    uint32_t total_morph_deltas;
    uint32_t total_meshlets;
    uint32_t total_meshlet_vertices;
    uint32_t total_meshlet_triangles;
    uint32_t node_count;
    uint32_t skin_count;
    uint32_t anim_count;
    uint32_t total_joints;
    uint32_t total_channels;
    uint32_t total_floats;
} OmdlHeader;

typedef enum {
    OMDL_MESHES, OMDL_VERTICES, OMDL_INDICES, OMDL_MORPHS,
    OMDL_MESHLETS, OMDL_MESHLET_BOUNDS, OMDL_MESHLET_SKINS,
    OMDL_MESHLET_VERTICES, OMDL_MESHLET_TRIANGLES,
    OMDL_NODES, OMDL_SKINS, OMDL_JOINTS, OMDL_IBMS,
    OMDL_ANIMATIONS, OMDL_CHANNELS, OMDL_FLOATS, OMDL_SECTION_COUNT
} OmdlSection;

/* V2 envelope is 64 bytes of explicitly little-endian fields. The legacy
 * payload behind it is still native ABI, and is accepted ONLY on an exact
 * schema/ABI-tag match. This is NOT a portable geometry serializer. */
#define OMDL_V2_ENVELOPE_BYTES 64u
#define OMDL_V2_VERSION 2u

typedef struct {
    OmdlHeader header;
    uint64_t offset[OMDL_SECTION_COUNT];
    uint64_t bytes[OMDL_SECTION_COUNT];
    uint64_t total_bytes;
} OmdlLayout;

/// Public interface
/* Checks the ENTIRE legacy file layout before any renderer upload. Exact size
 * is required: truncation, trailing bytes, integer overflow and mismatched
 * section descriptors are cache misses. Never call on a shared FILE* while
 * another thread is writing to that cache. */
bool omdl_cache_layout(FILE *file, uint32_t magic,
                       const size_t element_sizes[OMDL_SECTION_COUNT],
                       OmdlLayout *out);
bool omdl_cache_seek(FILE *file, const OmdlLayout *layout, OmdlSection section);
bool omdl_cache_read(FILE *file, const OmdlLayout *layout, OmdlSection section,
                     void *destination);
/* Checked half-open range of elements, including count == 0. */
bool omdl_cache_range(uint32_t first, uint32_t count, uint32_t total);

/* Short writes are errors; zero-length spans require no allocation. */
bool omdl_cache_write(FILE *file, const void *data, size_t stride, size_t count);

/// Versioned container and source identity
/* Begin before writing the existing OmdlHeader + native-array payload.
 * The file must be opened read/write ("w+b") for the final checksum pass. */
bool omdl_cache_v2_begin(FILE *file);
bool omdl_cache_v2_finish(FILE *file, uint64_t source_digest,
                          uint64_t abi_tag);
/* Rejects legacy, unknown, truncated, tampered, source-mismatched and
 * incompatible-ABI containers before any GPU upload. */
bool omdl_cache_layout_v2(FILE *file, uint32_t payload_magic,
                          const size_t element_sizes[OMDL_SECTION_COUNT],
                          uint64_t source_digest, uint64_t abi_tag,
                          OmdlLayout *out);
/* CRC64-ECMA is accidental-corruption detection, NOT an adversarial
 * authentication mechanism. Mix lengths explicitly between source buffers. */
uint64_t omdl_cache_digest_bytes(uint64_t crc, const void *bytes, size_t count);
uint64_t omdl_cache_digest_size(uint64_t crc, uint64_t count);
bool omdl_cache_digest_file(const char *path, uint64_t *out);
uint64_t omdl_cache_abi_tag(const size_t element_sizes[OMDL_SECTION_COUNT],
                            const size_t *additional_layout_data,
                            size_t additional_count);
