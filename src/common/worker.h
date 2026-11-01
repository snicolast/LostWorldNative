// A thread that runs one job at a time on request, for work done beside
// another thread's every frame (the 2D composition beside the scene build)
// without making and ending a thread each time, as std::async does.
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    ~Worker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Starts a job; the previous one must have been waited for. What the job
    // refers to must live until wait() returns
    void run(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = std::move(job);
            busy_ = true;
        }
        cv_.notify_all();
    }
    // Returns once the job started last is over (at once if none is running)
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return !busy_; });
    }

private:
    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            cv_.wait(lock, [&] { return stop_ || busy_; });
            if (stop_) return;
            std::function<void()> job = std::move(job_);
            lock.unlock();
            job();
            lock.lock();
            busy_ = false;
            cv_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::function<void()> job_;
    bool busy_ = false, stop_ = false;
};
