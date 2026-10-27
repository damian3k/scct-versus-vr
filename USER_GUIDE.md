# SCCT Versus VR — user guide

A VR mod for **Splinter Cell: Chaos Theory — Versus** (PC). Play the multiplayer in a VR headset
with head tracking, aim the Mercenary's gun with your right controller and use Quest/Touch
controllers for everything else. Other players do not need the mod — you can play with people on
a normal screen.

Version 0.8.3 · Gameplay video: https://www.youtube.com/watch?v=nbMLmVTR_hc

## Requirements

- **[Enhanced SCCT Versus v3.6](https://github.com/Joshhhuaaa/EnhancedSCCTVersus/releases/tag/v3.6)**
  (includes SCCT Versus Reloaded 3.0a) — the version the mod is tested on. Everyone you play with
  must use the same Enhanced version.
- Windows 10/11 and a PC VR setup with an **OpenXR runtime**: Virtual Desktop (VDXR), Meta Quest
  Link / Air Link, or SteamVR. Tested with Meta Quest 3 over Virtual Desktop.
- Microsoft Visual C++ 2015–2022 Redistributable (x86), already required by Enhanced/Reloaded.

## Installation

1. Close the game.
2. Copy `d3d9.dll`, `openxr_loader.dll` and `SCCT_VR.ini` into the game's **`System`** folder
   (the folder with `SCCT_Versus.exe` and `Reloaded.Core.dll`).
3. Start your VR runtime (Virtual Desktop / Link / SteamVR) and put the headset on.
4. Start the game with **`System\SCCT_Versus.exe`** as usual. Click the game window once so it has
   keyboard/mouse focus (controller input is sent to the focused game window).

Without a headset or runtime the game simply runs normally on the monitor.
To disable the mod set `Enabled=0` in `SCCT_VR.ini`, or delete `d3d9.dll`.

### Recommended game settings (`System\SCCT_Versus.config`)

- `"fps_merc"` and `"fps_spy"`: about **twice your headset refresh rate** (e.g. 144 for 72 Hz,
  180 for 90 Hz). The mod shows every second game frame to each eye.
- Game resolution: close to the headset's per-eye resolution (written to `SCCT_VR.log` on start).

## Controls (Quest / Touch)

| Controller | In game | In menus |
|---|---|---|
| Right controller (pointing) | Aim the Mercenary's weapon; **Spy:** turn the controller to steer the camera / aim | — |
| Right stick left/right | Turn (smooth; `SnapTurn` for snap) | Move cursor |
| Right stick up/down | Mouse wheel (switch item; zoom in/out while zoomed) | Move cursor |
| Eyes to the gun / head away | Zoom on / off (merc Assault Gun, spy binoculars, see *Zoom*) | — |
| Left hand to the magazine | Reload (Mercenary, like changing the magazine by hand) | — |
| Left stick | Move (W/A/S/D) | **Arrow keys** |
| Right trigger | Fire (left mouse) | Click |
| Left trigger | Alt-fire / gadget (right mouse) | — |
| Right grip | Use (Shift) | — |
| Left grip | Roll / laser (Q) | — |
| A | Jump (Space) | **Enter** |
| B | Crouch (C) | **Esc** (back) |
| X | Reload (R); **Spy:** tap = next gadget, hold = previous gadget | — |
| Y | Tap: vision modes (Tab); **hold: co-op move (T)** | — |
| Left stick click | Walk / run (middle mouse) | — |
| Right stick click | Fire mode / torch (E); **Spy:** draw / put away the weapon | — |
| Both stick clicks together | **Spy:** first-person view on / off (experimental) | — |
| Menu button | Esc | Esc |

**Menus switch the controls automatically:** whenever a menu is on screen - main menu, match
menus or the in-game menu during a match (open it with the **Menu** button) - the left stick, A and
B work as menu controls; when you leave the menu, however you leave it, the game controls return.

Everything can be remapped in `SCCT_VR.ini`, sections `[Bindings]`, `[SpyBindings]` (used when
not aiming with the controller, i.e. the Spy) and `[MenuBindings]`. Any control can also get a
second action on a long press with `<Name>Hold=...` (e.g. `YHold=T`).

Keyboard keys while playing:

| Key | Action |
|---|---|
| Home | Recenter the view (holding the Quest's Meta/Oculus button also recenters) |
| End | Controller aiming on/off |
| Insert | HUD mode: follow the weapon ↔ fixed in front of you |
| Num + / Num − | Bigger / smaller HUD (follow mode); the value is written to `SCCT_VR.log` |
| Pause | Write a diagnostic frame dump (`SCCT_VR_frame_pause1.txt`, `pause2`...) |

## How it plays

- **Mercenary:** first person. The gun is in your right hand and shoots where you point it; the
  game's own laser shows the aim. The right stick turns your body.
- **Mercenary reload by hand:** bring your left hand to the magazine of the gun — in front of
  and a little below your right hand — and the gun reloads (the mod presses R). Pull the hand
  away before the next reload. X still reloads too. Tune the spot with `ReloadForward`,
  `ReloadDown` and `ReloadRadius` in `SCCT_VR.ini` (`ReloadGesture=0` turns it off).
- **Mercenary zoom (Assault Gun sniper mode / binoculars):** see below.
- **Spy:** third person (the camera stays behind the spy). By default (`SpyAim=1`) you steer the
  camera and aim a drawn weapon (click the right stick to draw it) by **turning the right
  controller**; only its rotation counts, moving your hand does nothing. The right stick turns
  you. The VR view stays steady behind the spy while the controller turns the game camera (the
  mod finds the spy's model and places the view relative to it; `SCCT_VR.log`: "Spy model
  found"). `SpyAim=0` brings back the old mode (the right stick moves the camera like a mouse).
  Looking far to the side or behind shows black areas: the game only draws what its own camera
  sees.
- **Spy binoculars:** see *Zoom* below.
- **Spy first-person view (experimental):** see below.
- **HUD:** ammo, gadgets, crosshair and objective markers are on a panel that follows the game
  camera (the weapon), so markers sit exactly on their targets. Press Insert for a small HUD that
  stays still in front of you (markers will then drift off their targets).
- **Menus:** shown on a flat screen in front of you. Use the left stick + A/B, or the right stick +
  trigger as a mouse.

## Zoom: sniper mode (Mercenary, Assault Gun) and binoculars (Spy)

The Assault Gun has a zoom (sniper) mode. In VR you use it like a real scope:

1. **Bring your eyes down to the gun:** raise the right controller close to your face (about
   20 cm) and point it where you are looking, as if you put your eye to the sights. After a
   moment the zoom switches on (the mod presses the game's zoom key, **B** on the keyboard).
2. You now see the game's zoomed view on a screen in front of your eyes, with the game's own
   scope overlay, crosshair and HUD.
3. **Aim by moving the controller:** the controller moves the crosshair the way a mouse does in the
   normal game; the view follows the crosshair.
4. **Right stick up / down:** zoom in / out (mouse wheel). **Left trigger:** hold breath (steadier
   aim, right mouse). **Right trigger:** fire.
5. **Lift your head away from the gun** to leave the zoom (the mod presses B again). The normal VR
   view returns and the gun is back in your hand, pointing where the controller points.

If the game leaves the zoom on its own (e.g. reloading), nothing else is pressed when you lift
your head.

**Spy binoculars** use the same gesture: hold the right controller up to your face like
binoculars and they switch on (B). While looking through them you **look around with your
head**; the right stick zooms in / out. Take the controller away from your face to put them down.

Settings in `SCCT_VR.ini`:

| Option | Default | Meaning |
|---|---|---|
| `ScopeGesture` | 1 | 0 = no head-to-gun gesture (press B on the keyboard yourself) |
| `ScopeGestureDistance` | 0.22 | eyes-to-controller distance (m) that starts the zoom; raise it if the zoom is hard to start, lower it if it starts by accident |
| `ScopeView` | 1 | show the zoomed view on the screen in front of your eyes |
| `ScopeSize` / `ScopeDistance` | 0.8 / 1.0 | size and distance (m) of that screen |
| `ScopeMouseScale` | 1.0 | how far the crosshair moves for the same hand motion while zoomed |
| `ScopeKey` (in `[Bindings]`) | B | the game key the gesture presses |

## Spy first-person view (experimental)

**Click both thumbsticks at the same time** (while playing the spy) to switch between the normal
third-person camera and a view from the spy's eyes; click both again to go back. Neither stick's
own action is sent when you click them together.

- The view is attached to the top of the spy's head, so it follows crouching, standing up and
  climbing. You look around with your head and aim with the controller as usual; you can see
  your hands and arms.
- Using the **binoculars** once (while standing still) measures where the spy's eyes are exactly
  (`SCCT_VR.log`: "Spy eyes measured with the binoculars").
- This feature is **experimental and needs more work**. The game was not made for this view: it
  still renders and hides objects from its own camera behind the spy, so things can go wild —
  geometry appearing or disappearing near the head, walls when you stand with your back to them,
  odd moments while climbing or in tight spaces. Switch back to third person if it gets in the way.

| Option | Default | Meaning |
|---|---|---|
| `SpyEyeRight` / `SpyEyeUp` / `SpyEyeForward` | 0 / −8 / 16 | eye position relative to the top of the head (Unreal units, 100 = 1 m); forward keeps the view in front of the face |
| `SpyFpvShowBody` | 1 | 1 = you see your body and hands, 0 = hide the body too |
| `SpyFpvHideRadius` | 60 | gear this close to the spy (goggles...) is not drawn |

## Settings (`SCCT_VR.ini`)

Every option is commented in the file. The most useful ones:

| Option | Default | Meaning |
|---|---|---|
| `WorldScale` | 100 | Unreal units per meter. Higher = world feels smaller. |
| `HudFollowScale` | 0.45 | HUD size in follow mode (Num+/Num− in game) |
| `HudDistance` | 2.5 | HUD distance in meters |
| `HudFollowAim` | 1 | 1 = HUD follows the weapon, 0 = fixed in front of you |
| `AimMode` | 2 | 2 = controller aiming for the Mercenary only, 1 = always, 0 = never |
| `TurnSpeed` / `SnapTurn` | 120 / 0 | smooth turn speed (°/s) / snap turn angle (0 = off) |
| `SpyAim` | 1 | Spy: 1 = controller rotation steers the camera / aims, 0 = right stick (old mode) |
| `SpyStableCamera` | 1 | Spy: keep the VR view steady behind the spy while the controller turns the camera |
| `StickMouseSpeed` | 4500 | right-stick camera speed (Spy with `SpyAim=0`) |
| `MenuMouseSpeed` | 1500 | right-stick cursor speed in menus |
| `WeaponOffsetX/Y/Z`, `WeaponPitch` | 10 / −15 / 20, 0 | position/tilt of the gun in your hand |
| `FixProjectedLights` | 1 | keeps the flashlight and shadows fixed to the world |
| `DesktopMirror` | 2 | PC window during gameplay (for streaming / people watching), the right eye: 2 = like in the headset, the HUD where you see it in VR (the crosshair follows the gun); 1 = the HUD over the whole window; 0 = the raw headset frames (eyes alternate, no HUD) |

## Troubleshooting

- **No image in the headset / game is flat:** start the VR runtime before the game; check the first
  lines of `System\SCCT_VR.log` (it names the runtime and headset or says why VR was not started).
- **Controllers do nothing:** click the game window so it is in the foreground.
- **View starts in a strange place:** press Home (or hold the Meta button) to recenter.
- **Choppy image:** raise `fps_merc` / `fps_spy` (see above).
- **Game closes during a multiplayer match:** this was a bug in the mod, fixed in v0.7. If it
  still happens, send `SCCT_VR.log` and `SCCT_Versus_crash.log` to the mod author.
- **Something looks wrong:** press Pause at that moment and send `SCCT_VR.log` and the
  `SCCT_VR_frame_pause*.txt` file to the mod author. In VR, where the keyboard is out of reach,
  set `DumpEverySec=5` instead (a dump every 5 s, `SCCT_VR_frame_auto*.txt`) and set it back to 0
  afterwards.
- **Conflicts:** other mods that also use a `d3d9.dll` (e.g. ReShade for D3D9) cannot be used at the
  same time.

## Playing online

Same as Enhanced SCCT Versus: create a session, friends find it in the server list or add your
`IP:7776` to `serverList` in their `SCCT_Versus.config`. Everyone needs the same Enhanced version.

Earlier versions crashed shortly after a session was created or a match started; that was a bug
in the mod and is fixed in v0.7. If hosting still crashes on your setup, try setting
`"host_upnp": false` and `"list_on_reloaded_master_server": false` in `SCCT_Versus.config` (then
forward **UDP 7776 and 8888** on your router manually and give friends your `IP:7776` for their
`serverList`) and report it.

## Known issues

- **Looking behind you causes graphical glitches** (missing walls and objects, black areas). The
  game only draws what its own camera sees (where you aim / the spy camera looks) and leaves out
  the rest. We are still working out what to do about it.
- **Spy first-person view** is experimental and can behave strangely (see above).
- Each eye gets every second game frame, so the game needs about twice the headset refresh rate.
- The HUD follows the weapon, so it moves when you aim around.
- The game itself occasionally crashes when you leave a match (also happens without VR).
