// Small Win32 process wrapper used to drive ffmpeg without a shell.
#pragma once

#include "common.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

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

    bool valid() const { return process_ != nullptr; }
    bool captureStdout() const { return read_ != nullptr; }
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
    void startErrorReader(HANDLE readHandle);

    HANDLE process_ = nullptr;
    HANDLE read_ = nullptr;
    std::unique_ptr<std::thread> errorThread_;
    std::shared_ptr<ErrorState> errorState_;
};

ProcessResult runMerged(const fs::path& exe, const std::vector<std::wstring>& args,
                        size_t outputLimit = 8 * 1024 * 1024);
ProcessResult runQuiet(const fs::path& exe, const std::vector<std::wstring>& args);

}  // namespace ap
