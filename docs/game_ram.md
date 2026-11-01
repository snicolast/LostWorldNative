# Game RAM (confirmed addresses)

PowerPC RAM addresses (big-endian) used by the port. "Confirmed" means seen in
a real game with `--trace-survival`, not just in the disassembly.
`python tools/analyze.py` generates out/listing.asm for reading the code;
`python tools/encounters.py` lists the encounter scripts.

## Game and player

| Address | Type | What it is |
|---|---|---|
| 0x1A623C | byte | Current stage, from 0 (0 = stage 1). Set by fn_00081364 |
| 0x1C2AFC | 5 × int | List of the route's stages, in order |
| 0x1C2B10 | int | Index into that list; fn_00081364 does `stage = list[++index]` |
| fn_000812DC | function | New game: list = 0,1,2,3,4 and index = -1. Expert calls fn_00081364 several times to skip stages |
| 0x1C2AE0 | int | Chosen route (0 Beginner, 1 Expert, 2 Shuffle) |
| 0x1C2AD8 | int | Route selection: 0 attract, 1-2 choosing, 0xC playing |
| 0x1A3984 | int | Current zone of the stage (low nibble) |
| 0x1A6F28 | 3 × float | Camera position (x, y, z). Zones trigger by distance in x/z |
| 0x1A3454 | 3 × float | Point the camera turns towards (changed by encounter operation -9) |
| 0x1A36C8 | object | Player 1; player 2 follows (0x5C-byte objects, 0x1A3724) |
| 0x1A3720 | int | Player 1 lives (object + 0x58). Starts at LIFE + 1 (fn_00010804). 0x1C1260 is a copy (the hearts don't read it) |
| fn_000463D4, fn_00071460 | function | Player hit: sound, effect (task 0x46720) and lives - 1 |
| 0x1A857C | int | Player 1 score (the stage score is at 0x1A858C + 4 × stage); a raptor is worth 500 |
| 0x1C1D54 | int | The score shown by player 1's counter (+4 for player 2): fn_0003E800 redraws the digits when it doesn't match 0x1A857C, and the game sets -1 to force it |
| 0x1A3784 | 0x10 × n | Player slots; +0xD = 1/2 while playing (0x1A3791 player 1, 0x1A37A1 player 2), 0 on the CONTINUE screen, 3 out (also during the attract demo, which plays by itself and scores points) |
| 0x1A858C | 5 × int | Player 1 score per stage (0x80-byte block per player from 0x1A857C; +0x10 + 4 × stage) |
| 0x1A377C | int | Player 2 lives (object 2 + 0x58; the routine at 0x2FDC reads it as 0x1A36C8 + 0xB4) |
| fn_001180A8 | function | Reads the controls: 0xF0040008 player 1, 0xF004000C player 2, the system port (start 1 bit 4, start 2 bit 5) and the 8 ADC channels |
| 0xE9C | 4 bytes × 8 | ADC channels as fn_001180A8 leaves them (+4 per channel: 0/1 player 1 X/Y, 2/3 player 2): current, previous, average, difference |
| fn_0000CBF0 | function | The gun's aim in a stage from the ADC values (object +0x34 x, +0x38 y, units from the middle of the screen, X drawn at px 248 + 0.9797 x, 192 - 0.9957 y; player in byte +0xC of the object at +0). Piecewise linear about the calibration's centre, 232 (X) / 176 (Y) units to its edges, not clamped: the 8-bit ADC is what keeps it near the 4:3 screen. The runtime replaces it in a wide 3D view (rt_gun_aimed, at 0xD49C) |
| 0x123C | 6 bytes × 2 | Gun calibration (settings copy of the EEPROM), player 2 at 0x1242: X centre, Y centre, X right edge, X left edge, Y edge (ADC above the centre), Y edge (below). Factory: 118 110 213 24 207 12 / 123 108 222 29 199 5 |

## Results screen counters

What the game shows at the end of each stage (HITS, TECHNICAL SHOT, HIT RATE,
EVENT CLEAR). One 0x10-byte block per player from 0x1A6208, half
words; reset to 0 on each stage. Confirmed with automatic games
(src/runtime/stats.cpp reads them for statistics and achievements).

| Address | What it is |
|---|---|
| 0x1A620C | HITS: hits that score points (doesn't count types 7 and 8) |
| 0x1A620E | TECHNICAL SHOT: hits of a type other than 0 |
| 0x1A6212 | Type 8 hits |
| 0x1A6214 | Shots |
| fn_0003DC60 | `(type, player from 1, points)`: every scoring hit goes through here (67 calls). The enemies choose the type by the part hit: on raptors (fn_0005B5E0) 0 normal, 1, 3, 4, 5 and 6 specific parts (they count as technical); 2 is worth 200 points; 7 is an event with no points |

The game doesn't count kills: in script-machine encounters, a hit that
downs an enemy lowers 0x1A8468 in the same frame, so a drop right after a
hit is a downed dinosaur. The ones that don't use the machine (the stage 1
cabin, the bosses) only count hits.

## Camera and stage path

The camera task is fn_000195B8. The camera walks towards the path node it is
heading to while turning (it isn't on rails) and the game moves on to the next
node when it reaches it; if it is sent to a node it has already passed, the
game sets the next one again.

| Address | Type | What it is |
|---|---|---|
| 0x1A3960 | int | 1 = walk, 0 = stop. Set to 1 by the end of an encounter (-999, via fn_0003B3C4) and to 0 by the stop trigger fn_0003B3F4 |
| 0x1A3A0C | int | Path node it is heading to |
| 0x1A39C8 | float | Speed (accelerates and brakes by itself; ~8 walking, brakes in ~20 units) |
| 0x1A3994 | int | Target speed 0..0xFF (0xFF when walking) |
| 0x1A399C | int | 0 walking, 0xFF stopped |
| 0x1A3990 | int | Acceleration ramp (0 = sudden stop, like a stop trigger) |
| fn_0001A4DC | function | `(ramp, speed, stop)`: writes those three; the end of an encounter calls it with (0x22, 0xFF, 0); the trigger fn_0001A5B4 does the same with the record's data |
| 0x1A3958 | int | Heading (16 bits) |
| 0x1A39F4 / 0x1A3A4C | 3 × float | Camera position and the point it looks at |
| 0x1A6F4C | 3 × float | Point the camera looks at (copy; 0x1A6F28 is its position) |
| 0x1A397C | int | How the camera moves (fn_000167B0): 1 = from its own position 0x1A6F28 with speed and heading (fn_000167FC; writing 0x1A6F28 moves it), other = between path nodes (fn_0001727C, recomputes 0x1A6F28 every frame) |
| 0x1A3894 | pointer | Table of the stage's path nodes: 2 floats per node (x, -z) |
| 0x1A3B58 | float | Fraction 0..10 between node 0x1A3A0C and the next: position = (node·(10 - t) + next·t) · 0.1 + 0x1A37D0 / 0x1A37B4 (with z negated). Rewriting both nodes and setting t = 0 places the camera anywhere |
| 0x1A6F38 | float | Eye height above that path point (4.64): the eye is at y + 4.64 (the y axis points up) |

The image has no separate camera matrix: Real3D matrix 0 only
swaps the axes and the game sends every piece already relative to the camera. To
go from the game world to the screen (the survival mode markers) you use the
eye position, the point it looks at and the projection of the world viewport;
attack shakes and encounter camera turns aren't in there, which is why the
markers only appear during the breaks.

If the camera goes back and crosses the zone boundary (x = 384 between
zones 0 and 1 of stage 1), the game reloads the zone's triggers
and launches its encounters again.

## Settings (RAM copy of the EEPROM, at 0x121C)

Writing here only changes the current game; the EEPROM is not touched.
Don't write during boot (the game hangs): from frame ~1000 on.

| Address | What it is |
|---|---|
| 0x122A | LIFE (byte) |
| 0x122D | Difficulty = high nibble (7 by default). Read live by about 10 enemy routines |
| 0x1230 | BOSS ACTION |
| 0x1226 | COUNTRY |

## Tasks

| Address | What it is |
|---|---|
| 0x1C10B0 | First task; 0x1C10B4 the last |
| node +4 / +8 | Previous / next |
| node +0xC | Function run every frame (the task can change it: a raptor 0x53D9C switches to 0x63144 when it dies) |
| fn_00116A48 | `(size, function)` → creates the task at the end of the list (minimum size 0x10) |
| fn_00116674 | Deletes a task |
| fn_00116830 / fn_00116918 | Allocates / frees memory (heap at 0x1C10A4) |

## Zones: distance triggers

- fn_000144DC loads the triggers of the current zone: it creates the task
  fn_00014764 (stored at 0x1C12A0, +0x10 = zone) and a list of nodes from
  `table 0x120A1C[ 0x1A37D4[zone * 12] ] + 8` (28-byte records).
- 0x1A37D4: 12 bytes per zone `{index, x offset, z offset}`;
  record and encounter coordinates are relative to the zone.
- Record in ROM (24 bytes): `{function, x, z, radius, p0, p1}` (integers).
  fn_000143CC turns it into a node (0x20 bytes): `+4 function, +8 x, +0xC y
  (ground height, fn_00033448), +0x10 z, +0x14 radius, +0x18 p0, +0x1C p1`.
- The task fn_00014764 calls `function(node + 4)` when the camera is
  closer than radius / 2 in x/z (call at 0x148B0; returns to 0x148B4) and deletes the
  node. If the zone changes, it deletes all the nodes and ends.
- Functions seen in stage 1: 0x39428 (starts an encounter, p0 = number),
  0x1A940, 0x1A5B4, 0x1A580, 0x3B3F4, 0x3B3C4 (camera, sound, scenery).

## Encounters (the script machine)

Each shooting stop is a script. fn_00039428 (from a trigger) looks for the
header `-10 number` in the script and creates the task **fn_0003951C**, which
runs it. **The camera only keeps walking along the path when that task
ends** (operation -999); while it exists, it stands still when it reaches its next
stop point. Enemies that come in and leave by themselves (without shooting)
also count as cleared: without shooting, each encounter lasts 1-1.5 min.

| Address | What it is |
|---|---|
| 0x1268B8 | Script: 32-bit words (0x13DF in the ROM, all stages except 2) |
| 0x1A847C | Program counter (index: reads 0x1268B8 + 4 × pc; can point to any RAM) |
| 0x1A841C | Encounter number |
| 0x1A842C | Frames waited by the current operation |
| 0x1C1D0C | Countdown of operation -21 (loaded with its argument when it is at 0) |
| 0x1A8414 | Flags (0x200: -1 waits on 0x1A83D4 instead of 0x1A8468; 0x8: the camera is turning) |
| 0x1A8468 | Live enemies |
| 0x1A83D4 | Another enemy counter (for flag 0x200) |

Script format:

- Positive entry: `delay function nargs args...`: waits `delay` frames and
  calls `function(args)`. Enemy functions (raise 0x1A8468):
  0x53BB4 (raptor; args `slot, type, position group`), 0x61AF8,
  0x627E8, 0x46B50, 0x536CC, 0x63ADC, 0x7409C and 0x71914. 0x6053C, 0x5F8FC and
  0x4BF4C also raise the counter, but the 0x6053C ones (encounter 36) don't
  leave by themselves and leave the wave unfinished: off the list. Others: 0x48F78 (flying, don't
  count), 0x3B424 / 0x3B5C4 / 0x3B46C (sounds), 0x389DC / 0x386D8 (cutscenes).
- Operations (negative), with the words that follow them:

| Op | Args | What it does (seen in the trace) |
|---|---|---|
| -10 | 1 | Header of encounter N |
| -999 / -998 | 0 | End: the task ends and the camera moves on |
| -1 | 0 | Waits until no enemy is left alive |
| -27 | 1 | Waits until at most N are alive |
| -21 | 1 | Waits N frames |
| -9 | 3 | Turns the camera towards (x, y, z) relative to the zone (0x1A3454) |
| -20 | 1 | Flag 0x200 (which counter -1 waits on) |
| -25, -26 | 3, 2 | Encounter settings (to be confirmed) |
| -19, -11, -17, -24, -5, -8, -23, -28 | 1, 4, 2, 2, 1, 1, 1, 1 | To be confirmed (light, camera, loops) |
| -12, -13, -14, -15, -22, -29..-33 | 0 | To be confirmed (-14/-15 enclose blocks) |

Encounters seen: stage 1: 2 (intro), 3, 5, 4, 6 (zone 1), 7, 8, 9, 10; stage
3: 18 (cutscene), 19 (zone 0), 36, 30 (zone 1); stage 4: 37, 41 (zone 0, no
enemies from the list). Stage 2 doesn't use this machine (its triggers are
0x85DD4, 0x86434, 0x8609C...) and neither does stage 5 (0x1ECE8, 0xFF57C...).

The boot code copies the script from the ROM at 0xFFF468BC, which random
spawns (src/runtime/random_spawns.cpp) puts back before each encounter and
then changes in place. The zone's encounters are in its trigger records:
`0x1A37D4[zone * 12]` indexes the 28-byte tables at 0x120A1C, whose +8 is the
list of 24-byte records (fn_00039428 with p0 = the encounter).

## Bosses' life

Each boss keeps its life in a word of its object, which every hit lowers
(by 1, or by 2 while flag +0xC8 is clear) and the boss's code sets when the
fight starts. Its scoring call (fn_0003DC60) says which boss was hit by its
return address; at that call the caller's r31 + slot holds the object.

| Boss | Return address | Slot | Life word | Set to |
|---|---|---|---|---|
| T-Rex female | 0xCCB74 | +0x08 | +0x98 | 150 or 250 (fn_000CC48C, bit 0x8 of 0xE7C) |
| Deinosuchus | 0x8E86C / 0x8E888 (body), 0x8F14C (weak spot, -14) | +0x10 / +0x0C | +0x4C | 170, 35 |
| Carnotaurus | 0xE86EC (lowers it after the call) | +0x18 | +0x98 | 530, then about 100 again and again |
| T-Rex | 0xFE9D4 | +0x08 | +0x98 | 620, then about 150 again and again |

With 10 % of it (LW_BOSS_LIFE=10, random spawns) the T-Rex female goes down
after a few hits; the later values of the Carnotaurus and the T-Rex are set
again after almost every hit, so they are not just a life.

## Survival mode

`--survival STAGE:ZONE` or PLAY > SURVIVAL in the menu (src/runtime/survival.cpp,
screens in src/ui/survival_ui.cpp, startup in app.cpp):

- Arenas: SURVIVAL_ZONES in survival.cpp, the first encounter with known
  enemies in each zone, found with `LW_SURV_AUTOPLAY=1 --survival S:99
  --trace-survival` (the trace logs every encounter with its zone and its
  enemies): stage 1 zones 1 (encounter 3), 5 (8) and 7 (10, dropped: the
  T-Rex); stage 3 zones 0 (19) and 1 (30); stage 4 zone 1 (43; up to it the game
  waits for shots, so the loader fires by itself and keeps the lives).
- Entry: the window leaves the game in its "home" state (the arcade in
  attract mode: the first time it is saved at the SEGA logo, later times it is
  reloaded), and the game runs at full speed and silent (the sound board
  sends nothing to the output and comes back in with a 20 ms fade)
  behind the loading screen. The mode inserts a credit and presses start until
  route selection opens (0x1C2AD8), shoots in it, and the stage list
  is rewritten on the first call to fn_00081364 after fn_000812DC so
  the route lands on the chosen stage. If the arena is not reached after
  40000 frames, the window goes back to the menu (and the arena is locked
  again). Service corridor is never reached this way: stage 3's lobby waits
  for the 0x6053C enemies of encounter 36, which never leave by themselves,
  and playing the way's encounters without their enemies (or ending them at
  once) loses the stage's path; it only opens from the arcade game (below).
- Arena cache: the first time an arena is reached, the whole game state is
  saved to `save/survival/STAGE_ZONE.state`. Later runs load it right after
  the home state, put back the LIFE and difficulty settings (0x122A,
  0x122D) as they are now and take the arena's first encounter over
  again from its header (the stage and zone must match), so
  the way there is skipped. A state that does not load is deleted and the
  way there is played again; LW_SURV_NO_CACHE=1 ignores the cache.
- Unlocks: when the arcade game (0x1C2AD8 = 0xC, not --make-boss-state)
  calls fn_00039428 with an arena's first encounter (SurvivalZone::encounter)
  in its stage and zone, the arena is unlocked (save/survival/unlocked.ini,
  `stage:zone lives life`: the most lives the player had in that game and
  the LIFE setting then) with a notice, and the state is kept there if there
  is none. Loading it, the encounter is taken over from its header and the
  lives are set to that number, plus the change in the LIFE setting since. Reaching an arena in survival mode also unlocks it.
- Hold: on the first encounter with enemies in that zone it copies its script to
  0x700000 (free RAM: the game never goes past 0x200000) replacing the end with
  `-21 0x3FFFFFFF` (endless wait), and the game stops on the start
  screen. The encounter task never ends, so no other
  encounter starts (the call to fn_00039428 is skipped) and the camera doesn't move on
  by itself. On the first shot the game state is saved in memory
  (game_state_mark in runtime.cpp, like the "home" state): the start of
  each lap.
- Lap: when the machine reaches the endless wait, the stop is
  cleared; after a few seconds (4, fewer on each lap) the game's walker is
  released as at the end of an encounter (0x1A3960 = 1 and the gait
  0x22, 0xFF, 0 of fn_0001A4DC; flag 8 of 0x1A8414 is cleared so the
  camera stops looking at the last turn). The game walks along its path and the
  next stop's trigger (fn_0003B3F4) stops it; its encounter's trigger is
  skipped but recorded, and when the camera stops a
  wave is written at 0x710000 / 0x720000 (alternating), the -21 countdown is set to 0
  (0x1C1D0C, otherwise the wave inherits the endless wait) and its pc is changed.
  The dinosaurs left are the live counter plus the enemy calls the pc hasn't
  passed yet.
- Waves: an encounter is a row of sections: a camera turn (-9) towards where a
  group of spawn positions is, sometimes its settings (-25 / -26), its enemy
  calls (the raptor fn_00053BB4 takes `slot 1-5, type, group of positions`) and
  a wait for all of them to die (-1). Each wave is batches of 2 to 5
  dinosaurs, each from a random section (never the same one twice in a row) of
  the stop's encounter or of another encounter of the zone whose camera turn
  lands within 120 units of one of this stop's. The camera turns there before
  they come out, so nothing appears behind the player without warning.
  Raptors get distinct slots (one alive per slot) and sometimes another of the
  types the zone uses; other enemies stay as in their script, at most one of
  each kind per batch. Between batches, -1 or (more often later in the lap)
  -27 1, and a random pause. Dinosaurs per wave: those of the arena's first
  encounter (at least 8) plus 2 per stop of the lap and 6 per lap, up to 30;
  bigger batches and a higher difficulty nibble too. If a wave comes out the
  same as the one before, it is made again.
- Countdown: every wave begins with a 5 s wait (-21 288) after the camera turn
  of its first batch, while the screen counts 5..1 and GO! (SURVIVAL_COUNTDOWN);
  the game runs, so the player can aim. In the first wave (the encounter's own
  script) it goes after its first camera turn, minus what the script already
  waits before its first enemy (-21 and that call's delay). Ambushes have no
  countdown, only their AMBUSH! warning.
- The last stop of the lap is the one with no encounter trigger with
  known enemies ahead of it (the remaining stops only lead to the next
  zone; in the lobby, the corridor door). When it is cleared: fade
  to black, the saved state is reloaded and lives and
  score are restored (0x1C1D54 to -1 so the counter shows it). The state was kept
  before the encounter's first words ran, so the first wave of the new lap
  starts with what the encounter does before its first camera turn and enemy
  (calls and settings, no waits): in the Service corridor that opens the room
  behind the tanks (fn_0003B46C); without it the raptors stayed inside and
  wave 2 never ended. If a walk changes
  zone or doesn't reach any stop within a minute, the lap ends there.
- Ambush: on 35 % of walks (those longer than 40 units), halfway
  to the next stop the walker is stopped and a short batch from a section
  of that stop's encounter arrives at 0x730000, with its camera turn; once it is beaten the walk
  continues (or, if the stop's trigger fired meanwhile, it has already arrived).
- BONUS: on half of the walks a golden target appears for a few seconds on the
  4:3 screen; a shot from either gun within 30
  pixels (player_aim) adds a life at 0x1A3720 (up to 9, the hearts counter
  has one digit) or 5000 points.
- Lives: on entering fn_000463D4 / fn_00071460 with one life, the mode adds one
  before the hit, so the game never reaches 0 or CONTINUE; that hit
  ends the hunt (results screen, record in save/survival.ini). Play
  again restarts from the home state; Esc returns to the main menu
  with the home state. The record (save/survival.ini: `stage:zone wave score
  lap`) is the highest wave, with its score and its lap.
