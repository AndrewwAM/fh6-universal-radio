#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace fh6::sources {

class SpotifySource final : public IAudioSource {
public:
    explicit SpotifySource(SpotifyConfig cfg);
    ~SpotifySource() override;

    std::string_view name() const noexcept override { return "spotify"; }
    std::string_view display_name() const noexcept override { return "Spotify Connect"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override {} // Next/Prev are handled by the Spotify app
    void previous() override {}
    void pump(RingBuffer& ring) override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override;
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { 
        return {false, false, false}; // control is delegated to user
    }

private:
    struct Pipe;

    void start_pipe_locked();   // mu_ held
    void stop_pipe_locked();    // mu_ held
    bool cache_exists() const;

    SpotifyConfig cfg_;
    std::unique_ptr<Pipe> pipe_;

    mutable std::mutex mu_;
    TrackInfo info_{};
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
};

} // namespace fh6::sources