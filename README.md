# Stormfire

A dark, rainy, low-poly tower defense game for the Nintendo Wii (homebrew), played with the Classic Controller.

A campfire burns in the middle of a brick maze in the woods. Through a night of rain and lightning, enemies march in from every side of the forest. Build towers on the walls and coloured blocks to keep the fire alive through 12 waves and the Storm Golem.

## Controls (Classic Controller)

| Button | Action |
| --- | --- |
| Left stick / D-Pad | move the cursor |
| A | build the selected tower / upgrade the tower under the cursor |
| B | sell (70% back) |
| X / Y | choose tower |
| L | fast forward on / off |
| R | call the next wave early (bonus gold for the seconds you skip) |
| + | pause (resume / restart) |
| - | 16:9 / letterboxed 4:3 |
| HOME | exit |

## Towers

- **Blaster** (blue) - fast homing shots, hits flyers
- **Mortar** (red) - lobbed shells with splash damage, ground only
- **Frost** (ice) - pulses that slow and chip everything nearby, flyers included
- **Tesla** (yellow) - lightning that chains between enemies

Each tower upgrades twice. The fire has 20 health; every enemy that reaches it puts some out (the Golem takes 10). Lightning sometimes strikes an enemy for you.

## Building

Push to GitHub: the Actions workflow builds `boot.dol` in the devkitPro container and uploads it (plus a ready `apps/stormfire/` folder for an SD card) as the `stormfire-wii` artifact. Open `boot.dol` in Dolphin.

Locally with devkitPro installed: `make`.

## Tests

`sh tools/run_tests.sh` builds a native test harness (any C compiler; Pillow for PNG export) that checks the map, renders screenshots with a software copy of the GPU path, measures frame cost, and plays the level with bots of several skill levels. `sh tools/run_tests.sh reel` renders a preview video (needs ffmpeg).

## How it is made

- Everything is drawn as coloured triangles by the GPU: a small 3D layer projects low-poly boxes, prisms and crystals through a tilted perspective camera and draws them back to front.
- Lighting comes from a coarse 3D light grid rebuilt each frame from the campfire, torches, towers, explosions, the moon and lightning flashes.
- The scenery is projected once and only re-lit each frame, which keeps the Wii's CPU well inside 60 fps.
- Every sound, including the endless rain loop and the thunder, is synthesized at start-up. No asset files.
