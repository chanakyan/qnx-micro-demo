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
    int value;  ///< Raw numeric thread handle
    constexpr explicit ThreadId(int v) : value{v} {}
    constexpr auto operator<=>(const ThreadId&) const = default;
};

/** @brief Unique channel identifier (server-side IPC endpoint). */
struct ChannelId {
    int value;  ///< Raw numeric channel handle
    constexpr explicit ChannelId(int v) : value{v} {}
    constexpr auto operator<=>(const ChannelId&) const = default;
};

/** @brief Unique connection identifier (client-side binding to a channel). */
struct ConnectionId {
    int value;  ///< Raw numeric connection handle
    constexpr explicit ConnectionId(int v) : value{v} {}
    constexpr auto operator<=>(const ConnectionId&) const = default;
};

/** @brief Unique process identifier. */
struct ProcessId {
    int value;  ///< Raw numeric process handle
    constexpr explicit ProcessId(int v) : value{v} {}
    constexpr auto operator<=>(const ProcessId&) const = default;
};

/** @brief Capability token for memory region access control. */
struct CapabilityId {
    int value;  ///< Raw numeric capability handle
    constexpr explicit CapabilityId(int v) : value{v} {}
    constexpr auto operator<=>(const CapabilityId&) const = default;
};

/** @brief Thread scheduling priority (0 = idle, 255 = highest). */
struct Priority {
    int value;  ///< Priority level, higher value = higher urgency
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
    int value;  ///< Application-defined pulse code
    constexpr explicit PulseCode(int v) : value{v} {}
    constexpr auto operator<=>(const PulseCode&) const = default;
};

/** @brief Payload value delivered with an asynchronous pulse. */
struct PulseValue {
    int value;  ///< Application-defined pulse payload
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

/// @brief Send queue type for IPC channels.
using SendQueue = std::vector<std::byte>;

/// @brief Subscription list type for PPS.
using ThreadList = std::span<const ThreadId>;

} // namespace qnx
