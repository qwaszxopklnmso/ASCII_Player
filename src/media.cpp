#include "media.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <regex>
#include <sstream>

namespace ap {

namespace {

std::wstring joinArgs(const std::vector<std::wstring>& args) {
    std::wstring out;
    for (const auto& a : args) {
        if (!out.empty()) out.push_back(L' ');
        out += quoteWindowsArg(a);
    }
    return out;
}

std::string tail(const std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    return "..." + text.substr(text.size() - maxBytes);
}

bool fileExists(const fs::path& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(path, ec);
}

}  // namespace

std::wstring InputSpec::ffmpegInput() const {
    if (!isConcat()) return path.wstring();
    return L"concat:" + init.wstring() + L"|" + path.wstring();
}

std::string InputSpec::display() const {
    if (!isConcat()) return pathToUtf8(path);
    return pathToUtf8(init) + " + " + pathToUtf8(path);
}

std::wstring numberArg(double value) {
    wchar_t buf[64];
    std::swprintf(buf, 64, L"%.6f", value);
    std::wstring text(buf);
    while (text.size() > 1 && text.back() == L'0') text.pop_back();
    if (!text.empty() && text.back() == L'.') text.pop_back();
    return text;
}

std::wstring intArg(int value) {
    return std::to_wstring(value);
}

FFmpeg FFmpeg::locate() {
    std::vector<fs::path> candidates;
    fs::path dir = exeDir();
    candidates.push_back(dir / L"ffmpeg.exe");
    candidates.push_back(dir / L"bin" / L"ffmpeg.exe");
    candidates.push_back(dir / L"tools" / L"ffmpeg.exe");
    if (dir.has_parent_path()) {
        candidates.push_back(dir.parent_path() / L"ffmpeg.exe");
        candidates.push_back(dir.parent_path() / L"bin" / L"ffmpeg.exe");
    }
    candidates.push_back(currentDir() / L"ffmpeg.exe");

    wchar_t envPath[32768];
    DWORD n = GetEnvironmentVariableW(L"FFMPEG_PATH", envPath,
                                      static_cast<DWORD>(std::size(envPath)));
    if (n > 0 && n < std::size(envPath)) {
        fs::path p(envPath);
        if (fs::is_directory(p)) p /= L"ffmpeg.exe";
        candidates.push_back(p);
    }

    wchar_t found[32768];
    DWORD foundLen = SearchPathW(nullptr, L"ffmpeg.exe", nullptr,
                                 static_cast<DWORD>(std::size(found)), found, nullptr);
    if (foundLen > 0 && foundLen < std::size(found)) {
        candidates.emplace_back(std::wstring(found, foundLen));
    }

    for (const auto& candidate : candidates) {
        std::error_code ec;
        fs::path absolute = fs::absolute(candidate, ec);
        if (ec) absolute = candidate;
        if (fileExists(absolute)) return FFmpeg(absolute);
    }

    throw std::runtime_error(
        "ffmpeg.exe was not found.\n"
        "Put ffmpeg.exe next to ASCIIPlayer.exe, or add its folder to PATH.\n"
        "Download: https://www.gyan.dev/ffmpeg/builds/");
}

MediaInfo FFmpeg::probe(const InputSpec& input) const {
    std::error_code ec;
    check(fs::is_regular_file(input.path, ec),
          "Media file not found: " + pathToUtf8(input.path));
    if (!input.init.empty()) {
        check(fs::is_regular_file(input.init, ec),
              "Initialization segment not found: " + pathToUtf8(input.init));
    }

    std::vector<std::wstring> args = {
        L"-hide_banner", L"-nostdin", L"-i", input.ffmpegInput()};
    ProcessResult result = runMerged(args);
    check(result.started, result.errors);
    const std::string& text = result.output;

    MediaInfo info;
    std::regex durationRe(R"(Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?))");
    std::smatch match;
    if (std::regex_search(text, match, durationRe)) {
        info.duration = std::stod(match[1]) * 3600.0 + std::stod(match[2]) * 60.0 +
                        std::stod(match[3]);
    }

    std::istringstream stream(text);
    std::string line;
    std::regex resolutionRe(R"((\d{2,5})x(\d{2,5}))");
    std::regex fpsRe(R"(([0-9]+(?:\.[0-9]+)?)\s*fps)");
    std::regex sampleRateRe(R"((\d{3,6})\s*Hz)");
    while (std::getline(stream, line)) {
        if (line.find("Stream #") == std::string::npos) continue;
        bool video = line.find("Video:") != std::string::npos;
        bool audio = line.find("Audio:") != std::string::npos;
        if (video && !info.hasVideo) {
            if (line.find("attached pic") != std::string::npos) continue;
            if (std::regex_search(line, match, resolutionRe)) {
                info.hasVideo = true;
                info.width = std::stoi(match[1]);
                info.height = std::stoi(match[2]);
                if (std::regex_search(line, match, fpsRe)) {
                    info.fps = std::stod(match[1]);
                }
                size_t name = line.find("Video: ");
                if (name != std::string::npos) {
                    name += 7;
                    size_t end = line.find_first_of(" (,", name);
                    info.videoCodec = line.substr(name, end - name);
                }
            }
        } else if (audio && !info.hasAudio) {
            info.hasAudio = true;
            if (std::regex_search(line, match, sampleRateRe)) {
                info.audioSampleRate = std::stoi(match[1]);
            }
            if (line.find("stereo") != std::string::npos)
                info.audioChannels = 2;
            else if (line.find("mono") != std::string::npos)
                info.audioChannels = 1;
            else if (line.find("5.1") != std::string::npos)
                info.audioChannels = 6;
            size_t name = line.find("Audio: ");
            if (name != std::string::npos) {
                name += 7;
                size_t end = line.find_first_of(" (,", name);
                info.audioCodec = line.substr(name, end - name);
            }
        }
    }

    std::regex rotationRe(R"(rotation of\s+(-?[0-9]+(?:\.[0-9]+)?)\s+degrees)");
    if (std::regex_search(text, match, rotationRe)) {
        info.rotation = static_cast<int>(std::lround(std::stod(match[1])));
    }

    if (!info.hasVideo && !info.hasAudio) {
        std::string detail = tail(text, 1800);
        std::string hint;
        if (endsWithI(pathToUtf8(input.path), ".m4s") && input.init.empty()) {
            hint =
                "\nThis looks like a DASH/fMP4 media segment. It needs its init segment:\n"
                "  ASCIIPlayer.exe segment.m4s --init init_0.mp4\n"
                "Or pass a separate audio file with --audio audio.m4a.";
        }
        throw std::runtime_error("ffmpeg could not find an audio or video stream.\n" +
                                 detail + hint);
    }
    if (!info.hasVideo && info.hasAudio && info.audioSampleRate <= 0)
        info.audioSampleRate = 48000;
    if (info.fps <= 0.0) info.fps = 30.0;
    info.fps = std::clamp(info.fps, 1.0, 120.0);
    return info;
}

ProcessResult FFmpeg::runMerged(const std::vector<std::wstring>& args) const {
    return ap::runMerged(path_, args);
}

ProcessResult FFmpeg::runQuiet(const std::vector<std::wstring>& args) const {
    return ap::runQuiet(path_, args);
}

VideoStream::VideoStream(const FFmpeg& ffmpeg, const InputSpec& input, int outWidth,
                         int outHeight, double fps, double startSeconds,
                         double durationSeconds)
    : frameBytes_(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 3) {
    std::vector<std::wstring> args = {L"-hide_banner", L"-loglevel", L"error",
                                      L"-nostdin"};
    if (startSeconds > 0.0001) {
        args.push_back(L"-ss");
        args.push_back(numberArg(startSeconds));
    }
    if (durationSeconds > 0.0001) {
        args.push_back(L"-t");
        args.push_back(numberArg(durationSeconds));
    }
    args.push_back(L"-i");
    args.push_back(input.ffmpegInput());
    args.insert(args.end(), {L"-map", L"0:v:0", L"-an", L"-sn", L"-dn", L"-vf",
                             L"scale=" + std::to_wstring(outWidth) + L":" +
                                 std::to_wstring(outHeight) + L":flags=area",
                             L"-fps_mode", L"cfr", L"-r", numberArg(fps), L"-f",
                             L"rawvideo", L"-pix_fmt", L"rgb24", L"pipe:1"});
    process_ = std::make_unique<PipeProcess>(ffmpeg.path(), args, true, false);
}

VideoStream::~VideoStream() = default;

bool VideoStream::next(std::vector<uint8_t>& frame) {
    frame.resize(frameBytes_);
    if (!process_) return false;
    if (!process_->readExact(frame.data(), frameBytes_)) {
        return false;
    }
    ++framesRead_;
    return true;
}

int VideoStream::finish() {
    if (!process_) return -1;
    int code = process_->wait();
    errorText_ = process_->errorLog();
    return code;
}

void VideoStream::cancel() {
    if (!process_) return;
    process_->terminate();
}

}  // namespace ap
