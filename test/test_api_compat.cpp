// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory
//
// API compatibility test: verify qnx_micro.h signatures match
// the QNX Neutrino calling convention (int returns, int handles,
// void* buffers, size as int/size_t).
//
// Reference: QNX Neutrino sys/neutrino.h (openqnx)
//   ChannelCreate(unsigned flags) -> int
//   ConnectAttach(nd, pid, chid, index, flags) -> int
//   MsgSend(coid, smsg, sbytes, rmsg, rbytes) -> int
//   MsgReceive(chid, msg, bytes, info) -> int
//   MsgReply(rcvid, status, msg, bytes) -> int
//   MsgSendPulse(coid, priority, code, value) -> int
//
// Our API uses int32_t (fixed width) instead of int (platform-dependent)
// and qnx_process_id instead of pid_t. Otherwise the calling convention
// is identical: handle in, buffer+length, int return, negative on error.

#include <catch2/catch_test_macros.hpp>

// This is the only file that includes qnx_micro.h directly
#include "../src/qnx_micro.h"

#include <cstdint>
#include <type_traits>

// ─── Type checks ────────────────────────────────────────────────────────────

TEST_CASE("handle types are int32_t", "[api-compat]") {
    STATIC_REQUIRE(std::is_same_v<qnx_thread_id, int32_t>);
    STATIC_REQUIRE(std::is_same_v<qnx_channel_id, int32_t>);
    STATIC_REQUIRE(std::is_same_v<qnx_connection_id, int32_t>);
    STATIC_REQUIRE(std::is_same_v<qnx_process_id, int32_t>);
    STATIC_REQUIRE(std::is_same_v<qnx_capability_id, int32_t>);
}

TEST_CASE("pulse struct is 8 bytes (code + value)", "[api-compat]") {
    STATIC_REQUIRE(sizeof(qnx_pulse_t) == 8);
    STATIC_REQUIRE(std::is_standard_layout_v<qnx_pulse_t>);
}

// ─── Signature checks (function pointer compatibility) ──────────────────────

// QNX convention: returns int (we use int32_t), takes handle + buffer + length.
// These static_asserts verify the function signatures exist and return int32_t.

TEST_CASE("channel_create takes process_id, returns int32_t", "[api-compat]") {
    using F = decltype(&qnx_channel_create);
    STATIC_REQUIRE(std::is_same_v<std::invoke_result_t<F, qnx_process_id>, int32_t>);
}

TEST_CASE("connect_attach takes channel_id + process_id", "[api-compat]") {
    using F = decltype(&qnx_connect_attach);
    STATIC_REQUIRE(std::is_same_v<
        std::invoke_result_t<F, qnx_channel_id, qnx_process_id>, int32_t>);
}

TEST_CASE("msg_send takes conn + sbuf + slen + rbuf + rlen", "[api-compat]") {
    using F = decltype(&qnx_msg_send);
    STATIC_REQUIRE(std::is_same_v<
        std::invoke_result_t<F, qnx_connection_id, const void*, size_t, void*, size_t>,
        int32_t>);
}

TEST_CASE("msg_receive takes channel + buf + len", "[api-compat]") {
    using F = decltype(&qnx_msg_receive);
    STATIC_REQUIRE(std::is_same_v<
        std::invoke_result_t<F, qnx_channel_id, void*, size_t>, int32_t>);
}

TEST_CASE("msg_reply takes sender + buf + len", "[api-compat]") {
    using F = decltype(&qnx_msg_reply);
    STATIC_REQUIRE(std::is_same_v<
        std::invoke_result_t<F, qnx_thread_id, const void*, size_t>, int32_t>);
}

TEST_CASE("msg_send_pulse takes conn + code + value (no blocking)", "[api-compat]") {
    using F = decltype(&qnx_msg_send_pulse);
    STATIC_REQUIRE(std::is_same_v<
        std::invoke_result_t<F, qnx_connection_id, int32_t, int32_t>, int32_t>);
}

TEST_CASE("thread_create takes pid + priority", "[api-compat]") {
    using F = decltype(&qnx_thread_create);
    STATIC_REQUIRE(std::is_same_v<
        std::invoke_result_t<F, qnx_process_id, int32_t>, int32_t>);
}

// ─── Permission constants match QNX convention ─────────────────────────────

TEST_CASE("permission constants are standard POSIX-like bits", "[api-compat]") {
    STATIC_REQUIRE(QNX_PERM_NONE == 0);
    STATIC_REQUIRE(QNX_PERM_READ == 1);
    STATIC_REQUIRE(QNX_PERM_WRITE == 2);
    STATIC_REQUIRE(QNX_PERM_EXEC == 4);
    STATIC_REQUIRE((QNX_PERM_READ | QNX_PERM_WRITE) == 3);
}
