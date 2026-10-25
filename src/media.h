#pragma once

#include "common.h"
#include "process.h"

#include <memory>
#include <string>
#include <vector>

namespace ap {

struct MediaInfo {
    double duration = 0.0;
    bool hasVideo = false;
    bool hasAudio = false;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int rotation = 0;
    int audioSampleRate = 0;
    int audioChannels = 0;
    std::string videoCodec;
    std::string audioCodec;
};

struct InputSpec {
    fs::path path;
    fs::path init;
    bool isConcat() const { return !init.empty(); }
    std::wstring ffmpegInput() const;
    std::string display() const;
};

class FFmpeg {
public:
    static FFmpeg locate();
    const fs::path& path() const { return path_; }
    MediaInfo probe(const InputSpec& input) const;
    ProcessResult runMerged(const std::vector<std::wstring>& args) const;
    ProcessResult runQuiet(const std::vector<std::wstring>& args) const;

private:
    explicit FFmpeg(fs::path path) : path_(std::move(path)) {}
    fs::path path_;
};

class VideoStream {
public:
    VideoStream(const FFmpeg& ffmpeg, const InputSpec& input, int outWidth, int outHeight,
                double fps, double startSeconds, double durationSeconds);
    ~VideoStream();

    bool next(std::vector<uint8_t>& frame);
    int framesRead() const { return framesRead_; }
    const std::string& errorText() const { return errorText_; }
    int finish();
    void cancel();

private:
    std::unique_ptr<PipeProcess> process_;
    size_t frameBytes_ = 0;
    int framesRead_ = 0;
    std::string errorText_;
};

std::wstring numberArg(double value);
std::wstring intArg(int value);

}  // namespace ap
