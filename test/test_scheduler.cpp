// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

#include <catch2/catch_test_macros.hpp>

import std;
import qnx.scheduler;

using namespace qnx;
using namespace qnx::scheduler;

TEST_CASE("create thread returns valid id", "[scheduler]") {
    Scheduler s;
    auto tid = s.thread_create(pid(1), pri(10));
    REQUIRE(tid.has_value());
    REQUIRE(tid->value > 0);
}

TEST_CASE("thread starts in ready state", "[scheduler]") {
    Scheduler s;
    auto tid = s.thread_create(pid(1), pri(10));
    auto state = s.get_state(*tid);
    REQUIRE(state.has_value());
    REQUIRE(*state == ThreadState::ready);
}

TEST_CASE("schedule_next picks highest priority", "[scheduler]") {
    Scheduler s;
    auto lo = s.thread_create(pid(1), pri(5));
    auto hi = s.thread_create(pid(1), pri(50));
    auto picked = s.schedule_next();
    REQUIRE(picked.has_value());
    REQUIRE(*picked == *hi);
}

TEST_CASE("running thread has running state", "[scheduler]") {
    Scheduler s;
    auto tid = s.thread_create(pid(1), pri(10));
    (void)s.schedule_next();
    auto state = s.get_state(*tid);
    REQUIRE(*state == ThreadState::running);
}

TEST_CASE("block moves thread out of ready queue", "[scheduler]") {
    Scheduler s;
    auto t1 = s.thread_create(pid(1), pri(10));
    auto t2 = s.thread_create(pid(1), pri(5));
    (void)s.thread_block(*t1, ThreadState::send_blocked);
    auto picked = s.schedule_next();
    REQUIRE(picked.has_value());
    REQUIRE(*picked == *t2);
}

TEST_CASE("unblock puts thread back in ready queue", "[scheduler]") {
    Scheduler s;
    auto tid = s.thread_create(pid(1), pri(10));
    (void)s.thread_block(*tid, ThreadState::receive_blocked);
    (void)s.thread_unblock(*tid);
    auto state = s.get_state(*tid);
    REQUIRE(*state == ThreadState::ready);
}

TEST_CASE("preemption on tick", "[scheduler]") {
    Scheduler s;
    auto lo = s.thread_create(pid(1), pri(5));
    (void)s.schedule_next();  // lo is running
    auto hi = s.thread_create(pid(1), pri(50));
    s.tick();  // should preempt lo
    auto current = s.get_current();
    REQUIRE(current.has_value());
    REQUIRE(*current == *hi);
}

TEST_CASE("destroy thread makes it dead", "[scheduler]") {
    Scheduler s;
    auto tid = s.thread_create(pid(1), pri(10));
    (void)s.thread_destroy(*tid);
    auto state = s.get_state(*tid);
    REQUIRE(*state == ThreadState::dead);
}

TEST_CASE("dead thread cannot be unblocked", "[scheduler]") {
    Scheduler s;
    auto tid = s.thread_create(pid(1), pri(10));
    (void)s.thread_destroy(*tid);
    auto r = s.thread_unblock(*tid);
    REQUIRE(!r.has_value());
    REQUIRE(r.error() == KernelError::dead_thread);
}

TEST_CASE("effective_priority boosted by on_queue_change", "[scheduler][priority-inversion]") {
    Scheduler s;
    auto server = s.thread_create(pid(1), pri(5));   // low priority server
    auto client = s.thread_create(pid(2), pri(50));  // high priority client

    // Before: effective = declared
    REQUIRE(s.get_effective_priority(*server)->value == 5);

    // Client waits on server's channel — server gets boosted
    (void)s.on_queue_change(*server, pri(50));
    REQUIRE(s.get_effective_priority(*server)->value == 50);
}

TEST_CASE("effective_priority drops when waiter leaves", "[scheduler][priority-inversion]") {
    Scheduler s;
    auto server = s.thread_create(pid(1), pri(5));

    // Boost
    (void)s.on_queue_change(*server, pri(50));
    REQUIRE(s.get_effective_priority(*server)->value == 50);

    // All waiters gone
    (void)s.on_queue_change(*server, pri(0));
    REQUIRE(s.get_effective_priority(*server)->value == 5);  // back to declared
}

TEST_CASE("effective_priority tracks maximum of multiple waiters", "[scheduler][priority-inversion]") {
    Scheduler s;
    auto server = s.thread_create(pid(1), pri(5));

    // Two waiters: pri 30 and pri 80
    (void)s.on_queue_change(*server, pri(80));
    REQUIRE(s.get_effective_priority(*server)->value == 80);

    // Highest waiter leaves, next is 30
    (void)s.on_queue_change(*server, pri(30));
    REQUIRE(s.get_effective_priority(*server)->value == 30);
}

TEST_CASE("thread_count excludes dead threads", "[scheduler]") {
    Scheduler s;
    auto t1 = s.thread_create(pid(1), pri(10));
    auto t2 = s.thread_create(pid(1), pri(5));
    REQUIRE(s.thread_count() == 2);
    (void)s.thread_destroy(*t1);
    REQUIRE(s.thread_count() == 1);
}