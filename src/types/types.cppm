// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

// qnx.types — strong types for the microkernel
// Every handle, state, and permission is a distinct type.
// No raw int in the API. No implicit conversions.

export module qnx.types;
import std;

export namespace qnx {

// ─── Handle types ───────────────────────────────────────────────────────────

/** @brief Unique thread identifier within the microkernel. */
struct ThreadId {
    int value = -1;  ///< Raw numeric thread handle
    constexpr ThreadId() = default;
    constexpr explicit ThreadId(int v) : value{v} {}
    constexpr auto operator<=>(const ThreadId&) const = default;
};

/** @brief Unique channel identifier (server-side IPC endpoint). */
struct ChannelId {
    int value = -1;  ///< Raw numeric channel handle
    constexpr ChannelId() = default;
    constexpr explicit ChannelId(int v) : value{v} {}
    constexpr auto operator<=>(const ChannelId&) const = default;
};

/** @brief Unique connection identifier (client-side binding to a channel). */
struct ConnectionId {
    int value = -1;  ///< Raw numeric connection handle
    constexpr ConnectionId() = default;
    constexpr explicit ConnectionId(int v) : value{v} {}
    constexpr auto operator<=>(const ConnectionId&) const = default;
};

/** @brief Unique process identifier. */
struct ProcessId {
    int value = -1;  ///< Raw numeric process handle
    constexpr ProcessId() = default;
    constexpr explicit ProcessId(int v) : value{v} {}
    constexpr auto operator<=>(const ProcessId&) const = default;
};

/** @brief Capability token for memory region access control. */
struct CapabilityId {
    int value = -1;  ///< Raw numeric capability handle
    constexpr CapabilityId() = default;
    constexpr explicit CapabilityId(int v) : value{v} {}
    constexpr auto operator<=>(const CapabilityId&) const = default;
};

/** @brief Thread scheduling priority (0 = idle, 255 = highest). */
struct Priority {
    int value = 0;  ///< Priority level, higher value = higher urgency
    constexpr Priority() = default;
    constexpr explicit Priority(int v) : value{v} {}
    constexpr auto operator<=>(const Priority&) const = default;
};

// ─── Thread states (QNX Neutrino state machine) ────────────────────────────

/** @brief QNX Neutrino thread state machine states. */
enum class ThreadState {
    ready,            ///< Eligible for scheduling, waiting in ready queue
    running,          ///< Currently executing on CPU
    send_blocked,     ///< Blocked in MsgSend, waiting for server to receive
    receive_blocked,  ///< Blocked in MsgReceive, waiting for client to send
    reply_blocked,    ///< Message delivered to server, waiting for MsgReply
    dead              ///< Thread terminated, awaiting cleanup
};

// ─── IPC types ──────────────────────────────────────────────────────────────

/** @brief Pulse type code identifying the event kind. */
struct PulseCode {
    int value = 0;  ///< Application-defined pulse code
    constexpr PulseCode() = default;
    constexpr explicit PulseCode(int v) : value{v} {}
    constexpr auto operator<=>(const PulseCode&) const = default;
};

/** @brief Payload value delivered with an asynchronous pulse. */
struct PulseValue {
    int value = 0;  ///< Application-defined pulse payload
    constexpr PulseValue() = default;
    constexpr explicit PulseValue(int v) : value{v} {}
    constexpr auto operator<=>(const PulseValue&) const = default;
};

/** @brief Asynchronous non-blocking notification (code + value pair). */
struct Pulse {
    PulseCode  code;   ///< Event type identifier
    PulseValue value;  ///< Event payload
};

// ─── Memory permissions ────────────────────────────────────────────────────

/** @brief Bitmask flags for memory region access permissions. */
enum class Perm : std::uint8_t {
    none  = 0,  ///< No access
    read  = 1,  ///< Read access
    write = 2,  ///< Write access
    exec  = 4,  ///< Execute access
};

/**
 * @brief Combine two permission flags (bitwise OR).
 * @param a First permission set.
 * @param b Second permission set.
 * @return Union of both permission sets.
 */
[[nodiscard]] constexpr auto operator|(Perm a, Perm b) -> Perm {
    return static_cast<Perm>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/**
 * @brief Intersect two permission flags (bitwise AND).
 * @param a First permission set.
 * @param b Second permission set.
 * @return Intersection of both permission sets.
 */
[[nodiscard]] constexpr auto operator&(Perm a, Perm b) -> Perm {
    return static_cast<Perm>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

/**
 * @brief Check whether all required permissions are present.
 * @param have Permissions currently held.
 * @param need Permissions required for the operation.
 * @return True if every bit in need is set in have.
 */
[[nodiscard]] constexpr auto has_perm(Perm have, Perm need) -> bool {
    return (have & need) == need;
}

/** @brief Contiguous memory region with access permissions and capability ownership. */
struct MemRegion {
    std::uintptr_t base;    ///< Virtual base address of the region
    std::size_t    length;  ///< Size in bytes
    Perm           perm;    ///< Access permission flags
    CapabilityId   owner;   ///< Capability that governs this region
};

// ─── Kernel errors ─────────────────────────────────────────────────────────

/** @brief Error codes returned by kernel syscalls via std::expected. */
enum class KernelError {
    invalid_channel,     ///< Channel ID does not exist
    invalid_connection,  ///< Connection ID does not exist
    invalid_thread,      ///< Thread ID does not exist
    permission_denied,   ///< Caller lacks required capability
    would_block,         ///< Non-blocking call has no data available
    no_memory,           ///< Memory allocation failed
    channel_full,        ///< Channel message queue is at capacity
    dead_thread,         ///< Target thread has already terminated
    invalid_capability,  ///< Capability ID does not exist
    not_found,           ///< Requested resource not found
    already_exists       ///< Resource with this identity already registered
};

// ─── Result alias ──────────────────────────────────────────────────────────

/// @brief Kernel result type: value T on success, KernelError on failure.
template<typename T>
using Result = std::expected<T, KernelError>;

/// @brief Kernel result type for operations with no return value.
using VoidResult = std::expected<void, KernelError>;

// ─── Syscall IDs ───────────────────────────────────────────────────────────

/** @brief Enumeration of all microkernel syscall trap numbers. */
enum class SyscallId {
    channel_create,   ///< Create a new IPC channel
    connect_attach,   ///< Attach a connection to an existing channel
    msg_send,         ///< Synchronous blocking message send
    msg_receive,      ///< Blocking receive on a channel
    msg_reply,        ///< Reply to a received message, unblocking sender
    msg_send_pulse,   ///< Asynchronous non-blocking pulse delivery
    thread_create,    ///< Create a new thread in a process
    thread_destroy,   ///< Terminate a thread
    sched_set,        ///< Change thread scheduling parameters
    mem_map,          ///< Map a memory region with permissions
    mem_unmap,        ///< Unmap a previously mapped region
    cap_grant,        ///< Grant a memory capability to another process
    cap_revoke,       ///< Revoke a previously granted capability
    resmgr_register,  ///< Register a resource manager at a pathname
    resmgr_resolve    ///< Resolve a pathname to its resource manager
};

// ─── Constants ─────────────────────────────────────────────────────────────

inline constexpr int max_priority = 255;               ///< Highest schedulable priority level
inline constexpr int idle_priority = 0;                 ///< Priority reserved for the idle thread
inline constexpr std::size_t max_msg_size = 8192;       ///< Maximum IPC message payload in bytes

// ─── Fixed-capacity limits (bare-metal, zero heap allocation) ─────────────

inline constexpr std::size_t max_threads        = 256;  ///< Maximum threads system-wide
inline constexpr std::size_t max_channels       = 256;  ///< Maximum IPC channels
inline constexpr std::size_t max_connections     = 512;  ///< Maximum IPC connections
inline constexpr std::size_t max_reply_slots     = 256;  ///< Maximum pending reply slots
inline constexpr std::size_t max_capabilities    = 256;  ///< Maximum memory capabilities
inline constexpr std::size_t max_buffers         = 16;   ///< Maximum shared memory buffers
inline constexpr std::size_t max_pps_objects     = 64;   ///< Maximum PPS objects
inline constexpr std::size_t max_pps_attrs       = 16;   ///< Maximum attributes per PPS object
inline constexpr std::size_t max_subscriptions   = 64;   ///< Maximum PPS subscriptions
inline constexpr std::size_t max_resource_managers = 32; ///< Maximum resource manager registrations
inline constexpr std::size_t max_name_len        = 64;   ///< Maximum name/key string length
inline constexpr std::size_t max_path_len        = 128;  ///< Maximum pathname length
inline constexpr std::size_t max_pps_value_len   = 256;  ///< Maximum PPS attribute value length
inline constexpr std::size_t max_send_queue      = 8;    ///< Maximum pending messages per channel
inline constexpr std::size_t max_pulse_queue     = 8;    ///< Maximum pending pulses per channel
inline constexpr std::size_t max_msg_pool        = 64;   ///< Maximum concurrent in-flight message buffers
inline constexpr std::size_t max_ready_per_pri   = 64;   ///< Maximum threads per ready-queue priority level

// ─── Convenience aliases ───────────────────────────────────────────────────
// Cut the boilerplate. ProcessId{1} -> pid(1). Priority{10} -> pri(10).

/// @brief Shorthand for ProcessId construction.
[[nodiscard]] constexpr auto pid(int v) -> ProcessId { return ProcessId{v}; }

/// @brief Shorthand for Priority construction.
[[nodiscard]] constexpr auto pri(int v) -> Priority { return Priority{v}; }

/// @brief Shorthand for ThreadId construction.
[[nodiscard]] constexpr auto tid(int v) -> ThreadId { return ThreadId{v}; }

/// @brief Permission combo: read + write.
inline constexpr auto perm_rw  = Perm::read | Perm::write;

/// @brief Permission combo: read + execute.
inline constexpr auto perm_rx  = Perm::read | Perm::exec;

/// @brief Permission combo: read + write + execute.
inline constexpr auto perm_rwx = Perm::read | Perm::write | Perm::exec;

// ─── Message helpers ───────────────────────────────────────────────────────

/// @brief Convert a string literal to a byte span for IPC messages.
[[nodiscard]] inline auto as_bytes(std::string_view s) -> std::span<const std::byte> {
    return std::as_bytes(std::span{s.data(), s.size()});
}

/// @brief Convert a byte span back to string_view for display.
[[nodiscard]] inline auto as_string(std::span<const std::byte> b) -> std::string_view {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

/// @brief Fixed-capacity byte buffer with tracked length (no heap).
template<std::size_t Capacity>
struct FixedBytes {
    std::array<std::byte, Capacity> storage = {};
    std::size_t len = 0;

    constexpr FixedBytes() = default;

    /// @brief Construct from a span of bytes.
    FixedBytes(std::span<const std::byte> src)
        : len{std::min(src.size(), Capacity)} {
        std::copy_n(src.data(), len, storage.data());
    }

    [[nodiscard]] auto size() const -> std::size_t { return len; }
    [[nodiscard]] auto data() const -> const std::byte* { return storage.data(); }
    [[nodiscard]] auto data() -> std::byte* { return storage.data(); }
    [[nodiscard]] auto empty() const -> bool { return len == 0; }

    /// @brief Implicit conversion to span (read-only).
    operator std::span<const std::byte>() const { return {storage.data(), len}; }

    /// @brief Iterator support for range-based for.
    [[nodiscard]] auto begin() const { return storage.data(); }
    [[nodiscard]] auto end() const { return storage.data() + len; }
    [[nodiscard]] auto begin() { return storage.data(); }
    [[nodiscard]] auto end() { return storage.data() + len; }
};

/// @brief Fixed-size message buffer type for IPC channels (no heap).
using MsgBuffer = FixedBytes<max_msg_size>;

/// @brief Index into the global message buffer pool.
struct MsgSlotId {
    std::uint32_t value = 0xFFFFFFFF;
    constexpr MsgSlotId() = default;
    constexpr explicit MsgSlotId(std::uint32_t v) : value{v} {}
    [[nodiscard]] constexpr auto valid() const -> bool { return value != 0xFFFFFFFF; }
};

/// @brief Global fixed-capacity pool of message buffers (no heap).
struct MsgPool {
    std::array<MsgBuffer, max_msg_pool> slots = {};
    std::array<bool, max_msg_pool> used = {};

    [[nodiscard]] auto alloc() -> MsgSlotId {
        for (std::uint32_t i = 0; i < max_msg_pool; ++i) {
            if (!used[i]) {
                used[i] = true;
                slots[i] = MsgBuffer{};
                return MsgSlotId{i};
            }
        }
        return MsgSlotId{};  // invalid
    }

    auto free(MsgSlotId id) -> void {
        if (id.valid() && id.value < max_msg_pool) {
            used[id.value] = false;
        }
    }

    [[nodiscard]] auto at(MsgSlotId id) -> MsgBuffer& { return slots[id.value]; }
    [[nodiscard]] auto at(MsgSlotId id) const -> const MsgBuffer& { return slots[id.value]; }
};

/// @brief Subscription list type for PPS.
using ThreadList = std::span<const ThreadId>;

} // namespace qnx
