<!-- SPDX-License-Identifier: MIT -->
# IOSVM — the GPU wire protocol (v0.1)

IOSVM boots iOS in QEMU on Linux and translates Metal to Vulkan inside the
guest's user space. This repository is the part that carries the work across the
VM boundary: the guest translator writes command records into a shared-memory
ring, a virtio device picks them up, and a host backend replays them as real
Vulkan calls.

It is deliberately standalone: C11, libc only, buildable with `cc` and `make`,
and testable with no GPU and no Apple file. There is no Apple content here at all
— only the protocol, its codec, a strict in-process interpreter for tests, and
the tests.

## What is in here

| Path | What it is |
| --- | --- |
| `include/iosvm_gpu.h` | The protocol: magic, version, ring header, command envelope, ten opcodes and their payloads, acks, error codes, and the invariants as a comment block at the top. |
| `include/iosvm_ring.h`, `src/ring.c` | Single-producer/single-consumer ring primitives: `iosvm_ring_init`, `reserve`, `commit`, `peek`, `release`, `used`. |
| `include/iosvm_codec.h`, `src/codec.c` | Little-endian wire accessors, one encoder per opcode, `iosvm_decode_one` with full validation, ack codec, `iosvm_strerror`. |
| `include/iosvm_fake_gpu.h`, `src/fake_gpu.c` | Strict in-process interpreter: buffers and RGBA8 images in a simulated heap, byte-exact `WRITE`/`BLIT_COPY`/`BLIT_FILL`/`SCREENSHOT`, ordering and sequence checks, readback. |
| `tests/` | 56 cases, no external framework, `--verbose` mode. |
| `docs/protocol.md` | Byte layout diagrams and the versioning rule. |

## Build and test

```sh
make            # build/libiosvm_proto.a
make test       # build and run the test suite
make test-asan  # same, with -fsanitize=address,undefined
make clean
make help
```

`CC`, `CFLAGS` and `AR` are honoured. The default flags are
`-std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -O2 -g`.

## Invariants

These are the rules the code exists to enforce; the full text is at the top of
`include/iosvm_gpu.h`.

| # | Invariant | Where |
| --- | --- | --- |
| 1 | `cmd_head` is written only by the guest, `cmd_tail` only by the host; `ack_head` only by the host, `ack_tail` only by the guest. Neither side writes the other's counters or records. | `src/ring.c` |
| 2 | Counters are cumulative byte offsets, not indices: they advance modulo `P = floor(2^32 / size) * size` and the body is indexed with `counter % size`, which a wrap never changes because `size` divides `P`. | `src/ring.c` |
| 3 | `used = (head - tail) mod P`, exact while `used <= size`. `head == tail` always means empty and `used == size` means full. Capacity is not a constant: a record of `need` bytes is accepted when `used + padding + need <= size`, so a body can hold a full `size` bytes when the records line up with it. | `iosvm_ring_reserve` |
| 4 | A record never straddles the end of the body: the producer zeroes the tail gap, advances `head` to the next multiple of `size` and restarts at offset 0; the consumer sees the zero word and skips the same gap. A zero word is unambiguous padding because every real record starts with `IOSVM_PROTO_MAGIC`. | `reserve` / `peek` |
| 5 | Ring bodies, records, payloads and ring counters are 8-byte aligned; every payload length is a multiple of 8. | `iosvm_gpu.h` (`_Static_assert`), `src/ring.c` |
| 6 | Every multi-byte field is little-endian on the wire and converted explicitly. No struct is memcpy'd to or from shared memory. | `src/codec.c` |
| 7 | Unknown opcodes are rejected with `IOSVM_ERR_UNKNOWN_OP`, never skipped, and still get an ack: every command sequence number produces exactly one ack, in order. | `iosvm_decode_one`, host loop in `tests/test_integration.c` |
| 8 | A command's `seq` must be strictly greater than the previous one; 0 is reserved. | `iosvm_fake_gpu_exec` |
| 9 | A peer must match the major version and must not be newer in the minor, otherwise `IOSVM_ERR_VERSION`. | `iosvm_version_compatible` |
| 10 | Every length in the input is treated as hostile: it is checked against the remaining buffer before use, and nothing past the buffer is ever read. | `iosvm_decode_one` |

## What is implemented (v0.1)

* The protocol header with compile-time layout checks for every wire struct.
* Ring primitives for both directions with cumulative counters, including
  wrap-around, the zero-word padding marker and the
  `used + padding + len <= size` capacity rule. `commit` publishes the encoded
  record length, so an under-filled reservation cannot republish or replay a
  record, and an emptied ring is never wedged by its counter position.
* Encode/decode for all ten opcodes plus acks, with the full rejection set:
  truncated, bad magic, version mismatch, unknown op, misaligned length, wrong
  payload length.
* A strict fake GPU that refuses out-of-bounds ranges, unknown/reused handles,
  commands out of order, reserved fields that are not zero and sequence numbers
  that do not advance — and changes nothing when it refuses.
* 56 test cases, including a 512 KiB blit round trip through the ring and the
  interpreter with byte-identical readback, 1000 messages through a 256-byte
  ring, 40 buffers applied in order, and a 1M-step differential fuzz over five
  body sizes (three of them not powers of two).
* Clean under `-Wall -Wextra -Werror -Wshadow -Wconversion`,
  AddressSanitizer and UndefinedBehaviorSanitizer.

Verified on x86-64 Ubuntu 24.04 with GCC 13.3.0 and clang-format 18.1.3. The
commands that were actually run are the four in "Build and test";
`clang-format --dry-run --Werror include/*.h src/*.c tests/*.c` is clean.

## What is not implemented (and why)

Nothing in this repository talks to QEMU, Metal or Vulkan: there is no
counterpart on either end yet. Concretely, v0.1 has:

* **No virtio device and no host backend.** Nobody outside the tests consumes a
  record. The device, the Metal front end and the Vulkan replay layer do not
  exist in this repository.
* **No zero-copy.** `WRITE` copies its bytes inline (at most
  `IOSVM_MAX_INLINE_WRITE` = 64 KiB per record); larger transfers are meant to be
  done buffer-to-buffer and the fake GPU never maps guest memory. There is no
  export/import of hosted buffers and no shared mapping.
* **No multi-queue.** One command ring and one ack ring, one producer and one
  consumer per ring. There is no queue family, no per-queue submission, no
  ring-to-ring chaining.
* **No fenced timelines.** `FENCE_WAIT` carries a fence id and a timeout, and the
  fake GPU completes everything synchronously when `exec` returns. There are no
  Vulkan fences, semaphores or timelines, and no completion interrupts.
* **No cross-VM memory ordering.** Ring publication uses a compiler barrier
  (`iosvm_barrier`) only. It is correct for a single thread and for the
  in-process pipeline the tests drive; real barriers belong to the virtio device
  that does not exist yet. The counters are written byte-wise as little-endian
  values, so a reader on another CPU is *not* guaranteed to observe a whole
  counter — v0.1 is single-threaded/single-process.
* **No image layout work.** `IOSVM_FORMAT_RGBA8` is linear, tightly packed rows.
  No tiling, swizzling, strides, mip levels, array layers or formats other than
  RGBA8.
* **No real display path.** `PRESENT` only bumps a counter in the fake GPU;
  `SCREENSHOT` copies a region into a buffer.
* **No compression, no validation layers, no parse-level fuzzing, and no
  big-endian or 32-bit host was ever tested.** The code is endian-clean by
  construction but only ever ran on x86-64.
* **No synchronization on `reserve` abort.** A reservation that is never
  committed stalls that side of the ring by design; there is no abort primitive.

## Requirements and content

C11 compiler, `make`, and (for formatting checks) `clang-format`. No GPU, no
Apple software, no firmware, no network access. This repository contains no
Apple content whatsoever and needs no GPU to build or to test.

## License

MIT. See `LICENSE`.
