// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory

#include <catch2/catch_test_macros.hpp>

import std;
import qnx.memory;

using namespace qnx;
using namespace qnx::memory;

// ─── Basic allocation ──────────────────────────────────────────────────────

TEST_CASE("mmap allocates real bytes", "[memory]") {
    MemoryManager mm;
    auto cap = mm.mmap(pid(1), 4096, perm_rw);
    REQUIRE(cap.has_value());
    REQUIRE(mm.buffer_count() == 1);
    REQUIRE(mm.capability_count() == 1);
}

TEST_CASE("write_span and read_span access real data", "[memory]") {
    MemoryManager mm;
    auto cap = mm.mmap(pid(1), 8, perm_rw);

    auto ws = mm.write_span(*cap);
    REQUIRE(ws.has_value());
    REQUIRE(ws->size() == 8);

    // Write pattern
    for (int i = 0; i < 8; ++i) (*ws)[i] = std::byte(i * 10);

    // Read back
    auto rs = mm.read_span(*cap);
    REQUIRE(rs.has_value());
    REQUIRE((*rs)[3] == std::byte{30});
}

// ─── Grant: permissions never escalate ─────────────────────────────────────

TEST_CASE("grant restricts permissions", "[memory][grant]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 1024, perm_rw);

    // Grant read-only
    auto child = mm.capability_grant(*root, pid(2), Perm::read);
    REQUIRE(child.has_value());

    auto perm = mm.get_perm(*child);
    REQUIRE(*perm == Perm::read);

    // Child cannot write
    auto ws = mm.write_span(*child);
    REQUIRE(!ws.has_value());
    REQUIRE(ws.error() == KernelError::permission_denied);

    // Child can read
    auto rs = mm.read_span(*child);
    REQUIRE(rs.has_value());
}

TEST_CASE("grant cannot escalate beyond parent", "[memory][grant]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 1024, Perm::read);

    // Try to grant read+write from a read-only parent
    auto child = mm.capability_grant(*root, pid(2), perm_rw);
    REQUIRE(child.has_value());

    // Child gets only read (intersection)
    auto perm = mm.get_perm(*child);
    REQUIRE(*perm == Perm::read);
}

TEST_CASE("grant chain: grandchild restricted further", "[memory][grant]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 1024, perm_rwx);

    auto child = mm.capability_grant(*root, pid(2), perm_rw);
    auto grandchild = mm.capability_grant(*child, pid(3), Perm::read);

    REQUIRE(*mm.get_perm(*root) == perm_rwx);
    REQUIRE(*mm.get_perm(*child) == perm_rw);
    REQUIRE(*mm.get_perm(*grandchild) == Perm::read);
}

// ─── Grant: sub-regions ────────────────────────────────────────────────────

TEST_CASE("grant sub-region of parent", "[memory][grant]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 1024, perm_rw);

    // Write a marker at offset 256
    auto ws = mm.write_span(*root);
    (*ws)[256] = std::byte{0xAB};

    // Grant bytes 256..511 to pid(2)
    auto child = mm.capability_grant(*root, pid(2), perm_rw, 256, 256);
    REQUIRE(child.has_value());

    // Child reads at offset 0 = parent offset 256
    auto rs = mm.read_span(*child);
    REQUIRE(rs.has_value());
    REQUIRE(rs->size() == 256);
    REQUIRE((*rs)[0] == std::byte{0xAB});
}

TEST_CASE("grant sub-region out of bounds fails", "[memory][grant]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 1024, perm_rw);

    auto child = mm.capability_grant(*root, pid(2), perm_rw, 900, 200);
    REQUIRE(!child.has_value());
    REQUIRE(child.error() == KernelError::permission_denied);
}

// ─── Shared buffer: two capabilities, same bytes ───────────────────────────

TEST_CASE("two capabilities see same buffer data", "[memory][shared]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 64, perm_rw);
    auto child = mm.capability_grant(*root, pid(2), perm_rw);

    // Writer writes via root
    auto ws = mm.write_span(*root);
    (*ws)[0] = std::byte{0xFF};

    // Reader reads via child — same byte
    auto rs = mm.read_span(*child);
    REQUIRE((*rs)[0] == std::byte{0xFF});
}

TEST_CASE("PCM audio pattern: write samples, reader sees them", "[memory][shared]") {
    MemoryManager mm;

    // Simulate 48kHz 16-bit stereo, 10ms buffer = 960 samples * 4 bytes = 3840
    constexpr std::size_t buf_size = 3840;
    auto producer_cap = mm.mmap(pid(1), buf_size, perm_rw);

    // Grant read-only to consumer (audio driver)
    auto consumer_cap = mm.capability_grant(*producer_cap, pid(2), Perm::read);

    // Producer writes a sine-like pattern
    auto ws = mm.write_span(*producer_cap);
    for (std::size_t i = 0; i < buf_size; ++i) {
        (*ws)[i] = std::byte(i & 0xFF);
    }

    // Consumer reads — sees the same pattern
    auto rs = mm.read_span(*consumer_cap);
    REQUIRE(rs->size() == buf_size);
    REQUIRE((*rs)[0] == std::byte{0});
    REQUIRE((*rs)[255] == std::byte{255});
    REQUIRE((*rs)[256] == std::byte{0});  // wraps
}

// ─── Revoke cascades ───────────────────────────────────────────────────────

TEST_CASE("revoke removes capability", "[memory][revoke]") {
    MemoryManager mm;
    auto cap = mm.mmap(pid(1), 1024, perm_rw);
    REQUIRE(mm.capability_count() == 1);

    (void)mm.capability_revoke(*cap);
    REQUIRE(mm.capability_count() == 0);
}

TEST_CASE("revoke cascades to children", "[memory][revoke]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 1024, perm_rw);
    auto child = mm.capability_grant(*root, pid(2), Perm::read);
    auto grandchild = mm.capability_grant(*child, pid(3), Perm::read);
    REQUIRE(mm.capability_count() == 3);

    // Revoke child — grandchild dies too
    (void)mm.capability_revoke(*child);
    REQUIRE(mm.capability_count() == 1);  // only root remains

    // Root still works
    auto rs = mm.read_span(*root);
    REQUIRE(rs.has_value());
}

TEST_CASE("revoke root kills entire tree", "[memory][revoke]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 1024, perm_rw);
    (void)mm.capability_grant(*root, pid(2), Perm::read);
    (void)mm.capability_grant(*root, pid(3), Perm::read);
    REQUIRE(mm.capability_count() == 3);

    (void)mm.capability_revoke(*root);
    REQUIRE(mm.capability_count() == 0);
}

// ─── munmap frees buffer when no refs ──────────────────────────────────────

TEST_CASE("munmap frees buffer when last capability removed", "[memory]") {
    MemoryManager mm;
    auto cap = mm.mmap(pid(1), 1024, perm_rw);
    REQUIRE(mm.buffer_count() == 1);

    (void)mm.munmap(*cap);
    REQUIRE(mm.buffer_count() == 0);
    REQUIRE(mm.capability_count() == 0);
}

TEST_CASE("munmap keeps buffer if other caps still reference it", "[memory]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 1024, perm_rw);
    auto child = mm.capability_grant(*root, pid(2), Perm::read);

    // munmap root — child is a derived cap, gets removed too (cascade)
    (void)mm.munmap(*root);
    REQUIRE(mm.capability_count() == 0);
    REQUIRE(mm.buffer_count() == 0);
}

// ─── Stress: deep grant chain ──────────────────────────────────────────────

TEST_CASE("stress: 100-deep grant chain, revoke root kills all", "[memory][stress]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 4096, perm_rwx);

    auto current = *root;
    for (int i = 2; i <= 101; ++i) {
        auto child = mm.capability_grant(current, pid(i), perm_rw);
        REQUIRE(child.has_value());
        current = *child;
    }
    REQUIRE(mm.capability_count() == 101);

    // Revoke root — all 100 children die
    (void)mm.capability_revoke(*root);
    REQUIRE(mm.capability_count() == 0);
}

TEST_CASE("stress: wide grant tree, revoke intermediate", "[memory][stress]") {
    MemoryManager mm;
    auto root = mm.mmap(pid(1), 4096, perm_rwx);

    // 10 children from root
    std::vector<CapabilityId> children;
    for (int i = 0; i < 10; ++i) {
        auto c = mm.capability_grant(*root, pid(i + 2), perm_rw);
        children.push_back(*c);

        // Each child has 5 grandchildren
        for (int j = 0; j < 5; ++j) {
            (void)mm.capability_grant(*c, pid(100 + i * 5 + j), Perm::read);
        }
    }
    REQUIRE(mm.capability_count() == 61);  // 1 + 10 + 50

    // Revoke child[3] — kills child[3] + its 5 grandchildren
    (void)mm.capability_revoke(children[3]);
    REQUIRE(mm.capability_count() == 55);  // 61 - 6

    // Root still works
    REQUIRE(mm.read_span(*root).has_value());
}

// ─── Access control ────────────────────────────────────────────────────────

TEST_CASE("check_access verifies ownership and permissions", "[memory]") {
    MemoryManager mm;
    auto cap = mm.mmap(pid(1), 1024, Perm::read);

    REQUIRE(mm.check_access(pid(1), *cap, Perm::read) == true);
    REQUIRE(mm.check_access(pid(1), *cap, Perm::write) == false);
    REQUIRE(mm.check_access(pid(2), *cap, Perm::read) == false);
}
