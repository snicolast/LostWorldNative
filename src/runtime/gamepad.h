// Gamepads through SDL3's gamepad subsystem (no SDL window): Xbox,
// DualSense / DualShock 4, Switch Pro, Joy-Con and the rest SDL knows.
// The window thread polls them. In the game each pad aims like a light gun
// (the sticks move its sight, with a response curve and a speed-up at full
// tilt; the gyroscope moves it too on pads that have one; an aim assist
// slows it over the dinosaurs and follows them), fires, reloads
// and vibrates; in the menus it sends PadKeys. app.cpp decides which player
// a pad controls.
//
// Buttons (positions, so the same on every pad):
//   fire    R2, R1, bottom face button      reload  L2, L1, left / right face buttons
//   START   join the game (credit + start) or pause during one
//   BACK    pause (Share, View, -)           L3 / R3 sight to the centre
//   menus   d-pad or left stick, A to accept and B to go back (on Nintendo
//           pads the A and B they print), L1 / R1 change the settings tab,
//           the top face button resets the tab to its defaults
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct SDL_Gamepad;

struct GamepadSettings {
    int stick_speed = 5;   // 1..10: sight speed with the sticks
    bool gyro = true;      // the gyroscope moves the sight
    int gyro_speed = 5;    // 1..10
    int rumble = 2;        // vibration: 0 off, 1 low, 2 medium, 3 high
    int trigger_rumble = 100;   // the triggers' own motors (Xbox One / Series pads), % of the vibration's strength
    int assist = 2;        // aim assist: 0 off, 1 low, 2 medium, 3 high
};

// Something to shoot at, as a box in fractions of the original 4:3 screen
struct AimTarget {
    float x0, y0, x1, y1;
};

// What the pads send to the menus (any pad)
enum class PadKey { Up, Down, Left, Right, Accept, Back, TabPrev, TabNext, Reset, Start };

struct PadState {
    bool connected = false, has_gyro = false;
    std::string name;
    float aim_x = 0.5f, aim_y = 0.5f;   // sight, as a fraction of the original 4:3 screen
    bool aimed = false;                 // the sticks or the gyroscope moved it on purpose in the last poll
    bool fire = false, reload = false;  // held
    bool start = false, back = false;   // pressed since the last poll
    double last_used = -1e9;            // seconds (poll clock) of its last input
};

class Gamepads {
public:
    static constexpr int MAX = 4;

    bool init();   // false: no gamepads this run (the game works without them)
    void shutdown();
    // Trigger rumble asks SDL to open Xbox pads through raw input (with
    // Windows.Gaming.Input, which drives the trigger motors) instead of
    // XInput, which cannot: init() reads it (--no-trigger-rumble: off)
    bool trigger_rumble = true;
    // Reads the pads. `focused`: the window is in front (otherwise the pads
    // are idle); `menu`: a menu has the controls, so the d-pad, the left stick
    // and the buttons become PadKeys and the sight stays where it is
    void poll(const GamepadSettings& s, bool focused, bool menu);
    double now() const { return now_; }
    const PadState& pad(int i) const { return state_[i]; }
    bool changed() { bool c = changed_; changed_ = false; return c; }   // a pad came or went
    // Moves a pad's sight (it follows the mouse while the mouse aims)
    void set_aim(int i, float x, float y);
    std::vector<PadKey> take_keys();
    // What is on screen to shoot at, for the aim assist (each new game frame)
    void set_targets(std::vector<AimTarget> targets) { targets_ = std::move(targets); }
    // Vibration, 0..1 per motor, scaled by the settings' strength; the
    // triggers' own motors where the pad has them (Xbox One and later; also
    // scaled by TRIGGER RUMBLE, nothing on pads without them)
    void rumble(int i, float low, float high, int ms);
    void rumble_triggers(int i, float left, float right, int ms);

private:
    struct Slot {
        SDL_Gamepad* pad = nullptr;
        uint32_t id = 0;
        bool nintendo = false;   // A on the right: accept and back swap
        bool trigger_motors = false;   // SDL can drive the motors in its triggers
        bool buttons[32] = {}, left_trigger = false, right_trigger = false;
        float full_tilt = 0;     // seconds with a stick held all the way
        // Gyroscope: sight motion gathered since the last poll, the bias
        // learnt while the pad lies still, and gravity (which way is up)
        float gyro_dx = 0, gyro_dy = 0;
        uint64_t gyro_time = 0;
        float bias[3] = {}, still_sum[3] = {};
        float still_time = 0;
        int still_count = 0;
        bool calibrated = false;
        float gravity[3] = {0, 1, 0}, shake = 0;
        // Aim assist: the middle of the target it was on at the last poll
        bool locked = false;
        float lock_x = 0, lock_y = 0;
        // Menu direction held, for the key repeat
        int dir = -1;
        double next_repeat = 0;
    };
    void open(uint32_t id);
    void close(uint32_t id);
    int slot_of(uint32_t id) const;
    void gyro_sample(Slot& s, const float* w, uint64_t time_ns);
    void accel_sample(Slot& s, const float* a);
    void assist(Slot& s, const PadState& st, const GamepadSettings& set, float dt, float tilt, float& dx, float& dy,
                float& gx, float& gy, float& px, float& py);

    bool ok_ = false, changed_ = false;
    Slot slot_[MAX];
    PadState state_[MAX];
    std::vector<PadKey> keys_;
    std::vector<AimTarget> targets_;
    GamepadSettings set_;
    double now_ = 0;
    uint64_t last_poll_ = 0;
};
