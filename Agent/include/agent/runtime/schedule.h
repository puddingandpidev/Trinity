// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <cstdint>
#include <mutex>
#include <random>

namespace agent::runtime {

    /// <summary>
    /// How long the agent waits between check-ins, and how much it varies that wait.
    /// </summary>
    /// <remarks>
    /// The interval used to be a hardcoded 5000 ms, so `set/sleep` could only ever be answered with
    /// "Unsupported PROCESS task code 1". The value is process state, set by the command and read
    /// by the agent loop, so the two cannot disagree about which interval is in force.
    /// </remarks>
    class Schedule {
      public:
        static Schedule& Instance() {
            static Schedule instance;
            return instance;
        }

        void Set(int sleep_ms, int jitter_percent) {
            std::lock_guard<std::mutex> lock(mutex_);
            // A nonsense interval would strand the implant: clamp to [1s, 24h].
            sleep_ms_ = sleep_ms < 1000 ? 1000 : (sleep_ms > 86'400'000 ? 86'400'000 : sleep_ms);
            jitter_ = jitter_percent < 0 ? 0 : (jitter_percent > 90 ? 90 : jitter_percent);
        }

        [[nodiscard]] std::int64_t SleepMs() const { return sleep_ms_; }

        [[nodiscard]] int JitterPercent() const { return jitter_; }

        /// The wait for this iteration: the interval varied by up to ±jitter%.
        [[nodiscard]] std::int64_t NextDelayMs() {
            const std::int64_t base = sleep_ms_;
            const int jitter = jitter_;
            if (jitter == 0) {
                return base;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            std::uniform_int_distribution<int> spread(-jitter, jitter);
            const std::int64_t factor = 100 + spread(rng_);
            const std::int64_t delay = base * factor / 100;
            return delay < 1000 ? 1000 : delay;
        }

      private:
        Schedule() : rng_(std::random_device{}()) {}

        mutable std::mutex mutex_;
        std::int64_t sleep_ms_ = 5000;
        int jitter_ = 0;
        std::mt19937 rng_;
    };

}  // namespace agent::runtime
// ========================================== { END OF FILE } ========================================== //
