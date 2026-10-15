#define _POSIX_C_SOURCE 200809L
/// atomic_file — production offscreen output and agent receipt publication
#include "atomic_file.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OBS_ATOMIC_PATH_MAX 4096u

bool obs_atomic_write_bytes(const char *path, const void *data, size_t size) {
    static const char suffix[] = ".tmp.XXXXXX";
    if (!path || !*path || (!data && size)) return false;
    const size_t length = strnlen(path, OBS_ATOMIC_PATH_MAX + 1u);
    if (!length || length > OBS_ATOMIC_PATH_MAX ||
        length > SIZE_MAX - sizeof(suffix)) return false;
    char *temporary = malloc(length + sizeof(suffix));
    if (!temporary) return false;
    memcpy(temporary, path, length);
    memcpy(temporary + length, suffix, sizeof(suffix));
    int fd = mkstemp(temporary);
    if (fd < 0) { free(temporary); return false; }
    bool ok = true;
    const unsigned char *bytes = data;
    size_t used = 0;
    while (used < size) {
        /* Cap to SSIZE_MAX so both the POSIX return type and offset are safe. */
        const size_t amount = size - used > 1048576u ? 1048576u : size - used;
        ssize_t written = write(fd, bytes + used, amount);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        used += (size_t)written;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(temporary, path) != 0) ok = false;
    if (!ok) unlink(temporary);
    free(temporary);
    return ok;
}
