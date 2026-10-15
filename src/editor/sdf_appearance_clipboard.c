#define _POSIX_C_SOURCE 200809L
/// sdf_appearance_clipboard — immutable material + emission, not geometry
#include "sdf_appearance_clipboard.h"
#include "sdf_property_bridge.h"
#include "sdf_sculpt.h"
#include "atomic_file.h"
#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(double) == 8 && FLT_RADIX == 2 &&
               DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "OBSLOOK v1 requires IEEE754 binary64 doubles");

static ObsSdfEditValues sampled;
static bool valid;

bool obs_sdf_appearance_ready(void) { return valid; }
void obs_sdf_appearance_clear(void) {
    sampled = (ObsSdfEditValues){0};
    valid = false;
}
ObsSdfEditStatus obs_sdf_appearance_copy(uint32_t source_index) {
    /* A preview is not a durable material. Never sample a speculative edit
     * or steal an active viewport/inspector transaction. */
    if (obs_sdf_inspector_group_active()) return OBS_SDF_EDIT_CONFLICT;
    const uint64_t ticket = obs_sculpt_ticket_any_at(source_index);
    if (!ticket) return OBS_SDF_EDIT_STALE;
    ObsSdfEditValues replacement = {0};
    ObsSdfEditStatus st =
        obs_sdf_inspector_read_appearance(ticket, &replacement);
    if (st != OBS_SDF_EDIT_OK) return st;
    /* Preserve the prior sample unless the source was fully validated. */
    sampled = replacement;
    valid = true;
    return OBS_SDF_EDIT_OK;
}
ObsSdfEditStatus obs_sdf_appearance_paste(uint32_t target_index) {
    if (!valid) return OBS_SDF_EDIT_EMPTY;
    if (obs_sdf_inspector_group_active()) return OBS_SDF_EDIT_CONFLICT;
    /* One atomic material+emission journal command; geometry stays untouched. */
    return obs_sdf_property_apply(target_index,
        OBS_SDF_EDIT_MATERIAL | OBS_SDF_EDIT_EMISSION, &sampled);
}

/* OBSLOOK v1: 8-byte magic, little-endian version=1, payload length=80,
 * ten IEEE754 binary64 components, CRC32/IEEE of the first 96 bytes. */
#define OBS_LOOK_SIZE 100u
#define OBS_LOOK_DATA 80u
static const unsigned char look_magic[8] = {'O','B','S','L','O','O','K','1'};
static void put32(unsigned char *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8u*i));
}
static uint32_t get32(const unsigned char *p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (8u*i);
    return v;
}
static void put64(unsigned char *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8u*i));
}
static uint64_t get64(const unsigned char *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8u*i);
    return v;
}
static uint32_t checksum32(const unsigned char *p, size_t n) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static void to_fields(const ObsSdfEditValues *v, double d[10]) {
    for (unsigned i = 0; i < 4; ++i) d[i] = v->rgba[i];
    d[4] = v->roughness;
    d[5] = v->metallic;
    for (unsigned i = 0; i < 3; ++i) d[6+i] = v->emissive[i];
    d[9] = v->emissive_strength;
}
static void from_fields(const double d[10], ObsSdfEditValues *v) {
    *v = (ObsSdfEditValues){0};
    for (unsigned i = 0; i < 4; ++i) v->rgba[i] = d[i];
    v->roughness = d[4];
    v->metallic = d[5];
    for (unsigned i = 0; i < 3; ++i) v->emissive[i] = d[6+i];
    v->emissive_strength = d[9];
}
const char *obs_sdf_appearance_preset_path(void) {
    const char *env = getenv("OBSIDIAN_SDF_LOOK_PATH");
    return env && *env ? env : "obsidian-appearance.obslook";
}
bool obs_sdf_appearance_save_file(const char *path) {
    if (!path || !*path || !valid || obs_sdf_inspector_group_active() ||
        obs_sdf_inspector_validate_edit(
            OBS_SDF_EDIT_MATERIAL | OBS_SDF_EDIT_EMISSION, &sampled) !=
                OBS_SDF_EDIT_OK)
        return false;
    unsigned char data[OBS_LOOK_SIZE] = {0};
    memcpy(data, look_magic, sizeof(look_magic));
    put32(data + 8, 1);
    put32(data + 12, OBS_LOOK_DATA);
    double fields[10];
    to_fields(&sampled, fields);
    for (unsigned i = 0; i < 10; ++i) {
        uint64_t bits;
        memcpy(&bits, &fields[i], sizeof(bits));
        put64(data + 16 + i*8, bits);
    }
    put32(data + 96, checksum32(data, 96));
    return obs_atomic_write_bytes(path, data, sizeof(data));
}
bool obs_sdf_appearance_load_file(const char *path) {
    if (!path || !*path || obs_sdf_inspector_group_active()) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat sb;
    bool ok = fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) &&
              sb.st_size == (off_t)OBS_LOOK_SIZE;
    unsigned char data[OBS_LOOK_SIZE];
    size_t n = 0;
    while (ok && n < sizeof(data)) {
        ssize_t r = read(fd, data + n, sizeof(data) - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { ok = false; break; }
        n += (size_t)r;
    }
    if (close(fd) != 0) ok = false;
    if (!ok || memcmp(data, look_magic, sizeof(look_magic)) ||
        get32(data + 8) != 1 || get32(data + 12) != OBS_LOOK_DATA ||
        get32(data + 96) != checksum32(data, 96))
        return false;
    double fields[10];
    for (unsigned i = 0; i < 10; ++i) {
        uint64_t bits = get64(data + 16 + i*8);
        memcpy(&fields[i], &bits, sizeof(bits));
    }
    ObsSdfEditValues candidate;
    from_fields(fields, &candidate);
    if (obs_sdf_inspector_validate_edit(
            OBS_SDF_EDIT_MATERIAL | OBS_SDF_EDIT_EMISSION, &candidate) !=
        OBS_SDF_EDIT_OK)
        return false;
    sampled = candidate;
    valid = true;
    return true;
}
