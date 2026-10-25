#include "common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <system_error>

namespace ap {

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return std::wstring(text.begin(), text.end());
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string trim(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return text.substr(first, last - first);
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool endsWithI(const std::string& text, const std::string& suffix) {
    if (suffix.size() > text.size()) return false;
    return iequals(text.substr(text.size() - suffix.size()), suffix);
}

fs::path exePath() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    buf.resize(n);
    return fs::path(buf);
}

fs::path exeDir() {
    fs::path p = exePath();
    return p.has_parent_path() ? p.parent_path() : currentDir();
}

fs::path currentDir() {
    try {
        return fs::current_path();
    } catch (...) {
        return {};
    }
}

fs::path tempDir() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetTempPathW(static_cast<DWORD>(buf.size()), buf.data());
    if (n == 0 || n >= buf.size()) return fs::temp_directory_path();
    buf.resize(n);
    return fs::path(buf);
}

fs::path uniqueTempPath(const std::wstring& stem, const std::wstring& ext) {
    static std::atomic<unsigned long long> counter{0};
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::wstringstream ss;
    ss << stem << L"_" << GetCurrentProcessId() << L"_" << now << L"_"
       << counter.fetch_add(1) << ext;
    return tempDir() / ss.str();
}

fs::path makeWritableTempFile(const std::wstring& stem, const std::wstring& ext) {
    static std::atomic<unsigned long long> counter{0};
    std::vector<fs::path> candidates;
    candidates.push_back(tempDir());

    wchar_t localAppData[32768];
    DWORD localLen = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
                                             static_cast<DWORD>(std::size(localAppData)));
    if (localLen > 0 && localLen < std::size(localAppData))
        candidates.push_back(fs::path(localAppData) / L"Temp");
    candidates.push_back(exeDir());
    candidates.push_back(currentDir());

    DWORD lastError = ERROR_PATH_NOT_FOUND;
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (const fs::path& dir : candidates) {
        if (dir.empty()) continue;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            fs::create_directories(dir, ec);
            if (ec || !fs::is_directory(dir, ec)) continue;
        }
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::wstringstream name;
            name << stem << L"_" << GetCurrentProcessId() << L"_" << now << L"_"
                 << counter.fetch_add(1) << ext;
            fs::path path = dir / name.str();
            HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
                return path;
            }
            lastError = GetLastError();
            if (lastError != ERROR_FILE_EXISTS) break;
        }
    }
    throw std::runtime_error("Could not create a temporary media file: " +
                             std::error_code(lastError, std::system_category()).message());
}

std::string formatDuration(double seconds) {
    if (!(seconds >= 0.0) || seconds > 1e9) seconds = 0.0;
    long long total = static_cast<long long>(seconds + 0.5);
    long long h = total / 3600;
    long long m = (total / 60) % 60;
    long long s = total % 60;
    char buf[64];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
    else
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld", m, s);
    return buf;
}

std::string formatSize(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64];
    if (unit == 0)
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else
        std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return buf;
}

std::string basenameUtf8(const fs::path& path) {
    return pathToUtf8(path.filename());
}

std::wstring quoteWindowsArg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;

    std::wstring out;
    out.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring pathToWide(const fs::path& p) {
    return p.wstring();
}

std::string pathToUtf8(const fs::path& p) {
    return wideToUtf8(p.wstring());
}

TempFile::~TempFile() {
    reset();
}

TempFile::TempFile(TempFile&& other) noexcept : path_(std::move(other.path_)) {
    other.path_.clear();
}

TempFile& TempFile::operator=(TempFile&& other) noexcept {
    if (this != &other) {
        reset();
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

void TempFile::reset(fs::path path) {
    if (!path_.empty()) {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    path_ = std::move(path);
}

}  // namespace ap
