#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

struct NetworkPlaybackStatistics {
    uint32_t turn = 0, packets = 0, bytes = 0, state_drops = 0, queue_drops = 0;
    uint32_t decoded = 0, decode_errors = 0, resample_errors = 0, played = 0;
    uint32_t rx_gap_max_ms = 0, output_gap_max_ms = 0, decode_max_ms = 0;
    uint32_t starvation_events = 0, starvation_max_ms = 0;
    uint64_t played_samples = 0;
    int source_rate = 0, frame_ms = 0;
};

// Owned by AudioService's queue mutex. Only network TTS uses the startup gate;
// direct PCM preview and local sounds continue to play immediately.
class NetworkPlayback {
public:
    void Begin() {
        const uint32_t next = stats_.turn + 1;
        stats_ = {};
        stats_.turn = next;
        receiving_ = tracking_ = buffering_ = true;
        buffer_start_ = last_rx_ = last_output_end_ = empty_since_ = 0;
        pending_empty_ = false;
    }
    void End() { receiving_ = buffering_ = pending_empty_ = false; }
    void Reset() {
        End();
        tracking_ = false;
    }
    bool Active() const { return receiving_; }
    bool Prepared() const { return tracking_; }
    uint32_t CurrentTurn() const { return receiving_ ? stats_.turn : 0; }
    uint32_t Packet(uint64_t now_ms, size_t bytes, int rate, int frame_ms, bool accepted) {
        if (!receiving_)
            return 0;
        if (!stats_.packets)
            buffer_start_ = now_ms;
        else
            stats_.rx_gap_max_ms = std::max(stats_.rx_gap_max_ms, uint32_t(now_ms - last_rx_));
        if (pending_empty_) {
            const uint64_t gap = now_ms - empty_since_;
            // A queued I2S frame can still cover up to one frame of software
            // queue emptiness. Count only resumed data after that grace period.
            if (gap > uint32_t(std::max(stats_.frame_ms, 60))) {
                stats_.starvation_events++;
                stats_.starvation_max_ms = std::max(stats_.starvation_max_ms,
                                                    uint32_t(gap - std::max(stats_.frame_ms, 60)));
                buffering_ = true;
                buffer_start_ = now_ms;
            }
            pending_empty_ = false;
        }
        last_rx_ = now_ms;
        stats_.packets++;
        stats_.bytes += bytes;
        stats_.source_rate = rate;
        stats_.frame_ms = frame_ms;
        if (!accepted)
            stats_.state_drops++;
        return stats_.turn;
    }
    bool Ready(uint32_t turn, size_t pending, uint64_t now_ms) const {
        // 480 ms also covers the observed 397 ms delivery gap. Waits remain
        // bounded for short segments; End() flushes a short final tail at once.
        return !Owns(turn) || !receiving_ || !buffering_ || pending >= 8 ||
               (stats_.packets && now_ms - buffer_start_ >= 600);
    }
    void QueueDrop(uint32_t turn) {
        if (Owns(turn))
            stats_.queue_drops++;
    }
    void Decoded(uint32_t turn, bool okay, bool resample_error, uint32_t elapsed_ms) {
        if (!Owns(turn))
            return;
        if (okay)
            stats_.decoded++;
        else
            stats_.decode_errors++;
        if (resample_error)
            stats_.resample_errors++;
        stats_.decode_max_ms = std::max(stats_.decode_max_ms, elapsed_ms);
    }
    void OutputStart(uint32_t turn, uint64_t now_ms) {
        if (!Owns(turn))
            return;
        buffering_ = false;
        pending_empty_ = false;
        if (stats_.played)
            stats_.output_gap_max_ms =
                std::max(stats_.output_gap_max_ms, uint32_t(now_ms - last_output_end_));
    }
    void OutputDone(uint32_t turn, uint64_t now_ms, size_t samples) {
        if (!Owns(turn))
            return;
        stats_.played++;
        stats_.played_samples += samples;
        last_output_end_ = now_ms;
    }
    void QueueEmpty(uint32_t turn, uint64_t now_ms) {
        if (Owns(turn) && receiving_ && stats_.played && !pending_empty_) {
            pending_empty_ = true;
            empty_since_ = now_ms;
        }
    }
    NetworkPlaybackStatistics Snapshot() const { return stats_; }

private:
    bool Owns(uint32_t turn) const { return tracking_ && turn && turn == stats_.turn; }
    NetworkPlaybackStatistics stats_;
    bool receiving_ = false, tracking_ = false, buffering_ = false, pending_empty_ = false;
    uint64_t buffer_start_ = 0, last_rx_ = 0, last_output_end_ = 0, empty_since_ = 0;
};
