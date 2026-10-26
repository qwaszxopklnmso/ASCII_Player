#include "common.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ap {

#ifdef _WIN32

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

#else

namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::wstring utf8ToWide(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0;
        size_t length = 0;
        if (c < 0x80) {
            cp = c;
            length = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            length = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            length = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            length = 4;
        } else {
            out.push_back(static_cast<wchar_t>(0xFFFD));
            ++i;
            continue;
        }
        if (i + length > text.size()) {
            out.push_back(static_cast<wchar_t>(0xFFFD));
            break;
        }
        bool valid = true;
        for (size_t j = 1; j < length; ++j) {
            unsigned char next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (next & 0x3F);
        }
        if (!valid || (length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) ||
            (length == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(static_cast<wchar_t>(0xFFFD));
            ++i;
            continue;
        }
        out.push_back(static_cast<wchar_t>(cp));
        i += length;
    }
    return out;
}

std::string wideToUtf8(const std::wstring& text) {
    std::string out;
    out.reserve(text.size() * 3);
    for (wchar_t wc : text) {
        uint32_t cp = static_cast<uint32_t>(wc);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            cp = 0xFFFD;
        appendUtf8(out, cp);
    }
    return out;
}

#endif

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

#ifdef _WIN32

fs::path exePath() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    buf.resize(n);
    return fs::path(buf);
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

fs::path findInPath(const std::wstring& name) {
    std::wstring found(32768, L'\0');
    DWORD length = SearchPathW(nullptr, name.c_str(), nullptr,
                               static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (length == 0 || length >= found.size()) return {};
    found.resize(length);
    return fs::path(found);
}

#else

fs::path exePath() {
    std::vector<char> buffer(4096);
    while (true) {
        ssize_t n = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (n < 0) return {};
        if (static_cast<size_t>(n) < buffer.size()) {
            buffer.resize(static_cast<size_t>(n));
            return fs::path(std::string(buffer.begin(), buffer.end()));
        }
        buffer.resize(buffer.size() * 2);
    }
}

fs::path tempDir() {
    const char* env = std::getenv("TMPDIR");
    fs::path dir = (env && *env) ? fs::path(env) : fs::path("/tmp");
    std::error_code ec;
    if (fs::is_directory(dir, ec)) return dir;
    return fs::temp_directory_path();
}

fs::path uniqueTempPath(const std::wstring& stem, const std::wstring& ext) {
    static std::atomic<unsigned long long> counter{0};
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream ss;
    ss << wideToUtf8(stem) << "_" << ::getpid() << "_" << now << "_"
       << counter.fetch_add(1) << wideToUtf8(ext);
    return tempDir() / ss.str();
}

fs::path makeWritableTempFile(const std::wstring& stem, const std::wstring& ext) {
    static std::atomic<unsigned long long> counter{0};
    std::vector<fs::path> candidates;
    candidates.push_back(tempDir());

    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) candidates.push_back(fs::path(runtime));
    candidates.push_back(exeDir());
    candidates.push_back(currentDir());

    int lastError = ENOENT;
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (const fs::path& dir : candidates) {
        if (dir.empty()) continue;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            fs::create_directories(dir, ec);
            if (ec || !fs::is_directory(dir, ec)) continue;
        }
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::ostringstream name;
            name << wideToUtf8(stem) << "_" << ::getpid() << "_" << now << "_"
                 << counter.fetch_add(1) << wideToUtf8(ext);
            fs::path path = dir / name.str();
            int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) {
                ::close(fd);
                return path;
            }
            lastError = errno;
            if (lastError != EEXIST) break;
        }
    }
    throw std::runtime_error(std::string("Could not create a temporary media file: ") +
                             std::strerror(lastError));
}

fs::path findInPath(const std::wstring& name) {
    std::string text = wideToUtf8(name);
    if (text.empty()) return {};
    if (text.find('/') != std::string::npos) {
        fs::path direct(text);
        return isExecutableFile(direct) ? direct : fs::path();
    }
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return {};
    std::string paths(pathEnv);
    size_t start = 0;
    while (start <= paths.size()) {
        size_t end = paths.find(':', start);
        std::string part = paths.substr(start, end == std::string::npos
                                                   ? std::string::npos
                                                   : end - start);
        if (part.empty()) part = ".";
        fs::path candidate = fs::path(part) / text;
        if (isExecutableFile(candidate)) {
            std::error_code ec;
            fs::path absolute = fs::absolute(candidate, ec);
            return ec ? candidate : absolute;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

#endif

bool isExecutableFile(const fs::path& path) {
    if (path.empty()) return false;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return false;
#ifndef _WIN32
    return ::access(path.c_str(), X_OK) == 0;
#else
    return true;
#endif
}

fs::path currentDir() {
    try {
        return fs::current_path();
    } catch (...) {
        return {};
    }
}

fs::path exeDir() {
    fs::path p = exePath();
    return p.has_parent_path() ? p.parent_path() : currentDir();
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

#ifdef _WIN32
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
#endif

std::wstring pathToWide(const fs::path& p) {
#ifdef _WIN32
    return p.wstring();
#else
    return utf8ToWide(p.native());
#endif
}

std::string pathToUtf8(const fs::path& p) {
#ifdef _WIN32
    return wideToUtf8(p.wstring());
#else
    return p.native();
#endif
}

fs::path pathFromUtf8(const std::string& text) {
#ifdef _WIN32
    return fs::path(utf8ToWide(text));
#else
    return fs::path(text);
#endif
}

const char* programName() {
#ifdef _WIN32
    return "ASCIIPlayer.exe";
#else
    return "ascii-player";
#endif
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
