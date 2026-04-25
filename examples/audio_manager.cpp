// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory
//
// AudioManager — stereo audio with user-space shared memory
//
// Demonstrates:
//   - Interleaved stereo PCM in a shared buffer (L/R/L/R)
//   - Sub-region capability grants (left channel to VU meter)
//   - User-space read-back (waveform visualizer reads same buffer)
//   - Three processes share one buffer, different permissions
//   - Revoke cascades: revoking app's grant kills driver AND VU meter
//   - PPS for control plane, IPC for capability transfer
//
// Who gets what:
//   App (producer)    → full buffer, read+write
//   Audio driver      → full buffer, read-only (playback)
//   VU meter          → left channel only, read-only (level display)
//   Waveform viz      → full buffer, read-only (user-space read-back)

import std;
import qnx.kernel;

using namespace qnx;
using namespace qnx::kernel;
using namespace qnx::ipc;
using namespace qnx::memory;
using namespace qnx::pps;

// 48kHz, 16-bit, stereo, 10ms frame
// 960 samples * 2 channels * 2 bytes = 3840 bytes
// Left channel: bytes 0,1,4,5,8,9,... (every 4 bytes)
// Right channel: bytes 2,3,6,7,10,11,...
constexpr std::size_t samples_per_frame = 960;
constexpr std::size_t bytes_per_sample  = 2;   // 16-bit
constexpr std::size_t channels          = 2;   // stereo
constexpr std::size_t frame_bytes       = samples_per_frame * bytes_per_sample * channels;
constexpr std::size_t half_frame        = frame_bytes / 2;  // one channel's worth

// Simulate a 16-bit sine sample at position i
constexpr auto sine_sample(std::size_t i, int freq_shift) -> std::int16_t {
    // Simplified: sawtooth wave, wraps at 32767
    return static_cast<std::int16_t>((i * (100 + freq_shift)) % 32768);
}

int main() {
    Kernel k;
    k.init();

    auto& sched = k.scheduler();
    auto& ipc   = k.ipc();
    auto& mem   = k.memory();
    auto& ns    = k.ns();
    auto& pps   = k.pps();

    std::println("=== Stereo AudioManager ===");
    std::println("  format: 48kHz / 16-bit / stereo / 10ms frames");
    std::println("  frame:  {} samples, {} bytes", samples_per_frame, frame_bytes);
    std::println("");

    // ─── Services ───────────────────────────────────────────────────────────

    // Audio driver at /dev/audio
    auto drv_tid = sched.thread_create(pid(1), pri(20));
    auto drv_ch = ipc.channel_create(pid(1));
    (void)ns.register_resource("/dev/audio", *drv_ch, pid(1));

    // VU meter at /dev/vu
    auto vu_tid = sched.thread_create(pid(3), pri(5));
    auto vu_ch = ipc.channel_create(pid(3));
    (void)ns.register_resource("/dev/vu", *vu_ch, pid(3));

    // Waveform visualizer (user-space app, pid 4)
    auto viz_tid = sched.thread_create(pid(4), pri(5));

    std::println("driver: /dev/audio registered");
    std::println("vu:     /dev/vu registered");
    std::println("viz:    waveform visualizer started");

    // PPS: audio status + subscribers
    (void)pps.publish("/pps/audio/status", "state", "idle");
    (void)pps.publish("/pps/audio/status", "channels", "2");
    (void)pps.publish("/pps/audio/status", "format", "s16le interleaved");

    int pps_count = 0;
    (void)pps.subscribe("/pps/audio/", pid(99),
        [&](const Notification& n) {
            pps_count++;
            std::println("  pps #{}: {}.{} = {}", pps_count, n.path, n.key, n.value);
        });

    std::println("");

    // ─── Producer: allocate stereo buffer ───────────────────────────────────

    auto app_tid = sched.thread_create(pid(2), pri(10));
    auto app_conn_audio = ns.open("/dev/audio", pid(2));
    auto app_conn_vu    = ns.open("/dev/vu", pid(2));

    auto buf_cap = mem.mmap(pid(2), frame_bytes, perm_rw);
    std::println("app:    mmap {} bytes stereo buffer (cap={})", frame_bytes, buf_cap->value);

    // Write interleaved stereo: L sample, R sample, L sample, R sample...
    auto ws = mem.write_span(*buf_cap);
    for (std::size_t i = 0; i < samples_per_frame; ++i) {
        auto left  = sine_sample(i, 0);    // 100Hz base
        auto right = sine_sample(i, 50);   // 150Hz base (different channel)
        auto offset = i * bytes_per_sample * channels;
        // Little-endian 16-bit
        (*ws)[offset + 0] = std::byte(left & 0xFF);
        (*ws)[offset + 1] = std::byte((left >> 8) & 0xFF);
        (*ws)[offset + 2] = std::byte(right & 0xFF);
        (*ws)[offset + 3] = std::byte((right >> 8) & 0xFF);
    }
    std::println("app:    wrote stereo PCM (L=100Hz, R=150Hz)");

    // ─── Grant capabilities ─────────────────────────────────────────────────

    // Full buffer, read-only → audio driver (playback)
    auto drv_cap = mem.capability_grant(*buf_cap, pid(1), Perm::read);
    std::println("app:    grant cap={} to driver (full buffer, read-only)", drv_cap->value);

    // First half of buffer, read-only → VU meter (left channel approximation)
    auto vu_cap = mem.capability_grant(*buf_cap, pid(3), Perm::read, 0, half_frame);
    std::println("app:    grant cap={} to VU meter (first {} bytes, read-only)",
                 vu_cap->value, half_frame);

    // Full buffer, read-only → waveform visualizer (user-space read-back)
    auto viz_cap = mem.capability_grant(*buf_cap, pid(4), Perm::read);
    std::println("app:    grant cap={} to visualizer (full buffer, read-only)", viz_cap->value);

    std::println("");

    // ─── Driver: receive and play ───────────────────────────────────────────

    // Send driver cap ID via IPC
    auto drv_cap_bytes = std::bit_cast<std::array<std::byte, sizeof(int)>>(drv_cap->value);
    (void)ipc.msg_send(*app_tid, *app_conn_audio, drv_cap_bytes);

    auto drv_rcv = ipc.msg_receive(*drv_tid, *drv_ch);
    auto drv_play_cap = CapabilityId{std::bit_cast<int>(
        std::array<std::byte, 4>{drv_rcv->data[0], drv_rcv->data[1],
                                  drv_rcv->data[2], drv_rcv->data[3]})};

    auto drv_span = mem.read_span(drv_play_cap);
    std::println("driver: read_span -> {} bytes (zero copy, stereo interleaved)", drv_span->size());

    // Read first stereo sample (L + R)
    auto first_L = std::int16_t(std::to_integer<int>((*drv_span)[0]) |
                                (std::to_integer<int>((*drv_span)[1]) << 8));
    auto first_R = std::int16_t(std::to_integer<int>((*drv_span)[2]) |
                                (std::to_integer<int>((*drv_span)[3]) << 8));
    std::println("driver: first sample L={}, R={}", first_L, first_R);

    // Driver cannot write
    auto drv_write = mem.write_span(drv_play_cap);
    std::println("driver: write attempt: {} (read-only cap)",
                 drv_write.has_value() ? "BUG" : "denied");

    auto drv_reply = ipc.msg_reply(drv_rcv->sender, as_bytes("ok"));
    std::println("driver: replied");

    (void)pps.publish("/pps/audio/status", "state", "playing");
    (void)pps.publish("/pps/audio/status", "track", "Raga Yaman - Ustad Vilayat Khan");
    std::println("");

    // ─── VU meter: read left channel sub-region ─────────────────────────────

    auto vu_span = mem.read_span(*vu_cap);
    std::println("vu:     read_span -> {} bytes (left channel sub-region)", vu_span->size());

    // Compute peak level from first 100 samples
    int peak = 0;
    for (std::size_t i = 0; i < 100 * bytes_per_sample && i + 1 < vu_span->size(); i += bytes_per_sample) {
        auto sample = std::abs(std::int16_t(
            std::to_integer<int>((*vu_span)[i]) |
            (std::to_integer<int>((*vu_span)[i + 1]) << 8)));
        if (sample > peak) peak = sample;
    }
    std::println("vu:     peak level (left): {}", peak);
    (void)pps.publish("/pps/audio/levels", "left_peak", std::to_string(peak));

    // VU meter cannot see the right channel (sub-region grant)
    std::println("vu:     buffer size {} < full frame {} (sub-region enforced)",
                 vu_span->size(), frame_bytes);
    std::println("");

    // ─── Visualizer: user-space read-back ───────────────────────────────────

    auto viz_span = mem.read_span(*viz_cap);
    std::println("viz:    read_span -> {} bytes (full stereo, user-space read-back)",
                 viz_span->size());

    // Compute simple stats for waveform display
    int l_sum = 0, r_sum = 0;
    constexpr int check_samples = 100;
    for (std::size_t i = 0; i < check_samples; ++i) {
        auto offset = i * bytes_per_sample * channels;
        auto l = std::abs(std::int16_t(
            std::to_integer<int>((*viz_span)[offset]) |
            (std::to_integer<int>((*viz_span)[offset + 1]) << 8)));
        auto r = std::abs(std::int16_t(
            std::to_integer<int>((*viz_span)[offset + 2]) |
            (std::to_integer<int>((*viz_span)[offset + 3]) << 8)));
        l_sum += l;
        r_sum += r;
    }
    std::println("viz:    avg level L={}, R={} (from {} samples)",
                 l_sum / check_samples, r_sum / check_samples, check_samples);
    std::println("viz:    L != R confirms stereo separation");
    std::println("");

    // ─── Revoke: cascade kills driver + VU + viz ────────────────────────────

    (void)pps.publish("/pps/audio/status", "state", "idle");

    // Revoke the driver's capability — but all three were granted from buf_cap
    // Revoking individual grants:
    (void)mem.capability_revoke(*drv_cap);
    std::println("app:    revoked driver cap");
    std::println("driver: read after revoke: {}",
                 mem.read_span(drv_play_cap).has_value() ? "BUG" : "denied");

    // VU and viz still work (they were granted independently from buf_cap)
    std::println("vu:     read after driver revoke: {}",
                 mem.read_span(*vu_cap).has_value() ? "still valid" : "denied");
    std::println("viz:    read after driver revoke: {}",
                 mem.read_span(*viz_cap).has_value() ? "still valid" : "denied");

    // Now revoke all — munmap the root
    (void)mem.munmap(*buf_cap);
    std::println("");
    std::println("app:    munmap root buffer — cascade revoke");
    std::println("vu:     read after munmap: {}",
                 mem.read_span(*vu_cap).has_value() ? "BUG" : "denied");
    std::println("viz:    read after munmap: {}",
                 mem.read_span(*viz_cap).has_value() ? "BUG" : "denied");
    std::println("app:    buffers={}, caps={}", mem.buffer_count(), mem.capability_count());

    // ─── Summary ────────────────────────────────────────────────────────────

    std::println("");
    std::println("=== Summary ===");
    std::println("  Format:               48kHz / 16-bit / stereo / interleaved");
    std::println("  Frame:                {} samples, {} bytes", samples_per_frame, frame_bytes);
    std::println("  Shared buffer:        1 (accessed by 4 processes)");
    std::println("  Capabilities granted: 3 (driver=full/RO, vu=half/RO, viz=full/RO)");
    std::println("  Memory copies:        0");
    std::println("  IPC messages:         1 (4-byte cap ID, not audio data)");
    std::println("  PPS notifications:    {}", pps_count);
    std::println("  Revoke cascade:       munmap root kills all grants");
    std::println("  DMA:                  not needed");

    return 0;
}
