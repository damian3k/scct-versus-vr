# CLAUDE.md — SCCT Versus VR

Instructions for Claude Code (and other coding agents) working in this repository. Human
contributors: see [`CONTRIBUTING.md`](CONTRIBUTING.md), which has the full workflow.

## What this is

This project is a proxy `d3d9.dll` (C++17, MSVC, **x86/Win32 only**) that turns Splinter Cell:
Chaos Theory — Versus into a VR game.

- It loads under SCCT Versus Reloaded 3.0a, which converts the game's D3D8 into D3D9; the
  tested game is Enhanced SCCT Versus 3.6.
- Backends:
  - **OpenXR** through D3D9On12 on a PC (the released version);
  - **WinlatorXR XR API** (`src/xrapi.cpp`) for Quest standalone (work in progress; the APK is
    not in this repo).
- Architecture, frame structure and every trick: `README.md` → *Repository layout*, *How it
  works*, *Debugging*. Read the relevant section before you change a subsystem.

## Build

- `build.bat` → `bin\d3d9.dll` (+ `openxr_loader.dll`, `SCCT_VR.ini`). It needs VS 2022 and a
  Windows SDK ≥ 10.0.19041.
- From Git Bash, run it as `cmd //c build.bat`.
- **Treat warnings as errors.** For example, C4129 means an unescaped backslash in a string
  literal. Windows paths in C strings need `\\`: `"Z:\\tmp\\xr"`, `"System\\SCCT Versus"`.
  When you generate code through a shell or a script, check the escaping in the resulting file.
- `quest/build.bat` builds the Quest helpers (`vorbisfile.dll` proxy and `SCCT_Quest.exe`).

## You cannot run the game

Only the human can test, in the headset. A normal iteration:

1. Make the change and build it with no warnings.
2. Install it: copy `bin\d3d9.dll` into the game's `System` folder. Ask the user for the path.
   - The game must be **closed** first; check that `SCCT Versus` / `SCCT_Versus.exe` are not
     running.
   - **Never overwrite the user's installed `SCCT_VR.ini`.** It holds their settings. Tell them
     which new lines to add, or add only those lines.
3. Tell the user exactly what to test: maps, situations, what "fixed" looks like.
4. After the test, read `System\SCCT_VR.log` and any dumps:
   - `SCCT_VR_frame_pause<N>.txt` from the Pause key;
   - `SCCT_VR_frame_auto<N>.txt` with `DumpEverySec`;
   - `SCCT_VR_vs_*.bin`, disassembled with `python tools/disasm_vs.py`.

   Base your conclusions on the logs, dumps and screenshots, not on guesses.

Don't claim a fix works until the user has confirmed it in the headset.

## Hard rules

- **Don't break the PC (OpenXR) path.** Quest-only behavior goes behind `vr::UsesXrApi()` (or
  `g_xrApiMode` in input). When a change touches shared code, say so explicitly and list what
  needs re-testing on PC.
- **Ask before acting outside the working tree.** The following happen only when the user asks:
  - commit, push, open a PR;
  - release or package;
  - publish anything.

  `main` is the released state: work on a branch and open a PR.
- If the user says "analyze, don't change anything", only analyze. Before a larger change,
  describe the plan and wait for approval.
- Never commit `bin/`, `obj/`, logs, frame dumps, game files or WinlatorXR binaries.
- Keep the docs in sync when behavior changes: `README.md` (status / how it works),
  `USER_GUIDE.md` (players), `controls.txt`, `SCCT_VR.ini` comments. Don't rewrite docs
  nobody asked about.

## Technical pitfalls (learned the hard way)

- **State blocks:** the game sets textures, texture stage states, texture matrices, FVF, shaders,
  shader constants and streams mostly through `ApplyStateBlock`, so the hooked setters never
  see them. Only `SetTransform(VIEW/PROJECTION/WORLD/TEXTUREn)` reliably passes through the
  hooks. **Read state back with `Get*` on the device right before the draw** for any decision.
  - Exception: under Wine/DXVK (Quest), shader constant uploads *do* pass through
    `SetVertexShaderConstantF`. Code that corrects constants must not correct them twice; see
    `vsGame` in `hooks.cpp`.
- **COM pointers:** never keep one that a hook saw unless you `AddRef` it; the game frees
  buffers at any time. A cached vertex buffer pointer caused the old multiplayer crashes.
  Release everything you `AddRef`/`Get*`.
- **Never classify draws by reading vertex data.** Dynamic VBs read back as garbage in
  gameplay. Classification is structural:
  1. offscreen world → composite → post;
  2. objective markers;
  3. depth-only clear on the backbuffer;
  4. HUD;
  5. menus.

  The Esc menu is recognized by its 512×512 panel texture.
- **Stereo is AFR (alternate frame rendering):** even game frames render the left eye, odd ones
  the right; OpenXR frame calls run once per pair. Only render targets with the backbuffer size
  get the eye camera (shadow maps, reflections and camera gadgets keep the game camera).
- **Shader matrix correction** (`FixShaderConstantsForDraw`) finds the game's view-projection
  inside vertex shader constants and swaps in the eye's. The match must pass the
  non-degenerate basis check, because unrelated constant blocks can look like matrices. Restore
  the constants after the draw.
- **Math conventions:**
  - D3D row vectors (`v * M`), helpers in `src/vrmath.h`;
  - OpenXR is right-handed (meters), the game is left-handed (Unreal units), `WorldScale` units
    per meter.
- **Laptops with two GPUs:** D3D9On12 works only when the game's D3D9 adapter is the OpenXR
  runtime's GPU (LUID match). Otherwise the log says `Device is not a D3D9On12 device - VR
  disabled`.
- **Shutdown:** the XR session is ended before the process exits (`vr::Shutdown`), because some
  runtimes hang during DLL unload.

## Environment notes (Windows host)

- Shells are Git Bash and PowerShell.
- For `adb` in Git Bash, set `MSYS_NO_PATHCONV=1`. With several devices connected, pass `-s
  <serial>`.
- Quest standalone details (WinlatorXR, `Z:\tmp\xr` file bridge, UDP ports): README → *Quest
  standalone / WinlatorXR*, and `src/xrapi.cpp`.
- GitHub repo: `damian3k/scct-versus-vr`, default branch `main`; use the `gh` CLI.
