#pragma once

#include "common.h"
#include "render.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ap {

struct SceneFrame {
    double time = 0.0;
    std::vector<std::string> rows;
    std::vector<uint8_t> colors;
};

struct Scene {
    std::string source;
    std::string charset = kDefaultCharset;
    std::string colorMode = "ansi256";
    int width = 0;
    int height = 0;
    double fps = 30.0;
    double duration = 0.0;
    bool hasColors = true;
    std::vector<SceneFrame> frames;
};

class SceneWriter {
public:
    SceneWriter(const fs::path& path, int width, int height, double fps,
                double duration, const std::string& source, const std::string& charset,
                const std::string& colorMode);
    ~SceneWriter();
    void writeFrame(const AsciiFrame& frame, double timeSeconds);
    void finish();
    int frameCount() const { return frameCount_; }
    uint64_t bytesWritten() const { return bytesWritten_; }

private:
    void write(const std::string& text);
    std::ofstream out_;
    fs::path path_;
    int width_ = 0;
    int height_ = 0;
    std::string source_;
    std::string charset_;
    std::string colorMode_;
    int frameCount_ = 0;
    uint64_t bytesWritten_ = 0;
    bool finished_ = false;
};

Scene loadScene(const fs::path& path);
AsciiFrame sceneFrameToAscii(const Scene& scene, const SceneFrame& frame);

}  // namespace ap
