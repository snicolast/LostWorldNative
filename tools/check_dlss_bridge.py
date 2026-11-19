"""Checks that hub.toml gives launcher builds a DLSS bridge that fits this code.

Games built by NicoLauncher (llvm-mingw) reach DLSS only through
dlss_bridge.dll, built with MSVC in NicoLauncher-Hub (dlss/<game>/) and named
dlss_bridge-abi<N>-<commit>.dll. Without it, or with one of another ABI, DLSS
quietly falls back to another antialiasing, so this check stands in for a
player with an RTX card:

- error: hub.toml has no [[fetch]] for dlss_bridge.dll or nvngx_dlss.dll
- error: the DLL's ABI is not DLSS_BRIDGE_ABI in src/video/dlss_bridge.h
- warning: the bridge's code changed after the commit the DLL was built from
  (build a new one: NicoLauncher-Hub, Actions, "DLSS bridge")

    python tools/check_dlss_bridge.py
"""
import os
import re
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# What dlss_bridge.dll is built from (tools/dlss_bridge/CMakeLists.txt)
BRIDGE_CODE = ["src/video/dlss.cpp", "src/video/dlss.h", "src/video/dlss_bridge.cpp",
               "src/video/dlss_bridge.h", "tools/dlss_bridge"]
NAME = re.compile(r"/dlss_bridge-abi(\d+)-([0-9a-f]{12})\.dll$")


def main():
    errors, warnings = [], []
    fetches = tomllib.loads((ROOT / "hub.toml").read_text(encoding="utf-8")).get("fetch", [])
    by_dest = {f.get("to"): f for f in fetches}
    if "nvngx_dlss.dll" not in by_dest:
        errors.append('hub.toml: no [[fetch]] with to = "nvngx_dlss.dll"')
    bridge = by_dest.get("dlss_bridge.dll")
    header = (ROOT / "src/video/dlss_bridge.h").read_text(encoding="utf-8")
    abi = int(re.search(r"^#define DLSS_BRIDGE_ABI (\d+)", header, re.M).group(1))
    if bridge is None:
        errors.append('hub.toml: no [[fetch]] with to = "dlss_bridge.dll"')
    elif not (m := NAME.search(bridge["url"])):
        errors.append(f"hub.toml: {bridge['url']} is not named dlss_bridge-abi<N>-<commit>.dll")
    else:
        dll_abi, commit = int(m.group(1)), m.group(2)
        if dll_abi != abi:
            errors.append(f"hub.toml: the DLSS bridge is ABI {dll_abi}, this code is ABI {abi} "
                          "(src/video/dlss_bridge.h): the game would refuse it")
        known = subprocess.run(["git", "-C", str(ROOT), "cat-file", "-e", f"{commit}^{{commit}}"],
                               capture_output=True).returncode == 0
        if not known:
            warnings.append(f"the DLSS bridge was built from {commit}, which is not in this history: "
                            "cannot tell whether its code is the same")
        else:
            changed = subprocess.run(["git", "-C", str(ROOT), "log", "--format=%h %s", f"{commit}..HEAD",
                                      "--", *BRIDGE_CODE], capture_output=True, text=True, check=True).stdout.strip()
            if changed:
                warnings.append(f"the DLSS bridge's code changed after {commit}, which the DLL was built "
                                f"from: build a new one (NicoLauncher-Hub, Actions, \"DLSS bridge\").\n{changed}")
    ci = "GITHUB_ACTIONS" in os.environ
    for kind, items in (("warning", warnings), ("error", errors)):
        for text in items:
            print(f"::{kind}::{text}".replace("\n", "%0A") if ci else f"{kind}: {text}")
    if not errors:
        print(f"DLSS bridge in hub.toml: ABI {abi}, as this code")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
