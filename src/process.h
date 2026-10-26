// Small native process wrapper used to drive ffmpeg without a shell.
#pragma once

#include "common.h"

#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#endif

namespace ap {

struct ProcessResult {
    int exitCode = -1;
    std::string output;
    std::string errors;
    bool started = false;
};

class PipeProcess {
public:
    PipeProcess() = default;
    PipeProcess(const fs::path& exe, const std::vector<std::wstring>& args,
                bool captureStdout, bool mergeStderr);
    ~PipeProcess();

    PipeProcess(const PipeProcess&) = delete;
    PipeProcess& operator=(const PipeProcess&) = delete;
    PipeProcess(PipeProcess&& other) noexcept;
    PipeProcess& operator=(PipeProcess&& other) noexcept;

    bool valid() const {
#ifdef _WIN32
        return process_ != nullptr;
#else
        return process_ > 0;
#endif
    }
    bool captureStdout() const {
#ifdef _WIN32
        return read_ != nullptr;
#else
        return read_ >= 0;
#endif
    }
    size_t read(void* buffer, size_t bytes);
    bool readExact(void* buffer, size_t bytes);
    std::string readAll(size_t limit = 8 * 1024 * 1024);
    int wait();
    bool running() const;
    void terminate();
    const std::string& errorLog() const;

private:
    struct ErrorState {
        std::string text;
    };

    void cleanup();
    void joinErrorReader();
#ifdef _WIN32
    void startErrorReader(HANDLE readHandle);
    HANDLE process_ = nullptr;
    HANDLE read_ = nullptr;
#else
    void startErrorReader(int readFd);
    pid_t process_ = -1;
    int read_ = -1;
    mutable bool exited_ = false;
    mutable int status_ = 0;
#endif
    std::unique_ptr<std::thread> errorThread_;
    std::shared_ptr<ErrorState> errorState_;
};

ProcessResult runMerged(const fs::path& exe, const std::vector<std::wstring>& args,
                        size_t outputLimit = 8 * 1024 * 1024);
ProcessResult runQuiet(const fs::path& exe, const std::vector<std::wstring>& args);

}  // namespace ap
