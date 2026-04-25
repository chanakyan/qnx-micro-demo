// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

// qnx.scheduler — fixed-priority preemptive scheduler
// 256 priority levels. O(1) dispatch via bitmask.
// Thread state machine: ready/running/blocked/dead.
// IPC calls into scheduler to block/unblock threads.

export module qnx.scheduler;
import std;
export import qnx.types;

export namespace qnx::scheduler {

// ─── Thread descriptor ─────────────────────────────────────────────────────

/** @brief Per-thread scheduling metadata tracked by the kernel. */
struct Thread {
    ThreadId    id;                  ///< Unique thread identifier
    ProcessId   pid;                ///< Owning process
    Priority    priority;           ///< Declared scheduling priority
    Priority    effective_priority; ///< Max(declared, max waiter priority) — prevents inversion
    ThreadState state;              ///< Current position in the thread state machine
};

// ─── Scheduler state ───────────────────────────────────────────────────────

/**
 * @brief Fixed-priority preemptive scheduler with 256 priority levels.
 *
 * Uses a bitmask over 256 ready queues for O(1) highest-priority lookup.
 * The IPC subsystem calls thread_block/thread_unblock to move threads
 * through the QNX Neutrino state machine (ready/running/blocked/dead).
 */
/// @brief Callback type: IPC notifies scheduler when a channel's waiter set changes.
using QueueChangeFn = std::function<void(ThreadId server, Priority max_waiter_priority)>;

class Scheduler {
    std::vector<Thread> threads_;
    std::array<std::vector<ThreadId>, 256> ready_queues_;
    std::bitset<256> ready_mask_;
    std::optional<ThreadId> current_;
    int next_tid_ = 1;

    [[nodiscard]] auto find_thread(ThreadId tid) -> Thread* {
        for (auto& t : threads_) {
            if (t.id == tid) return &t;
        }
        return nullptr;
    }

    auto enqueue_ready(ThreadId tid, Priority pri) -> void {
        ready_queues_[pri.value].push_back(tid);
        ready_mask_.set(pri.value);
    }

    auto dequeue_ready(ThreadId tid, Priority pri) -> void {
        auto& q = ready_queues_[pri.value];
        std::erase(q, tid);
        if (q.empty()) ready_mask_.reset(pri.value);
    }

public:
    // ─── Thread lifecycle ──────────────────────────────────────────────────

    /**
     * @brief Create a new thread and place it on the ready queue.
     * @param pid Owning process.
     * @param pri Initial scheduling priority.
     * @return ThreadId of the newly created thread.
     */
    [[nodiscard]] auto thread_create(ProcessId pid, Priority pri) -> Result<ThreadId> {
        auto tid = ThreadId{next_tid_++};
        threads_.push_back(Thread{
            .id = tid, .pid = pid, .priority = pri,
            .effective_priority = pri, .state = ThreadState::ready
        });
        enqueue_ready(tid, pri);
        return tid;
    }

    /**
     * @brief Terminate a thread and remove it from all scheduling structures.
     * @param tid Thread to destroy.
     * @return Void on success, KernelError::invalid_thread if not found.
     */
    [[nodiscard]] auto thread_destroy(ThreadId tid) -> VoidResult {
        auto* t = find_thread(tid);
        if (!t) return std::unexpected(KernelError::invalid_thread);
        if (t->state == ThreadState::ready) dequeue_ready(tid, t->effective_priority);
        if (current_ == tid) current_ = std::nullopt;
        t->state = ThreadState::dead;
        return {};
    }

    // ─── State transitions (called by IPC) ─────────────────────────────────

    /**
     * @brief Transition a thread from ready/running to a blocked state.
     *
     * Called by IPC when a thread enters MsgSend, MsgReceive, or awaits MsgReply.
     * @param tid Thread to block.
     * @param reason Target blocked state (send_blocked, receive_blocked, reply_blocked).
     * @return Void on success, error if thread is invalid or dead.
     */
    [[nodiscard]] auto thread_block(ThreadId tid, ThreadState reason) -> VoidResult {
        auto* t = find_thread(tid);
        if (!t) return std::unexpected(KernelError::invalid_thread);
        if (t->state == ThreadState::dead) return std::unexpected(KernelError::dead_thread);
        if (t->state == ThreadState::ready) dequeue_ready(tid, t->effective_priority);
        if (current_ == tid) current_ = std::nullopt;
        t->state = reason;
        return {};
    }

    /**
     * @brief Move a blocked thread back to the ready queue.
     *
     * Called by IPC when a message arrives or a reply completes.
     * @param tid Thread to unblock.
     * @return Void on success, error if thread is invalid or dead.
     */
    [[nodiscard]] auto thread_unblock(ThreadId tid) -> VoidResult {
        auto* t = find_thread(tid);
        if (!t) return std::unexpected(KernelError::invalid_thread);
        if (t->state == ThreadState::dead) return std::unexpected(KernelError::dead_thread);
        t->state = ThreadState::ready;
        enqueue_ready(tid, t->effective_priority);
        return {};
    }

    // ─── Dispatch ──────────────────────────────────────────────────────────

    /**
     * @brief Select the highest-priority ready thread and mark it running.
     * @return ThreadId of the dispatched thread, or nullopt if no threads are ready.
     */
    [[nodiscard]] auto schedule_next() -> std::optional<ThreadId> {
        if (ready_mask_.none()) return std::nullopt;
        // Find highest set bit (highest priority with ready threads)
        for (int p = max_priority; p >= 0; --p) {
            if (ready_mask_.test(p) && !ready_queues_[p].empty()) {
                auto tid = ready_queues_[p].front();
                ready_queues_[p].erase(ready_queues_[p].begin());
                if (ready_queues_[p].empty()) ready_mask_.reset(p);

                auto* t = find_thread(tid);
                if (t) t->state = ThreadState::running;
                current_ = tid;
                return tid;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Timer tick: preempt current thread if a higher-priority thread is ready.
     *
     * If no thread is running, dispatches the next ready thread.
     * If a higher-priority thread exists, the current thread returns to ready.
     */
    auto tick() -> void {
        // Preemption: if a higher-priority thread is ready, preempt current
        if (!current_) { (void)schedule_next(); return; }

        auto* curr = find_thread(*current_);
        if (!curr || curr->state != ThreadState::running) {
            current_ = std::nullopt;
            (void)schedule_next();
            return;
        }

        if (ready_mask_.none()) return;

        for (int p = max_priority; p > curr->effective_priority.value; --p) {
            if (ready_mask_.test(p) && !ready_queues_[p].empty()) {
                // Preempt: current goes back to ready
                curr->state = ThreadState::ready;
                enqueue_ready(curr->id, curr->effective_priority);
                current_ = std::nullopt;
                (void)schedule_next();
                return;
            }
        }
    }

    // ─── Queries ───────────────────────────────────────────────────────────

    /**
     * @brief Query the current state of a thread.
     * @param tid Thread to query.
     * @return ThreadState on success, KernelError::invalid_thread if not found.
     */
    [[nodiscard]] auto get_state(ThreadId tid) const -> Result<ThreadState> {
        for (const auto& t : threads_) {
            if (t.id == tid) return t.state;
        }
        return std::unexpected(KernelError::invalid_thread);
    }

    /**
     * @brief Get the currently running thread.
     * @return ThreadId of the running thread, or nullopt if CPU is idle.
     */
    [[nodiscard]] auto get_current() const -> std::optional<ThreadId> {
        return current_;
    }

    /**
     * @brief Query a thread's declared scheduling priority.
     * @param tid Thread to query.
     * @return Priority on success, KernelError::invalid_thread if not found.
     */
    [[nodiscard]] auto get_priority(ThreadId tid) const -> Result<Priority> {
        for (const auto& t : threads_) {
            if (t.id == tid) return t.priority;
        }
        return std::unexpected(KernelError::invalid_thread);
    }

    /**
     * @brief Query a thread's effective priority (may be boosted by waiters).
     * @param tid Thread to query.
     * @return Effective priority on success, error if not found.
     */
    [[nodiscard]] auto get_effective_priority(ThreadId tid) const -> Result<Priority> {
        for (const auto& t : threads_) {
            if (t.id == tid) return t.effective_priority;
        }
        return std::unexpected(KernelError::invalid_thread);
    }

    /**
     * @brief IPC callback: a channel's waiter set changed, update server priority.
     *
     * Called by IPC on MsgSend (new waiter) and MsgReply (waiter leaves).
     * Server effective_priority = max(declared, max_waiter_priority).
     * If the server is in the ready queue, it is repositioned.
     * @param server_tid Server thread whose effective priority may change.
     * @param max_waiter_priority Highest priority among current waiters (or 0 if none).
     * @return Void on success, error if server not found.
     */
    [[nodiscard]] auto on_queue_change(ThreadId server_tid, Priority max_waiter_priority) -> VoidResult {
        auto* t = find_thread(server_tid);
        if (!t) return std::unexpected(KernelError::invalid_thread);

        auto old_eff = t->effective_priority;
        auto new_eff = Priority{std::max(t->priority.value, max_waiter_priority.value)};
        t->effective_priority = new_eff;

        // Reposition in ready queue if priority changed and thread is ready
        if (t->state == ThreadState::ready && old_eff != new_eff) {
            dequeue_ready(t->id, old_eff);
            enqueue_ready(t->id, new_eff);
        }

        return {};
    }

    /**
     * @brief Count of live (non-dead) threads in the system.
     * @return Number of threads not in the dead state.
     */
    [[nodiscard]] auto thread_count() const -> std::size_t {
        return std::ranges::count_if(threads_, [](const Thread& t) {
            return t.state != ThreadState::dead;
        });
    }
};

} // namespace qnx::scheduler
