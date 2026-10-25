#pragma once

#include "common.h"

#include <mmsystem.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ap {

class WavData {
public:
    WavData() = default;
    ~WavData();
    WavData(const WavData&) = delete;
    WavData& operator=(const WavData&) = delete;
    WavData(WavData&& other) noexcept;
    WavData& operator=(WavData&& other) noexcept;

    void open(const fs::path& path);
    bool isOpen() const { return data_ != nullptr; }
    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    int bitsPerSample() const { return bitsPerSample_; }
    int blockAlign() const { return blockAlign_; }
    size_t frameCount() const { return frameCount_; }
    double duration() const {
        return sampleRate_ > 0 ? static_cast<double>(frameCount_) / sampleRate_ : 0.0;
    }
    const uint8_t* data() const { return data_; }
    size_t dataBytes() const { return dataBytes_; }

    // Mixes a window of signed 16-bit samples to mono floats in [-1, 1].
    void monoWindow(double startSeconds, int sampleCount, std::vector<float>& out) const;

private:
    void close();

    HANDLE file_ = nullptr;
    HANDLE mapping_ = nullptr;
    const uint8_t* view_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t dataBytes_ = 0;
    int sampleRate_ = 0;
    int channels_ = 0;
    int bitsPerSample_ = 0;
    int blockAlign_ = 0;
    size_t frameCount_ = 0;
};

class WaveOutPlayer {
public:
    WaveOutPlayer() = default;
    ~WaveOutPlayer();
    WaveOutPlayer(const WaveOutPlayer&) = delete;
    WaveOutPlayer& operator=(const WaveOutPlayer&) = delete;

    bool start(const WavData& audio, std::string& error);
    void pump();
    void stop();
    bool started() const { return started_; }
    bool failed() const { return failed_; }
    bool finished() const;
    double positionSeconds() const;
    double durationSeconds() const { return audio_ ? audio_->duration() : 0.0; }

private:
    struct Buffer {
        std::vector<uint8_t> bytes;
        WAVEHDR header{};
        bool queued = false;
    };

    void close();
    size_t fillBuffer(Buffer& buffer);

    HWAVEOUT device_ = nullptr;
    const WavData* audio_ = nullptr;
    std::vector<Buffer> buffers_;
    size_t readOffset_ = 0;
    size_t queued_ = 0;
    bool started_ = false;
    bool failed_ = false;
    bool closed_ = false;
    mutable double lastPosition_ = 0.0;
};

}  // namespace ap
