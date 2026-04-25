// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

#include <catch2/catch_test_macros.hpp>

import std;
import qnx.scheduler;
import qnx.ipc;

using namespace qnx;
using namespace qnx::scheduler;
using namespace qnx::ipc;

TEST_CASE("channel_create returns valid id", "[ipc]") {
    Scheduler s;
    Ipc ipc{s};
    auto ch = ipc.channel_create(pid(1));
    REQUIRE(ch.has_value());
    REQUIRE(ch->value > 0);
}

TEST_CASE("connect_attach to valid channel succeeds", "[ipc]") {
    Scheduler s;
    Ipc ipc{s};
    auto ch = ipc.channel_create(pid(1));
    auto conn = ipc.connect_attach(*ch, pid(2));
    REQUIRE(conn.has_value());
}

TEST_CASE("connect_attach to invalid channel fails", "[ipc]") {
    Scheduler s;
    Ipc ipc{s};
    auto conn = ipc.connect_attach(ChannelId{999}, pid(2));
    REQUIRE(!conn.has_value());
    REQUIRE(conn.error() == KernelError::invalid_channel);
}

TEST_CASE("msg_send blocks sender", "[ipc]") {
    Scheduler s;
    Ipc ipc{s};
    auto server = s.thread_create(pid(1), pri(10));
    auto client = s.thread_create(pid(2), pri(5));
    auto ch = ipc.channel_create(pid(1));
    auto conn = ipc.connect_attach(*ch, pid(2));

    (void)ipc.msg_send(*client, *conn, as_bytes("abc"));

    // Client should be blocked (send_blocked or reply_blocked)
    auto state = s.get_state(*client);
    REQUIRE(state.has_value());
    REQUIRE((*state == ThreadState::send_blocked || *state == ThreadState::reply_blocked));
}

TEST_CASE("full send/receive/reply cycle", "[ipc]") {
    Scheduler s;
    Ipc ipc{s};
    auto server_tid = s.thread_create(pid(1), pri(10));
    auto client_tid = s.thread_create(pid(2), pri(5));
    auto ch = ipc.channel_create(pid(1));
    auto conn = ipc.connect_attach(*ch, pid(2));

    // Client sends
    (void)ipc.msg_send(*client_tid, *conn, as_bytes("hi"));

    // Server receives
    auto received = ipc.msg_receive(*server_tid, *ch);
    REQUIRE(received.has_value());
    REQUIRE(received->data.size() == 2);
    REQUIRE(received->sender == *client_tid);

    // Client should be reply_blocked
    auto client_state = s.get_state(*client_tid);
    REQUIRE(*client_state == ThreadState::reply_blocked);

    // Server replies
    auto reply_result = ipc.msg_reply(received->sender, as_bytes("k"));
    REQUIRE(reply_result.has_value());

    // Client should be ready
    client_state = s.get_state(*client_tid);
    REQUIRE(*client_state == ThreadState::ready);
}

TEST_CASE("msg_receive blocks when no sender", "[ipc]") {
    Scheduler s;
    Ipc ipc{s};
    auto server = s.thread_create(pid(1), pri(10));
    auto ch = ipc.channel_create(pid(1));

    auto result = ipc.msg_receive(*server, *ch);
    REQUIRE(!result.has_value());
    REQUIRE(result.error() == KernelError::would_block);

    auto state = s.get_state(*server);
    REQUIRE(*state == ThreadState::receive_blocked);
}

TEST_CASE("pulse does not block sender", "[ipc]") {
    Scheduler s;
    Ipc ipc{s};
    auto server = s.thread_create(pid(1), pri(10));
    auto client = s.thread_create(pid(2), pri(5));
    auto ch = ipc.channel_create(pid(1));
    auto conn = ipc.connect_attach(*ch, pid(2));

    auto result = ipc.msg_send_pulse(*conn, Pulse{PulseCode{1}, PulseValue{42}});
    REQUIRE(result.has_value());

    // Client should NOT be blocked
    auto state = s.get_state(*client);
    REQUIRE(*state == ThreadState::ready);
}

TEST_CASE("pulse unblocks receive-blocked server", "[ipc]") {
    Scheduler s;
    Ipc ipc{s};
    auto server = s.thread_create(pid(1), pri(10));
    auto ch = ipc.channel_create(pid(1));
    auto conn = ipc.connect_attach(*ch, pid(1));

    // Server blocks waiting for message
    (void)ipc.msg_receive(*server, *ch);
    REQUIRE(*s.get_state(*server) == ThreadState::receive_blocked);

    // Pulse arrives
    (void)ipc.msg_send_pulse(*conn, Pulse{PulseCode{1}, PulseValue{0}});

    // Server should be unblocked
    REQUIRE(*s.get_state(*server) == ThreadState::ready);
}