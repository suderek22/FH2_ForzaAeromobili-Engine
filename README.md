# Forza Aeromobili Engine v0.1.0

Experimental **GTA V style arcade flight engine** for **Forza Horizon 2** (Azimuth), built as a mod for the
[FH2 Mod Platform](https://github.com/) 2.8.0+.

This build adds **no new cars or models**. It only adds a toggle to the Mods page. When the toggle is on,
whichever car you are driving gets the flight model. The goal is to develop proper plane support for FH2
(see the roadmap).

> **Status: experimental.** Flight works in part but does not yet feel like a stable plane. The rewrite of the
> physics integration is the main goal of the next versions.

## Requirements

- Forza Horizon 2 running through Azimuth
- FH2 Mod Platform **2.8.0 or newer** (settings API v4)
- An XInput gamepad

## Install

1. Build the mod (see below) or take `ForzaAeromobiliEngine.dll` from the release.
2. Copy `ForzaAeromobiliEngine.dll` and `ForzaAeromobiliEngine.ini` into your game's `fh2-mods` folder.
3. Start the game. The platform log should show `Forza Aeromobili Engine 0.1.0 loaded (flight engine off)`.

## Usage

- Open **Pause > Settings > MODS** and switch **Flight engine** to **On**. The default is **Off**.
- The setting is saved to `[engine] enabled` in `ForzaAeromobiliEngine.ini`.
- When on, it applies to every car you drive. Below `air_speed` the car keeps its stock handling.

| Input | Action |
|-------|--------|
| RT | Throttle (0 = stand still on the ground) |
| LT | Airbrake (normal brakes on the ground) |
| Left stick | Pitch and roll |
| Right stick | Yaw |

Sticks at rest mean neutral controls, and the aircraft auto-levels.

The console command `aero_flight` reloads the `[flight]` tunables from the ini without restarting.

## Tunables (`ForzaAeromobiliEngine.ini`)

`max_accel`, `max_speed`, `coast_drag`, `airbrake`, `takeoff_speed`, `air_speed`, `lift_factor`, `grip`,
`alignment_force`, `climb_loss`, `pitch_rate`, `roll_rate`, `yaw_rate`, `ang_drag`, `level_rate`, `level_pitch`,
`sense`, `bank_yaw`, `bank_pitch`, `roll_dir`, `pitch_dir`, `yaw_dir`. Defaults are in the shipped ini.

## How it works

- Hooks `ForzaCar::Update` (game+0x2A9400) and `CCarDynamics::UpdateDynamics` (game+0x12C1EA0).
- Applies artificial forces and angular velocity to the car's rigid body (no real aerodynamic lift):
  nose-to-velocity alignment, faked bank turns, high angular drag, speed cap and auto-level.

## Build

Requires MSVC (Visual Studio Build Tools) and the platform headers. Set `FH2MOD_INCLUDE` to the platform
`include` folder if it is not at the default path, then run:

```
build.bat
```

## Known issues

- The aircraft can still spin or be hard to keep level, and the game's physics step may fight the angular writes.
- Only the local player car is affected.

## Roadmap

- Apply the angular and force writes inside the physics step to stop them being overwritten.
- Proper plane handling and stability.
- Later: real plane models, textures and Autoshow entries.
