#pragma once

#include "audio.h"
#include "common.h"

#include <algorithm>
#include <string>
#include <vector>

#ifndef _WIN32
#include <termios.h>
#endif

namespace ap {

struct RGB {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

enum class ColorMode { Mono, Ansi256, TrueColor };

// Light-to-dark default ramp, ordered by measured glyph coverage.
inline constexpr const char kDefaultCharset[] =
    " `.'-:,_\"^~;!><+=/\\|"
    "r*L?(c)Tv7Jz]iY1Fxls"
    "fInu{C[2te}oZj3ayE5V"
    "4kPwhSmAUXK69bdqpGO#"
    "RD%8HMWB0$g&NQ@";

struct RenderSettings {
    std::string charset = kDefaultCharset;
    double gamma = 0.85;
    double colorGain = 1.0;
    bool invert = false;
    ColorMode colorMode = ColorMode::TrueColor;
};

struct AsciiFrame {
    int width = 0;
    int height = 0;
    std::vector<std::string> rows;
    std::vector<RGB> colors;

    void reset(int w, int h) {
        width = w;
        height = h;
        rows.assign(static_cast<size_t>(h), std::string(static_cast<size_t>(w), ' '));
        colors.assign(static_cast<size_t>(w) * static_cast<size_t>(h), RGB{});
    }
    char at(int x, int y) const {
        return rows[static_cast<size_t>(y)][static_cast<size_t>(x)];
    }
    RGB colorAt(int x, int y) const {
        return colors[static_cast<size_t>(y) * static_cast<size_t>(width) +
                      static_cast<size_t>(x)];
    }
    void set(int x, int y, char c, RGB color) {
        rows[static_cast<size_t>(y)][static_cast<size_t>(x)] = c;
        colors[static_cast<size_t>(y) * static_cast<size_t>(width) +
               static_cast<size_t>(x)] = color;
    }
};

AsciiFrame renderRgbFrame(const uint8_t* rgb, int width, int height, int stride,
                          const RenderSettings& settings);

int quantizeXterm(RGB color);
RGB xtermColor(int index);
RGB hslToRgb(double h, double s, double l);

class Terminal {
public:
    Terminal(bool plain, bool altScreen);
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void enter();
    void leave();
    void draw(const AsciiFrame& frame, const std::string& status, ColorMode mode,
              bool center = false);
    void clear();
    int width() const { return width_; }
    int height() const { return height_; }
    int availableWidth() const { return width_; }
    int availableHeight() const { return std::max(1, height_ - 1); }
    bool isTty() const { return tty_; }
    bool quitRequested() const;

private:
    std::string colorSequence(RGB color, ColorMode mode, int& lastKey);
    void write(const std::string& text);

#ifdef _WIN32
    HANDLE out_ = INVALID_HANDLE_VALUE;
    DWORD originalMode_ = 0;
#else
    int outFd_ = 1;
    struct termios original_{};
    bool rawMode_ = false;
    int stdinFd_ = 0;
#endif
    bool tty_ = false;
    bool plain_ = false;
    bool altScreen_ = false;
    bool entered_ = false;
    int width_ = 100;
    int height_ = 30;
};

class AudioVisualizer {
public:
    AudioVisualizer(const WavData& audio, std::string title);
    AsciiFrame render(double timeSeconds, int width, int height, ColorMode mode);

private:
    void fft(std::vector<float>& real, std::vector<float>& imag) const;
    const WavData& audio_;
    std::string title_;
    std::vector<float> smooth_;
    std::vector<float> peaks_;
    std::vector<float> window_;
    std::vector<float> samples_;
    std::vector<float> real_;
    std::vector<float> imag_;
};

}  // namespace ap
