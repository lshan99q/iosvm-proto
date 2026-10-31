/* SPDX-License-Identifier: MIT */
/*
 * Strict in-process interpreter for the IOSVM GPU protocol.
 *
 * Everything here is deliberately literal: one flat byte heap, buffers and
 * RGBA8 images as contiguous extents in it, and validation before mutation so
 * that a refused command never changes the simulated state.
 */
#include "iosvm_fake_gpu.h"

#include <stdlib.h>
#include <string.h>

#include "iosvm_codec.h"

#define IOSVM_FAKE_MAX_RESOURCES 64u

enum fake_kind {
    IOSVM_FAKE_FREE = 0,
    IOSVM_FAKE_BUFFER = 1,
    IOSVM_FAKE_IMAGE = 2,
};

struct fake_res {
    uint32_t handle;
    uint32_t offset; /* byte offset into the heap */
    uint32_t size;   /* bytes */
    uint32_t width;  /* pixels, images only */
    uint32_t height; /* pixels, images only */
    uint8_t kind;
};

struct iosvm_fake_gpu {
    uint8_t *heap;
    uint32_t heap_size;
    uint32_t heap_used;
    struct fake_res res[IOSVM_FAKE_MAX_RESOURCES];
    uint32_t resources;
    uint32_t last_seq;
    uint32_t submits;
    uint32_t presents;
    uint32_t screenshots;
    uint32_t pending;
};

/* ------------------------------------------------------------------------- */
/* Resource table and heap                                                   */
/* ------------------------------------------------------------------------- */

static int fake_index(const struct iosvm_fake_gpu *gpu, uint32_t handle)
{
    uint32_t i;

    if (handle == 0u) {
        return -1;
    }
    for (i = 0; i < IOSVM_FAKE_MAX_RESOURCES; i++) {
        if (gpu->res[i].kind != IOSVM_FAKE_FREE && gpu->res[i].handle == handle) {
            return (int)i;
        }
    }
    return -1;
}

static struct fake_res *fake_find(struct iosvm_fake_gpu *gpu, uint32_t handle)
{
    int idx = fake_index(gpu, handle);

    return idx < 0 ? NULL : &gpu->res[(uint32_t)idx];
}

static const struct fake_res *fake_find_const(const struct iosvm_fake_gpu *gpu, uint32_t handle)
{
    int idx = fake_index(gpu, handle);

    return idx < 0 ? NULL : &gpu->res[(uint32_t)idx];
}

static struct fake_res *fake_free_slot(struct iosvm_fake_gpu *gpu)
{
    uint32_t i;

    for (i = 0; i < IOSVM_FAKE_MAX_RESOURCES; i++) {
        if (gpu->res[i].kind == IOSVM_FAKE_FREE) {
            return &gpu->res[i];
        }
    }
    return NULL;
}

/*
 * First fit over the live extents. Extents are 8-byte aligned and written in
 * place, so a freed resource is reusable by the next allocation of that size or
 * smaller. Returns IOSVM_ERR_FULL when nothing fits.
 */
static int32_t fake_alloc(struct iosvm_fake_gpu *gpu, uint32_t size, uint32_t *offset_out)
{
    uint32_t need;
    uint32_t cursor = 0u;

    if (size == 0u) {
        return IOSVM_ERR_BOUNDS;
    }
    need = iosvm_align8(size);
    if (need < size || need > gpu->heap_size) {
        return IOSVM_ERR_FULL;
    }

    for (;;) {
        uint32_t block_end = gpu->heap_size;
        uint32_t i;
        int32_t blocking = -1;

        for (i = 0; i < IOSVM_FAKE_MAX_RESOURCES; i++) {
            const struct fake_res *r = &gpu->res[i];

            if (r->kind == IOSVM_FAKE_FREE || r->offset < cursor) {
                continue;
            }
            if (r->offset < block_end) {
                block_end = r->offset;
                blocking = (int32_t)i;
            }
        }

        if (block_end - cursor >= need) {
            *offset_out = cursor;
            gpu->heap_used += need;
            return IOSVM_OK;
        }
        if (blocking < 0) {
            return IOSVM_ERR_FULL;
        }
        cursor =
            gpu->res[(uint32_t)blocking].offset + iosvm_align8(gpu->res[(uint32_t)blocking].size);
        if (cursor >= gpu->heap_size) {
            return IOSVM_ERR_FULL;
        }
    }
}

static void fake_release(struct iosvm_fake_gpu *gpu, struct fake_res *res)
{
    gpu->heap_used -= iosvm_align8(res->size);
    memset(res, 0, sizeof(*res));
    res->kind = IOSVM_FAKE_FREE;
    gpu->resources--;
}

/* Checks a handle for CREATE: non-zero, unused, and a free table slot exists. */
static int32_t fake_check_new_handle(struct iosvm_fake_gpu *gpu, uint32_t handle)
{
    if (handle == 0u) {
        return IOSVM_ERR_BAD_HANDLE;
    }
    if (fake_index(gpu, handle) >= 0) {
        return IOSVM_ERR_BAD_HANDLE;
    }
    if (fake_free_slot(gpu) == NULL) {
        return IOSVM_ERR_FULL;
    }
    return IOSVM_OK;
}

static int32_t fake_range(const struct fake_res *res, uint32_t offset, uint32_t len)
{
    if (len == 0u) {
        return IOSVM_ERR_BOUNDS;
    }
    if (offset > res->size || len > res->size - offset) {
        return IOSVM_ERR_BOUNDS;
    }
    return IOSVM_OK;
}

/* ------------------------------------------------------------------------- */
/* Payload helpers                                                           */
/* ------------------------------------------------------------------------- */

static int32_t fake_words(const struct iosvm_cmd *cmd, uint32_t *out, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        int32_t rc = iosvm_cmd_u32(cmd, i, &out[i]);

        if (rc != IOSVM_OK) {
            return rc;
        }
    }
    return IOSVM_OK;
}

/* ------------------------------------------------------------------------- */
/* Opcodes                                                                   */
/* ------------------------------------------------------------------------- */

static int32_t fake_create_buffer(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                                  uint32_t *handle_out)
{
    uint32_t a[4];
    uint32_t offset = 0u;
    struct fake_res *res;
    int32_t rc;

    rc = fake_words(cmd, a, 4u);
    if (rc != IOSVM_OK) {
        return rc;
    }
    /* a[0] handle, a[1] size, a[2] usage, a[3] flags */
    if (a[2] != 0u || a[3] != 0u) {
        return IOSVM_ERR_BOUNDS; /* reserved in v0.1 */
    }
    if (a[1] == 0u) {
        return IOSVM_ERR_BOUNDS;
    }
    rc = fake_check_new_handle(gpu, a[0]);
    if (rc != IOSVM_OK) {
        return rc;
    }
    rc = fake_alloc(gpu, a[1], &offset);
    if (rc != IOSVM_OK) {
        return rc;
    }

    res = fake_free_slot(gpu);
    if (res == NULL) {
        return IOSVM_ERR_FULL;
    }
    res->kind = IOSVM_FAKE_BUFFER;
    res->handle = a[0];
    res->offset = offset;
    res->size = a[1];
    gpu->resources++;
    *handle_out = a[0];
    return IOSVM_OK;
}

static int32_t fake_create_image(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                                 uint32_t *handle_out)
{
    uint32_t a[6];
    uint32_t offset = 0u;
    uint64_t bytes;
    struct fake_res *res;
    int32_t rc;

    rc = fake_words(cmd, a, 6u);
    if (rc != IOSVM_OK) {
        return rc;
    }
    /* a[0] handle, a[1] width, a[2] height, a[3] format, a[4] usage, a[5] flags */
    if (a[1] == 0u || a[1] > IOSVM_MAX_IMAGE_DIM || a[2] == 0u || a[2] > IOSVM_MAX_IMAGE_DIM) {
        return IOSVM_ERR_BOUNDS;
    }
    if (a[3] != (uint32_t)IOSVM_FORMAT_RGBA8) {
        return IOSVM_ERR_BOUNDS;
    }
    if (a[4] != 0u || a[5] != 0u) {
        return IOSVM_ERR_BOUNDS; /* reserved in v0.1 */
    }

    bytes = (uint64_t)a[1] * (uint64_t)a[2] * (uint64_t)IOSVM_RGBA8_BPP;
    if (bytes > (uint64_t)gpu->heap_size) {
        return IOSVM_ERR_FULL;
    }

    rc = fake_check_new_handle(gpu, a[0]);
    if (rc != IOSVM_OK) {
        return rc;
    }
    rc = fake_alloc(gpu, (uint32_t)bytes, &offset);
    if (rc != IOSVM_OK) {
        return rc;
    }

    res = fake_free_slot(gpu);
    if (res == NULL) {
        return IOSVM_ERR_FULL;
    }
    res->kind = IOSVM_FAKE_IMAGE;
    res->handle = a[0];
    res->offset = offset;
    res->size = (uint32_t)bytes;
    res->width = a[1];
    res->height = a[2];
    gpu->resources++;
    *handle_out = a[0];
    return IOSVM_OK;
}

static int32_t fake_destroy(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                            uint32_t *handle_out)
{
    uint32_t a[2];
    struct fake_res *res;
    int32_t rc;

    rc = fake_words(cmd, a, 2u);
    if (rc != IOSVM_OK) {
        return rc;
    }
    if (a[1] != 0u) {
        return IOSVM_ERR_BOUNDS; /* reserved in v0.1 */
    }
    res = fake_find(gpu, a[0]);
    if (res == NULL) {
        return IOSVM_ERR_BAD_HANDLE;
    }

    fake_release(gpu, res);
    *handle_out = a[0];
    return IOSVM_OK;
}

static int32_t fake_write(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                          uint32_t *handle_out)
{
    uint32_t a[4];
    struct fake_res *res;
    int32_t rc;

    rc = fake_words(cmd, a, 4u);
    if (rc != IOSVM_OK) {
        return rc;
    }
    /* a[0] handle, a[1] offset, a[2] len, a[3] reserved */
    if (a[3] != 0u) {
        return IOSVM_ERR_BOUNDS;
    }
    if (a[2] == 0u || a[2] > IOSVM_MAX_INLINE_WRITE) {
        return IOSVM_ERR_BOUNDS;
    }
    if (cmd->len < IOSVM_OP_WRITE_SIZE || a[2] > cmd->len - IOSVM_OP_WRITE_SIZE) {
        return IOSVM_ERR_TRUNCATED;
    }
    res = fake_find(gpu, a[0]);
    if (res == NULL) {
        return IOSVM_ERR_BAD_HANDLE;
    }
    rc = fake_range(res, a[1], a[2]);
    if (rc != IOSVM_OK) {
        return rc;
    }

    memcpy(gpu->heap + res->offset + a[1], cmd->payload + IOSVM_OP_WRITE_SIZE, (size_t)a[2]);
    gpu->pending++;
    *handle_out = a[0];
    return IOSVM_OK;
}

static int32_t fake_blit_copy(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                              uint32_t *handle_out)
{
    uint32_t a[6];
    struct fake_res *dst;
    struct fake_res *src;
    int32_t rc;

    rc = fake_words(cmd, a, 6u);
    if (rc != IOSVM_OK) {
        return rc;
    }
    /* a[0] dst, a[1] dst_offset, a[2] src, a[3] src_offset, a[4] len, a[5] reserved */
    if (a[5] != 0u || a[4] == 0u) {
        return IOSVM_ERR_BOUNDS;
    }
    dst = fake_find(gpu, a[0]);
    if (dst == NULL) {
        return IOSVM_ERR_BAD_HANDLE;
    }
    src = fake_find(gpu, a[2]);
    if (src == NULL) {
        return IOSVM_ERR_BAD_HANDLE;
    }
    rc = fake_range(src, a[3], a[4]);
    if (rc != IOSVM_OK) {
        return rc;
    }
    rc = fake_range(dst, a[1], a[4]);
    if (rc != IOSVM_OK) {
        return rc;
    }
    if (dst == src) {
        /* Overlapping ranges in one resource are undefined; refuse them. */
        uint64_t dst_end = (uint64_t)a[1] + (uint64_t)a[4];
        uint64_t src_end = (uint64_t)a[3] + (uint64_t)a[4];

        if ((uint64_t)a[3] < dst_end && (uint64_t)a[1] < src_end) {
            return IOSVM_ERR_BOUNDS;
        }
    }

    memmove(gpu->heap + dst->offset + a[1], gpu->heap + src->offset + a[3], (size_t)a[4]);
    gpu->pending++;
    *handle_out = a[0];
    return IOSVM_OK;
}

static int32_t fake_blit_fill(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                              uint32_t *handle_out)
{
    uint32_t a[4];
    struct fake_res *dst;
    uint32_t words;
    uint32_t i;
    int32_t rc;

    rc = fake_words(cmd, a, 4u);
    if (rc != IOSVM_OK) {
        return rc;
    }
    /* a[0] dst, a[1] dst_offset, a[2] len, a[3] value */
    if (a[2] == 0u || (a[2] % (uint32_t)sizeof(uint32_t)) != 0u) {
        return IOSVM_ERR_BOUNDS;
    }
    dst = fake_find(gpu, a[0]);
    if (dst == NULL) {
        return IOSVM_ERR_BAD_HANDLE;
    }
    rc = fake_range(dst, a[1], a[2]);
    if (rc != IOSVM_OK) {
        return rc;
    }

    words = a[2] / (uint32_t)sizeof(uint32_t);
    for (i = 0; i < words; i++) {
        iosvm_put_le32(gpu->heap + dst->offset + a[1] + (size_t)i * sizeof(uint32_t), a[3]);
    }
    gpu->pending++;
    *handle_out = a[0];
    return IOSVM_OK;
}

static int32_t fake_submit(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                           uint32_t *handle_out)
{
    uint32_t a[2];
    int32_t rc;

    rc = fake_words(cmd, a, 2u);
    if (rc != IOSVM_OK) {
        return rc;
    }
    /* a[0] cmd_count, a[1] flags */
    if (a[1] != 0u) {
        return IOSVM_ERR_BOUNDS;
    }
    if (a[0] == 0u || a[0] != gpu->pending) {
        /* Nothing to submit, or the guest miscounted its own batch. */
        return IOSVM_ERR_BOUNDS;
    }

    gpu->pending = 0u;
    gpu->submits++;
    *handle_out = gpu->submits; /* fence id for the matching FENCE_WAIT */
    return IOSVM_OK;
}

static int32_t fake_fence_wait(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                               uint32_t *handle_out)
{
    uint32_t a[2];
    int32_t rc;

    rc = fake_words(cmd, a, 2u);
    if (rc != IOSVM_OK) {
        return rc;
    }
    /* a[0] fence_id, a[1] timeout_ms (carried, not enforced) */
    if (gpu->pending != 0u) {
        return IOSVM_ERR_BOUNDS; /* batch not submitted yet */
    }
    if (a[0] == 0u || a[0] > gpu->submits) {
        return IOSVM_ERR_BOUNDS;
    }

    *handle_out = a[0];
    return IOSVM_OK;
}

static int32_t fake_present(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                            uint32_t *handle_out)
{
    uint32_t a[2];
    const struct fake_res *res;
    int32_t rc;

    rc = fake_words(cmd, a, 2u);
    if (rc != IOSVM_OK) {
        return rc;
    }
    /* a[0] image, a[1] reserved */
    if (a[1] != 0u) {
        return IOSVM_ERR_BOUNDS;
    }
    if (gpu->pending != 0u || gpu->submits == 0u) {
        return IOSVM_ERR_BOUNDS; /* nothing submitted: out of order */
    }
    res = fake_find_const(gpu, a[0]);
    if (res == NULL || res->kind != IOSVM_FAKE_IMAGE) {
        return IOSVM_ERR_BAD_HANDLE;
    }

    gpu->presents++;
    *handle_out = a[0];
    return IOSVM_OK;
}

static int32_t fake_screenshot(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                               uint32_t *handle_out)
{
    uint32_t a[6];
    const struct fake_res *image;
    const struct fake_res *dst;
    uint64_t bytes;
    uint32_t row;
    int32_t rc;

    rc = fake_words(cmd, a, 6u);
    if (rc != IOSVM_OK) {
        return rc;
    }
    /* a[0] image, a[1] dst, a[2] x, a[3] y, a[4] width, a[5] height */
    if (gpu->pending != 0u || gpu->submits == 0u) {
        return IOSVM_ERR_BOUNDS; /* capture the frame only after a submit */
    }
    image = fake_find_const(gpu, a[0]);
    if (image == NULL || image->kind != IOSVM_FAKE_IMAGE) {
        return IOSVM_ERR_BAD_HANDLE;
    }
    dst = fake_find_const(gpu, a[1]);
    if (dst == NULL || dst->kind != IOSVM_FAKE_BUFFER) {
        return IOSVM_ERR_BAD_HANDLE;
    }
    if (a[4] == 0u || a[5] == 0u) {
        return IOSVM_ERR_BOUNDS;
    }
    if ((uint64_t)a[2] + (uint64_t)a[4] > (uint64_t)image->width ||
        (uint64_t)a[3] + (uint64_t)a[5] > (uint64_t)image->height) {
        return IOSVM_ERR_BOUNDS;
    }
    bytes = (uint64_t)a[4] * (uint64_t)a[5] * (uint64_t)IOSVM_RGBA8_BPP;
    if (bytes > (uint64_t)dst->size) {
        return IOSVM_ERR_BOUNDS;
    }

    for (row = 0u; row < a[5]; row++) {
        size_t src_off =
            (size_t)image->offset + (((size_t)(a[3] + row) * (size_t)image->width) + (size_t)a[2]) *
                                        (size_t)IOSVM_RGBA8_BPP;
        size_t dst_off = (size_t)dst->offset + (size_t)row * (size_t)a[4] * (size_t)IOSVM_RGBA8_BPP;

        memcpy(gpu->heap + dst_off, gpu->heap + src_off, (size_t)a[4] * (size_t)IOSVM_RGBA8_BPP);
    }

    gpu->screenshots++;
    *handle_out = a[1];
    return IOSVM_OK;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

struct iosvm_fake_gpu *iosvm_fake_gpu_new(uint32_t memory_bytes)
{
    struct iosvm_fake_gpu *gpu;
    uint32_t bytes = memory_bytes == 0u ? IOSVM_FAKE_DEFAULT_MEMORY : memory_bytes;

    if (bytes == 0u || bytes > IOSVM_FAKE_MAX_MEMORY) {
        return NULL;
    }

    gpu = calloc(1u, sizeof(*gpu));
    if (gpu == NULL) {
        return NULL;
    }
    gpu->heap = calloc(1u, (size_t)bytes);
    if (gpu->heap == NULL) {
        free(gpu);
        return NULL;
    }
    gpu->heap_size = bytes;
    return gpu;
}

void iosvm_fake_gpu_free(struct iosvm_fake_gpu *gpu)
{
    if (gpu == NULL) {
        return;
    }
    free(gpu->heap);
    free(gpu);
}

int32_t iosvm_fake_gpu_exec(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                            uint32_t *handle)
{
    if (gpu == NULL || cmd == NULL || handle == NULL) {
        return IOSVM_ERR_BOUNDS;
    }
    *handle = 0u;

    if (cmd->magic != IOSVM_PROTO_MAGIC) {
        return IOSVM_ERR_UNKNOWN_OP;
    }
    if (cmd->seq == 0u || (gpu->last_seq != 0u && cmd->seq <= gpu->last_seq)) {
        return IOSVM_ERR_SEQ;
    }
    /* The sequence number is consumed even when the op itself fails. */
    gpu->last_seq = cmd->seq;

    switch (cmd->op) {
    case IOSVM_OP_CREATE_BUFFER:
        return fake_create_buffer(gpu, cmd, handle);
    case IOSVM_OP_CREATE_IMAGE:
        return fake_create_image(gpu, cmd, handle);
    case IOSVM_OP_DESTROY:
        return fake_destroy(gpu, cmd, handle);
    case IOSVM_OP_WRITE:
        return fake_write(gpu, cmd, handle);
    case IOSVM_OP_BLIT_COPY:
        return fake_blit_copy(gpu, cmd, handle);
    case IOSVM_OP_BLIT_FILL:
        return fake_blit_fill(gpu, cmd, handle);
    case IOSVM_OP_SUBMIT:
        return fake_submit(gpu, cmd, handle);
    case IOSVM_OP_FENCE_WAIT:
        return fake_fence_wait(gpu, cmd, handle);
    case IOSVM_OP_PRESENT:
        return fake_present(gpu, cmd, handle);
    case IOSVM_OP_SCREENSHOT:
        return fake_screenshot(gpu, cmd, handle);
    default:
        return IOSVM_ERR_UNKNOWN_OP;
    }
}

int32_t iosvm_fake_gpu_readback(const struct iosvm_fake_gpu *gpu, uint32_t handle, uint32_t offset,
                                void *dst, uint32_t len)
{
    const struct fake_res *res;
    int32_t rc;

    if (gpu == NULL || dst == NULL) {
        return IOSVM_ERR_BOUNDS;
    }
    res = fake_find_const(gpu, handle);
    if (res == NULL) {
        return IOSVM_ERR_BAD_HANDLE;
    }
    rc = fake_range(res, offset, len);
    if (rc != IOSVM_OK) {
        return rc;
    }

    memcpy(dst, gpu->heap + res->offset + offset, (size_t)len);
    return IOSVM_OK;
}

uint32_t iosvm_fake_gpu_size(const struct iosvm_fake_gpu *gpu, uint32_t handle)
{
    const struct fake_res *res;

    if (gpu == NULL) {
        return 0u;
    }
    res = fake_find_const(gpu, handle);
    return res == NULL ? 0u : res->size;
}

uint32_t iosvm_fake_gpu_memory_used(const struct iosvm_fake_gpu *gpu)
{
    return gpu == NULL ? 0u : gpu->heap_used;
}

void iosvm_fake_gpu_stats(const struct iosvm_fake_gpu *gpu, struct iosvm_fake_stats *out)
{
    if (gpu == NULL || out == NULL) {
        return;
    }
    out->submits = gpu->submits;
    out->presents = gpu->presents;
    out->screenshots = gpu->screenshots;
    out->pending = gpu->pending;
    out->resources = gpu->resources;
    out->fence_id = gpu->submits;
}
