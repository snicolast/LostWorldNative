# The Lost World in NicoEngine

[NicoEngine](https://github.com/snicolast/nicoengine) runs Sega Model 3 games
inside one program, with its own renderer, editor and options. It runs this
game too: it compiles the game's scene builder, its emulator and its sound
from a checkout of this repository, with the game's own recompiled program
(`build/gen`, made from your ROM by this game's build). Nothing of the
engine is needed to build or play the game, and nothing the engine does
changes how the game plays by itself.

The engine asks every game for the same things, through its **game API**
(the engine's `docs/GAME_API.md` and `api/nico_game_api.h`). This file says
what this game gives, where it is, and what in the game must not be deleted
or renamed although the game itself may not use it.

**API version kept: 1** (`src/engine/nico_game.cmake`, `NICO_GAME_API`).

## The game's side: `src/engine/`

Compiled by the engine only; the game's `CMakeLists.txt` never lists it.

| File | What it is |
| --- | --- |
| `nico_game.cmake` | What the engine compiles of the game: the API version, the game's id, the adapter's own files, the emulator's files, the 68000 core, the control modules. The engine's `CMakeLists.txt` reads it. |
| `game_scene.h` | The game as the engine's Model 3 adapter sees it: identity, the builder's setup (HUD marks, no cinematic bars), the HUD's sides and bands in a wide picture, the procedural skies, the texture pack. |
| `live_game.h`, `live_game.cpp` | The game running in the engine: starting the boards as `app.cpp` does for the cabinet's game, holding it, its controls, its settings, stopping it. |
| `live_host.cpp` | What the emulator asks of whoever hosts it, stood in for: the PC edition's modes (survival, shuffled encounters, records, boss maker) are never on inside the engine. |

Inside the engine the game is the cabinet's: a coin and START, attract mode,
no menu of the PC edition, nothing written to `save/` but the cabinet's
EEPROM (kept apart from the game's own).

## The pieces

| # | Piece | This game |
| --- | --- | --- |
| 1 | Identity | `game_scene.h` (`NAME`, `ID`, `REPO`, `EXE`), `nico_game.cmake` (`NICO_GAME_ID`, `NICO_GAME_BOARD`) |
| 2 | Scene | `src/video/r3d_scene.*`: `R3DSceneBuilder` with `build`, `set_vrom`, `instances()`, and `R3DViewport::build_index` |
| 3 | Frame state | `src/video/r3d_state.*`: `R3DFrameState`; `LWS1` snapshots (`src/runtime/debug_snapshot.cpp`) |
| 4 | Material traits | The builder's `R3D_*` vertex flags, which the engine's adapter turns into its materials. Scenery fading into the distance and the rain are not given yet (`game_scene.h`, `traits`) |
| 5 | Culled faces | Not given: the builder keeps no back faces, the engine finds them with a second build |
| 6 | Wide screen and HUD | The builder's `set_hud_edges` (`R3D_HUD_LEFT`, `R3D_HUD_RIGHT`) and `set_remove_letterbox`; the top rows' band (`game_scene.h`, `bands`, after `frame_prep.cpp`'s `hud_band`) |
| 7 | Packs and skies | `src/video/texture_pack.*`, `text_hd.*`, `pack2d.*`, `sky_pack.*` with `set_procedural`, `sky_procedural.*`, read from the folder the engine gives |
| 8 | Life | `live_game.cpp`: `live_start`, `live_hold`, `live_shown`, `live_stop` |
| 9 | Runtime hooks | `live_host.cpp`: 14 stand-ins |
| 10 | Controls | The engine's light gun module (`NICO_MODULE_GUN`, its `modules/light_guns.h`); `live_controls` gives what it says to the game's `Inputs`; `live_sights` gives the guns' sights for the engine to draw (the game's own crosshair is its window's) |
| 11 | Settings | `live_settings`: sound, sky, pad vibration (and the gun module's own, its crosshair among them) |
| 12 | Recompiled program | `build/gen` with `files.cmake` and `fns.h`, `src/runtime/ppc_rt.h` |

Version 1 still has the game's side in C++, talking to the engine's own
classes. The plain C table of the API (`nico_game_entry`) comes when the
engine's host calls it.

## Do not delete or rename

The engine compiles these and calls them by name. Changing one breaks the
engine's build of this game (its checks build this repository), not the
game's own.

**Files it compiles:** `src/video/` `r3d_state.cpp`, `r3d_scene.cpp`,
`r3d_screen.cpp`, `r3d_world.cpp`, `image_io.cpp`, `tilegen.cpp`,
`sky_pack.cpp`, `sky_procedural.cpp`, `texture_pack.cpp`, `text_hd.cpp`,
`pack2d.cpp`; and the files listed in `nico_game.cmake`
(`NICO_GAME_LIVE_SOURCES`, the Musashi core). A file moved or split there
is fixed in `nico_game.cmake`, in the same pull request.

**The scene builder** (`r3d_scene.h`): `R3DSceneBuilder::build`, `set_vrom`,
`instances()`, `set_hud_edges`, `set_remove_letterbox`, `set_texture_pack`,
`model`, `model_word`; `R3DViewport::build_index`, `trans_first`,
`trans_count`; `R3DInstance::opaque`; `R3DVertex::replace` and `tex`; the
flags `R3D_HUD_LEFT`, `R3D_HUD_RIGHT` and the material ones (`R3D_TEXTURED`,
`R3D_ALPHA_TEST`, `R3D_TEX_ALPHA`, `R3D_MIRROR_U/V`, `R3D_SMOOTH_U/V`,
`R3D_LUMINOUS`, `R3D_FIXED_SHADE`, `R3D_SPECULAR`, `R3D_SHININESS_SHIFT`,
`R3D_INVERT_RGB`, `R3D_WORLD`, `R3D_ACTOR`, `R3D_TRANSLATOR`).
`instances()` and `build_index` are used by nothing in the game itself.

**The 2D and the packs:** `TileGen::compose`, `front_offset`,
`SKY_COORDS_W/H`; `TextHD::ROWS`, `ROW_TEXELS`, `CELLS_X/Y`, `CELL_TEXELS`;
`Pack2D`; `SkyPack::FIRST_SLOT`, `set_procedural`; `TexturePack::init`,
`set_max_slots`, `active`, `take_new_images`.

**The emulator** (`runtime.h`, `inputs.h`, `ppc_rt.h`): `game_thread`,
`load_rom`, `g_crom`, `g_banked`, `g_vrom`, `g_data_dir`, `g_ram`,
`RAM_SIZE`, `g_sound`, `g_audio`, `g_eeprom`, `g_hold`, `g_quit`, `g_parked`,
`g_boot_skip`, `g_fast_idle`, `g_skip_attract`, `g_free_play`, `g_subtitles`,
`g_realtime`, `g_qpf`, `g_start_time`, `g_r3d_latest`, `g_r3d_mutex`,
`g_texture_ram`, `g_video_vram`, `g_video_frame`, `g_video_mutex`,
`enemy_marks`, `enemy_box`, `g_inputs`, `set_player_aim`, `player_join`,
`join_update`, `gun_reach`, `g_aim_reach`, `g_select_aim`,
`select_dot_on_screen`, `rd32`, `ram::P1_LIVES`, `ram::P2_LIVES`,
`disk_flush`.

**What `live_host.cpp` stands for** (declared by the game, defined by the
modes the engine leaves out): `frame_log`, `console_prompt`, `stats_frame`,
`stats_enter`, `stats_state_loaded`, `survival_enabled`,
`survival_owns_inputs`, `survival_frame`, `survival_enter`,
`survival_spawn_call`, `random_spawns_enter`, `random_spawns_frame`,
`g_boss_maker`, `boss_maker_frame`. A new function the runtime calls from
one of those modes needs a stand-in there too.

## Shared with the game's own window

Some rules are the game's and both hosts use them, so they cannot drift
apart: `enemy_box` (`runtime.h`, the aim assist's boxes) and
`select_dot_on_screen` (`inputs.h`, the stage select screen's gun). Others
are still written twice and are worth watching when they change:
`app.cpp`'s start of the boards and `live_start`; `frame_prep.cpp`'s
`hud_band` and `game_scene.h`'s `bands`; the game's `gamepad.cpp` and the
engine's light gun module.
