// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Kelei Chen
#pragma once

#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

// Single-owner file I/O; the producer never waits for disk capacity.
class LoggerOutput {
    struct Job {
        std::filesystem::path path;
        std::vector<uint8_t> bytes;
    };
    const size_t limit;
    const bool quiet;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Job> queue;
    size_t buffered = 0;
    bool stopping = false, error_reported = false;
    std::exception_ptr error;
    std::thread worker;

    static void io_error(const std::string &what) {
        throw std::runtime_error(what + ": " + std::strerror(errno));
    }
    void run() noexcept {
        FILE *file = nullptr;
        std::filesystem::path current;
        auto close = [&] {
            auto old = file;
            file = nullptr;
            if (old && std::fclose(old) == EOF)
                io_error("Output close " + current.string());
        };
        try {
            for (;;) {
                Job job;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock,
                               [&] { return stopping || !queue.empty(); });
                    if (queue.empty())
                        break;
                    job = std::move(queue.front());
                    queue.pop_front();
                }
                if (job.path != current) {
                    close();
                    std::filesystem::create_directories(job.path.parent_path());
                    file = std::fopen(job.path.c_str(), "wbx");
                    if (!file)
                        io_error("Output open " + job.path.string());
                    current = job.path;
                    if (!quiet)
                        std::fprintf(stderr, "\nOpened file %s\n",
                                     current.c_str());
                }
                if (std::fwrite(job.bytes.data(), 1, job.bytes.size(), file) !=
                    job.bytes.size())
                    io_error("Output write " + current.string());
                auto capacity = job.bytes.capacity();
                std::vector<uint8_t>().swap(job.bytes);
                std::lock_guard lock(mutex);
                buffered -= capacity;
            }
            close();
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                error = std::current_exception();
            }
            if (file)
                std::fclose(file);
        }
    }
    void stop() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        if (worker.joinable())
            worker.join();
    }

  public:
    LoggerOutput(size_t capacity, bool silent)
        : limit(capacity), quiet(silent), worker([this] { run(); }) {}
    LoggerOutput(const LoggerOutput &) = delete;
    LoggerOutput &operator=(const LoggerOutput &) = delete;
    ~LoggerOutput() {
        stop();
        if (error && !error_reported) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception &e) {
                std::fprintf(stderr, "Logger output error: %s\n", e.what());
            }
        }
    }
    void check() {
        std::lock_guard lock(mutex);
        if (error) {
            error_reported = true;
            std::rethrow_exception(error);
        }
    }
    void submit(const std::filesystem::path &path,
                std::vector<uint8_t> &&bytes) {
        std::lock_guard lock(mutex);
        if (error) {
            error_reported = true;
            std::rethrow_exception(error);
        }
        if (bytes.capacity() > limit - buffered)
            throw std::runtime_error(
                "Disk buffer full: output cannot keep up; stopping recording");
        const auto capacity = bytes.capacity();
        queue.push_back({path, std::move(bytes)});
        buffered += capacity;
        ready.notify_one();
    }
    void finish() {
        stop();
        check();
    }
};
