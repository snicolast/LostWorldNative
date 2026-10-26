**The game is not included.** You need your own copy of the arcade ROM, `lostwsga.zip`
(*Japan, Rev. A*). Lost World Native recompiles the game's code from it on your PC, so
there is no prebuilt `LostWorld.exe` to download: that exe would contain Sega's code.

## Install (Windows 10/11, 64-bit, Vulkan GPU)

1. Download `LostWorldNative-*.zip` below and extract it.
2. Copy your `lostwsga.zip` into the extracted folder.
3. Double-click `setup.bat`. It installs Python 3 and Build Tools for Visual Studio 2022
   with winget if they are missing (about 3 GB, Windows asks for permission), then builds
   the game. It takes a few minutes the first time.
4. Play with `LostWorld.exe`.

See the [README](https://github.com/snicolast/LostWorldNative#readme) and the
[full manual](https://github.com/snicolast/LostWorldNative/blob/master/docs/MANUAL.md).
