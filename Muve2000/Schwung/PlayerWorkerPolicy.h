#pragma once

#if defined(__linux__) && defined(MU2000_PLAYER)
#include <cstdlib>
#include <pthread.h>
#include <string_view>

namespace mu_player {

class WorkerPolicy {
public:
    WorkerPolicy() {
        const char* value = std::getenv("MU2000_WORKER_POLICY");
        const std::string_view mode = value ? value : "priority";
        const bool pin = mode == "both" || mode == "affinity";
        bool realtime = mode == "both" || mode == "priority";
        if (!pin && !realtime) return;
        const bool haveCpus = pthread_getaffinity_np(pthread_self(), sizeof(originalCpus), &originalCpus) == 0;
        // The chip workers rendezvous each sample; do not elevate them on one CPU.
        if (!haveCpus || CPU_COUNT(&originalCpus) < 2) return;

        cpu_set_t renderCpus;
        CPU_ZERO(&renderCpus);
        // Keep one core free of these workers on the four-core Move.
        constexpr int moveRenderCores = 3;
        for (int core = 0; core < moveRenderCores; ++core)
            if (CPU_ISSET(core, &originalCpus)) CPU_SET(core, &renderCpus);
        if (pin && CPU_COUNT(&renderCpus) >= 2)
            affinityChanged = pthread_setaffinity_np(pthread_self(), sizeof(renderCpus), &renderCpus) == 0;

        if (!realtime || pthread_getschedparam(pthread_self(), &originalPolicy, &originalPriority) != 0) return;
        // Below Move's audio workers and Link, not the priority of the caller.
        constexpr int renderPriority = 20;
        const sched_param priority{renderPriority};
        priorityChanged = pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority) == 0;
    }

    ~WorkerPolicy() {
        if (priorityChanged) pthread_setschedparam(pthread_self(), originalPolicy, &originalPriority);
        if (affinityChanged) pthread_setaffinity_np(pthread_self(), sizeof(originalCpus), &originalCpus);
    }

    WorkerPolicy(const WorkerPolicy&) = delete;
    WorkerPolicy& operator=(const WorkerPolicy&) = delete;

private:
    cpu_set_t originalCpus{};
    sched_param originalPriority{};
    int originalPolicy = SCHED_OTHER;
    bool affinityChanged = false, priorityChanged = false;
};

}
#endif
