#include "gamepad.h"
#include "inputs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <SDL3/SDL.h>

namespace {

constexpr float SCREEN_ASPECT = 496.0f / 384.0f;   // the sight moves as many pixels up as across
constexpr float STICK_DEAD = 0.15f, STICK_FULL = 0.92f;
constexpr float TRIGGER_DOWN = 0.35f;
constexpr float RUMBLE_STRENGTH[4] = {0.0f, 0.5f, 0.85f, 1.0f};   // OFF, LOW, MEDIUM, HIGH

// 1..10 around 5 = 1: each step about 19% faster (stick) or 32% (gyroscope)
float speed_step(int v, float per_doubling) { return std::exp2(float(v - 5) / per_doubling); }

// A stick with a round dead zone, its travel mapped to 0..1 on a curve that
// is gentle near the centre (fine aim) and steep at the edge (fast moves)
void stick(SDL_Gamepad* p, SDL_GamepadAxis ax, SDL_GamepadAxis ay, float& x, float& y, float& tilt) {
    float sx = float(SDL_GetGamepadAxis(p, ax)) / 32767.0f, sy = float(SDL_GetGamepadAxis(p, ay)) / 32767.0f;
    float m = std::sqrt(sx * sx + sy * sy);
    if (m < STICK_DEAD) return;
    float t = std::min((m - STICK_DEAD) / (STICK_FULL - STICK_DEAD), 1.0f);
    float c = 0.2f * t + 0.8f * t * t * t;
    x += sx / m * c;
    y += sy / m * c;
    tilt = std::max(tilt, t);
}

}  // namespace

bool Gamepads::init() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // The focus is the window's business (poll's `focused`); SDL has no window
    // of its own to judge it by
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // XInput has no way to the motors in an Xbox One / Series pad's triggers;
    // SDL's raw input driver reaches them through Windows.Gaming.Input (its
    // default correlation). Bluetooth pads go through SDL's HIDAPI driver,
    // which drives them on its own when their firmware allows
    if (trigger_rumble) SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("[gamepad] SDL could not initialize gamepads: %s\n", SDL_GetError());
        return false;
    }
    ok_ = true;
    last_poll_ = SDL_GetTicksNS();
    return true;   // the pads already plugged in arrive as SDL_EVENT_GAMEPAD_ADDED
}

void Gamepads::shutdown() {
    if (!ok_) return;
    for (Slot& s : slot_)
        if (s.pad) SDL_CloseGamepad(s.pad);
    SDL_Quit();
    ok_ = false;
}

int Gamepads::slot_of(uint32_t id) const {
    for (int i = 0; i < MAX; i++)
        if (slot_[i].pad && slot_[i].id == id) return i;
    return -1;
}

int Gamepads::connected() const {
    int n = 0;
    for (const PadState& p : state_) n += p.connected;
    return n;
}

void Gamepads::open(uint32_t id) {
    if (slot_of(id) >= 0) return;
    int i = 0;
    while (i < MAX && slot_[i].pad) i++;
    if (i == MAX) return;
    SDL_Gamepad* p = SDL_OpenGamepad(id);
    if (!p) return;
    Slot& s = slot_[i];
    s = Slot{};
    s.pad = p;
    s.id = id;
    s.nintendo = SDL_GetGamepadButtonLabel(p, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_B;
    PadState& st = state_[i];
    float x = st.aim_x, y = st.aim_y;
    st = PadState{};
    st.aim_x = x, st.aim_y = y;
    st.connected = true;
    const char* name = SDL_GetGamepadName(p);
    st.name = name ? name : "Gamepad";
    st.has_gyro = SDL_GamepadHasSensor(p, SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(p, SDL_SENSOR_GYRO, true);
    if (st.has_gyro && SDL_GamepadHasSensor(p, SDL_SENSOR_ACCEL)) SDL_SetGamepadSensorEnabled(p, SDL_SENSOR_ACCEL, true);
    s.trigger_motors = SDL_GetBooleanProperty(SDL_GetGamepadProperties(p), SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
    changed_ = true;
    printf("[gamepad] %d: %s%s%s\n", i + 1, st.name.c_str(), st.has_gyro ? " (with gyroscope)" : "",
           s.trigger_motors ? " (trigger rumble)" : "");
}

void Gamepads::close(uint32_t id) {
    int i = slot_of(id);
    if (i < 0) return;
    SDL_CloseGamepad(slot_[i].pad);
    slot_[i] = Slot{};
    state_[i].connected = false;
    state_[i].fire = state_[i].reload = false;
    changed_ = true;
    printf("[gamepad] %d disconnected\n", i + 1);
}

// Gravity, low-passed from the accelerometer, and how much the pad shakes
void Gamepads::accel_sample(Slot& s, const float* a) {
    float m = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (m < 1.0f) return;
    float d = 0;
    for (int k = 0; k < 3; k++) {
        float n = a[k] / m;
        d += std::fabs(n - s.gravity[k]);
        s.gravity[k] += (n - s.gravity[k]) * 0.05f;
    }
    s.shake += (d - s.shake) * 0.1f;
}

// One gyroscope reading (rad/s): calibration while the pad lies still, then
// sight motion. The turn left/right is taken around the up direction of the
// room ("player space"), so it works the same with the pad flat or upright
void Gamepads::gyro_sample(Slot& s, const float* w, uint64_t time_ns) {
    float dt = s.gyro_time ? float(double(time_ns - s.gyro_time) * 1e-9) : 0.0f;
    s.gyro_time = time_ns;
    if (dt <= 0 || dt > 0.05f) return;
    // Still: slow turning (after the bias) and a steady accelerometer. Half a
    // second of that and the average reading becomes the bias, refined as long
    // as the pad stays put; anything else starts the count again
    float off = 0;
    for (int k = 0; k < 3; k++) off = std::max(off, std::fabs(w[k] - s.bias[k]));
    bool still = off < (s.calibrated ? 0.04f : 0.15f) && s.shake < 0.01f;
    if (still) {
        s.still_time += dt;
        s.still_count++;
        for (int k = 0; k < 3; k++) s.still_sum[k] += w[k];
        if (s.still_time > 0.5f) {
            float keep = s.calibrated ? 0.8f : 0.0f;
            for (int k = 0; k < 3; k++) s.bias[k] = keep * s.bias[k] + (1 - keep) * s.still_sum[k] / float(s.still_count);
            s.calibrated = true;
            s.still_time = 0, s.still_count = 0;
            for (float& v : s.still_sum) v = 0;
        }
    } else {
        s.still_time = 0, s.still_count = 0;
        for (float& v : s.still_sum) v = 0;
    }
    float pitch = w[0] - s.bias[0], wy = w[1] - s.bias[1], wz = w[2] - s.bias[2];
    float yaw = wy * s.gravity[1] + wz * s.gravity[2];
    float most = std::sqrt(wy * wy + wz * wz);
    yaw = std::copysign(std::min(std::fabs(yaw) * 1.41f, most), yaw);
    // Very slow turns are scaled down: the hand's tremor does not shake the sight
    float m = std::sqrt(pitch * pitch + yaw * yaw), tight = 0.03f;
    if (m < tight) pitch *= m / tight, yaw *= m / tight;
    // A screen width per ~45 degrees at speed 5; counter-clockwise is positive
    float k = speed_step(set_.gyro_speed, 2.5f) / 0.8f * dt;
    s.gyro_dx -= yaw * k;
    s.gyro_dy -= pitch * k * SCREEN_ASPECT;
}

void Gamepads::poll(const GamepadSettings& set, bool focused, bool menu) {
    if (!ok_) return;
    set_ = set;
    uint64_t t = SDL_GetTicksNS();
    float dt = std::clamp(float(double(t - last_poll_) * 1e-9), 0.0f, 0.05f);
    last_poll_ = t;
    now_ = double(t) * 1e-9;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED: open(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: close(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
            int i = slot_of(e.gsensor.which);
            if (i < 0) break;
            if (e.gsensor.sensor == SDL_SENSOR_GYRO) gyro_sample(slot_[i], e.gsensor.data, e.gsensor.sensor_timestamp);
            else if (e.gsensor.sensor == SDL_SENSOR_ACCEL) accel_sample(slot_[i], e.gsensor.data);
            break;
        }
        default: break;
        }
    }
    for (int i = 0; i < MAX; i++) {
        Slot& s = slot_[i];
        PadState& st = state_[i];
        st.aimed = st.start = st.back = false;
        if (!s.pad) continue;
        SDL_Gamepad* p = s.pad;
        float gdx = s.gyro_dx, gdy = s.gyro_dy;
        s.gyro_dx = s.gyro_dy = 0;
        // Buttons and triggers, with what changed since the last poll
        bool now_down[32] = {};
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++) now_down[b] = SDL_GetGamepadButton(p, SDL_GamepadButton(b));
        bool lt = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > int(TRIGGER_DOWN * 32767);
        bool rt = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > int(TRIGGER_DOWN * 32767);
        auto down = [&](SDL_GamepadButton b) { return now_down[b]; };
        auto pressed = [&](SDL_GamepadButton b) { return now_down[b] && !s.buttons[b]; };
        bool lt_pressed = lt && !s.left_trigger, rt_pressed = rt && !s.right_trigger;
        float ax = 0, ay = 0, tilt = 0;
        stick(p, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, ax, ay, tilt);
        float lx = ax, ly = ay;   // the left stick alone, for the menus
        stick(p, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, ax, ay, tilt);
        bool any = lt || rt || tilt > 0 || std::fabs(gdx) + std::fabs(gdy) > 0.002f;
        for (int b = 0; b < 32; b++) any |= now_down[b];
        if (any && focused) st.last_used = now_;

        if (!focused) {
            st.fire = st.reload = false;
            s.full_tilt = 0;
        } else if (menu) {
            st.fire = st.reload = false;
            s.full_tilt = 0;
            SDL_GamepadButton accept = s.nintendo ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
            SDL_GamepadButton back = s.nintendo ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
            if (pressed(accept)) keys_.push_back(PadKey::Accept);
            if (pressed(back) || pressed(SDL_GAMEPAD_BUTTON_BACK)) keys_.push_back(PadKey::Back);
            if (pressed(SDL_GAMEPAD_BUTTON_START)) keys_.push_back(PadKey::Start);
            if (pressed(SDL_GAMEPAD_BUTTON_NORTH)) keys_.push_back(PadKey::Reset);
            if (pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || lt_pressed) keys_.push_back(PadKey::TabPrev);
            if (pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) || rt_pressed) keys_.push_back(PadKey::TabNext);
            // Direction: the d-pad or the left stick past half way, repeated while held
            int dir = down(SDL_GAMEPAD_BUTTON_DPAD_UP) ? 0 : down(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? 1
                    : down(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? 2 : down(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? 3 : -1;
            if (dir < 0 && std::max(std::fabs(lx), std::fabs(ly)) > 0.35f)
                dir = std::fabs(ly) >= std::fabs(lx) ? (ly < 0 ? 0 : 1) : (lx < 0 ? 2 : 3);
            if (dir != s.dir || (dir >= 0 && now_ >= s.next_repeat)) {
                if (dir >= 0) {
                    static const PadKey DIRS[4] = {PadKey::Up, PadKey::Down, PadKey::Left, PadKey::Right};
                    keys_.push_back(DIRS[dir]);
                    s.next_repeat = now_ + (dir != s.dir ? 0.4 : 0.11);
                }
                s.dir = dir;
            }
        } else {
            s.dir = -1;
            st.fire = rt || down(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) || down(SDL_GAMEPAD_BUTTON_SOUTH);
            st.reload = lt || down(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || down(SDL_GAMEPAD_BUTTON_WEST) ||
                        down(SDL_GAMEPAD_BUTTON_EAST);
            st.start = pressed(SDL_GAMEPAD_BUTTON_START);
            st.back = pressed(SDL_GAMEPAD_BUTTON_BACK);
            // Sticks: screen widths per second, up to 70% faster after a moment at full tilt
            float speed = 1.3f * speed_step(set.stick_speed, 4.0f);
            s.full_tilt = tilt >= 1.0f ? s.full_tilt + dt : 0.0f;
            speed *= 1.0f + 0.7f * std::clamp((s.full_tilt - 0.12f) / 0.3f, 0.0f, 1.0f);
            float dx = ax * speed * dt, dy = ay * speed * dt * SCREEN_ASPECT;
            bool gyro = set.gyro && st.has_gyro;
            if (!gyro) gdx = gdy = 0;
            // Not the gyroscope's drift and tremor while the pad lies on the desk
            bool moved = tilt > 0 || std::fabs(gdx) + std::fabs(gdy) > 0.0005f;
            if (pressed(SDL_GAMEPAD_BUTTON_LEFT_STICK) || pressed(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
                st.aim_x = st.aim_y = 0.5f;
                st.aimed = true;
            }
            float pull_x = 0, pull_y = 0;
            assist(s, st, set, dt, tilt, dx, dy, gdx, gdy, pull_x, pull_y);
            dx += gdx + pull_x, dy += gdy + pull_y;
            if (dx != 0 || dy != 0) {
                float reach = gun_reach();   // the whole 3D view, also the side bands of a wide one
                st.aim_x = std::clamp(st.aim_x + dx, -reach, 1 + reach);
                st.aim_y = std::clamp(st.aim_y + dy, 0.0f, 1.0f);
                if (moved) st.aimed = true;
            }
        }
        std::copy(std::begin(now_down), std::end(now_down), std::begin(s.buttons));
        s.left_trigger = lt, s.right_trigger = rt;
    }
}

// Aim assist on the target under the sight, or within a few pixels of it:
// the sticks (and less so the gyroscope) slow down over it, the sight
// follows it as it moves, and while a stick is moving it is drawn gently
// towards its middle. `dx`, `dy` (sticks) and `gx`, `gy` (gyroscope) are
// scaled in place; `px`, `py` is the pull to add
void Gamepads::assist(Slot& s, const PadState& st, const GamepadSettings& set, float dt, float tilt, float& dx,
                      float& dy, float& gx, float& gy, float& px, float& py) {
    static const float STRENGTH[4] = {0.0f, 0.5f, 0.8f, 1.0f};
    float k = STRENGTH[std::clamp(set.assist, 0, 3)];
    const float W = 496, H = 384, MARGIN = 14;   // pixels of the game screen
    const AimTarget* best = nullptr;
    float best_d = MARGIN;
    for (const AimTarget& t : targets_) {   // distance from the sight to the box (0 inside)
        float ox = std::max({t.x0 - st.aim_x, 0.0f, st.aim_x - t.x1}) * W;
        float oy = std::max({t.y0 - st.aim_y, 0.0f, st.aim_y - t.y1}) * H;
        float d = std::sqrt(ox * ox + oy * oy);
        if (d < best_d || (d == 0 && best_d == 0 && (t.x1 - t.x0) * (t.y1 - t.y0) < (best->x1 - best->x0) * (best->y1 - best->y0)))
            best = &t, best_d = d;   // the smallest of the boxes the sight is in: the nearer one in front
    }
    if (k <= 0 || !best) {
        s.locked = false;
        return;
    }
    float near = 1 - best_d / MARGIN;   // 1 on the target, 0 at the edge of the margin
    float cx = (best->x0 + best->x1) / 2, cy = (best->y0 + best->y1) / 2;
    float slow = 1 - 0.55f * k * near;
    dx *= slow, dy *= slow;
    gx *= 1 - 0.3f * k * near, gy *= 1 - 0.3f * k * near;
    // Following: the target's motion since the last poll, if it is the same one
    if (s.locked && std::fabs(cx - s.lock_x) * W < 25 && std::fabs(cy - s.lock_y) * H < 25) {
        px += (cx - s.lock_x) * 0.7f * k * near;
        py += (cy - s.lock_y) * 0.7f * k * near;
    }
    if (tilt > 0) {
        float f = std::min(1.0f, 4.0f * dt) * 0.35f * k * near * tilt;
        px += (cx - st.aim_x) * f;
        py += (cy - st.aim_y) * f;
    }
    s.locked = true;
    s.lock_x = cx, s.lock_y = cy;
}

void Gamepads::set_aim(int i, float x, float y) {
    float reach = gun_reach();
    state_[i].aim_x = std::clamp(x, -reach, 1 + reach);
    state_[i].aim_y = std::clamp(y, 0.0f, 1.0f);
}

std::vector<PadKey> Gamepads::take_keys() {
    std::vector<PadKey> k;
    k.swap(keys_);
    return k;
}

void Gamepads::rumble(int i, float low, float high, int ms) {
    if (!slot_[i].pad) return;
    float k = RUMBLE_STRENGTH[std::clamp(set_.rumble, 0, 3)] * 65535.0f;
    SDL_RumbleGamepad(slot_[i].pad, Uint16(std::clamp(low, 0.0f, 1.0f) * k), Uint16(std::clamp(high, 0.0f, 1.0f) * k),
                      Uint32(ms));
}

void Gamepads::rumble_triggers(int i, float left, float right, int ms) {
    Slot& s = slot_[i];
    if (!s.pad || set_.rumble <= 0 || set_.trigger_rumble <= 0) return;
    // (raw input knows a pad's trigger motors only once it has matched the
    // pad with Windows.Gaming.Input, after its first input: asked again then)
    if (!s.trigger_motors) {
        s.trigger_motors = SDL_GetBooleanProperty(SDL_GetGamepadProperties(s.pad), SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
        if (!s.trigger_motors) return;
        printf("[gamepad] %d: trigger rumble\n", i + 1);
    }
    float k = RUMBLE_STRENGTH[std::clamp(set_.rumble, 0, 3)] * std::clamp(set_.trigger_rumble, 0, 100) / 100.0f * 65535.0f;
    SDL_RumbleGamepadTriggers(s.pad, Uint16(std::clamp(left, 0.0f, 1.0f) * k), Uint16(std::clamp(right, 0.0f, 1.0f) * k),
                              Uint32(ms));
}
