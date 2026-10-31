/* SPDX-License-Identifier: MIT */
/*
 * Randomised ring test.
 *
 * The table-driven cases in test_ring.c check the invariants one at a time. This one does not know
 * what could go wrong: it publishes fixed-size ack slots in a pseudo-random pattern against an
 * in-test model and asserts that the consumer sees exactly the bytes the producer claimed, in
 * order, with no overlap and no gap. It caught nothing so far — which is the point of keeping it:
 * if a future change to the wrap or the tail-gap padding is wrong, a table-driven case may still
 * pass while this will not.
 *
 * The generator is a fixed LCG instead of rand(): the run must be identical on every machine and
 * on every libc, or a failure is not reproducible.
 */
#include "tests.h"

static uint32_t fuzz_state;

static uint32_t fuzz_next(uint32_t n)
{
    fuzz_state = fuzz_state * 1664525u + 1013904223u;
    return (fuzz_state >> 8) % n;
}

/*
 * One trial: a ring whose sizes are legal but deliberately awkward (not powers of two), then a
 * random interleaving of producer and consumer steps for a fixed count.
 */
static void fuzz_trial(uint32_t seed, uint32_t cmd_size, uint32_t ack_size, uint32_t steps)
{
    enum { N = 256 };
    static uint8_t model[N][IOSVM_ACK_WIRE_SIZE];
    size_t total = (size_t)IOSVM_RING_HDR_SIZE + cmd_size + ack_size;
    /* C11 requires aligned_alloc's size to be a multiple of the alignment; the ring size need not
     * be, so allocate rounded up and tell init the real size. */
    size_t alloc = (total + 63u) & ~(size_t)63u;
    uint8_t *mem = (uint8_t *)aligned_alloc(64, alloc);
    struct iosvm_ring ring;
    uint32_t head = 0, tail = 0;
    uint8_t salt = 0;
    uint32_t step;

    CHECK(mem != NULL);
    memset(mem, 0, alloc);
    CHECK_EQ(iosvm_ring_init(&ring, mem, total, cmd_size), IOSVM_OK);

    fuzz_state = seed;
    for (step = 0; step < steps; step++) {
        int produce = (fuzz_next(2) == 0) || (head - tail) > 4;

        if (produce && head < N) {
            uint8_t *slot = iosvm_ring_reserve(&ring, IOSVM_RING_ACK, IOSVM_ACK_WIRE_SIZE);
            uint32_t i;

            if (slot != NULL) {
                for (i = 0; i < IOSVM_ACK_WIRE_SIZE; i++) {
                    slot[i] = (uint8_t)(salt + i);
                }
                CHECK_EQ(iosvm_ring_commit(&ring, IOSVM_RING_ACK, IOSVM_ACK_WIRE_SIZE), IOSVM_OK);
                memcpy(model[head], slot, IOSVM_ACK_WIRE_SIZE);
                head++;
                salt++;
            }
        } else if (tail < head) {
            const uint8_t *rec = NULL;
            uint32_t len = 0;

            CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_ACK, &rec, &len), IOSVM_OK);
            CHECK(rec != NULL);
            CHECK_EQ(len, IOSVM_ACK_WIRE_SIZE);
            CHECK(iosvm_test_mem_eq(__FILE__, __LINE__, "acked bytes", rec, model[tail], len));
            CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_ACK, len), IOSVM_OK);
            tail++;
        }
    }

    /* Drain the rest: a record left unread is not an error, but a record that cannot be read is. */
    while (tail < head) {
        const uint8_t *rec = NULL;
        uint32_t len = 0;

        CHECK_EQ(iosvm_ring_peek(&ring, IOSVM_RING_ACK, &rec, &len), IOSVM_OK);
        CHECK(iosvm_test_mem_eq(__FILE__, __LINE__, "drained bytes", rec, model[tail], len));
        CHECK_EQ(iosvm_ring_release(&ring, IOSVM_RING_ACK, len), IOSVM_OK);
        tail++;
    }
    CHECK_EQ(iosvm_ring_used(&ring, IOSVM_RING_ACK), 0u);
    free(mem);
}

static void case_ring_fuzz_ack_wraparound(void)
{
    /* Sizes that are legal but not round: 8-aligned command body, 16-multiple ack body. */
    fuzz_trial(1u, 264u, 320u, 3000u);
    fuzz_trial(2u, 1000u, 2064u, 3000u);
    fuzz_trial(7u, 3592u, 1616u, 3000u);
    fuzz_trial(42u, 56u * IOSVM_RING_ALIGN, 32u * IOSVM_ACK_WIRE_SIZE, 3000u);
    fuzz_trial(99u, 256u, 256u, 3000u);
}

/*
 * The caller owns the geometry, and init must refuse a region that cannot be split into an aligned
 * command body and an ack body that is a whole number of slots. Passing an arbitrary buffer size
 * and expecting init to cope is the mistake this case pins down.
 */
static void case_ring_init_rejects_bad_geometry(void)
{
    uint8_t *mem = (uint8_t *)aligned_alloc(64, 8192);
    struct iosvm_ring ring;

    CHECK(mem != NULL);
    memset(mem, 0, 8192);

    /* 8192 = 32 header + 4096 command + 4064 ack; 4064 is a multiple of 16, so this one is fine. */
    CHECK_EQ(iosvm_ring_init(&ring, mem, 8192, 4096), IOSVM_OK);

    /* 8192 - 32 - 4096 = 4064; shift the command body by 8 to leave a 4056-byte ack body. */
    CHECK_EQ(iosvm_ring_init(&ring, mem, 8192, 4104), IOSVM_ERR_BAD_ALIGN);

    /* A command body that is not a multiple of the ring alignment. */
    CHECK_EQ(iosvm_ring_init(&ring, mem, 8192, 4097), IOSVM_ERR_BAD_ALIGN);

    /* Too small to hold a minimum body either side. */
    CHECK_EQ(iosvm_ring_init(&ring, mem, 64, 256), IOSVM_ERR_BOUNDS);

    /* A region that is not aligned at all. */
    CHECK_EQ(iosvm_ring_init(&ring, mem + 1, 8192 - 1, 4096), IOSVM_ERR_BAD_ALIGN);

    free(mem);
}

void iosvm_test_ring_fuzz(void)
{
    iosvm_run_case("ring fuzz: 5 seeds x 3000 ops, ack slots never overlap or corrupt",
                   case_ring_fuzz_ack_wraparound);
    iosvm_run_case("ring init: refuses a geometry the bodies cannot be split into",
                   case_ring_init_rejects_bad_geometry);
}
