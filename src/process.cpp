#include "process.h"

#include <algorithm>

namespace ap {

namespace {

std::wstring buildCommandLine(const fs::path& exe, const std::vector<std::wstring>& args) {
    std::wstring line = quoteWindowsArg(exe.wstring());
    for (const auto& arg : args) {
        line.push_back(L' ');
        line += quoteWindowsArg(arg);
    }
    return line;
}

std::string win32Message(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (n && buffer) text = wideToUtf8(std::wstring(buffer, n));
    if (buffer) LocalFree(buffer);
    text = trim(text);
    return text.empty() ? ("Win32 error " + std::to_string(code)) : text;
}

HANDLE openNul(DWORD access) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

PipeProcess::PipeProcess(const fs::path& exe, const std::vector<std::wstring>& args,
                         bool captureStdout, bool mergeStderr) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    HANDLE errRead = nullptr;
    HANDLE errWrite = nullptr;

    auto closeHandles = [&]() {
        if (outRead) CloseHandle(outRead);
        if (outWrite) CloseHandle(outWrite);
        if (errRead) CloseHandle(errRead);
        if (errWrite && errWrite != outWrite) CloseHandle(errWrite);
        outRead = outWrite = errRead = errWrite = nullptr;
    };

    if (captureStdout && !CreatePipe(&outRead, &outWrite, &sa, 0)) {
        DWORD error = GetLastError();
        closeHandles();
        check(false, "Could not create stdout pipe: " + win32Message(error));
    }
    if (outRead) SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    read_ = outRead;

    if (!(mergeStderr && captureStdout)) {
        if (!CreatePipe(&errRead, &errWrite, &sa, 0)) {
            DWORD error = GetLastError();
            closeHandles();
            read_ = nullptr;
            check(false, "Could not create stderr pipe: " + win32Message(error));
        }
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    } else {
        errWrite = outWrite;
    }

    HANDLE nulInput = openNul(GENERIC_READ);
    HANDLE nulOutput = captureStdout ? nullptr : openNul(GENERIC_WRITE);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nulInput;
    si.hStdOutput = captureStdout ? outWrite : nulOutput;
    si.hStdError = errWrite;
    PROCESS_INFORMATION pi{};

    std::wstring command = buildCommandLine(exe, args);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    BOOL ok = CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    DWORD createError = GetLastError();

    if (nulInput != INVALID_HANDLE_VALUE) CloseHandle(nulInput);
    if (nulOutput && nulOutput != INVALID_HANDLE_VALUE) CloseHandle(nulOutput);
    if (outWrite) CloseHandle(outWrite);
    if (errWrite && errWrite != outWrite) CloseHandle(errWrite);
    outWrite = nullptr;
    errWrite = nullptr;

    if (!ok) {
        closeHandles();
        read_ = nullptr;
        check(false,
              "Could not start ffmpeg: " + win32Message(createError) + "\n  " +
                  pathToUtf8(exe));
    }

    if (!(mergeStderr && captureStdout)) {
        startErrorReader(errRead);
        errRead = nullptr;
    }

    process_ = pi.hProcess;
    CloseHandle(pi.hThread);
}

PipeProcess::~PipeProcess() {
    cleanup();
}

PipeProcess::PipeProcess(PipeProcess&& other) noexcept
    : process_(other.process_),
      read_(other.read_),
      errorThread_(std::move(other.errorThread_)),
      errorState_(std::move(other.errorState_)) {
    other.process_ = nullptr;
    other.read_ = nullptr;
}

PipeProcess& PipeProcess::operator=(PipeProcess&& other) noexcept {
    if (this != &other) {
        cleanup();
        process_ = other.process_;
        read_ = other.read_;
        errorThread_ = std::move(other.errorThread_);
        errorState_ = std::move(other.errorState_);
        other.process_ = nullptr;
        other.read_ = nullptr;
    }
    return *this;
}

void PipeProcess::startErrorReader(HANDLE readHandle) {
    if (!readHandle || readHandle == INVALID_HANDLE_VALUE) return;
    errorState_ = std::make_shared<ErrorState>();
    try {
        auto state = errorState_;
        errorThread_ = std::make_unique<std::thread>([readHandle, state]() {
            char buffer[8192];
            DWORD got = 0;
            constexpr size_t kLimit = 1024 * 1024;
            while (ReadFile(readHandle, buffer, sizeof(buffer), &got, nullptr) &&
                   got > 0) {
                if (state->text.size() < kLimit) {
                    size_t room = kLimit - state->text.size();
                    state->text.append(buffer, std::min<size_t>(got, room));
                }
            }
            CloseHandle(readHandle);
        });
    } catch (const std::exception& e) {
        CloseHandle(readHandle);
        if (!errorState_) errorState_ = std::make_shared<ErrorState>();
        errorState_->text = std::string("stderr reader unavailable: ") + e.what();
    }
}

void PipeProcess::joinErrorReader() {
    if (errorThread_) {
        if (errorThread_->joinable()) errorThread_->join();
        errorThread_.reset();
    }
}

size_t PipeProcess::read(void* buffer, size_t bytes) {
    if (!read_ || bytes == 0) return 0;
    DWORD got = 0;
    DWORD want = static_cast<DWORD>(std::min<size_t>(bytes, 1u << 30));
    if (!ReadFile(read_, buffer, want, &got, nullptr)) {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) return 0;
        return 0;
    }
    return got;
}

bool PipeProcess::readExact(void* buffer, size_t bytes) {
    auto* p = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < bytes) {
        size_t n = read(p + done, bytes - done);
        if (n == 0) return false;
        done += n;
    }
    return true;
}

std::string PipeProcess::readAll(size_t limit) {
    std::string out;
    out.reserve(std::min<size_t>(limit, 256 * 1024));
    char buffer[65536];
    while (out.size() < limit) {
        size_t n = read(buffer, sizeof(buffer));
        if (n == 0) break;
        size_t room = limit - out.size();
        out.append(buffer, std::min(n, room));
    }
    char sink[65536];
    while (read(sink, sizeof(sink)) > 0) {
    }
    return out;
}

int PipeProcess::wait() {
    if (!process_) {
        joinErrorReader();
        return -1;
    }
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process_, &code);
    CloseHandle(process_);
    process_ = nullptr;
    if (read_) {
        CloseHandle(read_);
        read_ = nullptr;
    }
    joinErrorReader();
    return static_cast<int>(code);
}

bool PipeProcess::running() const {
    if (!process_) return false;
    return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

void PipeProcess::terminate() {
    if (process_) {
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, 2000);
    }
}

void PipeProcess::cleanup() {
    if (read_) {
        CloseHandle(read_);
        read_ = nullptr;
    }
    if (process_) {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 1000);
        }
        CloseHandle(process_);
        process_ = nullptr;
    }
    joinErrorReader();
}

const std::string& PipeProcess::errorLog() const {
    static const std::string empty;
    return errorState_ ? errorState_->text : empty;
}

ProcessResult runMerged(const fs::path& exe, const std::vector<std::wstring>& args,
                        size_t outputLimit) {
    ProcessResult result;
    try {
        PipeProcess process(exe, args, true, true);
        result.started = true;
        result.output = process.readAll(outputLimit);
        result.exitCode = process.wait();
        result.errors = process.errorLog();
    } catch (const std::exception& e) {
        result.started = false;
        result.errors = e.what();
    }
    return result;
}

ProcessResult runQuiet(const fs::path& exe, const std::vector<std::wstring>& args) {
    ProcessResult result;
    try {
        PipeProcess process(exe, args, false, false);
        result.started = true;
        result.exitCode = process.wait();
        result.errors = process.errorLog();
    } catch (const std::exception& e) {
        result.started = false;
        result.errors = e.what();
    }
    return result;
}

}  // namespace ap
