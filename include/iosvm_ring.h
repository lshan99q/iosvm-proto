/* SPDX-License-Identifier: MIT */
/*
 * Single-producer/single-consumer shared-memory ring for the IOSVM GPU protocol.
 *
 * A ring region starts with a 32-byte iosvm_ring_hdr and is followed by two
 * bodies: a command body (guest -> host) and an ack body (host -> guest). Both
 * bodies use the same primitives; the caller says which one it means with an
 * enum iosvm_ring_id.
 *
 * PRODUCER                                    CONSUMER
 *   iosvm_ring_reserve()  -> bytes to fill      iosvm_ring_peek()    -> record
 *   fill the reservation                        decode / use the record
 *   iosvm_ring_commit()   -> publish            iosvm_ring_release() -> retire
 *
 * Contracts:
 *   - reserve() returns NULL when the record cannot be placed right now: the
 *     ring is full, the tail gap is too small for it, or an earlier reservation
 *     has not been committed. NULL is not an error; retry after the consumer
 *     advances (this is how flow control works).
 *   - Exactly one reservation may be outstanding per ring: the caller must
 *     commit() (or, if it decides not to publish, must not call commit; the
 *     reservation is simply never published and the producer stays stuck until
 *     it does commit). v0.1 has no abort primitive on purpose.
 *   - peek() returns a pointer into the ring and the record length (header
 *     included). It is only valid until the next ring call on that side.
 *   - release() must be given the length peek() returned; anything else is
 *     refused with IOSVM_ERR_BOUNDS and the ring is left untouched.
 *   - used() is the number of published but unreleased bytes; 0 means empty.
 *     The usable capacity is body_size - 1 bytes.
 *
 * Records never straddle the end of the command body; the producer zero-fills
 * the tail gap and restarts at offset 0 (see the invariants in iosvm_gpu.h).
 *
 * Both bodies start 8-byte aligned, and the caller's region must be 8-byte
 * aligned. Command records are framed by iosvm_cmd_hdr; ack entries are fixed
 * IOSVM_ACK_WIRE_SIZE slots, so the ack body must be a multiple of 16 bytes.
 */
#ifndef IOSVM_RING_H
#define IOSVM_RING_H

#include <stddef.h>
#include <stdint.h>

#include "iosvm_gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Alignment required of the caller's ring memory and of every body offset. */
#define IOSVM_RING_ALIGN IOSVM_PROTO_ALIGN

/* Smallest body a ring may be formatted with. */
#define IOSVM_RING_MIN_SIZE 256u

/* Largest body accepted, a sanity bound against overflowing the offsets. */
#define IOSVM_RING_MAX_SIZE 0x40000000u

enum iosvm_ring_id {
    IOSVM_RING_CMD = 0, /* guest -> host command records */
    IOSVM_RING_ACK = 1, /* host -> guest acks, fixed 16-byte entries */
};

/*
 * Caller-owned ring handle. It holds no protocol state of its own: the counters
 * live in the shared region so that a second process can attach to it. Treat the
 * fields as private.
 */
struct iosvm_ring {
    uint8_t *mem;      /* start of the ring region (holds iosvm_ring_hdr) */
    uint32_t mem_size; /* total bytes of the region */
    uint8_t *cmd;      /* command body */
    uint32_t cmd_size; /* command body size in bytes */
    uint8_t *ack;      /* ack body */
    uint32_t ack_size; /* ack body size in bytes */

    /* producer state, one outstanding reservation per body */
    uint8_t *pending[2];
    uint32_t pending_len[2];

    /* consumer state, one outstanding peek per body */
    uint32_t peek_len[2];
};

/*
 * Format a ring in caller-provided memory and initialise `ring`.
 *
 * `cmd_size` is the command body size in bytes; the ack body gets the rest of
 * the region. Both sizes must be multiples of IOSVM_RING_ALIGN; the ack body
 * must also be a multiple of IOSVM_ACK_WIRE_SIZE. The whole region is zeroed.
 * Returns IOSVM_OK, IOSVM_ERR_BAD_ALIGN (region or sizes misaligned) or
 * IOSVM_ERR_BOUNDS (NULL, too small, sizes out of range).
 */
int32_t iosvm_ring_init(struct iosvm_ring *ring, void *mem, size_t mem_size, uint32_t cmd_size);

/*
 * Reserve `len` writable bytes at the producer head (rounded up to
 * IOSVM_PROTO_ALIGN) and return them, or NULL if the ring cannot take the
 * record now. `len` must be non-zero; on the ack ring it must be a multiple of
 * IOSVM_ACK_WIRE_SIZE.
 */
uint8_t *iosvm_ring_reserve(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len);

/*
 * Publish the reservation. `len` must be the same length that was reserved
 * (after alignment), otherwise nothing is published and IOSVM_ERR_BOUNDS is
 * returned. Returns IOSVM_OK, IOSVM_ERR_BOUNDS or IOSVM_ERR_TRUNCATED.
 */
int32_t iosvm_ring_commit(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len);

/*
 * Peek at the next published record.
 *
 * On success *rec points into the ring and *len is the total record length
 * (16 + payload for command records, IOSVM_ACK_WIRE_SIZE for acks). When the
 * ring is empty *rec is NULL and *len is 0 and the return value is IOSVM_OK.
 *
 * Returns IOSVM_ERR_TRUNCATED when the published bytes are not a well formed
 * record (corrupt head/tail, a record longer than the body, a partial record at
 * the wrap point) and IOSVM_ERR_BAD_ALIGN when the record length is not a
 * multiple of IOSVM_PROTO_ALIGN.
 */
int32_t iosvm_ring_peek(struct iosvm_ring *ring, enum iosvm_ring_id which, const uint8_t **rec,
                        uint32_t *len);

/*
 * Retire the record returned by the last peek(). `len` must equal that length.
 * Returns IOSVM_OK, IOSVM_ERR_BOUNDS (no peek, or wrong length) or
 * IOSVM_ERR_TRUNCATED (corrupt tail).
 */
int32_t iosvm_ring_release(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len);

/* Published but unreleased bytes in a body; 0 when empty or on bad arguments. */
uint32_t iosvm_ring_used(const struct iosvm_ring *ring, enum iosvm_ring_id which);

#ifdef __cplusplus
}
#endif

#endif /* IOSVM_RING_H */
