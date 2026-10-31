/* SPDX-License-Identifier: MIT */
/*
 * Command and ack codec for the IOSVM GPU protocol.
 *
 * Encoding writes a complete record (16-byte header plus payload) into a caller
 * buffer and returns its size. Decoding validates the envelope and hands back a
 * generic struct iosvm_cmd whose `payload` points into the caller's buffer.
 *
 * The decoder treats the input as hostile: every length is checked against the
 * remaining bytes before it is used, and no byte past the buffer is read.
 *
 * Decode failure mapping (the error set is closed, see iosvm_gpu.h):
 *   bad magic               -> IOSVM_ERR_UNKNOWN_OP (it is not an IOSVM record)
 *   version not understood  -> IOSVM_ERR_VERSION
 *   opcode outside 1..MAX-1 -> IOSVM_ERR_UNKNOWN_OP
 *   header or payload short -> IOSVM_ERR_TRUNCATED
 *   length not 8-aligned    -> IOSVM_ERR_BAD_ALIGN
 *   length != op's payload  -> IOSVM_ERR_BOUNDS
 */
#ifndef IOSVM_CODEC_H
#define IOSVM_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "iosvm_gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Little-endian wire accessors                                              */
/* ------------------------------------------------------------------------- */

/* Byte-wise on purpose: works on unaligned pointers and any host endianness. */
uint16_t iosvm_get_le16(const uint8_t *p);
uint32_t iosvm_get_le32(const uint8_t *p);
void iosvm_put_le16(uint8_t *p, uint16_t v);
void iosvm_put_le32(uint8_t *p, uint32_t v);

/* ------------------------------------------------------------------------- */
/* Decoding                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Decode one command record from `buf` (at least `len` bytes readable).
 * Returns the number of bytes the record occupies (16 + payload length) on
 * success, or a negative IOSVM_ERR_* value.
 *
 * The returned payload length always equals what the opcode requires, so a
 * caller may read the fixed fields with iosvm_cmd_u32() without re-checking.
 */
int32_t iosvm_decode_one(const void *buf, size_t len, struct iosvm_cmd *out);

/*
 * Read the `index`-th little-endian u32 from a decoded payload.
 * Returns IOSVM_ERR_BOUNDS when the payload is shorter than 4*(index+1) bytes.
 */
int32_t iosvm_cmd_u32(const struct iosvm_cmd *cmd, uint32_t index, uint32_t *out);

/* Decode one fixed-size ack. Returns IOSVM_ACK_WIRE_SIZE or a negative error. */
int32_t iosvm_decode_ack(const void *buf, size_t len, struct iosvm_ack *out);

/* ------------------------------------------------------------------------- */
/* Low-level encoding                                                        */
/* ------------------------------------------------------------------------- */

/*
 * Write a bare envelope with an arbitrary magic/version/op and payload length.
 * Used by tools and tests to build malformed records; it validates capacity
 * only. Returns IOSVM_CMD_HDR_SIZE or IOSVM_ERR_BOUNDS.
 */
int32_t iosvm_encode_cmd_hdr(uint8_t *buf, size_t cap, uint32_t magic, uint16_t version,
                             uint16_t op, uint32_t seq, uint32_t len);

/* ------------------------------------------------------------------------- */
/* Per-op encoding                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Every encoder writes a complete record (header + payload) at `buf`, which must
 * have room for IOSVM_CMD_HDR_SIZE plus the op's payload size. `seq` must be
 * non-zero and monotonically increasing; the sequence is not validated here.
 * Returns the record size in bytes or a negative IOSVM_ERR_* value
 * (IOSVM_ERR_BOUNDS for a buffer that is too small).
 */
int32_t iosvm_encode_create_buffer(uint8_t *buf, size_t cap, uint32_t seq,
                                   const struct iosvm_op_create_buffer *args);
int32_t iosvm_encode_create_image(uint8_t *buf, size_t cap, uint32_t seq,
                                  const struct iosvm_op_create_image *args);
int32_t iosvm_encode_destroy(uint8_t *buf, size_t cap, uint32_t seq,
                             const struct iosvm_op_destroy *args);
int32_t iosvm_encode_blit_copy(uint8_t *buf, size_t cap, uint32_t seq,
                               const struct iosvm_op_blit_copy *args);
int32_t iosvm_encode_blit_fill(uint8_t *buf, size_t cap, uint32_t seq,
                               const struct iosvm_op_blit_fill *args);
int32_t iosvm_encode_submit(uint8_t *buf, size_t cap, uint32_t seq,
                            const struct iosvm_op_submit *args);
int32_t iosvm_encode_fence_wait(uint8_t *buf, size_t cap, uint32_t seq,
                                const struct iosvm_op_fence_wait *args);
int32_t iosvm_encode_present(uint8_t *buf, size_t cap, uint32_t seq,
                             const struct iosvm_op_present *args);
int32_t iosvm_encode_screenshot(uint8_t *buf, size_t cap, uint32_t seq,
                                const struct iosvm_op_screenshot *args);

/*
 * WRITE is the only variable-length op: `data_len` bytes from `data` are copied
 * after the 16-byte fixed part and zero-padded up to IOSVM_PROTO_ALIGN bytes.
 * Returns the record size or a negative error (IOSVM_ERR_BOUNDS when the buffer
 * is too small, `data_len` is zero or exceeds IOSVM_MAX_INLINE_WRITE, or `data`
 * is NULL while `data_len` is not).
 */
int32_t iosvm_encode_write(uint8_t *buf, size_t cap, uint32_t seq, uint32_t handle, uint32_t offset,
                           const void *data, uint32_t data_len);

/* Write one fixed-size ack. Returns IOSVM_ACK_WIRE_SIZE or IOSVM_ERR_BOUNDS. */
int32_t iosvm_encode_ack(uint8_t *buf, size_t cap, const struct iosvm_ack *ack);

/* ------------------------------------------------------------------------- */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------- */

/* Human-readable name of a status code; never NULL. */
const char *iosvm_strerror(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* IOSVM_CODEC_H */
