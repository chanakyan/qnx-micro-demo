// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

// qnx.ipc — synchronous message-passing IPC
// QNX Neutrino model: send/receive/reply rendezvous.
// Client sends (blocks), server receives, processes, replies.
// Pulse: async notification, never blocks sender.

export module qnx.ipc;
import std;
export import qnx.types;
import qnx.scheduler;

export namespace qnx::ipc {

// ─── Channel (server-side endpoint) ────────────────────────────────────────

/** @brief A message queued from a sender waiting for the server to receive it. */
struct PendingMessage {
    ThreadId      sender;  ///< Thread that called MsgSend
    ConnectionId  conn;    ///< Connection the message arrived on
    std::vector<std::byte> data;  ///< Message payload bytes
};

/** @brief Server-side IPC endpoint that receives messages and pulses. */
struct Channel {
    ChannelId   id;      ///< Unique channel handle
    ProcessId   owner;   ///< Process that created this channel
    std::vector<PendingMessage> send_queue;   ///< Senders waiting for server to receive
    std::vector<Pulse>          pulse_queue;  ///< Queued async pulses
    std::optional<ThreadId>     receiver;     ///< Server thread blocked on MsgReceive, if any
};

// ─── Connection (client-side binding to a channel) ─────────────────────────

/** @brief Client-side binding that routes messages to a specific channel. */
struct Connection {
    ConnectionId  id;          ///< Unique connection handle
    ChannelId     channel;     ///< Target channel this connection sends to
    ProcessId     client_pid;  ///< Process that owns this connection
};

// ─── Reply slot (holds reply data for blocked sender) ──────────────────────

/** @brief Tracks a pending reply for a sender blocked in reply_blocked state. */
struct ReplySlot {
    ThreadId sender;                ///< Thread awaiting the reply
    std::vector<std::byte> reply_data;  ///< Reply payload (filled by MsgReply)
    bool replied;                   ///< True once server has called MsgReply
};

// ─── IPC subsystem ─────────────────────────────────────────────────────────

/**
 * @brief QNX Neutrino synchronous message-passing IPC subsystem.
 *
 * Implements the send/receive/reply rendezvous protocol. Client threads block
 * on MsgSend until the server calls MsgReceive and then MsgReply. Pulses
 * provide an asynchronous non-blocking notification path.
 */
class Ipc {
    std::vector<Channel>    channels_;
    std::vector<Connection> connections_;
    std::vector<ReplySlot>  reply_slots_;
    int next_ch_id_   = 1;
    int next_conn_id_ = 1;
    scheduler::Scheduler* sched_;

    [[nodiscard]] auto find_channel(ChannelId id) -> Channel* {
        for (auto& ch : channels_) {
            if (ch.id == id) return &ch;
        }
        return nullptr;
    }

    [[nodiscard]] auto find_connection(ConnectionId id) -> Connection* {
        for (auto& c : connections_) {
            if (c.id == id) return &c;
        }
        return nullptr;
    }

    [[nodiscard]] auto find_reply_slot(ThreadId sender) -> ReplySlot* {
        for (auto& rs : reply_slots_) {
            if (rs.sender == sender && !rs.replied) return &rs;
        }
        return nullptr;
    }

public:
    /**
     * @brief Construct the IPC subsystem with a reference to the scheduler.
     * @param sched Scheduler used to block/unblock threads during IPC.
     */
    explicit Ipc(scheduler::Scheduler& sched) : sched_{&sched} {}

    /// @brief Compute max priority among all senders waiting on a channel.
    [[nodiscard]] auto max_waiter_priority(const Channel& ch) const -> Priority {
        int max_pri = 0;
        for (const auto& msg : ch.send_queue) {
            auto pri = sched_->get_priority(msg.sender);
            if (pri && pri->value > max_pri) max_pri = pri->value;
        }
        return Priority{max_pri};
    }

    /// @brief Notify scheduler that a channel's waiter set changed (priority inheritance).
    auto notify_queue_change(const Channel& ch) -> void {
        if (ch.receiver) {
            (void)sched_->on_queue_change(*ch.receiver, max_waiter_priority(ch));
        }
        // Also boost the channel owner if it's running/ready
        // The owner thread is whoever calls MsgReceive — tracked by ch.receiver
    }

    // ─── Channel create / connect ──────────────────────────────────────────

    /**
     * @brief Create a new IPC channel owned by the given process.
     * @param owner Process that will receive messages on this channel.
     * @return ChannelId of the newly created channel.
     */
    [[nodiscard]] auto channel_create(ProcessId owner) -> Result<ChannelId> {
        auto id = ChannelId{next_ch_id_++};
        channels_.push_back(Channel{
            .id = id, .owner = owner,
            .send_queue = {}, .pulse_queue = {},
            .receiver = std::nullopt
        });
        return id;
    }

    /**
     * @brief Attach a client connection to an existing channel.
     * @param ch Target channel to connect to.
     * @param pid Client process establishing the connection.
     * @return ConnectionId on success, KernelError::invalid_channel if channel not found.
     */
    [[nodiscard]] auto connect_attach(ChannelId ch, ProcessId pid) -> Result<ConnectionId> {
        if (!find_channel(ch)) return std::unexpected(KernelError::invalid_channel);
        auto id = ConnectionId{next_conn_id_++};
        connections_.push_back(Connection{
            .id = id, .channel = ch, .client_pid = pid
        });
        return id;
    }

    // ─── MsgSend (client, blocking) ────────────────────────────────────────
    // Client thread -> SEND_BLOCKED
    // If server is RECEIVE_BLOCKED, unblock server immediately
    // Client stays blocked until MsgReply

    /**
     * @brief Send a synchronous message over a connection (blocking).
     *
     * The sending thread transitions to send_blocked. If the server is already
     * waiting in MsgReceive, the message is delivered immediately and the sender
     * moves to reply_blocked. Otherwise the message queues until the server receives.
     * @param sender Thread performing the send.
     * @param conn_id Connection to send over.
     * @param data Message payload.
     * @return Void on success, error if connection/channel invalid.
     */
    [[nodiscard]] auto msg_send(ThreadId sender, ConnectionId conn_id,
                                 std::span<const std::byte> data) -> VoidResult {
        auto* conn = find_connection(conn_id);
        if (!conn) return std::unexpected(KernelError::invalid_connection);

        auto* ch = find_channel(conn->channel);
        if (!ch) return std::unexpected(KernelError::invalid_channel);

        // Enqueue message
        auto msg = PendingMessage{
            .sender = sender, .conn = conn_id,
            .data = std::vector<std::byte>(data.begin(), data.end())
        };

        // Create reply slot for this sender
        reply_slots_.push_back(ReplySlot{
            .sender = sender, .reply_data = {}, .replied = false
        });

        // Block sender
        auto br = sched_->thread_block(sender, ThreadState::send_blocked);
        if (!br) return std::unexpected(br.error());

        // If server is waiting, deliver immediately
        if (ch->receiver) {
            auto server_tid = *ch->receiver;
            ch->receiver = std::nullopt;

            // Move sender to reply_blocked (server now has the message)
            (void)sched_->thread_block(sender, ThreadState::reply_blocked);

            // Unblock server
            (void)sched_->thread_unblock(server_tid);

            // Message goes directly to server (stored in channel for pickup)
            ch->send_queue.push_back(std::move(msg));
        } else {
            ch->send_queue.push_back(std::move(msg));
        }

        // Priority inheritance: boost server to max waiter priority
        notify_queue_change(*ch);

        return {};
    }

    // ─── MsgReceive (server, blocking) ─────────────────────────────────────
    // Server picks up a message from the channel.
    // If no sender waiting, server -> RECEIVE_BLOCKED

    /** @brief A message delivered to the server from MsgReceive. */
    struct ReceivedMessage {
        ConnectionId conn;    ///< Connection the message arrived on
        ThreadId     sender;  ///< Sending thread (needed for MsgReply)
        std::vector<std::byte> data;  ///< Message payload bytes
    };

    /**
     * @brief Receive the next message from a channel (blocking).
     *
     * If a sender is already queued, returns immediately and transitions
     * the sender to reply_blocked. If no messages are pending, the receiver
     * thread blocks until a sender arrives.
     * @param receiver Server thread calling receive.
     * @param ch_id Channel to receive from.
     * @return ReceivedMessage on success, KernelError::would_block if no messages pending.
     */
    [[nodiscard]] auto msg_receive(ThreadId receiver, ChannelId ch_id)
        -> Result<ReceivedMessage> {
        auto* ch = find_channel(ch_id);
        if (!ch) return std::unexpected(KernelError::invalid_channel);

        // Check pulse queue first (async, non-blocking delivery)
        // Pulses are returned as zero-length messages with pulse info
        // (simplified: skip pulses for now, handle messages)

        if (!ch->send_queue.empty()) {
            auto msg = std::move(ch->send_queue.front());
            ch->send_queue.erase(ch->send_queue.begin());

            // Sender transitions: send_blocked -> reply_blocked
            (void)sched_->thread_block(msg.sender, ThreadState::reply_blocked);

            return ReceivedMessage{
                .conn = msg.conn, .sender = msg.sender,
                .data = std::move(msg.data)
            };
        }

        // No messages waiting — block server
        ch->receiver = receiver;
        (void)sched_->thread_block(receiver, ThreadState::receive_blocked);
        return std::unexpected(KernelError::would_block);
    }

    // ─── MsgReply (server -> client) ───────────────────────────────────────
    // Unblocks the original sender, delivers reply data

    /**
     * @brief Reply to a previously received message, unblocking the sender.
     * @param sender Thread that originally sent the message (now reply_blocked).
     * @param data Reply payload to deliver to the sender.
     * @return Void on success, error if no pending reply slot for this sender.
     */
    [[nodiscard]] auto msg_reply(ThreadId sender, std::span<const std::byte> data)
        -> VoidResult {
        auto* slot = find_reply_slot(sender);
        if (!slot) return std::unexpected(KernelError::invalid_thread);

        slot->reply_data = std::vector<std::byte>(data.begin(), data.end());
        slot->replied = true;

        // Unblock sender
        auto r = sched_->thread_unblock(sender);

        // Clean up reply slot — don't leak
        std::erase_if(reply_slots_, [&](const ReplySlot& rs) {
            return rs.sender == sender && rs.replied;
        });

        // Priority inheritance: server priority may drop now that a waiter left.
        for (auto& ch : channels_) {
            notify_queue_change(ch);
        }

        return r;
    }

    // ─── MsgSendPulse (async, non-blocking) ────────────────────────────────
    // Sender is NOT blocked. Pulse is queued.

    /**
     * @brief Send an asynchronous pulse over a connection (non-blocking).
     *
     * The sender is never blocked. If the server is receive-blocked, it is
     * unblocked to process the pulse.
     * @param conn_id Connection to send the pulse over.
     * @param pulse Pulse code + value pair.
     * @return Void on success, error if connection/channel invalid.
     */
    [[nodiscard]] auto msg_send_pulse(ConnectionId conn_id, Pulse pulse)
        -> VoidResult {
        auto* conn = find_connection(conn_id);
        if (!conn) return std::unexpected(KernelError::invalid_connection);

        auto* ch = find_channel(conn->channel);
        if (!ch) return std::unexpected(KernelError::invalid_channel);

        ch->pulse_queue.push_back(pulse);

        // If server is receive-blocked, unblock it
        if (ch->receiver) {
            auto server_tid = *ch->receiver;
            ch->receiver = std::nullopt;
            (void)sched_->thread_unblock(server_tid);
        }

        return {};
    }

    // ─── Queries ───────────────────────────────────────────────────────────

    /**
     * @brief Number of active channels.
     * @return Channel count.
     */
    [[nodiscard]] auto channel_count() const -> std::size_t { return channels_.size(); }

    /**
     * @brief Number of active connections.
     * @return Connection count.
     */
    [[nodiscard]] auto connection_count() const -> std::size_t { return connections_.size(); }

    /**
     * @brief Retrieve the reply data for a completed MsgReply.
     * @param sender Thread whose reply to look up.
     * @return Span of reply bytes if replied, nullopt if still pending.
     */
    [[nodiscard]] auto get_reply(ThreadId sender) const -> std::optional<std::span<const std::byte>> {
        for (const auto& rs : reply_slots_) {
            if (rs.sender == sender && rs.replied) {
                return std::span<const std::byte>(rs.reply_data);
            }
        }
        return std::nullopt;
    }
};

} // namespace qnx::ipc
