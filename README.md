# Stormfire

A dark, rainy, low-poly tower defense game for the Nintendo Wii (homebrew), played with the Classic Controller.

A campfire burns in the middle of a maze in the woods, and enemies march in from every side of the forest. Build towers on the walls and coloured blocks to keep the fire alive.

## Levels

Pick a level on the title screen with left / right. Winning a level takes you straight to the next one.

1. **The Storm** - a night of rain and lightning in a brick maze. 12 waves, ending with the Storm Golem.
2. **Autumn Gold** - golden hour in an autumn forest: low sunbeams through red and orange trees, fog, falling leaves and long shadows. A ring road circles the fire with only two ways in. Tougher enemies:
   - **Beetle** - armoured; small hits bounce off (mortars and tesla do best)
   - **Slime** - splits into two slimelets when it pops
   - **Fox** - trots, then sprints in bursts
   - **Crow** - flies straight at the fire
   - **Pumpkin King** - the boss: armoured, drops gourds as it walks, and bursts into more when it dies

## Controls (Classic Controller)

| Button | Action |
| --- | --- |
| Left stick / D-Pad | move the cursor |
| A | build the selected tower / upgrade the tower under the cursor |
| B | sell (70% back) |
| L / R | choose tower |
| Y | fast forward on / off |
| X | call the next wave early (bonus gold for the seconds you skip) |
| + | pause (resume / sound on-off / restart / quit to title) |
| - | 16:9 / letterboxed 4:3 |
| HOME | exit |

## Towers

- **Blaster** (blue) - fast homing shots, hits flyers
- **Mortar** (red) - lobbed shells with splash damage, ground only
- **Frost** (ice) - pulses that slow and chip everything nearby, flyers included
- **Tesla** (yellow) - lightning that chains between enemies

Each tower upgrades twice. The fire has 20 health; every enemy that reaches it puts some out (bosses take 10). In the storm, lightning sometimes strikes an enemy for you.

## Building

Push to GitHub: the Actions workflow builds `boot.dol` in the devkitPro container and uploads it (plus a ready `apps/stormfire/` folder for an SD card) as the `stormfire-wii` artifact. Open `boot.dol` in Dolphin.

Locally with devkitPro installed: `make`.

## Tests

`sh tools/run_tests.sh` builds a native test harness (any C compiler; Pillow for PNG export) that checks the map, renders screenshots with a software copy of the GPU path, measures frame cost, and plays the level with bots of several skill levels. `sh tools/run_tests.sh reel 2` renders a preview video of level 2 (needs ffmpeg).

## How it is made

- Everything is drawn as coloured triangles by the GPU: a small 3D layer projects low-poly boxes, prisms and crystals through a tilted perspective camera and draws them back to front.
- Lighting comes from a coarse 3D light grid rebuilt each frame from the campfire, torches, towers, explosions, and the moon and lightning (level 1) or dappled low sunlight (level 2).
- The scenery is projected once and only re-lit each frame, which keeps the Wii's CPU well inside 60 fps.
- Every sound, including the thunder, is synthesized at start-up. No asset files.
