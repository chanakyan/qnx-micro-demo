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
    MsgSlotId     data_slot;  ///< Index into the message pool
};

/** @brief Server-side IPC endpoint that receives messages and pulses. */
struct Channel {
    ChannelId   id;      ///< Unique channel handle
    ProcessId   owner;   ///< Process that created this channel
    std::array<PendingMessage, max_send_queue> send_queue = {};  ///< Senders waiting for server to receive
    std::uint32_t num_send = 0;  ///< Number of pending messages
    std::array<Pulse, max_pulse_queue> pulse_queue = {};  ///< Queued async pulses
    std::uint32_t num_pulses = 0;  ///< Number of queued pulses
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
    MsgSlotId reply_slot;           ///< Index into pool for reply payload (filled by MsgReply)
    bool replied = false;           ///< True once server has called MsgReply
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
    std::array<Channel, max_channels>       channels_ = {};
    std::uint32_t num_channels_ = 0;
    std::array<Connection, max_connections>  connections_ = {};
    std::uint32_t num_connections_ = 0;
    std::array<ReplySlot, max_reply_slots>  reply_slots_ = {};
    std::uint32_t num_reply_slots_ = 0;
    MsgPool msg_pool_;
    int next_ch_id_   = 1;
    int next_conn_id_ = 1;
    scheduler::Scheduler* sched_;

    [[nodiscard]] auto find_channel_idx(ChannelId id) -> int {
        for (int i = 0; i < num_channels_; ++i) { if (channels_[i].id == id) return i; }
        return -1;
    }

    [[nodiscard]] auto find_connection_idx(ConnectionId id) -> int {
        for (int i = 0; i < num_connections_; ++i) { if (connections_[i].id == id) return i; }
        return -1;
    }

    [[nodiscard]] auto find_reply_slot_idx(ThreadId sender) -> int {
        for (int i = 0; i < num_reply_slots_; ++i) { if (reply_slots_[i].sender == sender && !reply_slots_[i].replied) return i; }
        return -1;
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
        auto s = std::span{ch.send_queue.data(), ch.num_send};
        for (const auto& msg : s) {
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
    }

    // ─── Channel create / connect ──────────────────────────────────────────

    /**
     * @brief Create a new IPC channel owned by the given process.
     * @param owner Process that will receive messages on this channel.
     * @return ChannelId of the newly created channel.
     */
    [[nodiscard]] auto channel_create(ProcessId owner) -> Result<ChannelId> {
        if (num_channels_ >= max_channels) return std::unexpected(KernelError::no_memory);
        auto id = ChannelId{next_ch_id_++};
        auto& ch = channels_[num_channels_++];
        ch.id = id;
        ch.owner = owner;
        ch.num_send = 0;
        ch.num_pulses = 0;
        ch.receiver = std::nullopt;
        return id;
    }

    /**
     * @brief Attach a client connection to an existing channel.
     * @param ch Target channel to connect to.
     * @param pid Client process establishing the connection.
     * @return ConnectionId on success, KernelError::invalid_channel if channel not found.
     */
    [[nodiscard]] auto connect_attach(ChannelId ch, ProcessId pid) -> Result<ConnectionId> {
        if (find_channel_idx(ch) < 0) return std::unexpected(KernelError::invalid_channel);
        if (num_connections_ >= max_connections) return std::unexpected(KernelError::no_memory);
        auto id = ConnectionId{next_conn_id_++};
        auto& c = connections_[num_connections_++];
        c.id = id;
        c.channel = ch;
        c.client_pid = pid;
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
        auto ci = find_connection_idx(conn_id);
        if (ci < 0) return std::unexpected(KernelError::invalid_connection);
        const auto& conn = connections_[ci];

        auto chi = find_channel_idx(conn.channel);
        if (chi < 0) return std::unexpected(KernelError::invalid_channel);
        auto& ch = channels_[chi];

        // Allocate pool slot for message data
        auto data_slot = msg_pool_.alloc();
        if (!data_slot.valid()) return std::unexpected(KernelError::no_memory);
        msg_pool_.at(data_slot) = MsgBuffer{data};

        // Enqueue message
        PendingMessage msg;
        msg.sender = sender;
        msg.conn = conn_id;
        msg.data_slot = data_slot;

        // Create reply slot for this sender
        if (num_reply_slots_ >= max_reply_slots) {
            msg_pool_.free(data_slot);
            return std::unexpected(KernelError::no_memory);
        }
        auto& slot = reply_slots_[num_reply_slots_++];
        slot.sender = sender;
        slot.reply_slot = MsgSlotId{};  // no reply data yet
        slot.replied = false;

        // Block sender
        auto br = sched_->thread_block(sender, ThreadState::send_blocked);
        if (!br) return std::unexpected(br.error());

        // If server is waiting, deliver immediately
        if (ch.receiver) {
            auto server_tid = *ch.receiver;
            ch.receiver = std::nullopt;

            // Move sender to reply_blocked (server now has the message)
            (void)sched_->thread_block(sender, ThreadState::reply_blocked);

            // Unblock server
            (void)sched_->thread_unblock(server_tid);

            // Message goes directly to server (stored in channel for pickup)
            if (ch.num_send < max_send_queue) ch.send_queue[ch.num_send++] = msg;
        } else {
            if (ch.num_send < max_send_queue) ch.send_queue[ch.num_send++] = msg;
        }

        // Priority inheritance: boost server to max waiter priority
        notify_queue_change(ch);

        return {};
    }

    // ─── MsgReceive (server, blocking) ─────────────────────────────────────
    // Server picks up a message from the channel.
    // If no sender waiting, server -> RECEIVE_BLOCKED

    /** @brief A message delivered to the server from MsgReceive. */
    struct ReceivedMessage {
        ConnectionId conn;    ///< Connection the message arrived on
        ThreadId     sender;  ///< Sending thread (needed for MsgReply)
        MsgBuffer    data;    ///< Message payload bytes (copied from pool)
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
        auto chi = find_channel_idx(ch_id);
        if (chi < 0) return std::unexpected(KernelError::invalid_channel);
        auto& ch = channels_[chi];

        // Check pulse queue first (async, non-blocking delivery)
        // Pulses are returned as zero-length messages with pulse info
        // (simplified: skip pulses for now, handle messages)

        if (ch.num_send > 0) {
            auto msg = ch.send_queue[0];
            // Shift remaining (FIFO)
            for (std::uint32_t i = 0; i + 1 < ch.num_send; ++i) {
                ch.send_queue[i] = ch.send_queue[i + 1];
            }
            --ch.num_send;

            // Sender transitions: send_blocked -> reply_blocked
            (void)sched_->thread_block(msg.sender, ThreadState::reply_blocked);

            ReceivedMessage rm;
            rm.conn = msg.conn;
            rm.sender = msg.sender;
            // Copy data from pool, then free the send slot
            if (msg.data_slot.valid()) rm.data = msg_pool_.at(msg.data_slot);
            msg_pool_.free(msg.data_slot);
            return rm;
        }

        // No messages waiting — block server
        ch.receiver = receiver;
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
        auto si = find_reply_slot_idx(sender);
        if (si < 0) return std::unexpected(KernelError::invalid_thread);

        // Allocate pool slot for reply data
        auto reply_data_slot = msg_pool_.alloc();
        if (!reply_data_slot.valid()) return std::unexpected(KernelError::no_memory);
        msg_pool_.at(reply_data_slot) = MsgBuffer{data};
        reply_slots_[si].reply_slot = reply_data_slot;
        reply_slots_[si].replied = true;

        // Unblock sender
        auto r = sched_->thread_unblock(sender);

        // Clean up reply slot — free pool slot, swap with last, decrement count
        for (std::uint32_t i = 0; i < num_reply_slots_; ++i) {
            if (reply_slots_[i].sender == sender && reply_slots_[i].replied) {
                msg_pool_.free(reply_slots_[i].reply_slot);
                reply_slots_[i] = reply_slots_[num_reply_slots_ - 1];
                --num_reply_slots_;
                break;
            }
        }

        // Priority inheritance: server priority may drop now that a waiter left.
        auto chs = std::span{channels_.data(), num_channels_};
        for (auto& ch : chs) {
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
        auto ci = find_connection_idx(conn_id);
        if (ci < 0) return std::unexpected(KernelError::invalid_connection);
        const auto& conn = connections_[ci];

        auto chi = find_channel_idx(conn.channel);
        if (chi < 0) return std::unexpected(KernelError::invalid_channel);
        auto& ch = channels_[chi];

        if (ch.num_pulses >= max_pulse_queue) return std::unexpected(KernelError::channel_full);
        ch.pulse_queue[ch.num_pulses++] = pulse;

        // If server is receive-blocked, unblock it
        if (ch.receiver) {
            auto server_tid = *ch.receiver;
            ch.receiver = std::nullopt;
            (void)sched_->thread_unblock(server_tid);
        }

        return {};
    }

    // ─── Queries ───────────────────────────────────────────────────────────

    /**
     * @brief Number of active channels.
     * @return Channel count.
     */
    [[nodiscard]] auto channel_count() const -> std::size_t { return num_channels_; }

    /**
     * @brief Number of active connections.
     * @return Connection count.
     */
    [[nodiscard]] auto connection_count() const -> std::size_t { return num_connections_; }

    /**
     * @brief Retrieve the reply data for a completed MsgReply.
     * @param sender Thread whose reply to look up.
     * @return Span of reply bytes if replied, nullopt if still pending.
     */
    [[nodiscard]] auto get_reply(ThreadId sender) const -> std::optional<std::span<const std::byte>> {
        for (int i = 0; i < num_reply_slots_; ++i) {
            const auto& rs = reply_slots_[i];
            if (rs.sender == sender && rs.replied && rs.reply_slot.valid()) {
                const auto& buf = msg_pool_.at(rs.reply_slot);
                return std::span<const std::byte>(buf);
            }
        }
        return std::nullopt;
    }
};

} // namespace qnx::ipc
