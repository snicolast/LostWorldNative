<p align="center">
  <img src="docs/img/marquee.png" alt="The Lost World - Arcade" width="100%">
</p>

<p align="center">
  <b>Sega's 1997 arcade light-gun game, running natively on PC.</b><br>
  Not an emulator: the original code, recompiled to C++, with a modern Vulkan renderer.
</p>

<p align="center">
  <img src="docs/img/hero.jpg" alt="Raptors in true widescreen with ray-traced sun shadows" width="100%">
</p>

## What you get

- **True widescreen** (16:9 or 21:9), not stretched. Any resolution.
- **Ray-traced sun shadows**, ambient occlusion, distance haze, bloom.
- **New weather**: dense rain, wet ground, lightning in the T-Rex storm.
- **Modern flashlight** in the night stages.
- **Sharp image**: MSAA or NVIDIA DLAA, HD text and HUD, texture packs.
- **Smooth motion** at 120 / 144 Hz and above.
- **Arcade sound**, with surround and headphone modes.
- **Mouse, gamepad (gyro too) or two mice** for 2 players.
- **New modes**: Boss Fight, Survival, achievements and stats.

<p align="center">
  <img src="docs/img/widescreen.jpg" alt="Arcade 4:3 next to native 16:9" width="100%">
</p>

<table>
  <tr>
    <td><img src="docs/img/menu.jpg" alt="Main menu"></td>
    <td><img src="docs/img/stage_select.jpg" alt="Stage select redrawn in HD"></td>
  </tr>
</table>

## ⚠️ ROM not included

You need your own copy of the arcade ROM. Nothing from it is in this repository.

Look for **`lostwsga.zip`**, the *Japan, Rev. A* set (Sega Model 3 Step 1.5).
Inside you should find files like `epr-19936.20` and `mpr-19918.1`. Other
versions of the game will not work.

## Install

There is no prebuilt exe: the game's code is recompiled from your own ROM on your PC.
You need Windows 10/11 (64-bit) and a Vulkan GPU.

1. Download the zip from [Releases](https://github.com/snicolast/LostWorldNative/releases) (or clone this repository).
2. Put `lostwsga.zip` next to `setup.bat`.
3. Double-click `setup.bat`. It installs [Python 3](https://www.python.org/downloads/) and
   [Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
   with winget if they are missing, then builds the game. It takes a few minutes and downloads
   SDL3 and NVIDIA DLSS on the first run.
4. Play with `LostWorld.exe`.

With Python and the Build Tools (*Desktop development with C++*) already installed,
`build.bat` does the build alone.

## Controls

| | Mouse | Gamepad |
|---|---|---|
| Aim | Move | Sticks or gyro |
| Fire | Left click | R2 / R1 |
| Reload | Right click | L2 / L1 |
| Coin + start | 5, then 1 | START |
| Pause | Esc | BACK |

F11 or Alt+Enter toggles fullscreen. Everything else is in **Options**.

## More

- [Full manual](docs/MANUAL.md): every option, the modes, texture packs, debugging.
- [How it works](docs/ARCHITECTURE.md): the PowerPC to C++ recompiler and the hardware.

## License

The code in this repository is under the [MIT License](LICENSE). The libraries and fonts in
`third_party/` keep their own licenses. The game itself (ROM, code and data) is not included
and stays the property of its owners: you need your own copy of `lostwsga.zip`.

<sub>A fan project for personal use, not affiliated with or endorsed by Sega.
The Lost World and Jurassic Park are trademarks of their respective owners.</sub>
