// Worker (common/worker.h): a job per run() runs once, on another thread,
// and wait() returns after it; many in a row; waiting with none running
#include <atomic>
#include <cstdio>
#include <thread>

#include "worker.h"

static int g_failed = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAILED line %d: %s\n", __LINE__, #cond); \
            g_failed++;                                      \
        }                                                    \
    } while (0)

int main() {
    {
        Worker w;
        w.wait();   // nothing running: at once
        std::atomic<int> runs{0};
        std::thread::id worker_id;
        w.run([&] {
            worker_id = std::this_thread::get_id();
            runs++;
        });
        w.wait();
        CHECK(runs == 1);
        CHECK(worker_id != std::this_thread::get_id());
        w.wait();   // again: still at once, nothing runs twice
        CHECK(runs == 1);
        // Many jobs in a row, each seen complete by wait(); the sum needs no atomics
        long sum = 0;
        for (int i = 1; i <= 2000; i++) {
            w.run([&, i] { sum += i; });
            w.wait();
        }
        CHECK(sum == 2000L * 2001 / 2);
        // The job runs beside the caller's own work
        std::atomic<bool> seen{false};
        w.run([&] { seen = true; });
        for (int i = 0; i < 1000000 && !seen; i++) std::this_thread::yield();
        CHECK(seen);
        w.wait();
    }   // the destructor ends the thread with no job pending
    {
        Worker w;   // destroyed right after a job: it is waited for
        std::atomic<int> runs{0};
        w.run([&] { runs++; });
        w.wait();
        CHECK(runs == 1);
    }
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("worker: ok\n");
    return 0;
}
