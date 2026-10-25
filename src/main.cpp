#include "audio.h"
#include "common.h"
#include "json.h"
#include "media.h"
#include "process.h"
#include "render.h"

#include <mmsystem.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <conio.h>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <io.h>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ap {
namespace {

constexpr const char* kVersion = "1.1.2";
std::atomic<bool> g_stop{false};

BOOL WINAPI consoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

bool environmentHas(const wchar_t* name) {
    wchar_t buffer[8];
    DWORD length = GetEnvironmentVariableW(name, buffer,
                                           static_cast<DWORD>(std::size(buffer)));
    return length > 0;
}

bool isWindowsTerminal() {
    return environmentHas(L"WT_SESSION");
}

bool isOtherModernTerminal() {
    const wchar_t* markers[] = {L"TERM_PROGRAM", L"WEZTERM_EXECUTABLE",
                                L"ALACRITTY_WINDOW_ID", L"KITTY_WINDOW_ID",
                                L"VSCODE_PID", L"ConEmuANSI"};
    for (const wchar_t* marker : markers) {
        if (environmentHas(marker)) return true;
    }
    return false;
}

std::wstring findWindowsTerminal() {
    wchar_t found[32768];
    DWORD length = SearchPathW(nullptr, L"wt.exe", nullptr,
                               static_cast<DWORD>(std::size(found)), found, nullptr);
    if (length > 0 && length < std::size(found)) return std::wstring(found, length);

    wchar_t localAppData[32768];
    DWORD localLength = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
                                                static_cast<DWORD>(std::size(localAppData)));
    if (localLength > 0 && localLength < std::size(localAppData)) {
        fs::path candidate =
            fs::path(localAppData) / L"Microsoft" / L"WindowsApps" / L"wt.exe";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate.wstring();
    }
    return {};
}

bool canAutoLaunchWindowsTerminal() {
    if (isWindowsTerminal() || isOtherModernTerminal()) return false;
    if (environmentHas(L"ASCII_PLAYER_NO_WT")) return false;
    DWORD mode = 0;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &mode)) return false;
    HWND window = GetConsoleWindow();
    if (!window || !IsWindowVisible(window)) return false;
    return !findWindowsTerminal().empty();
}

bool launchInWindowsTerminal(int argc, wchar_t** argv) {
    std::wstring wt = findWindowsTerminal();
    if (wt.empty()) return false;
    fs::path cwd = currentDir();
    std::wstring command = quoteWindowsArg(wt);
    command += L" -w -1 -M new-tab --title ";
    command += quoteWindowsArg(L"ASCII Player");
    command += L" --startingDirectory ";
    command += quoteWindowsArg(cwd.wstring());
    command += L" -- ";
    command += quoteWindowsArg(exePath().wstring());
    for (int i = 1; i < argc; ++i) {
        command.push_back(L' ');
        command += quoteWindowsArg(argv[i]);
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(wt.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    if (!ok) {
        ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    }
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

struct Options {
    std::string rawInput;
    fs::path input;
    fs::path init;
    fs::path audioInput;
    fs::path audioInit;
    fs::path jsonFile;
    fs::path exportJson;
    bool exportMode = false;
    bool playJsonMode = false;
    bool noAudio = false;
    bool loop = false;
    bool plain = false;
    bool altScreen = true;
    bool stats = false;
    bool colorExplicit = false;
    bool invert = false;
    bool noWt = false;
    bool full = false;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double start = 0.0;
    double duration = 0.0;
    double gamma = 0.85;
    double gain = 1.0;
    int maxFrames = 0;
    std::string charset = kDefaultCharset;
    ColorMode colorMode = ColorMode::Ansi256;
};

struct Grid {
    int width = 100;
    int height = 28;
};

struct PreparedAudio {
    TempFile file;
    std::unique_ptr<WavData> data;
};

void printBanner() {
    std::cout << "\n"
                 "  ASCII PLAYER  v" << kVersion << "\n"
                 "  color luminance -> character density\n"
                 "  mp4 / m4a / m4s / anything FFmpeg can decode\n\n";
}

void printHelp() {
    printBanner();
    std::cout <<
        "用法 / Usage:\n"
        "  ASCIIPlayer.exe <媒体文件> [选项]\n"
        "  ASCIIPlayer.exe <媒体文件> --export-json scene.json [选项]\n"
        "  ASCIIPlayer.exe --play-json scene.json [选项]\n"
        "\n"
        "常用选项:\n"
        "  --width N              ASCII 宽度（列），默认自动适配终端\n"
        "  --height N             ASCII 高度（行），默认按画面比例计算\n"
        "  --chars STR            自定义由疏到密的字符表（默认含符号、数字、大小写字母）\n"
        "  --fps N                播放/导出帧率（默认源帧率，最高 30）\n"
        "  --color MODE           truecolor | ansi256（默认） | mono\n"
        "  --gamma N              亮度伽马（默认 0.85，越小越亮）\n"
        "  --gain N               颜色增益（默认 1.0）\n"
        "  --invert               反转明暗\n"
        "  --no-audio             关闭声音（纯音频仍显示频谱）\n"
        "  --start SEC            从指定时间开始，支持 1:23.5\n"
        "  --duration SEC         只播放/导出指定时长\n"
        "  --loop                 循环播放\n"
        "  --init PATH            .m4s/fMP4 的初始化分片 init_0.mp4\n"
        "  --audio PATH           使用单独音轨；也可给 JSON 场景配同步声音\n"
        "  --audio-init PATH      单独音轨 .m4s 的初始化分片\n"
        "  --export-json PATH     导出可离线回放的彩色 JSON 场景\n"
        "  --play-json PATH       回放 JSON 场景\n"
        "  --full                 使用终端全部行列（默认最多 300 列，性能优先）\n"
        "  --no-wt                不自动切换到 Windows Terminal\n"
        "  --plain                无光标控制、无颜色（便于重定向）\n"
        "  --no-alt-screen        不使用备用屏幕\n"
        "  --stats                显示实际帧率、丢帧数与绘制耗时\n"
        "  --max-frames N         只渲染 N 帧（调试/截图）\n"
        "  -h, --help             显示帮助\n"
        "  -v, --version          显示版本\n"
        "\n"
        "示例:\n"
        "  ASCIIPlayer.exe movie.mp4\n"
        "  ASCIIPlayer.exe movie.mp4 --width 100 --chars \" .:-=+*#%@\"\n"
        "  ASCIIPlayer.exe music.m4a --color truecolor\n"
        "  ASCIIPlayer.exe clip.m4s --init init_0.mp4 --audio audio.m4a\n"
        "  ASCIIPlayer.exe movie.mp4 --export-json movie.json --fps 24\n"
        "  ASCIIPlayer.exe --play-json movie.json\n"
        "  ASCIIPlayer.exe --play-json movie.json --audio movie.mp4\n"
        "\n"
        "播放中按 Q / Esc 退出。也可以把媒体文件直接拖到 exe 上。\n"
        "\n"
        "提示：传统控制台会自动用 Windows Terminal 重新打开（--no-wt 关闭）。\n"
        "Bilibili 的 .m4s 视频/音频开头多余字节会自动跳过；没有音轨时会自动找同目录音轨。\n";
}

std::string trimPathInput(std::string text) {
    text = trim(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);
    return trim(text);
}

double parseDouble(const std::string& text, const std::string& option) {
    try {
        size_t used = 0;
        double value = std::stod(text, &used);
        check(used == text.size(), option + " expects a number.");
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error(option + " expects a number, got: " + text);
    }
}

int parseInt(const std::string& text, const std::string& option) {
    try {
        size_t used = 0;
        long long value = std::stoll(text, &used);
        check(used == text.size(), option + " expects an integer.");
        check(value >= -1000000 && value <= 1000000, option + " is out of range.");
        return static_cast<int>(value);
    } catch (const std::exception&) {
        throw std::runtime_error(option + " expects an integer, got: " + text);
    }
}

double parseTime(const std::string& text, const std::string& option) {
    if (text.find(':') == std::string::npos) {
        double value = parseDouble(text, option);
        check(value >= 0.0, option + " must be >= 0.");
        return value;
    }
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t pos = text.find(':', start);
        parts.push_back(text.substr(start, pos == std::string::npos ? pos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    check(parts.size() == 2 || parts.size() == 3, option + " expects [h:]m:s.");
    double seconds = 0.0;
    for (const auto& part : parts) seconds = seconds * 60.0 + parseDouble(part, option);
    check(seconds >= 0.0, option + " must be >= 0.");
    return seconds;
}

void validateCharset(const std::string& charset) {
    check(charset.size() >= 2, "--chars needs at least two characters.");
    check(charset.size() <= 128, "--chars is limited to 128 characters.");
    for (unsigned char c : charset) {
        check(c >= 32 && c <= 126, "--chars must contain printable ASCII characters only.");
    }
}

Options parseOptions(const std::vector<std::string>& args) {
    Options options;
    std::vector<std::string> positional;
    bool endOptions = false;
    for (size_t i = 1; i < args.size(); ++i) {
        std::string arg = args[i];
        if (!endOptions && arg == "--") {
            endOptions = true;
            continue;
        }
        if (!endOptions && arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            std::string key = arg;
            std::string value;
            size_t eq = arg.find('=');
            bool hasInline = eq != std::string::npos;
            if (hasInline) {
                key = arg.substr(0, eq);
                value = arg.substr(eq + 1);
            }
            auto takeValue = [&]() -> std::string {
                if (hasInline) return value;
                check(i + 1 < args.size(), key + " needs a value.");
                return args[++i];
            };
            if (key == "--help") {
                printHelp();
                std::exit(0);
            } else if (key == "--version") {
                std::cout << "ASCII Player " << kVersion << "\n";
                std::exit(0);
            } else if (key == "--width") {
                options.width = parseInt(takeValue(), key);
            } else if (key == "--height") {
                options.height = parseInt(takeValue(), key);
            } else if (key == "--fps") {
                options.fps = parseDouble(takeValue(), key);
            } else if (key == "--chars" || key == "--charset") {
                options.charset = takeValue();
            } else if (key == "--color") {
                std::string mode = toLower(takeValue());
                options.colorExplicit = true;
                if (mode == "truecolor" || mode == "true" || mode == "24" ||
                    mode == "24bit")
                    options.colorMode = ColorMode::TrueColor;
                else if (mode == "ansi256" || mode == "256" || mode == "xterm")
                    options.colorMode = ColorMode::Ansi256;
                else if (mode == "mono" || mode == "none" || mode == "off")
                    options.colorMode = ColorMode::Mono;
                else
                    throw std::runtime_error("--color expects truecolor, ansi256, or mono.");
            } else if (key == "--gamma") {
                options.gamma = parseDouble(takeValue(), key);
            } else if (key == "--gain") {
                options.gain = parseDouble(takeValue(), key);
            } else if (key == "--start") {
                options.start = parseTime(takeValue(), key);
            } else if (key == "--duration") {
                options.duration = parseTime(takeValue(), key);
            } else if (key == "--init") {
                options.init = fs::path(utf8ToWide(takeValue()));
            } else if (key == "--audio") {
                options.audioInput = fs::path(utf8ToWide(takeValue()));
            } else if (key == "--audio-init") {
                options.audioInit = fs::path(utf8ToWide(takeValue()));
            } else if (key == "--export-json") {
                options.exportJson = fs::path(utf8ToWide(takeValue()));
                options.exportMode = true;
            } else if (key == "--play-json") {
                options.jsonFile = fs::path(utf8ToWide(takeValue()));
                options.playJsonMode = true;
            } else if (key == "--max-frames") {
                options.maxFrames = parseInt(takeValue(), key);
            } else if (key == "--no-audio") {
                options.noAudio = true;
            } else if (key == "--loop") {
                options.loop = true;
            } else if (key == "--plain") {
                options.plain = true;
            } else if (key == "--no-alt-screen") {
                options.altScreen = false;
            } else if (key == "--stats") {
                options.stats = true;
            } else if (key == "--invert") {
                options.invert = true;
            } else if (key == "--no-wt") {
                options.noWt = true;
            } else if (key == "--full") {
                options.full = true;
            } else {
                throw std::runtime_error("Unknown option: " + key);
            }
        } else if (!endOptions && arg.size() >= 2 && arg[0] == '-' &&
                   arg != "-") {
            if (arg == "-h") {
                printHelp();
                std::exit(0);
            } else if (arg == "-v") {
                std::cout << "ASCII Player " << kVersion << "\n";
                std::exit(0);
            }
            throw std::runtime_error("Unknown option: " + arg);
        } else {
            positional.push_back(arg);
        }
    }

    if (!positional.empty()) {
        options.rawInput = positional.front();
        check(positional.size() == 1, "Only one media file can be played at a time.");
    }
    if (options.exportMode && options.playJsonMode)
        throw std::runtime_error("--export-json and --play-json cannot be used together.");
    if (options.playJsonMode && options.rawInput.empty() && !options.jsonFile.empty())
        options.input.clear();
    if (!options.rawInput.empty()) {
        options.input = fs::path(utf8ToWide(options.rawInput));
        if (!options.playJsonMode && endsWithI(options.rawInput, ".json")) {
            options.playJsonMode = true;
            options.jsonFile = options.input;
            options.input.clear();
        }
    }
    if (options.exportMode && options.input.empty())
        throw std::runtime_error("--export-json needs an input media file.");
    if (options.playJsonMode && options.rawInput.empty() && options.jsonFile.empty())
        throw std::runtime_error("--play-json needs a JSON scene path.");
    if (options.exportJson.empty() && options.exportMode)
        throw std::runtime_error("--export-json needs a path.");
    if (options.width != 0)
        check(options.width >= 10 && options.width <= 500, "--width must be 10..500.");
    if (options.height != 0)
        check(options.height >= 4 && options.height <= 250, "--height must be 4..250.");
    if (options.fps != 0.0) check(options.fps >= 1.0 && options.fps <= 120.0,
                                 "--fps must be 1..120.");
    check(options.gamma >= 0.1 && options.gamma <= 5.0, "--gamma must be 0.1..5.0.");
    check(options.gain >= 0.0 && options.gain <= 4.0, "--gain must be 0.0..4.0.");
    if (options.maxFrames < 0) options.maxFrames = 0;
    validateCharset(options.charset);
    if (options.exportMode && options.exportJson.extension().empty())
        options.exportJson += L".json";
    return options;
}

std::string sanitizeAscii(std::string text) {
    for (char& c : text) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || u > 126) c = '?';
    }
    return text;
}

std::string progressBar(double fraction, int width) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    int filled = static_cast<int>(std::lround(fraction * width));
    std::string bar = "[";
    for (int i = 0; i < width; ++i) bar.push_back(i < filled ? '#' : '-');
    bar += "]";
    return bar;
}

std::string buildStatus(const std::string& title, double now, double total, int width,
                        int height, double fps, bool silent, int loop) {
    std::ostringstream out;
    int nameWidth = std::max(8, std::min(30, width - 52));
    std::string name = title;
    if (static_cast<int>(name.size()) > nameWidth)
        name = name.substr(0, static_cast<size_t>(nameWidth - 3)) + "...";
    out << name << "  ";
    if (total > 0.0) {
        out << formatDuration(now) << "/" << formatDuration(total) << " "
            << progressBar(now / total, 18) << " ";
    } else {
        out << formatDuration(now) << " ";
    }
    out << width << "x" << height << " " << static_cast<int>(std::lround(fps)) << "fps";
    if (silent) out << " silent";
    if (loop > 1) out << " loop" << loop;
    return out.str();
}

std::string mediaTitle(const fs::path& path) {
    std::string name = basenameUtf8(path);
    if (name.empty()) name = "media";
    return sanitizeAscii(name);
}

void stderrLine(const std::string& text) {
    std::cerr << text << "\n";
}

std::string errorTail(const std::string& text, size_t maxBytes = 1200) {
    std::string cleaned = text;
    if (cleaned.size() > maxBytes)
        cleaned = "..." + cleaned.substr(cleaned.size() - maxBytes);
    return cleaned;
}

Grid videoGrid(const Options& options, const Terminal& terminal, const MediaInfo& info) {
    int displayWidth = info.width;
    int displayHeight = info.height;
    if (std::abs(info.rotation) % 180 == 90) std::swap(displayWidth, displayHeight);
    if (displayWidth <= 0 || displayHeight <= 0) {
        displayWidth = 16;
        displayHeight = 9;
    }
    const double aspect = static_cast<double>(displayWidth) / displayHeight;
    const int terminalWidth = std::max(10, terminal.availableWidth());
    const int terminalHeight = std::max(4, terminal.availableHeight());
    int maxWidth = options.width > 0 || options.full
                       ? terminalWidth
                       : std::min(terminalWidth, 300);
    int maxHeight = options.height > 0 || options.full
                        ? terminalHeight
                        : std::min(terminalHeight, 150);
    if (!terminal.isTty() && options.width == 0 && !options.full) maxWidth = 100;

    Grid grid;
    if (options.width > 0 && options.height > 0) {
        grid.width = options.width;
        grid.height = options.height;
    } else if (options.width > 0) {
        grid.width = options.width;
        grid.height = static_cast<int>(std::lround(grid.width / aspect * 0.5));
    } else if (options.height > 0) {
        grid.height = options.height;
        grid.width = static_cast<int>(std::lround(grid.height * 2.0 * aspect));
    } else {
        grid.width = maxWidth;
        grid.height = static_cast<int>(std::lround(grid.width / aspect * 0.5));
        if (grid.height > maxHeight) {
            grid.height = maxHeight;
            grid.width = static_cast<int>(std::lround(grid.height * 2.0 * aspect));
        }
    }
    grid.width = std::clamp(grid.width, 10, maxWidth);
    grid.height = std::clamp(grid.height, 4, maxHeight);
    return grid;
}

Grid audioGrid(const Options& options, const Terminal& terminal) {
    Grid grid;
    grid.width = options.width > 0 ? options.width : std::min(terminal.availableWidth(), 120);
    grid.height = options.height > 0 ? options.height : std::min(terminal.availableHeight(), 28);
    grid.width = std::clamp(grid.width, 24, std::max(24, terminal.availableWidth()));
    grid.height = std::clamp(grid.height, 12, std::max(12, terminal.availableHeight()));
    return grid;
}

Grid exportGrid(const Options& options, const MediaInfo& info, bool audioOnly) {
    Grid grid;
    if (audioOnly) {
        grid.width = options.width > 0 ? options.width : 96;
        grid.height = options.height > 0 ? options.height : 28;
        grid.width = std::clamp(grid.width, 24, 240);
        grid.height = std::clamp(grid.height, 12, 100);
        return grid;
    }
    int displayWidth = info.width;
    int displayHeight = info.height;
    if (std::abs(info.rotation) % 180 == 90) std::swap(displayWidth, displayHeight);
    if (displayWidth <= 0 || displayHeight <= 0) {
        displayWidth = 16;
        displayHeight = 9;
    }
    const double aspect = static_cast<double>(displayWidth) / displayHeight;
    if (options.width > 0 && options.height > 0) {
        grid.width = options.width;
        grid.height = options.height;
    } else if (options.width > 0) {
        grid.width = options.width;
        grid.height = static_cast<int>(std::lround(grid.width / aspect * 0.5));
    } else if (options.height > 0) {
        grid.height = options.height;
        grid.width = static_cast<int>(std::lround(grid.height * 2.0 * aspect));
    } else {
        grid.width = 96;
        grid.height = static_cast<int>(std::lround(grid.width / aspect * 0.5));
    }
    grid.width = std::clamp(grid.width, 10, options.full ? 500 : 300);
    grid.height = std::clamp(grid.height, 4, options.full ? 250 : 150);
    return grid;
}

TempFile extractAudio(const FFmpeg& ffmpeg, const InputSpec& input, double start,
                      double duration) {
    TempFile file(makeWritableTempFile(L"ascii_player_audio", L".wav"));
    std::vector<std::wstring> args = {L"-hide_banner", L"-loglevel", L"error",
                                      L"-nostdin", L"-y"};
    if (start > 0.0001) {
        args.push_back(L"-ss");
        args.push_back(numberArg(start));
    }
    if (duration > 0.0001) {
        args.push_back(L"-t");
        args.push_back(numberArg(duration));
    }
    args.push_back(L"-i");
    args.push_back(input.ffmpegInput());
    args.insert(args.end(), {L"-map", L"0:a:0", L"-vn", L"-ac", L"2",
                             L"-ar", L"48000", L"-c:a", L"pcm_s16le", L"-f", L"wav"});
    args.push_back(file.path().wstring());
    ProcessResult result = ffmpeg.runQuiet(args);
    std::error_code ec;
    check(result.started, result.errors);
    check(result.exitCode == 0 && fs::is_regular_file(file.path(), ec) &&
              fs::file_size(file.path(), ec) > 44,
          "Could not extract audio.\n" + errorTail(result.errors));
    return file;
}

std::unique_ptr<PreparedAudio> prepareAudio(const FFmpeg& ffmpeg,
                                            const InputSpec& input, double start,
                                            double duration, bool required,
                                            std::string& warning) {
    try {
        auto prepared = std::make_unique<PreparedAudio>();
        prepared->file = extractAudio(ffmpeg, input, start, duration);
        prepared->data = std::make_unique<WavData>();
        prepared->data->open(prepared->file.path());
        return prepared;
    } catch (const std::exception& e) {
        if (required) throw;
        warning = e.what();
        return nullptr;
    }
}

class PlaybackClock {
public:
    explicit PlaybackClock(WaveOutPlayer* audio) : audio_(audio) {
        start_ = std::chrono::steady_clock::now();
    }
    double wall() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start_).count();
    }
    double now() const {
        if (audio_ && audio_->started() && !audio_->failed() && !audio_->finished()) {
            double position = audio_->positionSeconds();
            if (position >= 0.0) return position;
        }
        return wall();
    }

private:
    WaveOutPlayer* audio_ = nullptr;
    std::chrono::steady_clock::time_point start_;
};

class FpsMeter {
public:
    void tick() {
        auto now = std::chrono::steady_clock::now();
        times_.push_back(now);
        while (!times_.empty() &&
               std::chrono::duration<double>(now - times_.front()).count() > 1.2)
            times_.pop_front();
    }

    double value(double fallback) const {
        if (times_.size() < 2) return fallback;
        double seconds =
            std::chrono::duration<double>(times_.back() - times_.front()).count();
        if (seconds <= 0.05) return fallback;
        return (times_.size() - 1) / seconds;
    }

private:
    std::deque<std::chrono::steady_clock::time_point> times_;
};

bool pollQuit(Terminal& terminal) {
    if (g_stop.load()) return true;
    if (terminal.quitRequested()) {
        g_stop = true;
        return true;
    }
    return false;
}

void waitUntil(double target, PlaybackClock& clock, WaveOutPlayer* audio,
               Terminal& terminal) {
    while (!g_stop.load()) {
        if (audio) audio->pump();
        if (pollQuit(terminal)) return;
        double now = clock.now();
        double remaining = target - now;
        if (remaining <= 0.0008) return;
        int millis = remaining > 0.008 ? 4 : 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    }
}

AsciiFrame resampleFrame(const AsciiFrame& source, int targetWidth, int targetHeight,
                         const std::string& charset) {
    if (source.width == targetWidth && source.height == targetHeight) return source;
    AsciiFrame out;
    out.reset(targetWidth, targetHeight);
    int levels[128];
    std::fill(std::begin(levels), std::end(levels), -1);
    for (size_t i = 0; i < charset.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(charset[i]);
        if (c < 128 && levels[c] < 0)
            levels[c] = static_cast<int>(std::lround(
                static_cast<double>(i) * 100.0 / std::max<size_t>(1, charset.size() - 1)));
    }
    for (int y = 0; y < targetHeight; ++y) {
        int sy0 = y * source.height / targetHeight;
        int sy1 = std::max(sy0 + 1, (y + 1) * source.height / targetHeight);
        sy1 = std::min(sy1, source.height);
        for (int x = 0; x < targetWidth; ++x) {
            int sx0 = x * source.width / targetWidth;
            int sx1 = std::max(sx0 + 1, (x + 1) * source.width / targetWidth);
            sx1 = std::min(sx1, source.width);
            double level = 0.0;
            double r = 0.0, g = 0.0, b = 0.0;
            int count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    char c = source.at(sx, sy);
                    unsigned char uc = static_cast<unsigned char>(c);
                    int sourceLevel = (uc < 128) ? levels[uc] : 50;
                    if (sourceLevel < 0) sourceLevel = 50;
                    level += sourceLevel;
                    RGB color = source.colorAt(sx, sy);
                    r += color.r;
                    g += color.g;
                    b += color.b;
                    ++count;
                }
            }
            if (count == 0) continue;
            level /= count * 100.0;
            int index = static_cast<int>(std::lround(level * (charset.size() - 1)));
            index = std::clamp(index, 0, static_cast<int>(charset.size()) - 1);
            RGB color{static_cast<uint8_t>(r / count), static_cast<uint8_t>(g / count),
                      static_cast<uint8_t>(b / count)};
            out.set(x, y, charset[static_cast<size_t>(index)], color);
        }
    }
    return out;
}

int playJson(const Options& options) {
    Scene scene = loadScene(options.jsonFile);
    Terminal terminal(options.plain, options.altScreen);
    ColorMode mode = options.colorMode;
    if (!scene.hasColors) mode = ColorMode::Mono;
    else if (!options.colorExplicit) mode = ColorMode::Ansi256;

    int availableWidth = terminal.availableWidth();
    int availableHeight = terminal.availableHeight();
    if (options.width > 0) availableWidth = std::min(availableWidth, options.width);
    if (options.height > 0) availableHeight = std::min(availableHeight, options.height);
    double scale = std::min({1.0,
                             static_cast<double>(availableWidth) / scene.width,
                             static_cast<double>(availableHeight) / scene.height});
    int targetWidth = std::max(1, static_cast<int>(std::floor(scene.width * scale)));
    int targetHeight = std::max(1, static_cast<int>(std::floor(scene.height * scale)));

    std::string title = mediaTitle(options.jsonFile);
    if (!scene.source.empty()) title = sanitizeAscii(scene.source);

    std::unique_ptr<PreparedAudio> jsonAudio;
    std::vector<std::string> playbackNotes;
    bool audioRequested = false;
    if (!options.audioInput.empty() && !options.noAudio) {
        if (terminal.isTty()) std::cerr << "Preparing audio..." << std::flush;
        FFmpeg ffmpeg = FFmpeg::locate();
        InputSpec audioInput{options.audioInput, options.audioInit};
        std::string audioWarning;
        double audioLimit = scene.duration > 0.0 ? scene.duration + 0.75 : 0.0;
        jsonAudio = prepareAudio(ffmpeg, audioInput, 0.0, audioLimit, true,
                                 audioWarning);
        audioRequested = jsonAudio && jsonAudio->data;
        if (terminal.isTty()) {
            std::cerr << "\r" << std::string(20, ' ') << "\r";
            std::cerr.flush();
        }
    }

    terminal.enter();
    ScopeExit leave([&]() { terminal.leave(); });

    int totalPlayed = 0;
    int loop = 0;
    std::string status;
    bool audioStartWarned = false;
    double firstTime = scene.frames.front().time;
    auto wallStartAll = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        ++loop;
        auto start = std::chrono::steady_clock::now();
        WaveOutPlayer player;
        bool audioActive = false;
        if (audioRequested) {
            std::string audioError;
            audioActive = player.start(*jsonAudio->data, audioError);
            if (!audioActive && !audioStartWarned) {
                playbackNotes.push_back(audioError);
                audioStartWarned = true;
            }
        }
        PlaybackClock clock(audioActive ? &player : nullptr);
        for (size_t i = 0; i < scene.frames.size(); ++i) {
            if (g_stop.load() || pollQuit(terminal)) break;
            double target = scene.frames[i].time - firstTime;
            if (target < 0.0) target = 0.0;
            if (audioActive) {
                waitUntil(target, clock, &player, terminal);
            } else {
                while (!g_stop.load() && !pollQuit(terminal)) {
                    double now = std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
                    if (now >= target) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (g_stop.load()) break;
            AsciiFrame frame = sceneFrameToAscii(scene, scene.frames[i]);
            if (targetWidth != scene.width || targetHeight != scene.height)
                frame = resampleFrame(frame, targetWidth, targetHeight, scene.charset);
            double now = std::max(0.0, scene.frames[i].time - firstTime);
            status = buildStatus(title, now, scene.duration, targetWidth, targetHeight,
                                 scene.fps, audioRequested && !audioActive, loop);
            terminal.draw(frame, status, mode, true);
            ++totalPlayed;
            if (options.maxFrames > 0 && totalPlayed >= options.maxFrames) {
                g_stop = true;
                break;
            }
        }
        player.stop();
        if (!options.loop || g_stop.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    terminal.leave();
    leave.dismiss();
    double elapsed = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - wallStartAll)
                         .count();
    if (options.stats) {
        std::cout << "Played " << totalPlayed << " JSON frames in " << elapsed << "s.\n";
    }
    for (const auto& note : playbackNotes) stderrLine("Note: " + note);
    return 0;
}

struct CompanionAudio {
    InputSpec spec;
    fs::path sourcePath;
    std::shared_ptr<TempFile> repaired;
};

std::wstring leadingDigits(const std::wstring& text) {
    size_t i = 0;
    while (i < text.size() && std::iswdigit(text[i])) ++i;
    return text.substr(0, i);
}

std::wstring lowerWide(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

bool audioCandidateExtension(const fs::path& path) {
    std::wstring ext = lowerWide(path.extension().wstring());
    return ext == L".m4a" || ext == L".aac" || ext == L".mp3" || ext == L".flac" ||
           ext == L".wav" || ext == L".ogg" || ext == L".opus" || ext == L".m4s" ||
           ext == L".mp4" || ext == L".mkv" || ext == L".webm" || ext == L".mov";
}

int companionScore(const fs::path& file, const fs::path& video,
                   const std::wstring& baseId) {
    std::error_code ec;
    if (fs::equivalent(file, video, ec)) return -1000;
    std::wstring name = lowerWide(file.filename().wstring());
    int score = 0;
    if (!baseId.empty()) {
        if (name.find(baseId) == std::wstring::npos) return -1000;
        score += 100;
    }
    if (name.find(L"audio") != std::wstring::npos ||
        name.find(L"音频") != std::wstring::npos)
        score += 55;
    if (name.find(L"30280") != std::wstring::npos)
        score += 45;
    else if (name.find(L"30250") != std::wstring::npos ||
             name.find(L"30251") != std::wstring::npos ||
             name.find(L"30252") != std::wstring::npos ||
             name.find(L"30232") != std::wstring::npos ||
             name.find(L"30216") != std::wstring::npos)
        score += 35;
    if (name.find(L"video") != std::wstring::npos ||
        name.find(L"视频") != std::wstring::npos ||
        name.find(L"30080") != std::wstring::npos ||
        name.find(L"30077") != std::wstring::npos)
        score -= 120;
    std::wstring ext = lowerWide(file.extension().wstring());
    if (ext == L".m4a" || ext == L".aac" || ext == L".mp3" || ext == L".flac" ||
        ext == L".wav" || ext == L".ogg" || ext == L".opus")
        score += 30;
    if (ext == L".m4s") score += 10;
    return score;
}

std::shared_ptr<TempFile> repairLeadingMp4Junk(const fs::path& file,
                                               size_t* skipped = nullptr) {
    std::error_code ec;
    uintmax_t fileSize = fs::file_size(file, ec);
    if (ec || fileSize < 32) return {};
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    char header[256]{};
    in.read(header, sizeof(header));
    std::streamsize got = in.gcount();
    if (got < 16) return {};
    const char signature[4] = {'f', 't', 'y', 'p'};
    auto* found = std::search(header, header + got, std::begin(signature), std::end(signature));
    if (found == header + got) return {};
    size_t position = static_cast<size_t>(found - header);
    if (position <= 4) return {};
    size_t start = position - 4;
    const auto* bytes = reinterpret_cast<const uint8_t*>(header);
    uint32_t boxSize = (static_cast<uint32_t>(bytes[start]) << 24) |
                       (static_cast<uint32_t>(bytes[start + 1]) << 16) |
                       (static_cast<uint32_t>(bytes[start + 2]) << 8) |
                       static_cast<uint32_t>(bytes[start + 3]);
    if (boxSize < 8 || static_cast<uintmax_t>(start) + boxSize > fileSize) return {};

    auto repaired =
        std::make_shared<TempFile>(makeWritableTempFile(L"ascii_player_mediafix", L".mp4"));
    in.clear();
    in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    std::ofstream out(repaired->path(), std::ios::binary | std::ios::trunc);
    if (!in || !out) return {};
    std::vector<char> buffer(1024 * 1024);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize chunkBytes = in.gcount();
        if (chunkBytes > 0) out.write(buffer.data(), chunkBytes);
    }
    out.close();
    if (!out.good()) return {};
    if (skipped) *skipped = start;
    return repaired;
}

bool candidateHasAudio(const FFmpeg& ffmpeg, const fs::path& path,
                       double expectedDuration, MediaInfo& info) {
    try {
        MediaInfo candidate = ffmpeg.probe(InputSpec{path, {}});
        if (!candidate.hasAudio) return false;
        if (expectedDuration > 1.0 && candidate.duration > 1.0 &&
            std::abs(candidate.duration - expectedDuration) > 3.0)
            return false;
        info = candidate;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<CompanionAudio> findCompanionAudio(const fs::path& video,
                                                 double videoDuration,
                                                 const FFmpeg& ffmpeg) {
    fs::path directory = video.parent_path();
    if (directory.empty()) directory = L".";
    std::wstring baseId = leadingDigits(video.stem().wstring());
    if (baseId.size() < 3)
        baseId = leadingDigits(video.parent_path().filename().wstring());

    struct Ranked {
        int score;
        fs::path path;
    };
    std::vector<Ranked> ranked;
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path& path = it->path();
        if (!audioCandidateExtension(path)) continue;
        int score = companionScore(path, video, baseId);
        if (score > 0) ranked.push_back({score, path});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
    if (ranked.size() > 8) ranked.resize(8);

    for (const auto& candidate : ranked) {
        MediaInfo audioInfo;
        if (candidateHasAudio(ffmpeg, candidate.path, videoDuration, audioInfo))
            return CompanionAudio{InputSpec{candidate.path, {}}, candidate.path, nullptr};

        auto repaired = repairLeadingMp4Junk(candidate.path);
        if (repaired && candidateHasAudio(ffmpeg, repaired->path(), videoDuration,
                                          audioInfo)) {
            return CompanionAudio{InputSpec{repaired->path(), {}}, candidate.path,
                                  repaired};
        }
    }
    return std::nullopt;
}

std::unique_ptr<PreparedAudio> prepareMediaAudio(const Options& options,
                                                 const FFmpeg& ffmpeg,
                                                 const MediaInfo& info,
                                                 const InputSpec& mainInput,
                                                 std::string& warning,
                                                 bool forceForVisualizer) {
    bool needAudio = forceForVisualizer || (info.hasAudio && !options.noAudio) ||
                     (!options.audioInput.empty() && !options.noAudio);
    if (!needAudio) return nullptr;
    InputSpec audioInput = mainInput;
    if (!options.audioInput.empty()) {
        audioInput.path = options.audioInput;
        audioInput.init = options.audioInit;
    }
    bool required = forceForVisualizer;
    return prepareAudio(ffmpeg, audioInput, options.start, options.duration, required,
                        warning);
}

double timelineDuration(const Options& options, const MediaInfo& info) {
    double duration = info.duration;
    if (options.start > 0.0 && duration > 0.0)
        duration = std::max(0.0, duration - options.start);
    if (options.duration > 0.0)
        duration = duration > 0.0 ? std::min(duration, options.duration)
                                  : options.duration;
    return duration;
}

RenderSettings makeRenderSettings(const Options& options);

int exportJson(const Options& options, const InputSpec& input, const MediaInfo& info,
               const FFmpeg& ffmpeg) {
    bool audioOnly = !info.hasVideo;
    Grid grid = exportGrid(options, info, audioOnly);
    double sourceFps = info.fps > 0 ? info.fps : 30.0;
    double fps = options.fps > 0 ? options.fps : std::min(30.0, sourceFps);
    fps = std::clamp(fps, 1.0, 120.0);
    std::string sourceName = basenameUtf8(options.input);
    std::string jsonColorMode =
        options.colorMode == ColorMode::Mono ? "mono" : "ansi256";
    double duration = timelineDuration(options, info);
    std::string warning;
    std::unique_ptr<PreparedAudio> audio;
    if (audioOnly) {
        audio = prepareMediaAudio(options, ffmpeg, info, input, warning, true);
        check(audio && audio->data, "No audio track was found for JSON export.");
        duration = audio->data->duration();
    }

    std::cerr << "Exporting " << grid.width << "x" << grid.height << " ASCII scene at "
              << std::fixed << std::setprecision(1) << fps << " fps\n";

    auto progress = [&](int frames, double timeSeconds) {
        if (_isatty(_fileno(stderr))) {
            std::fprintf(stderr, "\r  %d frames  %.1fs", frames, timeSeconds);
            std::fflush(stderr);
        } else if (frames > 0 && frames % 150 == 0) {
            std::fprintf(stderr, "  %d frames  %.1fs\n", frames, timeSeconds);
            std::fflush(stderr);
        }
    };

    int framesWritten = 0;
    SceneWriter writer(options.exportJson, grid.width, grid.height, fps, duration,
                       sourceName, options.charset, jsonColorMode);

    if (audioOnly) {
        int frameCount = static_cast<int>(std::ceil(duration * fps));
        if (options.maxFrames > 0) frameCount = std::min(frameCount, options.maxFrames);
        AudioVisualizer visualizer(*audio->data, sanitizeAscii(sourceName));
        for (int i = 0; i < frameCount; ++i) {
            if (g_stop.load()) break;
            double timeSeconds = i / fps;
            AsciiFrame frame = visualizer.render(timeSeconds, grid.width, grid.height,
                                                  options.colorMode);
            writer.writeFrame(frame, timeSeconds);
            framesWritten = i + 1;
            if ((i + 1) % 20 == 0) progress(framesWritten, timeSeconds);
        }
    } else {
        auto video = std::make_unique<VideoStream>(ffmpeg, input, grid.width, grid.height,
                                                   fps, options.start, options.duration);
        std::vector<uint8_t> rgb;
        int frameIndex = 0;
        bool reachedEof = false;
        bool stoppedEarly = false;
        while (!g_stop.load()) {
            if (!video->next(rgb)) {
                reachedEof = true;
                break;
            }
            if (duration > 0.0 && frameIndex / fps > duration + 0.001) {
                stoppedEarly = true;
                break;
            }
            AsciiFrame frame = renderRgbFrame(rgb.data(), grid.width, grid.height,
                                              grid.width * 3, makeRenderSettings(options));
            writer.writeFrame(frame, frameIndex / fps);
            progress(frameIndex + 1, frameIndex / fps);
            ++frameIndex;
            framesWritten = frameIndex;
            if (options.maxFrames > 0 && framesWritten >= options.maxFrames) {
                stoppedEarly = true;
                break;
            }
        }
        if (g_stop.load()) stoppedEarly = true;
        if (stoppedEarly) {
            video->cancel();
        } else {
            int code = video->finish();
            if (code != 0 && !reachedEof)
                throw std::runtime_error("ffmpeg could not decode video frames.\n" +
                                         errorTail(video->errorText()));
            if (code != 0 && framesWritten > 0)
                warning = "ffmpeg ended with an error after " +
                          std::to_string(framesWritten) + " frames.\n" +
                          errorTail(video->errorText());
        }
    }
    writer.finish();

    if (_isatty(_fileno(stderr))) std::fprintf(stderr, "\n");
    check(framesWritten > 0, "No frames were exported.");
    std::error_code ec;
    fs::path absolute = fs::absolute(options.exportJson, ec);
    if (ec) absolute = options.exportJson;
    std::cout << "JSON scene: " << pathToUtf8(absolute) << "\n"
              << "  frames: " << framesWritten << "  grid: " << grid.width << "x"
              << grid.height << "  fps: " << static_cast<int>(std::lround(fps))
              << "  size: " << formatSize(fs::file_size(options.exportJson, ec)) << "\n"
              << "  play: ASCIIPlayer.exe --play-json \""
              << pathToUtf8(absolute) << "\"\n";
    if (!warning.empty()) stderrLine("Note: " + warning);
    return 0;
}

RenderSettings makeRenderSettings(const Options& options) {
    RenderSettings settings;
    settings.charset = options.charset;
    settings.gamma = options.gamma;
    settings.colorGain = options.gain;
    settings.invert = options.invert;
    settings.colorMode = options.colorMode;
    return settings;
}

int playMedia(const Options& options, const InputSpec& input, const MediaInfo& info,
              const FFmpeg& ffmpeg) {
    Terminal terminal(options.plain, options.altScreen);
    bool audioOnly = !info.hasVideo;
    Grid grid = audioOnly ? audioGrid(options, terminal)
                          : videoGrid(options, terminal, info);
    double sourceFps = info.fps > 0 ? info.fps : 30.0;
    double fps = options.fps > 0 ? options.fps : std::min(30.0, sourceFps);
    fps = std::clamp(fps, 1.0, 120.0);
    std::string title = mediaTitle(options.input);
    std::string warning;

    if (terminal.isTty()) {
        std::cerr << "Preparing audio..." << std::flush;
    }
    std::unique_ptr<PreparedAudio> audio = prepareMediaAudio(
        options, ffmpeg, info, input, warning, audioOnly);
    if (terminal.isTty()) {
        std::cerr << "\r" << std::string(20, ' ') << "\r";
        std::cerr.flush();
    }
    if (!warning.empty()) stderrLine("Audio note: " + warning);

    double mediaDuration = timelineDuration(options, info);
    if (audioOnly && audio && audio->data)
        mediaDuration = audio->data->duration();
    RenderSettings renderSettings = makeRenderSettings(options);

    std::vector<std::string> playbackNotes;
    bool audioStartWarned = false;
    auto wallStartAll = std::chrono::steady_clock::now();
    int totalPlayed = 0;
    int loopIndex = 0;
    bool limitReached = false;
    uint64_t droppedFrames = 0;
    double drawSeconds = 0.0;
    FpsMeter fpsMeter;

    terminal.enter();
    ScopeExit leave([&]() { terminal.leave(); });

    while (!g_stop.load() && !limitReached) {
        ++loopIndex;
        WaveOutPlayer player;
        bool audioRequested = audio && audio->data && !options.noAudio;
        bool audioActive = false;
        if (audioRequested) {
            std::string audioError;
            audioActive = player.start(*audio->data, audioError);
            if (!audioActive && !audioStartWarned) {
                playbackNotes.push_back(audioError);
                audioStartWarned = true;
            }
        }
        PlaybackClock clock(audioActive ? &player : nullptr);

        if (audioOnly) {
            check(audio && audio->data, "No audio track found.");
            AudioVisualizer visualizer(*audio->data, sanitizeAscii(title));
            int frameIndex = 0;
            while (!g_stop.load() && !limitReached) {
                double target = frameIndex / fps;
                if (mediaDuration > 0.0 && target > mediaDuration + 0.0005) break;
                waitUntil(target, clock, audioActive ? &player : nullptr, terminal);
                if (g_stop.load()) break;
                player.pump();
                double now = std::min(std::max(0.0, clock.now()),
                                      mediaDuration > 0.0 ? mediaDuration : clock.now());
                AsciiFrame frame = visualizer.render(now, grid.width, grid.height,
                                                      options.colorMode);
                std::string status =
                    buildStatus(title, now, mediaDuration, grid.width, grid.height,
                                fpsMeter.value(fps), audioRequested && !audioActive,
                                loopIndex);
                terminal.draw(frame, status, options.colorMode);
                fpsMeter.tick();
                ++totalPlayed;
                ++frameIndex;
                if (options.maxFrames > 0 && totalPlayed >= options.maxFrames) {
                    limitReached = true;
                    break;
                }
                if (mediaDuration > 0.0 && now >= mediaDuration - 0.0005 &&
                    (clock.wall() > mediaDuration + 0.1 || player.finished()))
                    break;
                if (audioActive && player.finished() && clock.wall() > mediaDuration + 0.05)
                    break;
            }
        } else {
            auto video = std::make_unique<VideoStream>(
                ffmpeg, input, grid.width, grid.height, fps, options.start,
                options.duration);
            std::vector<uint8_t> rgb;
            if (!video->next(rgb)) {
                int code = video->finish();
                player.stop();
                throw std::runtime_error("No video frames were decoded.\n" +
                                         errorTail(video->errorText()) +
                                         (code == 0 ? "" : "\n(ffmpeg exit " +
                                                            std::to_string(code) + ")"));
            }
            int frameIndex = 0;
            bool reachedEof = false;
            bool stoppedEarly = false;
            while (!g_stop.load() && !limitReached) {
                player.pump();
                double beforeWait = std::max(0.0, clock.now());
                int desiredIndex = static_cast<int>(beforeWait * fps);
                if (desiredIndex > frameIndex + 1) {
                    int skip = desiredIndex - frameIndex - 1;
                    for (int i = 0; i < skip; ++i) {
                        if (!video->next(rgb)) {
                            reachedEof = true;
                            break;
                        }
                        ++frameIndex;
                        ++droppedFrames;
                    }
                    if (reachedEof) break;
                }

                double target = frameIndex / fps;
                if (mediaDuration > 0.0 && target > mediaDuration + 0.001) {
                    stoppedEarly = true;
                    break;
                }
                waitUntil(target, clock, audioActive ? &player : nullptr, terminal);
                if (g_stop.load()) break;
                player.pump();
                AsciiFrame frame = renderRgbFrame(rgb.data(), grid.width, grid.height,
                                                  grid.width * 3, renderSettings);
                double now = std::max(0.0, clock.now());
                if (mediaDuration > 0.0) now = std::min(now, mediaDuration);
                std::string status =
                    buildStatus(title, now, mediaDuration, grid.width, grid.height,
                                fpsMeter.value(fps), audioRequested && !audioActive,
                                loopIndex);
                auto drawStart = std::chrono::steady_clock::now();
                terminal.draw(frame, status, options.colorMode);
                drawSeconds += std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - drawStart)
                                   .count();
                fpsMeter.tick();
                ++totalPlayed;
                ++frameIndex;
                if (options.maxFrames > 0 && totalPlayed >= options.maxFrames) {
                    limitReached = true;
                    break;
                }
                if (!video->next(rgb)) {
                    reachedEof = true;
                    break;
                }
            }
            if (g_stop.load() || limitReached || stoppedEarly) stoppedEarly = true;
            player.stop();
            if (stoppedEarly) {
                video->cancel();
            } else {
                int code = video->finish();
                if (code != 0 && !reachedEof && video->framesRead() == 0) {
                    throw std::runtime_error("ffmpeg could not decode video frames.\n" +
                                             errorTail(video->errorText()));
                }
                if (code != 0 && video->framesRead() > 0) {
                    playbackNotes.push_back("ffmpeg ended with an error after " +
                                            std::to_string(video->framesRead()) +
                                            " frames.\n" + errorTail(video->errorText()));
                }
            }
        }

        if (!options.loop || g_stop.load() || limitReached) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }

    terminal.leave();
    leave.dismiss();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    double elapsed = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - wallStartAll)
                         .count();
    if (options.stats) {
        double effective = elapsed > 0.0 ? totalPlayed / elapsed : 0.0;
        double drawMilliseconds =
            totalPlayed > 0 ? drawSeconds * 1000.0 / totalPlayed : 0.0;
        std::cout << "Played " << totalPlayed << " frames in " << std::fixed
                  << std::setprecision(2) << elapsed << "s (" << effective << " fps), "
                  << "dropped " << droppedFrames << ", draw " << std::setprecision(1)
                  << drawMilliseconds << " ms/frame.\n";
    }
    for (const auto& note : playbackNotes) stderrLine("Note: " + note);
    return 0;
}

int run(const Options& options) {
    if (options.playJsonMode) return playJson(options);

    FFmpeg ffmpeg = FFmpeg::locate();
    InputSpec input{options.input, options.init};
    std::cerr << "Probing " << input.display() << " ...\n";
    std::shared_ptr<TempFile> repairedInput;
    MediaInfo info;
    try {
        info = ffmpeg.probe(input);
    } catch (const std::exception& error) {
        // Bilibili caches prefix both video and audio .m4s files with nine
        // ASCII '0' bytes; audio used to be handled only in companion lookup.
        size_t skipped = 0;
        auto repaired =
            input.init.empty() ? repairLeadingMp4Junk(input.path, &skipped) : nullptr;
        if (!repaired) throw;
        InputSpec repairedSpec{repaired->path(), {}};
        try {
            MediaInfo repairedInfo = ffmpeg.probe(repairedSpec);
            input = repairedSpec;
            info = repairedInfo;
            repairedInput = repaired;
        } catch (const std::exception&) {
            throw std::runtime_error(error.what());
        }
        std::cerr << "Input: skipped " << skipped << " leading byte(s) before MP4 header\n";
    }
    if (!options.audioInput.empty()) {
        std::error_code ec;
        check(fs::is_regular_file(options.audioInput, ec),
              "Audio file not found: " + pathToUtf8(options.audioInput));
    }
    if (options.exportMode) return exportJson(options, input, info, ffmpeg);

    Options effective = options;
    std::shared_ptr<TempFile> repairedCompanion;
    if (info.hasVideo && !info.hasAudio && effective.audioInput.empty() &&
        !effective.noAudio) {
        auto companion = findCompanionAudio(options.input, info.duration, ffmpeg);
        if (companion) {
            effective.audioInput = companion->spec.path;
            effective.audioInit = companion->spec.init;
            repairedCompanion = companion->repaired;
            std::cerr << "Companion audio: "
                      << pathToUtf8(companion->sourcePath);
            if (companion->repaired) std::cerr << " (repaired header)";
            std::cerr << "\n";
        }
    }
    (void)repairedCompanion;
    (void)repairedInput;
    return playMedia(effective, input, info, ffmpeg);
}

}  // namespace
}  // namespace ap

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"ASCII Player");
    SetConsoleCtrlHandler(ap::consoleCtrlHandler, TRUE);
    timeBeginPeriod(1);
    ap::ScopeExit timerEnd([]() { timeEndPeriod(1); });

    try {
        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) args.push_back(ap::wideToUtf8(argv[i]));
        bool interactive = false;
        if (args.size() == 1) {
            if (_isatty(_fileno(stdin)) && ap::canAutoLaunchWindowsTerminal() &&
                ap::launchInWindowsTerminal(argc, argv)) {
                return 0;
            }
            ap::printBanner();
            if (!_isatty(_fileno(stdin))) {
                ap::printHelp();
                return 1;
            }
            std::cout << "把媒体文件拖到这里，或输入路径，然后回车：\n> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line) || ap::trim(line).empty()) {
                ap::printHelp();
                return 1;
            }
            args.push_back(ap::trimPathInput(line));
            interactive = true;
        }
        ap::Options options = ap::parseOptions(args);
        if (!options.exportMode && !options.plain && !options.noWt &&
            ap::canAutoLaunchWindowsTerminal() &&
            ap::launchInWindowsTerminal(argc, argv)) {
            return 0;
        }
        int result = ap::run(options);
        if (interactive) {
            std::cout << "\n按 Enter 退出 ..." << std::flush;
            std::string dummy;
            std::getline(std::cin, dummy);
        }
        return result;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        if (argc <= 1 && _isatty(_fileno(stdin))) {
            std::cerr << "按 Enter 退出 ..." << std::flush;
            std::string dummy;
            std::getline(std::cin, dummy);
        }
        return 1;
    }
}
