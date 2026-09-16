ETHEREAL v0.1 — Standalone Roblox cinematic post-processing
=============================================================

NO OBS REQUIRED.

WHAT THIS VERSION DOES
----------------------
Ethereal finds RobloxPlayerBeta.exe automatically, captures ONLY the Roblox window
with Windows Graphics Capture, applies the effect on the GPU with Direct3D 11,
then draws the processed image in a topmost click-through window over Roblox.

Roblox itself is not modified.

No:
- DLL injection
- Roblox file changes
- memory reading
- depth-buffer hooks
- anti-cheat bypassing

CURRENT EFFECTS
---------------
- dreamy soft diffusion
- bloom / highlight glow
- screen-space sun + window light shafts
- atmospheric horizon haze
- approximate distant fog
- richer existing shadows
- filmic tone mapping
- warm/cool cinematic grading
- subtle vignette
- Ethereal Night preset with lightweight procedural stars / Milky Way-like dust

PRESETS
-------
Press F8 to cycle:

1. Dreamy Day
2. Golden Dusk
3. Misty Dawn
4. Ethereal Night

HOTKEYS
-------
F8      Change preset
F9      Toggle Ethereal effect on/off
+       Increase effect strength
-       Decrease effect strength
Esc     Close Ethereal

The overlay uses WS_EX_TRANSPARENT + WS_EX_NOACTIVATE, so clicks and keyboard input
continue going to Roblox underneath.

REQUIREMENTS
------------
- Windows 10 version 1903+ or Windows 11
- DirectX 11-capable GPU
- Visual Studio 2022 Community
- Windows 10/11 SDK
- CMake support (Visual Studio installer can add this)

RTX 3050 is more than enough for this first prototype at 1080p/60 in most cases.

BUILD — EASY WAY
----------------
1. Install Visual Studio 2022 Community.
2. In the installer enable:
      Desktop development with C++
      Windows 10/11 SDK
      C++ CMake tools for Windows
3. Double-click BUILD.bat.
4. When it finishes, double-click RUN.bat.

The executable will be:
    build\Release\Ethereal.exe

HOW TO USE
----------
1. Open Roblox.
2. Enter the game you want to film.
3. Keep Roblox visible.
4. Start Ethereal.exe.
5. Ethereal auto-detects Roblox and places itself over it.
6. Play normally.
7. Press F8 until the lighting preset fits the scene.
8. Record your screen with NVIDIA App / ShadowPlay or Xbox Game Bar.

RECORDING
---------
Because Ethereal is a real topmost Windows output, a screen/display recording can capture
the processed result.

For NVIDIA App / ShadowPlay:
- use desktop/display capture if game capture only sees raw Roblox
- 1080p60 is a good target
- use HEVC/H.265 if your editing workflow supports it

For Xbox Game Bar:
- behavior varies by Windows build/app capture mode
- if it captures only Roblox and misses the overlay, use NVIDIA display capture instead

PERFORMANCE / FAN NOISE
-----------------------
Recommended for RTX 3050:
- Roblox: cap at 60 FPS while filming
- 1920x1080
- close browsers / heavy background apps
- NVIDIA power mode: Normal / Optimal, not forced Maximum Performance
- start with effect strength 1.0

The prototype currently uses one fairly compact full-screen shader.
It does NOT run AI depth estimation or CPU per-pixel image processing.

WHY THE FOG / SHADOWS ARE APPROXIMATE
-------------------------------------
Ethereal intentionally does not access Roblox's depth buffer or geometry.

That means it cannot truly know:
- exact object distance
- where every wall blocks sunlight
- true 3D shadow positions

Instead it makes cinematic screen-space approximations based on brightness,
local contrast, screen position, and scene color.

This keeps the app independent from Roblox and lightweight.

KNOWN v0.1 LIMITATIONS
----------------------
- Window rays use a fixed screen direction for now.
- Distant haze is heuristic rather than true depth fog.
- The night sky mask can occasionally place stars over dark upper-screen objects.
- Roblox resize / display-mode changes may need restarting Ethereal in some cases.
- HDR displays are not specially handled yet.
- This source package has not been compiled inside ChatGPT's Linux environment;
  the Windows build is done locally through Visual Studio / Windows SDK.

NEXT VERSION IDEAS
------------------
- auto-detect ray direction from bright windows / sun
- selectable ray direction in a tiny launcher UI
- half-resolution volumetric buffer for longer / softer shafts
- stronger edge-aware distance haze
- adaptive sky color sampling
- automatic dawn/day/dusk/night detection
- quality presets for RTX 3050
- proper sliders and on-screen control panel
