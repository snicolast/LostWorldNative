# Lost World Native: full manual

Everything about building, options, modes, texture packs and debugging. For the short version see the [README](../README.md).

## Build and test

Requirements: Python 3, Visual Studio 2022 Build Tools (MSVC + CMake).

```
build.bat
LostWorld.exe
```

`LostWorld.exe` opens a Vulkan window and runs the game at its real speed
(57.5 Hz). It starts in a PC-style main menu (Play, Options, Quit; with
keyboard or mouse): Play > Classic Arcade launches the arcade game (5 = coin,
1 = start). During play Esc pauses the game and the sound (Resume,
Settings, Quit). Options are saved in save/settings.ini; command-line
options take priority. `--no-menu` goes straight to the
arcade game as before (Esc quits), and the tests (--replay, --auto, --shot,
--aim, --frames, --headless) never show the menu.

Two players can play, as in the arcade: player 2 joins at any
time with 6 (coin) and 2 (start), and has their own crosshair (blue; player
1's is yellow). With a second mouse (or a light gun that Windows
sees as a mouse), one click with it while player 1 is playing makes it
player 2's gun and joins the game (if player 2 joined with start first, their
first shot does it, unless a gamepad is player 2's); from then on each mouse moves its own
crosshair and the pointer stays inside the window. The guns (mouse, light gun or gamepad) aim
over the whole 3D view, also the side bands of a 16:9 or 21:9 picture, and the enemies drawn
there can be shot (except on the stage select screen, which stays 4:3). A shot outside the 3D
view (the black bars of a window wider than it) or on its very edge (where a light gun parks an
off-screen shot) reloads, as on the cabinet. Disable it in Settings > Controls >
Second mouse or with `--one-mouse`. Settings > Game > FREE PLAY (`--free-play`) makes 1 and 2
insert the credit themselves, so a light gun's start button starts and continues without the
coin key; Settings > Controls > HIDE IDLE CROSSHAIR (`--crosshair-hide N`) hides a player's
crosshair after 3, 5 or 10 seconds without moving or firing, until the gun moves again (a
player 2 who joined by mistake leaves no crosshair standing on the screen). A `crosshair.png`
next to LostWorld.exe replaces the drawn crosshairs with that picture, centred on the gun and sized
as pixels of a 1080-line screen; `crosshair_p2.png` gives player 2 their own (otherwise they use
`crosshair.png` too). In `--auto`, a 2 at the end of a button belongs to player
2 (`coin2@F`, `start2@F`, `trigger2@F`) and `aim2:X/Y@F` sets their gun's raw ADC values;
`point:X/Y@F` (`point2:` for player 2) aims as the mouse would, X and Y as fractions of the
4:3 screen (below 0 or above 1 in the side bands).
F11 or Alt+Enter toggles fullscreen.

With a gamepad (Xbox, DualSense, DualShock 4, Switch Pro and the others SDL3
knows): both sticks move the crosshair (soft curve near the centre for fine aim and
more speed after a moment at full tilt), R2 / R1 / bottom button fire and
L2 / L1 / side buttons reload. START inserts a credit and starts (or pauses
if already playing), BACK / Share / View pauses and L3 / R3 centre the crosshair. On
gamepads with a gyroscope you can also aim by moving the gamepad; it calibrates itself
when the gamepad is held still for a moment. It rumbles when firing and when taking a
hit. Menus are driven with the D-pad or the left stick, A / B (as
printed on a Nintendo gamepad) and L1 / R1 for tabs. Settings >
CONTROLS has the stick speed, the gyroscope and its speed, and the
rumble strength. The first gamepad, even one plugged in mid-game, shares
player 1 with the mouse (whichever moved last aims; START also hands it the
sight); a second gamepad is player 2. SDL3 is downloaded automatically
the first time you build.

Play > Boss Fight is the boss mode: a list of the four bosses (female
T-Rex, Deinosuchus, Carnotaurus and the final T-Rex), each starting
directly at its fight, with the score at zero. When the fight ends (the
boss falls or the game is over) it returns to the main menu; the pause menu has
Retry Fight and Boss List. Each fight starts from a saved game state in
save/boss/bossN.state. The first time the game is opened it prepares them
in the background, one at a time, or all at once if the list is opened
(`LostWorld.exe --make-boss-state N` plays without a window up to boss N and
saves the state; its log is kept in save/boss/bossN.log). If a new version changes the format, they are rebuilt.
RECORDS in the main menu shows the achievements (ACHIEVEMENTS) and the
statistics (STATS): games, time played, shots, hits and accuracy,
technique shots, dinosaurs killed, lives lost, continues, best
scores per route, and best time per stage and per boss. They are read from
the same counters the game uses for its results screen and are
saved in save/stats.ini and save/achievements.ini; when an achievement is unlocked a
notification appears in the bottom right. Tests (--auto, --headless, LW_SURV_GOD...)
don't count.
`LostWorld.exe --headless --frames 1800` runs it without a window, at full
speed, and prints the diagnostic report.
`LostWorld.exe --shot 1400` saves frame 1400 to out/capture.bmp and exits.
`LostWorld.exe --help` lists all command-line options.

`build.bat` extracts the ROM (the path you pass it, the one in the
`LW_ROM_ZIP` variable, or `lostwsga.zip` next to `build.bat`), recompiles the PowerPC code
to C++ and compiles it with MSVC.

At startup `LostWorld.exe` loads the ROM from the first thing it finds:
`--rom PATH\lostwsga.zip` or the `LW_ROM_ZIP` variable (it reads the zip directly,
without extracting it), `--data FOLDER`, `data\` in the current folder or next to the exe
(what `build.bat` extracts), and `lostwsga.zip` next to the exe or in the current
folder. If none is found, a message says where it looked and how to give it the path.

Finally it runs the tests in `tests/` (ctest): 2D composition against reference
hashes, the C++ and Python FNV-1a hash against the same vectors, the
audio queue (click-free resampling with mismatched clocks), the encounter
script reader shared by survival and random spawns, and the sound chips' part
of the game states.
`-DLW_TESTS=OFF` in CMake disables them; `tilegen_test tests\tilegen_expected.txt
--write` regenerates the reference after an intentional change to the composition.

If the report says a non-recompiled function was called, it is recorded in
`out/missing.txt` and the next run of `build.bat` includes it.

`data/`, `build/`, `out/` and `save/` contain material derived from the ROM or
saved games and are not versioned.

## Status

- Part 1 (done): PowerPC to C++ recompiler, boot, EEPROM, vblank and
  decrementer interrupts.
- Part 2 (done): 53C810 DMA, PCI space, Real3D synchronization,
  full CROM map and inputs. The game enters its main loop
  (about 30 logic cycles per second, like the original) and runs about 5 times
  faster than real time without graphics. On exit it dumps the Real3D
  memory, the texture FIFO and the 2D layer to out/real3d/.
- Part 3 (done): Vulkan window (headers, volk and glslang included, no SDK)
  and 2D layer.
- Part 4 (basics done): Real3D 3D engine on Vulkan: scene
  tree, polygons, textures (FIFO and VROM port), lighting, fog,
  transparency and 2D layer with priority. Playable with keyboard and mouse:
  5 = coin, 1 = start, mouse = gun (left click fires).
- Part 5 (done): sound (68000 + 2 SCSP with DSP) via WASAPI, calibrated mouse
  gun and its own crosshair (C key). The output resamples with a
  sinc filter to the sound card's rate and adjusts the speed (up to 0.5%)
  so the game clock and the card clock don't end up draining or
  filling the queue; gaps (pause) fade instead of cutting, and if
  the Windows output changes (e.g. headphones) the sound moves to it.
- Part 6 (in progress): modernization. Done: true widescreen, MSAA antialiasing,
  mipmapped textures with anisotropic filtering, scenes without black bars,
  xBR-scaled HUD, texture packs, interpolation to the
  monitor's refresh rate and NVIDIA DLAA. Next: VR.

## Video and sound options

- --aspect 16:9 (default), 21:9 or 4:3 (or WIDESCREEN in the settings,
  DISPLAY tab, applied at once): maximum width of the 3D view. WIDESCREEN
  21:9 allows up to 2.4:1, so 2560x1080 and 3440x1440 monitors are filled
  edge to edge. The sky (background 2D layers) is also drawn at the sides; the HUD stays in
  the central 4:3 with --hud-centre. The guns aim over the whole 3D view:
  the game's ADC values only reach a little beyond the 4:3 screen, so in a
  wider view the runtime gives the game the aim itself (rt_gun_aimed, in
  fn_0000CBF0), where its X is drawn and its shots land.
- --msaa N (default 4; 1 = off): edge antialiasing, including the
  cut-out edges of plants.
- --aa dlaa: NVIDIA DLAA (DLSS at native resolution) instead of MSAA. The 3D is
  drawn without MSAA with a different subpixel jitter each frame, together
  with its depth and a per-pixel motion vector, and DLSS turns it into
  a clean image of the same size. Needs an RTX GPU and nvngx_dlss.dll
  next to LostWorld.exe (build.bat downloads the official NVIDIA DLSS SDK with
  tools/get_dlss.bat the first time and copies the DLL); if anything is missing, MSAA is used. --aa none disables
  antialiasing.
- --aniso N (default 8; 0 = original look without mipmaps): texture
  filtering with the game's own mipmaps and N anisotropic samples.
- --scale F: internal 3D resolution (2 = 2x2 supersampling; 0.75 = 75%, faster).
  In the settings: RENDER RESOLUTION (50-100%, DISPLAY tab), applied at once.
- --half-rate (or FRAME RATE: HALF in the settings): on monitors above 100 Hz
  the 3D is drawn every other refresh (72 images per second at 144 Hz) and the
  refresh in between shows the same picture with only the crosshairs redrawn.
  About half the GPU work, with the motion still even.
- --no-vsync (or VSYNC: OFF in the settings, DISPLAY tab): each image goes out
  as soon as it is drawn instead of at the monitor's next refresh (Vulkan
  immediate mode, or mailbox if the driver has no immediate mode), still no
  more images than the monitor's refreshes. About 14 ms less latency (measured
  at 120 Hz), but tearing and less even motion on monitors without
  G-Sync/FreeSync: keep it on there. FRAME RATE: HALF does nothing with vsync
  off. The console prints `[video] vsync on/off` when it changes.
- QUALITY (settings, DISPLAY tab): LOW, MEDIUM, HIGH and ULTRA set together the
  options that cost the most (render resolution, frame rate, ray tracing,
  ambient occlusion, sun shadows, light shafts, soft particles, sky, surface
  detail, bloom); CUSTOM when they are mixed. On the first start (no
  save/settings.ini) a level is picked from the graphics card's name: an RTX
  3060 laptop gets MEDIUM, a 4070 HIGH, a 5070 Ti ULTRA. Measured on an RTX
  5070 Ti at 1440p, the most expensive effects are the ambient occlusion, the
  render resolution, the procedural sky and the sun shadows.
- --letterbox: keeps the black bars in scenes (removed by default).
- --boot: shows the boot screens (WARNING notice and copyright). By
  default they run at full speed with a black screen (about 2 s instead of
  15) and the game appears at the SEGA logo, with the same state and sound.
- --pixel2d: HUD and 2D layers with square pixels, without xBR scaling.
- --classic-text: letters and numbers in the original pixel art (scaled like the
  rest of the 2D). By default, if textures/text/ exists, the menus, INSERT
  COIN/CREDITS and the score are drawn with sharp vector fonts.
- --subtitles off|en|jp (or SUBTITLES in the settings): subtitles for what
  the characters say. Off by default; in English they are the ones from the export
  version, which the ROM also contains, and in Japanese the originals of this
  board. The CONTINUE hints appear in Japanese only with jp.
- --hud-centre: keeps the score and the ammo in the central 4:3 (by default they go
  to the edges of the widescreen).
- --no-interp (or MOTION: ORIGINAL in the settings, DISPLAY tab): shows
  the game's frames as they are. By default (MOTION: SMOOTH) the 3D is also drawn
  between game frames, at the monitor's refresh rate (60, 120, 144
  Hz...): each vertex is interpolated between its previous and current position, at
  the cost of some latency (see below). Each frame is timestamped with its vblank,
  not with the moment the CPU finishes it (which varies by a few ms
  from one frame to the next); each image is computed for the monitor vblank on
  which it will be shown (Windows provides it), not for when it is prepared, and always lags
  by the delay with which frames arrive, so motion advances at a
  steady rate. Each game frame's 3D scene and 2D layers are prepared on a
  thread of their own as soon as the game finishes it (the two side by side,
  ~7 ms), so the window thread only draws and has a new image for every
  refresh, also at 120 Hz and above (when it built them itself, ~14 ms, it
  missed refreshes at 120 Hz and the motion stuttered). With
  VK_KHR_present_wait (recent NVIDIA, AMD and Intel drivers) at most two
  images wait for the screen, three for a while if refreshes are being
  missed: what is drawn goes out two refreshes later (~17 ms at 120 Hz,
  ~32 ms with the whole swapchain), and that is the delay of the mouse
  crosshair, drawn at the last moment. In total the 3D image on screen is
  about 50-60 ms behind the game at 120 Hz.
  The 2D layers wait for the 3D of the same vblank, and the sky and backgrounds (the
  2D layers behind the 3D) are also drawn between frames, so the
  background turns together with the scenery instead of jumping 57.5 times per
  second. The game scrolls them in whole pixels (4 pixels of a 1440p screen),
  so each row's scroll is first reconstructed at sub-pixel precision from the
  last frames (src/video/sky_motion.h) and then goes on by the world camera's
  turn in the frame, when that agrees with the scroll seen. It goes on ahead
  rather than back because the 2D layers of a vblank
  are what the game wrote in the frame before (their sky scroll is that of the
  previous 3D frame's camera: drawn as they are, the sky trailed a frame behind
  the scenery while the camera turned). The distance
  haze, the sky's sun, its rays and the shadows also follow
  the camera between frames (except on camera cuts).
  Windows reports the vblanks of one monitor (the main one): when the
  window is on a monitor with another refresh rate, the refreshes are timed
  from the presents instead; and when the images do not go out one per
  refresh (vsync turned off in the driver, G-Sync/FreeSync below the
  monitor's rate, refreshes missed), each image's moment is when it is
  prepared, since it goes out about as soon as it is ready (the log says
  which, `[video] display clock`). If the motion stutters: keep vsync on (*Vertical sync:
  Use the 3D application setting* in the NVIDIA Control Panel), check that
  the window title shows the monitor's refresh rate (fewer Hz than the
  monitor without G-Sync/FreeSync means the GPU misses refreshes: lower
  the resolution, the edge smoothing or the ray tracing, or set the monitor
  to 60 Hz), and send a log written with LW_FRAMELOG (below).
  `LW_PACING=1` measures it: every 5 s it prints the speed of on-screen motion
  relative to real time (1.000 = steady), its spread between
  presents and how many stood still or went backwards, compared with
  the previous method.
- --haze [N] / --no-haze: distance haze (new effect, on by
  default). Distant scenery gradually
  takes on the horizon colour and loses saturation (aerial perspective), and the
  models the game adds in the distance as you advance fade in from
  the 2D background over half a second instead of popping in. N is the intensity
  (default 1; --no-haze removes it).
- --rt off|shadows|ao (RAY TRACING in the settings, LIGHTING tab: OFF,
  SHADOWS, SHADOWS + AO; --no-rt is --rt off): on a GPU with ray tracing
  (Vulkan ray queries: NVIDIA RTX, AMD RX 6000 or newer) the sun shadows and
  the ambient occlusion are traced by default. Every image, the opaque game
  world (a few thousand triangles, already in view space and interpolated
  like the picture) goes into acceleration structures rebuilt from scratch:
  what stands near the ground, which casts the sun shadows as with the shadow
  map, and what is higher up (canopies, roofs), which only the occlusion sees.
  The 3D pass sends a few rays from each sunlit point towards the sun's disc:
  no range or resolution limit, shadows sharp where they touch the ground and
  softer farther away, and the cut-out foliage casts its leaves (the shader
  reads the texture where a ray crosses it). The ambient occlusion sends six
  short rays over the hemisphere above each pixel instead of reading the
  depth buffer, so it also sees what is off screen or hidden behind
  something. With shadows only, the occlusion comes from the depth; without
  ray tracing the option shows N/A and both work as before.
- --saturation F (default 0.8) and --gamma F (default 1): colour saturation (0 = grey, 1 = original) and
  gamma (higher = lighter midtones) on the final image; also in
  the options menu (COLOR SATURATION, GAMMA).
- --volume V (default 5): volume.
- --speakers stereo|surround|headphones (AUDIO tab, SPEAKERS): the
  arcade cabinet has front speakers (master SCSP) and rear speakers (slave
  SCSP). In stereo they are mixed; in surround each pair goes to its own
  speakers (4.0, 5.1 or 7.1; with 5.1 to the sides if there are no rears); on
  headphones they are mixed with crossfeed and the rears sound duller and
  wider, as if from behind.
- --rear-boost N (AUDIO tab, REAR LEVEL; default 0 DB): dB on the rear
  speakers (slave SCSP), from -6 (quieter) to +12 in steps of 3. In the arcade they are in the
  seat, next to the head, and they carry the T-Rex roars and many effects.
  The T-Rex roars and the rest of the mix play at their original level.
- --linear-samples (AUDIO tab, SAMPLE QUALITY = ORIGINAL): linear
  sample interpolation like the original chip. Default is cubic
  (SMOOTH), with less harshness in low sounds and voices.

## Random spawns

PLAY > CLASSIC ARCADE opens ARCADE (the original game) and RANDOM SPAWNS:
the arcade game, every stage and stop the same, with its encounters
shuffled differently every game. Raptors come out of other spawn points
of the same zone (only ones the camera is already looking towards) and
sometimes as another raptor type of that zone, the river enemies of stage
2 come out a little to the side, every enemy comes a little sooner or later,
and runs of plain stops (camera turn, dinosaurs, wait for them) can come in
another order. Each boss starts with 70 % to 150 % of its arcade life.
Also from the command line: `--random-spawns`.

It changes the encounter scripts in place without adding or removing
anything, so the camera walks the stage path as in the arcade. The
opening of stage 2 and the bosses' own attacks stay as they are. The
Carnotaurus and the T-Rex only get the life of their first phase changed.

### Weather

Picking RANDOM SPAWNS, or a SURVIVAL arena, opens a WEATHER screen for
that run: RANDOM (the default: a new one at the start of every stage, and
of every survival run), SUNNY, CLOUDY (soft light, grey sky, a light
haze), FOG, RAIN, STORM (dark, heavy rain and lightning) or ORIGINAL (the
game's own). The last one picked comes up selected next time
(`run_weather` in settings.ini). CLASSIC ARCADE > ARCADE always keeps the
original weather. It is laid over the game's own light, only outdoors in
daylight (caves, corridors and the night keep theirs), and changes over a
few seconds. The rain and the lightning follow the RAIN and LIGHTNING
settings. `--weather sunny|cloudy|fog|rain|storm` forces one in every game
(tests, screenshots).

## Survival mode

In the menu, PLAY > SURVIVAL shows the working arenas (stage 1: the
jungle cabin and the dark forest; stage 3: the visitor centre lobby
and the service corridor; stage 4: the control room) with the record
for each. Also from the command line: `LostWorld.exe --survival
STAGE:ZONE` (for example `--survival 3:0`).

Arenas are locked until you reach them in the arcade game: when you get
there, ARENA UNLOCKED rises from the bottom of the screen and the game keeps
that moment in save/survival/. Survival then starts the arena straight
from it. An arena with a
record counts as unlocked, and `--survival` works on locked ones too.

Without a kept state (`--survival` on an arena not reached yet), the game reaches the arena by itself, at full speed and silently, behind a
loading screen that explains the mode, shows the record and says what is happening
(coin, route, trip to the arena) with a progress bar. The game
waits at the shooting point until the first shot (or Enter). Then waves
arrive; at the top you see the lap, the wave and the remaining dinosaurs. The
game leads the player through the zone as in the arcade: after clearing a
stop, a few seconds later the camera keeps walking along the stage's original
path (turning, climbing stairs and at the usual height, without
passing through anything) to the next stop, where that stop's wave arrives.
Every wave is different: batches of dinosaurs from random groups of the
zone's spawn points (the camera turns towards them first), with mixed raptor
types and random pauses, and at least as long as the first one. Every wave,
the first one too, starts with a 5 second countdown in position, with the
cursor free to aim.
Sometimes an ambush interrupts the walk (AMBUSH!: the walk resumes once it is
cleared) and sometimes a golden BONUS target appears for a few seconds: shooting it gives an
extra life (or 5000 points with full hearts). After the zone's last stop
the image fades to black and returns to the first for another lap (LAP),
harder: more dinosaurs per wave, more at once and shorter pauses (only in
RAM; the EEPROM is not touched). There is no CONTINUE: the hit that would take the last
life ends the hunt (the attack is shown first) with a screen showing the lap and
wave reached, score and record (save/survival.ini, per stage and zone);
a shot starts again from the beginning of the arena and Esc returns to the
main menu (the arcade game stays as it was). Esc during play opens
the pause menu as usual.

It uses the game's encounter machine (see game_ram.md). Stages 2 and 5
use a different system, and zone 7 of stage 1 (the T-Rex chase) reloads
the zone endlessly.

## Texture pack

`LostWorld.exe --dump-textures` saves each new texture that appears on
screen as out/textures/<hash>.png (the name identifies its content, so
the same texture always has the same name). To replace it, copy the
PNG to the textures/ folder with the same name and edit it: it can be any
resolution (for example 4x upscaled with an AI) and have its own alpha channel. At
startup, the game uses those in textures/ with trilinear and anisotropic filtering.
`python tools/texture_sheet.py` makes a sheet with all the dumped ones, numbered.
The repository ships with the replacements made so far (HD 2D, skies, text font).

2D graphics (for example the route map) are replaced separately: F12 during
play (or --shot N) saves the 2D layer to out/2d/capture_<frame>.png, and
`python tools/make_2d_replacement.py out/2d/capture_<frame> X Y W H name`
creates textures/2d/<name>.txt and a 4x template in out/2d/. The new drawing
goes in textures/2d/<name>.png. Whatever the game changes on top (the route
travelled) still appears over the drawing, and `--keep X Y W H` marks an area
where the game always shows through (the help texts). The replacement is recognized at
any position on the screen and during fades.

Text (test menus, notices, INSERT COIN, CREDITS, FREE PLAY and the
score digits) is redrawn with free fonts (SIL OFL, in
third_party/fonts): `python tools/make_text_atlas.py [out/snap/<snapshot>.bin]`
creates textures/text/atlas.png and glyphs.txt from the font patterns
the game loads into 2D memory. Each tile is recognized by its pattern
number and its content, and the colours come from the game's palette every
frame (fades and highlighted lines included).

Each stage's titles (STAGE N and the name below the map) are redrawn
with the Jurassic Park font as 2D replacements: with a capture of that screen,
`python tools/make_title.py out/2d/capture_<frame> X Y W H "STAGE1" stage1_title`
creates textures/2d/<name>.txt and .png with the original's colours (body,
inner line and shadow). The font has no numbers: the digits come from Saira
ExtraCondensed. Several replacements are shown at once (the map and both titles).
Jurassic_Park.ttf goes in third_party/fonts but is not versioned (it is free only
for personal use); without it the main menu uses Saira.

Screens drawn on the background 2D layers (STAGE SELECT) are rebuilt
on top of the backgrounds F12 saves in out/sky: `python tools/make_backdrop.py
tools/backdrops/stage_select.txt out/sky/<hash>.png` upscales them 4x, erases
each text in the list and rewrites it with its font, and saves
textures/sky/<hash>.png. With `--variants` it also creates the versions with
a different difficulty bar highlighted (each has its own hash).
A `keep X Y W H` line leaves that box of the backdrop to the game: it is
left out of the hash and the game's own pixels show there (CREDITS and
INSERT COIN under the chosen difficulty, which change with the credits);
make_backdrop.py writes it to textures/sky/<hash>.txt next to the picture.

Texts on the background layers that change piece by piece (the
tutorial screen: the phrase, INSERT MORE…) are replaced by area, like the front ones:
F12 also saves capture_<frame>_back (and _back_layer_a, only the text
layer) and `make_backdrop.py ... out/2d/capture_<frame>_back --pack2d X Y W H
name` writes textures/2d_back/<name>.txt and .png. What is behind the
text (the wooden logo) is still the game's.

The sky and the other backgrounds behind the 3D (512x512 2D layers that
wrap when scrolling) are replaced just like textures: F12 during
play (or --dump-textures, which saves each new background) saves them to
out/sky/<hash>.png. Copy the PNG to textures/sky/ with the same name and
edit it: preferably square and larger (2048x2048 is 4 times the detail) and without
transparency. It is drawn at full resolution following the game's
scrolling, with its fades, and the distance haze takes the colours of the new
sky. When wrapping, the left edge continues into the right one and the top into
the bottom. The stages' sky is 7d020e12272dd059.png.

By default the stages' sky is procedural (SKY setting in EFFECTS, or
--sky procedural|original): a gradient with soft clouds drifting
slowly, drawn at screen resolution and in perspective, with the
same scrolling, fades and fog as the original, and with the colours
of each stage's sky (it works for all the game's skies); the trees on the
horizon come from the original image. With ORIGINAL the game's image is used
(or its replacement from textures/sky).

Where the game has rain (the T-Rex forest: a few dozen white
streaks), the RAIN setting in ATMOSPHERE (--rain off|light|heavy, HEAVY by
default) replaces it with dense rain: thin drops anchored to the world in three
distance layers, slanted by the wind (the direction of the game's
streaks) and by the camera's motion, occluded by whatever is in front of them and
lit like the fog around them and by the lightning. The world gets wet
with it: darker surfaces with a sheen that reflects the fog and the
lightning, puddles on the ground, ripples and splashes where the drops land
(also on whatever faces upward, like the T-Rex's back). The wind
blows in gusts: it slants the rain, makes it fall in denser sheets that
sweep past, drags the fog banks and moves lighter and
darker bands through the distant fog.

Lightning (LIGHTNING), every 5 or 6 seconds on average where the game rains or
in its grey storm fogs, shows its branched bolt through the fog and behind the trees while
it flickers; its light comes from where it strikes, so whatever faces that way is
lit and the rest is left in silhouette, and the wet ground reflects it.

## Debugging

- F9 during play saves a snapshot of the scene to out/snap/
  (3D memory, textures and 2D layer); `LostWorld.exe --replay out/snap/snap_N.bin`
  shows it again without running the game.
- LW_FILM=F,N (environment variable) records N game frames starting at
  F as snapshots in out/film/; `LostWorld.exe --replay out/film`
  plays them back in order and saves each image to out/film_out/, to compare
  effects that depend on camera motion.

- --trace A,B,C counts calls to those functions and shows the registers
  of the first three.
- out/ram.bin saves the RAM on exit.
- --diff FN@FRAME saves the state before and after a function and
  tools/diff_unicorn.py re-runs it in Unicorn to compare (the tools'
  packages: `pip install -r tools/requirements.txt`).
- --shot N saves frame N (3D + 2D) to out/capture.bmp and the scene to
  out/scene.txt; --auto coin@F,start@F
  presses buttons on a schedule.
- --trace-survival writes to out/survival_trace.txt, frame by frame, the stage,
  the zone, the camera, the tasks that are created and die, the zone's
  triggers and the encounter machine. With LW_SURV_GOD=1 the player doesn't lose lives
  (only together with --trace-survival); LW_SURV_RAM=FROM,TO,EVERY saves the RAM.
  `python tools/encounters.py` lists the ROM's encounter scripts.
- LW_SHOTS=N[,FROM[,TO]] saves one frame out of every N to out/shots/, and
  LW_FAST=1 runs the window at full speed (for recording long tests).

Debug environment variables (read at startup; none of them changes
normal behaviour when unset):

| Variable | Effect |
|---|---|
| LW_ROM_ZIP=path | Path to lostwsga.zip, for build.bat and for LostWorld.exe (like --rom) |
| LW_NOSOUND | Doesn't start the sound board (automated tests, multiple instances) |
| LW_FAST | The window runs at full speed, without waiting for vsync or the game's pace |
| LW_FILM=F,N | Records N frames from F as snapshots in out/film/ |
| LW_SHOTS=N[,FROM[,TO]] | Saves one frame out of every N to out/shots/ |
| LW_INTERP=0.5 | Fixes the interpolation between the previous and the latest frame (0 to 1) |
| LW_PACING=1 | Every 5 s, how steadily the interpolated motion advances (speed, spread, still or backward presents), with the current and the previous method |
| LW_FRAMELOG=path | Writes a line per present (timings, frames shown) and per game frame, and when each image reached the screen (VK_KHR_present_wait); `python tools/frame_log.py path` reports missed refreshes, uneven motion and latency |
| LW_SKYLOG=path | Writes the back layers' scroll of each 2D frame, the world camera of each 3D frame and, per present, the pairing and the sub-pixel position drawn; `python tools/sky_log.py path` compares how the sky scrolls with how the camera turns (pixels per radian, how many frames apart) and how smoothly the sky is drawn between frames |
| LW_QUEUE=N | At most N images waiting for the screen (2 by default; 0 = the whole swapchain) |
| LW_MENU_KEYS=40,13,27 | Presses those keys in the menu (one every 10 presents; `cX/Y` clicks pixel X,Y), saves out/menu.bmp and exits |
| LW_MENU_SHOT=path.bmp | Saves the menu image each time it is redrawn |
| LW_MENU_PAD=1 | The menus show the gamepad's buttons in the key help, as if a gamepad drove them |
| LW_NOFOG | Removes fog from all viewports |
| LW_NOLOD | Draws all levels of detail, including those the game would discard by distance |
| LW_FOGDBG | Prints per frame the camera, the ground and the models matched by the world tracking |
| LW_FOGSUN=x,y,z | Forces the sun direction (view space) for the sun disc and shadows |
| LW_RTSPLIT | Ray traced shadows and occlusion on the left half of the picture, the shadow map and the depth based occlusion on the right, to compare them |
| LW_GPUTIME | Every 300 images, the GPU time of the whole frame and of each pass (ray tracing build, shadow map, 3D opaque, ambient occlusion, rest of the 3D, DLAA, bloom, light shafts, diffusion, exposure, composition) |
| LW_MIST=rise,banks | Sets the two distance haze parameters |
| LW_FLASH=brightness | Fixes the grey-fog lightning brightness and shows its bolt |
| LW_RAIN=amount | Fixes the rain amount (0 to 1) even if the game isn't raining |
| LW_RAINDBG | Prints per frame the game's rain streaks, their direction and the camera |
| LW_BOSS=N | Starts the boss mode fight with boss N (1-4) directly |
| LW_BOSS_LIFE=PCT | Random spawns: every boss gets PCT % of its life instead of a random share |
| LW_SEED=N | Survival, random spawns and random weather: the same random numbers every run (N instead of the clock) |
| LW_HIT_PROBE=file | Writes RAM 0x100000-0x200000 to the file after every scoring hit and every 120 frames (to find what hits change) |
| LW_SNDDBG | Sound board trace (68000 and SCSP) |
| LW_SNDLOG | Each SCSP note to out/voices.csv (registers, duration, direct and effects peaks) and the per-second level of the direct mix and effects return to out/mix.csv |
| LW_SURV_TRACE=path | File for the --trace-survival trace (default out/survival_trace.txt) |
| LW_SURV_LIFE=N | Sets the player's starting life (with --trace-survival) |
| LW_SURV_GOD=1 | The player doesn't lose lives (with --trace-survival) |
| LW_SURV_RAM=FROM,TO,EVERY | Saves the RAM for those frames to out/survival_ram.bin (with --trace-survival) |
| LW_SURV_AUTOSTART=1 | Survival: starts and replays by itself (also without a window) |
| LW_SURV_AUTOPLAY=1 | With --trace-survival: fires automatically all over the screen (to go through stages and see their encounters in the trace) |
| LW_SURV_SHOTS=1 | Survival: saves each screen of the mode to out/survival_<mode>.bmp and fires by itself (=2: returns to the menu after the results) |
| LW_SURV_AMBUSH=1 | Survival: an ambush on every walk between stops |
| LW_SURV_BONUS=1 | Survival: the BONUS target appears on every walk and is shot automatically after a second |
