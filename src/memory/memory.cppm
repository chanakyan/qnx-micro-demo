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
    int value;
    constexpr explicit BufferId(int v) : value{v} {}
    constexpr auto operator<=>(const BufferId&) const = default;
};

/// @brief Actual memory buffer with reference counting.
struct SharedBuffer {
    BufferId              id;      ///< Unique buffer identity
    std::vector<std::byte> data;   ///< The actual bytes
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
    std::vector<SharedBuffer> buffers_;
    std::vector<Capability>   caps_;
    int next_cap_id_ = 1;
    int next_buf_id_ = 1;

    [[nodiscard]] auto find_cap(CapabilityId id) -> Capability* {
        for (auto& c : caps_) {
            if (c.id == id) return &c;
        }
        return nullptr;
    }

    [[nodiscard]] auto find_cap(CapabilityId id) const -> const Capability* {
        for (const auto& c : caps_) {
            if (c.id == id) return &c;
        }
        return nullptr;
    }

    [[nodiscard]] auto find_buffer(BufferId id) -> SharedBuffer* {
        for (auto& b : buffers_) {
            if (b.id == id) return &b;
        }
        return nullptr;
    }

    [[nodiscard]] auto find_buffer(BufferId id) const -> const SharedBuffer* {
        for (const auto& b : buffers_) {
            if (b.id == id) return &b;
        }
        return nullptr;
    }

    /// @brief Collect all capability IDs that are children (direct or transitive) of a parent.
    auto collect_children(CapabilityId parent_id, std::vector<CapabilityId>& out) const -> void {
        for (const auto& c : caps_) {
            if (c.parent == parent_id) {
                out.push_back(c.id);
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
        auto buf_id = BufferId{next_buf_id_++};
        buffers_.push_back(SharedBuffer{
            .id = buf_id, .data = std::vector<std::byte>(length, std::byte{0})
        });

        auto cap_id = CapabilityId{next_cap_id_++};
        caps_.push_back(Capability{
            .id = cap_id, .owner = pid, .buffer = buf_id,
            .offset = 0, .length = length, .perm = perm,
            .parent = std::nullopt
        });

        return cap_id;
    }

    /**
     * @brief Release a capability and its buffer if no other capabilities reference it.
     * @param cap_id Capability to release.
     * @return Void on success.
     */
    [[nodiscard]] auto munmap(CapabilityId cap_id) -> VoidResult {
        auto* cap = find_cap(cap_id);
        if (!cap) return std::unexpected(KernelError::invalid_capability);

        auto buf_id = cap->buffer;

        // Revoke this and all children
        std::vector<CapabilityId> to_remove;
        to_remove.push_back(cap_id);
        collect_children(cap_id, to_remove);

        std::erase_if(caps_, [&](const Capability& c) {
            return std::ranges::find(to_remove, c.id) != to_remove.end();
        });

        // If no capabilities reference this buffer, free it
        bool still_referenced = std::ranges::any_of(caps_, [&](const Capability& c) {
            return c.buffer == buf_id;
        });
        if (!still_referenced) {
            std::erase_if(buffers_, [&](const SharedBuffer& b) {
                return b.id == buf_id;
            });
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
        auto* parent = find_cap(cap_id);
        if (!parent) return std::unexpected(KernelError::invalid_capability);

        // Default: same length as parent
        if (sub_length == 0) sub_length = parent->length - sub_offset;

        // Sub-region must fit within parent
        if (sub_offset + sub_length > parent->length) {
            return std::unexpected(KernelError::permission_denied);
        }

        auto new_id = CapabilityId{next_cap_id_++};
        caps_.push_back(Capability{
            .id = new_id, .owner = target, .buffer = parent->buffer,
            .offset = parent->offset + sub_offset,
            .length = sub_length,
            .perm = parent->perm & restricted,  // never escalate
            .parent = cap_id
        });

        return new_id;
    }

    /**
     * @brief Revoke a capability and all capabilities derived from it.
     * @param cap_id Capability to revoke.
     * @return Void on success.
     */
    [[nodiscard]] auto capability_revoke(CapabilityId cap_id) -> VoidResult {
        if (!find_cap(cap_id)) return std::unexpected(KernelError::invalid_capability);

        std::vector<CapabilityId> to_remove;
        to_remove.push_back(cap_id);
        collect_children(cap_id, to_remove);

        std::erase_if(caps_, [&](const Capability& c) {
            return std::ranges::find(to_remove, c.id) != to_remove.end();
        });

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
        auto* cap = find_cap(cap_id);
        if (!cap) return std::unexpected(KernelError::invalid_capability);
        if (!has_perm(cap->perm, Perm::write)) return std::unexpected(KernelError::permission_denied);

        auto* buf = find_buffer(cap->buffer);
        if (!buf) return std::unexpected(KernelError::no_memory);

        return std::span<std::byte>{buf->data.data() + cap->offset, cap->length};
    }

    /**
     * @brief Get a read-only span into the buffer through a capability.
     * @param cap_id Capability authorizing access.
     * @return Span of const bytes, or error if capability invalid or lacks read permission.
     */
    [[nodiscard]] auto read_span(CapabilityId cap_id) const
        -> Result<std::span<const std::byte>> {
        auto* cap = find_cap(cap_id);
        if (!cap) return std::unexpected(KernelError::invalid_capability);
        if (!has_perm(cap->perm, Perm::read)) return std::unexpected(KernelError::permission_denied);

        auto* buf = find_buffer(cap->buffer);
        if (!buf) return std::unexpected(KernelError::no_memory);

        return std::span<const std::byte>{buf->data.data() + cap->offset, cap->length};
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
        auto* cap = find_cap(cap_id);
        return cap && cap->owner == pid && has_perm(cap->perm, required);
    }

    /// @brief Number of active capabilities.
    [[nodiscard]] auto capability_count() const -> std::size_t { return caps_.size(); }

    /// @brief Number of active buffers.
    [[nodiscard]] auto buffer_count() const -> std::size_t { return buffers_.size(); }

    /// @brief Get the permissions of a capability.
    [[nodiscard]] auto get_perm(CapabilityId cap_id) const -> Result<Perm> {
        auto* cap = find_cap(cap_id);
        if (!cap) return std::unexpected(KernelError::invalid_capability);
        return cap->perm;
    }

    /// @brief Get the parent of a capability (nullopt if root).
    [[nodiscard]] auto get_parent(CapabilityId cap_id) const -> Result<std::optional<CapabilityId>> {
        auto* cap = find_cap(cap_id);
        if (!cap) return std::unexpected(KernelError::invalid_capability);
        return cap->parent;
    }
};

} // namespace qnx::memory
