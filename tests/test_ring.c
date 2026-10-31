/* SPDX-License-Identifier: MIT */
/*
 * Ring primitives: formatting, framing, flow control, wrap-around, padding and
 * the ack direction.
 *
 * The counters are cumulative byte offsets (not indices), so several cases
 * assert the raw counter values in the header. That is deliberate: it pins the
 * representation that makes an emptied ring reusable and makes the padding
 * marker observable.
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

/* Write a framed 24-byte SUBMIT record into a fresh reservation. */
static int32_t put_submit(uint8_t *slot, uint32_t seq, uint32_t cmd_count)
{
    struct iosvm_op_submit args;

    args.cmd_count = cmd_count;
    args.flags = 0u;
    return iosvm_encode_submit(slot, 24u, seq, &args);
}

/* Stamp a minimal BLIT_FILL envelope of `total` bytes; the ring never decodes it. */
static void stamp_record(uint8_t *rec, uint32_t total, uint32_t seq)
{
    iosvm_put_le32(rec + IOSVM_CMD_HDR_MAGIC_OFF, IOSVM_PROTO_MAGIC);
    iosvm_put_le16(rec + IOSVM_CMD_HDR_VERSION_OFF, IOSVM_PROTO_VERSION);
    iosvm_put_le16(rec + IOSVM_CMD_HDR_OP_OFF, (uint16_t)IOSVM_OP_BLIT_FILL);
    iosvm_put_le32(rec + IOSVM_CMD_HDR_SEQ_OFF, seq);
    iosvm_put_le32(rec + IOSVM_CMD_HDR_LEN_OFF, total - IOSVM_CMD_HDR_SIZE);
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

    /*
     * A 260-byte command body is not a multiple of IOSVM_RING_ALIGN, while the
     * 272-byte ack body is a multiple of 16: only the command-body alignment
     * check can reject this geometry.
     */
    {
        static _Alignas(8) uint8_t odd_mem[IOSVM_RING_HDR_SIZE + 260u + 272u];

        CHECK_EQ(iosvm_ring_init(&ring, odd_mem, sizeof(odd_mem), 260u), IOSVM_ERR_BAD_ALIGN);
    }

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

    /* Counters are little-endian in the header (and cumulative). */
    CHECK_EQ(mem[IOSVM_RING_HDR_CMD_HEAD_OFF], 32u);
    CHECK_EQ(mem[IOSVM_RING_HDR_CMD_HEAD_OFF + 1u], 0u);
    CHECK_EQ(mem[IOSVM_RING_HDR_CMD_TAIL_OFF], 32u);
}

static void case_overrun(void)
{
    static _Alignas(8) uint8_t mem[RING_MEM];
    struct iosvm_ring ring;
    uint8_t *body = mem + IOSVM_RING_HDR_SIZE;
    uint32_t reserved = 0u;
    uint32_t consumed = 0u;

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), RING_BODY), IOSVM_OK);

    for (;;) {
        uint8_t *slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);

        if (slot == NULL) {
            break;
        }
        CHECK(slot == body + reserved * 24u);
        CHECK_EQ(put_submit(slot, reserved + 1u, 1u), 24);
        CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
        reserved++;
        CHECK(iosvm_ring_used(&ring, IOSVM_RING_CMD) <= RING_BODY);
        CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), reserved * 24u);
    }

    /*
     * A 288-byte body of 24-byte records: twelve records fill it exactly and
     * used == size is a representable "full" state. The old index
     * representation stopped one byte short of that.
     */
    CHECK_EQ(reserved, 12u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), RING_BODY);
    /* A record larger than the body can never be placed. */
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, RING_BODY) == NULL);
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, RING_BODY + 8u) == NULL);
    /* A record that would fit in an empty body still has to wait while full. */
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 16u) == NULL);

    CHECK_EQ(drain_any(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, reserved);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);

    /* Space is reusable once released, even when all the counters are past size. */
    {
        uint8_t *slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);

        CHECK(slot != NULL);
        CHECK(slot == body + 0u);
        CHECK_EQ(put_submit(slot, reserved + 1u, 1u), 24);
        CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
        CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF), 12u * 24u + 24u);
    }
    CHECK_EQ(drain_any(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, reserved + 1u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 13u * 24u);
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
        CHECK(iosvm_ring_used(&ring, IOSVM_RING_CMD) <= 256u);

        if ((i % 3u) == 0u) {
            CHECK_EQ(consume_fills(&ring, &consumed), IOSVM_OK);
        }
    }

    CHECK_EQ(consume_fills(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, 1000u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF), 1000u * 32u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 1000u * 32u);
}

/* 24-byte records do not divide the 256-byte body, so this enters the padding path. */
static void case_wrap_padding(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 256u + 256u];
    struct iosvm_ring ring;
    uint8_t *body = mem + IOSVM_RING_HDR_SIZE;
    const uint8_t *rec = NULL;
    uint8_t *slot;
    uint32_t len = 0u;
    uint32_t consumed = 0u;
    uint32_t i;

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);

    for (i = 0u; i < 10u; i++) {
        slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
        CHECK(slot == body + i * 24u);
        CHECK_EQ(put_submit(slot, i + 1u, 1u), 24);
        CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    }
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 240u);
    CHECK_EQ(drain_any(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, 10u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF), 240u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 240u);

    /* Dirty the tail gap: the ring must zero it so the consumer can skip it. */
    memset(body + 240u, 0xa5, 16u);
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
    CHECK(slot == body);
    for (i = 240u; i < 256u; i++) {
        CHECK_EQ(body[i], 0u);
    }
    CHECK_EQ(put_submit(slot, 11u, 1u), 24);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    /* head advanced by the 16 bytes of padding plus the 24-byte record. */
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF), 280u);

    /* peek() retires the padding itself: tail jumps to the next multiple of 256. */
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK(rec == body);
    CHECK_EQ(len, 24u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 256u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 24u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);

    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF), 280u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 280u);
}

/* 40-byte records do not divide a 256-byte body either; a second geometry for the marker. */
static void case_wrap_padding_40(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 256u + 256u];
    struct iosvm_ring ring;
    struct iosvm_op_create_image args;
    uint8_t *body = mem + IOSVM_RING_HDR_SIZE;
    const uint8_t *rec = NULL;
    uint8_t *slot;
    uint32_t len = 0u;
    uint32_t i;

    memset(&args, 0, sizeof(args));
    args.width = 1u;
    args.height = 1u;
    args.format = IOSVM_FORMAT_RGBA8;

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);

    for (i = 0u; i < 6u; i++) {
        slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 40u);
        CHECK(slot == body + i * 40u);
        args.handle = i + 1u;
        CHECK_EQ(iosvm_encode_create_image(slot, 40u, i + 1u, &args), 40);
        CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 40u), IOSVM_OK);
    }
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 240u);

    /* Consume five, leaving one 40-byte record at the end of the body. */
    for (i = 0u; i < 5u; i++) {
        CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
        CHECK(rec == body + i * 40u);
        CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);
    }
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 40u);

    /* 16 bytes of padding, then the new record at offset 0. */
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 40u);
    CHECK(slot == body);
    for (i = 240u; i < 256u; i++) {
        CHECK_EQ(body[i], 0u);
    }
    args.handle = 7u;
    CHECK_EQ(iosvm_encode_create_image(slot, 40u, 7u, &args), 40);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 40u), IOSVM_OK);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF), 296u);

    /* The old record at the end of the body is delivered first, intact. */
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK(rec == body + 200u);
    CHECK_EQ(len, 40u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 240u);

    /* Then the padding is skipped and the wrapped record is delivered. */
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK(rec == body);
    CHECK_EQ(len, 40u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 256u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 296u);
}

/*
 * Wrapping must never overwrite unconsumed data. The producer may wrap while a
 * record is still unread at the end of the body, but only when the padding plus
 * the new record fit in the free bytes in front of the consumer tail.
 */
static void case_wrap_unconsumed(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 256u + 256u];
    struct iosvm_ring ring;
    uint8_t *body = mem + IOSVM_RING_HDR_SIZE;
    const uint8_t *rec = NULL;
    struct iosvm_cmd cmd;
    uint8_t *slot;
    uint32_t len = 0u;
    uint32_t i;

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);

    for (i = 0u; i < 10u; i++) {
        slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
        CHECK(slot == body + i * 24u);
        CHECK_EQ(put_submit(slot, i + 1u, 1u), 24);
        CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    }
    /* Leave the tenth record (at 216..240) unread. */
    for (i = 0u; i < 9u; i++) {
        CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
        CHECK(rec == body + i * 24u);
        CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);
    }
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 24u);

    /* Wrap: 16 bytes of padding, then the new record at 0. */
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
    CHECK(slot == body);
    CHECK_EQ(put_submit(slot, 11u, 1u), 24);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF), 280u);

    /* The unconsumed record is delivered first, from its old bytes. */
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK(rec == body + 216u);
    CHECK_EQ(len, 24u);
    CHECK_EQ(iosvm_decode_one(rec, (size_t)len, &cmd), 24);
    CHECK_EQ(cmd.seq, 10u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 240u);

    /* Then the padding is skipped and the new record follows. */
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK(rec == body);
    CHECK_EQ(len, 24u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF), 256u);
    CHECK_EQ(iosvm_decode_one(rec, (size_t)len, &cmd), 24);
    CHECK_EQ(cmd.seq, 11u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);

    /*
     * The same body can also be too full to wrap: 240 used bytes leave only 16
     * free, so 16 bytes of padding plus a 24-byte record cannot fit. Refusing
     * keeps the unread record at offset 0 intact.
     */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    for (i = 0u; i < 10u; i++) {
        slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
        CHECK(slot == body + i * 24u);
        CHECK_EQ(put_submit(slot, i + 1u, 1u), 24);
        CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    }
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 240u);
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u) == NULL);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 240u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK(rec == body);
    CHECK_EQ(iosvm_decode_one(rec, (size_t)len, &cmd), 24);
    CHECK_EQ(cmd.seq, 1u);
}

/*
 * Reproducer for the wedged-empty-ring deadlock: after the ring drains with the
 * counters on a non-zero offset, a reserve() of a record that fits in the whole
 * body must still succeed. Both geometries used by the reviewer are covered.
 */
static int wedge_probe(uint32_t body, uint32_t total)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 65576u + 256u];
    struct iosvm_ring ring;
    const uint8_t *rec = NULL;
    uint32_t len = 0u;
    uint32_t i;
    uint8_t *slot;

    if (iosvm_ring_init(&ring, mem, IOSVM_RING_HDR_SIZE + body + 256u, body) != IOSVM_OK) {
        return 0;
    }
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, total);
    if (slot == NULL) {
        return 0;
    }
    stamp_record(slot, total, 1u);
    if (iosvm_ring_commit(&ring, IOSVM_RING_CMD, total) != IOSVM_OK) {
        return 0;
    }
    if (iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len) != IOSVM_OK || rec == NULL ||
        len != total) {
        return 0;
    }
    if (iosvm_ring_release(&ring, IOSVM_RING_CMD, len) != IOSVM_OK) {
        return 0;
    }
    if (iosvm_ring_used(&ring, IOSVM_RING_CMD) != 0u) {
        return 0;
    }
    if (iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF) != total) {
        return 0;
    }
    /* Retry the whole cycle three times: the ring must never wedge on an empty body. */
    for (i = 0u; i < 3u; i++) {
        const uint8_t *again = NULL;
        uint32_t again_len = 0u;

        slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, total);
        if (slot == NULL) {
            printf("WEDGED: body=%u total=%u used=%u\n", body, total,
                   iosvm_ring_used(&ring, IOSVM_RING_CMD));
            return 0;
        }
        if (iosvm_ring_used(&ring, IOSVM_RING_CMD) != 0u) {
            return 0;
        }
        stamp_record(slot, total, 2u + i);
        if (iosvm_ring_commit(&ring, IOSVM_RING_CMD, total) != IOSVM_OK) {
            return 0;
        }
        if (iosvm_ring_peek(&ring, IOSVM_RING_CMD, &again, &again_len) != IOSVM_OK ||
            again == NULL || again_len != total) {
            return 0;
        }
        if (iosvm_ring_release(&ring, IOSVM_RING_CMD, again_len) != IOSVM_OK) {
            return 0;
        }
        if (iosvm_ring_used(&ring, IOSVM_RING_CMD) != 0u) {
            return 0;
        }
    }
    return 1;
}

static void case_wedge(void)
{
    CHECK_EQ(wedge_probe(256u, 136u), 1);
    CHECK_EQ(wedge_probe(65576u, 65568u), 1);
}

/*
 * commit() publishes the length the record header encodes. A reservation that is
 * only partly filled must not leave its zero tail published, because the
 * consumer would read the zeroes at the tail as wrap padding and deliver the
 * same record again forever.
 */
static void case_commit_encoded_length(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 256u + 256u];
    struct iosvm_ring ring;
    const uint8_t *rec = NULL;
    struct iosvm_cmd cmd;
    uint8_t *slot;
    uint32_t len = 0u;

    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);

    /* Reserve 32, write a 24-byte record, commit the reservation: publish 24. */
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 32u);
    CHECK(slot != NULL);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    CHECK_EQ(put_submit(slot, 1u, 1u), 24);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 32u), IOSVM_OK);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 24u);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF), 24u);

    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK(rec != NULL);
    CHECK_EQ(len, 24u);
    CHECK_EQ(iosvm_decode_one(rec, (size_t)len, &cmd), 24);
    CHECK_EQ(cmd.seq, 1u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);

    /* The unused zero bytes are unpublished, so the record is not delivered again. */
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK(rec == NULL);
    CHECK_EQ(len, 0u);

    /* And the ring keeps moving: the next reservation starts at offset 24. */
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 32u);
    CHECK(slot != NULL);
    CHECK_EQ(put_submit(slot, 2u, 1u), 24);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 32u), IOSVM_OK);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF), 48u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK_EQ(len, 24u);
    CHECK_EQ(iosvm_decode_one(rec, (size_t)len, &cmd), 24);
    CHECK_EQ(cmd.seq, 2u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);

    /* A header that claims more than the reservation is refused, nothing published. */
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 32u);
    CHECK(slot != NULL);
    iosvm_put_le32(slot + IOSVM_CMD_HDR_MAGIC_OFF, IOSVM_PROTO_MAGIC);
    iosvm_put_le32(slot + IOSVM_CMD_HDR_LEN_OFF, 24u); /* 16 + 24 > 32 */
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 32u), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    /* The refused commit keeps the reservation outstanding. */
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 32u) == NULL);
    /* Fixing the header makes the same reservation publishable. */
    iosvm_put_le32(slot + IOSVM_CMD_HDR_LEN_OFF, 8u);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 32u), IOSVM_OK);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 24u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK_EQ(len, 24u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);

    /* A zero first word would be read as padding by the consumer: refused. */
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 32u);
    CHECK(slot != NULL);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 32u), IOSVM_ERR_UNKNOWN_OP);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    /* A misaligned payload length would desynchronise the body: refused. */
    iosvm_put_le32(slot + IOSVM_CMD_HDR_MAGIC_OFF, IOSVM_PROTO_MAGIC);
    iosvm_put_le32(slot + IOSVM_CMD_HDR_LEN_OFF, 12u);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 32u), IOSVM_ERR_BAD_ALIGN);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    /* Fix it and drain. */
    iosvm_put_le32(slot + IOSVM_CMD_HDR_LEN_OFF, 16u);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 32u), IOSVM_OK);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_OK);
    CHECK_EQ(len, 32u);
    CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_CMD, len), IOSVM_OK);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);

    /* A command reservation too small to hold a header can never be committed. */
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 8u) == NULL);
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
    /* Ack entries are exactly one slot: anything else could need padding. */
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_ACK, 32u) == NULL);
    CHECK(iosvm_ring_reserve(&ring, IOSVM_RING_ACK, 24u) == NULL);

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
                CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_ACK_HEAD_OFF) % IOSVM_ACK_WIRE_SIZE,
                         0u);
                break;
            }
            CHECK_EQ(consume_acks(&ring, &consumed), IOSVM_OK);
        }
    }

    CHECK_EQ(consume_acks(&ring, &consumed), IOSVM_OK);
    CHECK_EQ(consumed, 200u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_ACK), 0u);
    /* The ack counters are cumulative too: 200 slots wrapped the 256-byte body. */
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_ACK_HEAD_OFF), 200u * IOSVM_ACK_WIRE_SIZE);
    CHECK_EQ(iosvm_get_le32(mem + IOSVM_RING_HDR_ACK_TAIL_OFF), 200u * IOSVM_ACK_WIRE_SIZE);
}

static void case_corrupt(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 256u + 256u];
    struct iosvm_ring ring;
    const uint8_t *rec = NULL;
    uint32_t len = 0u;
    uint8_t *slot;

    /* head outside the body (past what the body could ever hold) */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    iosvm_put_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF, 4096u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_TRUNCATED);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);

    /* A counter past the body-derived period is not a valid cumulative offset. */
    {
        static _Alignas(8) uint8_t odd[RING_MEM];

        CHECK_EQ(iosvm_ring_init(&ring, odd, sizeof(odd), RING_BODY), IOSVM_OK);
        iosvm_put_le32(odd + IOSVM_RING_HDR_CMD_HEAD_OFF, 0xffffffffu);
        CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_TRUNCATED);
        CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
    }

    /*
     * length field is not 8-byte aligned. commit() now validates the header it
     * publishes, so the corruption is applied to the bytes after publication.
     */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
    CHECK(slot != NULL);
    CHECK_EQ(put_submit(slot, 1u, 1u), 24);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    iosvm_put_le32(mem + IOSVM_RING_HDR_SIZE + IOSVM_CMD_HDR_LEN_OFF, 12u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_BAD_ALIGN);

    /* length field claims more than a whole body */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    slot = iosvm_ring_reserve(&ring, IOSVM_RING_CMD, 24u);
    CHECK(slot != NULL);
    CHECK_EQ(put_submit(slot, 1u, 1u), 24);
    CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_CMD, 24u), IOSVM_OK);
    iosvm_put_le32(mem + IOSVM_RING_HDR_SIZE + IOSVM_CMD_HDR_LEN_OFF, 4096u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_TRUNCATED);

    /* published bytes that start with a zero word cannot be a record at offset 0 */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    iosvm_put_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF, 32u);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 32u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_TRUNCATED);
}

/*
 * The two peek() bounds: a record must fit both before the end of the body and
 * in the bytes the producer actually published. Neither may be read.
 */
static void case_peek_bounds(void)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 256u + 256u];
    struct iosvm_ring ring;
    const uint8_t *rec = NULL;
    uint32_t len = 0u;
    uint8_t *body = mem + IOSVM_RING_HDR_SIZE;

    /* Record total (32) longer than the 16 bytes left before the end of the body. */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    iosvm_put_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF, 240u);
    iosvm_put_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF, 272u); /* used == 32 */
    iosvm_put_le32(body + 240u + IOSVM_CMD_HDR_MAGIC_OFF, IOSVM_PROTO_MAGIC);
    iosvm_put_le32(body + 240u + IOSVM_CMD_HDR_LEN_OFF, 16u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_TRUNCATED);
    CHECK(rec == NULL);
    CHECK_EQ(len, 0u);

    /* Record total (40) longer than the 16 published bytes. */
    CHECK_EQ(iosvm_ring_init(&ring, mem, sizeof(mem), 256u), IOSVM_OK);
    iosvm_put_le32(mem + IOSVM_RING_HDR_CMD_TAIL_OFF, 0u);
    iosvm_put_le32(mem + IOSVM_RING_HDR_CMD_HEAD_OFF, 16u); /* used == 16 */
    iosvm_put_le32(body + IOSVM_CMD_HDR_MAGIC_OFF, IOSVM_PROTO_MAGIC);
    iosvm_put_le32(body + IOSVM_CMD_HDR_LEN_OFF, 24u);
    CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_CMD, &rec, &len), IOSVM_ERR_TRUNCATED);
    CHECK(rec == NULL);
    CHECK_EQ(len, 0u);
}

/*
 * Differential fuzz: 1,000,000 producer/consumer steps against an in-test FIFO
 * model, over five body sizes (three of them not powers of two). Records are
 * variable-length WRITEs; one in five reservations is deliberately over-sized so
 * that commit() has to publish the encoded length.
 */
#define FUZZ_QCAP 2048u
#define FUZZ_MAX_DATA 48u

struct fuzz_rec {
    uint32_t seq;
    uint32_t handle;
    uint32_t offset;
    uint32_t data_len;
    uint8_t data[FUZZ_MAX_DATA];
};

struct fuzz_queue {
    struct fuzz_rec item[FUZZ_QCAP];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t produced;
    uint32_t consumed;
    uint32_t pad_skips;
};

static uint32_t fuzz_next(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

/* Returns 1 when a record was produced, 0 when the ring is full, -1 on error. */
static int fuzz_produce(struct iosvm_ring *ring, struct fuzz_queue *q, uint32_t *rng)
{
    uint32_t r = fuzz_next(rng);
    uint32_t data_len = 8u + 8u * (r % 6u); /* 8..48, always 8-aligned */
    uint32_t total = 32u + data_len;
    uint32_t reserved = total + (((r >> 8) % 5u == 0u) ? 8u : 0u);
    uint32_t seq = q->produced + 1u;
    uint32_t handle = 0x100u + (q->produced % 7u);
    uint32_t offset = q->produced * 4u;
    uint8_t data[FUZZ_MAX_DATA];
    struct fuzz_rec *rec;
    uint8_t *slot;
    uint32_t i;

    for (i = 0u; i < data_len; i++) {
        data[i] = (uint8_t)((seq * 31u + i * 17u + 7u) & 0xffu);
    }

    slot = iosvm_ring_reserve(ring, IOSVM_RING_CMD, reserved);
    if (slot == NULL) {
        return 0;
    }
    if (iosvm_encode_write(slot, (size_t)reserved, seq, handle, offset, data, data_len) !=
        (int32_t)total) {
        return -1;
    }
    if (iosvm_ring_commit(ring, IOSVM_RING_CMD, reserved) != IOSVM_OK) {
        return -1;
    }
    if (q->count >= FUZZ_QCAP) {
        return -1;
    }

    rec = &q->item[q->head];
    rec->seq = seq;
    rec->handle = handle;
    rec->offset = offset;
    rec->data_len = data_len;
    memcpy(rec->data, data, (size_t)data_len);
    q->head = (q->head + 1u) % FUZZ_QCAP;
    q->count++;
    q->produced++;
    return 1;
}

/* Returns 1 when a record was consumed, 0 when the ring is empty, -1 on error. */
static int fuzz_consume(struct iosvm_ring *ring, struct fuzz_queue *q)
{
    uint32_t tail_before = iosvm_get_le32(ring->mem + IOSVM_RING_HDR_CMD_TAIL_OFF);
    const uint8_t *rec = NULL;
    uint32_t len = 0u;
    uint32_t handle = 0u;
    uint32_t offset = 0u;
    uint32_t inner = 0u;
    struct iosvm_cmd cmd;
    struct fuzz_rec *want;
    int32_t rc = iosvm_ring_peek(ring, IOSVM_RING_CMD, &rec, &len);

    if (rc != IOSVM_OK) {
        return -1;
    }
    if (rec == NULL) {
        return (len == 0u) ? 0 : -1;
    }
    if (q->count == 0u) {
        return -1;
    }
    want = &q->item[q->tail];
    if (len != 32u + want->data_len) {
        return -1;
    }
    rc = iosvm_decode_one(rec, (size_t)len, &cmd);
    if (rc != (int32_t)len || cmd.op != (uint16_t)IOSVM_OP_WRITE || cmd.seq != want->seq) {
        return -1;
    }
    if (iosvm_cmd_u32(&cmd, 0u, &handle) != IOSVM_OK ||
        iosvm_cmd_u32(&cmd, 1u, &offset) != IOSVM_OK ||
        iosvm_cmd_u32(&cmd, 2u, &inner) != IOSVM_OK) {
        return -1;
    }
    if (handle != want->handle || offset != want->offset || inner != want->data_len) {
        return -1;
    }
    if (memcmp(cmd.payload + IOSVM_OP_WRITE_SIZE, want->data, (size_t)want->data_len) != 0) {
        return -1;
    }
    if (iosvm_get_le32(ring->mem + IOSVM_RING_HDR_CMD_TAIL_OFF) != tail_before) {
        q->pad_skips++; /* peek() retired wrap padding before this record */
    }
    if (iosvm_ring_release(ring, IOSVM_RING_CMD, len) != IOSVM_OK) {
        return -1;
    }
    q->tail = (q->tail + 1u) % FUZZ_QCAP;
    q->count--;
    q->consumed++;
    return 1;
}

static void fuzz_body(uint32_t body, uint32_t steps, struct fuzz_queue *total)
{
    static _Alignas(8) uint8_t mem[IOSVM_RING_HDR_SIZE + 65576u + 256u];
    struct iosvm_ring ring;
    uint32_t rng = 0x12345678u ^ body;
    uint32_t step;

    memset(total, 0, sizeof(*total));
    CHECK_EQ(iosvm_ring_init(&ring, mem, IOSVM_RING_HDR_SIZE + body + 256u, body), IOSVM_OK);

    for (step = 0u; step < steps; step++) {
        uint32_t r = fuzz_next(&rng);
        int32_t rc;

        if ((r % 3u) != 0u) {
            rc = fuzz_produce(&ring, total, &rng);
            CHECK(rc >= 0);
            if (rc == 1) {
                CHECK(iosvm_ring_used(&ring, IOSVM_RING_CMD) <= body);
                continue;
            }
        }
        rc = fuzz_consume(&ring, total);
        CHECK(rc >= 0);
        if (rc == 0) {
            /* Empty ring: an empty ring must always accept a record that fits. */
            CHECK_EQ(fuzz_produce(&ring, total, &rng), 1);
        }
        CHECK(iosvm_ring_used(&ring, IOSVM_RING_CMD) <= body);
    }

    for (;;) {
        int32_t rc = fuzz_consume(&ring, total);

        CHECK(rc >= 0);
        if (rc == 0) {
            break;
        }
    }
    CHECK_EQ(total->count, 0u);
    CHECK_EQ(total->produced, total->consumed);
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_CMD), 0u);
}

static void case_fuzz(void)
{
    static struct fuzz_queue total;
    static struct fuzz_queue part;
    static const uint32_t bodies[] = {256u, 288u, 520u, 1000u, 65576u};
    uint32_t i;

    memset(&total, 0, sizeof(total));
    for (i = 0u; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        fuzz_body(bodies[i], 200000u, &part);
        total.produced += part.produced;
        total.consumed += part.consumed;
        total.pad_skips += part.pad_skips;
    }
    CHECK_EQ(total.produced, total.consumed);
    CHECK_EQ(total.produced >= 400000u, 1u);
    CHECK_EQ(total.pad_skips >= 1u, 1u);
    printf("    1M-step fuzz: %u records produced/consumed, %u padding skips, bodies"
           " 256/288/520/1000/65576\n",
           total.produced, total.pad_skips);
}

void iosvm_test_ring(void)
{
    iosvm_run_case("ring: init validates and formats the header", case_init);
    iosvm_run_case("ring: reserve/commit/peek/release framing", case_frame);
    iosvm_run_case("ring: fills the body and refuses to overrun it", case_overrun);
    iosvm_run_case("ring: 1000 messages wrap around", case_wrap_1000);
    iosvm_run_case("ring: wrap skips zero padding", case_wrap_padding);
    iosvm_run_case("ring: 40-byte records skip padding on wrap", case_wrap_padding_40);
    iosvm_run_case("ring: wrap keeps unconsumed records intact", case_wrap_unconsumed);
    iosvm_run_case("ring: emptied ring accepts a fitting record (wedge)", case_wedge);
    iosvm_run_case("ring: commit publishes the encoded record length", case_commit_encoded_length);
    iosvm_run_case("ring: ack ring runs the other way", case_ack_direction);
    iosvm_run_case("ring: corrupt counters and lengths are rejected", case_corrupt);
    iosvm_run_case("ring: peek refuses records past the body or past used", case_peek_bounds);
    iosvm_run_case("ring: differential fuzz against a FIFO model", case_fuzz);
}
