/* SPDX-License-Identifier: MIT */
/*
 * Codec: one round trip per opcode, plus every rejection path the decoder has.
 * The buffer is deliberately larger than any record so that tests can also show
 * that nothing is written past the declared capacity.
 */
#include "tests.h"

static uint8_t g_buf[IOSVM_MAX_RECORD_SIZE];

static void roundtrip(int32_t written, struct iosvm_cmd *cmd, uint32_t expect_total)
{
    CHECK_EQ(written, (int32_t)expect_total);
    CHECK_EQ(iosvm_decode_one(g_buf, (size_t)written, cmd), written);
}

static void check_word(const struct iosvm_cmd *cmd, uint32_t index, uint32_t want)
{
    uint32_t got = 0u;

    CHECK_EQ(iosvm_cmd_u32(cmd, index, &got), IOSVM_OK);
    CHECK_EQ(got, want);
}

static void check_payload(const struct iosvm_cmd *cmd, const uint32_t *want, uint32_t count)
{
    uint32_t i;

    CHECK_EQ(cmd->len, count * 4u);
    for (i = 0u; i < count; i++) {
        check_word(cmd, i, want[i]);
    }
}

static void case_create_buffer(void)
{
    struct iosvm_op_create_buffer args;
    const uint32_t want[4] = {0x11u, 0x2000u, 0u, 0u};
    struct iosvm_cmd cmd;

    args.handle = want[0];
    args.size = want[1];
    args.usage = want[2];
    args.flags = want[3];
    roundtrip(iosvm_encode_create_buffer(g_buf, sizeof(g_buf), 5u, &args), &cmd, 32u);
    CHECK_EQ(cmd.magic, IOSVM_PROTO_MAGIC);
    CHECK_EQ(cmd.version, IOSVM_PROTO_VERSION);
    CHECK_EQ(cmd.op, IOSVM_OP_CREATE_BUFFER);
    CHECK_EQ(cmd.seq, 5u);
    check_payload(&cmd, want, 4u);
}

static void case_create_image(void)
{
    struct iosvm_op_create_image args;
    const uint32_t want[6] = {0x21u, 640u, 480u, IOSVM_FORMAT_RGBA8, 0u, 0u};
    struct iosvm_cmd cmd;

    args.handle = want[0];
    args.width = want[1];
    args.height = want[2];
    args.format = want[3];
    args.usage = want[4];
    args.flags = want[5];
    roundtrip(iosvm_encode_create_image(g_buf, sizeof(g_buf), 6u, &args), &cmd, 40u);
    CHECK_EQ(cmd.op, IOSVM_OP_CREATE_IMAGE);
    check_payload(&cmd, want, 6u);
}

static void case_destroy(void)
{
    struct iosvm_op_destroy args;
    const uint32_t want[2] = {0x31u, 0u};
    struct iosvm_cmd cmd;

    args.handle = want[0];
    args.reserved = want[1];
    roundtrip(iosvm_encode_destroy(g_buf, sizeof(g_buf), 7u, &args), &cmd, 24u);
    CHECK_EQ(cmd.op, IOSVM_OP_DESTROY);
    check_payload(&cmd, want, 2u);
}

static void case_write(void)
{
    static const uint8_t data[9] = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04, 0x05};
    struct iosvm_cmd cmd;
    int32_t written = iosvm_encode_write(g_buf, sizeof(g_buf), 8u, 0x41u, 0x100u, data, 5u);

    /* 16 header + 16 fixed + 8 padded data */
    roundtrip(written, &cmd, 40u);
    CHECK_EQ(cmd.op, IOSVM_OP_WRITE);
    CHECK_EQ(cmd.len, 24u);
    check_word(&cmd, 0u, 0x41u);
    check_word(&cmd, 1u, 0x100u);
    check_word(&cmd, 2u, 5u);
    check_word(&cmd, 3u, 0u);
    CHECK_MEM(cmd.payload + IOSVM_OP_WRITE_SIZE, data, 5u);
    CHECK_EQ(cmd.payload[21], 0u);
    CHECK_EQ(cmd.payload[22], 0u);
    CHECK_EQ(cmd.payload[23], 0u);

    /* 8 data bytes need no padding; 9 need one more 8-byte block. */
    written = iosvm_encode_write(g_buf, sizeof(g_buf), 9u, 0x42u, 0u, data, 8u);
    roundtrip(written, &cmd, 40u);
    CHECK_EQ(cmd.len, 24u);
    CHECK_MEM(cmd.payload + IOSVM_OP_WRITE_SIZE, data, 8u);

    written = iosvm_encode_write(g_buf, sizeof(g_buf), 10u, 0x42u, 0u, data, 9u);
    roundtrip(written, &cmd, 48u);
    CHECK_EQ(cmd.len, 32u);
    CHECK_MEM(cmd.payload + IOSVM_OP_WRITE_SIZE, data, 9u);
    CHECK_EQ(cmd.payload[16u + 9u], 0u);
    CHECK_EQ(cmd.payload[31u], 0u);

    /* Zero-length and oversized inline writes are refused. */
    CHECK_EQ(iosvm_encode_write(g_buf, sizeof(g_buf), 10u, 1u, 0u, data, 0u), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_encode_write(g_buf, sizeof(g_buf), 10u, 1u, 0u, NULL, 4u), IOSVM_ERR_BOUNDS);
    CHECK_EQ(
        iosvm_encode_write(g_buf, sizeof(g_buf), 10u, 1u, 0u, data, IOSVM_MAX_INLINE_WRITE + 1u),
        IOSVM_ERR_BOUNDS);
}

static void case_blit_copy(void)
{
    struct iosvm_op_blit_copy args;
    const uint32_t want[6] = {0x51u, 0x10u, 0x52u, 0x20u, 0x3000u, 0u};
    struct iosvm_cmd cmd;

    args.dst = want[0];
    args.dst_offset = want[1];
    args.src = want[2];
    args.src_offset = want[3];
    args.len = want[4];
    args.reserved = want[5];
    roundtrip(iosvm_encode_blit_copy(g_buf, sizeof(g_buf), 11u, &args), &cmd, 40u);
    CHECK_EQ(cmd.op, IOSVM_OP_BLIT_COPY);
    check_payload(&cmd, want, 6u);
}

static void case_blit_fill(void)
{
    struct iosvm_op_blit_fill args;
    const uint32_t want[4] = {0x61u, 0x40u, 0x1000u, 0xcafef00du};
    struct iosvm_cmd cmd;

    args.dst = want[0];
    args.dst_offset = want[1];
    args.len = want[2];
    args.value = want[3];
    roundtrip(iosvm_encode_blit_fill(g_buf, sizeof(g_buf), 12u, &args), &cmd, 32u);
    CHECK_EQ(cmd.op, IOSVM_OP_BLIT_FILL);
    check_payload(&cmd, want, 4u);
}

static void case_submit(void)
{
    struct iosvm_op_submit args;
    const uint32_t want[2] = {7u, 0u};
    struct iosvm_cmd cmd;

    args.cmd_count = want[0];
    args.flags = want[1];
    roundtrip(iosvm_encode_submit(g_buf, sizeof(g_buf), 13u, &args), &cmd, 24u);
    CHECK_EQ(cmd.op, IOSVM_OP_SUBMIT);
    check_payload(&cmd, want, 2u);
}

static void case_fence_wait(void)
{
    struct iosvm_op_fence_wait args;
    const uint32_t want[2] = {3u, 1000u};
    struct iosvm_cmd cmd;

    args.fence_id = want[0];
    args.timeout_ms = want[1];
    roundtrip(iosvm_encode_fence_wait(g_buf, sizeof(g_buf), 14u, &args), &cmd, 24u);
    CHECK_EQ(cmd.op, IOSVM_OP_FENCE_WAIT);
    check_payload(&cmd, want, 2u);
}

static void case_present(void)
{
    struct iosvm_op_present args;
    const uint32_t want[2] = {0x71u, 0u};
    struct iosvm_cmd cmd;

    args.image = want[0];
    args.reserved = want[1];
    roundtrip(iosvm_encode_present(g_buf, sizeof(g_buf), 15u, &args), &cmd, 24u);
    CHECK_EQ(cmd.op, IOSVM_OP_PRESENT);
    check_payload(&cmd, want, 2u);
}

static void case_screenshot(void)
{
    struct iosvm_op_screenshot args;
    const uint32_t want[6] = {0x81u, 0x82u, 16u, 32u, 64u, 48u};
    struct iosvm_cmd cmd;

    args.image = want[0];
    args.dst = want[1];
    args.x = want[2];
    args.y = want[3];
    args.width = want[4];
    args.height = want[5];
    roundtrip(iosvm_encode_screenshot(g_buf, sizeof(g_buf), 16u, &args), &cmd, 40u);
    CHECK_EQ(cmd.op, IOSVM_OP_SCREENSHOT);
    check_payload(&cmd, want, 6u);
}

static void case_ack(void)
{
    struct iosvm_ack in;
    struct iosvm_ack out;
    uint8_t raw[IOSVM_ACK_WIRE_SIZE];

    in.seq = 42u;
    in.status = IOSVM_ERR_BAD_HANDLE;
    in.handle = 0x99u;
    in.reserved = 0u;
    CHECK_EQ(iosvm_encode_ack(raw, sizeof(raw), &in), (int32_t)IOSVM_ACK_WIRE_SIZE);
    CHECK_EQ(iosvm_decode_ack(raw, sizeof(raw), &out), (int32_t)IOSVM_ACK_WIRE_SIZE);
    CHECK_EQ(out.seq, 42u);
    CHECK_EQ(out.status, IOSVM_ERR_BAD_HANDLE);
    CHECK_EQ(out.handle, 0x99u);
    CHECK_EQ(out.reserved, 0u);

    /* 0 and positive statuses survive too. */
    in.status = IOSVM_OK;
    CHECK_EQ(iosvm_encode_ack(raw, sizeof(raw), &in), (int32_t)IOSVM_ACK_WIRE_SIZE);
    CHECK_EQ(iosvm_decode_ack(raw, sizeof(raw), &out), (int32_t)IOSVM_ACK_WIRE_SIZE);
    CHECK_EQ(out.status, 0);

    CHECK_EQ(iosvm_decode_ack(raw, 8u, &out), IOSVM_ERR_TRUNCATED);
    CHECK_EQ(iosvm_decode_ack(NULL, sizeof(raw), &out), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_decode_ack(raw, sizeof(raw), NULL), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_encode_ack(raw, 8u, &in), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_encode_ack(NULL, sizeof(raw), &in), IOSVM_ERR_BOUNDS);
}

static void case_truncated(void)
{
    struct iosvm_cmd cmd;

    /* A SUBMIT record is 24 bytes; claim 16 payload bytes and offer 32. */
    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_SUBMIT, 1u, 16u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(g_buf, 32u, &cmd), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_decode_one(g_buf, 24u, &cmd), IOSVM_ERR_TRUNCATED);
    CHECK_EQ(iosvm_decode_one(g_buf, 16u, &cmd), IOSVM_ERR_TRUNCATED);
    CHECK_EQ(iosvm_decode_one(g_buf, 15u, &cmd), IOSVM_ERR_TRUNCATED);
    CHECK_EQ(iosvm_decode_one(g_buf, 0u, &cmd), IOSVM_ERR_TRUNCATED);
    CHECK_EQ(iosvm_decode_one(NULL, 32u, &cmd), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_decode_one(g_buf, 32u, NULL), IOSVM_ERR_BOUNDS);
}

static void case_bad_magic(void)
{
    struct iosvm_cmd cmd;

    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), 0xdeadbeefu, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_SUBMIT, 1u, 8u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    memset(g_buf + IOSVM_CMD_HDR_SIZE, 0, 8u);
    CHECK_EQ(iosvm_decode_one(g_buf, 24u, &cmd), IOSVM_ERR_UNKNOWN_OP);

    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), 0u, IOSVM_PROTO_VERSION, IOSVM_OP_SUBMIT,
                                  1u, 8u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    memset(g_buf + IOSVM_CMD_HDR_SIZE, 0, 8u);
    CHECK_EQ(iosvm_decode_one(g_buf, 24u, &cmd), IOSVM_ERR_UNKNOWN_OP);
}

static void case_version(void)
{
    struct iosvm_cmd cmd;

    /* Major mismatch: never understood. */
    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, 0x0201u, IOSVM_OP_SUBMIT,
                                  1u, 8u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(g_buf, 24u, &cmd), IOSVM_ERR_VERSION);

    /* Newer minor: rejected rather than partially understood. */
    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, 0x0002u, IOSVM_OP_SUBMIT,
                                  1u, 8u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(g_buf, 24u, &cmd), IOSVM_ERR_VERSION);

    /* Older minor: accepted. */
    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, 0x0000u, IOSVM_OP_SUBMIT,
                                  1u, 8u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    memset(g_buf + IOSVM_CMD_HDR_SIZE, 0, 8u);
    CHECK_EQ(iosvm_decode_one(g_buf, 24u, &cmd), 24);
    CHECK_EQ(cmd.op, IOSVM_OP_SUBMIT);
}

static void case_unknown_op(void)
{
    struct iosvm_cmd cmd;

    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_INVALID, 1u, 8u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(g_buf, 24u, &cmd), IOSVM_ERR_UNKNOWN_OP);

    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  (uint16_t)IOSVM_OP_MAX, 1u, 8u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(g_buf, 24u, &cmd), IOSVM_ERR_UNKNOWN_OP);

    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  0x7777u, 1u, 8u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(g_buf, 24u, &cmd), IOSVM_ERR_UNKNOWN_OP);
}

static void case_misaligned(void)
{
    struct iosvm_cmd cmd;

    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_SUBMIT, 1u, 12u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(g_buf, 40u, &cmd), IOSVM_ERR_BAD_ALIGN);

    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_BLIT_COPY, 1u, 20u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(g_buf, 40u, &cmd), IOSVM_ERR_BAD_ALIGN);
}

static void case_payload_length(void)
{
    struct iosvm_op_submit args;
    struct iosvm_op_destroy args_d;
    struct iosvm_cmd cmd;

    /* A DESTROY is 8 bytes; claim 16 and provide them. */
    args_d.handle = 1u;
    args_d.reserved = 0u;
    CHECK_EQ(iosvm_encode_destroy(g_buf, sizeof(g_buf), 1u, &args_d), 24);
    iosvm_put_le32(g_buf + IOSVM_CMD_HDR_LEN_OFF, 16u);
    memset(g_buf + 24u, 0, 16u);
    CHECK_EQ(iosvm_decode_one(g_buf, 40u, &cmd), IOSVM_ERR_BOUNDS);

    /* A SUBMIT is 8 bytes; claim 16. */
    args.cmd_count = 1u;
    args.flags = 0u;
    CHECK_EQ(iosvm_encode_submit(g_buf, sizeof(g_buf), 1u, &args), 24);
    iosvm_put_le32(g_buf + IOSVM_CMD_HDR_LEN_OFF, 16u);
    CHECK_EQ(iosvm_decode_one(g_buf, 40u, &cmd), IOSVM_ERR_BOUNDS);

    /* WRITE with no room for a single data byte. */
    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_WRITE, 1u, 16u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(g_buf, 32u, &cmd), IOSVM_ERR_BOUNDS);

    /* Fixed op with an empty payload. */
    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_CREATE_BUFFER, 1u, 0u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(g_buf, 16u, &cmd), IOSVM_ERR_BOUNDS);
}

static void case_hostile_length(void)
{
    struct iosvm_cmd cmd;
    uint8_t small[32];
    uint8_t copy[32];

    /* A length that would need hundreds of megabytes, offered 32 bytes. */
    memset(small, 0, sizeof(small));
    CHECK_EQ(iosvm_encode_cmd_hdr(small, sizeof(small), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_WRITE, 1u, 0xfffffff8u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    memcpy(copy, small, sizeof(copy));
    CHECK_EQ(iosvm_decode_one(small, sizeof(small), &cmd), IOSVM_ERR_TRUNCATED);
    CHECK_MEM(small, copy, sizeof(small));

    /* Just past the declared buffer by one byte. */
    memset(small, 0, sizeof(small));
    CHECK_EQ(iosvm_encode_cmd_hdr(small, sizeof(small), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_WRITE, 1u, 16u),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    CHECK_EQ(iosvm_decode_one(small, 31u, &cmd), IOSVM_ERR_TRUNCATED);
    CHECK_EQ(iosvm_decode_one(small, 32u, &cmd), IOSVM_ERR_BOUNDS);
}

static void case_encode_capacity(void)
{
    struct iosvm_op_create_buffer args;
    struct iosvm_op_screenshot shot;
    uint8_t small[64];
    uint32_t i;

    args.handle = 1u;
    args.size = 16u;
    args.usage = 0u;
    args.flags = 0u;

    memset(small, 0xaa, sizeof(small));
    CHECK_EQ(iosvm_encode_create_buffer(small, 31u, 1u, &args), IOSVM_ERR_BOUNDS);
    for (i = 0; i < sizeof(small); i++) {
        CHECK_EQ(small[i], 0xaau);
    }

    /* Exactly enough room works and does not touch the bytes after it. */
    memset(small, 0xaa, sizeof(small));
    CHECK_EQ(iosvm_encode_create_buffer(small, 32u, 1u, &args), 32);
    CHECK_EQ(small[32], 0xaau);
    CHECK_EQ(small[63], 0xaau);

    memset(small, 0xaa, sizeof(small));
    CHECK_EQ(iosvm_encode_cmd_hdr(small, 15u, IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_SUBMIT, 1u, 8u),
             IOSVM_ERR_BOUNDS);
    CHECK_EQ(small[0], 0xaau);

    CHECK_EQ(iosvm_encode_create_buffer(NULL, 64u, 1u, &args), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_encode_create_buffer(small, 64u, 1u, NULL), IOSVM_ERR_BOUNDS);
    shot.image = 1u;
    shot.dst = 2u;
    shot.x = 0u;
    shot.y = 0u;
    shot.width = 1u;
    shot.height = 1u;
    CHECK_EQ(iosvm_encode_screenshot(small, 40u, 1u, &shot), 40);
    CHECK_EQ(iosvm_encode_screenshot(small, 39u, 1u, &shot), IOSVM_ERR_BOUNDS);
}

static void case_cmd_u32_bounds(void)
{
    struct iosvm_op_create_buffer args;
    struct iosvm_cmd cmd;
    uint32_t word = 0u;

    args.handle = 1u;
    args.size = 2u;
    args.usage = 0u;
    args.flags = 0u;
    CHECK_EQ(iosvm_encode_create_buffer(g_buf, sizeof(g_buf), 1u, &args), 32);
    CHECK_EQ(iosvm_decode_one(g_buf, 32u, &cmd), 32);

    CHECK_EQ(iosvm_cmd_u32(&cmd, 3u, &word), IOSVM_OK);
    CHECK_EQ(word, 0u);
    CHECK_EQ(iosvm_cmd_u32(&cmd, 4u, &word), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_cmd_u32(&cmd, 0xffffffffu, &word), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_cmd_u32(NULL, 0u, &word), IOSVM_ERR_BOUNDS);
    CHECK_EQ(iosvm_cmd_u32(&cmd, 0u, NULL), IOSVM_ERR_BOUNDS);
}

static void case_strerror(void)
{
    static const int32_t codes[] = {
        IOSVM_OK,
        IOSVM_ERR_VERSION,
        IOSVM_ERR_UNKNOWN_OP,
        IOSVM_ERR_BOUNDS,
        IOSVM_ERR_BAD_HANDLE,
        IOSVM_ERR_FULL,
        IOSVM_ERR_TRUNCATED,
        IOSVM_ERR_BAD_ALIGN,
        IOSVM_ERR_SEQ,
    };
    uint32_t i;

    for (i = 0u; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *msg = iosvm_strerror(codes[i]);

        CHECK(msg != NULL);
        CHECK(msg[0] != '\0');
    }
    CHECK(iosvm_strerror(1234) != NULL);
    CHECK(iosvm_strerror(-99) != NULL);
    CHECK(strcmp(iosvm_strerror(IOSVM_OK), iosvm_strerror(IOSVM_ERR_SEQ)) != 0);
}

/*
 * A WRITE record carries its data length twice: the envelope says how many payload bytes follow,
 * and the payload's own header says how many of them are data (the rest is padding). They must
 * agree. Accepting a disagreement is how a permissive decoder hands a host a length it will trust
 * and a buffer that does not have it.
 */
static void build_write(uint32_t envelope_payload, uint32_t inner_len)
{
    CHECK_EQ(iosvm_encode_cmd_hdr(g_buf, sizeof(g_buf), IOSVM_PROTO_MAGIC, IOSVM_PROTO_VERSION,
                                  IOSVM_OP_WRITE, 1u, envelope_payload),
             (int32_t)IOSVM_CMD_HDR_SIZE);
    memset(g_buf + IOSVM_CMD_HDR_SIZE, 0, envelope_payload);
    iosvm_put_le32(g_buf + IOSVM_CMD_HDR_SIZE + offsetof(struct iosvm_op_write, len), inner_len);
}

static void case_write_length_mismatch(void)
{
    struct iosvm_cmd cmd;

    /* Consistent: 8 data bytes need a 24-byte envelope. */
    build_write(24u, 8u);
    CHECK_EQ(iosvm_decode_one(g_buf, IOSVM_CMD_HDR_SIZE + 24u, &cmd),
             (int32_t)(IOSVM_CMD_HDR_SIZE + 24u));

    /* Envelope says 8 bytes of data, the payload claims 100. */
    build_write(24u, 100u);
    CHECK_EQ(iosvm_decode_one(g_buf, IOSVM_CMD_HDR_SIZE + 24u, &cmd), IOSVM_ERR_BOUNDS);

    /* Padding rule: 7 data bytes need align8(7) = 8, so a 24-byte envelope, and 32 is two bytes too
     * many. (len 7 with a 24-byte envelope is legal — the first version of this case had that
     * wrong.) */
    build_write(32u, 7u);
    CHECK_EQ(iosvm_decode_one(g_buf, IOSVM_CMD_HDR_SIZE + 32u, &cmd), IOSVM_ERR_BOUNDS);

    /* Zero-length write is not a write. */
    build_write(24u, 0u);
    CHECK_EQ(iosvm_decode_one(g_buf, IOSVM_CMD_HDR_SIZE + 24u, &cmd), IOSVM_ERR_BOUNDS);

    /* One byte over the inline limit. */
    build_write(24u, IOSVM_MAX_INLINE_WRITE + 1u);
    CHECK_EQ(iosvm_decode_one(g_buf, IOSVM_CMD_HDR_SIZE + 24u, &cmd), IOSVM_ERR_BOUNDS);

    /* A big envelope with a tiny inner length: refused before anything is read from it. */
    build_write(65552u, 1u);
    CHECK_EQ(iosvm_decode_one(g_buf, IOSVM_CMD_HDR_SIZE + 65552u, &cmd), IOSVM_ERR_BOUNDS);

    /* And the largest legal one is still accepted. */
    build_write(IOSVM_OP_WRITE_SIZE + IOSVM_MAX_INLINE_WRITE, IOSVM_MAX_INLINE_WRITE);
    CHECK_EQ(iosvm_decode_one(g_buf, IOSVM_MAX_RECORD_SIZE, &cmd), (int32_t)IOSVM_MAX_RECORD_SIZE);
}

void iosvm_test_codec(void)
{
    iosvm_run_case("codec: create_buffer round trip", case_create_buffer);
    iosvm_run_case("codec: create_image round trip", case_create_image);
    iosvm_run_case("codec: destroy round trip", case_destroy);
    iosvm_run_case("codec: write round trip pads with zeroes", case_write);
    iosvm_run_case("codec: blit_copy round trip", case_blit_copy);
    iosvm_run_case("codec: blit_fill round trip", case_blit_fill);
    iosvm_run_case("codec: submit round trip", case_submit);
    iosvm_run_case("codec: fence_wait round trip", case_fence_wait);
    iosvm_run_case("codec: present round trip", case_present);
    iosvm_run_case("codec: screenshot round trip", case_screenshot);
    iosvm_run_case("codec: ack round trip keeps negative status", case_ack);
    iosvm_run_case("codec: truncated records are rejected", case_truncated);
    iosvm_run_case("codec: bad magic is rejected", case_bad_magic);
    iosvm_run_case("codec: version mismatch is rejected", case_version);
    iosvm_run_case("codec: unknown opcodes are rejected", case_unknown_op);
    iosvm_run_case("codec: misaligned payload length is rejected", case_misaligned);
    iosvm_run_case("codec: payload length must match the opcode", case_payload_length);
    iosvm_run_case("codec: WRITE envelope and inner length must agree", case_write_length_mismatch);
    iosvm_run_case("codec: hostile length never reads past the buffer", case_hostile_length);
    iosvm_run_case("codec: encoders refuse short buffers", case_encode_capacity);
    iosvm_run_case("codec: cmd_u32 is bounds checked", case_cmd_u32_bounds);
    iosvm_run_case("codec: strerror covers every code", case_strerror);
}
