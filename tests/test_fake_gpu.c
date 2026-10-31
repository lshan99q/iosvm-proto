/* SPDX-License-Identifier: MIT */
/*
 * Fake GPU interpreter: resource handling, transfer ops, ordering rules,
 * sequence enforcement and the guarantee that a refused command mutates
 * nothing.
 */
#include "tests.h"

static uint8_t g_buf[IOSVM_MAX_RECORD_SIZE];
static struct iosvm_fake_gpu *g_gpu;

static void new_gpu(uint32_t bytes)
{
    if (g_gpu != NULL) {
        iosvm_fake_gpu_free(g_gpu);
    }
    g_gpu = iosvm_fake_gpu_new(bytes);
}

static int32_t exec_one(int32_t written, uint32_t *handle)
{
    struct iosvm_cmd cmd;
    int32_t rc;

    if (written < 0) {
        return written;
    }
    rc = iosvm_decode_one(g_buf, (size_t)written, &cmd);
    if (rc != written) {
        return IOSVM_ERR_TRUNCATED;
    }
    return iosvm_fake_gpu_exec(g_gpu, &cmd, handle);
}

/* Build a fixed-width record out of payload words and execute it. */
static int32_t run_cmd(uint16_t op, uint32_t seq, const uint32_t *words, uint32_t nwords,
                       uint32_t *handle)
{
    int32_t total;
    uint32_t i;

    total = iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION, op,
                                 seq, nwords * 4u);
    if (total < 0) {
        return total;
    }
    for (i = 0u; i < nwords; i++) {
        iosvm_put_le32(g_buf + IOSVM_CMD_HDR_SIZE + (size_t)i * 4u, words[i]);
    }
    return exec_one(total + (int32_t)(nwords * 4u), handle);
}

static int32_t run_write(uint32_t seq, uint32_t handle, uint32_t offset, const void *data,
                         uint32_t len, uint32_t *out)
{
    return exec_one(iosvm_encode_write(g_buf, sizeof(g_buf), seq, handle, offset, data, len), out);
}

static void case_create_destroy(void)
{
    uint32_t w[6];
    uint32_t h = 0u;
    uint32_t seq = 1u;

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    w[0] = 1u;
    w[1] = 4096u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);
    CHECK_EQ(h, 1u);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 1u), 4096u);
    CHECK_EQ(iosvm_fake_gpu_memory_used(g_gpu), 4096u);

    /* Duplicate handle. */
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_ERR_BAD_HANDLE);
    /* Handle zero is not a resource. */
    w[0] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_ERR_BAD_HANDLE);
    /* Zero size is refused. */
    w[0] = 2u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 2u), 0u);
    CHECK_EQ(iosvm_fake_gpu_memory_used(g_gpu), 4096u);

    /* Image with a usable size. */
    w[0] = 3u;
    w[1] = 4u;
    w[2] = 4u;
    w[3] = (uint32_t)IOSVM_FORMAT_RGBA8;
    w[4] = 0u;
    w[5] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_IMAGE, seq++, w, 6u, &h), IOSVM_OK);
    CHECK_EQ(h, 3u);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 3u), 4u * 4u * 4u);

    /* DESTROY of an occupied and of an unknown handle. */
    w[0] = 1u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_DESTROY, seq++, w, 2u, &h), IOSVM_OK);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 1u), 0u);
    CHECK_EQ(run_cmd(IOSVM_OP_DESTROY, seq++, w, 2u, &h), IOSVM_ERR_BAD_HANDLE);
    CHECK_EQ(iosvm_fake_gpu_memory_used(g_gpu), 64u);

    /* A destroyed handle can be recreated. */
    w[0] = 1u;
    w[1] = 8u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);
}

static void case_write(void)
{
    static const uint8_t data[8] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
    uint8_t back[8];
    uint32_t w[4];
    uint32_t h = 0u;
    uint32_t seq = 1u;
    uint32_t i;

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    w[0] = 1u;
    w[1] = 64u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);

    CHECK_EQ(run_write(seq++, 1u, 16u, data, sizeof(data), &h), IOSVM_OK);
    CHECK_EQ(h, 1u);
    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 1u, 16u, back, sizeof(back)), IOSVM_OK);
    CHECK_MEM(back, data, sizeof(back));

    /* Neighbouring bytes are untouched. */
    for (i = 0u; i < 16u; i++) {
        back[0] = 0xffu;
        CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 1u, i, back, 1u), IOSVM_OK);
        CHECK_EQ(back[0], 0u);
    }

    /* Out of bounds and unknown handle are refused. */
    CHECK_EQ(run_write(seq++, 1u, 60u, data, sizeof(data), &h), IOSVM_ERR_BOUNDS);
    CHECK_EQ(run_write(seq++, 1u, 4096u, data, sizeof(data), &h), IOSVM_ERR_BOUNDS);
    CHECK_EQ(run_write(seq++, 99u, 0u, data, sizeof(data), &h), IOSVM_ERR_BAD_HANDLE);
    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 1u, 24u, back, sizeof(back)), IOSVM_OK);
    for (i = 0u; i < sizeof(back); i++) {
        CHECK_EQ(back[i], 0u);
    }

    /* A readback past the end is refused rather than clamped. */
    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 1u, 60u, back, sizeof(back)), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 1u, 0u, back, 0u), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 9u, 0u, back, sizeof(back)), IOSVM_ERR_BAD_HANDLE);
}

static void case_blit_copy(void)
{
    uint8_t src[32];
    uint8_t back[16];
    uint32_t w[6];
    uint32_t h = 0u;
    uint32_t seq = 1u;
    uint32_t i;

    for (i = 0u; i < sizeof(src); i++) {
        src[i] = (uint8_t)(0x80u + i);
    }

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    w[0] = 1u;
    w[1] = 32u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);
    w[0] = 2u;
    w[1] = 64u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);
    CHECK_EQ(run_write(seq++, 1u, 0u, src, sizeof(src), &h), IOSVM_OK);

    /* 16 bytes from src[8] to dst[32]. */
    w[0] = 2u;
    w[1] = 32u;
    w[2] = 1u;
    w[3] = 8u;
    w[4] = 16u;
    w[5] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_COPY, seq++, w, 6u, &h), IOSVM_OK);
    CHECK_EQ(h, 2u);
    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 2u, 32u, back, sizeof(back)), IOSVM_OK);
    CHECK_MEM(back, src + 8, sizeof(back));

    /* Bad source handle, out of bounds on either side, zero length. */
    w[2] = 99u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_COPY, seq++, w, 6u, &h), IOSVM_ERR_BAD_HANDLE);
    w[2] = 1u;
    w[3] = 24u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_COPY, seq++, w, 6u, &h), IOSVM_ERR_BOUNDS);
    w[3] = 0u;
    w[4] = 65u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_COPY, seq++, w, 6u, &h), IOSVM_ERR_BOUNDS);
    w[4] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_COPY, seq++, w, 6u, &h), IOSVM_ERR_BOUNDS);

    /* Overlapping ranges inside one resource are undefined. */
    w[0] = 2u;
    w[1] = 8u;
    w[2] = 2u;
    w[3] = 0u;
    w[4] = 16u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_COPY, seq++, w, 6u, &h), IOSVM_ERR_BOUNDS);

    /* Disjoint ranges inside one resource are fine. */
    w[1] = 48u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_COPY, seq++, w, 6u, &h), IOSVM_OK);
}

static void case_blit_fill(void)
{
    uint32_t words[4];
    uint32_t w[4];
    uint32_t h = 0u;
    uint32_t seq = 1u;
    uint32_t i;

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    w[0] = 1u;
    w[1] = 64u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);

    w[0] = 1u;
    w[1] = 8u;
    w[2] = 16u;
    w[3] = 0xdeadbeefu;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_FILL, seq++, w, 4u, &h), IOSVM_OK);
    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 1u, 8u, words, sizeof(words)), IOSVM_OK);
    for (i = 0u; i < 4u; i++) {
        CHECK_EQ(words[i], 0xdeadbeefu);
    }

    /* Length must be a non-zero multiple of 4 and inside the resource. */
    w[2] = 6u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_FILL, seq++, w, 4u, &h), IOSVM_ERR_BOUNDS);
    w[2] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_FILL, seq++, w, 4u, &h), IOSVM_ERR_BOUNDS);
    w[2] = 16u;
    w[1] = 60u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_FILL, seq++, w, 4u, &h), IOSVM_ERR_BOUNDS);
    w[1] = 0u;
    w[0] = 42u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_FILL, seq++, w, 4u, &h), IOSVM_ERR_BAD_HANDLE);
}

static void case_submit(void)
{
    struct iosvm_fake_stats st;
    uint8_t data[4] = {1u, 2u, 3u, 4u};
    uint32_t w[4];
    uint32_t h = 0u;
    uint32_t seq = 1u;

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    /* Nothing queued. */
    w[0] = 1u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_SUBMIT, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);

    w[0] = 1u;
    w[1] = 16u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);
    CHECK_EQ(run_write(seq++, 1u, 0u, data, sizeof(data), &h), IOSVM_OK);

    /* The guest miscounted its own batch. */
    w[0] = 2u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_SUBMIT, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);
    CHECK_EQ(h, 0u);
    w[0] = 1u;
    CHECK_EQ(run_cmd(IOSVM_OP_SUBMIT, seq++, w, 2u, &h), IOSVM_OK);
    CHECK_EQ(h, 1u); /* fence id */

    /* The batch is closed: resubmitting it is out of order. */
    CHECK_EQ(run_cmd(IOSVM_OP_SUBMIT, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);
    /* A new batch needs new commands. */
    CHECK_EQ(run_write(seq++, 1u, 4u, data, sizeof(data), &h), IOSVM_OK);
    CHECK_EQ(run_cmd(IOSVM_OP_SUBMIT, seq++, w, 2u, &h), IOSVM_OK);
    CHECK_EQ(h, 2u);

    iosvm_fake_gpu_stats(g_gpu, &st);
    CHECK_EQ(st.submits, 2u);
    CHECK_EQ(st.fence_id, 2u);
    CHECK_EQ(st.pending, 0u);
    CHECK_EQ(st.resources, 1u);
}

static void case_fence_wait(void)
{
    struct iosvm_fake_stats st;
    uint8_t data[4] = {1u, 2u, 3u, 4u};
    uint32_t w[4];
    uint32_t h = 0u;
    uint32_t seq = 1u;

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    w[0] = 1u;
    w[1] = 16u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);

    /* No submit yet: there is nothing to wait for. */
    w[0] = 1u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_FENCE_WAIT, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);

    CHECK_EQ(run_write(seq++, 1u, 0u, data, sizeof(data), &h), IOSVM_OK);
    /* A batch is still open. */
    CHECK_EQ(run_cmd(IOSVM_OP_FENCE_WAIT, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);

    w[0] = 1u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_SUBMIT, seq++, w, 2u, &h), IOSVM_OK);

    /* Fence 2 does not exist yet. */
    w[0] = 2u;
    w[1] = 1000u;
    CHECK_EQ(run_cmd(IOSVM_OP_FENCE_WAIT, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);
    w[0] = 1u;
    CHECK_EQ(run_cmd(IOSVM_OP_FENCE_WAIT, seq++, w, 2u, &h), IOSVM_OK);
    CHECK_EQ(h, 1u);
    w[0] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_FENCE_WAIT, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);

    iosvm_fake_gpu_stats(g_gpu, &st);
    CHECK_EQ(st.submits, 1u);
}

static void case_present_and_screenshot(void)
{
    struct iosvm_fake_stats st;
    uint32_t w[6];
    uint32_t h = 0u;
    uint32_t seq = 1u;
    uint8_t pixels[64];
    uint32_t i;

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    /* 4x4 image (64 bytes) and a buffer big enough to hold a capture. */
    w[0] = 1u;
    w[1] = 4u;
    w[2] = 4u;
    w[3] = (uint32_t)IOSVM_FORMAT_RGBA8;
    w[4] = 0u;
    w[5] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_IMAGE, seq++, w, 6u, &h), IOSVM_OK);
    w[0] = 2u;
    w[1] = 64u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);
    w[0] = 3u;
    w[1] = 8u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);

    /* Present before any submit is out of order. */
    w[0] = 1u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_PRESENT, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);

    /* Paint the image with a repeated 32-bit pattern. */
    w[0] = 1u;
    w[1] = 0u;
    w[2] = 64u;
    w[3] = 0x11223344u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_FILL, seq++, w, 4u, &h), IOSVM_OK);

    /* Capturing or presenting a batch that is still open is out of order. */
    w[0] = 1u;
    w[1] = 2u;
    w[2] = 0u;
    w[3] = 0u;
    w[4] = 4u;
    w[5] = 4u;
    CHECK_EQ(run_cmd(IOSVM_OP_SCREENSHOT, seq++, w, 6u, &h), IOSVM_ERR_BOUNDS);

    w[0] = 1u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_SUBMIT, seq++, w, 2u, &h), IOSVM_OK);

    w[0] = 1u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_PRESENT, seq++, w, 2u, &h), IOSVM_OK);
    /* A buffer is not an image. */
    w[0] = 2u;
    CHECK_EQ(run_cmd(IOSVM_OP_PRESENT, seq++, w, 2u, &h), IOSVM_ERR_BAD_HANDLE);
    w[0] = 9u;
    CHECK_EQ(run_cmd(IOSVM_OP_PRESENT, seq++, w, 2u, &h), IOSVM_ERR_BAD_HANDLE);

    /* Capture the right half of every row. */
    w[0] = 1u;
    w[1] = 2u;
    w[2] = 2u;
    w[3] = 0u;
    w[4] = 2u;
    w[5] = 4u;
    CHECK_EQ(run_cmd(IOSVM_OP_SCREENSHOT, seq++, w, 6u, &h), IOSVM_OK);
    CHECK_EQ(h, 2u);
    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 2u, 0u, pixels, 32u), IOSVM_OK);
    for (i = 0u; i < 32u; i++) {
        if ((i % 4u) == 0u) {
            CHECK_EQ(pixels[i], 0x44u);
        }
        if ((i % 4u) == 1u) {
            CHECK_EQ(pixels[i], 0x33u);
        }
        if ((i % 4u) == 2u) {
            CHECK_EQ(pixels[i], 0x22u);
        }
        if ((i % 4u) == 3u) {
            CHECK_EQ(pixels[i], 0x11u);
        }
    }

    /* Out of the image and too small a destination. */
    w[1] = 2u;
    w[2] = 3u;
    w[4] = 2u;
    CHECK_EQ(run_cmd(IOSVM_OP_SCREENSHOT, seq++, w, 6u, &h), IOSVM_ERR_BOUNDS);
    w[1] = 3u;
    w[2] = 0u;
    w[4] = 4u;
    w[5] = 4u;
    CHECK_EQ(run_cmd(IOSVM_OP_SCREENSHOT, seq++, w, 6u, &h), IOSVM_ERR_BOUNDS);
    w[1] = 2u;
    w[0] = 2u;
    CHECK_EQ(run_cmd(IOSVM_OP_SCREENSHOT, seq++, w, 6u, &h), IOSVM_ERR_BAD_HANDLE);

    iosvm_fake_gpu_stats(g_gpu, &st);
    CHECK_EQ(st.submits, 1u);
    CHECK_EQ(st.presents, 1u);
    CHECK_EQ(st.screenshots, 1u);
}

static void case_seq(void)
{
    uint32_t w[4];
    uint32_t h = 0u;

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    w[0] = 1u;
    w[1] = 16u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, 5u, w, 4u, &h), IOSVM_OK);
    /* Backwards and repeated sequence numbers are refused. */
    CHECK_EQ(run_cmd(IOSVM_OP_DESTROY, 4u, w, 2u, &h), IOSVM_ERR_SEQ);
    CHECK_EQ(run_cmd(IOSVM_OP_DESTROY, 5u, w, 2u, &h), IOSVM_ERR_SEQ);
    CHECK_EQ(run_cmd(IOSVM_OP_DESTROY, 0u, w, 2u, &h), IOSVM_ERR_SEQ);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 1u), 16u);
    /* Forward progress still works. */
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_DESTROY, 6u, w, 2u, &h), IOSVM_OK);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 1u), 0u);
}

static void case_out_of_memory(void)
{
    struct iosvm_fake_stats st;
    uint32_t w[6];
    uint32_t h = 0u;
    uint32_t seq = 1u;
    uint32_t i;

    new_gpu(8192u);
    CHECK(g_gpu != NULL);

    w[0] = 1u;
    w[1] = 8192u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);

    /* A large allocation that has no room fails without reserving anything. */
    w[0] = 2u;
    w[1] = 8u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_ERR_FULL);
    CHECK_EQ(iosvm_fake_gpu_memory_used(g_gpu), 8192u);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 2u), 0u);

    /* Freeing the big buffer makes the space usable again. */
    w[0] = 1u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_DESTROY, seq++, w, 2u, &h), IOSVM_OK);
    w[0] = 2u;
    w[1] = 8192u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);

    iosvm_fake_gpu_stats(g_gpu, &st);
    CHECK_EQ(st.resources, 1u);

    /* Images that do not fit the heap are refused the same way. */
    w[0] = 3u;
    w[1] = 1024u;
    w[2] = 1024u;
    w[3] = (uint32_t)IOSVM_FORMAT_RGBA8;
    w[4] = 0u;
    w[5] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_IMAGE, seq++, w, 6u, &h), IOSVM_ERR_FULL);

    /* Fill the default 8 MiB heap with 1 MiB buffers. */
    new_gpu(0u);
    CHECK(g_gpu != NULL);
    seq = 1u;
    for (i = 0u; i < 8u; i++) {
        w[0] = 100u + i;
        w[1] = 1024u * 1024u;
        w[2] = 0u;
        w[3] = 0u;
        CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);
    }
    w[0] = 999u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_ERR_FULL);
}

static void case_reserved_fields(void)
{
    uint32_t w[6];
    uint32_t h = 0u;
    uint32_t seq = 1u;

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    /* CREATE_BUFFER: usage and flags are reserved. */
    w[0] = 1u;
    w[1] = 16u;
    w[2] = 1u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_ERR_BOUNDS);
    w[2] = 0u;
    w[3] = 1u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_ERR_BOUNDS);
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 1u), 16u);

    /* CREATE_IMAGE: only RGBA8, no flags. */
    w[0] = 2u;
    w[1] = 2u;
    w[2] = 2u;
    w[3] = 9u;
    w[4] = 0u;
    w[5] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_IMAGE, seq++, w, 6u, &h), IOSVM_ERR_BOUNDS);
    w[3] = (uint32_t)IOSVM_FORMAT_RGBA8;
    w[5] = 1u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_IMAGE, seq++, w, 6u, &h), IOSVM_ERR_BOUNDS);
    w[5] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_IMAGE, seq++, w, 6u, &h), IOSVM_OK);

    /* DESTROY reserved word. */
    w[0] = 1u;
    w[1] = 1u;
    CHECK_EQ(run_cmd(IOSVM_OP_DESTROY, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 1u), 16u);

    /* SUBMIT flags. */
    w[0] = 1u;
    w[1] = 1u;
    CHECK_EQ(run_cmd(IOSVM_OP_SUBMIT, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);

    /* A real batch, then PRESENT with a reserved word set. */
    w[0] = 1u;
    w[1] = 0u;
    w[2] = 4u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_BLIT_FILL, seq++, w, 4u, &h), IOSVM_OK);
    w[0] = 1u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_SUBMIT, seq++, w, 2u, &h), IOSVM_OK);
    w[0] = 2u;
    w[1] = 1u;
    CHECK_EQ(run_cmd(IOSVM_OP_PRESENT, seq++, w, 2u, &h), IOSVM_ERR_BOUNDS);
}

static void case_refused_command_mutates_nothing(void)
{
    struct iosvm_fake_stats before;
    struct iosvm_fake_stats after;
    static const uint8_t data[4] = {9u, 8u, 7u, 6u};
    uint8_t back[4];
    uint32_t w[4];
    uint32_t h = 0u;
    uint32_t seq = 1u;

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    w[0] = 1u;
    w[1] = 16u;
    w[2] = 0u;
    w[3] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_BUFFER, seq++, w, 4u, &h), IOSVM_OK);
    CHECK_EQ(run_write(seq++, 1u, 0u, data, sizeof(data), &h), IOSVM_OK);
    iosvm_fake_gpu_stats(g_gpu, &before);

    CHECK_EQ(run_write(seq++, 1u, 13u, data, sizeof(data), &h), IOSVM_ERR_BOUNDS);
    iosvm_fake_gpu_stats(g_gpu, &after);
    CHECK_EQ(after.pending, before.pending);
    CHECK_EQ(after.resources, before.resources);
    CHECK_EQ(iosvm_fake_gpu_memory_used(g_gpu), 16u);
    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 1u, 0u, back, sizeof(back)), IOSVM_OK);
    CHECK_MEM(back, data, sizeof(back));

    /* The one thing a refusal does change: its sequence number is consumed, because a seq is a
     * stream position whether or not the command succeeded. Replaying it is an error, and the
     * guest retries with a fresh one — which is what the rest of this case does. */
    {
        uint32_t refused_seq = seq - 1u;
        CHECK_EQ(run_write(refused_seq, 1u, 0u, data, sizeof(data), &h), IOSVM_ERR_SEQ);
    }

    /* The batch counter is unchanged, so the guest can still submit it. */
    w[0] = 1u;
    w[1] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_SUBMIT, seq++, w, 2u, &h), IOSVM_OK);
}

static void case_image_linear(void)
{
    static const uint8_t pixels[16] = {1u, 2u,  3u,  4u,  5u,  6u,  7u,  8u,
                                       9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u};
    uint8_t back[16];
    uint32_t w[6];
    uint32_t h = 0u;
    uint32_t seq = 1u;

    new_gpu(0u);
    CHECK(g_gpu != NULL);

    w[0] = 1u;
    w[1] = 2u;
    w[2] = 2u;
    w[3] = (uint32_t)IOSVM_FORMAT_RGBA8;
    w[4] = 0u;
    w[5] = 0u;
    CHECK_EQ(run_cmd(IOSVM_OP_CREATE_IMAGE, seq++, w, 6u, &h), IOSVM_OK);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 1u), 16u);

    /* Images accept linear bytes, so a byte write is visible in a readback. */
    CHECK_EQ(run_write(seq++, 1u, 0u, pixels, sizeof(pixels), &h), IOSVM_OK);
    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 1u, 0u, back, sizeof(back)), IOSVM_OK);
    CHECK_MEM(back, pixels, sizeof(back));
}

void iosvm_test_fake_gpu(void)
{
    iosvm_run_case("fake: create and destroy resources", case_create_destroy);
    iosvm_run_case("fake: write and readback", case_write);
    iosvm_run_case("fake: blit_copy transfers bytes", case_blit_copy);
    iosvm_run_case("fake: blit_fill repeats a 32-bit pattern", case_blit_fill);
    iosvm_run_case("fake: submit closes the batch", case_submit);
    iosvm_run_case("fake: fence_wait needs a real fence", case_fence_wait);
    iosvm_run_case("fake: present and screenshot after submit", case_present_and_screenshot);
    iosvm_run_case("fake: sequence must advance", case_seq);
    iosvm_run_case("fake: out of memory returns FULL", case_out_of_memory);
    iosvm_run_case("fake: reserved fields are enforced", case_reserved_fields);
    iosvm_run_case("fake: refused command mutates nothing", case_refused_command_mutates_nothing);
    iosvm_run_case("fake: images are linear RGBA8", case_image_linear);

    if (g_gpu != NULL) {
        iosvm_fake_gpu_free(g_gpu);
        g_gpu = NULL;
    }
}
