/* SPDX-License-Identifier: MIT */
/*
 * End-to-end: guest encodes records into the command ring, the host decodes
 * them, runs them through the fake GPU and publishes acks on the ack ring.
 *
 * This is the closest thing v0.1 has to a real pipeline. Everything runs in one
 * thread here; the ring still applies its real flow-control rules (a full
 * command ring blocks the producer until the consumer releases records).
 */
#include "tests.h"

#define BLIT_BYTES (512u * 1024u)
#define CHUNK_BYTES 1024u
#define READ_CHUNK 4096u
#define NBUFS 40u
#define MAX_STATUSES 64u

static uint8_t g_scratch[IOSVM_MAX_RECORD_SIZE];
static struct iosvm_fake_gpu *g_gpu;

struct pipe {
    struct iosvm_ring ring;
    uint32_t seq;
    uint32_t acks;
    uint32_t error_acks;
    uint32_t processed;
    uint32_t spins;
    uint32_t status_count;
    int32_t statuses[MAX_STATUSES];
};

static struct pipe g_pipe;

static uint8_t pattern_byte(uint32_t index)
{
    return (uint8_t)(((index * 31u) + 7u) & 0xffu);
}

static void new_gpu(uint32_t bytes)
{
    if (g_gpu != NULL) {
        iosvm_fake_gpu_free(g_gpu);
    }
    g_gpu = iosvm_fake_gpu_new(bytes);
}

static uint32_t next_seq(void)
{
    g_pipe.seq++;
    return g_pipe.seq;
}

static void pipe_reset(uint8_t *mem, size_t mem_size, uint32_t cmd_size)
{
    memset(&g_pipe, 0, sizeof(g_pipe));
    CHECK_EQ(iosvm_ring_init(&g_pipe.ring, mem, mem_size, cmd_size), IOSVM_OK);
}

static void pipe_drain_acks(void)
{
    for (;;) {
        const uint8_t *rec = NULL;
        uint32_t len = 0u;
        struct iosvm_ack ack;
        int32_t rc = iosvm_ring_peek(&g_pipe.ring, IOSVM_RING_ACK, &rec, &len);

        CHECK_EQ(rc, IOSVM_OK);
        if (rec == NULL) {
            return;
        }
        CHECK_EQ(len, IOSVM_ACK_WIRE_SIZE);
        CHECK_EQ(iosvm_decode_ack(rec, (size_t)len, &ack), (int32_t)len);
        /* One ack per command, in command order. */
        CHECK_EQ(ack.seq, g_pipe.acks + 1u);
        CHECK_EQ(ack.reserved, 0u);
        g_pipe.acks++;
        if (ack.status != IOSVM_OK) {
            g_pipe.error_acks++;
        }
        if (g_pipe.status_count < MAX_STATUSES) {
            g_pipe.statuses[g_pipe.status_count] = ack.status;
            g_pipe.status_count++;
        }
        CHECK_EQ(iosvm_ring_release(&g_pipe.ring, IOSVM_RING_ACK, len), IOSVM_OK);
    }
}

static void pipe_host_step(void)
{
    const uint8_t *rec = NULL;
    uint32_t len = 0u;
    struct iosvm_cmd cmd;
    struct iosvm_ack ack;
    uint32_t handle = 0u;
    uint8_t *slot;
    int32_t rc = iosvm_ring_peek(&g_pipe.ring, IOSVM_RING_CMD, &rec, &len);

    CHECK_EQ(rc, IOSVM_OK);
    if (rec == NULL) {
        return;
    }
    CHECK_EQ(iosvm_decode_one(rec, (size_t)len, &cmd), (int32_t)len);

    ack.seq = cmd.seq;
    ack.status = iosvm_fake_gpu_exec(g_gpu, &cmd, &handle);
    ack.handle = handle;
    ack.reserved = 0u;

    slot = iosvm_ring_reserve(&g_pipe.ring, IOSVM_RING_ACK, IOSVM_ACK_WIRE_SIZE);
    if (slot == NULL) {
        pipe_drain_acks();
        slot = iosvm_ring_reserve(&g_pipe.ring, IOSVM_RING_ACK, IOSVM_ACK_WIRE_SIZE);
        CHECK(slot != NULL);
    }
    CHECK_EQ(iosvm_encode_ack(slot, IOSVM_ACK_WIRE_SIZE, &ack), (int32_t)IOSVM_ACK_WIRE_SIZE);
    CHECK_EQ(iosvm_ring_commit(&g_pipe.ring, IOSVM_RING_ACK, IOSVM_ACK_WIRE_SIZE), IOSVM_OK);
    CHECK_EQ(iosvm_ring_release(&g_pipe.ring, IOSVM_RING_CMD, len), IOSVM_OK);
    g_pipe.processed++;
}

static void pipe_pump(void)
{
    for (;;) {
        const uint8_t *rec = NULL;
        uint32_t len = 0u;

        CHECK_EQ(iosvm_ring_peek(&g_pipe.ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
        if (rec == NULL) {
            return;
        }
        pipe_host_step();
        g_pipe.spins++;
        CHECK(g_pipe.spins < 1000000u);
    }
}

/* Guest side: copy an encoded record into the command ring, pumping when full. */
static void pipe_send(const uint8_t *record, uint32_t len)
{
    for (;;) {
        uint8_t *slot = iosvm_ring_reserve(&g_pipe.ring, IOSVM_RING_CMD, len);

        if (slot != NULL) {
            memcpy(slot, record, (size_t)len);
            CHECK_EQ(iosvm_ring_commit(&g_pipe.ring, IOSVM_RING_CMD, len), IOSVM_OK);
            return;
        }
        pipe_host_step();
        g_pipe.spins++;
        CHECK(g_pipe.spins < 1000000u);
    }
}

static void send_encoded(int32_t written)
{
    if (written < 0) {
        iosvm_test_fail(__FILE__, __LINE__, "encoder failed");
        return;
    }
    pipe_send(g_scratch, (uint32_t)written);
}

static void create_buffer(uint32_t handle, uint32_t size)
{
    struct iosvm_op_create_buffer args;

    args.handle = handle;
    args.size = size;
    args.usage = 0u;
    args.flags = 0u;
    send_encoded(iosvm_encode_create_buffer(g_scratch, sizeof(g_scratch), next_seq(), &args));
}

static void case_blit_512k(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 8192u + 2048u];
    uint8_t expected[READ_CHUNK];
    uint32_t offset;
    uint32_t i;

    new_gpu(0u);
    CHECK(g_gpu != NULL);
    pipe_reset(mem, sizeof(mem), 8192u);

    create_buffer(1u, BLIT_BYTES);
    create_buffer(2u, BLIT_BYTES);

    for (offset = 0u; offset < BLIT_BYTES; offset += CHUNK_BYTES) {
        uint8_t data[CHUNK_BYTES];

        for (i = 0u; i < CHUNK_BYTES; i++) {
            data[i] = pattern_byte(offset + i);
        }
        send_encoded(iosvm_encode_write(g_scratch, sizeof(g_scratch), next_seq(), 1u, offset, data,
                                        CHUNK_BYTES));
    }

    {
        struct iosvm_op_blit_copy copy;

        copy.dst = 2u;
        copy.dst_offset = 0u;
        copy.src = 1u;
        copy.src_offset = 0u;
        copy.len = BLIT_BYTES;
        copy.reserved = 0u;
        send_encoded(iosvm_encode_blit_copy(g_scratch, sizeof(g_scratch), next_seq(), &copy));
    }
    {
        struct iosvm_op_submit sub;

        sub.cmd_count = (BLIT_BYTES / CHUNK_BYTES) + 1u;
        sub.flags = 0u;
        send_encoded(iosvm_encode_submit(g_scratch, sizeof(g_scratch), next_seq(), &sub));
    }
    {
        struct iosvm_op_fence_wait fence;

        fence.fence_id = 1u;
        fence.timeout_ms = 1000u;
        send_encoded(iosvm_encode_fence_wait(g_scratch, sizeof(g_scratch), next_seq(), &fence));
    }

    pipe_pump();
    pipe_drain_acks();
    CHECK_EQ(g_pipe.acks, 2u + (BLIT_BYTES / CHUNK_BYTES) + 1u + 1u + 1u);
    CHECK_EQ(g_pipe.processed, g_pipe.acks);
    CHECK_EQ(g_pipe.error_acks, 0u);
    CHECK_EQ(iosvm_ring_used(&g_pipe.ring, IOSVM_RING_CMD), 0u);
    CHECK_EQ(iosvm_ring_used(&g_pipe.ring, IOSVM_RING_ACK), 0u);

    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 1u), BLIT_BYTES);
    CHECK_EQ(iosvm_fake_gpu_size(g_gpu, 2u), BLIT_BYTES);
    CHECK_EQ(iosvm_fake_gpu_memory_used(g_gpu), 2u * BLIT_BYTES);

    for (offset = 0u; offset < BLIT_BYTES; offset += READ_CHUNK) {
        uint8_t src[READ_CHUNK];
        uint8_t dst[READ_CHUNK];

        for (i = 0u; i < READ_CHUNK; i++) {
            expected[i] = pattern_byte(offset + i);
        }
        CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 1u, offset, src, READ_CHUNK), IOSVM_OK);
        CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 2u, offset, dst, READ_CHUNK), IOSVM_OK);
        CHECK_MEM(src, expected, READ_CHUNK);
        CHECK_MEM(dst, src, READ_CHUNK);
    }

    {
        struct iosvm_fake_stats st;

        iosvm_fake_gpu_stats(g_gpu, &st);
        CHECK_EQ(st.submits, 1u);
        CHECK_EQ(st.pending, 0u);
        CHECK_EQ(st.resources, 2u);
        CHECK_EQ(st.fence_id, 1u);
    }
}

static void case_40_buffers(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 4096u + 1024u];
    uint32_t i;

    new_gpu(0u);
    CHECK(g_gpu != NULL);
    pipe_reset(mem, sizeof(mem), 4096u);

    for (i = 0u; i < NBUFS; i++) {
        create_buffer(100u + i, 64u);
    }
    for (i = 0u; i < NBUFS; i++) {
        uint32_t word = 0xa5a50000u | i;

        send_encoded(
            iosvm_encode_write(g_scratch, sizeof(g_scratch), next_seq(), 100u + i, 0u, &word, 4u));
    }
    {
        struct iosvm_op_submit sub;

        sub.cmd_count = NBUFS;
        sub.flags = 0u;
        send_encoded(iosvm_encode_submit(g_scratch, sizeof(g_scratch), next_seq(), &sub));
    }

    pipe_pump();
    pipe_drain_acks();
    CHECK_EQ(g_pipe.acks, NBUFS + NBUFS + 1u);
    CHECK_EQ(g_pipe.processed, g_pipe.acks);
    CHECK_EQ(g_pipe.error_acks, 0u);

    /* Every buffer holds its own value: the commands ran in order. */
    for (i = 0u; i < NBUFS; i++) {
        uint8_t back[4];
        uint8_t expect[4];
        uint32_t word = 0xa5a50000u | i;

        iosvm_put_le32(expect, word);
        CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 100u + i, 0u, back, sizeof(back)), IOSVM_OK);
        CHECK_MEM(back, expect, sizeof(back));
    }

    {
        struct iosvm_fake_stats st;

        iosvm_fake_gpu_stats(g_gpu, &st);
        CHECK_EQ(st.resources, NBUFS);
        CHECK_EQ(st.pending, 0u);
        CHECK_EQ(st.submits, 1u);
    }
}

static void case_error_acks(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 1024u + 512u];
    static const uint8_t four[4] = {0xde, 0xad, 0xbe, 0xef};
    uint8_t back[4];

    new_gpu(0u);
    CHECK(g_gpu != NULL);
    pipe_reset(mem, sizeof(mem), 1024u);

    create_buffer(1u, 16u);
    /* Writes past the end of the buffer: refused, and acked as an error. */
    send_encoded(
        iosvm_encode_write(g_scratch, sizeof(g_scratch), next_seq(), 1u, 14u, four, sizeof(four)));
    /* A valid write that must still be applied. */
    send_encoded(
        iosvm_encode_write(g_scratch, sizeof(g_scratch), next_seq(), 1u, 0u, four, sizeof(four)));
    {
        struct iosvm_op_destroy bad;

        bad.handle = 77u;
        bad.reserved = 0u;
        send_encoded(iosvm_encode_destroy(g_scratch, sizeof(g_scratch), next_seq(), &bad));
    }
    {
        struct iosvm_op_submit sub;

        sub.cmd_count = 1u;
        sub.flags = 0u;
        send_encoded(iosvm_encode_submit(g_scratch, sizeof(g_scratch), next_seq(), &sub));
    }
    {
        struct iosvm_op_fence_wait fence;

        fence.fence_id = 1u;
        fence.timeout_ms = 0u;
        send_encoded(iosvm_encode_fence_wait(g_scratch, sizeof(g_scratch), next_seq(), &fence));
    }

    pipe_pump();
    pipe_drain_acks();
    CHECK_EQ(g_pipe.acks, 6u);
    CHECK_EQ(g_pipe.processed, g_pipe.acks);
    CHECK_EQ(g_pipe.error_acks, 2u);
    CHECK_EQ(g_pipe.status_count, 6u);
    CHECK_EQ(g_pipe.statuses[0], IOSVM_OK);
    CHECK_EQ(g_pipe.statuses[1], IOSVM_ERR_BOUNDS);
    CHECK_EQ(g_pipe.statuses[2], IOSVM_OK);
    CHECK_EQ(g_pipe.statuses[3], IOSVM_ERR_BAD_HANDLE);
    CHECK_EQ(g_pipe.statuses[4], IOSVM_OK);
    CHECK_EQ(g_pipe.statuses[5], IOSVM_OK);

    CHECK_EQ(iosvm_fake_gpu_readback(g_gpu, 1u, 0u, back, sizeof(back)), IOSVM_OK);
    CHECK_MEM(back, four, sizeof(back));
}

void iosvm_test_integration(void)
{
    iosvm_run_case("integration: 512 KiB blit through ring and fake GPU", case_blit_512k);
    iosvm_run_case("integration: 40 buffers in order", case_40_buffers);
    iosvm_run_case("integration: errors are acked and do not stall", case_error_acks);

    if (g_gpu != NULL) {
        iosvm_fake_gpu_free(g_gpu);
        g_gpu = NULL;
    }
}
