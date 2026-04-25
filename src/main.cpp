// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

import std;
import qnx.kernel;

using namespace qnx;
using namespace qnx::kernel;

int main() {
    Kernel k;
    k.init();

    auto& sched = k.scheduler();
    auto& ipc   = k.ipc();
    auto& ns    = k.ns();

    std::println("qnx-micro — C++26 microkernel");
    std::println("  threads: {}", sched.thread_count());

    // Create a server process and channel
    auto server_tid = sched.thread_create(pid(1), pri(10));
    auto ch = ipc.channel_create(pid(1));
    std::println("  server thread={}, channel={}", server_tid->value, ch->value);

    // Register as /dev/console
    auto reg = ns.register_resource("/dev/console", *ch, pid(1));
    std::println("  registered /dev/console");

    // Create a client
    auto client_tid = sched.thread_create(pid(2), pri(5));
    std::println("  client thread={}", client_tid->value);

    // Client opens /dev/console
    auto conn = ns.open("/dev/console", pid(2));
    std::println("  client connected, conn={}", conn->value);

    // Client sends a message
    auto send_result = ipc.msg_send(*client_tid, *conn, as_bytes("hello"));
    std::println("  client sent 'hello', now blocked");

    // Server receives
    auto received = ipc.msg_receive(*server_tid, *ch);
    if (received) {
        std::println("  server received: '{}' from thread {}",
                     as_string(received->data), received->sender.value);

        // Server replies
        auto reply_result = ipc.msg_reply(received->sender, as_bytes("ok"));
        std::println("  server replied 'ok'");
    }

    // Check client state — should be unblocked
    auto client_state = sched.get_state(*client_tid);
    std::println("  client state: {}",
                 *client_state == ThreadState::ready ? "ready" : "other");

    std::println("\nqnx-micro: {} threads, {} channels, {} connections, {} resources",
                 sched.thread_count(), ipc.channel_count(),
                 ipc.connection_count(), ns.count());

    return 0;
}