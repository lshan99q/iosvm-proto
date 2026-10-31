/* SPDX-License-Identifier: MIT */
/*
 * Protocol constants, wire layout and version gating.
 *
 * The _Static_assert()s in iosvm_gpu.h reject a wrong layout at build time;
 * these cases re-check the same numbers at run time so that the suite reports
 * them and so that a compiler that ignores a static assert cannot hide a bug.
 */
#include "tests.h"

static void case_magic(void)
{
    uint8_t wire[4];
    uint32_t back;

    iosvm_put_le32(wire, IOSVM_PROTO_MAGIC);
    CHECK_EQ(wire[0], (uint32_t)'I');
    CHECK_EQ(wire[1], (uint32_t)'O');
    CHECK_EQ(wire[2], (uint32_t)'S');
    CHECK_EQ(wire[3], (uint32_t)'V');

    back = iosvm_get_le32(wire);
    CHECK_EQ(back, IOSVM_PROTO_MAGIC);
    CHECK(IOSVM_PROTO_MAGIC != 0u);
    CHECK(IOSVM_PROTO_MAGIC != IOSVM_PROTO_VERSION);
}

static void case_version_and_ops(void)
{
    CHECK_EQ(IOSVM_PROTO_VERSION, 0x0001u);
    CHECK_EQ(IOSVM_PROTO_VERSION_MAJOR, 0u);
    CHECK_EQ(IOSVM_PROTO_VERSION_MINOR, 1u);
    CHECK_EQ(IOSVM_PROTO_ALIGN, 8u);

    CHECK_EQ(IOSVM_OP_INVALID, 0u);
    CHECK_EQ(IOSVM_OP_CREATE_BUFFER, 1u);
    CHECK_EQ(IOSVM_OP_CREATE_IMAGE, 2u);
    CHECK_EQ(IOSVM_OP_DESTROY, 3u);
    CHECK_EQ(IOSVM_OP_WRITE, 4u);
    CHECK_EQ(IOSVM_OP_BLIT_COPY, 5u);
    CHECK_EQ(IOSVM_OP_BLIT_FILL, 6u);
    CHECK_EQ(IOSVM_OP_SUBMIT, 7u);
    CHECK_EQ(IOSVM_OP_FENCE_WAIT, 8u);
    CHECK_EQ(IOSVM_OP_PRESENT, 9u);
    CHECK_EQ(IOSVM_OP_SCREENSHOT, 10u);
    CHECK_EQ(IOSVM_OP_MAX, 11u);

    CHECK_EQ(IOSVM_OK, 0u);
    CHECK(IOSVM_ERR_VERSION < 0);
    CHECK(IOSVM_ERR_UNKNOWN_OP < 0);
    CHECK(IOSVM_ERR_BOUNDS < 0);
    CHECK(IOSVM_ERR_BAD_HANDLE < 0);
    CHECK(IOSVM_ERR_FULL < 0);
    CHECK(IOSVM_ERR_TRUNCATED < 0);
    CHECK(IOSVM_ERR_BAD_ALIGN < 0);
    CHECK(IOSVM_ERR_SEQ < 0);

    /* The error set is closed: all eight codes are distinct. */
    CHECK(IOSVM_ERR_VERSION != IOSVM_ERR_UNKNOWN_OP);
    CHECK(IOSVM_ERR_UNKNOWN_OP != IOSVM_ERR_BOUNDS);
    CHECK(IOSVM_ERR_BOUNDS != IOSVM_ERR_BAD_HANDLE);
    CHECK(IOSVM_ERR_BAD_HANDLE != IOSVM_ERR_FULL);
    CHECK(IOSVM_ERR_FULL != IOSVM_ERR_TRUNCATED);
    CHECK(IOSVM_ERR_TRUNCATED != IOSVM_ERR_BAD_ALIGN);
    CHECK(IOSVM_ERR_BAD_ALIGN != IOSVM_ERR_SEQ);
}

static void case_struct_sizes(void)
{
    CHECK_EQ(sizeof(struct iosvm_ring_hdr), 32u);
    CHECK_EQ(sizeof(struct iosvm_cmd_hdr), 16u);
    CHECK_EQ(sizeof(struct iosvm_cmd) >= 20u, 1u);
    CHECK_EQ(sizeof(struct iosvm_op_create_buffer), 16u);
    CHECK_EQ(sizeof(struct iosvm_op_create_image), 24u);
    CHECK_EQ(sizeof(struct iosvm_op_destroy), 8u);
    CHECK_EQ(sizeof(struct iosvm_op_write), 16u);
    CHECK_EQ(sizeof(struct iosvm_op_blit_copy), 24u);
    CHECK_EQ(sizeof(struct iosvm_op_blit_fill), 16u);
    CHECK_EQ(sizeof(struct iosvm_op_submit), 8u);
    CHECK_EQ(sizeof(struct iosvm_op_fence_wait), 8u);
    CHECK_EQ(sizeof(struct iosvm_op_present), 8u);
    CHECK_EQ(sizeof(struct iosvm_op_screenshot), 24u);
    CHECK_EQ(sizeof(struct iosvm_ack), 16u);

    CHECK_EQ(IOSVM_RING_HDR_SIZE, 32u);
    CHECK_EQ(IOSVM_CMD_HDR_SIZE, 16u);
    CHECK_EQ(IOSVM_ACK_WIRE_SIZE, 16u);
    CHECK_EQ(IOSVM_MAX_RECORD_SIZE, 16u + 16u + 65536u);
}

static void case_struct_offsets(void)
{
    CHECK_EQ(offsetof(struct iosvm_ring_hdr, magic), 0u);
    CHECK_EQ(offsetof(struct iosvm_ring_hdr, version), 4u);
    CHECK_EQ(offsetof(struct iosvm_ring_hdr, cmd_head), 8u);
    CHECK_EQ(offsetof(struct iosvm_ring_hdr, cmd_tail), 12u);
    CHECK_EQ(offsetof(struct iosvm_ring_hdr, ack_head), 16u);
    CHECK_EQ(offsetof(struct iosvm_ring_hdr, ack_tail), 20u);
    CHECK_EQ(offsetof(struct iosvm_ring_hdr, cmd_size), 24u);
    CHECK_EQ(offsetof(struct iosvm_ring_hdr, ack_size), 28u);

    CHECK_EQ(offsetof(struct iosvm_cmd_hdr, magic), 0u);
    CHECK_EQ(offsetof(struct iosvm_cmd_hdr, version), 4u);
    CHECK_EQ(offsetof(struct iosvm_cmd_hdr, op), 6u);
    CHECK_EQ(offsetof(struct iosvm_cmd_hdr, seq), 8u);
    CHECK_EQ(offsetof(struct iosvm_cmd_hdr, len), 12u);

    CHECK_EQ(offsetof(struct iosvm_cmd, magic), 0u);
    CHECK_EQ(offsetof(struct iosvm_cmd, version), 4u);
    CHECK_EQ(offsetof(struct iosvm_cmd, op), 6u);
    CHECK_EQ(offsetof(struct iosvm_cmd, seq), 8u);
    CHECK_EQ(offsetof(struct iosvm_cmd, len), 12u);
    CHECK_EQ(offsetof(struct iosvm_cmd, payload), 16u);

    CHECK_EQ(offsetof(struct iosvm_op_create_buffer, size), 4u);
    CHECK_EQ(offsetof(struct iosvm_op_create_buffer, flags), 12u);
    CHECK_EQ(offsetof(struct iosvm_op_create_image, format), 12u);
    CHECK_EQ(offsetof(struct iosvm_op_create_image, usage), 16u);
    CHECK_EQ(offsetof(struct iosvm_op_destroy, reserved), 4u);
    CHECK_EQ(offsetof(struct iosvm_op_write, len), 8u);
    CHECK_EQ(offsetof(struct iosvm_op_write, reserved), 12u);
    CHECK_EQ(offsetof(struct iosvm_op_blit_copy, dst_offset), 4u);
    CHECK_EQ(offsetof(struct iosvm_op_blit_copy, src_offset), 12u);
    CHECK_EQ(offsetof(struct iosvm_op_blit_copy, len), 16u);
    CHECK_EQ(offsetof(struct iosvm_op_blit_fill, len), 8u);
    CHECK_EQ(offsetof(struct iosvm_op_blit_fill, value), 12u);
    CHECK_EQ(offsetof(struct iosvm_op_submit, cmd_count), 0u);
    CHECK_EQ(offsetof(struct iosvm_op_submit, flags), 4u);
    CHECK_EQ(offsetof(struct iosvm_op_fence_wait, fence_id), 0u);
    CHECK_EQ(offsetof(struct iosvm_op_fence_wait, timeout_ms), 4u);
    CHECK_EQ(offsetof(struct iosvm_op_present, image), 0u);
    CHECK_EQ(offsetof(struct iosvm_op_present, reserved), 4u);
    CHECK_EQ(offsetof(struct iosvm_op_screenshot, dst), 4u);
    CHECK_EQ(offsetof(struct iosvm_op_screenshot, x), 8u);
    CHECK_EQ(offsetof(struct iosvm_op_screenshot, y), 12u);
    CHECK_EQ(offsetof(struct iosvm_op_screenshot, width), 16u);
    CHECK_EQ(offsetof(struct iosvm_op_screenshot, height), 20u);

    CHECK_EQ(offsetof(struct iosvm_ack, seq), 0u);
    CHECK_EQ(offsetof(struct iosvm_ack, status), 4u);
    CHECK_EQ(offsetof(struct iosvm_ack, handle), 8u);
    CHECK_EQ(offsetof(struct iosvm_ack, reserved), 12u);
}

static void case_payload_alignment(void)
{
    CHECK_EQ(sizeof(struct iosvm_op_create_buffer) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_op_create_image) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_op_destroy) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_op_write) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_op_blit_copy) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_op_blit_fill) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_op_submit) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_op_fence_wait) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_op_present) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_op_screenshot) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_ack) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_cmd_hdr) % IOSVM_PROTO_ALIGN, 0u);
    CHECK_EQ(sizeof(struct iosvm_ring_hdr) % IOSVM_PROTO_ALIGN, 0u);

    CHECK_EQ(iosvm_align8(0u), 0u);
    CHECK_EQ(iosvm_align8(1u), 8u);
    CHECK_EQ(iosvm_align8(8u), 8u);
    CHECK_EQ(iosvm_align8(9u), 16u);
    CHECK_EQ(iosvm_align8(24u), 24u);
}

static void case_version_gate(void)
{
    CHECK_EQ(iosvm_version_compatible(IOSVM_PROTO_VERSION), 1u);
    CHECK_EQ(iosvm_version_compatible(0x0000u), 1u); /* older minor: understood */
    CHECK_EQ(iosvm_version_compatible(0x0002u), 0u); /* newer minor: rejected */
    CHECK_EQ(iosvm_version_compatible(0x00ffu), 0u);
    CHECK_EQ(iosvm_version_compatible(0x0101u), 0u); /* newer major: rejected */
    CHECK_EQ(iosvm_version_compatible(0x0200u), 0u);
    CHECK_EQ(iosvm_version_compatible(0xffffu), 0u);
}

void iosvm_test_proto(void)
{
    iosvm_run_case("proto: magic spells IOSV on the wire", case_magic);
    iosvm_run_case("proto: version, opcodes and error codes", case_version_and_ops);
    iosvm_run_case("proto: struct sizes", case_struct_sizes);
    iosvm_run_case("proto: struct field offsets", case_struct_offsets);
    iosvm_run_case("proto: every payload is 8-byte aligned", case_payload_alignment);
    iosvm_run_case("proto: version gate", case_version_gate);
}
