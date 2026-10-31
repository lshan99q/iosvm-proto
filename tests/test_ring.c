/* SPDX-License-Identifier: MIT */
/*
 * Ring primitives: formatting, framing, flow control, wrap-around, padding and
 * the ack direction.
 */
#include "tests.h"

#define RING_BODY 288u /* 12 records of 24 bytes */
#define RING_MEM (IOSVM_RING_HDR_SIZE + RING_BODY + RING_BODY)

static int32_t drain_any(struct iosvm_ring *ring, uint32_t *consumed)
{
    for (;;) {
        const uint8_t *rec = NULL;
        uint32_t len = 0u;
        struct iosvm_cmd cmd;
        int32_t rc = iosvm_ring_peek(ring, IOSVM_RING_CMD, &rec, &len);

        if (rc != IOSVM_OK) {
            return rc;
        }
        if (rec == NULL) {
            return IOSVM_OK;
        }
        rc = iosvm_decode_one(rec, (size_t)len, &cmd);
        if (rc < 0) {
            return rc;
        }
        if ((uint32_t)rc != len) {
            return IOSVM_ERR_TRUNCATED;
        }
        (*consumed)++;
        rc = iosvm_ring_release(ring, IOSVM_RING_CMD, len);
        if (rc != IOSVM_OK) {
            return rc;
        }
    }
}

/* Consumes blit_fill records and checks dst == index, value == index ^ 0x5a5a5a5a. */
static int32_t consume_fills(struct iosvm_ring *ring, uint32_t *consumed)
{
    for (;;) {
        const uint8_t *rec = NULL;
        uint32_t len = 0u;
        uint32_t dst = 0u;
        uint32_t value = 0u;
        struct iosvm_cmd cmd;
        int32_t rc = iosvm_ring_peek(ring, IOSVM_RING_CMD, &rec, &len);

        if (rc != IOSVM_OK) {
            return rc;
        }
        if (rec == NULL) {
            return IOSVM_OK;
        }
        rc = iosvm_decode_one(rec, (size_t)len, &cmd);
        if (rc < 0) {
            return rc;
        }
        if (cmd.op != (uint16_t)IOSVM_OP_BLIT_FILL) {
            return IOSVM_ERR_UNKNOWN_OP;
        }
        rc = iosvm_cmd_u32(&cmd, 0u, &dst);
        if (rc != IOSVM_OK) {
            return rc;
        }
        rc = iosvm_cmd_u32(&cmd, 3u, &value);
        if (rc != IOSVM_OK) {
            return rc;
        }
        if (cmd.seq != *consumed + 1u || dst != *consumed || value != (*consumed ^ 0x5a5a5a5au)) {
            return IOSVM_ERR_BOUNDS;
        }
        (*consumed)++;
        rc = iosvm_ring_release(ring, IOSVM_RING_CMD, len);
        if (rc != IOSVM_OK) {
            return rc;
        }
    }
}

/* Consumes acks whose handle is seq*3 and whose status is BOUNDS every 7th. */
static int32_t consume_acks(struct iosvm_ring *ring, uint32_t *consumed)
{
    for (;;) {
        const uint8_t *rec = NULL;
        uint32_t len = 0u;
        struct iosvm_ack ack;
        int32_t rc = iosvm_ring_peek(ring, IOSVM_RING_ACK, &rec, &len);

        if (rc != IOSVM_OK) {
            return rc;
        }
        if (rec == NULL) {
            return IOSVM_OK;
        }
        if (len != IOSVM_ACK_WIRE_SIZE) {
            return IOSVM_ERR_BAD_ALIGN;
        }
        rc = iosvm_decode_ack(rec, (size_t)len, &ack);
        if (rc != (int32_t)IOSVM_ACK_WIRE_SIZE) {
            return rc;
        }
        (*consumed)++;
        if (ack.seq != *consumed) {
            return IOSVM_ERR_SEQ;
        }
        if (ack.handle != *consumed * 3u) {
            return IOSVM_ERR_BAD_HANDLE;
        }
        if (ack.status != ((*consumed % 7u == 0u) ? IOSVM_ERR_BOUNDS : IOSVM_OK)) {
            return IOSVM_ERR_BOUNDS;
        }
        rc = iosvm_ring_release(ring, IOSVM_RING_ACK, len);
        if (rc != IOSVM_OK) {
            return rc;
        }
    }
}

static void case_init(void)
{
    static _Alignas(8) uint8_t mem[RING_MEM];
    struct iosvm_ring ring;

    CHECK_EQ(iosvm_ring_init(NULL, mem, sizeof(mem), RING_BODY), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_ring_init(&ring, NULL, sizeof(mem), RING_BODY), IOSVM_ERR_BOUNDS);

    CHECK_EQ(iosvm_ring_init(&ring, mem + 1, sizeof(mem) - 1u, RING_BODY - 8u),
             IOSVM_ERR_BAD_ALIGN);
    CHECK_EQ(iosvm_ring_init(&ring, mem, 64u, 32u), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 100u), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), RING_BODY + 8u), IOSVM_ERR_BAD_ALIGN);
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem) - 8u, RING_BODY), IOSVM_ERR_BAD_ALIGN);
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), IOSVM_RING_MAX_SIZE + 8u), IOSVM_ERR_BOUNDS);

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), RING_BODY), IOSVM_OK);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_MAGIC_OFF), IOSVM_PROTO_MAGIC);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_VERSION_OFF), IOSVM_PROTO_VERSION);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_SIZE_OFF), RING_BODY);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_ACK_SIZE_OFF), RING_BODY);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF), 0u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 0u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_ACK), 0u);
}

static void case_frame(void)
{
    static _Alignas(8) uint8_t mem[RING_MEM];
    struct iosvm_ring ring;
    struct iosvm_op_blit_fill args;
    const uint8_t *rec = NULL;
    uint32_t len = 0u;
    uint8_t *slot;

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), RING_BODY), IOSVM_OK);

    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK(rec == NULL);
    CHECK_EQ(len, 0u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, 32u), IOSVM_ERR_BOUNDS);
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 0u) == NULL);

    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 32u);
    CHECK(slot != NULL);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    /* A second reservation before committing is refused. */
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 32u) == NULL);
    /* Nothing is published until the committed length matches. */
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    /* A refused commit leaves the reservation outstanding. */
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 32u) == NULL);

    args.dst = 0x11u;
    args.dst_offset = 0x22u;
    args.len = 0x33u;
    args.value = 0x44u;
    CHECK_EQ(iosvm_encode_blit_fill(slot, 32u, 9u, &args), 32);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 32u), IOSVM_OK);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 32u);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 32u), IOSVM_ERR_BOUNDS);

    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK(rec != NULL);
    CHECK_EQ(len, 32u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, 16u), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, 32u), IOSVM_ERR_BOUNDS);

    /* Counters are little-endian in the header. */
    CHECK_EQ(mem[IOSVM_RING_HDR_CMD_HEAD_OFF], 32u);
    CHECK_EQ(mem[IOSVM_RING_HDR_CMD_HEAD_OFF + 1u], 0u);
    CHECK_EQ(mem[IOSVM_RING_HDR_CMD_TAIL_OFF], 32u);
}

static void case_overrun(void)
{
    static _Alignas(8) uint8_t mem[RING_MEM];
    struct iosvm_ring ring;
    struct iosvm_op_submit args;
    uint32_t reserved = 0u;
    uint32_t consumed = 0u;

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), RING_BODY), IOSVM_OK);

    args.cmd_count = 1u;
    args.flags = 0u;
    for (;;) {
        uint8_t *slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);

        if (slot == NULL) {
            break;
        }
        CHECK_EQ(iosvm_encode_submit(slot, 24u, reserved + 1u, &args), 24);
        CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
        reserved++;
        CHECK(iosvm_ring_used(&ring, IOSVM_RING_CMD) <= RING_BODY - 1u);
        CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), reserved * 24u);
    }

    /* 288 byte body, 24 byte records, usable capacity 287: 11 records fit. */
    CHECK_EQ(reserved, 11u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 264u);
    /* A record that cannot fit in a body of this size is never accepted. */
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, RING_BODY) == NULL);
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, RING_BODY + 8u) == NULL);

    CHECK_EQ(drain_any(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, reserved);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);

    /* Space is reusable once released. */
    {
        uint8_t *slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);

        CHECK(slot != NULL);
        CHECK_EQ(iosvm_encode_submit(slot, 24u, reserved + 1u, &args), 24);
        CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    }
    CHECK_EQ(drain_any(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, reserved + 1u);
}

static void case_wrap_1000(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 256u + 256u];
    struct iosvm_ring ring;
    struct iosvm_op_blit_fill args;
    uint32_t consumed = 0u;
    uint32_t i;

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);

    for (i = 0u; i < 1000u; i++) {
        uint8_t *slot;

        for (;;) {
            slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 32u);
            if (slot != NULL) {
                break;
            }
            CHECK_EQ(consume_fills(&ring, &consumed), IOSVM_OK);
        }

        args.dst = i;
        args.dst_offset = 0u;
        args.len = 4u;
        args.value = i ^ 0x5a5a5a5au;
        CHECK_EQ(iosvm_encode_blit_fill(slot, 32u, i + 1u, &args), 32);
        CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 32u), IOSVM_OK);
        CHECK(iosvm_ring_used(&ring, IOSVM_RING_CMD) <= 255u);

        if ((i % 3u) == 0u) {
            CHECK_EQ(consume_fills(&ring, &consumed), IOSVM_OK);
        }
    }

    CHECK_EQ(consume_fills(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, 1000u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
}

static void case_wrap_padding(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 256u + 256u];
    struct iosvm_ring ring;
    struct iosvm_op_submit args;
    uint8_t *body = mem + IOSVM_RING_HDR_SIZE;
    uint8_t *slot;
    uint32_t consumed = 0u;
    uint32_t i;

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);

    args.cmd_count = 1u;
    args.flags = 0u;
    for (i = 0u; i < 10u; i++) {
        slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
        CHECK(slot == body + i * 24u);
        CHECK_EQ(iosvm_encode_submit(slot, 24u, i + 1u, &args), 24);
        CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    }
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 240u);
    CHECK_EQ(drain_any(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, 10u);

    /* Dirty the tail gap: the ring must zero it so the consumer can skip it. */
    memset(body + 240u, 0xa5, 16u);
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
    CHECK(slot == body);
    for (i = 240u; i < 256u; i++) {
        CHECK_EQ(body[i], 0u);
    }
    CHECK_EQ(iosvm_encode_submit(slot, 24u, 11u, &args), 24);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);

    /* The consumer sees padding at 240, skips to 0 and finds the record. */
    CHECK_EQ(drain_any(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, 11u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
}

static void case_ack_direction(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 256u + 256u];
    struct iosvm_ring ring;
    uint32_t consumed = 0u;
    uint32_t i;

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_ACK, 8u) == NULL);
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_ACK, 0u) == NULL);

    for (i = 1u; i <= 200u; i++) {
        for (;;) {
            uint8_t *slot = iosvm_ring_reserve(&ring, IOSVM_RING_ACK, IOSVM_ACK_WIRE_SIZE);

            if (slot != NULL) {
                struct iosvm_ack ack;

                ack.seq = i;
                ack.status = (i % 7u == 0u) ? IOSVM_ERR_BOUNDS : IOSVM_OK;
                ack.handle = i * 3u;
                ack.reserved = 0u;
                CHECK_EQ(iosvm_encode_ack(slot, IOSVM_ACK_WIRE_SIZE, &ack),
                         (int32_t)IOSVM_ACK_WIRE_SIZE);
                CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_ACK, IOSVM_ACK_WIRE_SIZE), IOSVM_OK);
                break;
            }
            CHECK_EQ(consume_acks(&ring, &consumed), IOSVM_OK);
        }
    }

    CHECK_EQ(consume_acks(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, 200u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_ACK), 0u);
}

static void case_corrupt(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 256u + 256u];
    struct iosvm_ring ring;
    const uint8_t *rec = NULL;
    uint32_t len = 0u;
    uint8_t *slot;

    /* head outside the body */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    iosvm_put_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF, 4096u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_TRUNCATED);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);

    /* length field is not 8-byte aligned */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
    CHECK(slot != NULL);
    iosvm_put_le32(slot + IOSVM_CMD_HDR_MAGIC_OFF, IOSVM_PROTO_MAGIC);
    iosvm_put_le32(slot + IOSVM_CMD_HDR_LEN_OFF, 12u);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_BAD_ALIGN);

    /* length field claims more than a whole body */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
    CHECK(slot != NULL);
    iosvm_put_le32(slot + IOSVM_CMD_HDR_MAGIC_OFF, IOSVM_PROTO_MAGIC);
    iosvm_put_le32(slot + IOSVM_CMD_HDR_LEN_OFF, 4096u);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_TRUNCATED);

    /* published bytes that start with a zero word cannot be a record */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    iosvm_put_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF, 32u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 32u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_TRUNCATED);
}

void iosvm_test_ring(void)
{
    iosvm_run_case("ring: init validates and formats the header", case_init);
    iosvm_run_case("ring: reserve/commit/peek/release framing", case_frame);
    iosvm_run_case("ring: refuses to overrun capacity-1", case_overrun);
    iosvm_run_case("ring: 1000 messages wrap around", case_wrap_1000);
    iosvm_run_case("ring: wrap skips zero padding", case_wrap_padding);
    iosvm_run_case("ring: ack ring runs the other way", case_ack_direction);
    iosvm_run_case("ring: corrupt counters and lengths are rejected", case_corrupt);
}
