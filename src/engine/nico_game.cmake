# The Lost World's side for NicoEngine (docs/NICOENGINE.md): what the engine
# compiles of this game, read by the engine's CMakeLists.txt from a checkout
# of this repository. The game's own build never reads it, and nothing in
# this folder is needed to build or play the game.

# The version of the engine's game API this side keeps (NICO_GAME_API_VERSION
# in the engine's api/nico_game_api.h): the engine warns when it asks for a
# newer one
set(NICO_GAME_API 1)
set(NICO_GAME_ID lw)
set(NICO_GAME_BOARD model3)

# The Model 3 adapter, which reads the game's frames: the engine always takes
# the scene builder and the 2D of src/video (r3d_state, r3d_scene, r3d_screen,
# r3d_world, image_io, tilegen, sky_pack, and texture_pack, text_hd, pack2d);
# these are this game's own besides (the procedural skies)
set(NICO_GAME_ADAPTER_SOURCES sky_procedural.cpp)

# The game running inside the engine: the cabinet's game alone. The emulator
# of the board and its sound, from src/. The survival mode, the shuffled
# encounters, the weather, the records and the boss maker of the PC edition
# are not compiled: live_host.cpp stands for them. No music board, no Z80
set(NICO_GAME_LIVE_SOURCES
    runtime/runtime.cpp
    runtime/eeprom.cpp
    runtime/disk_writer.cpp
    runtime/inputs.cpp
    runtime/scsi53c810.cpp
    runtime/game_flow.cpp
    runtime/game_modes.cpp
    audio/audio_out.cpp
    audio/audio_stream.cpp
    audio/scsp.cpp
    audio/sound_board.cpp
    audio/weather_sound.cpp)
set(NICO_GAME_LIVE_C_SOURCES)       # (C files from the repository's root)
set(NICO_GAME_LIVE_INCLUDE)         # (folders from the repository's root)
set(NICO_GAME_MUSASHI third_party/musashi)   # the sound board's 68000

# This folder: what stands for the game's window inside the engine (compiled
# with the emulator) and what starts the game and gives it its controls
set(NICO_GAME_HOST live_host.cpp)
set(NICO_GAME_START live_game.cpp)

# The engine's control modules it is played with (NICO_MODULE_* in the API):
# the light guns (the engine's modules/light_guns.h)
set(NICO_GAME_MODULES gun)
