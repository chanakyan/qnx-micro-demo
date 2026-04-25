// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

// qnx.kernel — microkernel core
// Bootstrap, subsystem ownership, syscall dispatch.
// Everything is a message. The kernel just routes them.

export module qnx.kernel;
import std;
export import qnx.types;
export import qnx.scheduler;
export import qnx.ipc;
export import qnx.memory;
export import qnx.driver;
export import qnx.pps;

export namespace qnx::kernel {

/**
 * @brief QNX Neutrino-style microkernel core.
 *
 * Owns all subsystems (scheduler, IPC, memory, driver namespace, PPS)
 * and provides the bootstrap sequence. The kernel itself is minimal --
 * all services run as user-space resource managers communicating via IPC.
 */
class Kernel {
    scheduler::Scheduler sched_;
    ipc::Ipc             ipc_;
    memory::MemoryManager mem_;
    driver::Namespace    ns_;
    pps::Pps             pps_;
    bool initialized_ = false;

public:
    /** @brief Construct the kernel and wire subsystem dependencies. */
    Kernel() : ipc_{sched_}, ns_{ipc_} {}

    /**
     * @brief Initialize the kernel: create idle thread and begin scheduling.
     *
     * Must be called before any other kernel operation. Creates the idle
     * thread at priority 0 and performs the first schedule dispatch.
     */
    auto init() -> void {
        // Create idle thread at priority 0
        auto idle = sched_.thread_create(ProcessId{0}, Priority{idle_priority});
        if (idle) (void)sched_.schedule_next();
        initialized_ = true;
    }

    // ─── Accessors (subsystem references) ──────────────────────────────────

    /**
     * @brief Access the scheduler subsystem.
     * @return Reference to the scheduler.
     */
    [[nodiscard]] auto scheduler() -> scheduler::Scheduler& { return sched_; }

    /**
     * @brief Access the IPC subsystem.
     * @return Reference to the IPC manager.
     */
    [[nodiscard]] auto ipc()       -> ipc::Ipc&             { return ipc_; }

    /**
     * @brief Access the memory management subsystem.
     * @return Reference to the memory manager.
     */
    [[nodiscard]] auto memory()    -> memory::MemoryManager& { return mem_; }

    /**
     * @brief Access the driver/resource manager namespace.
     * @return Reference to the pathname namespace.
     */
    [[nodiscard]] auto ns()        -> driver::Namespace&     { return ns_; }

    /**
     * @brief Access the PPS subsystem.
     * @return Reference to the PPS manager.
     */
    [[nodiscard]] auto pps()       -> pps::Pps&              { return pps_; }

    /**
     * @brief Check whether the kernel has been initialized.
     * @return True if init() has been called.
     */
    [[nodiscard]] auto is_initialized() const -> bool { return initialized_; }

    /**
     * @brief Advance the scheduler by one timer tick (preemption check).
     */
    auto tick() -> void { sched_.tick(); }
};

} // namespace qnx::kernel
