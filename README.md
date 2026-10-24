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
3. **Hallows Eve** - dusk at the end of autumn: bare swaying trees hung with lanterns, a rising moon and thick fog rolling through and over the course. Long zigzag lanes reach the fire from all four sides. Some enemies stop to pound on your towers - the blows rattle them, but a tower you place stays put.
   - **Zombie** - slow and tough; stops to pound on towers it passes
   - **Ghost** - floats straight at the fire and keeps fading out of reach
   - **Skeleton** - falls apart when beaten, then pulls itself back together once
   - **Bat** - small, fast flyers in swarms
   - **Gravebreaker** - armoured brute with a tombstone hammer
   - **Ghoul King** - the boss: armoured, raises zombies as it walks and bursts into more
4. **Dawn Isle** - an island of old stone and brass machines on a calm sea at sunrise. Enemies come ashore along six wooden piers, and the fire has only two doors. The sun climbs as the waves go by: rosy first light with long shadows and mist on the water, then a golden morning, then bright day, with the sun's rays, shadows and glitter on the sea changing as it rises. Rune stones glow, orreries turn, gears spin on the walls and gulls wheel overhead. The toughest enemies yet:
   - **Automaton** - an armoured brass soldier with a wind-up key in its back
   - **Springer** - crouches on coiled springs, then leaps down the path
   - **Brass Moth** - flies straight at the fire and shields everything under it (extra armour)
   - **Tinker** - a little repair-bot that heals everything nearby every few seconds - pick it off first
   - **Sentinel** - a stone-and-brass guardian that pounds on towers
   - **Architect** - the boss: armoured, builds automatons as it walks, and its shockwave jams every tower nearby for a few seconds
5. **Pirate Storm** - the epic one: a much bigger map (27 x 17), all at sea in a storm, with rain, lightning and wind gusts that tear across the water. Build on wooden docks, cargo crates and sandbars; the pirates sail up the open-water lanes from eight directions to plunder a treasure chest on an island in the middle. The whole world rides the swell - everything rolls and bobs with the sea - and lantern buoys mark the way in. 15 waves:
   - **Rowboat** - armoured little boats full of rowing pirates
   - **Shark** - fast, and dives under the surface (out of reach) every few seconds
   - **Skull Pirate** - a skeleton buccaneer on a barrel; knock it into the sea and it climbs back aboard once
   - **Parrot** - flies straight at the treasure
   - **Pirate Ship** - big and armoured; its cannon broadsides jam every tower alongside for a moment
   - **Kraken** - the boss: a vast head and eight writhing tentacles; it dives out of reach now and then and calls up sharks as it comes

The bigger map is drawn smaller to fit, so use **ZL / ZR** (L2 / R2) to zoom in and out - the view follows the cursor.

Each level has its own music (dreary for the storm, light and fresh for autumn, fun and spooky for Halloween, slow and mysterious for the island, an epic stormy sea shanty for the pirates). Battles shake the screen with a little depth parallax (the near side of the board moves more than the far side), big hits punch the view, and every weapon has extra flourishes: blaster muzzle flashes and flying casings, mortar landing markers, fireballs and scorch marks, frost splinters and frosted-over enemies, and tesla coils that visibly charge up before they fire.

## Controls (Classic Controller)

| Button | Action |
| --- | --- |
| Left stick / D-Pad | move the cursor |
| A | build the selected tower / upgrade the tower under the cursor |
| B | sell (70% back) |
| L / R | choose tower |
| ZL / ZR | zoom out / in |
| Y | fast forward on / off |
| X | call the next wave early (bonus gold for the seconds you skip) |
| + | pause (resume / music on-off / sound on-off / restart / quit to title) |
| - | 16:9 / letterboxed 4:3 |
| HOME | exit |

## Towers

- **Blaster** (blue) - fast homing shots, hits flyers
- **Mortar** (red) - lobbed shells with splash damage, ground only
- **Frost** (ice) - pulses that slow and chip everything nearby, flyers included
- **Tesla** (yellow) - lightning that chains between enemies
- **Prism** (violet, expensive: 300) - a continuous beam that locks onto one enemy and grows stronger the longer it holds on, hits flyers

Each tower upgrades twice, and once placed it stays until you sell it - nothing can destroy it. The fire (or on level 5, the treasure) has 20 health; every enemy that reaches it takes some (bosses take 10). In the storm, lightning sometimes strikes an enemy for you.

## Building

Push to GitHub: the Actions workflow builds `boot.dol` in the devkitPro container and uploads it (plus a ready `apps/stormfire/` folder for an SD card) as the `stormfire-wii` artifact. Open `boot.dol` in Dolphin.

Locally with devkitPro installed: `make`.

## Tests

`sh tools/run_tests.sh` builds a native test harness (any C compiler; Pillow for PNG export) that checks the map, renders screenshots with a software copy of the GPU path, measures frame cost, and plays the level with bots of several skill levels. `sh tools/run_tests.sh reel 5` renders a preview video of level 5 with its music (needs ffmpeg).

## How it is made

- Everything is drawn as coloured triangles by the GPU: a small 3D layer projects low-poly boxes, prisms and crystals through a tilted perspective camera and draws them back to front.
- Lighting comes from a coarse 3D light grid rebuilt each frame from the campfire, torches, towers, explosions, and the moon and lightning (level 1) or dappled low sunlight (level 2).
- The scenery is projected once and only re-lit each frame, which keeps the Wii's CPU well inside 60 fps.
- The sea swell on level 5 is part of the view transform: a slow roll and bob folded into the 2D affine every vertex already goes through, plus one table lookup per vertex for a wave rolling across the screen.
- Every sound and all five music loops are composed and synthesized at start-up (a small offline synth with plucked strings, FM bells, a theremin, a fiddle, an accordion, brass, timpani and reverb). No asset files.
- Trees sway by storing a sway weight with each cached vertex, so the whole forest moves for the cost of one sine per tree.
