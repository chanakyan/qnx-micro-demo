// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

// qnx.pps — Persistent Publish/Subscribe
// QNX PPS model: objects in a /pps/ namespace, key-value attributes,
// subscribers get notified on change. No polling. Persistent across restarts.
//
// Publisher writes: pps::publish("/pps/system/battery", "level", "87")
// Subscriber gets:  callback fires with {path, key, value}

export module qnx.pps;
import std;
export import qnx.types;

export namespace qnx::pps {

// ─── PPS types ─────────────────────────────────────────────────────────────

/** @brief Unique PPS object identifier. */
struct ObjectId {
    int value;  ///< Raw numeric object handle
    constexpr explicit ObjectId(int v) : value{v} {}
    constexpr auto operator<=>(const ObjectId&) const = default;
};

/** @brief Unique subscription handle for change notifications. */
struct SubscriptionId {
    int value;  ///< Raw numeric subscription handle
    constexpr explicit SubscriptionId(int v) : value{v} {}
    constexpr auto operator<=>(const SubscriptionId&) const = default;
};

/** @brief A single key-value attribute within a PPS object. */
struct Attribute {
    std::string key;    ///< Attribute name
    std::string value;  ///< Attribute value (string-encoded)
};

/** @brief Change notification delivered to PPS subscribers. */
struct Notification {
    std::string path;   ///< PPS object path that changed
    std::string key;    ///< Attribute key that was affected
    std::string value;  ///< New value (or last value on deletion)
    /** @brief Type of change that triggered this notification. */
    enum class Kind {
        created,   ///< New attribute added to the object
        modified,  ///< Existing attribute value changed
        deleted    ///< Attribute or object removed
    } kind;  ///< What kind of change occurred
};

/// @brief Callback signature for PPS change notifications.
/// @brief Subscriber callback type.
/// TODO: replace with std::move_only_function when libc++ ships it (P0288R9).
using NotifyFn = std::function<void(const Notification&)>;

// ─── PPS object ────────────────────────────────────────────────────────────

/** @brief A named PPS object containing key-value attributes. */
struct Object {
    ObjectId    id;          ///< Unique object handle
    std::string path;        ///< Fully qualified PPS path (e.g. "/pps/system/battery")
    std::vector<Attribute> attrs;  ///< Current attribute set
    bool persistent;         ///< If true, survives restart via backing store
};

// ─── Subscription ──────────────────────────────────────────────────────────

/** @brief A subscription to change notifications under a PPS path prefix. */
struct Subscription {
    SubscriptionId id;              ///< Unique subscription handle
    std::string    path_prefix;     ///< Path prefix filter (e.g. "/pps/system/")
    ProcessId      subscriber;      ///< Process that registered this subscription
    NotifyFn       callback;        ///< Invoked on each matching change
};

// ─── PPS subsystem ─────────────────────────────────────────────────────────

/**
 * @brief QNX Persistent Publish/Subscribe (PPS) subsystem.
 *
 * Objects live in a /pps/ namespace with key-value attributes. Publishers
 * write attributes; subscribers receive callbacks on change. Objects
 * marked persistent survive process restarts.
 */
class Pps {
    std::vector<Object>       objects_;
    std::vector<Subscription> subscriptions_;
    int next_obj_id_ = 1;
    int next_sub_id_ = 1;

    [[nodiscard]] auto find_object(std::string_view path) -> Object* {
        for (auto& o : objects_) {
            if (o.path == path) return &o;
        }
        return nullptr;
    }

    auto notify(const Notification& n) -> void {
        for (const auto& sub : subscriptions_) {
            if (n.path.starts_with(sub.path_prefix) || n.path == sub.path_prefix) {
                sub.callback(n);
            }
        }
    }

public:
    // ─── Object lifecycle ──────────────────────────────────────────────────

    /**
     * @brief Create a new PPS object at the given path.
     * @param path Fully qualified PPS path (e.g. "/pps/system/battery").
     * @param persistent If true, object survives process restarts.
     * @return ObjectId on success, KernelError::already_exists if path taken.
     */
    [[nodiscard]] auto create_object(std::string_view path, bool persistent = true)
        -> Result<ObjectId> {
        if (find_object(path)) return std::unexpected(KernelError::already_exists);
        auto id = ObjectId{next_obj_id_++};
        objects_.push_back(Object{
            .id = id, .path = std::string(path),
            .attrs = {}, .persistent = persistent
        });
        return id;
    }

    /**
     * @brief Delete a PPS object and notify subscribers of each attribute removal.
     * @param path PPS object path to delete.
     * @return Void on success, KernelError::not_found if object does not exist.
     */
    [[nodiscard]] auto delete_object(std::string_view path) -> VoidResult {
        auto it = std::ranges::find_if(objects_, [&](const Object& o) {
            return o.path == path;
        });
        if (it == objects_.end()) return std::unexpected(KernelError::not_found);

        // Notify subscribers of deletion
        for (const auto& attr : it->attrs) {
            notify(Notification{
                .path = it->path, .key = attr.key, .value = attr.value,
                .kind = Notification::Kind::deleted
            });
        }

        objects_.erase(it);
        return {};
    }

    // ─── Publish (write attribute) ─────────────────────────────────────────

    /**
     * @brief Write an attribute to a PPS object (auto-creates the object if needed).
     *
     * If the attribute already exists, its value is updated (modified notification).
     * If the attribute is new, it is created (created notification).
     * @param path PPS object path.
     * @param key Attribute name.
     * @param value Attribute value.
     * @return Void on success.
     */
    [[nodiscard]] auto publish(std::string_view path, std::string_view key,
                                std::string_view value) -> VoidResult {
        auto* obj = find_object(path);
        if (!obj) {
            // Auto-create object on first publish
            auto r = create_object(path);
            if (!r) return std::unexpected(r.error());
            obj = find_object(path);
        }

        // Find existing attribute or create new
        auto it = std::ranges::find_if(obj->attrs, [&](const Attribute& a) {
            return a.key == key;
        });

        auto kind = Notification::Kind::modified;
        if (it != obj->attrs.end()) {
            it->value = std::string(value);
        } else {
            obj->attrs.push_back(Attribute{
                .key = std::string(key), .value = std::string(value)
            });
            kind = Notification::Kind::created;
        }

        notify(Notification{
            .path = std::string(path), .key = std::string(key),
            .value = std::string(value), .kind = kind
        });

        return {};
    }

    // ─── Read attribute ────────────────────────────────────────────────────

    /**
     * @brief Read a single attribute value from a PPS object.
     * @param path PPS object path.
     * @param key Attribute name to read.
     * @return String view of the attribute value, or KernelError::not_found.
     */
    [[nodiscard]] auto read(std::string_view path, std::string_view key) const
        -> Result<std::string_view> {
        for (const auto& o : objects_) {
            if (o.path == path) {
                for (const auto& a : o.attrs) {
                    if (a.key == key) return std::string_view{a.value};
                }
                return std::unexpected(KernelError::not_found);
            }
        }
        return std::unexpected(KernelError::not_found);
    }

    // ─── Read all attributes of an object ──────────────────────────────────

    /**
     * @brief Read all attributes of a PPS object.
     * @param path PPS object path.
     * @return Span of attributes, or KernelError::not_found.
     */
    [[nodiscard]] auto read_object(std::string_view path) const
        -> Result<std::span<const Attribute>> {
        for (const auto& o : objects_) {
            if (o.path == path) return std::span<const Attribute>{o.attrs};
        }
        return std::unexpected(KernelError::not_found);
    }

    // ─── Subscribe ─────────────────────────────────────────────────────────

    /**
     * @brief Subscribe to change notifications for objects under a path prefix.
     * @param path_prefix Path prefix filter (e.g. "/pps/system/").
     * @param pid Subscribing process.
     * @param callback Function invoked on each matching change notification.
     * @return SubscriptionId on success.
     */
    [[nodiscard]] auto subscribe(std::string_view path_prefix, ProcessId pid,
                                  NotifyFn callback) -> Result<SubscriptionId> {
        auto id = SubscriptionId{next_sub_id_++};
        subscriptions_.push_back(Subscription{
            .id = id, .path_prefix = std::string(path_prefix),
            .subscriber = pid, .callback = std::move(callback)
        });
        return id;
    }

    /**
     * @brief Remove a subscription.
     * @param id Subscription to cancel.
     * @return Void on success, KernelError::not_found if subscription does not exist.
     */
    [[nodiscard]] auto unsubscribe(SubscriptionId id) -> VoidResult {
        auto it = std::ranges::find_if(subscriptions_, [&](const Subscription& s) {
            return s.id == id;
        });
        if (it == subscriptions_.end()) return std::unexpected(KernelError::not_found);
        subscriptions_.erase(it);
        return {};
    }

    // ─── Queries ───────────────────────────────────────────────────────────

    /**
     * @brief Number of PPS objects in the system.
     * @return Object count.
     */
    [[nodiscard]] auto object_count() const -> std::size_t { return objects_.size(); }

    /**
     * @brief Number of active subscriptions.
     * @return Subscription count.
     */
    [[nodiscard]] auto subscription_count() const -> std::size_t { return subscriptions_.size(); }

    /**
     * @brief List all PPS object paths under a given prefix.
     * @param prefix Path prefix to filter by.
     * @return Vector of matching object paths.
     */
    [[nodiscard]] auto list(std::string_view prefix) const -> std::vector<std::string_view> {
        std::vector<std::string_view> result;
        for (const auto& o : objects_) {
            if (std::string_view{o.path}.starts_with(prefix)) {
                result.push_back(o.path);
            }
        }
        return result;
    }
};

} // namespace qnx::pps
