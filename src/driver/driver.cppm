// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

// qnx.driver — user-space resource manager framework
// QNX model: drivers register a path prefix (/dev/ser1),
// receive messages via IPC. open() resolves path to connection.

export module qnx.driver;
import std;
export import qnx.types;
import qnx.ipc;

export namespace qnx::driver {

/** @brief A registered resource manager binding a pathname to an IPC channel. */
struct ResourceManager {
    std::string   path;     ///< Pathname prefix this resource manager handles (e.g. "/dev/ser1")
    ChannelId     channel;  ///< IPC channel where this manager receives messages
    ProcessId     pid;      ///< Process hosting this resource manager
};

/**
 * @brief QNX pathname-space namespace for user-space resource managers.
 *
 * Drivers register a path prefix and an IPC channel. When a client opens
 * a path, the namespace resolves it via longest-prefix match to the
 * appropriate channel, then creates a connection for the client.
 */
class Namespace {
    std::vector<ResourceManager> entries_;
    ipc::Ipc* ipc_;

public:
    /**
     * @brief Construct the namespace with a reference to the IPC subsystem.
     * @param ipc IPC subsystem used to create connections on open().
     */
    explicit Namespace(ipc::Ipc& ipc) : ipc_{&ipc} {}

    /**
     * @brief Register a resource manager at the given pathname.
     * @param path Pathname prefix to claim (e.g. "/dev/ser1").
     * @param ch IPC channel where the manager receives messages.
     * @param pid Process hosting the resource manager.
     * @return Void on success, KernelError::already_exists if path already registered.
     */
    [[nodiscard]] auto register_resource(std::string_view path, ChannelId ch, ProcessId pid)
        -> VoidResult {
        // Check for duplicate
        for (const auto& e : entries_) {
            if (e.path == path) return std::unexpected(KernelError::already_exists);
        }
        entries_.push_back(ResourceManager{
            .path = std::string(path), .channel = ch, .pid = pid
        });
        return {};
    }

    /**
     * @brief Remove a resource manager registration.
     * @param path Pathname prefix to unregister.
     * @return Void on success, KernelError::not_found if path not registered.
     */
    [[nodiscard]] auto unregister_resource(std::string_view path) -> VoidResult {
        auto it = std::ranges::find_if(entries_, [&](const ResourceManager& rm) {
            return rm.path == path;
        });
        if (it == entries_.end()) return std::unexpected(KernelError::not_found);
        entries_.erase(it);
        return {};
    }

    /**
     * @brief Resolve a pathname to the IPC channel of its resource manager.
     *
     * Uses longest-prefix matching: "/dev/ser1/control" matches "/dev/ser1"
     * over "/dev".
     * @param path Pathname to resolve.
     * @return ChannelId of the best-matching resource manager, or KernelError::not_found.
     */
    [[nodiscard]] auto resolve(std::string_view path) const
        -> Result<ChannelId> {
        // Longest prefix match
        const ResourceManager* best = nullptr;
        for (const auto& e : entries_) {
            if (path.starts_with(e.path)) {
                if (!best || e.path.size() > best->path.size()) {
                    best = &e;
                }
            }
        }
        if (!best) return std::unexpected(KernelError::not_found);
        return best->channel;
    }

    /**
     * @brief Open a pathname: resolve to a resource manager and create a connection.
     * @param path Pathname to open.
     * @param client_pid Process performing the open.
     * @return ConnectionId to use for IPC with the resource manager.
     */
    [[nodiscard]] auto open(std::string_view path, ProcessId client_pid)
        -> Result<ConnectionId> {
        auto ch = resolve(path);
        if (!ch) return std::unexpected(ch.error());
        return ipc_->connect_attach(*ch, client_pid);
    }

    /**
     * @brief Number of registered resource managers.
     * @return Entry count.
     */
    [[nodiscard]] auto count() const -> std::size_t { return entries_.size(); }
};

} // namespace qnx::driver
