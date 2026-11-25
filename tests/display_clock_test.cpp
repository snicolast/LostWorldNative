// The display clock and the frame pacer (src/runtime/display_clock.h): when
// each image will be on screen, from uneven wake-ups, and how far behind the
// game the picture runs. Driven with made-up times, no window.
#include <cmath>
#include <cstdio>

#include "display_clock.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// A small repeatable jitter in [-amp, amp]
static double jitter(int i, double amp) { return amp * (double((i * 7919) % 101) / 50.0 - 1.0); }

int main() {
    const double P60 = 1.0 / 60, P144 = 1.0 / 144;

    // VSYNC on, DWM reports the vblanks: wake-ups up to 0.5 ms off (and
    // 2-6 ms after the vblank), the moments one refresh apart, each on a vblank
    {
        DisplayClock c;
        c.quiet = true;
        DisplaySample s{true, P60, 10.0};
        double prev = 0;
        bool even = true, on_vblank = true;
        for (int i = 0; i < 300; i++) {
            double now = 10.0 + i * P60 + 0.004 + jitter(i, 0.0005);
            double at = c.step(now, s);
            if (i > 1) even &= near(at - prev, P60, 1e-9);
            double k = (at - 10.0) / P60;
            on_vblank &= near(k, std::round(k), 1e-6);
            on_vblank &= at >= now - 1e-9;   // never before the wake-up
            prev = at;
        }
        CHECK(c.mode == DisplayClock::Vblanks);
        CHECK(even && on_vblank);
        CHECK(near(c.period, P60));
        // A wake-up early (even before the last image's moment) still moves
        // on by one refresh
        double before = c.at;
        double early = c.step(before - 0.001, s);
        CHECK(near(early - before, P60, 1e-9));
        // A stall (a window drag): it starts again on the next vblank
        double now = 10.0 + 400 * P60 + 0.003;
        double at = c.step(now, s);
        CHECK(at >= now && at - now <= P60 + 1e-9);
    }

    // DWM's vblanks are another monitor's (60 Hz) while the window's runs
    // at 144: one image per refresh, timed from the wake-ups at 144 Hz
    {
        DisplayClock c;
        c.quiet = true;
        c.monitor_hz = 144;
        DisplaySample s{true, P60, 0.0};
        double prev = 0, worst = 0;
        for (int i = 0; i < 2000; i++) {
            double now = 5.0 + i * P144 + jitter(i, 0.0005);
            double at = c.step(now, s);
            if (i > 1000) worst = std::fmax(worst, std::fabs((at - prev) - P144));
            prev = at;
        }
        CHECK(c.mode == DisplayClock::Refreshes);
        CHECK(std::fabs(c.period - P144) < 0.0002);
        CHECK(worst < 0.0005);   // evened out: steps close to one refresh
    }

    // VSYNC off at 90 images a second on a 60 Hz monitor: each image when
    // it is ready, moments evened out at the wake-ups' interval
    {
        DisplayClock c;
        c.quiet = true;
        DisplaySample s{true, P60, 0.0};
        double prev = 0, worst = 0, behind = 0;
        for (int i = 0; i < 1000; i++) {
            double now = 1.0 + i / 90.0 + jitter(i, 0.001);
            double at = c.step(now, s);
            if (i > 500) {
                worst = std::fmax(worst, std::fabs((at - prev) - 1 / 90.0));
                behind = std::fmax(behind, std::fabs(at - now));
            }
            prev = at;
        }
        CHECK(c.mode == DisplayClock::Presents);
        CHECK(near(c.interval, 1 / 90.0, 0.0005));
        CHECK(worst < 0.002 && behind < 0.01);
        // 3% off the refresh rate, coming from fewer images (40 a second):
        // not yet one image per refresh (2% to come back)...
        for (int i = 0; i < 1500; i++) c.step(20.0 + i / 40.0, s);
        for (int i = 0; i < 1500; i++) c.step(80.0 + i * P60 * 1.03, s);
        CHECK(c.mode == DisplayClock::Presents);
        // ...at the refresh rate: vblanks again, and 3% off now and then keeps them (4% to leave)
        for (int i = 0; i < 1500; i++) c.step(110.0 + i * P60, s);
        CHECK(c.mode == DisplayClock::Vblanks);
        for (int i = 0; i < 1500; i++) c.step(140.0 + i * P60 * 1.03, s);
        CHECK(c.mode == DisplayClock::Vblanks);
    }

    // The pacer: a later frame raises the lateness at once, with 1.5 ms to spare
    {
        FramePacer p;
        double t = 100;
        p.frame(0.006, t);
        CHECK(near(p.lateness, 0.0075));
        CHECK(near(p.shown_time(t), t - 0.0075 - p.next_period - 0.0005));
        // A one-off spike (much later than the rest) does not raise it, nor
        // the frames right after it (the same hitch)...
        p.frame(0.030, t += 1);
        CHECK(near(p.lateness, 0.0075));
        p.frame(0.030, t += 0.1);
        CHECK(near(p.lateness, 0.0075));
        // ...a second one 0.25-3 s after the first does
        p.frame(0.030, t += 0.9);
        CHECK(near(p.lateness, 0.0315));
        // Frames earlier than that: no change for 5 s, then it eases down
        // towards them, never below
        p.frame(0.005, t += 4);
        CHECK(near(p.lateness, 0.0315));
        t += 2;   // (5 s since it rose: from here on, every frame)
        for (int i = 0; i < 200; i++) p.frame(0.005, t += 1.0 / 57.5);
        CHECK(near(p.lateness, 0.005 + 0.0265 * std::pow(0.99, 200), 0.0005));   // by 1% of the excess a frame
        for (int i = 0; i < 2000; i++) p.frame(0.005, t += 1.0 / 57.5);
        CHECK(near(p.lateness, 0.005, 1e-9));
        // Never above 50 ms, however late
        p.frame(0.5, t += 10);
        p.frame(0.5, t += 1);
        CHECK(p.lateness <= 0.0515 + 1e-12);
    }

    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    printf("display_clock: all checks passed\n");
    return 0;
}
