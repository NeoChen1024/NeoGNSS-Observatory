// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace neognss_obs {
// One owner for the large Arrow builders. The importer bounds outstanding
// batches and drains them before returning arrays or publishing a checkpoint.
class CnexBuildLane {
  public:
    explicit CnexBuildLane(bool enabled) {
        if (enabled)
            worker = std::thread([this] { run(); });
    }
    ~CnexBuildLane() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        if (worker.joinable())
            worker.join();
    }
    std::future<void> submit(std::packaged_task<void()> job) {
        auto future = job.get_future();
        if (!worker.joinable())
            job();
        else {
            {
                std::lock_guard lock(mutex);
                jobs.push_back(std::move(job));
            }
            ready.notify_one();
        }
        return future;
    }

  private:
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::packaged_task<void()>> jobs;
    bool stopping = false;
    std::thread worker;
    void run() {
        for (;;) {
            std::unique_lock lock(mutex);
            ready.wait(lock, [&] { return stopping || !jobs.empty(); });
            if (jobs.empty())
                return;
            auto job = std::move(jobs.front());
            jobs.pop_front();
            lock.unlock();
            job(); // packaged_task propagates exceptions to the feeding thread.
        }
    }
};
} // namespace neognss_obs
