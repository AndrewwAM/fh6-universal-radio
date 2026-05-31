#include "fh6/sources/jellyfin_source.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fh6::sources {

namespace {

using subprocess::create_kill_on_close_job;
using subprocess::describe_launch_failure;
using subprocess::open_nul;
using subprocess::open_stderr_log;
using subprocess::quote;
using subprocess::spawn_in_job;
using subprocess::widen;

// PCM contract written by ffmpeg: 48000 Hz * 2 ch * 2 bytes.
constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

// 5 s ceilings on every WinHTTP phase so an unreachable server cannot stall
// the bridge thread (or a settings PATCH handler) for the default 60 seconds.
constexpr int kHttpTimeoutMs = 5000;

struct WinHttpDeleter {
    void operator()(void* h) const noexcept { if (h) WinHttpCloseHandle(h); }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpDeleter>;

bool config_complete(const JellyfinConfig& c) noexcept {
    return !c.server_url.empty() && !c.api_key.empty() &&
           !c.user_id.empty() && (!c.default_playlist.empty() || c.use_favorites);
}

// Fields that determine which playlist gets fetched. `shuffle` deliberately
// omitted -- it doesn't require a re-query.
bool same_query_target(const JellyfinConfig& a, const JellyfinConfig& b) noexcept {
    if (a.server_url != b.server_url || a.api_key != b.api_key ||
        a.user_id != b.user_id || a.use_favorites != b.use_favorites) return false;
    // default_playlist is irrelevant when both sides fetch favorites.
    if (a.use_favorites && b.use_favorites) return true;
    return a.default_playlist == b.default_playlist;
}

std::optional<std::string> http_get(const JellyfinConfig& cfg, const std::string& path) {
    // Reject control characters in the API key so a malformed value cannot
    // break out of the Authorization header (header injection).
    if (cfg.api_key.find_first_of("\r\n\"") != std::string::npos) {
        log::error("[jellyfin] api_key contains invalid characters");
        return std::nullopt;
    }

    URL_COMPONENTS comp{};
    comp.dwStructSize     = sizeof(comp);
    comp.dwHostNameLength = (DWORD)-1;
    const std::wstring url = widen(cfg.server_url);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &comp) || !comp.lpszHostName) {
        log::error("[jellyfin] invalid server_url '{}' -- expected http:// or https://",
                   cfg.server_url);
        return std::nullopt;
    }
    const std::wstring host(comp.lpszHostName, comp.dwHostNameLength);

    WinHttpHandle session{WinHttpOpen(L"FH6 Universal Radio/1.0",
                                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session.get(), kHttpTimeoutMs, kHttpTimeoutMs,
                       kHttpTimeoutMs, kHttpTimeoutMs);

    WinHttpHandle conn{WinHttpConnect(session.get(), host.c_str(), comp.nPort, 0)};
    if (!conn) return std::nullopt;

    WinHttpHandle req{WinHttpOpenRequest(conn.get(), L"GET", widen(path).c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          comp.nScheme == INTERNET_SCHEME_HTTPS
                                              ? WINHTTP_FLAG_SECURE : 0)};
    if (!req) return std::nullopt;

    const std::wstring auth = widen(std::format(
        "Authorization: MediaBrowser Token=\"{}\"", cfg.api_key));
    WinHttpAddRequestHeaders(req.get(), auth.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        log::error("[jellyfin] HTTP send/receive failed (err {})", GetLastError());
        return std::nullopt;
    }

    DWORD status = 0, status_sz = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &avail) || avail == 0) break;
        const std::size_t off = body.size();
        body.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.get(), body.data() + off, avail, &got)) break;
        body.resize(off + got);
        if (got == 0) break;
    }

    if (status != 200) {
        log::error("[jellyfin] HTTP {} from server: {}", status, body);
        return std::nullopt;
    }
    return body;
}

std::optional<std::vector<JellyfinTrack>> fetch_tracks(const JellyfinConfig& cfg) {
    std::string path;
    if (cfg.use_favorites) {
        path = std::format("/Users/{}/Items?Filters=IsFavorite&IncludeItemTypes=Audio&Recursive=true", cfg.user_id);
    } else {
        path = std::format("/Users/{}/Items?ParentId={}&Filters=IsNotFolder", cfg.user_id, cfg.default_playlist);
    }
    auto body = http_get(cfg, path);
    if (!body) return std::nullopt;

    std::vector<JellyfinTrack> out;
    try {
        const auto root  = nlohmann::json::parse(*body);
        const auto items = root.find("Items");
        if (items == root.end() || !items->is_array()) {
            log::error("[jellyfin] response missing Items array");
            return std::nullopt;
        }
        out.reserve(items->size());
        for (const auto& item : *items) {
            JellyfinTrack t;
            t.id = item.value("Id", "");
            if (t.id.empty()) continue;
            t.title = item.value("Name", "Unknown Track");
            if (auto aa = item.find("AlbumArtist"); aa != item.end() && aa->is_string())
                t.artist = aa->get<std::string>();
            else if (auto ar = item.find("Artists");
                     ar != item.end() && ar->is_array() && !ar->empty()
                     && ar->front().is_string())
                t.artist = ar->front().get<std::string>();
            t.album = item.value("Album", "");
            if (auto r = item.find("RunTimeTicks");
                r != item.end() && r->is_number_unsigned())
                t.duration_ms = r->get<std::uint64_t>() / 10'000u;  // 10000 ticks = 1 ms
            out.push_back(std::move(t));
        }
    } catch (const std::exception& e) {
        log::error("[jellyfin] JSON parse error: {}", e.what());
        return std::nullopt;
    }
    log::info("[jellyfin] fetched {} track(s)", out.size());
    return out;
}

void shuffle_range(std::vector<JellyfinTrack>& q, std::size_t from) {
    if (from >= q.size() || q.size() - from < 2) return;
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::shuffle(q.begin() + (std::ptrdiff_t)from, q.end(), rng);
}

// Per-source HTTP serializer; held outside mu_ across the network round-trip.
std::mutex& fetch_serializer() {
    static std::mutex m;
    return m;
}

} // namespace

struct JellyfinSource::Pipe {
    HANDLE job       = nullptr;
    HANDLE proc      = nullptr;
    HANDLE read_pipe = nullptr;
    std::uint64_t bytes_written = 0;
    std::atomic<std::uint64_t> position_ms{0};
    bool ended = false;
    std::size_t for_queue_idx = 0;

    ~Pipe() {
        // Close the read side first so ffmpeg's next write returns
        // ERROR_BROKEN_PIPE; dropping the job handle then reaps it via
        // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.
        if (read_pipe) CloseHandle(read_pipe);
        if (job)       CloseHandle(job);
        if (proc)      CloseHandle(proc);
    }
};

JellyfinSource::JellyfinSource(JellyfinConfig cfg, std::filesystem::path ffmpeg_path)
    : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)} {}

JellyfinSource::~JellyfinSource() {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
}

bool JellyfinSource::initialize() {
    if (!cfg_.enabled) return false;
    if (!config_complete(cfg_)) return true;   // tile visible; user can fill fields later

    // Construction precedes registration -- no other thread holds a reference
    // yet, so the fetch can run without locking.
    if (auto tracks = fetch_tracks(cfg_)) {
        queue_ = std::move(*tracks);
        if (cfg_.shuffle) shuffle_range(queue_, 0);
    }
    return true;
}

void JellyfinSource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
}

std::unique_ptr<JellyfinSource::Pipe>
JellyfinSource::spawn_pipe_locked(std::size_t for_idx) {
    if (queue_.empty() || for_idx >= queue_.size()) return nullptr;

    auto pipe = std::make_unique<Pipe>();
    pipe->for_queue_idx = for_idx;
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[jellyfin] CreateJobObject failed ({})", GetLastError());
        return nullptr;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20)) return nullptr;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    const std::wstring ff = ffmpeg_path_.empty() ? std::wstring{L"ffmpeg"}
                                                 : ffmpeg_path_.wstring();
    const std::string stream_url = std::format("{}/Audio/{}/stream?static=true",
                                                cfg_.server_url, queue_[for_idx].id);
    // Pass the API key via -headers so it isn't visible on the ffmpeg command
    // line to other local processes. \r\n is the canonical separator ffmpeg
    // expects between (and trailing) custom headers.
    const std::wstring auth_header = widen(std::format(
        "Authorization: MediaBrowser Token=\"{}\"\r\n", cfg_.api_key));

    std::wstring cmd = quote(ff) +
        L" -loglevel error -headers " + quote(auth_header) +
        L" -i " + quote(widen(stream_url)) + L" -f s16le ";
    if (volume_norm_.load(std::memory_order_acquire))
        cmd += L"-af loudnorm=I=-14:TP=-2:LRA=11 ";
    cmd += L"-acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    pipe->proc = spawn_in_job(pipe->job, cmd, nul_in, out_w, err_log);
    const DWORD ec = pipe->proc ? 0u : GetLastError();
    CloseHandle(out_w);
    if (nul_in)  CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);
    if (!pipe->proc) {
        CloseHandle(out_r);
        log::warn("[jellyfin] failed to launch ffmpeg -- {}",
                  describe_launch_failure(ff, ec, !ffmpeg_path_.empty()));
        return nullptr;  // ~Pipe reaps the job
    }

    pipe->read_pipe = out_r;
    return pipe;
}

void JellyfinSource::start_pipe_locked() {
    stop_pipe_locked();
    pipe_ = spawn_pipe_locked(current_idx_);
}

void JellyfinSource::stop_pipe_locked() {
    // Symmetric with YT: prefetch is preserved across stop_pipe_locked() so a
    // pending promotion isn't dropped on re-spawn. stop()/shutdown() drop it
    // explicitly.
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void JellyfinSource::discard_prefetch_locked() noexcept { prefetch_.reset(); }

std::size_t JellyfinSource::next_queue_idx_locked() const noexcept {
    if (queue_.empty()) return 0;
    return (current_idx_ + 1) % queue_.size();
}

bool JellyfinSource::promote_prefetch_locked(std::size_t expected_idx) {
    if (!prefetch_ || prefetch_->for_queue_idx != expected_idx) {
        discard_prefetch_locked();
        return false;
    }
    pipe_ = std::move(prefetch_);
    return true;
}

void JellyfinSource::maybe_spawn_prefetch_locked() {
    if (!prebuffer_next_.load(std::memory_order_acquire)) return;
    if (prefetch_ || !pipe_ || queue_.size() < 2) return;
    // Match YT's threshold: ~0.5 s of PCM proves the current pipe is viable
    // before we commit a second ffmpeg.
    constexpr std::uint64_t kViableBytes = 96 * 1024;
    if (pipe_->bytes_written < kViableBytes) return;
    prefetch_ = spawn_pipe_locked(next_queue_idx_locked());
}

void JellyfinSource::advance_locked(std::ptrdiff_t step) {
    if (queue_.empty()) return;
    const auto n = (std::ptrdiff_t)queue_.size();
    auto i = (std::ptrdiff_t)current_idx_ + step;
    current_idx_ = (std::size_t)(((i % n) + n) % n);
    if (step == 1 && promote_prefetch_locked(current_idx_)) {
        // Promoted: pipe_ is the pre-warmed pipeline, no fresh spawn needed.
    } else {
        discard_prefetch_locked();   // backwards step invalidates the prefetch
        start_pipe_locked();
    }
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void JellyfinSource::play() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return;            // cast()/set_config() will populate
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void JellyfinSource::pause() {
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void JellyfinSource::stop() {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
    current_idx_ = 0;
}

void JellyfinSource::next()     { std::scoped_lock lk{mu_}; advance_locked(+1); }
void JellyfinSource::previous() { std::scoped_lock lk{mu_}; advance_locked(-1); }

bool JellyfinSource::cast(std::string playlist_id) {
    if (playlist_id.empty()) return false;

    // Build the fetch config from a fresh cfg_ snapshot + the cast target,
    // then run the HTTP call with no class locks held.
    JellyfinConfig snap;
    {
        std::scoped_lock lk{mu_};
        snap = cfg_;
    }
    snap.default_playlist = playlist_id;
    snap.use_favorites    = false;   // cast targets a specific playlist
    if (!config_complete(snap)) return false;

    std::optional<std::vector<JellyfinTrack>> tracks;
    {
        std::scoped_lock fetch_lk{fetch_serializer()};
        tracks = fetch_tracks(snap);
    }
    if (!tracks) return false;

    std::scoped_lock lk{mu_};
    cfg_.default_playlist = std::move(playlist_id);
    queue_                = std::move(*tracks);
    current_idx_          = 0;
    if (cfg_.shuffle) shuffle_range(queue_, 0);
    discard_prefetch_locked();   // stale: targets the old playlist
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

void JellyfinSource::set_config(JellyfinConfig cfg) {
    // Decide what's actually changing under a brief lock; do the HTTP fetch
    // with no class lock held; then commit under the lock again.
    bool requery, shuffle_flip;
    {
        std::scoped_lock lk{mu_};
        requery      = !same_query_target(cfg_, cfg) && config_complete(cfg);
        shuffle_flip = cfg_.shuffle != cfg.shuffle;
    }

    std::optional<std::vector<JellyfinTrack>> tracks;
    if (requery) {
        std::scoped_lock fetch_lk{fetch_serializer()};
        tracks = fetch_tracks(cfg);
    }

    std::scoped_lock lk{mu_};
    const bool was_playing = state_.load(std::memory_order_acquire) == PlaybackState::playing;
    cfg_ = std::move(cfg);

    if (tracks) {
        discard_prefetch_locked();
        stop_pipe_locked();
        queue_       = std::move(*tracks);
        current_idx_ = 0;
        if (cfg_.shuffle) shuffle_range(queue_, 0);
        if (was_playing) {
            start_pipe_locked();
            if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
        }
    } else if (shuffle_flip && cfg_.shuffle) {
        shuffle_range(queue_, current_idx_ + 1);   // preserve the currently-playing track
        discard_prefetch_locked();                  // next-idx URL just changed
    }
}

void JellyfinSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

void JellyfinSource::set_playback_options(const PlaybackConfig& opts) {
    {
        std::scoped_lock lk{mu_};
        eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, 48000.0f);
    }
    // loudnorm is in the ffmpeg argv; new state takes effect on the next track.
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
    const bool prev = prebuffer_next_.exchange(opts.prebuffer_next_track,
                                                std::memory_order_acq_rel);
    if (prev && !opts.prebuffer_next_track) {
        std::scoped_lock lk{mu_};
        discard_prefetch_locked();
    }
}

TrackInfo JellyfinSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo info;
    if (queue_.empty() || current_idx_ >= queue_.size()) return info;
    const auto& t    = queue_[current_idx_];
    info.title       = t.title;
    info.artist      = t.artist;
    info.album       = t.album;
    info.duration_ms = t.duration_ms;
    if (pipe_) info.position_ms = pipe_->position_ms.load(std::memory_order_acquire);
    return info;
}

AuthState JellyfinSource::auth_state() const noexcept {
    std::scoped_lock lk{mu_};
    return config_complete(cfg_) ? AuthState::authenticated : AuthState::needs_auth;
}

void JellyfinSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p) return;

    auto update_position = [&] {
        const std::size_t r = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        p->position_ms.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };
    auto on_eof = [&] {
        if (p->read_pipe) {
            CloseHandle(p->read_pipe);
            p->read_pipe = nullptr;
        }
        p->ended = true;
    };

    if (p->ended) {
        update_position();
        if (ring.readable() == 0) advance_locked(+1);
        return;
    }
    if (!p->read_pipe) return;

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        on_eof();
        return;
    }
    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        want &= ~std::size_t{3};   // whole stereo s16 frames -- EQ never sees half a sample
        if (!want) break;

        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            on_eof();
            break;
        }
        const DWORD aligned = (got / 4u) * 4u;
        if (aligned) eq_.process(reinterpret_cast<int16_t*>(buf), aligned / 4u);
        ring.write(buf, aligned);
        p->bytes_written += aligned;
        avail = avail > got ? avail - got : 0;
    }
    update_position();
    maybe_spawn_prefetch_locked();
}

} // namespace fh6::sources
