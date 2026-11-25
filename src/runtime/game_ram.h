// Game RAM: the addresses of The Lost World's own variables that the port
// reads or writes (PowerPC addresses, big-endian values). One list for every
// file, in the order of docs/game_ram.md, which says how each was found and
// what its values mean. A new address goes here (and in the doc), never as a
// constant of its own in a .cpp.
//
// The game's functions (FN_* addresses) stay with the code that hooks them,
// and the encounter script's own layout is in encounter_script.h.
#pragma once
#include <cstdint>

namespace ram {

// ---- Game and player ----
constexpr uint32_t STAGE = 0x1A623C;           // byte: current stage, from 0 (0 = stage 1)
constexpr uint32_t PLAYLIST = 0x1C2AFC;        // 5 ints: the run's stages, in order
constexpr uint32_t PLAYLIST_INDEX = 0x1C2B10;  // int: position in that list (-1 before the first stage)
constexpr uint32_t COURSE = 0x1C2AE0;          // int: chosen course (0 Beginner, 1 Expert, 2 Shuffle)
// int: 0 before the first game (and the attract), 1-2 choosing a course, 9-0xB
// the stage intro, 0xC playing (it stays 0xC in the attract after a game)
constexpr uint32_t COURSE_SELECT = 0x1C2AD8;
// int: 0 logo screens, 1 title, demo and rankings, 0xA around a coin or START,
// 0xB from the course selection to the end of a game, 0x14 / 0x15 a stage cleared
constexpr uint32_t GAME_MODE = 0x1A6164;
constexpr uint32_t LANGUAGE = 0x1A6168;        // int: the game's text language, set once at start (from SETTING_COUNTRY)
constexpr uint32_t ZONE = 0x1A3984;            // low nibble: current zone of the stage
constexpr uint32_t CAMERA_POS = 0x1A6F28;      // 3 floats: camera position (x, y, z)
constexpr uint32_t LOOK_AT = 0x1A3454;         // 3 floats: point the camera turns to (encounter op -9)
constexpr uint32_t FOCAL_LENGTH = 0x1A33FC;    // float: the projection's focal length in pixels
// Players: 0x5C-byte objects from 0x1A36C8; their lives at +0x58
constexpr uint32_t P1_LIVES = 0x1A3720;        // int: player 1 lives
constexpr uint32_t P1_LIVES_BYTE = P1_LIVES + 3;   // its low byte (0: down)
constexpr uint32_t P2_LIVES = 0x1A377C;        // int: player 2 lives
constexpr uint32_t P2_HUD_LIVES = 0x1C1264;    // int: player 2 medkits drawn on the HUD (player 1's at 0x1C1260)
// Player slots, 0x10 bytes each: byte +0xD is 1 / 2 playing, 0 on the
// CONTINUE screen, 3 out (also during the attract demo)
constexpr uint32_t PLAYER_SLOTS = 0x1A3784;
constexpr uint32_t P1_SLOT = PLAYER_SLOTS + 0xD;          // 0x1A3791
constexpr uint32_t P2_SLOT = PLAYER_SLOTS + 0x10 + 0xD;   // 0x1A37A1
constexpr uint32_t P2_CONTINUE = 0x1C1234;     // int: player 2's CONTINUE countdown (0x4B0 at the start)
// int: players in the game, bit 0x01000000 player 1, 0x02000000 player 2
// (the hit function picks its victim among them, op -14 checks both)
constexpr uint32_t PLAYERS_IN = 0x7F4;
constexpr uint32_t PIECES = 0x1A37A4;          // int × 2: powerbar pieces, per player (+4 player 2)
constexpr uint32_t P1_SCORE = 0x1A857C;        // int: player 1 score (0x80-byte block per player)
constexpr uint32_t P1_STAGE_SCORES = 0x1A858C; // 5 ints: player 1 score per stage (+4 × stage)
constexpr uint32_t P1_SCORE_SHOWN = 0x1C1D54;  // int: the score player 1's counter shows (-1 redraws it)
// The HUD's text layer: a pointer to it, cell (x, y) at + y * 256 + x * 2 (0 blank)
constexpr uint32_t TEXT_LAYER = 0x1A6FEC;
constexpr uint32_t TEXT_TABLE = 0x134B28;      // 16 bytes per text: per language, image and width

// ---- Results screen counters (half words, a 0x10-byte block per player) ----
constexpr uint32_t COUNTERS = 0x1A6208;        // player 1: +4 hits, +6 technical, +0xA type 8, +0xC shots

// ---- Settings (RAM copy of the EEPROM, at 0x121C; writing changes this game only) ----
constexpr uint32_t SETTING_COIN = 0x1225;      // byte: COIN ASSIGNMENTS (0x1A = FREE PLAY)
constexpr uint32_t SETTING_COUNTRY = 0x1226;   // byte: COUNTRY
constexpr uint32_t SETTING_LIFE = 0x122A;      // byte: LIFE
constexpr uint32_t SETTING_DIFF = 0x122D;      // high nibble: difficulty (read live)
constexpr uint32_t FREE_PLAY = 0x12F4;         // byte: 0xFF free play, 0 otherwise (worked out of SETTING_COIN)

// ---- Tasks and zones ----
constexpr uint32_t TASK_HEAD = 0x1C10B0;       // first task; node +8 next, +0xC function
constexpr uint32_t TRIGGER_TASK = 0x1C12A0;    // the zone trigger task (fn_00014764); +0: its first node
constexpr uint32_t ZONE_INFO = 0x1A37D4;       // 12 bytes per zone: {trigger table index, x, z}
constexpr uint32_t TRIGGER_TABLES = 0x120A1C;  // 28 bytes each, +8: the zone's records

// ---- Encounters (the script machine) ----
constexpr uint32_t VM_PC = 0x1A847C;           // int: script position (reads script::BASE + 4 × pc)
constexpr uint32_t VM_ENCOUNTER = 0x1A841C;    // int: encounter number (the boss fights' "label")
constexpr uint32_t VM_ALIVE = 0x1A8468;        // int: enemies alive (what op -1 waits for)
constexpr uint32_t VM_ALIVE2 = 0x1A83D4;       // int: the count op -1 waits for while flag 0x200 is set
constexpr uint32_t VM_FLAGS = 0x1A8414;
constexpr uint32_t VM_WAIT = 0x1A842C;         // int: frames waited by the current op
constexpr uint32_t VM_COUNTDOWN = 0x1C1D0C;    // int: op -21's frames left (loaded from its argument when 0)

// ---- Camera walk along the stage path ----
constexpr uint32_t CAM_WALK = 0x1A3960;        // 1: walk on (set when an encounter ends), 0: stop
constexpr uint32_t CAM_NODE = 0x1A3A0C;        // path node the camera heads for
constexpr uint32_t CAM_SPEED = 0x1A39C8;       // float: speeds up and slows down by itself
// Gait (fn_0001A4DC writes them): target speed 0..0xFF, 0 walking / 0xFF
// stopped, and how fast it gets there
constexpr uint32_t CAM_GAIT_SPEED = 0x1A3994, CAM_GAIT_HALT = 0x1A399C, CAM_GAIT_RAMP = 0x1A3990;

}  // namespace ram
