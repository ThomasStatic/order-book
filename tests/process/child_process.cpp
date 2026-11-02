#include "child_process.hpp"

#include <array>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace order_book::process_testing {

#ifdef _WIN32
    namespace {
        // One argument, quoted the way CommandLineToArgvW reads it back.
        std::string quoted(const std::string& argument) {
            if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos) {
                return argument;
            }
            std::string out = "\"";
            std::size_t backslashes = 0;
            for (const char character : argument) {
                if (character == '\\') {
                    ++backslashes;
                    continue;
                }
                out.append(character == '"' ? backslashes * 2 + 1 : backslashes, '\\');
                backslashes = 0;
                out.push_back(character);
            }
            out.append(backslashes * 2, '\\');
            return out + "\"";
        }
    }  // namespace

    struct ChildProcess::Platform {
        HANDLE process = nullptr;
        HANDLE stdinWrite = nullptr;
        HANDLE outputs[2] = {nullptr, nullptr};  // the read ends of stdout and stderr
    };

    ChildProcess::ChildProcess(const std::string& program, const std::vector<std::string>& arguments)
        : platform(std::make_unique<Platform>()) {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE childStdin = nullptr;
        HANDLE childOutputs[2] = {nullptr, nullptr};
        if (!CreatePipe(&childStdin, &platform->stdinWrite, &inherit, 0) || !CreatePipe(&platform->outputs[0], &childOutputs[0], &inherit, 0) ||
            !CreatePipe(&platform->outputs[1], &childOutputs[1], &inherit, 0)) {
            throw std::runtime_error("CreatePipe failed");
        }
        // Only the child's ends are inherited.
        SetHandleInformation(platform->stdinWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(platform->outputs[0], HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(platform->outputs[1], HANDLE_FLAG_INHERIT, 0);

        std::string commandLine = quoted(program);
        for (const std::string& argument : arguments) {
            commandLine += ' ' + quoted(argument);
        }
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childStdin;
        startup.hStdOutput = childOutputs[0];
        startup.hStdError = childOutputs[1];
        PROCESS_INFORMATION created{};
        const BOOL started = CreateProcessA(program.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                            &startup, &created);
        CloseHandle(childStdin);
        CloseHandle(childOutputs[0]);
        CloseHandle(childOutputs[1]);
        if (!started) {
            throw std::runtime_error("could not start " + program);
        }
        CloseHandle(created.hThread);
        platform->process = created.hProcess;
        for (int which = 0; which < 2; ++which) {
            readers[which] = std::thread([this, which] { collect(which); });
        }
    }

    void ChildProcess::collect(int which) {
        std::array<char, 4096> buffer;
        DWORD read = 0;
        while (ReadFile(platform->outputs[which], buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read != 0) {
            const std::lock_guard<std::mutex> hold(guard);
            streams[which].text.append(buffer.data(), read);
            changed.notify_all();
        }
        const std::lock_guard<std::mutex> hold(guard);
        streams[which].ended = true;
        changed.notify_all();
    }

    void ChildProcess::writeStdin(const std::string& text) {
        DWORD written = 0;
        if (platform->stdinWrite != nullptr) {
            WriteFile(platform->stdinWrite, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        }
    }

    void ChildProcess::closeStdin() {
        if (platform->stdinWrite != nullptr) {
            CloseHandle(platform->stdinWrite);
            platform->stdinWrite = nullptr;
        }
    }

    void ChildProcess::kill() {
        TerminateProcess(platform->process, 137);
    }

    ExitStatus ChildProcess::wait(std::chrono::milliseconds limit) {
        if (status) {
            return *status;
        }
        ExitStatus result;
        if (WaitForSingleObject(platform->process, static_cast<DWORD>(limit.count())) != WAIT_OBJECT_0) {
            kill();
            WaitForSingleObject(platform->process, INFINITE);
            result.killed = true;
        }
        DWORD code = 0;
        GetExitCodeProcess(platform->process, &code);
        result.code = static_cast<int>(code);
        status = result;
        awaitStreamsEnded();
        return result;
    }

    ChildProcess::~ChildProcess() {
        if (!status) {
            kill();
            wait(std::chrono::milliseconds(10'000));
        }
        closeStdin();
        for (std::thread& reader : readers) {
            if (reader.joinable()) {
                reader.join();
            }
        }
        for (HANDLE output : platform->outputs) {
            CloseHandle(output);
        }
        CloseHandle(platform->process);
    }
#else
    struct ChildProcess::Platform {
        pid_t process = -1;
        int stdinWrite = -1;
        int outputs[2] = {-1, -1};
    };

    ChildProcess::ChildProcess(const std::string& program, const std::vector<std::string>& arguments)
        : platform(std::make_unique<Platform>()) {
        int stdinPipe[2];
        int outputPipes[2][2];
        if (pipe(stdinPipe) != 0 || pipe(outputPipes[0]) != 0 || pipe(outputPipes[1]) != 0) {
            throw std::runtime_error("pipe failed");
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, stdinPipe[0], 0);
        posix_spawn_file_actions_adddup2(&actions, outputPipes[0][1], 1);
        posix_spawn_file_actions_adddup2(&actions, outputPipes[1][1], 2);
        for (const int end : {stdinPipe[0], stdinPipe[1], outputPipes[0][0], outputPipes[0][1], outputPipes[1][0], outputPipes[1][1]}) {
            posix_spawn_file_actions_addclose(&actions, end);
        }
        std::vector<std::string> owned{program};
        owned.insert(owned.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (std::string& argument : owned) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        const int spawned = posix_spawn(&platform->process, program.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(stdinPipe[0]);
        close(outputPipes[0][1]);
        close(outputPipes[1][1]);
        platform->stdinWrite = stdinPipe[1];
        platform->outputs[0] = outputPipes[0][0];
        platform->outputs[1] = outputPipes[1][0];
        if (spawned != 0) {
            throw std::runtime_error("could not start " + program);
        }
        // A child that stops reading stdin must not take this process down with SIGPIPE.
        std::signal(SIGPIPE, SIG_IGN);
        for (int which = 0; which < 2; ++which) {
            readers[which] = std::thread([this, which] { collect(which); });
        }
    }

    void ChildProcess::collect(int which) {
        std::array<char, 4096> buffer;
        for (;;) {
            const ssize_t got = read(platform->outputs[which], buffer.data(), buffer.size());
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got <= 0) {
                break;
            }
            const std::lock_guard<std::mutex> hold(guard);
            streams[which].text.append(buffer.data(), static_cast<std::size_t>(got));
            changed.notify_all();
        }
        const std::lock_guard<std::mutex> hold(guard);
        streams[which].ended = true;
        changed.notify_all();
    }

    void ChildProcess::writeStdin(const std::string& text) {
        std::size_t sent = 0;
        while (platform->stdinWrite >= 0 && sent < text.size()) {
            const ssize_t wrote = write(platform->stdinWrite, text.data() + sent, text.size() - sent);
            if (wrote <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(wrote);
        }
    }

    void ChildProcess::closeStdin() {
        if (platform->stdinWrite >= 0) {
            close(platform->stdinWrite);
            platform->stdinWrite = -1;
        }
    }

    void ChildProcess::kill() {
        ::kill(platform->process, SIGKILL);
    }

    ExitStatus ChildProcess::wait(std::chrono::milliseconds limit) {
        if (status) {
            return *status;
        }
        ExitStatus result;
        int raw = 0;
        const auto deadline = std::chrono::steady_clock::now() + limit;
        for (;;) {
            const pid_t done = waitpid(platform->process, &raw, WNOHANG);
            if (done == platform->process) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                kill();
                waitpid(platform->process, &raw, 0);
                result.killed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        result.code = WIFEXITED(raw) ? WEXITSTATUS(raw) : WIFSIGNALED(raw) ? 128 + WTERMSIG(raw) : -1;
        status = result;
        awaitStreamsEnded();
        return result;
    }

    ChildProcess::~ChildProcess() {
        if (!status) {
            kill();
            wait(std::chrono::milliseconds(10'000));
        }
        closeStdin();
        for (std::thread& reader : readers) {
            if (reader.joinable()) {
                reader.join();
            }
        }
        for (const int output : platform->outputs) {
            close(output);
        }
    }
#endif

    // The child has gone, so its pipes end as soon as the readers drain them.
    void ChildProcess::awaitStreamsEnded() {
        std::unique_lock<std::mutex> hold(guard);
        changed.wait_for(hold, std::chrono::seconds(30), [this] { return streams[0].ended && streams[1].ended; });
    }

    std::optional<std::string> ChildProcess::readLine(std::chrono::milliseconds limit) {
        std::unique_lock<std::mutex> hold(guard);
        Stream& output = streams[0];
        const bool found = changed.wait_for(hold, limit, [&output] {
            return output.text.find('\n', output.lineStart) != std::string::npos || output.ended;
        });
        const std::size_t newline = output.text.find('\n', output.lineStart);
        if (!found || newline == std::string::npos) {
            hold.unlock();
            if (!found) {
                kill();
            }
            return std::nullopt;
        }
        std::string line = output.text.substr(output.lineStart, newline - output.lineStart);
        output.lineStart = newline + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return line;
    }

    std::string ChildProcess::standardOutput() {
        const std::lock_guard<std::mutex> hold(guard);
        return streams[0].text;
    }

    std::string ChildProcess::standardError() {
        const std::lock_guard<std::mutex> hold(guard);
        return streams[1].text;
    }

}  // namespace order_book::process_testing
