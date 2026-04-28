// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

// qnx.memory — capability-based memory management
// Real backing store. No shared memory without explicit capability grant.
// Every region has an owner. Permissions are checked on access.
// Grant never escalates. Revoke cascades to children.

export module qnx.memory;
import std;
export import qnx.types;

export namespace qnx::memory {

// ─── Shared buffer (the actual bytes) ──────────────────────────────────────

/// @brief Backing store ID — multiple capabilities can reference the same buffer.
struct BufferId {
    int value = -1;
    constexpr BufferId() = default;
    constexpr explicit BufferId(int v) : value{v} {}
    constexpr auto operator<=>(const BufferId&) const = default;
};

/// @brief Actual memory buffer with fixed backing store.
struct SharedBuffer {
    BufferId              id;      ///< Unique buffer identity
    std::array<std::byte, max_msg_size> data = {};  ///< The actual bytes
    std::size_t data_len = 0;  ///< Allocated length within data
};

// ─── Capability ────────────────────────────────────────────────────────────

/// @brief A capability token binding a process to a shared buffer with permissions.
struct Capability {
    CapabilityId           id;       ///< Unique capability handle
    ProcessId              owner;    ///< Process holding this capability
    BufferId               buffer;   ///< Which shared buffer this references
    std::size_t            offset;   ///< Offset into the buffer
    std::size_t            length;   ///< Accessible length from offset
    Perm                   perm;     ///< Access permissions
    std::optional<CapabilityId> parent; ///< Capability this was granted from (nullopt = root)
};

// ─── Memory manager ───────────────────────────────────────────────────────

/**
 * @brief Capability-based memory manager with real backing store.
 *
 * Capabilities reference shared buffers. Grant creates a child capability
 * with restricted (never escalated) permissions. Revoke cascades to all
 * children of the revoked capability.
 */
class MemoryManager {
    std::array<SharedBuffer, max_buffers> buffers_ = {};
    std::uint32_t num_buffers_ = 0;
    std::array<Capability, max_capabilities> caps_ = {};
    std::uint32_t num_caps_ = 0;
    int next_cap_id_ = 1;
    int next_buf_id_ = 1;

    [[nodiscard]] auto find_cap_idx(CapabilityId id) -> int {
        for (std::uint32_t i = 0; i < num_caps_; ++i) {
            if (caps_[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }

    [[nodiscard]] auto find_cap_idx(CapabilityId id) const -> int {
        for (std::uint32_t i = 0; i < num_caps_; ++i) {
            if (caps_[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }

    [[nodiscard]] auto find_buffer_idx(BufferId id) -> int {
        for (std::uint32_t i = 0; i < num_buffers_; ++i) {
            if (buffers_[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }

    [[nodiscard]] auto find_buffer_idx(BufferId id) const -> int {
        for (std::uint32_t i = 0; i < num_buffers_; ++i) {
            if (buffers_[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }

    /// @brief Fixed-size buffer for collecting capability IDs during revocation.
    struct CapIdList {
        std::array<CapabilityId, max_capabilities> ids = {};
        std::uint32_t count = 0;
        auto push(CapabilityId id) -> void {
            if (count < max_capabilities) ids[count++] = id;
        }
        [[nodiscard]] auto contains(CapabilityId id) const -> bool {
            auto s = std::span{ids.data(), count};
            return std::ranges::find(s, id) != s.end();
        }
    };

    /// @brief Collect all capability IDs that are children (direct or transitive) of a parent.
    auto collect_children(CapabilityId parent_id, CapIdList& out) const -> void {
        auto s = std::span{caps_.data(), num_caps_};
        for (const auto& c : s) {
            if (c.parent == parent_id) {
                out.push(c.id);
                collect_children(c.id, out);
            }
        }
    }

public:
    // ─── Allocate ──────────────────────────────────────────────────────────

    /**
     * @brief Allocate a real memory buffer and return a root capability.
     * @param pid Process requesting the allocation.
     * @param length Size in bytes.
     * @param perm Access permissions.
     * @return CapabilityId of the root capability for this buffer.
     */
    [[nodiscard]] auto mmap(ProcessId pid, std::size_t length, Perm perm)
        -> Result<CapabilityId> {
        if (num_buffers_ >= max_buffers) return std::unexpected(KernelError::no_memory);
        if (num_caps_ >= max_capabilities) return std::unexpected(KernelError::no_memory);
        if (length > max_msg_size) return std::unexpected(KernelError::no_memory);

        auto buf_id = BufferId{next_buf_id_++};
        auto& buf = buffers_[num_buffers_++];
        buf.id = buf_id;
        buf.data = {};  // zero-fill
        buf.data_len = length;

        auto cap_id = CapabilityId{next_cap_id_++};
        auto& cap = caps_[num_caps_++];
        cap.id = cap_id;
        cap.owner = pid;
        cap.buffer = buf_id;
        cap.offset = 0;
        cap.length = length;
        cap.perm = perm;
        cap.parent = std::nullopt;

        return cap_id;
    }

    /**
     * @brief Release a capability and its buffer if no other capabilities reference it.
     * @param cap_id Capability to release.
     * @return Void on success.
     */
    [[nodiscard]] auto munmap(CapabilityId cap_id) -> VoidResult {
        auto ci = find_cap_idx(cap_id);
        if (ci < 0) return std::unexpected(KernelError::invalid_capability);
        auto& cap = caps_[ci];

        auto buf_id = cap.buffer;

        // Revoke this and all children
        CapIdList to_remove;
        to_remove.push(cap_id);
        collect_children(cap_id, to_remove);

        // Remove matching capabilities (swap-with-last)
        for (std::uint32_t i = 0; i < num_caps_; ) {
            if (to_remove.contains(caps_[i].id)) {
                caps_[i] = caps_[num_caps_ - 1];
                --num_caps_;
            } else {
                ++i;
            }
        }

        // If no capabilities reference this buffer, free it
        auto cap_span = std::span{caps_.data(), num_caps_};
        bool still_referenced = std::ranges::any_of(cap_span, [&](const Capability& c) {
            return c.buffer == buf_id;
        });
        if (!still_referenced) {
            for (std::uint32_t i = 0; i < num_buffers_; ++i) {
                if (buffers_[i].id == buf_id) {
                    buffers_[i] = buffers_[num_buffers_ - 1];
                    --num_buffers_;
                    break;
                }
            }
        }

        return {};
    }

    // ─── Grant ─────────────────────────────────────────────────────────────

    /**
     * @brief Grant a capability to another process with restricted permissions.
     *
     * The child capability references the same buffer as the parent.
     * Permissions are intersected (never escalated). The child can
     * access a sub-range of the parent's region.
     * @param cap_id Source capability.
     * @param target Receiving process.
     * @param restricted Maximum permissions (intersected with parent).
     * @param sub_offset Offset within parent's region (default 0).
     * @param sub_length Length within parent's region (default = parent's length).
     * @return CapabilityId of the child capability.
     */
    [[nodiscard]] auto capability_grant(CapabilityId cap_id, ProcessId target,
                                         Perm restricted,
                                         std::size_t sub_offset = 0,
                                         std::size_t sub_length = 0)
        -> Result<CapabilityId> {
        auto pi = find_cap_idx(cap_id);
        if (pi < 0) return std::unexpected(KernelError::invalid_capability);
        auto& parent = caps_[pi];

        // Default: same length as parent
        if (sub_length == 0) sub_length = parent.length - sub_offset;

        // Sub-region must fit within parent
        if (sub_offset + sub_length > parent.length) {
            return std::unexpected(KernelError::permission_denied);
        }

        if (num_caps_ >= max_capabilities) return std::unexpected(KernelError::no_memory);
        auto new_id = CapabilityId{next_cap_id_++};
        auto& c = caps_[num_caps_++];
        c.id = new_id;
        c.owner = target;
        c.buffer = parent.buffer;
        c.offset = parent.offset + sub_offset;
        c.length = sub_length;
        c.perm = parent.perm & restricted;  // never escalate
        c.parent = cap_id;

        return new_id;
    }

    /**
     * @brief Revoke a capability and all capabilities derived from it.
     * @param cap_id Capability to revoke.
     * @return Void on success.
     */
    [[nodiscard]] auto capability_revoke(CapabilityId cap_id) -> VoidResult {
        if (find_cap_idx(cap_id) < 0) return std::unexpected(KernelError::invalid_capability);

        CapIdList to_remove;
        to_remove.push(cap_id);
        collect_children(cap_id, to_remove);

        // Remove matching capabilities (swap-with-last)
        for (std::uint32_t i = 0; i < num_caps_; ) {
            if (to_remove.contains(caps_[i].id)) {
                caps_[i] = caps_[num_caps_ - 1];
                --num_caps_;
            } else {
                ++i;
            }
        }

        return {};
    }

    // ─── Access ────────────────────────────────────────────────────────────

    /**
     * @brief Get a writable span into the buffer through a capability.
     * @param cap_id Capability authorizing access.
     * @return Span of bytes, or error if capability invalid or lacks write permission.
     */
    [[nodiscard]] auto write_span(CapabilityId cap_id)
        -> Result<std::span<std::byte>> {
        auto ci = find_cap_idx(cap_id);
        if (ci < 0) return std::unexpected(KernelError::invalid_capability);
        auto& cap = caps_[ci];
        if (!has_perm(cap.perm, Perm::write)) return std::unexpected(KernelError::permission_denied);

        auto bi = find_buffer_idx(cap.buffer);
        if (bi < 0) return std::unexpected(KernelError::no_memory);
        auto& buf = buffers_[bi];

        return std::span<std::byte>{buf.data.data() + cap.offset, std::min(cap.length, buf.data_len - cap.offset)};
    }

    /**
     * @brief Get a read-only span into the buffer through a capability.
     * @param cap_id Capability authorizing access.
     * @return Span of const bytes, or error if capability invalid or lacks read permission.
     */
    [[nodiscard]] auto read_span(CapabilityId cap_id) const
        -> Result<std::span<const std::byte>> {
        auto ci = find_cap_idx(cap_id);
        if (ci < 0) return std::unexpected(KernelError::invalid_capability);
        auto& cap = caps_[ci];
        if (!has_perm(cap.perm, Perm::read)) return std::unexpected(KernelError::permission_denied);

        auto bi = find_buffer_idx(cap.buffer);
        if (bi < 0) return std::unexpected(KernelError::no_memory);
        auto& buf = buffers_[bi];

        return std::span<const std::byte>{buf.data.data() + cap.offset, std::min(cap.length, buf.data_len - cap.offset)};
    }

    // ─── Queries ───────────────────────────────────────────────────────────

    /**
     * @brief Check whether a process holds a capability with required permissions.
     * @param pid Process to check.
     * @param cap_id Capability to verify.
     * @param required Required permission bits.
     * @return True if the process owns the capability with sufficient permissions.
     */
    [[nodiscard]] auto check_access(ProcessId pid, CapabilityId cap_id, Perm required) const
        -> bool {
        auto ci = find_cap_idx(cap_id);
        return ci >= 0 && caps_[ci].owner == pid && has_perm(caps_[ci].perm, required);
    }

    /// @brief Number of active capabilities.
    [[nodiscard]] auto capability_count() const -> std::size_t { return num_caps_; }

    /// @brief Number of active buffers.
    [[nodiscard]] auto buffer_count() const -> std::size_t { return num_buffers_; }

    /// @brief Get the permissions of a capability.
    [[nodiscard]] auto get_perm(CapabilityId cap_id) const -> Result<Perm> {
        auto ci = find_cap_idx(cap_id);
        if (ci < 0) return std::unexpected(KernelError::invalid_capability);
        return caps_[ci].perm;
    }

    /// @brief Get the parent of a capability (nullopt if root).
    [[nodiscard]] auto get_parent(CapabilityId cap_id) const -> Result<std::optional<CapabilityId>> {
        auto ci = find_cap_idx(cap_id);
        if (ci < 0) return std::unexpected(KernelError::invalid_capability);
        return caps_[ci].parent;
    }
};

} // namespace qnx::memory
