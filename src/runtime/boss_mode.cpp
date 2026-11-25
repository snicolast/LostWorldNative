// Boss mode, the window's side (boss_mode.h)
#include "boss_mode.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "game_ram.h"
#include "ppc_rt.h"
#include "runtime.h"
#include "stats.h"
#include "../common/clock.h"
#include "../common/paths.h"
#include "../ui/frontend.h"

void BossMode::init(Frontend& frontend) {
    job = CreateJobObjectW(nullptr, nullptr);
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    frontend.boss_status.assign(BOSS_COUNT, Frontend::BOSS_MISSING);
    for (int i = 0; i < BOSS_COUNT; i++)
        if (game_state_valid(boss_state_path(i))) frontend.boss_status[i] = Frontend::BOSS_READY;
    // Debugging aid: LW_BOSS=N starts the fight with boss N (1-4) right away
    if (const char* b = getenv("LW_BOSS")) {
        int i = atoi(b) - 1;
        if (i >= 0 && i < BOSS_COUNT && frontend.boss_status[i] == Frontend::BOSS_READY) pending = i;
    }
}

// Starts LostWorld.exe --make-boss-state for boss i (see job)
void BossMode::start_maker(int i, Frontend& frontend) {
    const std::wstring exe = exe_path_w();
    const std::string& rom = rom_zip.empty() ? g_data_dir : rom_zip;
    std::wstring cmd = L"\"" + exe + L"\" --make-boss-state " + std::to_wstring(i + 1) +
                       (rom_zip.empty() ? L" --data \"" : L" --rom \"") + widen(rom) +
                       L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
        frontend.boss_status[i] = Frontend::BOSS_FAILED;
        return;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    makers[i] = pi.hProcess;
    frontend.boss_status[i] = Frontend::BOSS_PREPARING;
    printf("[boss] preparing %ls\n", BOSSES[i].name);
}

void BossMode::update(Frontend& frontend, const Steps& steps) {
    for (int i = 0; i < BOSS_COUNT; i++) {
        if (makers[i] && WaitForSingleObject(makers[i], 0) == WAIT_OBJECT_0) {
            CloseHandle(makers[i]);
            makers[i] = nullptr;
            bool ok = game_state_valid(boss_state_path(i));
            frontend.boss_status[i] = ok ? Frontend::BOSS_READY : Frontend::BOSS_FAILED;
            printf("[boss] %ls: %s\n", BOSSES[i].name, ok ? "ready" : "could not be prepared");
            frontend.invalidate();
        }
    }
    // Missing states are made from the launch on (once the boot is
    // over and the EEPROM file exists, see --make-boss-state), one at
    // a time so the game keeps its speed; all at once while the list
    // of bosses is open
    bool can_make = !g_boot_skip && GetFileAttributesA("save/eeprom.bin") != INVALID_FILE_ATTRIBUTES;
    for (int i = 0; can_make && i < BOSS_COUNT; i++) {
        bool busy = std::any_of(makers.begin(), makers.end(), [](HANDLE h) { return h != nullptr; });
        if (frontend.boss_status[i] == Frontend::BOSS_MISSING &&
            (!busy || frontend.screen() == Frontend::Screen::Boss)) {
            start_maker(i, frontend);
            frontend.invalidate();
        }
    }
    if (pending >= 0 && !g_boot_skip) {
        if (game_state_load(boss_state_path(pending), true)) {
            loading = true;
            fight = pending;
            over_at = 0;
            shown_at = 0;
            frontend.boss_mode = true;
            // The menu stays up while the game runs on, unseen, muted and
            // at full speed, to put the fight back (a state only goes in from
            // the game's main loop, which can be seconds away, e.g. while the
            // next stage loads after a win). Closed at once, the game's last
            // picture showed for a moment first; at real speed behind the
            // menu, the click seemed not to work (runtime.cpp: the boot skip
            // ends once the state is in)
            steps.run_unseen();
            g_hold = false;
        } else {
            frontend.boss_status[pending] = Frontend::BOSS_MISSING;   // made again
            frontend.invalidate();
        }
        pending = -1;
    }
    if (loading && game_state_result() != 0) {
        loading = false;
        if (game_state_result() > 0) {
            // The boss states put the boss's stage first in the run, over
            // stage 1, and left the rest of the list as it was (0,1,2,3,4):
            // after the Deinosuchus came stage 2 again. The stages after it
            // follow on from it instead (1,2,3,4,0 for stage 2), so its stage
            // ends when the boss is down (states made before this included)
            int stage = BOSSES[fight].stage_index;
            int index = int32_t(rd32(ram::PLAYLIST_INDEX));   // the run's stage list and position
            for (int k = 1; index >= 0 && index + k < 5; k++) wr32(ram::PLAYLIST + 4 * (index + k), uint32_t((stage + k) % 5));
            lives = rd8(ram::P1_LIVES_BYTE);
            stats_boss_begin(fight);
            shown_at = now_seconds() + 0.1;   // its first frames drawn
        }
        if (game_state_result() < 0) {
            printf("[boss] could not start the fight\n");
            end(frontend);
            frontend.open_boss_list(true);
            steps.menu_over();
        }
    }
    if (shown_at > 0 && now_seconds() >= shown_at) {
        shown_at = 0;
        if (fight >= 0) steps.close_menu();
    }
    // The fight is over once its stage ends (the boss is down) or the
    // game is (no credits left): a moment later, the main menu, with
    // the arcade game as it was waiting behind it. Stage 5 is the last:
    // the game ends after it (the ending, TOTAL SCORE) with the stage
    // unchanged, and the player goes out with lives left, as when a whole
    // game is finished (stats.cpp, end_game); out with none, the continue
    // ran out
    if (fight >= 0 && !loading && !frontend.active()) {
        // Player 1 state (docs/game_ram.md): 3 out, 0 on the CONTINUE screen
        bool out = rd8(ram::P1_SLOT) == 3;
        if (!out) lives = rd8(ram::P1_LIVES_BYTE);
        bool stage_ended = rd8(ram::STAGE) != BOSSES[fight].stage_index;
        bool over = stage_ended || out;
        if (!over) over_at = 0;
        else if (over_at == 0) {
            over_at = now_seconds();
            stats_boss_end(stage_ended || lives > 0);   // the boss is down
        }
        // The next stage starting (its map) is not shown: the menu at once
        if (over_at > 0 && (stage_ended || now_seconds() - over_at > 3)) {
            to_menu(frontend);
            steps.menu_over();
        }
    }
}

void BossMode::end(Frontend& frontend) {
    fight = -1;
    frontend.boss_mode = false;
}

void BossMode::to_menu(Frontend& frontend) {
    end(frontend);
    game_state_load_home();
    frontend.open(Frontend::Screen::Main);
}
