// ASCII Player - shared Windows/UTF-8 helpers.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ap {

namespace fs = std::filesystem;

inline void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

std::wstring utf8ToWide(const std::string& text);
std::string wideToUtf8(const std::wstring& text);
std::string toLower(std::string text);
std::string trim(const std::string& text);
bool iequals(const std::string& a, const std::string& b);
bool endsWithI(const std::string& text, const std::string& suffix);

fs::path exePath();
fs::path exeDir();
fs::path currentDir();
fs::path tempDir();
fs::path uniqueTempPath(const std::wstring& stem, const std::wstring& ext);
fs::path makeWritableTempFile(const std::wstring& stem, const std::wstring& ext);
std::string formatDuration(double seconds);
std::string formatSize(uint64_t bytes);
std::string basenameUtf8(const fs::path& path);

// Windows argument quoting for CreateProcessW command lines.
std::wstring quoteWindowsArg(const std::wstring& arg);

std::wstring pathToWide(const fs::path& p);
std::string pathToUtf8(const fs::path& p);

class TempFile {
public:
    TempFile() = default;
    explicit TempFile(fs::path path) : path_(std::move(path)) {}
    ~TempFile();
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& other) noexcept;
    TempFile& operator=(TempFile&& other) noexcept;

    const fs::path& path() const { return path_; }
    void reset(fs::path path = {});
    void keep() { path_.clear(); }

private:
    fs::path path_;
};

class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~ScopeExit() {
        if (fn_) fn_();
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    void dismiss() { fn_ = nullptr; }

private:
    std::function<void()> fn_;
};

}  // namespace ap
