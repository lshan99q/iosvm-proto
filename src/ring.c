/* SPDX-License-Identifier: MIT */
/*
 * Single-producer/single-consumer ring primitives.
 *
 * The counters live in the shared region, so this file never caches them across
 * calls: every operation re-reads head/tail, applies the protocol invariants and
 * writes only the counter it owns. Record framing lives in iosvm_gpu.h; the ring
 * itself only understands "aligned records of a known length".
 *
 * head and tail are cumulative byte offsets, not indices. They advance modulo
 * ring_period(size), the largest multiple of the body size that fits in
 * uint32_t, so a counter wrap never changes counter % size and the body can
 * always be addressed with `counter % size`. See iosvm_ring.h for the
 * caller-facing contract and the capacity rule.
 */
#include "iosvm_ring.h"

#include <string.h>

#include "iosvm_codec.h"

static int ring_id_valid(enum iosvm_ring_id which)
{
    return which == IOSVM_RING_CMD || which == IOSVM_RING_ACK;
}

static uint8_t *ring_body(struct iosvm_ring *ring, enum iosvm_ring_id which)
{
    return which == IOSVM_RING_CMD ? ring->cmd : ring->ack;
}

static const uint8_t *ring_body_const(const struct iosvm_ring *ring, enum iosvm_ring_id which)
{
    return which == IOSVM_RING_CMD ? ring->cmd : ring->ack;
}

static uint32_t ring_body_size(const struct iosvm_ring *ring, enum iosvm_ring_id which)
{
    return which == IOSVM_RING_CMD ? ring->cmd_size : ring->ack_size;
}

static uint32_t ring_head_off(enum iosvm_ring_id which)
{
    return which == IOSVM_RING_CMD ? IOSVM_RING_HDR_CMD_HEAD_OFF : IOSVM_RING_HDR_ACK_HEAD_OFF;
}

static uint32_t ring_tail_off(enum iosvm_ring_id which)
{
    return which == IOSVM_RING_CMD ? IOSVM_RING_HDR_CMD_TAIL_OFF : IOSVM_RING_HDR_ACK_TAIL_OFF;
}

static uint32_t ring_get(const struct iosvm_ring *ring, uint32_t off)
{
    return iosvm_get_le32(ring->mem + off);
}

static void ring_set(struct iosvm_ring *ring, uint32_t off, uint32_t value)
{
    iosvm_put_le32(ring->mem + off, value);
}

/*
 * Counter modulus: the largest multiple of the body size that fits in uint32_t.
 * It is in (2^31, 2^32]; for a power-of-two body it is exactly 2^32, i.e. plain
 * uint32_t wraparound. Because size divides it, wrapping a counter leaves
 * `counter % size` unchanged, which is what keeps the body indices stable.
 */
static uint64_t ring_period(uint32_t size)
{
    uint64_t span = (uint64_t)size;

    return (0x100000000ull / span) * span;
}

/*
 * Advance a cumulative counter by `delta` modulo `period`. One subtraction is
 * always enough: every caller advances by at most 2*size, and period > 3*2^30
 * while size <= 2^30, so the intermediate stays below 2*period.
 */
static uint32_t ring_add(uint32_t off, uint32_t delta, uint64_t period)
{
    uint64_t next = (uint64_t)off + (uint64_t)delta;

    if (next >= period) {
        next -= period;
    }
    return (uint32_t)next;
}

/* used = (head - tail) modulo period; exact while used <= size < period. */
static uint32_t ring_used_between(uint32_t head, uint32_t tail, uint64_t period)
{
    if (head >= tail) {
        return head - tail;
    }
    return (uint32_t)(period - ((uint64_t)tail - (uint64_t)head));
}

/* Published but unreleased bytes, assuming a formatted and uncorrupted header. */
static uint32_t ring_used_at(const struct iosvm_ring *ring, enum iosvm_ring_id which)
{
    uint32_t size = ring_body_size(ring, which);
    uint64_t period;
    uint32_t head;
    uint32_t tail;
    uint32_t used;

    if (size < IOSVM_RING_MIN_SIZE) {
        return 0u;
    }
    period = ring_period(size);
    head = ring_get(ring, ring_head_off(which));
    tail = ring_get(ring, ring_tail_off(which));
    if ((uint64_t)head >= period || (uint64_t)tail >= period) {
        return 0u;
    }
    used = ring_used_between(head, tail, period);
    return (used > size) ? 0u : used;
}

int32_t iosvm_ring_init(struct iosvm_ring *ring, void *mem, size_t mem_size, uint32_t cmd_size)
{
    uint8_t *base = (uint8_t *)mem;
    size_t ack_size;

    if (ring == NULL || mem == NULL) {
        return IOSVM_ERR_BOUNDS;
    }
    memset(ring, 0, sizeof(*ring));

    if (((uintptr_t)mem % (uintptr_t)IOSVM_RING_ALIGN) != 0u) {
        return IOSVM_ERR_BAD_ALIGN;
    }
    if (mem_size < (size_t)IOSVM_RING_HDR_SIZE + 2u * (size_t)IOSVM_RING_MIN_SIZE) {
        return IOSVM_ERR_BOUNDS;
    }
    if (cmd_size < IOSVM_RING_MIN_SIZE || cmd_size > IOSVM_RING_MAX_SIZE) {
        return IOSVM_ERR_BOUNDS;
    }
    if ((cmd_size % IOSVM_RING_ALIGN) != 0u) {
        return IOSVM_ERR_BAD_ALIGN;
    }
    if ((size_t)cmd_size > mem_size - (size_t)IOSVM_RING_HDR_SIZE) {
        return IOSVM_ERR_BOUNDS;
    }

    ack_size = mem_size - (size_t)IOSVM_RING_HDR_SIZE - (size_t)cmd_size;
    if (ack_size < (size_t)IOSVM_RING_MIN_SIZE || ack_size > (size_t)IOSVM_RING_MAX_SIZE) {
        return IOSVM_ERR_BOUNDS;
    }
    if ((ack_size % (size_t)IOSVM_RING_ALIGN) != 0u) {
        return IOSVM_ERR_BAD_ALIGN;
    }
    /* Ack entries are fixed-size slots, so they must divide the body evenly. */
    if ((ack_size % (size_t)IOSVM_ACK_WIRE_SIZE) != 0u) {
        return IOSVM_ERR_BAD_ALIGN;
    }

    memset(base, 0, mem_size);
    iosvm_put_le32(base + IOSVM_RING_HDR_MAGIC_OFF, IOSVM_PROTO_MAGIC);
    iosvm_put_le32(base + IOSVM_RING_HDR_VERSION_OFF, IOSVM_PROTO_VERSION);
    iosvm_put_le32(base + IOSVM_RING_HDR_CMD_SIZE_OFF, cmd_size);
    iosvm_put_le32(base + IOSVM_RING_HDR_ACK_SIZE_OFF, (uint32_t)ack_size);

    ring->mem = base;
    ring->mem_size = (uint32_t)mem_size;
    ring->cmd = base + IOSVM_RING_HDR_SIZE;
    ring->cmd_size = cmd_size;
    ring->ack = base + IOSVM_RING_HDR_SIZE + cmd_size;
    ring->ack_size = (uint32_t)ack_size;
    return IOSVM_OK;
}

uint8_t *iosvm_ring_reserve(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len)
{
    uint8_t *base;
    uint32_t size;
    uint32_t need;
    uint32_t head;
    uint32_t tail;
    uint32_t used;
    uint32_t index;
    uint32_t gap;
    uint32_t pad;
    uint32_t slot;
    uint64_t period;

    if (ring == NULL || !ring_id_valid(which)) {
        return NULL;
    }
    if (ring->pending[which] != NULL) {
        /* One outstanding reservation per side; commit it first. */
        return NULL;
    }
    if (len == 0u || len > UINT32_MAX - (IOSVM_PROTO_ALIGN - 1u)) {
        return NULL;
    }

    need = iosvm_align8(len);
    base = ring_body(ring, which);
    size = ring_body_size(ring, which);
    if (base == NULL || size < IOSVM_RING_MIN_SIZE || need > size) {
        /* A record that cannot fit in an empty body can never be published. */
        return NULL;
    }
    if (which == IOSVM_RING_ACK) {
        /* Acks are fixed 16-byte slots, so a slot always fits before the end. */
        if (need != IOSVM_ACK_WIRE_SIZE) {
            return NULL;
        }
    } else if (need < IOSVM_CMD_HDR_SIZE) {
        /* commit() has to read a complete command header to learn the length. */
        return NULL;
    }

    period = ring_period(size);
    head = ring_get(ring, ring_head_off(which));
    tail = ring_get(ring, ring_tail_off(which));
    if ((uint64_t)head >= period || (uint64_t)tail >= period) {
        return NULL;
    }
    used = ring_used_between(head, tail, period);
    if (used > size) {
        return NULL;
    }

    /*
     * A record never straddles the end of the body. If the bytes before the end
     * are too few, the producer publishes that gap as zero padding (a zero word
     * cannot start a record: records start with IOSVM_PROTO_MAGIC) and places
     * the record at offset 0 of the next lap.
     */
    index = head % size;
    gap = size - index;
    pad = (gap < need) ? gap : 0u;
    slot = (pad == 0u) ? index : 0u;
    if (pad + need > size - used) {
        return NULL;
    }

    if (pad != 0u) {
        memset(base + index, 0, (size_t)pad);
    }
    /* Do not publish stale bytes: the reservation is handed out zeroed. */
    memset(base + slot, 0, (size_t)need);

    ring->pending[which] = base + slot;
    ring->pending_off[which] = ring_add(head, pad, period);
    ring->pending_len[which] = need;
    return ring->pending[which];
}

int32_t iosvm_ring_commit(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len)
{
    uint8_t *base;
    uint32_t size;
    uint32_t total;
    uint32_t payload;
    uint64_t period;

    if (ring == NULL || !ring_id_valid(which)) {
        return IOSVM_ERR_BOUNDS;
    }
    if (ring->pending[which] == NULL) {
        return IOSVM_ERR_BOUNDS;
    }
    if (len == 0u || len > UINT32_MAX - (IOSVM_PROTO_ALIGN - 1u)) {
        return IOSVM_ERR_BOUNDS;
    }
    if (iosvm_align8(len) != ring->pending_len[which]) {
        return IOSVM_ERR_BOUNDS;
    }

    base = ring_body(ring, which);
    size = ring_body_size(ring, which);
    if (base == NULL || size < IOSVM_RING_MIN_SIZE) {
        return IOSVM_ERR_BOUNDS;
    }

    if (which == IOSVM_RING_ACK) {
        /* Ack slots are fixed size and have no length field to trust. */
        total = ring->pending_len[which];
    } else {
        /*
         * Publish the length the record header encodes, not the reservation:
         * a caller that reserved more than it wrote publishes exactly the
         * record, so the consumer cannot walk into the unused zero tail and
         * mistake it for wrap padding.
         */
        if (ring->pending_len[which] < IOSVM_CMD_HDR_SIZE) {
            return IOSVM_ERR_BOUNDS;
        }
        if (iosvm_get_le32(ring->pending[which] + IOSVM_CMD_HDR_MAGIC_OFF) != IOSVM_PROTO_MAGIC) {
            /* A zero word would be read as padding; nothing is published. */
            return IOSVM_ERR_UNKNOWN_OP;
        }
        payload = iosvm_get_le32(ring->pending[which] + IOSVM_CMD_HDR_LEN_OFF);
        if ((payload % IOSVM_PROTO_ALIGN) != 0u) {
            return IOSVM_ERR_BAD_ALIGN;
        }
        if (payload > ring->pending_len[which] - IOSVM_CMD_HDR_SIZE) {
            return IOSVM_ERR_BOUNDS;
        }
        total = IOSVM_CMD_HDR_SIZE + payload;
    }

    period = ring_period(size);
    iosvm_barrier();
    ring_set(ring, ring_head_off(which), ring_add(ring->pending_off[which], total, period));
    ring->pending[which] = NULL;
    ring->pending_off[which] = 0u;
    ring->pending_len[which] = 0u;
    return IOSVM_OK;
}

int32_t iosvm_ring_peek(struct iosvm_ring *ring, enum iosvm_ring_id which, const uint8_t **rec,
                        uint32_t *len)
{
    const uint8_t *base;
    uint32_t size;
    uint64_t period;
    int skipped_pad = 0;

    if (ring == NULL || !ring_id_valid(which) || rec == NULL || len == NULL) {
        return IOSVM_ERR_BOUNDS;
    }
    *rec = NULL;
    *len = 0u;
    ring->peek_len[which] = 0u;

    base = ring_body_const(ring, which);
    size = ring_body_size(ring, which);
    if (base == NULL || size < IOSVM_RING_MIN_SIZE) {
        return IOSVM_ERR_TRUNCATED;
    }
    period = ring_period(size);

    for (;;) {
        uint32_t head = ring_get(ring, ring_head_off(which));
        uint32_t tail = ring_get(ring, ring_tail_off(which));
        uint32_t used;
        uint32_t index;
        uint32_t avail;
        uint32_t payload;
        uint32_t total;

        if ((uint64_t)head >= period || (uint64_t)tail >= period) {
            return IOSVM_ERR_TRUNCATED;
        }
        iosvm_barrier();

        used = ring_used_between(head, tail, period);
        if (used > size) {
            /* More published bytes than the body can hold: corrupt counters. */
            return IOSVM_ERR_TRUNCATED;
        }
        if (used == 0u) {
            return IOSVM_OK;
        }
        index = tail % size;
        avail = size - index;

        if (which == IOSVM_RING_ACK) {
            if (avail < IOSVM_ACK_WIRE_SIZE || used < IOSVM_ACK_WIRE_SIZE) {
                return IOSVM_ERR_TRUNCATED;
            }
            *rec = base + index;
            *len = IOSVM_ACK_WIRE_SIZE;
            ring->peek_len[which] = IOSVM_ACK_WIRE_SIZE;
            return IOSVM_OK;
        }

        /*
         * The producer zeroed the rest of the body to mark a wrap. A zero word
         * can only be padding and only before the end of the body; at index 0 it
         * would be a corrupt record. At most one skip is allowed per peek.
         */
        if (avail < 4u || used < 4u) {
            return IOSVM_ERR_TRUNCATED;
        }
        if (iosvm_get_le32(base + index) == 0u) {
            if (index == 0u || skipped_pad || used < avail) {
                return IOSVM_ERR_TRUNCATED;
            }
            ring_set(ring, ring_tail_off(which), ring_add(tail, avail, period));
            skipped_pad = 1;
            continue;
        }

        /* Command records: a record never straddles the end of the body. */
        if (avail < IOSVM_CMD_HDR_SIZE || used < IOSVM_CMD_HDR_SIZE) {
            return IOSVM_ERR_TRUNCATED;
        }
        payload = iosvm_get_le32(base + index + IOSVM_CMD_HDR_LEN_OFF);
        if ((payload % IOSVM_PROTO_ALIGN) != 0u) {
            return IOSVM_ERR_BAD_ALIGN;
        }
        if (payload > size - IOSVM_CMD_HDR_SIZE) {
            return IOSVM_ERR_TRUNCATED;
        }
        total = IOSVM_CMD_HDR_SIZE + payload;
        if (total > avail || total > used) {
            return IOSVM_ERR_TRUNCATED;
        }

        *rec = base + index;
        *len = total;
        ring->peek_len[which] = total;
        return IOSVM_OK;
    }
}

int32_t iosvm_ring_release(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len)
{
    uint32_t size;
    uint32_t head;
    uint32_t tail;
    uint32_t used;
    uint64_t period;

    if (ring == NULL || !ring_id_valid(which)) {
        return IOSVM_ERR_BOUNDS;
    }
    if (ring->peek_len[which] == 0u || len != ring->peek_len[which]) {
        return IOSVM_ERR_BOUNDS;
    }

    size = ring_body_size(ring, which);
    if (size < IOSVM_RING_MIN_SIZE) {
        return IOSVM_ERR_TRUNCATED;
    }
    period = ring_period(size);
    head = ring_get(ring, ring_head_off(which));
    tail = ring_get(ring, ring_tail_off(which));
    if ((uint64_t)head >= period || (uint64_t)tail >= period) {
        return IOSVM_ERR_TRUNCATED;
    }
    used = ring_used_between(head, tail, period);
    if (used < len) {
        return IOSVM_ERR_TRUNCATED;
    }

    iosvm_barrier();
    ring_set(ring, ring_tail_off(which), ring_add(tail, len, period));
    ring->peek_len[which] = 0u;
    return IOSVM_OK;
}

uint32_t iosvm_ring_used(const struct iosvm_ring *ring, enum iosvm_ring_id which)
{
    if (ring == NULL || !ring_id_valid(which)) {
        return 0u;
    }
    return ring_used_at(ring, which);
}
