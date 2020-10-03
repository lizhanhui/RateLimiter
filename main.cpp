#include <iostream>
#include <array>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

template<int PARTITION>
class RateLimiter {
public:
    explicit RateLimiter(int permit) : interval_(1000 / PARTITION), stopped_(false) {
        int avg = permit / PARTITION;
        for (auto &i : partition_) {
            i = avg;
        }

        int r = permit % PARTITION;

        if (r) {
            int step = PARTITION / r;
            for (int i = 0; i < r; ++i) {
                partition_[i * step]++;
            }
        }

        permits_ = partition_;

        auto lambda_tick = [this]() {
            while (!stopped_) {
                tick();
            }
        };

        thread_tick_ = std::thread(lambda_tick);
    }

    ~RateLimiter() {
        stopped_.store(true, std::memory_order_relaxed);
        if (thread_tick_.joinable()) {
            thread_tick_.join();
        }
    }

    std::array<int, PARTITION> &partition() {
        return permits_;
    }

    int slot() {
        auto current = std::chrono::steady_clock::now();
        long ms = std::chrono::duration_cast<std::chrono::milliseconds>(current.time_since_epoch()).count();
        return ms / interval_ % PARTITION;
    }

    void acquire() {
        int idx = slot();
        {
            std::unique_lock<std::mutex> lk(mtx_);
            if (permits_[idx] > 0) {
                --permits_[idx];
                return;
            }

            cv_.wait(lk, [this]() {
                int idx = slot();
                return permits_[idx] > 0;
            });
            idx = slot();
            --permits_[idx];
        }
    }

    void tick() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / PARTITION));
        int idx = slot();
        {
            std::unique_lock<std::mutex> lk(mtx_);
            permits_[idx] = partition_[idx];
            cv_.notify_all();
        }
    }

private:
    std::array<int, PARTITION> partition_;
    std::array<int, PARTITION> permits_;
    int interval_;
    std::mutex mtx_;
    std::condition_variable cv_;

    std::atomic_bool stopped_;
    std::thread thread_tick_;
};

template<int PARTITION>
std::ostream &operator<<(std::ostream &out, RateLimiter<PARTITION> &rate_limiter) {
    out << "[";
    for (const auto &e : rate_limiter.partition()) {
        out << e << ", ";
    }
    out << "]";
    return out;
}

int getAndSet(std::atomic_int &counter, int val) {
    int value;
    while (true) {
        value = counter.load(std::memory_order_relaxed);
        if (counter.compare_exchange_weak(value, val, std::memory_order_relaxed)) {
            return value;
        }
    }
}

int getAndReset(std::atomic_int &counter) {
    return getAndSet(counter, 0);
}

int main() {
    std::cout << "Hello, World!" << std::endl;

    RateLimiter<5> rate_limiter(500);

    std::cout << rate_limiter << std::endl;

    std::atomic_bool stopped(false);
    std::atomic_int counter(0);


    std::thread thread_stopper([&stopped] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        stopped.store(true, std::memory_order_relaxed);
    });

    std::thread thread_stats([&stopped, &counter] {
        while (!stopped) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::cout << "QPS:" << getAndReset(counter) << std::endl;
        }
    });

    while (!stopped) {
        rate_limiter.acquire();
        counter.fetch_add(1, std::memory_order_relaxed);
    }

    if (thread_stopper.joinable()) {
        thread_stopper.join();
    }

    if (thread_stats.joinable()) {
        thread_stats.join();
    }

    return 0;
}
