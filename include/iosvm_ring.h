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
 * COUNTER REPRESENTATION
 *   head and tail are cumulative byte offsets, NOT indices in [0, size). They
 *   advance modulo P, the largest multiple of the body size that fits in a
 *   uint32_t:
 *
 *       P = floor(2^32 / size) * size        (so 2^31 < P <= 2^32)
 *
 *   Because `size` divides P, wrapping a counter does not change
 *   `counter % size`, which is the offset of that byte inside the body. used is
 *   `(head - tail) modulo P` and is exact while used <= size, so head == tail
 *   always means empty and a full ring (used == size) is representable. For a
 *   power-of-two body P is exactly 2^32: plain uint32_t wraparound.
 *
 *   Counters are not indices and must never be compared against `size`. A
 *   counter counts every byte that side has ever produced or consumed; only
 *   differences between the two counters are meaningful, and only modulo P.
 *
 * CAPACITY AND PADDING
 *   A record never straddles the end of the body. When the bytes before the end
 *   cannot hold it, the producer zeroes that gap (the padding marker: a zero
 *   32-bit word, which is unambiguous because every command record starts with
 *   IOSVM_PROTO_MAGIC), advances head to the next multiple of `size`, and
 *   places the record at offset 0 of the next lap. The consumer recognises the
 *   zero word and skips the same gap.
 *
 *   Capacity therefore depends on where the counters are and on the padding a
 *   record needs. With
 *
 *       need = align8(len)
 *       pad  = (need > size - head % size) ? size - head % size : 0
 *
 *   reserve() succeeds exactly when `used + pad + need <= size`. A body of
 *   `size` bytes can hold `size` bytes when the records line up with it; unlike
 *   the old index representation there is no unconditional size - 1 reserve.
 *   The only permanent refusal is `need > size` (or a geometry/API rule).
 *
 *   There is no way to reset the counters, so a record that is longer than
 *   `max(head % size, size - head % size)` cannot be placed into an empty ring:
 *   it would have to straddle either the end of the body or the start of the
 *   next lap. Keep the largest record at or below half the body size and an
 *   empty ring can always accept it, whatever the counter position.
 *
 * CONTRACTS
 *   - reserve() returns NULL when the record cannot be placed right now (the
 *     formula above fails), when need > size, when an earlier reservation has
 *     not been committed, or when the length breaks the ring's rules (see the
 *     function comment). NULL is not an error; retry after the consumer
 *     advances (this is how flow control works).
 *   - Exactly one reservation may be outstanding per ring: the caller must
 *     commit() (or, if it decides not to publish, must not call commit; the
 *     reservation is simply never published and the producer stays stuck until
 *     it does commit). v0.1 has no abort primitive on purpose.
 *   - commit() publishes the length the record header encodes, not the
 *     reservation length, so a caller cannot accidentally publish an unfilled
 *     tail. Command records must carry IOSVM_PROTO_MAGIC and an 8-aligned
 *     payload length that fits the reservation; a commit that fails publishes
 *     nothing and leaves the reservation outstanding so the caller can fix the
 *     bytes and retry. Ack reservations are fixed 16-byte slots and publish the
 *     whole slot.
 *   - peek() returns a pointer into the ring and the record length (header
 *     included); it skips wrap padding and never returns a zero-length record.
 *     The pointer is only valid until the next ring call on that side.
 *   - release() must be given the length peek() returned; anything else is
 *     refused with IOSVM_ERR_BOUNDS and the ring is left untouched. Padding is
 *     retired by peek(), not by release().
 *   - used() is the number of published but unreleased bytes, padding that the
 *     consumer has not skipped yet included; 0 means empty.
 *
 * Both bodies start 8-byte aligned, and the caller's region must be 8-byte
 * aligned. Command records are framed by iosvm_cmd_hdr; ack entries are fixed
 * IOSVM_ACK_WIRE_SIZE slots, so the ack body must be a multiple of 16 bytes and
 * an ack reservation is always exactly one slot (it never needs padding).
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
 * Caller-owned ring handle. It holds no protocol state of its own: the head and
 * tail counters and the record bytes live in the shared region, so a future
 * attach API could map that region from another process. v0.1 has no attach API
 * and iosvm_ring_init() formats and zeroes the whole region, so a second
 * process can only share the ring if the region is passed to it by other means
 * (and the v0.1 no-cross-VM-ordering caveat in iosvm_gpu.h applies). Treat the
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
    uint32_t pending_off[2]; /* cumulative offset of the reserved record */
    uint32_t pending_len[2];

    /* consumer state, one outstanding peek per body */
    uint32_t peek_len[2];
};

/*
 * Format a ring in caller-provided memory and initialise `ring`.
 *
 * `cmd_size` is the command body size in bytes; the ack body gets the rest of
 * the region. Both sizes must be multiples of IOSVM_RING_ALIGN; the ack body
 * must also be a multiple of IOSVM_ACK_WIRE_SIZE. The whole region is zeroed,
 * so both cumulative counters start at 0.
 * Returns IOSVM_OK, IOSVM_ERR_BAD_ALIGN (region or sizes misaligned) or
 * IOSVM_ERR_BOUNDS (NULL, too small, sizes out of range).
 */
int32_t iosvm_ring_init(struct iosvm_ring *ring, void *mem, size_t mem_size, uint32_t cmd_size);

/*
 * Reserve `len` writable bytes (rounded up to IOSVM_PROTO_ALIGN) and return
 * them, or NULL if the record cannot be placed right now.
 *
 * `len` must be non-zero and `align8(len)` must not exceed the body size. On the
 * command ring it must be at least IOSVM_CMD_HDR_SIZE (commit needs a complete
 * header); on the ack ring `align8(len)` must be exactly IOSVM_ACK_WIRE_SIZE.
 *
 * The reservation is zeroed and may be published at its encoded length: see
 * iosvm_ring_commit(). If the ring had to wrap, the padding bytes in front of
 * it are zeroed and published by the commit as well.
 */
uint8_t *iosvm_ring_reserve(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len);

/*
 * Publish the reservation. `len` must be the same length that was reserved
 * (before alignment), otherwise nothing is published and IOSVM_ERR_BOUNDS is
 * returned.
 *
 * On the command ring the published length is read from the record header, so a
 * reservation that was only partly filled publishes just the bytes the header
 * describes and the unused zero tail stays unpublished. The header is validated
 * against the reservation:
 *   - magic must be IOSVM_PROTO_MAGIC        (else IOSVM_ERR_UNKNOWN_OP: a zero
 *     word would be read as wrap padding by the consumer),
 *   - the payload length must be 8-aligned   (else IOSVM_ERR_BAD_ALIGN),
 *   - 16 + payload length must fit in the reservation (else IOSVM_ERR_BOUNDS).
 * A refused commit publishes nothing and leaves the reservation outstanding;
 * the caller may fix the bytes and commit again. On the ack ring the published
 * length is always one IOSVM_ACK_WIRE_SIZE slot.
 *
 * Returns IOSVM_OK or one of the errors above (IOSVM_ERR_BOUNDS also when there
 * is no reservation or `len` names a different one).
 */
int32_t iosvm_ring_commit(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len);

/*
 * Peek at the next published record.
 *
 * On success *rec points into the ring and *len is the total record length
 * (16 + payload for command records, IOSVM_ACK_WIRE_SIZE for acks). When the
 * ring is empty *rec is NULL and *len is 0 and the return value is IOSVM_OK.
 *
 * If the next position holds wrap padding (a zero word before the end of the
 * body) it is skipped: tail advances to the next multiple of the body size and
 * the peek is retried once. A zero word at offset 0, a second padding marker, or
 * padding longer than the published bytes is corrupt.
 *
 * Returns IOSVM_ERR_TRUNCATED when the published bytes are not a well formed
 * record (corrupt head/tail, more used bytes than the body holds, a record
 * longer than the body, a record that would cross the end of the body, a record
 * longer than `used`) and IOSVM_ERR_BAD_ALIGN when the record length is not a
 * multiple of IOSVM_PROTO_ALIGN.
 */
int32_t iosvm_ring_peek(struct iosvm_ring *ring, enum iosvm_ring_id which, const uint8_t **rec,
                        uint32_t *len);

/*
 * Retire the record returned by the last peek(). `len` must equal that length.
 * Returns IOSVM_OK, IOSVM_ERR_BOUNDS (no peek, or wrong length) or
 * IOSVM_ERR_TRUNCATED (corrupt counters, or fewer published bytes than `len`).
 */
int32_t iosvm_ring_release(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len);

/*
 * Published but unreleased bytes in a body; 0 when empty or on bad arguments.
 * This includes wrap padding until the consumer skips it. It is a byte count,
 * not a position: never compare a head/tail counter with it.
 */
uint32_t iosvm_ring_used(const struct iosvm_ring *ring, enum iosvm_ring_id which);

#ifdef __cplusplus
}
#endif

#endif /* IOSVM_RING_H */
