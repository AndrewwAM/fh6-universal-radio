#include "fh6/sources/spotify_source.hpp"
#include "fh6/log.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fh6::sources {

namespace {

// CreateProcess hands one string to the child via GetCommandLineW
std::wstring quote(const std::wstring& s) {
    if (s.empty()) return L"\"\"";
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out{L"\""};
    for (auto c : s) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L'"';
    return out;
}

// NUL is Windows' /dev/null
HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

// tee stderr to temp folder for debugging
HANDLE open_stderr_log() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    auto path = std::filesystem::temp_directory_path() / "fh6-spotify-stderr.log";
    HANDLE h  = CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? open_nul(GENERIC_WRITE) : h;
}

std::filesystem::path stderr_log_path() {
    return std::filesystem::temp_directory_path() / "fh6-spotify-stderr.log";
}

// Job Object with KILL_ON_JOB_CLOSE
HANDLE create_kill_on_close_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

// spawn under job
HANDLE spawn_in_job(HANDLE job, const std::wstring& cmd, HANDLE stdin_h, HANDLE stdout_h,
                    HANDLE stderr_h) {
    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdin_h;
    si.hStdOutput = stdout_h;
    si.hStdError  = stderr_h;

    PROCESS_INFORMATION pi{};
    std::wstring mut = cmd;
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return nullptr;
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

} // namespace

struct SpotifySource::Pipe {
    HANDLE job         = nullptr;
    HANDLE proc_spot   = nullptr;
    HANDLE proc_ff     = nullptr;
    HANDLE read_pipe   = nullptr;
    // log parsing
    HANDLE err_pipe    = nullptr; 
    HANDLE log_file    = nullptr;
    std::string err_buf;
    bool ended         = false;

    ~Pipe() {
        if (read_pipe)  CloseHandle(read_pipe);
        if (err_pipe)   CloseHandle(err_pipe);
        if (log_file && log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
        if (job)        CloseHandle(job);
        if (proc_spot)  CloseHandle(proc_spot);
        if (proc_ff)    CloseHandle(proc_ff);
    }
};

SpotifySource::SpotifySource(SpotifyConfig cfg) : cfg_{std::move(cfg)} {
    info_.title = "Ready to Cast";
    info_.artist = "Spotify Connect";
}

SpotifySource::~SpotifySource() { stop_pipe_locked(); }

bool SpotifySource::initialize() {
    if (!cfg_.enabled) return false;
    
    // ensure cache directory exists
    std::error_code ec;
    std::filesystem::create_directories(cfg_.cache_dir, ec);
    
    return true;
}

void SpotifySource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

bool SpotifySource::cache_exists() const {
    auto creds_path = cfg_.cache_dir / "credentials.json";
    return std::filesystem::exists(creds_path);
}

AuthState SpotifySource::auth_state() const noexcept {
    return cache_exists() ? AuthState::authenticated : AuthState::needs_auth;
}

std::string SpotifySource::auth_instructions() const {
    return "1. Ensure your PC and phone are on the same Wi-Fi network.\n"
           "2. Open the Spotify app on your phone.\n"
           "3. Tap the 'Devices' icon and select 'FH6 Radio'.\n"
           "Once connected, credentials will automatically save to the cache folder.";
}

void SpotifySource::start_pipe_locked() {
    stop_pipe_locked();

    auto pipe = std::make_unique<Pipe>();
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[spotify] CreateJobObject failed ({})", GetLastError());
        return;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE spot_out_r = nullptr, spot_out_w = nullptr;
    HANDLE spot_err_r = nullptr, spot_err_w = nullptr;
    HANDLE ff_out_r = nullptr, ff_out_w = nullptr;

    auto bail = [&] {
        if (spot_out_r) CloseHandle(spot_out_r);
        if (spot_out_w) CloseHandle(spot_out_w);
        if (spot_err_r) CloseHandle(spot_err_r);
        if (spot_err_w) CloseHandle(spot_err_w);
        if (ff_out_r) CloseHandle(ff_out_r);
        if (ff_out_w) CloseHandle(ff_out_w);
    };

    // reduced pipe size to 4KB (OS minimum) to eliminate residual data backlog
    if (!CreatePipe(&spot_out_r, &spot_out_w, &sa, 4096)) { bail(); return; }
    SetHandleInformation(spot_out_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    // create error pipe and ensure read end isn't passed to children
    if (!CreatePipe(&spot_err_r, &spot_err_w, &sa, 4096)) { bail(); return; }
    SetHandleInformation(spot_err_r, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 4096)) { bail(); return; }
    SetHandleInformation(ff_out_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    pipe->log_file = open_stderr_log(); // keep the log file open in our Pipe struct

    const auto spot = cfg_.librespot_path.empty() ? L"librespot" : cfg_.librespot_path.wstring();
    const auto ff   = cfg_.ffmpeg_path.empty() ? L"ffmpeg" : cfg_.ffmpeg_path.wstring();
    const auto cache = cfg_.cache_dir.wstring();

    std::wstring spot_cmd = quote(spot) + 
                            L" --name \"FH6 Radio\"" +
                            L" --bitrate 320" +
                            L" --backend pipe" +
                            L" --initial-volume 100" +
                            L" --cache " + quote(cache);

    // librespot defaults to 44100Hz s16le. We must resample to 48000Hz for FH6.
    // added flags to disable FFmpeg internal buffering for perfect UI sync
    std::wstring ff_cmd = quote(ff) + 
                          L" -loglevel error" +
                          L" -fflags nobuffer -flags low_delay" + 
                          L" -blocksize 4096" + // force micro-block processing
                          L" -f s16le -ar 44100 -ac 2 -i pipe:0" +
                          L" -flush_packets 1" + 
                          L" -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    // pass spot_err_w to librespot instead of the raw file
    pipe->proc_spot = spawn_in_job(pipe->job, spot_cmd, nul_in, spot_out_w, spot_err_w);
    CloseHandle(spot_out_w); spot_out_w = nullptr;
    CloseHandle(spot_err_w); spot_err_w = nullptr;
    
    if (!pipe->proc_spot) {
        log::warn("[spotify] failed to launch librespot ({}) -- check {}", GetLastError(),
                  stderr_log_path().string());
        bail();
        if (nul_in)  CloseHandle(nul_in);
        return;
    }

    // FFmpeg can keep logging to the file directly
    pipe->proc_ff = spawn_in_job(pipe->job, ff_cmd, spot_out_r, ff_out_w, pipe->log_file);
    CloseHandle(spot_out_r); spot_out_r = nullptr;
    CloseHandle(ff_out_w);   ff_out_w = nullptr;

    if (!pipe->proc_ff) {
        log::warn("[spotify] failed to launch ffmpeg ({}) -- check {}", GetLastError(),
                  stderr_log_path().string());
        bail(); // clean up unassigned pipe handles
        if (nul_in)   CloseHandle(nul_in);
        return;
    }

    pipe->err_pipe  = spot_err_r; // save our read handle
    pipe->read_pipe = ff_out_r;
    pipe_           = std::move(pipe);

    info_.title = "Streaming via Spotify Connect";
    info_.artist = "Spotify";
    state_.store(PlaybackState::playing, std::memory_order_release);

    log::info("[spotify] librespot pipe started (listening on network)");
}

void SpotifySource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void SpotifySource::play() {
    std::scoped_lock lk{mu_};
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void SpotifySource::pause() {
    // let the process run so it stays visible on the network, 
    // but stop pumping audio into the ring buffer.
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void SpotifySource::stop() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

TrackInfo SpotifySource::current_track() const {
    std::scoped_lock lk{mu_};
    return info_;
}

void SpotifySource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p || !p->read_pipe || p->ended) return;

    // non-blocking parse of track metadata
    if (p->err_pipe) {
        DWORD err_avail = 0;
        while (PeekNamedPipe(p->err_pipe, nullptr, 0, nullptr, &err_avail, nullptr) && err_avail > 0) {
            char buf[1024];
            DWORD to_read = std::min<DWORD>(err_avail, (DWORD)sizeof(buf));
            DWORD got = 0;
            if (!ReadFile(p->err_pipe, buf, to_read, &got, nullptr) || got == 0) break;
            
            // tee the output to our log file so you can still check it on disk
            if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                WriteFile(p->log_file, buf, got, &w, nullptr);
            }
            
            p->err_buf.append(buf, got);
            
            // process all complete lines
            size_t pos;
            while ((pos = p->err_buf.find('\n')) != std::string::npos) {
                std::string line = p->err_buf.substr(0, pos);
                p->err_buf.erase(0, pos + 1);
                
                // strip Windows carriage return if it exists
                if (!line.empty() && line.back() == '\r') line.pop_back();
                
                // look for the track load event signature
                const std::string marker = "librespot_playback::player] <";
                size_t m_pos = line.find(marker);
                if (m_pos != std::string::npos) {
                    size_t start = m_pos + marker.length();
                    size_t end = line.find("> (", start);
                    if (end != std::string::npos) {
                        info_.title = line.substr(start, end - start);
                        // optionally set the artist field since librespot clumps them together
                        info_.artist = "Spotify Connect"; 
                    }
                }
            }
        }
    }

    // cap the ring buffer at 0.15 seconds (150ms)
    // 48000Hz * 2ch * 2 bytes * 0.15s = 28800 bytes
    const std::size_t MAX_BUFFER_BYTES = 28800;

    if (ring.readable() >= MAX_BUFFER_BYTES) return;

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        p->ended = true;
        return;
    }
    
    while (avail > 0) {
        // stop reading if we hit our buffer cap
        if (ring.readable() >= MAX_BUFFER_BYTES) break;

        const std::size_t writable = ring.writable();
        if (writable < 4) break;
        
        std::size_t want = std::min<std::size_t>(writable, avail);
        
        // do not read more than what keeps us under the buffer limit
        std::size_t allowed = MAX_BUFFER_BYTES - ring.readable();
        if (want > allowed) want = allowed;
        
        if (want > 4096) want = 4096;
        
        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            p->ended = true;
            return;
        }
        ring.write(buf, got);
        avail = avail > got ? avail - got : 0;
    }
}

} // namespace fh6::sources