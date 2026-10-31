<!-- SPDX-License-Identifier: MIT -->
# IOSVM GPU wire protocol, v0.1

This document is the byte-level companion to `include/iosvm_gpu.h`. The header is
normative; this file explains the shape of the bytes and the rules that make two
independent implementations agree.

## 1. The shared region

One region of shared memory, 8-byte aligned:

```text
byte 0                                                            32
+----------+----------+----------+----------+----------+----------+----------+----------+
|  magic   | version  | cmd_head | cmd_tail | ack_head | ack_tail | cmd_size | ack_size |
|  u32 LE  |  u32 LE  |  u32 LE  |  u32 LE  |  u32 LE  |  u32 LE  |  u32 LE  |  u32 LE  |
+----------+----------+----------+----------+----------+----------+----------+----------+
     0          4          8         12         16         20         24         28

byte 32                                                      32 + cmd_size
+------------------------------------------------------------+----------------------------+
|                     command body (cmd_size)                |      ack body (ack_size)   |
+------------------------------------------------------------+----------------------------+
```

Constraints: the body sizes are multiples of 8, the ack body is additionally a
multiple of 16 (acks are fixed 16-byte slots), and each body is at least 256
bytes.

`cmd_head`/`cmd_tail`/`ack_head`/`ack_tail` are **cumulative byte offsets**, not
indices: a counter counts every byte that side has produced or consumed. They
advance modulo `P = floor(2^32 / size) * size`, the largest multiple of the body
size that fits in 32 bits (for a power-of-two body `P = 2^32`, i.e. plain
32-bit wraparound). A body offset is `counter % size`; because `size` divides
`P`, a counter wrap does not change it. Never compare a counter against `size`:
only differences between the two counters are meaningful, and only modulo `P`.

Writers:

| Field | Written by | Read by |
| --- | --- | --- |
| `magic`, `version`, `cmd_size`, `ack_size` | the formatter, once | both |
| `cmd_head` | guest (producer) | host (consumer) |
| `cmd_tail` | host (consumer) | guest |
| `ack_head` | host (ack producer) | guest (ack consumer) |
| `ack_tail` | guest (ack consumer) | host |

## 2. Command record

```text
byte 0                    16                                  16 + len
+------------------------+-----------------------------------+----------+
|    header (16 bytes)   |      payload (len bytes)          | padding  |
+------------------------+-----------------------------------+----------+
```

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | `IOSVM_PROTO_MAGIC`; its little-endian bytes are `'I' 'O' 'S' 'V'` |
| 4 | 2 | `version` | `IOSVM_PROTO_VERSION` = `0x0001` (major high byte, minor low byte) |
| 6 | 2 | `op` | one of the ten opcodes, `0` and `≥ IOSVM_OP_MAX` are invalid |
| 8 | 4 | `seq` | guest sequence number, starts at 1, strictly increasing |
| 12 | 4 | `len` | payload bytes that follow; a multiple of 8 |
| 16 | len | payload | see below |

Record size is always `16 + len`, which is a multiple of 8. A 0-byte payload is
never legal: every opcode has at least 8 payload bytes.

### Opcodes and payload sizes

| Op | Name | Payload | Fields (u32 LE each unless noted) |
| --- | --- | --- | --- |
| 1 | `CREATE_BUFFER` | 16 | handle, size, usage (`0`), flags (`0`) |
| 2 | `CREATE_IMAGE` | 24 | handle, width, height, format, usage (`0`), flags (`0`) |
| 3 | `DESTROY` | 8 | handle, reserved (`0`) |
| 4 | `WRITE` | 16 + align8(n) | handle, offset, len=n, reserved (`0`), then n inline bytes and 0..7 zero pad |
| 5 | `BLIT_COPY` | 24 | dst, dst_offset, src, src_offset, len, reserved (`0`) |
| 6 | `BLIT_FILL` | 16 | dst, dst_offset, len (multiple of 4), value (32-bit pattern) |
| 7 | `SUBMIT` | 8 | cmd_count, flags (`0`) |
| 8 | `FENCE_WAIT` | 8 | fence_id, timeout_ms |
| 9 | `PRESENT` | 8 | image, reserved (`0`) |
| 10 | `SCREENSHOT` | 24 | image, dst, x, y, width, height |

`format` is `1` (`RGBA8`) and nothing else in v0.1. Images are linear, tightly
packed rows of `w * 4` bytes. Everywhere a handle or offset appears it is a
32-bit unsigned little-endian value; `reserved` words must be zero and a
non-zero value is refused.

### Worked example

`WRITE` handle 1, offset 0, three bytes `"Hi!"` (`48 69 21`), so `len = 16 + 8 = 24`:

```text
49 4f 53 56  01 00  04 00  01 00 00 00  18 00 00 00   <- header
                                  magic  ver  op   seq 1        len 24
01 00 00 00  00 00 00 00  03 00 00 00  00 00 00 00   <- handle 1, offset 0, n=3, reserved
48 69 21 00 00 00 00 00                             <- data + zero padding
```

The whole record is 40 bytes and starts on an 8-byte boundary.

## 3. Ack record

Every consumed command produces exactly one 16-byte ack, in command order:

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `seq` | sequence number being acked |
| 4 | 4 | `status` | `0` or a negative `IOSVM_ERR_*` (two's complement) |
| 8 | 4 | `handle` | handle produced or addressed; `SUBMIT` puts the fence id here; `0` if none |
| 12 | 4 | `reserved` | `0` |

Ack slots are fixed size, so the ack body must divide evenly by 16 and acks never
need wrap padding.

## 4. Flow control and wrapping

The consumer owns `tail`; the producer owns `head`. Both are cumulative, so:

```text
used  = (head - tail) mod P     empty when used == 0 (head == tail)
                                full  when used == size
```

Both states are representable; there is no unconditional `size - 1` reserve.
Capacity depends on where the counters are and on the padding the next record
needs, because a single record is never split across the end of a body. With
`need = align8(record length)` and `gap = size - head % size`, a record is
accepted exactly when `used + pad + need <= size`, where `pad` is `gap` when
`need > gap` and `0` otherwise. A body can therefore hold a full `size` bytes
when the records line up with it; the only permanent refusal is `need > size`.

When the remaining tail gap is smaller than the next record:

```text
 +-------------+---------------------+
 |  records    |  zero padding       |   producer zeroes the gap and starts at 0
 +-------------+---------------------+
 ^tail         ^head                 ^size
                  -> head advances to the next multiple of size; the record starts at offset 0
```

The consumer recognises padding because a real record always begins with
`IOSVM_PROTO_MAGIC`; a zero word at the consumer tail means "skip to the next
multiple of the body size". The producer only wraps when the padding and the
record fit in front of the consumer tail, so padding can never overwrite unread
data, and the consumer never reads unpadded stale bytes. `head` advances by
exactly the record's encoded length (`16 + payload`) plus any padding.

Because the counters are cumulative, an empty ring whose counters sit at a
non-zero body offset stays usable: the producer either fits the record before
the end or publishes the gap as padding and places the record at offset 0.
There is no `head == tail == k` state that can never accept a record.

`reserve()` returns `NULL` when the record cannot be placed now: the ring is
full, the padding plus the record do not fit, the record is larger than the
body, or the previous reservation has not been committed. `NULL` is flow
control, not an error.

## 5. Versioning and unknown opcodes

* The version is `major.minor`, major in the high byte.
* A host accepts a guest only if the major matches **and** the guest's minor is
  not newer than the host's. Anything else is `IOSVM_ERR_VERSION` and the host
  still acks the command.
* A newer guest is rejected rather than partially understood, so a host can never
  apply an opcode it only half knows.
* **An unknown opcode is rejected, never skipped.** Skipping it would break the
  one-ack-per-sequence-number stream (the guest would wait forever or associate
  the next ack with the wrong command) and would let a newer guest drive an older
  host.

## 6. Decode error mapping

| Input | Result |
| --- | --- |
| buffer shorter than the 16-byte header | `IOSVM_ERR_TRUNCATED` |
| `magic != IOSVM_PROTO_MAGIC` | `IOSVM_ERR_UNKNOWN_OP` |
| version not understood | `IOSVM_ERR_VERSION` |
| `op == 0` or `op >= IOSVM_OP_MAX` | `IOSVM_ERR_UNKNOWN_OP` |
| `len` claims more bytes than the caller has | `IOSVM_ERR_TRUNCATED` |
| `len` not a multiple of 8 | `IOSVM_ERR_BAD_ALIGN` |
| `len` differs from the opcode's fixed size (or a `WRITE` has no data block) | `IOSVM_ERR_BOUNDS` |
| caller buffer too small on encode | `IOSVM_ERR_BOUNDS` |
| handle unknown, zero, reused, or the wrong kind of resource | `IOSVM_ERR_BAD_HANDLE` |
| `seq` not strictly increasing | `IOSVM_ERR_SEQ` |
| simulated memory exhausted | `IOSVM_ERR_FULL` |

The error set is closed: an implementation must not invent new negative numbers
on the wire, because an older peer would map them to `IOSVM_ERR_UNKNOWN_OP` when
reported through `iosvm_strerror`.
