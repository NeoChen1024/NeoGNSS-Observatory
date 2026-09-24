// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <neognss_obs/measurements.hpp>
#include <neognss_obs/raw_bits.hpp>
#include <stdexcept>
#include <thread>

namespace neognss_obs {
struct CnexDecodedFrame {
    cppgnss::FrameView frame;
    std::optional<neognss_obs::Measurements> measurements, extras;
    std::optional<cppgnss::RTCM3::ObservationMessage> rtcm;
    std::optional<cppgnss::RTCM3::MsmHeader> rtcm_header;
    uint64_t invalid_frames_before = 0;
    neognss_obs::RawBitsResult bits;
    std::exception_ptr error;

    void decode() noexcept {
        try {
            if (frame.protocol == cppgnss::Protocol::rtcm3) {
                if (!cppgnss::RTCM3::is_observation_message(frame.id))
                    return;
                auto result = cppgnss::RTCM3::parse_observation(frame);
                if (result) {
                    rtcm = std::move(result).value();
                    rtcm_header = *rtcm;
                } else {
                    if (frame.id >= 1071 && frame.id <= 1137) {
                        auto header = cppgnss::RTCM3::parse_msm_header(frame);
                        if (!header)
                            throw std::runtime_error(header.error().detail);
                        rtcm_header = std::move(header).value();
                    }
                    if (result.error().code !=
                        cppgnss::ParseErrorCode::UNSUPPORTED_LAYOUT)
                        throw std::runtime_error(result.error().detail);
                }
                return;
            }
            measurements = neognss_obs::decode_measurements(frame);
            bits = neognss_obs::decode_raw_bits(frame);
            extras = neognss_obs::decode_measurement_extras(frame);
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
