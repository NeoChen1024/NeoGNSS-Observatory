// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cppgnss/measurements.hpp>
#include <cppgnss/raw_bits.hpp>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace neognss_obs {
struct CnexDecodedFrame {
    cppgnss::FrameView frame;
    std::optional<cppgnss::Measurements> measurements, extras;
    cppgnss::RawBitsResult bits;
    std::exception_ptr error;

    void decode() noexcept {
        try {
            measurements = cppgnss::decode_measurements(frame);
            bits = cppgnss::decode_raw_bits(frame);
            extras = cppgnss::decode_measurement_extras(frame);
        } catch (...) {
            error = std::current_exception();
        }
    }
};

// Only stateless protocol work runs here. The caller retains byte ownership
// until run() returns and consumes results/errors in original stream order.
class CnexDecodePool {
  public:
    explicit CnexDecodePool(unsigned count) {
        if (count < 1 || count > 32)
            throw std::invalid_argument("Expected 1..32 decode workers");
        try {
            for (unsigned i = 1; i < count; ++i)
                workers.emplace_back([this] { worker(); });
        } catch (...) {
            stop();
            throw;
        }
    }
    ~CnexDecodePool() { stop(); }
    CnexDecodePool(const CnexDecodePool &) = delete;
    CnexDecodePool &operator=(const CnexDecodePool &) = delete;

    void run(std::vector<CnexDecodedFrame> &batch) {
        if (workers.empty() || batch.size() < 128) {
            for (auto &item : batch)
                item.decode();
            return;
        }
        {
            std::lock_guard lock(mutex);
            frames = &batch;
            next.store(0, std::memory_order_relaxed);
            finished = 0;
            ++generation;
        }
        ready.notify_all();
        work(); // The feeding thread is also a worker.
        std::unique_lock lock(mutex);
        done.wait(lock, [&] { return finished == workers.size(); });
        frames = nullptr;
    }

  private:
    std::mutex mutex;
    std::condition_variable ready, done;
    std::vector<std::thread> workers;
    std::vector<CnexDecodedFrame> *frames = nullptr;
    std::atomic<size_t> next{0};
    size_t generation = 0, finished = 0;
    bool stopping = false;

    void work() {
        for (;;) {
            auto begin = next.fetch_add(32, std::memory_order_relaxed);
            if (begin >= frames->size())
                return;
            auto end = std::min(begin + 32, frames->size());
            for (auto i = begin; i < end; ++i)
                (*frames)[i].decode();
        }
    }
    void worker() {
        size_t seen = 0;
        for (;;) {
            std::unique_lock lock(mutex);
            ready.wait(lock, [&] { return stopping || generation != seen; });
            if (stopping)
                return;
            seen = generation;
            lock.unlock();
            work();
            lock.lock();
            ++finished;
            done.notify_one();
        }
    }
    void stop() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_all();
        for (auto &worker : workers)
            worker.join();
    }
};
} // namespace neognss_obs
