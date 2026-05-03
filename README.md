# Cannonade for Pebble

Cannonade is a turn-based artillery game for Pebble smartwatches, ported from the Fitbit original. Adjust your angle and shot speed, account for wind, and land a shell on the opposing fort before the computer does the same to you.

Targets the **Emery** platform (Pebble Time 2, 200×228 color display with touch screen and speaker).

## Features

- Touch-only gameplay with immediate aiming on press, fire on release
- Four computer skill levels: Beginner, Medium, Advanced, and Sniper
- Persistent score, selected skill level, and turn order across sessions
- Wind, destructible buildings with collapse animations, shell arc, and hit explosions
- Fort destruction animation — forts slide off-screen when destroyed
- Speaker sound effects: cannon fire, building hit, fort destroyed
- Game timer pauses automatically when a notification arrives
- Bitmap artwork for the battlefield, forts, and HUD icons
- No companion app or phone configuration required

## Controls

### Title Screen

| Touch target | Action |
|---|---|
| Left half of `<< Level: ... >>` | Previous AI difficulty |
| Right half of `<< Level: ... >>` | Next AI difficulty |
| Anywhere else | Start / continue game |

Back button exits the app.

### Gameplay

Touch zones divide the screen into five regions:

```
┌─────────────────┐
│   increase angle │  top 30%
├──────┬──────┬───┤
│ spd- │ FIRE │spd+│  middle 40%
├──────┴──────┴───┤
│   decrease angle │  bottom 30%
└─────────────────┘
 left  center right
 30%   40%   30%
```

- **Press** a directional zone to adjust the parameter immediately.
- **Release** inside the center fire zone to shoot.

Back button returns from an active game to the title screen.

## Building

Install the Pebble SDK, then build from the repository root:

```sh
pebble build
```

The packaged app is written to `build/cannonade-pebble.pbw`.

## Running in the Emulator

```sh
pebble install --emulator emery
```

Or use the npm helper (build + install in one step):

```sh
npm run start
```

## Project Layout

```
src/c/main.c        Game logic, rendering, physics, sound
resources/images/   Bitmap and PNG assets
package.json        Pebble manifest and resource declarations
wscript             Native build script
```

## Implementation Notes

- **No Clay / AppMessage / companion JS.** All settings persist on-watch via `persist_*`.
- **Trig via Pebble lookup tables** (`sin_lookup` / `cos_lookup`) rather than linked libm, avoiding a crash seen on real hardware with SDK `sqrtf`.
- **Speaker sounds** are pure `SpeakerNote` sequences (no audio resources) — zero resource budget cost.
- **Building collapse** animates during the explosion phase: regular buildings drop 1 px/frame, forts slide off-screen at 6 px/frame.
