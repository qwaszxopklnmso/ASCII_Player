#include "process.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ap {

#ifdef _WIN32

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

#else  // POSIX

namespace {

std::string errnoMessage(int code) {
    return std::string(std::strerror(code));
}

std::vector<std::string> narrowArgs(const fs::path& exe,
                                    const std::vector<std::wstring>& args) {
    std::vector<std::string> out;
    out.reserve(args.size() + 1);
    out.push_back(pathToUtf8(exe));
    for (const auto& arg : args) out.push_back(wideToUtf8(arg));
    return out;
}

}  // namespace

PipeProcess::PipeProcess(const fs::path& exe, const std::vector<std::wstring>& args,
                         bool captureStdout, bool mergeStderr) {
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};

    if (captureStdout && ::pipe(outPipe) != 0) {
        int error = errno;
        check(false, "Could not create stdout pipe: " + errnoMessage(error));
    }
    if (!(mergeStderr && captureStdout)) {
        if (::pipe(errPipe) != 0) {
            int error = errno;
            if (outPipe[0] >= 0) ::close(outPipe[0]);
            if (outPipe[1] >= 0) ::close(outPipe[1]);
            check(false, "Could not create stderr pipe: " + errnoMessage(error));
        }
    }

    std::vector<std::string> command = narrowArgs(exe, args);
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (auto& item : command) argv.push_back(item.data());
    argv.push_back(nullptr);

    pid_t child = ::fork();
    if (child < 0) {
        int error = errno;
        if (outPipe[0] >= 0) ::close(outPipe[0]);
        if (outPipe[1] >= 0) ::close(outPipe[1]);
        if (errPipe[0] >= 0) ::close(errPipe[0]);
        if (errPipe[1] >= 0) ::close(errPipe[1]);
        check(false, "Could not start ffmpeg: " + errnoMessage(error) + "\n  " +
                         pathToUtf8(exe));
    }

    if (child == 0) {
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull < 0) _exit(127);
        ::dup2(devnull, STDIN_FILENO);
        if (captureStdout)
            ::dup2(outPipe[1], STDOUT_FILENO);
        else
            ::dup2(devnull, STDOUT_FILENO);
        if (mergeStderr && captureStdout)
            ::dup2(outPipe[1], STDERR_FILENO);
        else
            ::dup2(errPipe[1], STDERR_FILENO);

        if (outPipe[0] >= 0) ::close(outPipe[0]);
        if (outPipe[1] >= 0) ::close(outPipe[1]);
        if (errPipe[0] >= 0) ::close(errPipe[0]);
        if (errPipe[1] >= 0) ::close(errPipe[1]);
        if (devnull > STDERR_FILENO) ::close(devnull);
        ::execv(exe.c_str(), argv.data());
        _exit(127);
    }

    if (outPipe[1] >= 0) ::close(outPipe[1]);
    if (errPipe[1] >= 0) ::close(errPipe[1]);

    process_ = child;
    if (captureStdout) {
        read_ = outPipe[0];
        ::fcntl(read_, F_SETFD, ::fcntl(read_, F_GETFD) | FD_CLOEXEC);
    }
    if (!(mergeStderr && captureStdout)) {
        ::fcntl(errPipe[0], F_SETFD, ::fcntl(errPipe[0], F_GETFD) | FD_CLOEXEC);
        startErrorReader(errPipe[0]);
    }
}

PipeProcess::~PipeProcess() {
    cleanup();
}

PipeProcess::PipeProcess(PipeProcess&& other) noexcept
    : process_(other.process_),
      read_(other.read_),
      exited_(other.exited_),
      status_(other.status_),
      errorThread_(std::move(other.errorThread_)),
      errorState_(std::move(other.errorState_)) {
    other.process_ = -1;
    other.read_ = -1;
}

PipeProcess& PipeProcess::operator=(PipeProcess&& other) noexcept {
    if (this != &other) {
        cleanup();
        process_ = other.process_;
        read_ = other.read_;
        exited_ = other.exited_;
        status_ = other.status_;
        errorThread_ = std::move(other.errorThread_);
        errorState_ = std::move(other.errorState_);
        other.process_ = -1;
        other.read_ = -1;
    }
    return *this;
}

void PipeProcess::startErrorReader(int readFd) {
    if (readFd < 0) return;
    errorState_ = std::make_shared<ErrorState>();
    try {
        auto state = errorState_;
        errorThread_ = std::make_unique<std::thread>([readFd, state]() {
            char buffer[8192];
            constexpr size_t kLimit = 1024 * 1024;
            while (true) {
                ssize_t got = ::read(readFd, buffer, sizeof(buffer));
                if (got < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (got == 0) break;
                if (state->text.size() < kLimit) {
                    size_t room = kLimit - state->text.size();
                    state->text.append(buffer, std::min<size_t>(
                                                   static_cast<size_t>(got), room));
                }
            }
            ::close(readFd);
        });
    } catch (const std::exception& e) {
        ::close(readFd);
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
    if (read_ < 0 || bytes == 0) return 0;
    while (true) {
        ssize_t got = ::read(read_, buffer, bytes);
        if (got < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        return static_cast<size_t>(got);
    }
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
    if (process_ <= 0) {
        joinErrorReader();
        return -1;
    }
    if (!exited_) {
        int status = 0;
        pid_t got = 0;
        do {
            got = ::waitpid(process_, &status, 0);
        } while (got < 0 && errno == EINTR);
        if (got == process_) {
            exited_ = true;
            status_ = status;
        }
    }
    int code = -1;
    if (exited_) {
        if (WIFEXITED(status_))
            code = WEXITSTATUS(status_);
        else if (WIFSIGNALED(status_))
            code = 128 + WTERMSIG(status_);
    }
    process_ = -1;
    if (read_ >= 0) {
        ::close(read_);
        read_ = -1;
    }
    joinErrorReader();
    return code;
}

bool PipeProcess::running() const {
    if (process_ <= 0 || exited_) return false;
    int status = 0;
    pid_t got = ::waitpid(process_, &status, WNOHANG);
    if (got == 0) return true;
    if (got == process_) {
        exited_ = true;
        status_ = status;
    }
    return false;
}

void PipeProcess::terminate() {
    if (process_ <= 0 || exited_) return;
    ::kill(process_, SIGTERM);
    for (int i = 0; i < 100; ++i) {
        if (!running()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(process_, SIGKILL);
    int status = 0;
    while (::waitpid(process_, &status, 0) < 0 && errno == EINTR) {
    }
    exited_ = true;
    status_ = status;
}

void PipeProcess::cleanup() {
    if (read_ >= 0) {
        ::close(read_);
        read_ = -1;
    }
    if (process_ > 0) {
        if (!exited_) {
            int status = 0;
            pid_t got = ::waitpid(process_, &status, WNOHANG);
            if (got == process_) {
                exited_ = true;
                status_ = status;
            } else if (got == 0) {
                ::kill(process_, SIGTERM);
                for (int i = 0; i < 50; ++i) {
                    got = ::waitpid(process_, &status, WNOHANG);
                    if (got == process_) {
                        exited_ = true;
                        status_ = status;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (!exited_) {
                    ::kill(process_, SIGKILL);
                    while (::waitpid(process_, &status, 0) < 0 && errno == EINTR) {
                    }
                    exited_ = true;
                    status_ = status;
                }
            }
        }
        process_ = -1;
    }
    joinErrorReader();
}

#endif

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
