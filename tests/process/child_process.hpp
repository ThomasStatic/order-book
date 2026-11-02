#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// A real child process for tests: a program run with its stdin, stdout, and stderr piped here.
// Every wait has a hard limit, and a wait that runs out kills the child, so a hang fails a test
// rather than stalling CI. See docs/internals/testing.md.
namespace order_book::process_testing {

    struct ExitStatus {
        bool killed = false;  // by a wait that ran out
        int code = 0;         // the exit status, or on POSIX 128 plus a signal that ended it
    };

    struct ChildOptions {
        // Windows only: the child shares this process's console in a process group of its own,
        // so sendCtrlBreak reaches it and nothing else. Elsewhere it changes nothing.
        bool ownProcessGroup = false;
    };

    class ChildProcess {
    public:
        // Throws std::runtime_error if the program can't be started.
        ChildProcess(const std::string& program, const std::vector<std::string>& arguments, ChildOptions options = {});
        // Kills the child if it is still running.
        ~ChildProcess();
        ChildProcess(const ChildProcess&) = delete;
        ChildProcess& operator=(const ChildProcess&) = delete;

        // The next whole line of stdout, without its newline, or nothing if none came within the
        // limit, in which case the child is killed.
        std::optional<std::string> readLine(std::chrono::milliseconds limit);
        void writeStdin(const std::string& text);
        void closeStdin();
        // Waits for the child to exit, killing it if it hasn't within the limit, then for both its
        // streams to end, so standardOutput and standardError hold everything it wrote.
        ExitStatus wait(std::chrono::milliseconds limit);
        // Whether the child is still running once this long has passed, killing nothing. If it
        // exited sooner, wait then says how.
        bool stillRunningAfter(std::chrono::milliseconds limit);
#ifdef _WIN32
        // Ctrl-Break to the child's process group, which needs ownProcessGroup and a console in
        // this process. Whether the system sent it.
        bool sendCtrlBreak();
#else
        // Whether the system sent it.
        bool sendSignal(int signal);
#endif
        // Everything each stream has written so far, readLine's lines included.
        std::string standardOutput();
        std::string standardError();

    private:
        struct Platform;
        struct Stream {
            std::string text;
            std::size_t lineStart = 0;  // of stdout, what readLine has handed out
            bool ended = false;
        };

        void kill();
        void collect(int which);  // 0 stdout, 1 stderr: a reader thread's body
        void awaitStreamsEnded();

        std::unique_ptr<Platform> platform;
        std::mutex guard;
        std::condition_variable changed;
        Stream streams[2];
        std::thread readers[2];
        std::optional<ExitStatus> status;
    };

}  // namespace order_book::process_testing
