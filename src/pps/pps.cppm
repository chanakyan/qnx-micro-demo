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
    int value = -1;  ///< Raw numeric object handle
    constexpr ObjectId() = default;
    constexpr explicit ObjectId(int v) : value{v} {}
    constexpr auto operator<=>(const ObjectId&) const = default;
};

/** @brief Unique subscription handle for change notifications. */
struct SubscriptionId {
    int value = -1;  ///< Raw numeric subscription handle
    constexpr SubscriptionId() = default;
    constexpr explicit SubscriptionId(int v) : value{v} {}
    constexpr auto operator<=>(const SubscriptionId&) const = default;
};

/// @brief Fixed-capacity string type for kernel use (no heap).
template<std::size_t N>
struct FixedString {
    std::array<char, N> storage = {};
    std::size_t len = 0;

    constexpr FixedString() = default;

    FixedString(std::string_view sv) : len{std::min(sv.size(), N - 1)} {
        std::copy_n(sv.data(), len, storage.data());
        storage[len] = '\0';
    }

    [[nodiscard]] auto view() const -> std::string_view { return {storage.data(), len}; }
    [[nodiscard]] auto size() const -> std::size_t { return len; }
    [[nodiscard]] auto c_str() const -> const char* { return storage.data(); }

    /// @brief Implicit conversion to string_view.
    operator std::string_view() const { return view(); }

    auto operator==(std::string_view sv) const -> bool { return view() == sv; }
    auto operator==(const FixedString& o) const -> bool { return view() == o.view(); }

    /// @brief Check if this string starts with a prefix.
    [[nodiscard]] auto starts_with(std::string_view prefix) const -> bool {
        return view().starts_with(prefix);
    }
};

/** @brief A single key-value attribute within a PPS object. */
struct Attribute {
    FixedString<max_name_len>      key;    ///< Attribute name
    FixedString<max_pps_value_len> value;  ///< Attribute value (string-encoded)
};

/** @brief Change notification delivered to PPS subscribers. */
struct Notification {
    FixedString<max_path_len>      path;   ///< PPS object path that changed
    FixedString<max_name_len>      key;    ///< Attribute key that was affected
    FixedString<max_pps_value_len> value;  ///< New value (or last value on deletion)
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
    FixedString<max_path_len> path;  ///< Fully qualified PPS path (e.g. "/pps/system/battery")
    std::array<Attribute, max_pps_attrs> attrs = {};  ///< Current attribute set
    std::uint32_t num_attrs = 0;  ///< Number of active attributes
    bool persistent = false;       ///< If true, survives restart via backing store
};

// ─── Subscription ──────────────────────────────────────────────────────────

/** @brief A subscription to change notifications under a PPS path prefix. */
struct Subscription {
    SubscriptionId id;                        ///< Unique subscription handle
    FixedString<max_path_len> path_prefix;    ///< Path prefix filter (e.g. "/pps/system/")
    ProcessId      subscriber;                ///< Process that registered this subscription
    NotifyFn       callback;                  ///< Invoked on each matching change
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
    std::array<Object, max_pps_objects>         objects_ = {};
    std::uint32_t num_objects_ = 0;
    std::array<Subscription, max_subscriptions> subscriptions_ = {};
    std::uint32_t num_subscriptions_ = 0;
    int next_obj_id_ = 1;
    int next_sub_id_ = 1;

    [[nodiscard]] auto find_object_idx(std::string_view path) -> int {
        for (std::uint32_t i = 0; i < num_objects_; ++i) {
            if (objects_[i].path == path) return static_cast<int>(i);
        }
        return -1;
    }

    auto notify(const Notification& n) -> void {
        auto s = std::span{subscriptions_.data(), num_subscriptions_};
        for (const auto& sub : s) {
            auto npath = n.path.view();
            auto prefix = sub.path_prefix.view();
            if (npath.starts_with(prefix) || npath == prefix) {
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
        if (find_object_idx(path) >= 0) return std::unexpected(KernelError::already_exists);
        if (num_objects_ >= max_pps_objects) return std::unexpected(KernelError::no_memory);
        auto id = ObjectId{next_obj_id_++};
        auto& obj = objects_[num_objects_++];
        obj.id = id;
        obj.path = FixedString<max_path_len>{path};
        obj.num_attrs = 0;
        obj.persistent = persistent;
        return id;
    }

    /**
     * @brief Delete a PPS object and notify subscribers of each attribute removal.
     * @param path PPS object path to delete.
     * @return Void on success, KernelError::not_found if object does not exist.
     */
    [[nodiscard]] auto delete_object(std::string_view path) -> VoidResult {
        auto s = std::span{objects_.data(), num_objects_};
        Object* found = nullptr;
        std::uint32_t found_idx = 0;
        for (std::uint32_t i = 0; i < num_objects_; ++i) {
            if (objects_[i].path == path) {
                found = &objects_[i];
                found_idx = i;
                break;
            }
        }
        if (!found) return std::unexpected(KernelError::not_found);

        // Notify subscribers of deletion
        auto attrs = std::span{found->attrs.data(), found->num_attrs};
        for (const auto& attr : attrs) {
            Notification n;
            n.path = found->path;
            n.key = attr.key;
            n.value = attr.value;
            n.kind = Notification::Kind::deleted;
            notify(n);
        }

        // Swap with last, decrement
        objects_[found_idx] = objects_[num_objects_ - 1];
        --num_objects_;
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
        auto oi = find_object_idx(path);
        if (oi < 0) {
            // Auto-create object on first publish
            auto r = create_object(path);
            if (!r) return std::unexpected(r.error());
            oi = find_object_idx(path);
        }
        auto& obj = objects_[oi];

        // Find existing attribute or create new
        auto attr_span = std::span{obj.attrs.data(), obj.num_attrs};
        Attribute* found_attr = nullptr;
        for (auto& a : attr_span) {
            if (a.key == key) { found_attr = &a; break; }
        }

        auto kind = Notification::Kind::modified;
        if (found_attr) {
            found_attr->value = FixedString<max_pps_value_len>{value};
        } else {
            if (obj.num_attrs >= max_pps_attrs) return std::unexpected(KernelError::no_memory);
            auto& a = obj.attrs[obj.num_attrs++];
            a.key = FixedString<max_name_len>{key};
            a.value = FixedString<max_pps_value_len>{value};
            kind = Notification::Kind::created;
        }

        Notification n;
        n.path = FixedString<max_path_len>{path};
        n.key = FixedString<max_name_len>{key};
        n.value = FixedString<max_pps_value_len>{value};
        n.kind = kind;
        notify(n);

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
        auto s = std::span{objects_.data(), num_objects_};
        for (const auto& o : s) {
            if (o.path == path) {
                auto attrs = std::span{o.attrs.data(), o.num_attrs};
                for (const auto& a : attrs) {
                    if (a.key == key) return a.value.view();
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
        auto s = std::span{objects_.data(), num_objects_};
        for (const auto& o : s) {
            if (o.path == path) return std::span<const Attribute>{o.attrs.data(), o.num_attrs};
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
        if (num_subscriptions_ >= max_subscriptions) return std::unexpected(KernelError::no_memory);
        auto id = SubscriptionId{next_sub_id_++};
        auto& sub = subscriptions_[num_subscriptions_++];
        sub.id = id;
        sub.path_prefix = FixedString<max_path_len>{path_prefix};
        sub.subscriber = pid;
        sub.callback = std::move(callback);
        return id;
    }

    /**
     * @brief Remove a subscription.
     * @param id Subscription to cancel.
     * @return Void on success, KernelError::not_found if subscription does not exist.
     */
    [[nodiscard]] auto unsubscribe(SubscriptionId id) -> VoidResult {
        for (std::uint32_t i = 0; i < num_subscriptions_; ++i) {
            if (subscriptions_[i].id == id) {
                subscriptions_[i] = std::move(subscriptions_[num_subscriptions_ - 1]);
                --num_subscriptions_;
                return {};
            }
        }
        return std::unexpected(KernelError::not_found);
    }

    // ─── Queries ───────────────────────────────────────────────────────────

    /**
     * @brief Number of PPS objects in the system.
     * @return Object count.
     */
    [[nodiscard]] auto object_count() const -> std::size_t { return num_objects_; }

    /**
     * @brief Number of active subscriptions.
     * @return Subscription count.
     */
    [[nodiscard]] auto subscription_count() const -> std::size_t { return num_subscriptions_; }

    /// @brief Fixed-capacity list of path views returned by list().
    struct PathList {
        std::array<std::string_view, max_pps_objects> paths = {};
        std::uint32_t count = 0;
        [[nodiscard]] auto size() const -> std::size_t { return count; }
        [[nodiscard]] auto begin() const { return paths.data(); }
        [[nodiscard]] auto end() const { return paths.data() + count; }
    };

    /**
     * @brief List all PPS object paths under a given prefix.
     * @param prefix Path prefix to filter by.
     * @return PathList of matching object paths.
     */
    [[nodiscard]] auto list(std::string_view prefix) const -> PathList {
        PathList result;
        auto s = std::span{objects_.data(), num_objects_};
        for (const auto& o : s) {
            if (o.path.view().starts_with(prefix) && result.count < max_pps_objects) {
                result.paths[result.count++] = o.path.view();
            }
        }
        return result;
    }
};

} // namespace qnx::pps
