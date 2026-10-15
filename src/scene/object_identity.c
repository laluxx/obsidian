/// object_identity.c — stable paged slots with non-repeating generations
#include "object_identity.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// Bounds and storage
static bool view_ok(ObsIdentityView v) {
    return (v.kind == OBS_ID_MESH || v.kind == OBS_ID_NODE ||
            v.kind == OBS_ID_MATERIAL) &&
           v.stride >= sizeof(uint64_t) &&
           v.identity_offset <= v.stride - sizeof(uint64_t) &&
           (!v.count || v.items) &&
           (!v.stride || v.count <= SIZE_MAX / v.stride);
}
static ObsIdentitySlot *slot_at(const ObsIdentityRegistry *r, uint32_t index) {
    if (!r || index >= r->issued || !r->pages ||
        index / OBS_ID_PAGE_SIZE >= r->page_count) return NULL;
    return &r->pages[index / OBS_ID_PAGE_SIZE]->slots[index % OBS_ID_PAGE_SIZE];
}
static uint64_t item_id(ObsIdentityView v, size_t index) {
    uint64_t result;
    const unsigned char *p = (const unsigned char *)v.items + index * v.stride + v.identity_offset;
    memcpy(&result, p, sizeof(result));
    return result;
}
static void set_item_id(ObsIdentityView v, size_t index, uint64_t value) {
    unsigned char *p = (unsigned char *)v.items + index * v.stride + v.identity_offset;
    memcpy(p, &value, sizeof(value));
}
static uint64_t pack(uint32_t index, uint32_t generation) {
    return ((uint64_t)generation << 32) | ((uint64_t)index + 1);
}
static bool unpack(uint64_t id, uint32_t *index, uint32_t *generation) {
    uint32_t lo = (uint32_t)id;
    uint32_t hi = (uint32_t)(id >> 32);
    if (!lo || !hi) return false;
    *index = lo - 1;
    *generation = hi;
    return true;
}
static bool slot_for_token(const ObsIdentityRegistry *r, const void *token,
                           uint32_t *index, ObsIdentitySlot **slot) {
    /* Integer address comparison never dereferences untrusted foreign data.
     * Only exact, aligned addresses within our allocated pages are accepted. */
    uintptr_t addr = (uintptr_t)token;
    if (!addr || !r) return false;
    for (size_t p = 0; p < r->page_count; ++p) {
        uintptr_t start = (uintptr_t)&r->pages[p]->slots[0];
        if (addr < start || addr - start >= sizeof(r->pages[p]->slots)) continue;
        uintptr_t offset = addr - start;
        if (offset % sizeof(ObsIdentitySlot)) return false;
        size_t pos = p * OBS_ID_PAGE_SIZE + offset / sizeof(ObsIdentitySlot);
        if (pos >= r->issued) return false;
        if (index) *index = (uint32_t)pos;
        if (slot) *slot = slot_at(r, (uint32_t)pos);
        return true;
    }
    return false;
}
static ObsIdentitySlot *new_slot(ObsIdentityRegistry *r, uint32_t *index) {
    if (r->free_head) {
        uint32_t i = r->free_head - 1;
        ObsIdentitySlot *s = slot_at(r, i);
        if (!s || !s->generation || s->live) return NULL;
        r->free_head = s->next_free;
        s->next_free = 0;
        *index = i;
        return s;
    }
    if (r->issued == UINT32_MAX) return NULL;
    if (r->issued / OBS_ID_PAGE_SIZE == r->page_count) {
        if (r->page_count == r->page_capacity) {
            size_t capacity = r->page_capacity ? r->page_capacity * 2 : 4;
            if (capacity <= r->page_capacity || capacity > SIZE_MAX / sizeof(*r->pages))
                return NULL;
            ObsIdentityPage **pages = realloc(r->pages, capacity * sizeof(*pages));
            if (!pages) return NULL;
            r->pages = pages;
            r->page_capacity = capacity;
        }
        ObsIdentityPage *page = calloc(1, sizeof(*page));
        if (!page) return NULL;
        r->pages[r->page_count++] = page;
    }
    uint32_t i = r->issued++;
    ObsIdentitySlot *s = slot_at(r, i);
    s->generation = 1;
    *index = i;
    return s;
}

/// Synchronization and reclamation
bool obs_identity_sync(ObsIdentityRegistry *r, ObsIdentityView v) {
    if (!r || !view_ok(v)) return false;
    if (r->synced[v.kind] && r->synced_revision[v.kind] == v.revision) return true;
    for (uint32_t i = 0; i < r->issued; ++i) {
        ObsIdentitySlot *s = slot_at(r, i);
        if (s->live && s->kind == v.kind) s->seen = false;
    }
    for (size_t i = 0; i < v.count; ++i) {
        uint32_t token_index, token_gen;
        uint64_t id = item_id(v, i);
        if (!id) continue;
        ObsIdentitySlot *s = NULL;
        if (unpack(id, &token_index, &token_gen)) s = slot_at(r, token_index);
        if (!s || !s->live || s->kind != v.kind || s->generation != token_gen || s->seen) {
            /* Reject duplicated or invalid legacy IDs. No stale aliasing. */
            set_item_id(v, i, 0);
            continue;
        }
        s->seen = true;
        s->dense_index = i;
    }
    for (uint32_t i = 0; i < r->issued; ++i) {
        ObsIdentitySlot *s = slot_at(r, i);
        if (!s->live || s->kind != v.kind || s->seen) continue;
        s->live = false;
        /* Never wrap generations: a retired slot is never reused. */
        if (s->generation != UINT32_MAX) {
            ++s->generation;
            s->next_free = r->free_head;
            r->free_head = i + 1;
        }
    }
    r->synced_revision[v.kind] = v.revision;
    r->synced[v.kind] = true;
    return true;
}

/// Borrow and resolve
bool obs_identity_borrow(ObsIdentityRegistry *r, ObsIdentityView v, size_t index,
                         const void **token, uint32_t *generation) {
    if (!token || !generation || !obs_identity_sync(r, v) || index >= v.count) return false;
    uint32_t token_index = 0, token_gen = 0;
    uint64_t id = item_id(v, index);
    ObsIdentitySlot *s = NULL;
    if (unpack(id, &token_index, &token_gen)) {
        s = slot_at(r, token_index);
        if (!s || !s->live || s->kind != v.kind || s->generation != token_gen ||
            s->dense_index != index) s = NULL;
    }
    if (!s) {
        s = new_slot(r, &token_index);
        if (!s) return false;
        s->kind = v.kind;
        s->dense_index = index;
        s->live = true;
        s->seen = true;
        set_item_id(v, index, pack(token_index, s->generation));
    }
    *token = s;
    *generation = s->generation;
    return true;
}
bool obs_identity_generation(ObsIdentityRegistry *r, ObsIdentityView v,
                             const void *token, uint32_t *generation) {
    ObsIdentitySlot *s = NULL;
    if (!generation || !obs_identity_sync(r, v) ||
        !slot_for_token(r, token, NULL, &s) || !s || !s->live || s->kind != v.kind)
        return false;
    *generation = s->generation;
    return true;
}
bool obs_identity_resolve(ObsIdentityRegistry *r, ObsIdentityView v,
                          const void *token, uint32_t generation, size_t *index) {
    if (!index || !obs_identity_sync(r, v)) return false;
    uint32_t token_index = 0;
    ObsIdentitySlot *s = NULL;
    if (!slot_for_token(r, token, &token_index, &s) || !s || !s->live ||
        s->kind != v.kind || !generation || s->generation != generation ||
        s->dense_index >= v.count ||
        item_id(v, s->dense_index) != pack(token_index, generation)) return false;
    *index = s->dense_index;
    return true;
}
void obs_identity_destroy(ObsIdentityRegistry *r) {
    if (!r) return;
    for (size_t i = 0; i < r->page_count; ++i) free(r->pages[i]);
    free(r->pages);
    memset(r, 0, sizeof(*r));
}
