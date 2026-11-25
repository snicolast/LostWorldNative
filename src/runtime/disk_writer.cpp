// Background file writes (disk_writer.h).
#include "disk_writer.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

struct Job {
    std::string path, data;
};

struct Writer {
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::deque<Job> jobs;
    bool busy = false, started = false;
    std::set<std::string> failed;   // logged once each
};

// Never destroyed: the thread may still be running while the process exits
Writer& writer() {
    static Writer* w = new Writer;
    return *w;
}

bool replace_now(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp.c_str(), path.c_str()) == 0;
#endif
    if (!ok) remove(tmp.c_str());
    return ok;
}

void run() {
    Writer& w = writer();
    std::unique_lock<std::mutex> lock(w.mutex);
    for (;;) {
        w.wake.wait(lock, [&] { return !w.jobs.empty(); });
        Job job = std::move(w.jobs.front());
        w.jobs.pop_front();
        w.busy = true;
        lock.unlock();
        bool ok = replace_now(job.path, job.data);
        lock.lock();
        w.busy = false;
        if (!ok && w.failed.insert(job.path).second) printf("[disk] could not write %s\n", job.path.c_str());
        if (w.jobs.empty()) w.idle.notify_all();
    }
}

void push(const std::string& path, std::string data) {
    Writer& w = writer();
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        if (!w.started) {
            std::thread(run).detach();
            w.started = true;
        }
        // A job still waiting for this file takes the newer content
        Job* last = nullptr;
        for (auto it = w.jobs.rbegin(); it != w.jobs.rend() && !last; ++it)
            if (it->path == path) last = &*it;
        if (last)
            last->data = std::move(data);
        else
            w.jobs.push_back({path, std::move(data)});
    }
    w.wake.notify_one();
}

}  // namespace

void disk_replace(const std::string& path, std::string data) { push(path, std::move(data)); }

void disk_flush() {
    Writer& w = writer();
    std::unique_lock<std::mutex> lock(w.mutex);
    w.idle.wait_for(lock, std::chrono::seconds(5), [&] { return w.jobs.empty() && !w.busy; });
}
