#include "fh6/sources/local_file_source.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <windows.h>
#include <cstdio>
#include <limits>

// miniaudio decodes mp3/flac/wav/ogg to S16LE/48k/stereo; everything else
// falls through to the ffmpeg fallback below.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_GENERATION
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244) // narrowing inside miniaudio's header
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <random>

namespace fh6::sources {

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kFrameBytes   = 4; // s16 * 2ch
constexpr std::uint64_t kBytesPerSec = std::uint64_t{kSampleRate} * kFrameBytes;

using subprocess::create_kill_on_close_job;
using subprocess::describe_launch_failure;
using subprocess::open_nul;
using subprocess::open_stderr_log;
using subprocess::quote;
using subprocess::spawn_in_job;

bool extension_matches(const std::filesystem::path& p, const std::vector<std::string>& exts) {
    if (!p.has_extension()) return false;
    auto e = p.extension().string();
    if (!e.empty() && e.front() == '.') e.erase(0, 1);
    std::ranges::transform(e, e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return std::ranges::find(exts, e) != exts.end();
}

struct ProbedMetadata {
    std::uint64_t duration_ms = 0;
    std::string title, artist, album;
};

bool ieq_str(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

// `ffmpeg -i <file>` with no output specified exits non-zero but first dumps
// the input header (Duration + container Metadata block) to stderr.
ProbedMetadata probe_metadata(const std::wstring& ff_bin, const std::filesystem::path& file) {
    ProbedMetadata out;

    HANDLE job = create_kill_on_close_job();
    if (!job) return out;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) {
        CloseHandle(job);
        return out;
    }
    SetHandleInformation(rd, 0, HANDLE_FLAG_INHERIT);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE nul_out = open_nul(GENERIC_WRITE);
    const std::wstring cmd =
        quote(ff_bin) + L" -hide_banner -nostdin -i " + quote(file.wstring());
    HANDLE proc = spawn_in_job(job, cmd, nul_in, nul_out, wr);

    CloseHandle(wr);
    if (nul_in)  CloseHandle(nul_in);
    if (nul_out) CloseHandle(nul_out);
    if (!proc) {
        CloseHandle(rd);
        CloseHandle(job);
        return out;
    }

    std::string err;
    char buf[1024];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) err.append(buf, got);
    CloseHandle(rd);
    CloseHandle(proc);
    CloseHandle(job);

    if (auto pos = err.find("Duration:"); pos != std::string::npos) {
        pos += 9;
        while (pos < err.size() && err[pos] == ' ') ++pos;
        int h = 0, m = 0;
        double s = 0.0;
        if (std::sscanf(err.c_str() + pos, "%d:%d:%lf", &h, &m, &s) == 3)
            out.duration_ms = static_cast<std::uint64_t>((h * 3600 + m * 60) * 1000.0 + s * 1000.0);
    }

    // First non-empty match wins so the container block (emitted before any
    // per-stream block) beats codec-internal "encoder" tags.
    for (std::size_t lstart = 0; lstart < err.size();) {
        auto nl   = err.find('\n', lstart);
        auto stop = (nl == std::string::npos) ? err.size() : nl;
        std::string_view line(err.data() + lstart, stop - lstart);
        lstart = (nl == std::string::npos) ? err.size() : nl + 1;

        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        auto key = line.substr(0, colon);
        while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
        auto val = line.substr(colon + 1);
        while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
        if (val.empty()) continue;

        if (out.title.empty()       && ieq_str(key, "title"))  out.title.assign(val);
        else if (out.artist.empty() && ieq_str(key, "artist")) out.artist.assign(val);
        else if (out.album.empty()  && ieq_str(key, "album"))  out.album.assign(val);
    }
    return out;
}

} // namespace

struct LocalFileSource::Decoder {
    ma_decoder ma{};
    bool ma_open = false;
    bool ma_eof = false;

    HANDLE ff_job              = nullptr;
    HANDLE ff_proc             = nullptr;
    HANDLE ff_pipe             = nullptr;
    std::uint64_t ff_bytes_out = 0;
    bool ff_eof                = false;

    TrackInfo info{};
    // Per-decoder so a prefetched track promotes with its own ReplayGain
    // multiplier instead of inheriting the current track's.
    float loudness_coef     = 1.0f;
    std::size_t for_cursor  = 0;

    bool any_open() const noexcept { return ma_open || ff_pipe != nullptr; }

    ~Decoder() {
        if (ma_open)  ma_decoder_uninit(&ma);
        if (ff_pipe)  CloseHandle(ff_pipe);
        if (ff_proc)  CloseHandle(ff_proc);
        // KILL_ON_JOB_CLOSE on the job reaps ffmpeg if it's still resident.
        if (ff_job)   CloseHandle(ff_job);
    }
};

LocalFileSource::LocalFileSource(LocalFilesConfig cfg, std::filesystem::path ffmpeg_path)
    : cfg_{std::move(cfg)},
      ffmpeg_path_{std::move(ffmpeg_path)} {}

LocalFileSource::~LocalFileSource() { close_current(); }

void LocalFileSource::shutdown() noexcept { close_current(); }

bool LocalFileSource::initialize() {
    if (!cfg_.enabled) return false;
    // No directory yet is fine. auth_state stays needs_auth until the user
    // sets one from the dashboard, which calls set_directory() to rescan.
    if (cfg_.music_dir.empty() || !std::filesystem::exists(cfg_.music_dir)) {
        log::warn("[local] registered with no music_dir yet -- set one from the dashboard");
        return true;
    }
    rebuild_playlist();
    log::info("[local] discovered {} tracks in {}", playlist_.size(), cfg_.music_dir.string());
    return true;
}

void LocalFileSource::rebuild_playlist() {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();   // playlist contents/order are about to change
    playlist_.clear();
    std::error_code ec;
    auto add = [&](const std::filesystem::path& p) {
        if (extension_matches(p, cfg_.supported_formats)) playlist_.push_back(p);
    };
    if (cfg_.recursive) {
        for (const auto& e : std::filesystem::recursive_directory_iterator(
                 cfg_.music_dir, std::filesystem::directory_options::skip_permission_denied, ec))
            if (e.is_regular_file(ec)) add(e.path());
    } else {
        for (const auto& e : std::filesystem::directory_iterator(cfg_.music_dir, ec))
            if (e.is_regular_file(ec)) add(e.path());
    }
    if (cfg_.shuffle) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::shuffle(playlist_.begin(), playlist_.end(), rng);
    } else {
        std::ranges::sort(playlist_);
    }
    cursor_ = 0;
}

void LocalFileSource::close_current() {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    dec_.reset();
}

std::unique_ptr<LocalFileSource::Decoder>
LocalFileSource::open_decoder_locked(std::size_t index) {
    if (playlist_.empty()) return nullptr;
    const std::size_t resolved = index % playlist_.size();
    const auto& path           = playlist_[resolved];

    auto d         = std::make_unique<Decoder>();
    d->for_cursor  = resolved;

    auto meta      = probe_metadata(ffmpeg_path_.empty() ? L"ffmpeg" : ffmpeg_path_.wstring(),
                                     path);
    d->info.duration_ms = meta.duration_ms;
    d->info.title       = std::move(meta.title);
    d->info.artist      = std::move(meta.artist);
    d->info.album       = std::move(meta.album);

    ma_decoder_config mc = ma_decoder_config_init(ma_format_s16, 2, kSampleRate);
    if (ma_decoder_init_file(path.string().c_str(), &mc, &d->ma) == MA_SUCCESS) {
        d->ma_open = true;
        // Decoded frame count beats ffmpeg's header duration on VBR MP3s.
        ma_uint64 frames = 0;
        if (ma_decoder_get_length_in_pcm_frames(&d->ma, &frames) == MA_SUCCESS)
            d->info.duration_ms = (frames * 1000ull) / kSampleRate;
    } else if (!open_track_ffmpeg(*d, path)) {
        log::warn("[local] failed to open {} (miniaudio rejected the format and the ffmpeg "
                  "fallback also failed; install ffmpeg or convert the file)",
                  path.string());
        return nullptr;
    }

    if (d->info.title.empty()) d->info.title = path.stem().string();
    if (d->info.album.empty()) d->info.album = path.parent_path().filename().string();

    float gain_db = std::numeric_limits<float>::quiet_NaN();
    float peak    = std::numeric_limits<float>::quiet_NaN();
    parse_replaygain_file(path, gain_db, peak);
    d->loudness_coef = compute_loudness_correction(gain_db, peak);

    return d;
}

bool LocalFileSource::open_track(std::size_t index) {
    auto d = open_decoder_locked(index);
    if (!d) return false;
    cursor_ = d->for_cursor;
    dec_    = std::move(d);
    position_ms_.store(0, std::memory_order_release);
    log::info("[local] now playing: {}", playlist_[cursor_].string());
    return true;
}

bool LocalFileSource::open_track_ffmpeg(Decoder& d, const std::filesystem::path& path) {
    const std::wstring ff = ffmpeg_path_.empty() ? L"ffmpeg" : ffmpeg_path_.wstring();

    d.ff_job = create_kill_on_close_job();
    if (!d.ff_job) {
        log::warn("[local] ffmpeg fallback: CreateJobObject failed ({})", GetLastError());
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return false;
    SetHandleInformation(rd, 0, HANDLE_FLAG_INHERIT);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();
    std::wstring cmd =
        quote(ff) + L" -hide_banner -loglevel error -nostdin -vn -i " + quote(path.wstring()) +
        L" -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    d.ff_proc = spawn_in_job(d.ff_job, cmd, nul_in, wr, err_log);
    const DWORD ec = d.ff_proc ? 0u : GetLastError();
    CloseHandle(wr);
    if (nul_in)  CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);
    if (!d.ff_proc) {
        CloseHandle(rd);
        log::warn("[local] ffmpeg fallback: failed to launch -- {}",
                  describe_launch_failure(ff, ec, !ffmpeg_path_.empty()));
        return false;  // ~Decoder reaps the job
    }

    d.ff_pipe = rd;
    return true;
}

void LocalFileSource::discard_prefetch_locked() noexcept { prefetch_dec_.reset(); }

std::size_t LocalFileSource::next_cursor_locked() const noexcept {
    if (playlist_.empty()) return 0;
    return (cursor_ + 1) % playlist_.size();
}

bool LocalFileSource::promote_prefetch_locked(std::size_t expected_cursor) {
    if (!prefetch_dec_ || prefetch_dec_->for_cursor != expected_cursor) {
        discard_prefetch_locked();
        return false;
    }
    dec_    = std::move(prefetch_dec_);
    cursor_ = dec_->for_cursor;
    position_ms_.store(0, std::memory_order_release);
    log::info("[local] now playing (prebuffered): {}", playlist_[cursor_].string());
    return true;
}

void LocalFileSource::maybe_spawn_prefetch_locked() {
    if (!prebuffer_next_.load(std::memory_order_acquire)) return;
    if (prefetch_dec_ || !dec_ || playlist_.size() < 2) return;
    // Local files (esp. miniaudio) open near-instantly, so no byte threshold is
    // needed -- spawning on the first pump tick after a track change is cheap.
    prefetch_dec_ = open_decoder_locked(next_cursor_locked());
}

void LocalFileSource::play() {
    std::scoped_lock lk{mu_};
    if ((!dec_ || !dec_->any_open()) && !open_track(cursor_)) {
        state_.store(PlaybackState::stopped, std::memory_order_release);
        return;
    }
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void LocalFileSource::pause() { state_.store(PlaybackState::paused, std::memory_order_release); }

bool LocalFileSource::skip_next() {
    std::scoped_lock lk{mu_};
    if (playlist_.empty()) return false;
    const std::size_t next = next_cursor_locked();
    if (!promote_prefetch_locked(next) && !open_track(next)) return false;
    state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

bool LocalFileSource::restart_current() {
    std::scoped_lock lk{mu_};
    if (!dec_ || !dec_->any_open()) return false;
    if (dec_->ma_open) {
        if (ma_decoder_seek_to_pcm_frame(&dec_->ma, 0) != MA_SUCCESS) return false;
    } else {
        // ffmpeg pipe is forward-only: re-open the same track from t=0.
        if (!open_track(cursor_)) return false;
    }
    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

void LocalFileSource::stop() {
    close_current();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void LocalFileSource::next() {
    std::scoped_lock lk{mu_};
    if (playlist_.empty()) return;
    const std::size_t next = next_cursor_locked();
    if (!promote_prefetch_locked(next)) open_track(next);
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void LocalFileSource::previous() {
    std::scoped_lock lk{mu_};
    if (playlist_.empty()) return;   // size()-1 would underflow size_t
    discard_prefetch_locked();       // prefetch targets cursor+1; previous() rewinds
    open_track(cursor_ == 0 ? playlist_.size() - 1 : cursor_ - 1);
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void LocalFileSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

    std::scoped_lock lk{mu_};
    if (!dec_ || !dec_->any_open()) return;

    const float rg = volume_norm_.load(std::memory_order_acquire)
                         ? dec_->loudness_coef
                         : 1.0f;
    auto advance_at_eof = [&] {
        const std::size_t next = next_cursor_locked();
        if (!promote_prefetch_locked(next)) open_track(next);
    };

    auto apply_dsp = [&](int16_t* samples, std::size_t frames) {
        if (rg != 1.0f) {
            const std::size_t n = frames * 2;
            for (std::size_t i = 0; i < n; ++i) {
                float s = static_cast<float>(samples[i]) * rg;
                if (s >  32767.0f) s =  32767.0f;
                if (s < -32768.0f) s = -32768.0f;
                samples[i] = static_cast<int16_t>(s);
            }
        }
        eq_.process(samples, frames);
    };

    if (dec_->ma_open) {
        // if end of the file, wait for the ring buffer to drain
        if (dec_->ma_eof) {
            ma_uint64 cursor = 0;
            if (ma_decoder_get_cursor_in_pcm_frames(&dec_->ma, &cursor) == MA_SUCCESS) {
                const uint64_t queued = ring.readable() / kFrameBytes;
                const uint64_t played = cursor > queued ? cursor - queued : 0;
                position_ms_.store((played * 1000ull) / kSampleRate, std::memory_order_release);
            }

            if (ring.readable() == 0) advance_at_eof();
            return;
        }

        constexpr std::size_t kChunkFrames = 4096;
        while (ring.writable() >= kChunkFrames * kFrameBytes) {
            int16_t scratch[kChunkFrames * 2];
            ma_uint64 read = 0;
            if (ma_decoder_read_pcm_frames(&dec_->ma, scratch, kChunkFrames, &read) != MA_SUCCESS)
                read = 0;
            if (read == 0) {
                dec_->ma_eof = true;
                break;
            }
            apply_dsp(scratch, static_cast<std::size_t>(read));
            ring.write(scratch, read * kFrameBytes);
            if (read < kChunkFrames) {
                dec_->ma_eof = true;
                break;
            }
        }

        // Audible head, not decoder head: subtract what's still queued in the ring.
        ma_uint64 cursor = 0;
        if (ma_decoder_get_cursor_in_pcm_frames(&dec_->ma, &cursor) == MA_SUCCESS) {
            const uint64_t queued = ring.readable() / kFrameBytes;
            const uint64_t played = cursor > queued ? cursor - queued : 0;
            position_ms_.store((played * 1000ull) / kSampleRate, std::memory_order_release);
        }
        maybe_spawn_prefetch_locked();
        return;
    }

    auto update_position = [&] {
        const std::uint64_t queued = ring.readable();
        const std::uint64_t played =
            dec_->ff_bytes_out > queued ? dec_->ff_bytes_out - queued : 0;
        position_ms_.store(played * 1000ull / kBytesPerSec, std::memory_order_release);
    };

    if (dec_->ff_eof) {
        update_position();
        if (ring.readable() == 0) advance_at_eof();
        return;
    }

    DWORD avail = 0;
    if (!PeekNamedPipe(dec_->ff_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        dec_->ff_eof = true;
        update_position();
        return;
    }
    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < kFrameBytes) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        // Force whole stereo S16 frames so the EQ never sees half a sample.
        want &= ~(kFrameBytes - 1);
        if (!want) break;
        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(dec_->ff_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            dec_->ff_eof = true;
            break;
        }
        const DWORD aligned = (got / (DWORD)kFrameBytes) * (DWORD)kFrameBytes;
        if (aligned) {
            apply_dsp(reinterpret_cast<int16_t*>(buf), aligned / kFrameBytes);
            ring.write(buf, aligned);
            dec_->ff_bytes_out += aligned;
        }
        avail = avail > got ? avail - got : 0;
    }
    update_position();
    maybe_spawn_prefetch_locked();
}

TrackInfo LocalFileSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo info;
    if (dec_) info = dec_->info;
    info.position_ms = position_ms_.load(std::memory_order_acquire);
    return info;
}

void LocalFileSource::set_directory(std::filesystem::path dir, bool recursive) {
    {
        std::scoped_lock lk{mu_};
        if (cfg_.music_dir == dir && cfg_.recursive == recursive) return;
        cfg_.music_dir = std::move(dir);
        cfg_.recursive = recursive;
    }
    rebuild_playlist();
    log::info("[local] rescanned: {} tracks under {}", playlist_.size(), cfg_.music_dir.string());
}

AuthState LocalFileSource::auth_state() const noexcept {
    std::scoped_lock lk{mu_};
    if (cfg_.music_dir.empty()) return AuthState::needs_auth;
    if (!std::filesystem::exists(cfg_.music_dir)) return AuthState::error;
    return playlist_.empty() ? AuthState::needs_auth : AuthState::none_required;
}

std::string LocalFileSource::auth_instructions() const {
    std::scoped_lock lk{mu_};
    if (cfg_.music_dir.empty()) {
        return "Pick a folder containing your music in the Settings drawer "
               "(Local files -> Music directory), then click Save.";
    }
    if (!std::filesystem::exists(cfg_.music_dir))
        return "Music folder doesn't exist: " + cfg_.music_dir.string();
    if (playlist_.empty()) {
        return "No audio files matching the configured extensions were found in " +
               cfg_.music_dir.string();
    }
    return {};
}

std::size_t LocalFileSource::track_count() const noexcept {
    std::scoped_lock lk{mu_};
    return playlist_.size();
}

void LocalFileSource::set_shuffle(bool shuffle) {
    {
        std::scoped_lock lk{mu_};
        cfg_.shuffle = shuffle;
    }
    rebuild_playlist();
}

void LocalFileSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

void LocalFileSource::set_playback_options(const PlaybackConfig& opts) {
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
    eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, (float)kSampleRate);
    const bool prev = prebuffer_next_.exchange(opts.prebuffer_next_track,
                                                std::memory_order_acq_rel);
    if (prev && !opts.prebuffer_next_track) {
        std::scoped_lock lk{mu_};
        discard_prefetch_locked();
    }
}

std::vector<std::string> LocalFileSource::playlist_snapshot() const {
    std::scoped_lock lk{mu_};
    std::vector<std::string> out;
    out.reserve(playlist_.size());
    for (const auto& p : playlist_) out.push_back(p.string());
    return out;
}

} // namespace fh6::sources
