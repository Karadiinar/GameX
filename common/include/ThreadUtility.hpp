#pragma once
#include <vector>
#include <thread>
#include <set>
#include <queue>
#include <mutex>
#include <optional>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
    #include <sched.h>
    #include <fstream>
    #include <string>
#endif

namespace Rebel::Concurrent {

    template<typename T>
    class ThreadSafeQueue {
    public:
        void push(T item) {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }

        std::optional<T> try_pop() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                return std::nullopt;
            }
            T item = std::move(queue_.front());
            queue_.pop();
            return item;
        }

        bool empty() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.empty();
        }

    private:
        std::queue<T> queue_;
        mutable std::mutex mutex_;
    };

}

namespace Rebel::ThreadUtils {

    inline std::vector<uint32_t> GetPhysicalCoreMap() {
        std::vector<uint32_t> coreIds;
#ifdef _WIN32
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (length == 0) return coreIds;

        std::vector<uint8_t> buffer(length);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data(), &length)) {
            auto* ptr = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data();
            for (DWORD i = 0; i < length; ) {
                for (int bit = 0; bit < 64; ++bit) {
                    if ((ptr->Processor.GroupMask[0].Mask >> bit) & 1) {
                        coreIds.push_back(bit);
                        break; 
                    }
                }
                i += ptr->Size;
                ptr = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((uint8_t*)ptr + ptr->Size);
            }
        }
#else
        std::set<int> seen;
        for (uint32_t i = 0; i < std::thread::hardware_concurrency(); ++i) {
            std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/core_id";
            std::ifstream f(path);
            int physId;
            if (f >> physId && seen.find(physId) == seen.end()) {
                coreIds.push_back(i);
                seen.insert(physId);
            }
        }
#endif
        return coreIds;
    }

    inline void SetThreadAffinity(std::thread& t, uint32_t logicalId) {
#ifdef _WIN32
        SetThreadAffinityMask((HANDLE)t.native_handle(), (DWORD_PTR)1 << logicalId);
#else
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(logicalId, &cpuset);
        pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
    }
}