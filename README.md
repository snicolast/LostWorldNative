<p align="center">
  <img src="docs/img/marquee.png" alt="The Lost World - Arcade" width="100%">
</p>

<p align="center">
  <b>Sega's 1997 arcade light-gun game, running natively on PC.</b><br>
  Not an emulator: the original code, recompiled to C++, with a modern Vulkan renderer.
</p>

<p align="center">
  <a href="https://www.youtube.com/@snicolast"><img src="https://img.shields.io/badge/YouTube-@snicolast-red?logo=youtube&logoColor=white" alt="YouTube: snicolast"></a>
  <a href="https://buymeacoffee.com/snicolast8u"><img src="https://img.shields.io/badge/Buy_me_a_coffee-snicolast8u-FFDD00?logo=buymeacoffee&logoColor=black" alt="Buy me a coffee"></a>
</p>

<p align="center">
  <img src="docs/img/hero.jpg" alt="Raptors in true widescreen with ray-traced sun shadows" width="100%">
</p>

## What you get

- **True widescreen** (16:9 or 21:9, in Options > Display > Widescreen), not stretched. Any resolution.
- **Ray-traced sun shadows**, ambient occlusion, distance haze, bloom.
- **New weather**: dense rain, wet ground, lightning in the T-Rex storm.
- **Modern flashlight** in the night stages.
- **Sharp image**: MSAA or NVIDIA DLAA, HD text and HUD, texture packs.
- **Smooth motion** at 120 / 144 Hz and above.
- **Arcade sound**, with surround and headphone modes.
- **Mouse, gamepad (gyro too) or two mice** for 2 players.
- **New modes**: Random Spawns (the arcade game shuffled), Boss Fight, Survival, achievements and stats.
- **NEW:** Ultra-widescreen support.

<p align="center">
  <img src="docs/img/widescreen.jpg" alt="Arcade 4:3 next to native 16:9" width="100%">
</p>

<table>
  <tr>
    <td><img src="docs/img/main_menu.jpg" alt="Main menu"></td>
    <td><img src="docs/img/stage_select.jpg" alt="Stage select redrawn in HD"></td>
  </tr>
</table>

## ⚠️ ROM not included

You need your own copy of the arcade ROM. Nothing from it is in this repository.

Look for **`lostwsga.zip`**, the *Japan, Rev. A* set (Sega Model 3 Step 1.5).
Inside you should find files like `epr-19936.20` and `mpr-19918.1`. Other
versions of the game will not work.

## Install

You need Windows 10/11 (64-bit) and a GPU with a Vulkan 1.3 driver, or a 1.2 one
that offers `VK_KHR_dynamic_rendering` (most cards and integrated graphics from 2016 on,
with current drivers; the console's first `[vulkan]` line says what yours offers).

1. Download this repository (green **Code** button > **Download ZIP**) and unzip it.
2. Put `lostwsga.zip` in the folder.
3. Double-click `install.bat`. It installs what the build needs with winget, Python 3 and the C++
   tools of Build Tools for Visual Studio 2022 (a few GB, accept the Windows prompt), then builds
   the game. It takes a while the first time; you can also drop `lostwsga.zip` on `install.bat`.
4. Play with `LostWorld.exe`.

<details>
<summary>Manual install</summary>

Install [Python 3.10 or later](https://www.python.org/downloads/) and
[Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
with the *Desktop development with C++* workload. The full workload is over 6 GB; the build only
needs the MSVC compiler, CMake and a Windows SDK. In a command prompt:

```
winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
```

Or in the Visual Studio Installer, choose *Individual components* instead of the workload and
tick *MSVC v143 - VS 2022 C++ x64/x86 build tools (Latest)*, *C++ CMake tools for Windows*
and one *Windows 11 SDK* (it works on Windows 10 too).

Then run `build.bat` with `lostwsga.zip` next to it. It downloads SDL3, miniz and NVIDIA DLSS on
the first run.
</details>

If the build can't download SDL3 or miniz (a proxy, an antivirus, a flaky connection), it lists
the links: download them with your browser, put them in `third_party\downloads\` keeping the file
name and run `build.bat` again.

### Update

Run `update.bat`. It downloads the latest version from GitHub (no git needed), replaces only the
files that changed and rebuilds `LostWorld.exe`. Your ROM, settings, records, crosshairs and any
textures of your own are kept; a repository file you edited yourself is copied to
`out\update_backup\` before it is replaced. `update.bat -Check` only says whether there is
something new. In a folder cloned with git it runs `git pull` instead.

The game also checks at launch: when there is news of a new version (`news.txt` on GitHub), the main
menu shows what is new with an **UPDATE NOW** button, which closes the game, runs `update.bat` and
starts the game again once it is rebuilt. Turn it off in OPTIONS > GAME > CHECK FOR UPDATES.

Got the game before the updater existed? Download the repository once more as a ZIP and copy
`update.bat` and `tools\update.ps1` into your folder.

## Controls

| | Mouse | Gamepad |
|---|---|---|
| Aim | Move | Sticks or gyro |
| Fire | Left click | R2 / R1 |
| Reload | Right click | L2 / L1 |
| Coin + start | 5, then 1 | START |
| Pause | Esc | BACK |

F11 or Alt+Enter toggles fullscreen. Everything else is in **Options**.

Light guns that Windows sees as a mouse (Sinden, Gun4IR and the like) work as is; for a Sinden, keep
its software running for the white border. The guns aim over the whole widescreen picture, side
bands included, and shooting off the picture reloads. **Options >
Game > Free play** lets 1 / 2 (a gun's start button) start and continue without a coin, and
**Controls > Hide idle crosshair** clears a crosshair whose gun has not been used for a few seconds.
**Controls > Gun test** shows every mouse and gun Windows sees and what each one does; fire player 1's
gun and then player 2's there to keep them as those players' guns from then on.

Want your own crosshair? Drop a `crosshair.png` (with transparency) next to `LostWorld.exe`, and
`crosshair_p2.png` for player 2 if you want a different one. It is centred on the gun, and its size
is in pixels of a 1080p screen (it scales with the window).

## More

- [Full manual](docs/MANUAL.md): every option, the modes, texture packs, debugging.
- [How it works](docs/ARCHITECTURE.md): the PowerPC to C++ recompiler and the hardware.
- [Other recompiled arcade games](https://github.com/snicolast/StarWarsTrilogyNative): Star Wars Trilogy
  Arcade, also in the main menu under **More recomps**.

## Support

If you enjoy the port and want to help with the time and tools behind it, you can buy me a coffee.
Entirely optional: it supports the work on this project, not the game, which belongs to Sega.

<a href="https://buymeacoffee.com/snicolast8u"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me a Coffee" height="50"></a>

## License

The code in this repository is under the [MIT License, with additional terms](LICENSE). The libraries and fonts in
`third_party/` keep their own licenses. The menu music (`res/menu/music.mp3`) was made
with Suno for this port. The game itself (ROM, code and data) is not included
and stays the property of its owners: you need your own copy of `lostwsga.zip`.

**No prebuilt downloads.** The license does not allow anyone to share a compiled build of this
port, a ROM, or anything made from one. I have never published any of them: everyone builds the
game themselves from their own `lostwsga.zip`. Any ready-to-play download you find elsewhere is
not from me, breaks the license, and is at your own risk.

<sub>A fan project for personal use, not affiliated with or endorsed by Sega.
The Lost World and Jurassic Park are trademarks of their respective owners.</sub>
