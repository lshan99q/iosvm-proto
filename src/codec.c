/* SPDX-License-Identifier: MIT */
/*
 * Command and ack codec. All wire fields are little-endian and are converted
 * field by field, so the code neither depends on host endianness nor on the
 * alignment of the caller's buffer.
 */
#include "iosvm_codec.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Little-endian wire accessors                                              */
/* ------------------------------------------------------------------------- */

uint16_t iosvm_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

uint32_t iosvm_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void iosvm_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

void iosvm_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Payload size of every fixed-length opcode, indexed by opcode. WRITE is
 * variable and is handled separately by the decoder (it may never appear here).
 */
static uint32_t op_payload_size(uint16_t op)
{
    static const uint32_t sizes[IOSVM_OP_MAX] = {
        0u, /* IOSVM_OP_INVALID: never decodable */
        (uint32_t)sizeof(struct iosvm_op_create_buffer),
        (uint32_t)sizeof(struct iosvm_op_create_image),
        (uint32_t)sizeof(struct iosvm_op_destroy),
        (uint32_t)sizeof(struct iosvm_op_write), /* fixed part only */
        (uint32_t)sizeof(struct iosvm_op_blit_copy),
        (uint32_t)sizeof(struct iosvm_op_blit_fill),
        (uint32_t)sizeof(struct iosvm_op_submit),
        (uint32_t)sizeof(struct iosvm_op_fence_wait),
        (uint32_t)sizeof(struct iosvm_op_present),
        (uint32_t)sizeof(struct iosvm_op_screenshot),
    };

    return sizes[op];
}

/*
 * Write the envelope and hand back a pointer to the payload area.
 * Returns the total record size or IOSVM_ERR_BOUNDS. Nothing is written when
 * the buffer is too small.
 */
static int32_t wire_begin(uint8_t *buf, size_t cap, uint16_t op, uint32_t seq, uint32_t payload_len,
                          uint8_t **payload)
{
    size_t total;

    if (buf == NULL || payload == NULL) {
        return IOSVM_ERR_BOUNDS;
    }

    total = (size_t)payload_len + (size_t)IOSVM_CMD_HDR_SIZE;
    if (cap < total) {
        return IOSVM_ERR_BOUNDS;
    }

    iosvm_put_le32(buf + IOSVM_CMD_HDR_MAGIC_OFF, IOSVM_PROTO_MAGIC);
    iosvm_put_le16(buf + IOSVM_CMD_HDR_VERSION_OFF, IOSVM_PROTO_VERSION);
    iosvm_put_le16(buf + IOSVM_CMD_HDR_OP_OFF, op);
    iosvm_put_le32(buf + IOSVM_CMD_HDR_SEQ_OFF, seq);
    iosvm_put_le32(buf + IOSVM_CMD_HDR_LEN_OFF, payload_len);

    *payload = buf + IOSVM_CMD_HDR_SIZE;
    return (int32_t)total;
}

/* ------------------------------------------------------------------------- */
/* Decoding                                                                  */
/* ------------------------------------------------------------------------- */

int32_t iosvm_decode_one(const void *buf, size_t len, struct iosvm_cmd *out)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint16_t version;
    uint16_t op;
    uint32_t payload_len;
    size_t total;

    if (p == NULL || out == NULL) {
        return IOSVM_ERR_BOUNDS;
    }
    if (len < (size_t)IOSVM_CMD_HDR_SIZE) {
        return IOSVM_ERR_TRUNCATED;
    }
    if (iosvm_get_le32(p + IOSVM_CMD_HDR_MAGIC_OFF) != IOSVM_PROTO_MAGIC) {
        return IOSVM_ERR_UNKNOWN_OP;
    }

    version = iosvm_get_le16(p + IOSVM_CMD_HDR_VERSION_OFF);
    if (!iosvm_version_compatible(version)) {
        return IOSVM_ERR_VERSION;
    }

    op = iosvm_get_le16(p + IOSVM_CMD_HDR_OP_OFF);
    if (op <= (uint16_t)IOSVM_OP_INVALID || op >= (uint16_t)IOSVM_OP_MAX) {
        return IOSVM_ERR_UNKNOWN_OP;
    }

    payload_len = iosvm_get_le32(p + IOSVM_CMD_HDR_LEN_OFF);
    /* Compare before adding: `payload_len + IOSVM_CMD_HDR_SIZE` can wrap on a 32-bit host. */
    if ((size_t)payload_len > len - (size_t)IOSVM_CMD_HDR_SIZE) {
        /* The header promises more bytes than the caller has. Never read them. */
        return IOSVM_ERR_TRUNCATED;
    }
    total = (size_t)payload_len + (size_t)IOSVM_CMD_HDR_SIZE;
    if ((payload_len % IOSVM_PROTO_ALIGN) != 0u) {
        return IOSVM_ERR_BAD_ALIGN;
    }

    if (op == (uint16_t)IOSVM_OP_WRITE) {
        /*
         * The payload is `len` bytes of data plus padding, and the envelope says how long it is.
         * The two must agree: a host that trusts the envelope without reading `len` would read or
         * write the wrong range, and a decoder that trusts `len` alone would let a peer promise
         * bytes it did not send.
         */
        uint32_t data_len;

        /* At least one data byte, rounded up to the alignment. */
        if (payload_len < IOSVM_OP_WRITE_SIZE + IOSVM_PROTO_ALIGN) {
            return IOSVM_ERR_BOUNDS;
        }
        if (payload_len - IOSVM_OP_WRITE_SIZE > iosvm_align8(IOSVM_MAX_INLINE_WRITE)) {
            return IOSVM_ERR_BOUNDS;
        }
        data_len = iosvm_get_le32(p + IOSVM_CMD_HDR_SIZE + offsetof(struct iosvm_op_write, len));
        if (data_len == 0u || data_len > IOSVM_MAX_INLINE_WRITE) {
            return IOSVM_ERR_BOUNDS;
        }
        if (payload_len != IOSVM_OP_WRITE_SIZE + iosvm_align8(data_len)) {
            return IOSVM_ERR_BOUNDS;
        }
    } else if (payload_len != op_payload_size(op)) {
        return IOSVM_ERR_BOUNDS;
    }

    if (total > (size_t)INT32_MAX) {
        return IOSVM_ERR_BOUNDS;
    }

    out->magic = IOSVM_PROTO_MAGIC;
    out->version = version;
    out->op = op;
    out->seq = iosvm_get_le32(p + IOSVM_CMD_HDR_SEQ_OFF);
    out->len = payload_len;
    out->payload = p + IOSVM_CMD_HDR_SIZE;
    return (int32_t)total;
}

int32_t iosvm_cmd_u32(const struct iosvm_cmd *cmd, uint32_t index, uint32_t *out)
{
    size_t off;

    if (cmd == NULL || out == NULL || cmd->payload == NULL) {
        return IOSVM_ERR_BOUNDS;
    }
    if ((size_t)index > (size_t)cmd->len / 4u) {
        return IOSVM_ERR_BOUNDS;
    }

    off = (size_t)index * 4u;
    if (off + 4u > (size_t)cmd->len) {
        return IOSVM_ERR_BOUNDS;
    }

    *out = iosvm_get_le32(cmd->payload + off);
    return IOSVM_OK;
}

int32_t iosvm_decode_ack(const void *buf, size_t len, struct iosvm_ack *out)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t status;

    if (p == NULL || out == NULL) {
        return IOSVM_ERR_BOUNDS;
    }
    if (len < (size_t)IOSVM_ACK_WIRE_SIZE) {
        return IOSVM_ERR_TRUNCATED;
    }

    out->seq = iosvm_get_le32(p + offsetof(struct iosvm_ack, seq));
    status = iosvm_get_le32(p + offsetof(struct iosvm_ack, status));
    memcpy(&out->status, &status, sizeof(out->status));
    out->handle = iosvm_get_le32(p + offsetof(struct iosvm_ack, handle));
    out->reserved = iosvm_get_le32(p + offsetof(struct iosvm_ack, reserved));
    return (int32_t)IOSVM_ACK_WIRE_SIZE;
}

/* ------------------------------------------------------------------------- */
/* Encoding                                                                  */
/* ------------------------------------------------------------------------- */

int32_t iosvm_encode_cmd_hdr(uint8_t *buf, size_t cap, uint32_t magic, uint16_t version,
                             uint16_t op, uint32_t seq, uint32_t len)
{
    if (buf == NULL) {
        return IOSVM_ERR_BOUNDS;
    }
    if (cap < (size_t)IOSVM_CMD_HDR_SIZE) {
        return IOSVM_ERR_BOUNDS;
    }

    iosvm_put_le32(buf + IOSVM_CMD_HDR_MAGIC_OFF, magic);
    iosvm_put_le16(buf + IOSVM_CMD_HDR_VERSION_OFF, version);
    iosvm_put_le16(buf + IOSVM_CMD_HDR_OP_OFF, op);
    iosvm_put_le32(buf + IOSVM_CMD_HDR_SEQ_OFF, seq);
    iosvm_put_le32(buf + IOSVM_CMD_HDR_LEN_OFF, len);
    return (int32_t)IOSVM_CMD_HDR_SIZE;
}

#define IOSVM_ENCODE_CHECK(cond)                                                                   \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            return IOSVM_ERR_BOUNDS;                                                               \
        }                                                                                          \
    } while (0)

int32_t iosvm_encode_create_buffer(uint8_t *buf, size_t cap, uint32_t seq,
                                   const struct iosvm_op_create_buffer *args)
{
    uint8_t *p = NULL;
    int32_t total;

    IOSVM_ENCODE_CHECK(args != NULL);
    total = wire_begin(buf, cap, (uint16_t)IOSVM_OP_CREATE_BUFFER, seq, IOSVM_OP_CREATE_BUFFER_SIZE,
                       &p);
    if (total < 0) {
        return total;
    }
    iosvm_put_le32(p + offsetof(struct iosvm_op_create_buffer, handle), args->handle);
    iosvm_put_le32(p + offsetof(struct iosvm_op_create_buffer, size), args->size);
    iosvm_put_le32(p + offsetof(struct iosvm_op_create_buffer, usage), args->usage);
    iosvm_put_le32(p + offsetof(struct iosvm_op_create_buffer, flags), args->flags);
    return total;
}

int32_t iosvm_encode_create_image(uint8_t *buf, size_t cap, uint32_t seq,
                                  const struct iosvm_op_create_image *args)
{
    uint8_t *p = NULL;
    int32_t total;

    IOSVM_ENCODE_CHECK(args != NULL);
    total =
        wire_begin(buf, cap, (uint16_t)IOSVM_OP_CREATE_IMAGE, seq, IOSVM_OP_CREATE_IMAGE_SIZE, &p);
    if (total < 0) {
        return total;
    }
    iosvm_put_le32(p + offsetof(struct iosvm_op_create_image, handle), args->handle);
    iosvm_put_le32(p + offsetof(struct iosvm_op_create_image, width), args->width);
    iosvm_put_le32(p + offsetof(struct iosvm_op_create_image, height), args->height);
    iosvm_put_le32(p + offsetof(struct iosvm_op_create_image, format), args->format);
    iosvm_put_le32(p + offsetof(struct iosvm_op_create_image, usage), args->usage);
    iosvm_put_le32(p + offsetof(struct iosvm_op_create_image, flags), args->flags);
    return total;
}

int32_t iosvm_encode_destroy(uint8_t *buf, size_t cap, uint32_t seq,
                             const struct iosvm_op_destroy *args)
{
    uint8_t *p = NULL;
    int32_t total;

    IOSVM_ENCODE_CHECK(args != NULL);
    total = wire_begin(buf, cap, (uint16_t)IOSVM_OP_DESTROY, seq, IOSVM_OP_DESTROY_SIZE, &p);
    if (total < 0) {
        return total;
    }
    iosvm_put_le32(p + offsetof(struct iosvm_op_destroy, handle), args->handle);
    iosvm_put_le32(p + offsetof(struct iosvm_op_destroy, reserved), args->reserved);
    return total;
}

int32_t iosvm_encode_write(uint8_t *buf, size_t cap, uint32_t seq, uint32_t handle, uint32_t offset,
                           const void *data, uint32_t data_len)
{
    uint8_t *p = NULL;
    uint32_t padded;
    int32_t total;

    if (data == NULL || data_len == 0u || data_len > IOSVM_MAX_INLINE_WRITE) {
        return IOSVM_ERR_BOUNDS;
    }
    padded = iosvm_align8(data_len);
    total = wire_begin(buf, cap, (uint16_t)IOSVM_OP_WRITE, seq, IOSVM_OP_WRITE_SIZE + padded, &p);
    if (total < 0) {
        return total;
    }
    iosvm_put_le32(p + offsetof(struct iosvm_op_write, handle), handle);
    iosvm_put_le32(p + offsetof(struct iosvm_op_write, offset), offset);
    iosvm_put_le32(p + offsetof(struct iosvm_op_write, len), data_len);
    iosvm_put_le32(p + offsetof(struct iosvm_op_write, reserved), 0u);
    memcpy(p + IOSVM_OP_WRITE_SIZE, data, (size_t)data_len);
    if (padded > data_len) {
        memset(p + IOSVM_OP_WRITE_SIZE + data_len, 0, (size_t)(padded - data_len));
    }
    return total;
}

int32_t iosvm_encode_blit_copy(uint8_t *buf, size_t cap, uint32_t seq,
                               const struct iosvm_op_blit_copy *args)
{
    uint8_t *p = NULL;
    int32_t total;

    IOSVM_ENCODE_CHECK(args != NULL);
    total = wire_begin(buf, cap, (uint16_t)IOSVM_OP_BLIT_COPY, seq, IOSVM_OP_BLIT_COPY_SIZE, &p);
    if (total < 0) {
        return total;
    }
    iosvm_put_le32(p + offsetof(struct iosvm_op_blit_copy, dst), args->dst);
    iosvm_put_le32(p + offsetof(struct iosvm_op_blit_copy, dst_offset), args->dst_offset);
    iosvm_put_le32(p + offsetof(struct iosvm_op_blit_copy, src), args->src);
    iosvm_put_le32(p + offsetof(struct iosvm_op_blit_copy, src_offset), args->src_offset);
    iosvm_put_le32(p + offsetof(struct iosvm_op_blit_copy, len), args->len);
    iosvm_put_le32(p + offsetof(struct iosvm_op_blit_copy, reserved), args->reserved);
    return total;
}

int32_t iosvm_encode_blit_fill(uint8_t *buf, size_t cap, uint32_t seq,
                               const struct iosvm_op_blit_fill *args)
{
    uint8_t *p = NULL;
    int32_t total;

    IOSVM_ENCODE_CHECK(args != NULL);
    total = wire_begin(buf, cap, (uint16_t)IOSVM_OP_BLIT_FILL, seq, IOSVM_OP_BLIT_FILL_SIZE, &p);
    if (total < 0) {
        return total;
    }
    iosvm_put_le32(p + offsetof(struct iosvm_op_blit_fill, dst), args->dst);
    iosvm_put_le32(p + offsetof(struct iosvm_op_blit_fill, dst_offset), args->dst_offset);
    iosvm_put_le32(p + offsetof(struct iosvm_op_blit_fill, len), args->len);
    iosvm_put_le32(p + offsetof(struct iosvm_op_blit_fill, value), args->value);
    return total;
}

int32_t iosvm_encode_submit(uint8_t *buf, size_t cap, uint32_t seq,
                            const struct iosvm_op_submit *args)
{
    uint8_t *p = NULL;
    int32_t total;

    IOSVM_ENCODE_CHECK(args != NULL);
    total = wire_begin(buf, cap, (uint16_t)IOSVM_OP_SUBMIT, seq, IOSVM_OP_SUBMIT_SIZE, &p);
    if (total < 0) {
        return total;
    }
    iosvm_put_le32(p + offsetof(struct iosvm_op_submit, cmd_count), args->cmd_count);
    iosvm_put_le32(p + offsetof(struct iosvm_op_submit, flags), args->flags);
    return total;
}

int32_t iosvm_encode_fence_wait(uint8_t *buf, size_t cap, uint32_t seq,
                                const struct iosvm_op_fence_wait *args)
{
    uint8_t *p = NULL;
    int32_t total;

    IOSVM_ENCODE_CHECK(args != NULL);
    total = wire_begin(buf, cap, (uint16_t)IOSVM_OP_FENCE_WAIT, seq, IOSVM_OP_FENCE_WAIT_SIZE, &p);
    if (total < 0) {
        return total;
    }
    iosvm_put_le32(p + offsetof(struct iosvm_op_fence_wait, fence_id), args->fence_id);
    iosvm_put_le32(p + offsetof(struct iosvm_op_fence_wait, timeout_ms), args->timeout_ms);
    return total;
}

int32_t iosvm_encode_present(uint8_t *buf, size_t cap, uint32_t seq,
                             const struct iosvm_op_present *args)
{
    uint8_t *p = NULL;
    int32_t total;

    IOSVM_ENCODE_CHECK(args != NULL);
    total = wire_begin(buf, cap, (uint16_t)IOSVM_OP_PRESENT, seq, IOSVM_OP_PRESENT_SIZE, &p);
    if (total < 0) {
        return total;
    }
    iosvm_put_le32(p + offsetof(struct iosvm_op_present, image), args->image);
    iosvm_put_le32(p + offsetof(struct iosvm_op_present, reserved), args->reserved);
    return total;
}

int32_t iosvm_encode_screenshot(uint8_t *buf, size_t cap, uint32_t seq,
                                const struct iosvm_op_screenshot *args)
{
    uint8_t *p = NULL;
    int32_t total;

    IOSVM_ENCODE_CHECK(args != NULL);
    total = wire_begin(buf, cap, (uint16_t)IOSVM_OP_SCREENSHOT, seq, IOSVM_OP_SCREENSHOT_SIZE, &p);
    if (total < 0) {
        return total;
    }
    iosvm_put_le32(p + offsetof(struct iosvm_op_screenshot, image), args->image);
    iosvm_put_le32(p + offsetof(struct iosvm_op_screenshot, dst), args->dst);
    iosvm_put_le32(p + offsetof(struct iosvm_op_screenshot, x), args->x);
    iosvm_put_le32(p + offsetof(struct iosvm_op_screenshot, y), args->y);
    iosvm_put_le32(p + offsetof(struct iosvm_op_screenshot, width), args->width);
    iosvm_put_le32(p + offsetof(struct iosvm_op_screenshot, height), args->height);
    return total;
}

int32_t iosvm_encode_ack(uint8_t *buf, size_t cap, const struct iosvm_ack *ack)
{
    uint32_t status;

    if (buf == NULL || ack == NULL) {
        return IOSVM_ERR_BOUNDS;
    }
    if (cap < (size_t)IOSVM_ACK_WIRE_SIZE) {
        return IOSVM_ERR_BOUNDS;
    }

    memcpy(&status, &ack->status, sizeof(status));
    iosvm_put_le32(buf + offsetof(struct iosvm_ack, seq), ack->seq);
    iosvm_put_le32(buf + offsetof(struct iosvm_ack, status), status);
    iosvm_put_le32(buf + offsetof(struct iosvm_ack, handle), ack->handle);
    iosvm_put_le32(buf + offsetof(struct iosvm_ack, reserved), ack->reserved);
    return (int32_t)IOSVM_ACK_WIRE_SIZE;
}

/* ------------------------------------------------------------------------- */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------- */

const char *iosvm_strerror(int32_t status)
{
    switch (status) {
    case IOSVM_OK:
        return "ok";
    case IOSVM_ERR_VERSION:
        return "protocol version not understood";
    case IOSVM_ERR_UNKNOWN_OP:
        return "unknown opcode or not an IOSVM record";
    case IOSVM_ERR_BOUNDS:
        return "range, size or argument out of bounds";
    case IOSVM_ERR_BAD_HANDLE:
        return "unknown, reused or invalid handle";
    case IOSVM_ERR_FULL:
        return "ring or memory is full";
    case IOSVM_ERR_TRUNCATED:
        return "buffer or record truncated";
    case IOSVM_ERR_BAD_ALIGN:
        return "length or address is not 8-byte aligned";
    case IOSVM_ERR_SEQ:
        return "sequence number did not advance";
    default:
        return "unknown status";
    }
}
