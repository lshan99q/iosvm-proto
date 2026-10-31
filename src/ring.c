/* SPDX-License-Identifier: MIT */
/*
 * Single-producer/single-consumer ring primitives.
 *
 * The counters live in the shared region, so this file never caches them across
 * calls: every operation re-reads head/tail, applies the protocol invariants and
 * writes only the counter it owns. Record framing lives in iosvm_gpu.h; the ring
 * itself only understands "aligned records of a known length".
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

/* Published but unreleased bytes, assuming a formatted and uncorrupted header. */
static uint32_t ring_used_at(const struct iosvm_ring *ring, enum iosvm_ring_id which)
{
    uint32_t size = ring_body_size(ring, which);
    uint32_t head;
    uint32_t tail;

    if (size < IOSVM_RING_MIN_SIZE) {
        return 0u;
    }

    head = ring_get(ring, ring_head_off(which));
    tail = ring_get(ring, ring_tail_off(which));
    if (head >= size || tail >= size) {
        return 0u;
    }

    return (head >= tail) ? (head - tail) : (size - (tail - head));
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
    uint32_t free_bytes;
    uint32_t off;

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
    if (base == NULL || size < IOSVM_RING_MIN_SIZE || need >= size) {
        return NULL;
    }
    if (which == IOSVM_RING_ACK && (need % IOSVM_ACK_WIRE_SIZE) != 0u) {
        return NULL;
    }

    head = ring_get(ring, ring_head_off(which));
    tail = ring_get(ring, ring_tail_off(which));
    if (head >= size || tail >= size) {
        return NULL;
    }

    used = (head >= tail) ? (head - tail) : (size - (tail - head));
    free_bytes = (size - 1u) - used;
    if (need > free_bytes) {
        return NULL;
    }

    off = head;
    if ((size - off) < need) {
        /*
         * No room before the end of the body. The tail gap becomes zero padding
         * that the consumer skips, and the record starts at offset 0. Only the
         * bytes in front of the consumer tail are free after the wrap, so the
         * record must end before it.
         */
        if (tail <= 1u || need > (tail - 1u)) {
            return NULL;
        }
        memset(base + off, 0, (size_t)(size - off));
        off = 0u;
    }

    /* Do not publish stale bytes: the reservation is handed out zeroed. */
    memset(base + off, 0, (size_t)need);

    ring->pending[which] = base + off;
    ring->pending_len[which] = need;
    return ring->pending[which];
}

int32_t iosvm_ring_commit(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len)
{
    uint8_t *base;
    uint32_t size;
    uint32_t head;

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

    head = (uint32_t)(ring->pending[which] - base) + ring->pending_len[which];
    if (head >= size) {
        head -= size;
    }

    iosvm_barrier();
    ring_set(ring, ring_head_off(which), head);
    ring->pending[which] = NULL;
    ring->pending_len[which] = 0u;
    return IOSVM_OK;
}

int32_t iosvm_ring_peek(struct iosvm_ring *ring, enum iosvm_ring_id which, const uint8_t **rec,
                        uint32_t *len)
{
    const uint8_t *base;
    uint32_t size;
    uint32_t head;
    uint32_t tail;
    uint32_t used;
    uint32_t avail;
    uint32_t payload_len;
    uint32_t total;

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

    for (;;) {
        head = ring_get(ring, ring_head_off(which));
        tail = ring_get(ring, ring_tail_off(which));
        if (head >= size || tail >= size) {
            return IOSVM_ERR_TRUNCATED;
        }
        iosvm_barrier();

        used = (head >= tail) ? (head - tail) : (size - (tail - head));
        if (used == 0u) {
            return IOSVM_OK;
        }
        avail = (tail < head) ? (head - tail) : (size - tail);

        if (which == IOSVM_RING_ACK) {
            if (avail < IOSVM_ACK_WIRE_SIZE || used < IOSVM_ACK_WIRE_SIZE) {
                return IOSVM_ERR_TRUNCATED;
            }
            *rec = base + tail;
            *len = IOSVM_ACK_WIRE_SIZE;
            ring->peek_len[which] = IOSVM_ACK_WIRE_SIZE;
            return IOSVM_OK;
        }

        /* Command records: a record never straddles the end of the body. */
        if (avail < IOSVM_CMD_HDR_SIZE) {
            if (tail == 0u) {
                return IOSVM_ERR_TRUNCATED;
            }
            ring_set(ring, ring_tail_off(which), 0u); /* skip zero padding */
            continue;
        }
        if (iosvm_get_le32(base + tail) == 0u) {
            if (tail == 0u) {
                return IOSVM_ERR_TRUNCATED;
            }
            ring_set(ring, ring_tail_off(which), 0u); /* skip zero padding */
            continue;
        }

        payload_len = iosvm_get_le32(base + tail + IOSVM_CMD_HDR_LEN_OFF);
        if ((payload_len % IOSVM_PROTO_ALIGN) != 0u) {
            return IOSVM_ERR_BAD_ALIGN;
        }
        if (payload_len > size - IOSVM_CMD_HDR_SIZE) {
            return IOSVM_ERR_TRUNCATED;
        }
        total = IOSVM_CMD_HDR_SIZE + payload_len;
        if (total > avail || total > used) {
            return IOSVM_ERR_TRUNCATED;
        }

        *rec = base + tail;
        *len = total;
        ring->peek_len[which] = total;
        return IOSVM_OK;
    }
}

int32_t iosvm_ring_release(struct iosvm_ring *ring, enum iosvm_ring_id which, uint32_t len)
{
    uint32_t size;
    uint32_t tail;

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

    tail = ring_get(ring, ring_tail_off(which));
    if (tail >= size) {
        return IOSVM_ERR_TRUNCATED;
    }
    tail += len;
    if (tail >= size) {
        tail -= size;
    }

    iosvm_barrier();
    ring_set(ring, ring_tail_off(which), tail);
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
