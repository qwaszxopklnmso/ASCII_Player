#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <mmsystem.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace ap {

namespace {

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

#ifdef _WIN32
std::string waveOutError(MMRESULT result) {
    switch (result) {
        case MMSYSERR_NODRIVER:
            return "no audio output driver";
        case MMSYSERR_ALLOCATED:
            return "audio device is already in use";
        case MMSYSERR_BADDEVICEID:
            return "invalid audio device";
        case WAVERR_BADFORMAT:
            return "audio format is not supported";
        default:
            return "waveOut error " + std::to_string(static_cast<unsigned>(result));
    }
}
#endif

}  // namespace

WavData::~WavData() {
    close();
}

WavData::WavData(WavData&& other) noexcept
    : path_(std::move(other.path_)),
#ifdef _WIN32
      file_(other.file_),
      mapping_(other.mapping_),
      view_(other.view_),
#else
      fd_(other.fd_),
      map_(other.map_),
      mapSize_(other.mapSize_),
#endif
      data_(other.data_),
      dataBytes_(other.dataBytes_),
      sampleRate_(other.sampleRate_),
      channels_(other.channels_),
      bitsPerSample_(other.bitsPerSample_),
      blockAlign_(other.blockAlign_),
      frameCount_(other.frameCount_) {
#ifdef _WIN32
    other.file_ = nullptr;
    other.mapping_ = nullptr;
    other.view_ = nullptr;
#else
    other.fd_ = -1;
    other.map_ = nullptr;
    other.mapSize_ = 0;
#endif
    other.data_ = nullptr;
    other.dataBytes_ = 0;
}

WavData& WavData::operator=(WavData&& other) noexcept {
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
#ifdef _WIN32
        file_ = other.file_;
        mapping_ = other.mapping_;
        view_ = other.view_;
        other.file_ = nullptr;
        other.mapping_ = nullptr;
        other.view_ = nullptr;
#else
        fd_ = other.fd_;
        map_ = other.map_;
        mapSize_ = other.mapSize_;
        other.fd_ = -1;
        other.map_ = nullptr;
        other.mapSize_ = 0;
#endif
        data_ = other.data_;
        dataBytes_ = other.dataBytes_;
        sampleRate_ = other.sampleRate_;
        channels_ = other.channels_;
        bitsPerSample_ = other.bitsPerSample_;
        blockAlign_ = other.blockAlign_;
        frameCount_ = other.frameCount_;
        other.data_ = nullptr;
        other.dataBytes_ = 0;
    }
    return *this;
}

void WavData::close() {
#ifdef _WIN32
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    data_ = nullptr;
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (file_ && file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
    }
    file_ = nullptr;
#else
    if (map_ && map_ != MAP_FAILED && mapSize_ > 0) {
        munmap(map_, mapSize_);
        map_ = nullptr;
        mapSize_ = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    data_ = nullptr;
#endif
    dataBytes_ = 0;
}

void WavData::open(const fs::path& path) {
    close();
    path_ = path;
#ifdef _WIN32
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(file_ != INVALID_HANDLE_VALUE,
          "Could not open extracted audio: " + pathToUtf8(path));
    LARGE_INTEGER size{};
    check(GetFileSizeEx(file_, &size) && size.QuadPart >= 44,
          "Extracted audio file is empty.");
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    check(mapping_ != nullptr, "Could not map extracted audio.");
    const uint8_t* base =
        static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    check(base != nullptr, "Could not view extracted audio.");
    view_ = base;
    data_ = base;
    size_t total = static_cast<size_t>(size.QuadPart);
#else
    fd_ = ::open(path.c_str(), O_RDONLY);
    check(fd_ >= 0, "Could not open extracted audio: " + pathToUtf8(path));
    struct stat st {};
    check(::fstat(fd_, &st) == 0, "Could not stat extracted audio.");
    check(st.st_size >= 44, "Extracted audio file is empty.");
    mapSize_ = static_cast<size_t>(st.st_size);
    map_ = mmap(nullptr, mapSize_, PROT_READ, MAP_PRIVATE, fd_, 0);
    check(map_ != MAP_FAILED, "Could not map extracted audio.");
    const uint8_t* base = static_cast<const uint8_t*>(map_);
    data_ = base;
    size_t total = mapSize_;
#endif

    check(total >= 12 && std::memcmp(base, "RIFF", 4) == 0 &&
              std::memcmp(base + 8, "WAVE", 4) == 0,
          "Extracted audio is not a RIFF/WAVE file.");
    size_t offset = 12;
    bool haveFmt = false;
    size_t dataOffset = 0;
    size_t dataSize = 0;
    while (offset + 8 <= total) {
        const uint8_t* chunk = base + offset;
        uint32_t chunkSize = readLe32(chunk + 4);
        size_t body = offset + 8;
        if (body + chunkSize > total) chunkSize = static_cast<uint32_t>(total - body);
        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
            uint16_t format = readLe16(base + body);
            channels_ = readLe16(base + body + 2);
            sampleRate_ = static_cast<int>(readLe32(base + body + 4));
            blockAlign_ = readLe16(base + body + 12);
            bitsPerSample_ = readLe16(base + body + 14);
            haveFmt = (format == 1 || format == 0xFFFE) && channels_ > 0 &&
                      sampleRate_ > 0 && bitsPerSample_ == 16 && blockAlign_ > 0;
            check(haveFmt, "Extracted audio is not 16-bit PCM WAV.");
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            dataOffset = body;
            dataSize = chunkSize;
            break;
        }
        offset = body + chunkSize + (chunkSize & 1u);
    }
    check(haveFmt && dataOffset > 0 && dataSize > 0, "WAVE data chunk not found.");
    data_ = base + dataOffset;
    dataBytes_ = dataSize;
    frameCount_ = dataBytes_ / static_cast<size_t>(blockAlign_);
    check(frameCount_ > 0, "WAVE contains no audio frames.");
}

void WavData::monoWindow(double startSeconds, int sampleCount,
                         std::vector<float>& out) const {
    out.assign(static_cast<size_t>(std::max(0, sampleCount)), 0.0f);
    if (!data_ || sampleRate_ <= 0 || channels_ <= 0 || sampleCount <= 0) return;
    long long first = static_cast<long long>(std::floor(startSeconds * sampleRate_));
    for (int i = 0; i < sampleCount; ++i) {
        long long frame = first + i;
        if (frame < 0 || static_cast<size_t>(frame) >= frameCount_) continue;
        const uint8_t* p = data_ + static_cast<size_t>(frame) * blockAlign_;
        float sum = 0.0f;
        for (int c = 0; c < channels_; ++c) {
            int16_t sample = 0;
            std::memcpy(&sample, p + c * 2, sizeof(sample));
            sum += static_cast<float>(sample) / 32768.0f;
        }
        out[static_cast<size_t>(i)] = sum / static_cast<float>(channels_);
    }
}

#ifdef _WIN32

WaveOutPlayer::~WaveOutPlayer() {
    close();
}

size_t WaveOutPlayer::fillBuffer(Buffer& buffer) {
    if (!audio_ || readOffset_ >= audio_->dataBytes()) return 0;
    size_t room = audio_->dataBytes() - readOffset_;
    size_t bytes = std::min(buffer.bytes.size(), room);
    bytes -= bytes % static_cast<size_t>(audio_->blockAlign());
    if (bytes == 0) return 0;
    std::memcpy(buffer.bytes.data(), audio_->data() + readOffset_, bytes);
    readOffset_ += bytes;
    return bytes;
}

bool WaveOutPlayer::start(const WavData& audio, std::string& error) {
    stop();
    audio_ = &audio;
    readOffset_ = 0;
    queued_ = 0;
    started_ = false;
    failed_ = false;
    closed_ = false;
    lastPosition_ = 0.0;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = static_cast<WORD>(audio.channels());
    wfx.nSamplesPerSec = static_cast<DWORD>(audio.sampleRate());
    wfx.wBitsPerSample = static_cast<WORD>(audio.bitsPerSample());
    wfx.nBlockAlign = static_cast<WORD>(audio.blockAlign());
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    MMRESULT result = waveOutOpen(&device_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        error = "Audio disabled: " + waveOutError(result) + ".";
        failed_ = true;
        device_ = nullptr;
        return false;
    }

    const size_t bufferBytes =
        static_cast<size_t>(std::max(1024, audio.sampleRate() * audio.blockAlign() / 25));
    constexpr size_t kBufferCount = 8;
    buffers_.clear();
    buffers_.resize(kBufferCount);
    for (auto& buffer : buffers_) {
        buffer.bytes.resize(bufferBytes);
        buffer.header = {};
        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.bytes.data());
        buffer.header.dwBufferLength = 0;
        buffer.header.dwUser = 0;
        size_t bytes = fillBuffer(buffer);
        if (bytes == 0) break;
        buffer.header.dwBufferLength = static_cast<DWORD>(bytes);
        if (waveOutPrepareHeader(device_, &buffer.header, sizeof(WAVEHDR)) !=
            MMSYSERR_NOERROR) {
            error = "Audio disabled: could not prepare playback buffer.";
            failed_ = true;
            close();
            return false;
        }
        if (waveOutWrite(device_, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            error = "Audio disabled: could not queue playback buffer.";
            failed_ = true;
            close();
            return false;
        }
        buffer.queued = true;
        ++queued_;
    }
    if (queued_ == 0) {
        error = "Audio disabled: no playable samples.";
        failed_ = true;
        close();
        return false;
    }
    started_ = true;
    return true;
}

void WaveOutPlayer::pump() {
    if (!device_ || !started_ || closed_) return;
    for (auto& buffer : buffers_) {
        if (!buffer.queued) continue;
        if ((buffer.header.dwFlags & WHDR_DONE) == 0) continue;
        waveOutUnprepareHeader(device_, &buffer.header, sizeof(WAVEHDR));
        buffer.queued = false;
        if (queued_ > 0) --queued_;
        size_t bytes = fillBuffer(buffer);
        if (bytes == 0) continue;
        buffer.header.dwBufferLength = static_cast<DWORD>(bytes);
        buffer.header.dwFlags = 0;
        if (waveOutPrepareHeader(device_, &buffer.header, sizeof(WAVEHDR)) !=
            MMSYSERR_NOERROR) {
            failed_ = true;
            continue;
        }
        if (waveOutWrite(device_, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            failed_ = true;
            continue;
        }
        buffer.queued = true;
        ++queued_;
    }
    MMTIME time{};
    time.wType = TIME_SAMPLES;
    if (waveOutGetPosition(device_, &time, sizeof(time)) == MMSYSERR_NOERROR &&
        time.wType == TIME_SAMPLES) {
        double seconds =
            static_cast<double>(time.u.sample) / static_cast<double>(audio_->sampleRate());
        lastPosition_ = std::min(seconds, audio_->duration());
    }
}

bool WaveOutPlayer::finished() const {
    if (!audio_ || !started_) return true;
    if (readOffset_ < audio_->dataBytes() || queued_ > 0) return false;
    return true;
}

double WaveOutPlayer::positionSeconds() const {
    return lastPosition_;
}

void WaveOutPlayer::close() {
    if (closed_) return;
    closed_ = true;
    if (device_) {
        waveOutReset(device_);
        for (auto& buffer : buffers_) {
            if (buffer.header.dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(device_, &buffer.header, sizeof(WAVEHDR));
            }
            buffer.queued = false;
        }
        waveOutClose(device_);
        device_ = nullptr;
    }
    buffers_.clear();
    queued_ = 0;
    started_ = false;
}

void WaveOutPlayer::stop() {
    close();
    audio_ = nullptr;
    readOffset_ = 0;
    lastPosition_ = 0.0;
}

#else  // POSIX: play the extracted WAV through a small command-line player.

WaveOutPlayer::~WaveOutPlayer() {
    close();
}

bool WaveOutPlayer::spawnPlayer(const fs::path& exe,
                                const std::vector<std::string>& args,
                                std::string& error) {
    std::vector<std::string> command;
    command.reserve(args.size() + 1);
    command.push_back(pathToUtf8(exe));
    command.insert(command.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (auto& item : command) argv.push_back(item.data());
    argv.push_back(nullptr);

    int errorPipe[2] = {-1, -1};
    if (::pipe(errorPipe) != 0) {
        error = std::string("could not create exec pipe: ") + std::strerror(errno);
        return false;
    }

    pid_t child = ::fork();
    if (child < 0) {
        int forkError = errno;
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);
        error = std::string("could not fork: ") + std::strerror(forkError);
        return false;
    }

    if (child == 0) {
        ::close(errorPipe[0]);
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull < 0) {
            int openError = errno;
            ssize_t ignored = ::write(errorPipe[1], &openError, sizeof(openError));
            (void)ignored;
            _exit(127);
        }
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) ::close(devnull);

        // Close the error pipe automatically only if execve() succeeds.
        ::fcntl(errorPipe[1], F_SETFD, ::fcntl(errorPipe[1], F_GETFD) | FD_CLOEXEC);
        ::execv(command.front().c_str(), argv.data());

        int execError = errno;
        ssize_t ignored = ::write(errorPipe[1], &execError, sizeof(execError));
        (void)ignored;
        _exit(127);
    }

    ::close(errorPipe[1]);
    int execError = 0;
    ssize_t got = 0;
    do {
        got = ::read(errorPipe[0], &execError, sizeof(execError));
    } while (got < 0 && errno == EINTR);
    ::close(errorPipe[0]);

    if (got > 0) {
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        error = std::strerror(execError);
        return false;
    }

    process_ = child;
    return true;
}

bool WaveOutPlayer::start(const WavData& audio, std::string& error) {
    stop();
    audio_ = &audio;
    started_ = false;
    failed_ = false;
    closed_ = false;
    lastPosition_ = 0.0;
    processExited_ = false;
    sinkName_.clear();

    if (audio.path().empty()) {
        error = "Audio disabled: the extracted WAV path is unavailable.";
        failed_ = true;
        return false;
    }
    const std::string wav = pathToUtf8(audio.path());

    struct Candidate {
        const char* name;
        std::vector<std::string> args;
    };
    const std::vector<Candidate> candidates = {
        {"paplay", {wav}},
        {"pw-play", {wav}},
        {"aplay", {"-q", wav}},
        {"ffplay", {"-nodisp", "-autoexit", "-loglevel", "quiet", wav}},
        {"mpv", {"--no-video", "--really-quiet", wav}},
        {"play", {"-q", wav}},
        {"cvlc", {"--play-and-exit", "--quiet", wav}},
    };

    std::string lastError;
    for (const auto& candidate : candidates) {
        fs::path exe = findInPath(utf8ToWide(candidate.name));
        if (exe.empty()) continue;
        std::string spawnError;
        if (spawnPlayer(exe, candidate.args, spawnError)) {
            sinkName_ = candidate.name;
            startTime_ = std::chrono::steady_clock::now();
            started_ = true;
            return true;
        }
        lastError = candidate.name;
        if (!spawnError.empty()) lastError += ": " + spawnError;
    }

    failed_ = true;
    error =
        "Audio disabled: no command-line audio player found"
        + (lastError.empty() ? std::string(".") : " (" + lastError + ").") +
        "\n  Install pulseaudio-utils (paplay), pipewire-bin (pw-play), "
        "alsa-utils (aplay), ffmpeg (ffplay), or mpv.";
    return false;
}

void WaveOutPlayer::pump() {
    if (!started_ || closed_ || !audio_) return;
    if (process_ > 0 && !processExited_) {
        int status = 0;
        pid_t got = ::waitpid(process_, &status, WNOHANG);
        if (got == process_) {
            processExited_ = true;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failed_ = true;
        }
    }
    double elapsed = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - startTime_)
                         .count();
    lastPosition_ = std::min(std::max(0.0, elapsed), audio_->duration());
}

bool WaveOutPlayer::finished() const {
    if (!audio_ || !started_) return true;
    if (processExited_ || failed_) return true;
    return lastPosition_ >= audio_->duration();
}

double WaveOutPlayer::positionSeconds() const {
    return lastPosition_;
}

void WaveOutPlayer::close() {
    if (closed_) return;
    closed_ = true;
    if (process_ > 0 && !processExited_) {
        ::kill(process_, SIGTERM);
        for (int i = 0; i < 100; ++i) {
            int status = 0;
            pid_t got = ::waitpid(process_, &status, WNOHANG);
            if (got == process_) {
                processExited_ = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!processExited_) {
            ::kill(process_, SIGKILL);
            int status = 0;
            while (::waitpid(process_, &status, 0) < 0 && errno == EINTR) {
            }
            processExited_ = true;
        }
    }
    process_ = -1;
    processExited_ = false;
    started_ = false;
}

void WaveOutPlayer::stop() {
    close();
    audio_ = nullptr;
    lastPosition_ = 0.0;
    sinkName_.clear();
}

#endif

}  // namespace ap
