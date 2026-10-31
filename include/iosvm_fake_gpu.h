/* SPDX-License-Identifier: MIT */
/*
 * Strict in-process interpreter for the IOSVM GPU protocol.
 *
 * This is a test double, not an emulator: it keeps a fixed-size simulated heap
 * in which buffers and RGBA8 images live as flat byte arrays, applies WRITE,
 * BLIT_COPY, BLIT_FILL and SCREENSHOT literally, and validates everything it is
 * given. It exists so that this repository can be tested with no GPU and no
 * Apple content at all.
 *
 * Strictness is the point. The interpreter refuses ranges outside the resource,
 * unknown or reused handles, commands that make no sense in the current state
 * (a SUBMIT with nothing queued, a second SUBMIT of the same batch, a
 * SCREENSHOT before any submit), and any sequence number that does not advance.
 * Every refusal returns one of the IOSVM_ERR_* codes and leaves the simulated
 * state untouched.
 *
 * What it does not do: no real GPU work, no tiling or swizzling (images are
 * linear RGBA8 rows), no asynchronous execution, no real fences (everything is
 * complete by the time exec() returns), no multi-queue, no present path.
 */
#ifndef IOSVM_FAKE_GPU_H
#define IOSVM_FAKE_GPU_H

#include <stdint.h>

#include "iosvm_gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Simulated heap size used when iosvm_fake_gpu_new() gets 0. */
#define IOSVM_FAKE_DEFAULT_MEMORY (8u * 1024u * 1024u)

/* Largest heap the interpreter accepts. */
#define IOSVM_FAKE_MAX_MEMORY (1024u * 1024u * 1024u)

struct iosvm_fake_gpu;

/* Simulated counters, for tests that want to see what the interpreter did. */
struct iosvm_fake_stats {
    uint32_t submits;     /* successful SUBMIT commands */
    uint32_t presents;    /* successful PRESENT commands */
    uint32_t screenshots; /* successful SCREENSHOT commands */
    uint32_t pending;     /* WRITE/BLIT_* commands queued since the last submit */
    uint32_t resources;   /* live buffers and images */
    uint32_t fence_id;    /* highest fence id handed out (== submits) */
};

/*
 * Create an interpreter with `memory_bytes` of simulated memory (0 selects
 * IOSVM_FAKE_DEFAULT_MEMORY). Returns NULL on allocation failure or when the
 * requested size is 0-adjacent nonsense (above IOSVM_FAKE_MAX_MEMORY).
 * The heap starts zeroed.
 */
struct iosvm_fake_gpu *iosvm_fake_gpu_new(uint32_t memory_bytes);

/* Release the interpreter and its heap. NULL is accepted. */
void iosvm_fake_gpu_free(struct iosvm_fake_gpu *gpu);

/*
 * Execute one decoded command. Returns IOSVM_OK or a negative IOSVM_ERR_* to be
 * sent back in the ack; `handle` receives the handle the command produced or
 * addressed (0 when there is none). A command that returns an error must still
 * be acked by the caller: its sequence number has been consumed.
 */
int32_t iosvm_fake_gpu_exec(struct iosvm_fake_gpu *gpu, const struct iosvm_cmd *cmd,
                            uint32_t *handle);

/*
 * Copy `len` bytes at `offset` of the resource named by `handle` into `dst`.
 * Images are read as linear RGBA8 rows. Returns IOSVM_OK, IOSVM_ERR_BAD_HANDLE
 * or IOSVM_ERR_BOUNDS.
 */
int32_t iosvm_fake_gpu_readback(const struct iosvm_fake_gpu *gpu, uint32_t handle, uint32_t offset,
                                void *dst, uint32_t len);

/* Size in bytes of a resource, 0 when the handle is unknown. */
uint32_t iosvm_fake_gpu_size(const struct iosvm_fake_gpu *gpu, uint32_t handle);

/* Bytes currently allocated out of the simulated heap. */
uint32_t iosvm_fake_gpu_memory_used(const struct iosvm_fake_gpu *gpu);

/* Snapshot the counters. */
void iosvm_fake_gpu_stats(const struct iosvm_fake_gpu *gpu, struct iosvm_fake_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* IOSVM_FAKE_GPU_H */
