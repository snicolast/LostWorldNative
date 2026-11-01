# Native port architecture

Goal: a PC executable that runs the original logic of The Lost World
translated to C++, with its own engine for graphics, sound and controls. The game
still needs your `lostwsga.zip` to load its data; the repository never
contains ROM data (`data/` is in `.gitignore`).

## Original hardware (Model 3 Step 1.5)

| Part | Original | In the port |
|---|---|---|
| CPU | PowerPC 603e, 100 MHz | Code translated to C++ (static recompilation) |
| GPU | Real3D Pro-1000 (scene graph in memory, matrices, textures in VROM) | Own Vulkan 1.3 renderer (`src/video`) that reads the same structures |
| 2D layer | Tilegen (HUD, text) | Own 2D layer over the 3D scene |
| Sound | 68000 + 2× SCSP | Musashi 68000 + own SCSP (`src/audio`), resampled to the output device |
| Guns | Analog position read | Mice and light guns (raw input), gamepads (SDL3, with gyro) |

## What we already know about the program

- The reset vector (`0xFFF00100`) initialises the hardware, clears RAM and copies
  three blocks from CROM to RAM (table at `0xFFF00058`):
  - `FFE00000 → 00000000` (1 MB)
  - `FFF20000 → 000FFFFC` (237 KB)
  - `FFF9C000 → 0019C000` (27 KB)
- Then it jumps to `0x00000000`: **the whole game runs from RAM**, which makes
  recompilation easier (fixed code, no dynamic loading).
- About **240,000 instructions (938 KB)** reachable in **1,636 functions**.
- The decoder recognises 100% of the reachable code (the 4 "unknowns"
  are empty exception vectors).
- 95 indirect `bctr` jumps (switches/jump tables) to resolve.
- Heavy floating-point use (`fmuls`, `fadds`, `fctiwz`...): 3D logic and
  matrices on the CPU, which gives us direct access to camera and projection for
  widescreen and VR.
- Hardware regions accessed: `0x84000000`–`0x9C000000` (Real3D),
  `0xC0000000`–`0xC1000000`, `0xF0040000` (inputs), `0xF0100000` (system
  control and interrupts), `0xF0800000`/`0xF0C00000` (PCI bridge).

## Build pipeline

```
lostwsga.zip ──romtool.py──▶ data/*.bin   (LostWorld.exe also reads the zip directly: rom_zip.cpp)
data/crom.bin ──analyze.py──▶ out/listing.asm, functions.txt, report.txt
              ──recomp.py──▶ build/gen/*.cpp  (one C++ function per PPC function, 40 per file)
gen/*.cpp + src/ ──CMake/Ninja/MSVC──▶ LostWorld.exe   (build.bat does all of it, then runs ctest)
```

The recompiled code (`build/gen`, derived from the ROM) is never in the
repository and never distributed: everyone builds their own executable from
their own `lostwsga.zip`. Shaders (`src/video/shaders/*.vert|frag`) are
compiled to SPIR-V headers at build time by glslang (target Vulkan 1.2); the
68000 of the sound board is Musashi, whose opcode tables are generated at build
time too. SDL3 (gamepads only) and miniz (the zip) are fetched by CMake.

## How the recompiled code runs

`tools/recomp.py` turns every PowerPC function into a C++ function `fn_XXXXXXXX(Ctx& c)`:
registers live in `Ctx`, memory goes through `rd8/16/32` and `wr8/16/32`
(`src/runtime/ppc_rt.h`), which take the fast path into the 8 MB RAM array and
fall back to `rt_rd*_slow`/`rt_wr*_slow` for everything else (the devices:
Real3D, tilegen, inputs, sound port, SCSI, EEPROM; `src/runtime/runtime.cpp`).
Every basic block adds its instruction count to `c.cycles`; `POLL(pc)` at the
back branches calls `rt_event` when the next timeline event is due, which is
how vblanks, timers and interrupts happen at the original 100 MHz pace. A few
functions are wrapped or replaced by the runtime (`IDLE_LOOPS`, subtitles,
projection for the aim assist, the gun aim in a wide view: the tables at the
top of `recomp.py`); `rt_hook_function` lets the modes trace or intercept a
function by address.

The game itself is unchanged: the modes (bosses, survival, random spawns,
weather) and the fixes work by reading and writing game RAM at the addresses
documented in `docs/game_ram.md`, or by intercepting calls.

## Threads

| Thread | Where | What |
|---|---|---|
| Game | `runtime.cpp`, started by `app.cpp` with a 512 MB stack (the recompiled code nests deeply) | Runs the recompiled game at real speed (`pace_frame`), publishes a Real3D frame at each command-port write (`r3d_flush`: unchanged 64 KB pages shared with the previous frame) and the 2D layer memory at each vblank (dirty pages only), drives the sound board, saves/loads states at safe points (`rt_state_point`) |
| Window | `app.cpp` (`App::run`) | Win32 messages, inputs (mice/light guns, gamepads), the menus, and one image per monitor refresh: picks the newest prepared frame, interpolates the 3D between game frames (`set_interpolation`), composes and presents (`VkPresenter::frame`) |
| Preparation | `App::prepare_frames` | For every game frame as soon as it is published: builds the 3D scene (`R3DSceneBuilder`) and composes the 2D layers (`TileGen::compose`, 2D replacements, HD text) side by side, so the window thread only draws |
| Audio | `src/audio/audio_out.cpp` | WASAPI output; pulls from the `AudioStream` queue the sound board fills (resampling and rate control there) |
| Boss state makers | separate processes (`--make-boss-state N`) | Play headless up to each boss and save its state, one at a time while the game runs |

Shared state crosses threads as immutable `shared_ptr` snapshots
(`R3DFrameState`, `Prepared`) or under short mutexes; the game thread is never
blocked by drawing.

## One frame, end to end

1. The game writes culling/polygon RAM, the texture FIFO and the tilegen; at
   the command-port write `r3d_flush` snapshots the Real3D pages (shared where
   unchanged) with the vblank it belongs to and wakes the preparation thread.
2. Preparation builds `R3DScene` (viewports, vertices with their previous
   position for interpolation, LOD, the world tracker's camera estimate) and
   the 2D layers of the same vblank (`Layers2D`), and queues the result.
3. At each monitor vblank (`DisplayClock`, DWM timing or `VK_KHR_present_wait`)
   the window thread takes the frame due, sets the interpolation fraction and
   records the GPU passes (`R3DRenderer::record`): a strip of the procedural
   sky's cloud panorama, ray tracing build (if any), sun shadow map, the world
   into HDR colour + motion + glow (MSAA or DLAA; the glow only with bloom on),
   ambient occlusion at half resolution, bloom, light shafts, diffusion,
   exposure, and the composition into the swapchain image (2D back layer or
   procedural sky where the 3D leaves them visible, the 3D image, 2D front
   layer and HD text, replacement pictures, sharpening, crosshairs).
4. One frame is in flight at a time (a single fence), which keeps the light
   gun's latency low; with `VK_KHR_present_wait` at most two images wait for
   the screen.

## 2D layers and replacements

`src/video/tilegen.cpp` composes the four tilegen layers into a back picture
(behind the 3D) and a front picture (HUD, text). On top of the original pixels:
`TextHD` draws the game's letters from vector fonts (an atlas per glyph
pattern), `Pack2D` recognises 8x8-cell pictures by hash and swaps in PNGs
(`textures/2d`, `textures/2d_back`), `SkyPack` recognises the 512x512 backdrops
by hash and either swaps in a PNG (`textures/sky`) or draws the procedural sky
(`sky_procedural.glsl`, with a CPU twin for the tests: its clouds are kept in a
panorama texture redrawn a strip per frame, `sky_fields.frag`, and shaded with
the sky's colours in the composition), and `SkyMotion` moves
the back layers between game frames with the camera. `TexturePack` does the same
for 3D textures (`textures/`, by hash of the decoded texture).

## Saved state

`save/` holds `settings.ini` (the menu; one table in `frontend.cpp` drives the
file, the rows and their behaviour), `eeprom.bin` (the arcade's settings and
records), `survival/` (arenas unlocked, records) and the game states: the
bosses' in `save/boss/` and the survival arenas' in `save/survival/`, in the
`LGWS` format (version 2):
the call chain, the CPU context, RAM, device pages, texture RAM and the sound
board (`SNDS`), all run-length packed. A state is only loaded from the same
call chain it was saved in (the main loop), so loads happen at `IDLE` points.

## Modes

- **Boss mode** (`bosses.h`, `runtime.cpp`): a saved state per boss, made by a
  helper process; retry from the pause menu.
- **Survival** (`survival.cpp`): the game's own encounter machine
  (`encounter_script.h`) is held at one stop and fed waves; laps restore a
  marked state under a fade.
- **Random spawns** (`random_spawns.cpp`): the arcade game with its encounter
  scripts shuffled per stage.
- **Weather** (`weather.cpp`, `r3d_weather.h`, `weather_sound.cpp`): presets
  that drive the renderer's fog, rain, lightning and the sound.
- **Achievements and stats** (`stats.cpp`): read from game RAM at every vblank
  on the game thread; the menu shows copies.

## Settings

The menu (`src/ui/frontend.cpp`) owns `FrontendSettings` and `save/settings.ini`;
`app.cpp` maps them to the command-line `Options` (`options.cpp`, one table for
the parser and `--help`) and hands them to the renderer and the sound. The
first start picks a QUALITY level from the GPU's name (`suggest_quality`).

## Tests

`ctest` (run by `build.bat`): tilegen composition against golden hashes, FNV-1a
in C++ and Python, the options table, the sky scroll reconstruction, the audio
queue, the ROM layout from the zip, the encounter script reader (against
`tools/encounters.py`), the SCSP's part of the saved states, the weather sound
and the procedural sky (CPU against GLSL). Full-game checks are headless runs
(`--headless --frames N`, deterministic: `out/ram.bin` and `out/real3d/*` can be
compared by hash) and `--shot N` captures.
