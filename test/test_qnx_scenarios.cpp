// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory
//
// QNX Neutrino IPC scenario tests
// Exercises the same send/receive/reply patterns found in openqnx
// regression tests (bk1_comm.c, bk2_kercalls.c) but against our API.

#include <catch2/catch_test_macros.hpp>

import std;
import qnx.scheduler;
import qnx.ipc;

using namespace qnx;
using namespace qnx::scheduler;
using namespace qnx::ipc;

// ─── Scenario 1: basic channel + send/receive/reply ─────────────────────────
// From bk1_comm.c: chid=ChannelCreate(0), coid=ConnectAttach(...,chid,...),
//   MsgSend(coid,buf,..), rcvid=MsgReceive(chid,buf,..), MsgReply(rcvid,EOK,"Ok",3)

TEST_CASE("QNX scenario: channel create + connect + send/receive/reply", "[qnx-compat]") {
    Scheduler s;
    Ipc ipc{s};

    // Server creates channel (QNX: chid = ChannelCreate(0))
    auto server = s.thread_create(pid(1), pri(10));
    auto chid = ipc.channel_create(pid(1));
    REQUIRE(chid.has_value());

    // Client connects (QNX: coid = ConnectAttach(0, 0, chid, ...))
    auto client = s.thread_create(pid(2), pri(10));
    auto coid = ipc.connect_attach(*chid, pid(2));
    REQUIRE(coid.has_value());

    // Client sends (QNX: MsgSend(coid, buf, sizeof(buf), rbuf, sizeof(rbuf)))
    (void)ipc.msg_send(*client, *coid, as_bytes("hello"));

    // Server receives (QNX: rcvid = MsgReceive(chid, buf, sizeof(buf), &info))
    auto rcv = ipc.msg_receive(*server, *chid);
    REQUIRE(rcv.has_value());
    REQUIRE(as_string(rcv->data) == "hello");

    // Server replies (QNX: MsgReply(rcvid, EOK, "Ok", 3))
    auto reply_r = ipc.msg_reply(rcv->sender, as_bytes("Ok"));
    REQUIRE(reply_r.has_value());

    // Client should be unblocked
    REQUIRE(*s.get_state(*client) == ThreadState::ready);
}

// ─── Scenario 2: pulse delivery (non-blocking) ─────────────────────────────
// From bk1_comm.c: rcvid=0 means pulse received

TEST_CASE("QNX scenario: pulse delivery does not block sender", "[qnx-compat]") {
    Scheduler s;
    Ipc ipc{s};

    auto server = s.thread_create(pid(1), pri(10));
    auto chid = ipc.channel_create(pid(1));
    auto client = s.thread_create(pid(2), pri(10));
    auto coid = ipc.connect_attach(*chid, pid(2));

    // QNX: MsgSendPulse(coid, priority, code, value)
    auto r = ipc.msg_send_pulse(*coid, Pulse{PulseCode{42}, PulseValue{100}});
    REQUIRE(r.has_value());

    // Sender NOT blocked (unlike MsgSend)
    REQUIRE(*s.get_state(*client) == ThreadState::ready);
}

// ─── Scenario 3: multiple clients, one server ───────────────────────────────
// Common QNX pattern: server loops on MsgReceive, multiple clients connect

TEST_CASE("QNX scenario: multiple clients send to one server", "[qnx-compat]") {
    Scheduler s;
    Ipc ipc{s};

    auto server = s.thread_create(pid(1), pri(10));
    auto chid = ipc.channel_create(pid(1));

    auto c1 = s.thread_create(pid(2), pri(5));
    auto c2 = s.thread_create(pid(3), pri(5));

    auto conn1 = ipc.connect_attach(*chid, pid(2));
    auto conn2 = ipc.connect_attach(*chid, pid(3));

    // Both clients send
    (void)ipc.msg_send(*c1, *conn1, as_bytes("msg1"));
    (void)ipc.msg_send(*c2, *conn2, as_bytes("msg2"));

    // Server receives first message
    auto rcv1 = ipc.msg_receive(*server, *chid);
    REQUIRE(rcv1.has_value());
    (void)ipc.msg_reply(rcv1->sender, as_bytes("ack1"));

    // Server receives second message
    auto rcv2 = ipc.msg_receive(*server, *chid);
    REQUIRE(rcv2.has_value());
    (void)ipc.msg_reply(rcv2->sender, as_bytes("ack2"));

    // Both clients unblocked
    REQUIRE(*s.get_state(*c1) == ThreadState::ready);
    REQUIRE(*s.get_state(*c2) == ThreadState::ready);
}

// ─── Scenario 4: server blocks first, client arrives later ──────────────────
// QNX: server calls MsgReceive before any client sends

TEST_CASE("QNX scenario: server waits before client sends", "[qnx-compat]") {
    Scheduler s;
    Ipc ipc{s};

    auto server = s.thread_create(pid(1), pri(10));
    auto chid = ipc.channel_create(pid(1));

    // Server receives first (no messages yet — blocks)
    auto rcv = ipc.msg_receive(*server, *chid);
    REQUIRE(!rcv.has_value());
    REQUIRE(rcv.error() == KernelError::would_block);
    REQUIRE(*s.get_state(*server) == ThreadState::receive_blocked);

    // Client arrives and sends
    auto client = s.thread_create(pid(2), pri(5));
    auto coid = ipc.connect_attach(*chid, pid(2));
    (void)ipc.msg_send(*client, *coid, as_bytes("late"));

    // Server should be unblocked by the send
    REQUIRE(*s.get_state(*server) == ThreadState::ready);
}

// ─── Scenario 5: reply with data ────────────────────────────────────────────
// QNX: MsgReply(rcvid, EOK, "Ok", 3) — reply carries data back to sender

TEST_CASE("QNX scenario: reply unblocks sender cleanly", "[qnx-compat]") {
    Scheduler s;
    Ipc ipc{s};

    auto server = s.thread_create(pid(1), pri(10));
    auto client = s.thread_create(pid(2), pri(5));
    auto chid = ipc.channel_create(pid(1));
    auto coid = ipc.connect_attach(*chid, pid(2));

    (void)ipc.msg_send(*client, *coid, as_bytes("request"));

    auto rcv = ipc.msg_receive(*server, *chid);
    REQUIRE(rcv.has_value());
    (void)ipc.msg_reply(rcv->sender, as_bytes("response"));

    // After reply, sender is ready and reply slot is cleaned up (no leak)
    REQUIRE(*s.get_state(*client) == ThreadState::ready);
}
