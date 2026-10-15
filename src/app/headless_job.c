#define _POSIX_C_SOURCE 200809L
/// headless_job — headless result protocol shared by the real CLI and tests
#include "headless_job.h"
#include "atomic_file.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBS_AGENT_PATH_MAX 4096u

bool obs_headless_parse_frames(const char *text, uint32_t *out) {
    if (!text || !*text || !out) return false;
    uint32_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const uint32_t digit = *p - '0';
        if (value > (10000u - digit) / 10u) return false;
        value = 10u * value + digit;
    }
    if (!value) return false;
    *out = value;
    return true;
}

bool obs_headless_execute_frames(uint32_t count, ObsAgentFrameFn frame,
                                 void *userdata, uint32_t *completed) {
    if (completed) *completed = 0;
    if (!frame || !count || count > 10000u) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!frame(i, i + 1u == count, userdata)) return false;
        if (completed) *completed = i + 1u;
    }
    return true;
}

static char hex_digit(unsigned digit) {
    return "0123456789abcdef"[digit & 15u];
}

/* Returns a JSON string including quotes; never emits unescaped control
 * bytes or raw quote/backslash. Caller owns the result. */
static char *json_path(const char *path) {
    if (!path) return NULL;
    size_t length = strnlen(path, OBS_AGENT_PATH_MAX + 1u);
    if (!length || length > OBS_AGENT_PATH_MAX) return NULL;
    char *out = malloc(length * 6u + 3u);
    if (!out) return NULL;
    size_t n = 0;
    out[n++] = '"';
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (c == '"' || c == '\\') {
            out[n++] = '\\'; out[n++] = (char)c;
        } else if (c < 0x20u) {
            out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
            out[n++] = hex_digit(c >> 4); out[n++] = hex_digit(c);
        } else out[n++] = (char)c;
    }
    out[n++] = '"'; out[n] = '\0';
    return out;
}

bool obs_headless_write_receipt(const char *path, const ObsAgentReceipt *r) {
    if (!path || !r || !r->image_path || !r->completed_frames ||
        r->completed_frames > 10000u || !r->width || !r->height ||
        r->width > 8192u || r->height > 8192u ||
        r->source > OBS_AGENT_SOURCE_FILE ||
        strcmp(path, r->image_path) == 0) return false;
    char *encoded = json_path(r->image_path);
    if (!encoded) return false;
    const char *source = r->source == OBS_AGENT_SOURCE_FILE ? "file" :
                         r->source == OBS_AGENT_SOURCE_INLINE ? "inline" : "none";
    const uint64_t size = (uint64_t)r->width * r->height * 4u;
    int required = snprintf(NULL, 0,
        "{\"schema\":\"obsidian.headless.v1\",\"status\":\"ok\","
        "\"width\":%" PRIu32 ",\"height\":%" PRIu32 ","
        "\"frames\":%" PRIu32 ",\"format\":\"rgba8-unorm\","
        "\"bytes\":%" PRIu64 ",\"source\":\"%s\",\"image\":%s}\n",
        r->width, r->height, r->completed_frames, size, source, encoded);
    if (required < 0 || (size_t)required > SIZE_MAX - 1u) {
        free(encoded); return false;
    }
    char *content = malloc((size_t)required + 1u);
    if (!content) { free(encoded); return false; }
    int filled = snprintf(content, (size_t)required + 1u,
        "{\"schema\":\"obsidian.headless.v1\",\"status\":\"ok\","
        "\"width\":%" PRIu32 ",\"height\":%" PRIu32 ","
        "\"frames\":%" PRIu32 ",\"format\":\"rgba8-unorm\","
        "\"bytes\":%" PRIu64 ",\"source\":\"%s\",\"image\":%s}\n",
        r->width, r->height, r->completed_frames, size, source, encoded);
    bool ok = filled == required &&
        obs_atomic_write_bytes(path, content, (size_t)required);
    free(content);
    free(encoded);
    return ok;
}
