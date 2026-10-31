/* SPDX-License-Identifier: MIT */
/*
 * IOSVM GPU wire protocol, version 0.1.
 *
 * IOSVM runs iOS in QEMU on Linux and translates Metal to Vulkan inside the
 * guest. This header is the only contract between the two halves that do not
 * exist yet: the guest translator writes command records into a shared-memory
 * ring, a virtio device on the host picks them up, and a host backend replays
 * them as Vulkan calls.
 *
 * INVARIANTS
 * ----------
 * Ownership of the ring header
 *   - `magic`, `version`, `cmd_size` and `ack_size` are written once by the
 *     ring formatter and are read-only afterwards.
 *   - `cmd_head` is written only by the command producer (guest) and read by
 *     the command consumer (host).
 *   - `cmd_tail` is written only by the command consumer (host).
 *   - `ack_head` is written only by the ack producer (host).
 *   - `ack_tail` is written only by the ack consumer (guest).
 *   A side never writes a counter it does not own, and never writes into the
 *   other side's ring body. The guest owns command records once published; the
 *   host never rewrites them in place.
 *
 * Capacity and wrapping
 *   - `cmd_head`/`cmd_tail`/`ack_head`/`ack_tail` are cumulative byte offsets:
 *     they count every byte that side has ever produced or consumed, and they
 *     advance modulo P = floor(2^32 / size) * size, the largest multiple of the
 *     body size that fits in uint32_t (P == 2^32 for a power-of-two body).
 *     They are not indices and must not be compared against `size`; a body
 *     offset is `counter % size`, and because size divides P a counter wrap
 *     leaves that offset unchanged. Only differences between the two counters
 *     are meaningful.
 *   - used == (head - tail) mod P and is exact while used <= size. head == tail
 *     always means empty and used == size means full, so both states are
 *     representable. There is no unconditional "capacity is size - 1" reserve:
 *     capacity depends on where the counters are and on the padding the next
 *     record needs. With need = align8(record length) and gap = size - head %
 *     size, the producer accepts exactly when used + pad + need <= size, where
 *     pad is gap when need > gap and 0 otherwise.
 *   - A record never straddles the end of the body. When the tail gap cannot
 *     hold the next record the producer zero-fills the gap, advances head to the
 *     next multiple of `size` and restarts at offset 0; the consumer sees the
 *     zero word and skips the same gap. A zero word is unambiguous padding
 *     because every real command starts with IOSVM_PROTO_MAGIC.
 *
 * Alignment
 *   - Ring bodies start at an 8-byte aligned address, every command starts at
 *     an 8-byte aligned byte offset, the command header is 16 bytes, and every
 *     payload length is a multiple of 8. Every fixed payload struct below is a
 *     multiple of 8 bytes; _Static_assert()s in this header enforce that.
 *
 * Versioning and unknown opcodes
 *   - A host that does not understand a guest rejects it. An unknown opcode is
 *     answered with IOSVM_ERR_UNKNOWN_OP, never skipped: skipping would
 *     desynchronise the one-ack-per-sequence-number stream and let a newer
 *     guest drive an older host into undefined behaviour.
 *   - The version is major.minor (major = high byte). A peer must match the
 *     major and must not be newer in the minor, otherwise IOSVM_ERR_VERSION.
 *
 * Endianness
 *   - Every multi-byte field on the wire is little-endian, including the ring
 *     header counters. Both sides convert explicitly; no struct in this header
 *     is ever memcpy()ed to or from the wire shared memory.
 *
 * Concurrency
 *   - One producer and one consumer per ring. The only ordering requirement in
 *     v0.1 is a compiler barrier between writing a record's bytes and
 *     publishing head, and between reading head and reading the bytes; see
 *     iosvm_barrier(). Cross-VM memory ordering is not implemented in v0.1 and
 *     is the virtio device's job once it exists.
 */
#ifndef IOSVM_GPU_H
#define IOSVM_GPU_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 'IOSV' as a u32. 0x56534F49 stored little-endian produces the wire bytes
 * 'I', 'O', 'S', 'V' (0x49, 0x4f, 0x53, 0x56). The C multi-character constant
 * 'IOSV' is implementation-defined (GCC and Clang evaluate it to 0x494F5356),
 * so the value is spelled out and the byte order is asserted in the tests.
 */
#define IOSVM_PROTO_MAGIC 0x56534F49u

/* major.minor; major = high byte, minor = low byte. */
#define IOSVM_PROTO_VERSION 0x0001u
#define IOSVM_PROTO_MAJOR(v) ((uint16_t)(((v) >> 8) & 0xffu))
#define IOSVM_PROTO_MINOR(v) ((uint16_t)((v) & 0xffu))
#define IOSVM_PROTO_VERSION_MAJOR IOSVM_PROTO_MAJOR(IOSVM_PROTO_VERSION)
#define IOSVM_PROTO_VERSION_MINOR IOSVM_PROTO_MINOR(IOSVM_PROTO_VERSION)

/* Every ring body, record and payload length is a multiple of this. */
#define IOSVM_PROTO_ALIGN 8u

/* Status codes. Zero is success; every failure is negative and errno style. */
enum iosvm_status {
    IOSVM_OK = 0,
    /* Peer speaks a major version we do not, or a newer minor. */
    IOSVM_ERR_VERSION = -1,
    /* Opcode is not in this protocol version (also used for a bad magic). */
    IOSVM_ERR_UNKNOWN_OP = -2,
    /* A range, size or argument is outside what the operation allows. */
    IOSVM_ERR_BOUNDS = -3,
    /* Handle is unknown, already in use, zero, or names the wrong resource. */
    IOSVM_ERR_BAD_HANDLE = -4,
    /* The ring cannot take another record right now. */
    IOSVM_ERR_FULL = -5,
    /* Buffer ended before the bytes the header promised. */
    IOSVM_ERR_TRUNCATED = -6,
    /* A length or address is not aligned to IOSVM_PROTO_ALIGN. */
    IOSVM_ERR_BAD_ALIGN = -7,
    /* Sequence number did not advance. */
    IOSVM_ERR_SEQ = -8,
};

/* ------------------------------------------------------------------------- */
/* Ring header                                                               */
/* ------------------------------------------------------------------------- */

#define IOSVM_RING_HDR_MAGIC_OFF 0u
#define IOSVM_RING_HDR_VERSION_OFF 4u
#define IOSVM_RING_HDR_CMD_HEAD_OFF 8u
#define IOSVM_RING_HDR_CMD_TAIL_OFF 12u
#define IOSVM_RING_HDR_ACK_HEAD_OFF 16u
#define IOSVM_RING_HDR_ACK_TAIL_OFF 20u
#define IOSVM_RING_HDR_CMD_SIZE_OFF 24u
#define IOSVM_RING_HDR_ACK_SIZE_OFF 28u
#define IOSVM_RING_HDR_SIZE 32u

/*
 * Layout of the 32 bytes at the start of a ring region. Documented here for the
 * device side; src/ring.c accesses the fields byte-wise instead of casting this
 * struct over possibly unaligned shared memory.
 */
struct iosvm_ring_hdr {
    uint32_t magic;    /* IOSVM_PROTO_MAGIC */
    uint32_t version;  /* IOSVM_PROTO_VERSION */
    uint32_t cmd_head; /* cumulative, producer writes */
    uint32_t cmd_tail; /* cumulative, consumer writes */
    uint32_t ack_head; /* cumulative, ack producer writes */
    uint32_t ack_tail; /* cumulative, ack consumer writes */
    uint32_t cmd_size; /* bytes in the command body */
    uint32_t ack_size; /* bytes in the ack body */
};

_Static_assert(sizeof(struct iosvm_ring_hdr) == 32u, "ring header must be 32 bytes");
_Static_assert(offsetof(struct iosvm_ring_hdr, magic) == IOSVM_RING_HDR_MAGIC_OFF,
               "ring header magic offset");
_Static_assert(offsetof(struct iosvm_ring_hdr, version) == IOSVM_RING_HDR_VERSION_OFF,
               "ring header version offset");
_Static_assert(offsetof(struct iosvm_ring_hdr, cmd_head) == IOSVM_RING_HDR_CMD_HEAD_OFF,
               "ring header cmd_head offset");
_Static_assert(offsetof(struct iosvm_ring_hdr, cmd_tail) == IOSVM_RING_HDR_CMD_TAIL_OFF,
               "ring header cmd_tail offset");
_Static_assert(offsetof(struct iosvm_ring_hdr, ack_head) == IOSVM_RING_HDR_ACK_HEAD_OFF,
               "ring header ack_head offset");
_Static_assert(offsetof(struct iosvm_ring_hdr, ack_tail) == IOSVM_RING_HDR_ACK_TAIL_OFF,
               "ring header ack_tail offset");
_Static_assert(offsetof(struct iosvm_ring_hdr, cmd_size) == IOSVM_RING_HDR_CMD_SIZE_OFF,
               "ring header cmd_size offset");
_Static_assert(offsetof(struct iosvm_ring_hdr, ack_size) == IOSVM_RING_HDR_ACK_SIZE_OFF,
               "ring header ack_size offset");

/* ------------------------------------------------------------------------- */
/* Command envelope                                                          */
/* ------------------------------------------------------------------------- */

#define IOSVM_CMD_HDR_MAGIC_OFF 0u
#define IOSVM_CMD_HDR_VERSION_OFF 4u
#define IOSVM_CMD_HDR_OP_OFF 6u
#define IOSVM_CMD_HDR_SEQ_OFF 8u
#define IOSVM_CMD_HDR_LEN_OFF 12u
#define IOSVM_CMD_HDR_SIZE 16u

/* Every command record: this header followed by exactly `len` payload bytes. */
struct iosvm_cmd_hdr {
    uint32_t magic;   /* IOSVM_PROTO_MAGIC */
    uint16_t version; /* IOSVM_PROTO_VERSION */
    uint16_t op;      /* enum iosvm_op */
    uint32_t seq;     /* guest command sequence, strictly increasing */
    uint32_t len;     /* payload bytes that follow, multiple of 8 */
};

_Static_assert(sizeof(struct iosvm_cmd_hdr) == IOSVM_CMD_HDR_SIZE,
               "command header must be 16 bytes");
_Static_assert(offsetof(struct iosvm_cmd_hdr, magic) == IOSVM_CMD_HDR_MAGIC_OFF,
               "command header magic offset");
_Static_assert(offsetof(struct iosvm_cmd_hdr, version) == IOSVM_CMD_HDR_VERSION_OFF,
               "command header version offset");
_Static_assert(offsetof(struct iosvm_cmd_hdr, op) == IOSVM_CMD_HDR_OP_OFF,
               "command header op offset");
_Static_assert(offsetof(struct iosvm_cmd_hdr, seq) == IOSVM_CMD_HDR_SEQ_OFF,
               "command header seq offset");
_Static_assert(offsetof(struct iosvm_cmd_hdr, len) == IOSVM_CMD_HDR_LEN_OFF,
               "command header len offset");

/* ------------------------------------------------------------------------- */
/* Opcodes                                                                   */
/* ------------------------------------------------------------------------- */

enum iosvm_op {
    IOSVM_OP_INVALID = 0,
    IOSVM_OP_CREATE_BUFFER = 1,
    IOSVM_OP_CREATE_IMAGE = 2,
    IOSVM_OP_DESTROY = 3,
    IOSVM_OP_WRITE = 4,
    IOSVM_OP_BLIT_COPY = 5,
    IOSVM_OP_BLIT_FILL = 6,
    IOSVM_OP_SUBMIT = 7,
    IOSVM_OP_FENCE_WAIT = 8,
    IOSVM_OP_PRESENT = 9,
    IOSVM_OP_SCREENSHOT = 10,
    /* One past the highest defined opcode; not a valid opcode. */
    IOSVM_OP_MAX = 11,
};

/*
 * A command whose sequence number is not strictly greater than the previous one
 * is rejected with IOSVM_ERR_SEQ. Sequence numbers start at 1; 0 is reserved.
 */

/* ------------------------------------------------------------------------- */
/* Payloads                                                                  */
/* ------------------------------------------------------------------------- */

/* IOSVM_OP_CREATE_BUFFER: a linear byte buffer owned by the handle. */
#define IOSVM_OP_CREATE_BUFFER_SIZE 16u
struct iosvm_op_create_buffer {
    uint32_t handle; /* guest chosen, non-zero, must not already exist */
    uint32_t size;   /* bytes, non-zero */
    uint32_t usage;  /* bitmask, must be 0 in v0.1 */
    uint32_t flags;  /* must be 0 in v0.1 */
};

_Static_assert(sizeof(struct iosvm_op_create_buffer) == IOSVM_OP_CREATE_BUFFER_SIZE,
               "create_buffer must be 16 bytes");
_Static_assert(offsetof(struct iosvm_op_create_buffer, handle) == 0u, "create_buffer.handle");
_Static_assert(offsetof(struct iosvm_op_create_buffer, size) == 4u, "create_buffer.size");
_Static_assert(offsetof(struct iosvm_op_create_buffer, usage) == 8u, "create_buffer.usage");
_Static_assert(offsetof(struct iosvm_op_create_buffer, flags) == 12u, "create_buffer.flags");

/* IOSVM_OP_CREATE_IMAGE: a w*h RGBA8 image, rows packed tightly, w*4 bytes each. */
#define IOSVM_OP_CREATE_IMAGE_SIZE 24u
struct iosvm_op_create_image {
    uint32_t handle; /* guest chosen, non-zero, must not already exist */
    uint32_t width;  /* pixels, 1..IOSVM_MAX_IMAGE_DIM */
    uint32_t height; /* pixels, 1..IOSVM_MAX_IMAGE_DIM */
    uint32_t format; /* IOSVM_FORMAT_RGBA8 */
    uint32_t usage;  /* bitmask, must be 0 in v0.1 */
    uint32_t flags;  /* must be 0 in v0.1 */
};

_Static_assert(sizeof(struct iosvm_op_create_image) == IOSVM_OP_CREATE_IMAGE_SIZE,
               "create_image must be 24 bytes");
_Static_assert(offsetof(struct iosvm_op_create_image, handle) == 0u, "create_image.handle");
_Static_assert(offsetof(struct iosvm_op_create_image, width) == 4u, "create_image.width");
_Static_assert(offsetof(struct iosvm_op_create_image, height) == 8u, "create_image.height");
_Static_assert(offsetof(struct iosvm_op_create_image, format) == 12u, "create_image.format");
_Static_assert(offsetof(struct iosvm_op_create_image, usage) == 16u, "create_image.usage");
_Static_assert(offsetof(struct iosvm_op_create_image, flags) == 20u, "create_image.flags");

/* IOSVM_OP_DESTROY: releases a buffer or an image. */
#define IOSVM_OP_DESTROY_SIZE 8u
struct iosvm_op_destroy {
    uint32_t handle;   /* must exist */
    uint32_t reserved; /* must be 0 */
};

_Static_assert(sizeof(struct iosvm_op_destroy) == IOSVM_OP_DESTROY_SIZE, "destroy must be 8 bytes");
_Static_assert(offsetof(struct iosvm_op_destroy, handle) == 0u, "destroy.handle");
_Static_assert(offsetof(struct iosvm_op_destroy, reserved) == 4u, "destroy.reserved");

/*
 * IOSVM_OP_WRITE: `len` bytes of inline data followed by 0..7 padding bytes,
 * written at `offset` into the resource's linear byte image. The payload length
 * on the wire is 16 + align8(len).
 */
#define IOSVM_OP_WRITE_SIZE 16u
struct iosvm_op_write {
    uint32_t handle;   /* buffer or image */
    uint32_t offset;   /* byte offset into the resource */
    uint32_t len;      /* bytes of inline data, 1..IOSVM_MAX_INLINE_WRITE */
    uint32_t reserved; /* must be 0 */
};

_Static_assert(sizeof(struct iosvm_op_write) == IOSVM_OP_WRITE_SIZE,
               "write header must be 16 bytes");
_Static_assert(offsetof(struct iosvm_op_write, handle) == 0u, "write.handle");
_Static_assert(offsetof(struct iosvm_op_write, offset) == 4u, "write.offset");
_Static_assert(offsetof(struct iosvm_op_write, len) == 8u, "write.len");
_Static_assert(offsetof(struct iosvm_op_write, reserved) == 12u, "write.reserved");

/* IOSVM_OP_BLIT_COPY: byte copy between two resources. */
#define IOSVM_OP_BLIT_COPY_SIZE 24u
struct iosvm_op_blit_copy {
    uint32_t dst;        /* destination handle */
    uint32_t dst_offset; /* byte offset in the destination */
    uint32_t src;        /* source handle */
    uint32_t src_offset; /* byte offset in the source */
    uint32_t len;        /* bytes, non-zero */
    uint32_t reserved;   /* must be 0 */
};

_Static_assert(sizeof(struct iosvm_op_blit_copy) == IOSVM_OP_BLIT_COPY_SIZE,
               "blit_copy must be 24 bytes");
_Static_assert(offsetof(struct iosvm_op_blit_copy, dst) == 0u, "blit_copy.dst");
_Static_assert(offsetof(struct iosvm_op_blit_copy, dst_offset) == 4u, "blit_copy.dst_offset");
_Static_assert(offsetof(struct iosvm_op_blit_copy, src) == 8u, "blit_copy.src");
_Static_assert(offsetof(struct iosvm_op_blit_copy, src_offset) == 12u, "blit_copy.src_offset");
_Static_assert(offsetof(struct iosvm_op_blit_copy, len) == 16u, "blit_copy.len");
_Static_assert(offsetof(struct iosvm_op_blit_copy, reserved) == 20u, "blit_copy.reserved");
_Static_assert(IOSVM_OP_BLIT_COPY_SIZE % IOSVM_PROTO_ALIGN == 0u, "blit_copy alignment");

/* IOSVM_OP_BLIT_FILL: fill `len` bytes with `value` repeated (native byte order). */
#define IOSVM_OP_BLIT_FILL_SIZE 16u
struct iosvm_op_blit_fill {
    uint32_t dst;        /* destination handle */
    uint32_t dst_offset; /* byte offset in the destination */
    uint32_t len;        /* bytes, non-zero, multiple of 4 */
    uint32_t value;      /* 32-bit pattern repeated over the range */
};

_Static_assert(sizeof(struct iosvm_op_blit_fill) == IOSVM_OP_BLIT_FILL_SIZE,
               "blit_fill must be 16 bytes");
_Static_assert(offsetof(struct iosvm_op_blit_fill, dst) == 0u, "blit_fill.dst");
_Static_assert(offsetof(struct iosvm_op_blit_fill, dst_offset) == 4u, "blit_fill.dst_offset");
_Static_assert(offsetof(struct iosvm_op_blit_fill, len) == 8u, "blit_fill.len");
_Static_assert(offsetof(struct iosvm_op_blit_fill, value) == 12u, "blit_fill.value");

/*
 * IOSVM_OP_SUBMIT: closes the current command batch. `cmd_count` must equal the
 * number of WRITE/BLIT_COPY/BLIT_FILL commands queued since the previous SUBMIT
 * and must be non-zero. The ack's handle field is the fence id (the 1-based
 * count of successful submits).
 */
#define IOSVM_OP_SUBMIT_SIZE 8u
struct iosvm_op_submit {
    uint32_t cmd_count; /* commands since the last submit, non-zero */
    uint32_t flags;     /* must be 0 in v0.1 */
};

_Static_assert(sizeof(struct iosvm_op_submit) == IOSVM_OP_SUBMIT_SIZE, "submit must be 8 bytes");
_Static_assert(offsetof(struct iosvm_op_submit, cmd_count) == 0u, "submit.cmd_count");
_Static_assert(offsetof(struct iosvm_op_submit, flags) == 4u, "submit.flags");

/* IOSVM_OP_FENCE_WAIT: wait for a fence id returned by an earlier SUBMIT ack. */
#define IOSVM_OP_FENCE_WAIT_SIZE 8u
struct iosvm_op_fence_wait {
    uint32_t fence_id;   /* 1-based submit counter, non-zero */
    uint32_t timeout_ms; /* carried on the wire, not enforced in v0.1 */
};

_Static_assert(sizeof(struct iosvm_op_fence_wait) == IOSVM_OP_FENCE_WAIT_SIZE,
               "fence_wait must be 8 bytes");
_Static_assert(offsetof(struct iosvm_op_fence_wait, fence_id) == 0u, "fence_wait.fence_id");
_Static_assert(offsetof(struct iosvm_op_fence_wait, timeout_ms) == 4u, "fence_wait.timeout_ms");

/* IOSVM_OP_PRESENT: hand an image to the display path. */
#define IOSVM_OP_PRESENT_SIZE 8u
struct iosvm_op_present {
    uint32_t image;    /* image handle */
    uint32_t reserved; /* must be 0 */
};

_Static_assert(sizeof(struct iosvm_op_present) == IOSVM_OP_PRESENT_SIZE, "present must be 8 bytes");
_Static_assert(offsetof(struct iosvm_op_present, image) == 0u, "present.image");
_Static_assert(offsetof(struct iosvm_op_present, reserved) == 4u, "present.reserved");

/*
 * IOSVM_OP_SCREENSHOT: copy the w*h region at (x, y) of `image` into `dst` at
 * byte offset 0, rows packed tightly, RGBA8.
 */
#define IOSVM_OP_SCREENSHOT_SIZE 24u
struct iosvm_op_screenshot {
    uint32_t image;  /* source image handle */
    uint32_t dst;    /* destination buffer handle */
    uint32_t x;      /* left edge in pixels */
    uint32_t y;      /* top edge in pixels */
    uint32_t width;  /* region width in pixels, non-zero */
    uint32_t height; /* region height in pixels, non-zero */
};

_Static_assert(sizeof(struct iosvm_op_screenshot) == IOSVM_OP_SCREENSHOT_SIZE,
               "screenshot must be 24 bytes");
_Static_assert(offsetof(struct iosvm_op_screenshot, image) == 0u, "screenshot.image");
_Static_assert(offsetof(struct iosvm_op_screenshot, dst) == 4u, "screenshot.dst");
_Static_assert(offsetof(struct iosvm_op_screenshot, x) == 8u, "screenshot.x");
_Static_assert(offsetof(struct iosvm_op_screenshot, y) == 12u, "screenshot.y");
_Static_assert(offsetof(struct iosvm_op_screenshot, width) == 16u, "screenshot.width");
_Static_assert(offsetof(struct iosvm_op_screenshot, height) == 20u, "screenshot.height");

/* ------------------------------------------------------------------------- */
/* Images and limits                                                         */
/* ------------------------------------------------------------------------- */

enum iosvm_format {
    IOSVM_FORMAT_INVALID = 0,
    IOSVM_FORMAT_RGBA8 = 1,
    IOSVM_FORMAT_MAX = 2,
};

/* Bytes per pixel of IOSVM_FORMAT_RGBA8. */
#define IOSVM_RGBA8_BPP 4u

/* Largest image edge accepted by v0.1, in pixels. */
#define IOSVM_MAX_IMAGE_DIM 16384u

/*
 * Largest inline data payload a single WRITE may carry. Bigger transfers are
 * expected to go buffer-to-buffer through the guest's own staging memory; v0.1
 * has no scatter/gather yet.
 */
#define IOSVM_MAX_INLINE_WRITE 65536u

/* The largest record the producer may build: WRITE header plus max inline data. */
#define IOSVM_MAX_RECORD_SIZE (IOSVM_CMD_HDR_SIZE + IOSVM_OP_WRITE_SIZE + IOSVM_MAX_INLINE_WRITE)

/* ------------------------------------------------------------------------- */
/* Acks                                                                      */
/* ------------------------------------------------------------------------- */

#define IOSVM_ACK_WIRE_SIZE 16u

/* One ack per consumed command, in command order. */
struct iosvm_ack {
    uint32_t seq;      /* sequence number of the command being acked */
    int32_t status;    /* IOSVM_OK or a negative error */
    uint32_t handle;   /* handle the command produced or addressed, 0 if none */
    uint32_t reserved; /* must be 0 */
};

_Static_assert(sizeof(struct iosvm_ack) == IOSVM_ACK_WIRE_SIZE, "ack must be 16 bytes");
_Static_assert(offsetof(struct iosvm_ack, seq) == 0u, "ack.seq");
_Static_assert(offsetof(struct iosvm_ack, status) == 4u, "ack.status");
_Static_assert(offsetof(struct iosvm_ack, handle) == 8u, "ack.handle");
_Static_assert(offsetof(struct iosvm_ack, reserved) == 12u, "ack.reserved");

/* ------------------------------------------------------------------------- */
/* Decoded command (host-side convenience view, host order)                  */
/* ------------------------------------------------------------------------- */

/* iosvm_decode_one() fills this in; `payload` points into the caller's buffer. */
struct iosvm_cmd {
    uint32_t magic;         /* validated == IOSVM_PROTO_MAGIC */
    uint16_t version;       /* wire version, host order */
    uint16_t op;            /* enum iosvm_op, host order */
    uint32_t seq;           /* host order */
    uint32_t len;           /* payload length in bytes */
    const uint8_t *payload; /* payload bytes inside the decoded buffer */
};

_Static_assert(offsetof(struct iosvm_cmd, magic) == 0u, "cmd.magic");
_Static_assert(offsetof(struct iosvm_cmd, version) == 4u, "cmd.version");
_Static_assert(offsetof(struct iosvm_cmd, op) == 6u, "cmd.op");
_Static_assert(offsetof(struct iosvm_cmd, seq) == 8u, "cmd.seq");
_Static_assert(offsetof(struct iosvm_cmd, len) == 12u, "cmd.len");
_Static_assert(offsetof(struct iosvm_cmd, payload) == 16u, "cmd.payload");

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* True when a peer speaking `version` is understood by this build. */
static inline int iosvm_version_compatible(uint16_t version)
{
    return IOSVM_PROTO_MAJOR(version) == IOSVM_PROTO_VERSION_MAJOR &&
           IOSVM_PROTO_MINOR(version) <= IOSVM_PROTO_VERSION_MINOR;
}

/*
 * Compiler barrier: keeps the compiler from moving record bytes across the
 * counter update. This is not a cross-VM barrier (see the header comment).
 */
static inline void iosvm_barrier(void)
{
    atomic_signal_fence(memory_order_seq_cst);
}

/* Round a length up to the protocol alignment. */
static inline uint32_t iosvm_align8(uint32_t len)
{
    return (len + (IOSVM_PROTO_ALIGN - 1u)) & ~(IOSVM_PROTO_ALIGN - 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* IOSVM_GPU_H */
