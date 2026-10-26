#include "render.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <cerrno>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace ap {

namespace {

const uint8_t kCubeLevels[6] = {0, 95, 135, 175, 215, 255};

int nearestCubeLevel(int value) {
    int best = 0;
    int bestDistance = 1 << 30;
    for (int i = 0; i < 6; ++i) {
        int d = std::abs(value - static_cast<int>(kCubeLevels[i]));
        if (d < bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

RGB cubeColor(int r, int g, int b) {
    return RGB{kCubeLevels[r], kCubeLevels[g], kCubeLevels[b]};
}

int colorDistanceSq(RGB a, RGB b) {
    int dr = static_cast<int>(a.r) - static_cast<int>(b.r);
    int dg = static_cast<int>(a.g) - static_cast<int>(b.g);
    int db = static_cast<int>(a.b) - static_cast<int>(b.b);
    return dr * dr + dg * dg + db * db;
}

std::string truncateAscii(const std::string& text, size_t width) {
    if (text.size() <= width) return text;
    if (width <= 3) return text.substr(0, width);
    return text.substr(0, width - 3) + "...";
}

}  // namespace

int quantizeXterm(RGB color) {
    int r = nearestCubeLevel(color.r);
    int g = nearestCubeLevel(color.g);
    int b = nearestCubeLevel(color.b);
    int cubeIndex = 16 + 36 * r + 6 * g + b;
    RGB cube = cubeColor(r, g, b);

    int average = (static_cast<int>(color.r) + color.g + color.b) / 3;
    int grayIndex = static_cast<int>(std::lround((average - 8.0) / 10.0));
    grayIndex = std::clamp(grayIndex, 0, 23);
    int grayValue = 8 + grayIndex * 10;
    RGB gray{static_cast<uint8_t>(grayValue), static_cast<uint8_t>(grayValue),
             static_cast<uint8_t>(grayValue)};
    int grayColorIndex = 232 + grayIndex;

    if (colorDistanceSq(color, gray) < colorDistanceSq(color, cube))
        return grayColorIndex;
    return cubeIndex;
}

RGB xtermColor(int index) {
    static const RGB base[16] = {
        {0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},
        {0, 0, 128},     {128, 0, 128},   {0, 128, 128},   {192, 192, 192},
        {128, 128, 128}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
        {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255}};
    index = std::clamp(index, 0, 255);
    if (index < 16) return base[index];
    if (index < 232) {
        int n = index - 16;
        int r = n / 36;
        int g = (n % 36) / 6;
        int b = n % 6;
        return RGB{kCubeLevels[r], kCubeLevels[g], kCubeLevels[b]};
    }
    int gray = 8 + (index - 232) * 10;
    return RGB{static_cast<uint8_t>(gray), static_cast<uint8_t>(gray),
               static_cast<uint8_t>(gray)};
}

RGB hslToRgb(double h, double s, double l) {
    h = std::fmod(h, 360.0);
    if (h < 0) h += 360.0;
    s = std::clamp(s, 0.0, 1.0);
    l = std::clamp(l, 0.0, 1.0);
    double c = (1.0 - std::abs(2.0 * l - 1.0)) * s;
    double x = c * (1.0 - std::abs(std::fmod(h / 60.0, 2.0) - 1.0));
    double m = l - c / 2.0;
    double r = 0, g = 0, b = 0;
    if (h < 60) {
        r = c;
        g = x;
    } else if (h < 120) {
        r = x;
        g = c;
    } else if (h < 180) {
        g = c;
        b = x;
    } else if (h < 240) {
        g = x;
        b = c;
    } else if (h < 300) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    auto channel = [&](double v) {
        return static_cast<uint8_t>(std::clamp((v + m) * 255.0, 0.0, 255.0));
    };
    return RGB{channel(r), channel(g), channel(b)};
}

AsciiFrame renderRgbFrame(const uint8_t* rgb, int width, int height, int stride,
                          const RenderSettings& settings) {
    AsciiFrame frame;
    frame.reset(width, height);
    const std::string& ramp = settings.charset;
    if (ramp.empty()) return frame;
    const int levels = static_cast<int>(ramp.size());
    for (int y = 0; y < height; ++y) {
        const uint8_t* line = rgb + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = line + static_cast<size_t>(x) * 3;
            double r = p[0] / 255.0;
            double g = p[1] / 255.0;
            double b = p[2] / 255.0;
            double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            double value = std::max({r, g, b});
            double luminance = 0.78 * luma + 0.22 * value;
            if (settings.gamma != 1.0 && settings.gamma > 0.0)
                luminance = std::pow(std::clamp(luminance, 0.0, 1.0), settings.gamma);
            if (settings.invert) luminance = 1.0 - luminance;
            int index = static_cast<int>(std::lround(
                std::clamp(luminance, 0.0, 1.0) * static_cast<double>(levels - 1)));
            index = std::clamp(index, 0, levels - 1);
            double gain = settings.colorGain;
            RGB color{
                static_cast<uint8_t>(std::clamp(p[0] * gain, 0.0, 255.0)),
                static_cast<uint8_t>(std::clamp(p[1] * gain, 0.0, 255.0)),
                static_cast<uint8_t>(std::clamp(p[2] * gain, 0.0, 255.0))};
            frame.set(x, y, ramp[static_cast<size_t>(index)], color);
        }
    }
    return frame;
}

#ifdef _WIN32

Terminal::Terminal(bool plain, bool altScreen)
    : plain_(plain), altScreen_(altScreen) {
    out_ = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    tty_ = out_ != INVALID_HANDLE_VALUE && GetConsoleMode(out_, &mode) != FALSE;
    if (tty_) {
        originalMode_ = mode;
        SetConsoleMode(out_, mode | ENABLE_PROCESSED_OUTPUT |
                                ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleOutputCP(CP_UTF8);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(out_, &info)) {
            int cols = info.srWindow.Right - info.srWindow.Left + 1;
            int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
            width_ = std::clamp(cols - 1, 20, 500);
            height_ = std::clamp(rows, 8, 250);
        }
    }
    wchar_t envBuffer[64];
    DWORD envLength = GetEnvironmentVariableW(L"COLUMNS", envBuffer,
                                              static_cast<DWORD>(std::size(envBuffer)));
    if (envLength > 0 && envLength < std::size(envBuffer)) {
        int value = _wtoi(envBuffer);
        if (value >= 20) width_ = std::clamp(value - 1, 20, 500);
    }
    envLength = GetEnvironmentVariableW(L"LINES", envBuffer,
                                        static_cast<DWORD>(std::size(envBuffer)));
    if (envLength > 0 && envLength < std::size(envBuffer)) {
        int value = _wtoi(envBuffer);
        if (value >= 8) height_ = std::clamp(value, 8, 250);
    }
    if (!tty_) {
        width_ = std::min(width_, 180);
        height_ = std::min(height_, 60);
    }
}

Terminal::~Terminal() {
    leave();
}

void Terminal::enter() {
    if (plain_ || !tty_ || entered_) return;
    entered_ = true;
    if (altScreen_) {
        write("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H\x1b[40m");
    } else {
        write("\x1b[2J\x1b[H\x1b[?25l\x1b[40m");
    }
}

void Terminal::leave() {
    if (entered_) {
        if (altScreen_) {
            write("\x1b[0m\x1b[?25h\x1b[?1049l");
        } else {
            write("\x1b[0m\x1b[?25h\n");
        }
        entered_ = false;
    }
    if (tty_ && out_ != INVALID_HANDLE_VALUE) {
        SetConsoleMode(out_, originalMode_);
    }
}

void Terminal::clear() {
    if (plain_ || !tty_) return;
    write("\x1b[2J\x1b[H");
}

void Terminal::write(const std::string& text) {
    if (text.empty() || out_ == INVALID_HANDLE_VALUE) return;
    size_t done = 0;
    while (done < text.size()) {
        DWORD written = 0;
        DWORD amount = static_cast<DWORD>(
            std::min<size_t>(text.size() - done, 1u << 20));
        if (!WriteFile(out_, text.data() + done, amount, &written, nullptr)) break;
        if (written == 0) break;
        done += written;
    }
}

#else

Terminal::Terminal(bool plain, bool altScreen)
    : plain_(plain), altScreen_(altScreen) {
    stdinFd_ = STDIN_FILENO;
    outFd_ = STDOUT_FILENO;
    tty_ = isatty(outFd_) != 0 && isatty(stdinFd_) != 0;

    struct winsize size {};
    if (tty_ && ioctl(outFd_, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        width_ = std::clamp(static_cast<int>(size.ws_col) - 1, 20, 500);
        height_ = std::clamp(static_cast<int>(size.ws_row), 8, 250);
    }
    if (const char* columns = std::getenv("COLUMNS")) {
        int value = std::atoi(columns);
        if (value >= 20) width_ = std::clamp(value - 1, 20, 500);
    }
    if (const char* lines = std::getenv("LINES")) {
        int value = std::atoi(lines);
        if (value >= 8) height_ = std::clamp(value, 8, 250);
    }
    if (!tty_) {
        width_ = std::min(width_, 180);
        height_ = std::min(height_, 60);
    }
}

Terminal::~Terminal() {
    leave();
}

void Terminal::enter() {
    if (plain_ || !tty_ || entered_) return;
    entered_ = true;
    if (tcgetattr(stdinFd_, &original_) == 0) {
        struct termios raw = original_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(stdinFd_, TCSANOW, &raw) == 0) rawMode_ = true;
    }
    if (altScreen_) {
        write("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H\x1b[40m");
    } else {
        write("\x1b[2J\x1b[H\x1b[?25l\x1b[40m");
    }
}

void Terminal::leave() {
    if (entered_) {
        if (altScreen_) {
            write("\x1b[0m\x1b[?25h\x1b[?1049l");
        } else {
            write("\x1b[0m\x1b[?25h\n");
        }
        entered_ = false;
    }
    if (rawMode_) {
        tcsetattr(stdinFd_, TCSANOW, &original_);
        rawMode_ = false;
    }
}

void Terminal::clear() {
    if (plain_ || !tty_) return;
    write("\x1b[2J\x1b[H");
}

void Terminal::write(const std::string& text) {
    if (text.empty() || outFd_ < 0) return;
    size_t done = 0;
    while (done < text.size()) {
        ssize_t written = ::write(outFd_, text.data() + done, text.size() - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (written == 0) break;
        done += static_cast<size_t>(written);
    }
}

#endif

std::string Terminal::colorSequence(RGB color, ColorMode mode, int& lastKey) {
    if (mode == ColorMode::Mono) return {};
    int key;
    if (mode == ColorMode::TrueColor) {
        key = (static_cast<int>(color.r) << 16) | (static_cast<int>(color.g) << 8) |
              static_cast<int>(color.b);
        if (key == lastKey) return {};
        lastKey = key;
        char buf[40];
        std::snprintf(buf, sizeof(buf), "\x1b[38;2;%u;%u;%um",
                      static_cast<unsigned>(color.r), static_cast<unsigned>(color.g),
                      static_cast<unsigned>(color.b));
        return buf;
    }
    key = quantizeXterm(color) + 1;
    if (key == lastKey) return {};
    lastKey = key;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "\x1b[38;5;%dm", key - 1);
    return buf;
}

void Terminal::draw(const AsciiFrame& frame, const std::string& status, ColorMode mode,
                    bool center) {
    if (frame.width <= 0 || frame.height <= 0) return;
    if (plain_ || !tty_) {
        std::string text;
        for (const auto& row : frame.rows) {
            text += row;
            text.push_back('\n');
        }
        write(text);
        return;
    }

    int drawW = std::min(frame.width, width_);
    int drawH = std::min(frame.height, std::max(1, height_ - 1));
    int xoff = center ? std::max(0, (width_ - drawW) / 2) : 0;
    int yoff = center ? std::max(0, (height_ - 1 - drawH) / 2) : 0;

    std::string text;
    text.reserve(static_cast<size_t>(drawW) * static_cast<size_t>(drawH) * 12 + 4096);
    text += "\x1b[?2026h";
    int lastKey = -1;
    for (int y = 0; y < drawH; ++y) {
        char position[48];
        std::snprintf(position, sizeof(position), "\x1b[%d;%dH\x1b[40m", yoff + y + 1,
                      xoff + 1);
        text += position;
        const std::string& row = frame.rows[static_cast<size_t>(y)];
        for (int x = 0; x < drawW; ++x) {
            char c = x < static_cast<int>(row.size()) ? row[static_cast<size_t>(x)] : ' ';
            if (c == ' ') {
                text.push_back(' ');
                continue;
            }
            RGB color = frame.colorAt(x, y);
            text += colorSequence(color, mode, lastKey);
            text.push_back(c);
        }
        text += "\x1b[0m\x1b[K";
        lastKey = -1;
    }

    int statusRow = std::max(1, height_);
    char statusPos[32];
    std::snprintf(statusPos, sizeof(statusPos), "\x1b[%d;1H", statusRow);
    text += statusPos;
    text += "\x1b[48;5;236m\x1b[38;5;252m";
    std::string clipped = truncateAscii(status, static_cast<size_t>(width_));
    text += clipped;
    text += "\x1b[0m\x1b[K";
    text += "\x1b[?2026l";
    write(text);
}

bool Terminal::quitRequested() const {
    if (!tty_) return false;
#ifdef _WIN32
    bool quit = false;
    while (_kbhit()) {
        int c = _getch();
        if (c == 27 || c == 'q' || c == 'Q') quit = true;
    }
    return quit;
#else
    bool quit = false;
    unsigned char buffer[64];
    while (true) {
        ssize_t got = ::read(stdinFd_, buffer, sizeof(buffer));
        if (got > 0) {
            for (ssize_t i = 0; i < got; ++i) {
                unsigned char c = buffer[i];
                if (c == 27 || c == 'q' || c == 'Q' || c == 3) quit = true;
            }
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        break;
    }
    return quit;
#endif
}

AudioVisualizer::AudioVisualizer(const WavData& audio, std::string title)
    : audio_(audio), title_(std::move(title)) {
    constexpr int kFftSize = 2048;
    window_.resize(kFftSize);
    for (int i = 0; i < kFftSize; ++i) {
        window_[static_cast<size_t>(i)] =
            static_cast<float>(0.5 * (1.0 - std::cos(2.0 * 3.14159265358979323846 * i /
                                                      (kFftSize - 1))));
    }
}

void AudioVisualizer::fft(std::vector<float>& real, std::vector<float>& imag) const {
    const int n = static_cast<int>(real.size());
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(real[static_cast<size_t>(i)], real[static_cast<size_t>(j)]);
            std::swap(imag[static_cast<size_t>(i)], imag[static_cast<size_t>(j)]);
        }
    }
    for (int length = 2; length <= n; length <<= 1) {
        double angle = -2.0 * 3.14159265358979323846 / length;
        float wReal = static_cast<float>(std::cos(angle));
        float wImag = static_cast<float>(std::sin(angle));
        for (int i = 0; i < n; i += length) {
            float curReal = 1.0f;
            float curImag = 0.0f;
            for (int j = 0; j < length / 2; ++j) {
                size_t a = static_cast<size_t>(i + j);
                size_t b = static_cast<size_t>(i + j + length / 2);
                float uReal = real[a];
                float uImag = imag[a];
                float vReal = real[b] * curReal - imag[b] * curImag;
                float vImag = real[b] * curImag + imag[b] * curReal;
                real[a] = uReal + vReal;
                imag[a] = uImag + vImag;
                real[b] = uReal - vReal;
                imag[b] = uImag - vImag;
                float nextReal = curReal * wReal - curImag * wImag;
                curImag = curReal * wImag + curImag * wReal;
                curReal = nextReal;
            }
        }
    }
}

AsciiFrame AudioVisualizer::render(double timeSeconds, int width, int height,
                                   ColorMode mode) {
    (void)mode;
    AsciiFrame frame;
    frame.reset(width, height);
    constexpr int kFftSize = 2048;

    int titleWidth = std::min(width, 70);
    std::string title = "~ AUDIO REACTOR ~";
    if (!title_.empty()) title += "  " + truncateAscii(title_, static_cast<size_t>(titleWidth) - 20);
    int titleX = std::max(0, (width - static_cast<int>(title.size())) / 2);
    for (size_t i = 0; i < title.size(); ++i) {
        int x = titleX + static_cast<int>(i);
        if (x >= 0 && x < width)
            frame.set(x, 0, title[i], RGB{92, 220, 230});
    }
    std::string sub = "spectrum analyzer / " +
                      std::to_string(audio_.sampleRate() / 1000) + " kHz / " +
                      std::to_string(audio_.channels()) + " ch";
    int subX = std::max(0, (width - static_cast<int>(sub.size())) / 2);
    for (size_t i = 0; i < sub.size(); ++i) {
        int x = subX + static_cast<int>(i);
        if (x >= 0 && x < width)
            frame.set(x, 1, sub[i], RGB{70, 110, 125});
    }

    const int plotTop = 3;
    const int baseline = std::max(plotTop + 2, height - 2);
    const int plotBottom = baseline - 1;
    const int plotHeight = std::max(1, plotBottom - plotTop + 1);
    const int bandCount = std::clamp((width - 6) / 2, 12, 64);
    if (static_cast<int>(smooth_.size()) != bandCount) {
        smooth_.assign(static_cast<size_t>(bandCount), 0.0f);
        peaks_.assign(static_cast<size_t>(bandCount), 0.0f);
    }

    for (int y = plotTop; y <= plotBottom; y += std::max(2, plotHeight / 3)) {
        for (int x = 3; x < width - 2; x += 6) {
            frame.set(x, y, '.', RGB{22, 44, 52});
        }
    }

    samples_.assign(kFftSize, 0.0f);
    audio_.monoWindow(timeSeconds, kFftSize, samples_);
    real_.assign(kFftSize, 0.0f);
    imag_.assign(kFftSize, 0.0f);
    for (int i = 0; i < kFftSize; ++i) {
        real_[static_cast<size_t>(i)] =
            samples_[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)];
    }
    fft(real_, imag_);

    const double sampleRate = std::max(1, audio_.sampleRate());
    const double fMin = 35.0;
    const double fMax = std::min(16000.0, sampleRate * 0.48);
    const double logMin = std::log(fMin);
    const double logMax = std::log(fMax);
    std::vector<float> targets(static_cast<size_t>(bandCount), 0.0f);
    for (int band = 0; band < bandCount; ++band) {
        double t0 = static_cast<double>(band) / bandCount;
        double t1 = static_cast<double>(band + 1) / bandCount;
        double f0 = std::exp(logMin + (logMax - logMin) * t0);
        double f1 = std::exp(logMin + (logMax - logMin) * t1);
        int b0 = std::clamp(static_cast<int>(std::floor(f0 * kFftSize / sampleRate)), 1,
                            kFftSize / 2 - 1);
        int b1 = std::clamp(static_cast<int>(std::ceil(f1 * kFftSize / sampleRate)), b0,
                            kFftSize / 2 - 1);
        float maxMag = 0.0f;
        for (int bin = b0; bin <= b1; ++bin) {
            float re = real_[static_cast<size_t>(bin)];
            float im = imag_[static_cast<size_t>(bin)];
            float mag = std::sqrt(re * re + im * im) * 2.0f / kFftSize;
            maxMag = std::max(maxMag, mag);
        }
        double db = 20.0 * std::log10(static_cast<double>(maxMag) + 1e-9);
        float value = static_cast<float>(std::clamp((db + 58.0) / 50.0, 0.0, 1.0));
        targets[static_cast<size_t>(band)] = value;
        float& smooth = smooth_[static_cast<size_t>(band)];
        smooth = std::max(value, smooth * 0.82f);
    }

    float peak = 0.0f;
    for (float v : smooth_) peak = std::max(peak, v);
    float gain = 1.0f;
    if (peak > 0.06f) gain = std::min(1.8f, 0.94f / peak);
    for (float& v : smooth_) v = std::clamp(v * gain, 0.0f, 1.0f);
    for (int band = 0; band < bandCount; ++band) {
        float& cap = peaks_[static_cast<size_t>(band)];
        if (smooth_[static_cast<size_t>(band)] >= cap)
            cap = smooth_[static_cast<size_t>(band)];
        else
            cap = std::max(smooth_[static_cast<size_t>(band)], cap - 0.012f);
    }

    const int step = std::max(2, (width - 4) / bandCount);
    const int barWidth = std::max(1, step - 1);
    const int totalWidth = step * bandCount;
    const int xStart = std::max(1, (width - totalWidth) / 2);
    for (int band = 0; band < bandCount; ++band) {
        int x = xStart + band * step;
        if (x + barWidth > width) break;
        int rows = static_cast<int>(std::lround(
            smooth_[static_cast<size_t>(band)] * static_cast<float>(plotHeight)));
        rows = std::clamp(rows, 0, plotHeight);
        double hue = 186.0 + 126.0 * (static_cast<double>(band) / std::max(1, bandCount - 1));
        for (int j = 0; j < rows; ++j) {
            int y = plotBottom - j;
            double lightness = 0.34 + 0.28 * (static_cast<double>(j) / plotHeight);
            RGB color = hslToRgb(hue, 0.85, lightness);
            char c = (j == rows - 1) ? '@' : '#';
            for (int w = 0; w < barWidth; ++w) {
                if (x + w < width) frame.set(x + w, y, c, color);
            }
        }
        int capRow = static_cast<int>(std::lround(
            peaks_[static_cast<size_t>(band)] * static_cast<float>(plotHeight)));
        capRow = std::clamp(capRow, 0, plotHeight);
        if (capRow > rows && capRow > 0) {
            int y = plotBottom - capRow + 1;
            for (int w = 0; w < barWidth; ++w) {
                if (x + w < width) frame.set(x + w, y, '=', RGB{235, 245, 250});
            }
        }
    }

    for (int x = 0; x < width; ++x) {
        char c = (x % 4 == 0) ? '+' : '-';
        frame.set(x, baseline, c, RGB{38, 82, 94});
    }
    std::string low = "35Hz";
    std::string high = "16kHz";
    for (size_t i = 0; i < low.size() && static_cast<int>(i) < width; ++i)
        frame.set(static_cast<int>(i), height - 1, low[i], RGB{52, 88, 98});
    for (size_t i = 0; i < high.size() && static_cast<int>(i) < width; ++i)
        frame.set(width - 1 - static_cast<int>(i), height - 1,
                  high[high.size() - 1 - i], RGB{52, 88, 98});
    return frame;
}

}  // namespace ap
