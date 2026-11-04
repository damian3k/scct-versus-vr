# Contributing to SCCT Versus VR

Welcome! This page explains how the project is set up, how to build and test it, and how
changes get into the repository and into releases. If you work with Claude Code, it also reads
[`CLAUDE.md`](CLAUDE.md), which has the same rules in short form plus the technical pitfalls.

Project owner: **damian3k** (GitHub). He decides what gets merged and released.

---

## 1. What the project is

A single proxy `d3d9.dll` for **Splinter Cell: Chaos Theory — Versus** (32-bit, Unreal Engine
2.5) that turns the game into a stereo, 6DoF VR game with motion controllers. It runs under
**SCCT Versus Reloaded 3.0a** (shipped in **Enhanced SCCT Versus 3.6**), which converts the
game's Direct3D 8 into Direct3D 9; our DLL sits below that layer.

There are two ways it runs, from the same source:

| Backend | Where | How the image gets to the headset |
|---|---|---|
| **OpenXR** (`Backend=0` on a PC, or `1`) | Windows PC + PC VR (tested: Quest 3 through Virtual Desktop / VDXR) | D3D9On12 on the OpenXR runtime's GPU, frames copied into OpenXR swapchains |
| **XR API** (`Backend=0` inside WinlatorXR, or `2`) | Quest standalone, the game running in WinlatorXR (Wine/Box64) | Plain D3D9; head/controller poses come in UDP packets, the eye is marked in a corner pixel |

The PC version is the released one. The Quest standalone version is work in progress. Its APK,
a modified WinlatorXR, is **not** in this repository and not published yet.

Read the README sections **Repository layout**, **How it works** and **Debugging** before you
change code. They describe the frame structure and the tricks the mod relies on.

---

## 2. What you need

- Windows 10/11.
- **Visual Studio 2022** with the C++ workload and a Windows 10/11 SDK (10.0.19041 or newer, for
  `d3d9on12.h`). Everything is built for **x86 / Win32**, because the game is 32-bit.
- The game: **[Enhanced SCCT Versus v3.6](https://github.com/Joshhhuaaa/EnhancedSCCTVersus/releases/tag/v3.6)**
  (includes Reloaded 3.0a). Other versions are not tested.
- A PC VR headset with an OpenXR runtime that has a **32-bit (x86)** runtime DLL. The
  developer's setup is Meta Quest 3 + Virtual Desktop (VDXR), RTX 3090. Other runtimes may not
  work; if you try one, report what happens.
- Optional: Python 3 (for `tools/disasm_vs.py`), Git Bash, `gh` (GitHub CLI).
- For multiplayer tests, a second player or a second machine.

---

## 3. Build and install

```
build.bat
```

This builds `bin\d3d9.dll` and copies `openxr_loader.dll` and the default `SCCT_VR.ini` next to
it. Check the compiler output for **warnings**, not only errors. A warning such as C4129
(an unescaped `\` in a string) is a real bug.

Install for testing:

1. **Close the game.** The DLL is locked while the game runs, and a half-copied DLL gives
   confusing results.
2. Copy `bin\d3d9.dll` (and `openxr_loader.dll` the first time) into the game's `System` folder,
   next to `SCCT_Versus.exe` and `Reloaded.Core.dll`.
3. Copy `SCCT_VR.ini` only the first time, or when you mean to reset it. The ini in the game
   folder holds your own settings, so don't overwrite it with every build. When you add an
   option, add the line to your installed ini by hand.

Smoke test without the game (it runs flat without a headset):

```
cl /nologo /O2 /MT test\testapp.cpp /Fe:bin\testapp.exe user32.lib   (VS x86 prompt)
bin\testapp.exe 10
```

---

## 4. How a change is tested

The game can only be tested **in the headset, by a person**. The loop is:

1. Change the code, build, and fix all warnings.
2. Install into the game (see above).
3. Play the situation that the change is about, in VR. If possible, also play a quick online
   match.
4. Read `System\SCCT_VR.log`. For rendering problems, take a frame dump with the **Pause** key
   (`SCCT_VR_frame_pause<N>.txt`), or set `DumpEverySec=N` in the ini to dump automatically.
   Screenshots or a short video help a lot.
5. Repeat until it works, then check that nothing else broke. The usual places to check:
   - menus (main menu and the Esc menu);
   - HUD and objective markers;
   - shadows and the flashlight;
   - light shafts (Warehouse);
   - spy camouflage;
   - merc motion vision;
   - the zoom / binoculars;
   - the PC window (`DesktopMirror`).

Rules that come from experience on this project:

- **Don't break the PC version.** The PC version is the released one. Any Quest/XR API change
  must leave the OpenXR path unchanged. Guard Quest-only behavior with `vr::UsesXrApi()`.
- **Test first, package later.** Release zips and docs are refreshed only after the change has
  been confirmed in the headset.
- New behavior that might not suit everyone gets an option in `SCCT_VR.ini`.

---

## 5. Code conventions

- C++17, MSVC, no external libraries besides the OpenXR loader in `deps/`.
- Follow the style of the surrounding code: 4-space indents, `CamelCase` functions, short
  comments that explain *why*. Logging uses `Log(...)` and writes to `SCCT_VR.log`. Keep log
  output readable for players, because they send us their logs.
- **New ini option:**
  1. add the field with its default to `Config` in `src/config.h`;
  2. read it in `LoadConfig()` in `src/config.cpp`;
  3. add a commented line to `SCCT_VR.ini`;
  4. if players should know about it, document it in `USER_GUIDE.md` (Settings).
- Keep the docs in sync when behavior changes:
  - `README.md` (status, how it works);
  - `USER_GUIDE.md` (players);
  - `controls.txt` (controller layout).

  Commits that only change docs are prefixed `README:` / `Docs:`.

---

## 6. Git and pull requests

- `main` is the released state. **Don't push to `main` directly.** Work on a branch, then open a
  pull request to `main`:
  - with write access to the repo, branch in it (`fix/light-shafts`, `feature/reload-gesture`);
  - otherwise, fork the repo.
- One topic per pull request. In the description, write:
  - what changed and why;
  - **how you tested it** (headset, runtime, maps, online or offline);
  - anything you could not test (for example "Quest standalone not tested").
- Commit messages: short imperative summary in English, e.g. `Spy camera: keep the view steady
  in vents`. Release commits are named `vX.Y.Z: <main change>`.
- Never commit build output (`bin/`, `obj/`), logs, frame dumps or game files. `.gitignore`
  already covers most of them.
- Don't commit anything from the game itself (copyrighted) or from a WinlatorXR build.

---

## 7. Releases (owner only)

The owner tags and publishes releases after testing. You don't need to do this, but this is
what happens:

1. Bump the version in the README status section.
2. Commit `vX.Y.Z: ...` and push to `main`.
3. Build the zip `SCCT_VR_mod_vX.Y.Z.zip` with:
   - `d3d9.dll`;
   - `openxr_loader.dll` and `OpenXR_loader_LICENSE.txt`;
   - `SCCT_VR.ini`;
   - `README.md`;
   - `controls.txt`.
4. Publish with `gh release create vX.Y.Z --repo damian3k/scct-versus-vr`. The notes have the
   same layout as the previous releases.

---

## 8. Where to start

The **Roadmap** and **Known issues** sections in the README list open work. The big item is
"Stage 2", hooking the engine's camera so the game renders from the head (no culling glitches
when looking behind). Before you start something bigger, talk to the owner, so two people don't
work on the same thing.

Useful places in the code:

| Task | File / function |
|---|---|
| Something drawn in the wrong place in one eye | `hooks.cpp`: `RefreshDrawState`, `FixCameraSpaceTexgen`, `FixShaderConstantsForDraw` |
| HUD / menu / scene classification | `hooks.cpp` (draw hooks), `vr.cpp` (`NoteSceneDraw`, `SceneActive`, HUD layer) |
| Head tracking, eye cameras, frames sent to the headset | `vr.cpp` (`EyeTransform`, `OnPresent`) |
| Controller bindings, gestures | `input.cpp`, `controls.txt` |
| Quest standalone | `xrapi.cpp`, and the `vr::UsesXrApi()` branches |
| Startup, exports, crash log | `dllmain.cpp` |
