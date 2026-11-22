#pragma once
#include "../Source/Protocol.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace scbridge::move {
class Engine {
    using Clock = std::chrono::steady_clock;
    std::filesystem::path folder;
    int request = -1, response = -1;
    int runtimeLock = -1;
    pid_t child = -1;
    const std::atomic<bool> &stopping;

    bool transfer(int fd, void *data, size_t bytes, bool writing, Clock::time_point deadline) {
        auto *cursor = static_cast<unsigned char *>(data);
        while (bytes && !stopping.load()) {
            if (Clock::now() >= deadline)
                return false;
            pollfd p{fd, short(writing ? POLLOUT : POLLIN), 0};
            const int available = poll(&p, 1, 20);
            if (!available) {
                if (child > 0 && waitpid(child, nullptr, WNOHANG) == child) {
                    child = -1;
                    return false;
                }
                continue;
            }
            if (available < 0 && errno == EINTR)
                continue;
            if (available < 0 || !(p.revents & p.events))
                return false;
            const auto n = writing ? write(fd, cursor, bytes) : read(fd, cursor, bytes);
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (n <= 0)
                return false;
            cursor += n;
            bytes -= size_t(n);
        }
        return !bytes;
    }
    bool exited(unsigned attempts) {
        for (unsigned i = 0; i < attempts; ++i) {
            if (waitpid(child, nullptr, WNOHANG) == child)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

  public:
    Shared packet{};
    const char *startError = "Cannot start Move Canvas engine";
    explicit Engine(const std::atomic<bool> &stop) : stopping(stop) {
        packet.version = protocolVersion;
        packet.sampleRate = 44100;
        for (unsigned i = 0; i < parameterCount; ++i)
            packet.parameters[i] = controls[i].initial;
    }
    bool start(const std::filesystem::path &runtime) {
        // Both modules use this runtime. A second resident worker can exhaust Move's memory.
        runtimeLock = open(runtime.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (runtimeLock < 0)
            return false;
        if (flock(runtimeLock, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK)
                startError = "Close the other Move Canvas instance, then reopen this one";
            return false;
        }
        char pattern[] = "/dev/shm/sound-canvas-XXXXXX";
        if (!mkdtemp(pattern))
            return false;
        folder = pattern;
        const auto in = folder / "commands", out = folder / "responses";
        if (mkfifo(in.c_str(), 0600) || mkfifo(out.c_str(), 0600))
            return false;
        request = open(in.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        response = open(out.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (request < 0 || response < 0)
            return false;
        auto script = (runtime / "start-engine.sh").string(), directory = runtime.string(), work = folder.string();
        char shell[] = "/bin/sh";
        auto parent = std::to_string(getpid());
        char *args[]{shell, script.data(), directory.data(), work.data(), parent.data(), nullptr};
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        // A helper must not retain Move's audio, SPI or set file descriptors.
        posix_spawn_file_actions_addclosefrom_np(&actions, 3);
        pid_t pid;
        const int error = posix_spawn(&pid, shell, &actions, nullptr, args, environ);
        posix_spawn_file_actions_destroy(&actions);
        if (error)
            return false;
        child = pid;
        char ready = 0;
        return transfer(response, &ready, 1, false, Clock::now() + std::chrono::seconds(45)) && ready == 'R';
    }
    bool render(char command) {
        const auto deadline = Clock::now() + std::chrono::seconds(command == 'S' ? 30 : 4);
        constexpr size_t header = offsetof(Shared, events);
        const unsigned frames = packet.frames;
        char reply = 0;
        return transfer(request, &command, 1, true, deadline) &&
               transfer(request, &packet, header, true, deadline) &&
               transfer(request, packet.events, packet.eventCount * sizeof(Event), true, deadline) &&
               transfer(request, packet.midi, packet.midiBytes, true, deadline) &&
               transfer(response, &reply, 1, false, deadline) && reply == 'D' &&
               transfer(response, &packet, header, false, deadline) && packet.result == 0 &&
               packet.frames == frames &&
               transfer(response, packet.left, frames * sizeof(float), false, deadline) &&
               transfer(response, packet.right, frames * sizeof(float), false, deadline);
    }
    ~Engine() {
        if (child > 0) {
            const char quit = 'Q';
            if (request >= 0)
                (void)write(request, &quit, 1);
            if (!exited(40)) {
                kill(child, SIGTERM);
                if (!exited(30)) {
                    kill(child, SIGKILL);
                    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
                    }
                }
            }
        }
        if (response >= 0)
            close(response);
        if (request >= 0)
            close(request);
        std::error_code error;
        if (!folder.empty())
            std::filesystem::remove_all(folder, error);
        if (runtimeLock >= 0)
            close(runtimeLock);
    }
};
} // namespace scbridge::move
