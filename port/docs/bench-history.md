# Switch and bench history

The source comments say what each switch does, why, and its verdict (on or off by default). The numbers, dates and
issue/PR/bench references that used to sit in those comments are kept here, so nothing is lost. "Bench #n" and "#n"
are issue or pull request numbers of the project's GitHub repository; dates are 2026.

| Switch / topic | Verdict now | Where it was recorded |
| --- | --- | --- |
| `DKR_SPLIT_RENDER` (two-core renderer, roadmap P21) | On by default on both consoles. | Old 3DS: before it the renderer took 31 ms a frame (bench #258); on by default there since bench #263 (Jungle Falls 21.0 to 29.0 fps, Crescent Island 20.0 to 29.1). New 3DS: bench #286 (renderer 7.4-8.4 to 3.7-4.3 ms; Haunted Woods' 1% low 30 to 58 fps). The New 3DS bench row was perfect, owner 2026-11-05 21:17 ("default on both"). |
| `DKR_SPLIT_SYSCORE` (backend on the system core, New 3DS) | Off by default. | Issue #390: game core 2-14% idle in a busy race, a 33 ms frame every 8-16. |
| `DKR_TEX_FREE_QUEUE` (v33) | On. | Bench #457: on an Old 3DS a mid-race texture free made the walk wait for the whole queue, frames of 49-54 ms. |
| `DKR_QUEUE_WAKE` (roadmap P32, per-queue wake-ups) | On for a New 3DS, off on Old 3DS and PC. | No gain in switch bench #246. On 2026-11-01 made the New 3DS default: with every thread woken by every message an AI Diddy's head flickered in every switch bench race; the per-queue race had none. Old 3DS never flickered there and wasn't benched. |
| Interrupt-disabled wait (`wait_for_int_enabled`, os_core.c) | Always on outside lockstep. | Issues #91, #93, #97, #102: 60 fps freezes, the sound thread spun in a loop of the game's `sndp_allocate` lists. |
| Bounded queue waits (os_core.c) | Always on. | Issues #107 and #114: a lost 3DS condition variable wake-up left the scheduler asleep on a full interrupt queue. Done-message wait: issues #91 and #93 at 60 fps. |
| GPU time cuts (gpu_cuts.inc, thread "Cut New 3DS GPU time", 2026-11-07) | See each switch. | At 800 pixels with 2x anti-aliasing the GPU held the New 3DS back (switch bench #444: 14-16 ms a frame, the limit in every race); the bench's cost rows put about 5 ms of it outside the track, racers, objects and sky. |
| `DKR_COLOUR_FILL` | On by default on a New 3DS. | The colour clear was two triangles over the whole target: 384,000 pixels at 800x480, as many as the sky. |
| `DKR_FB_VRAM` | On by default on a New 3DS (after #451). | The copy to the screen: 1.2 to 0.55 ms a frame. |
| `DKR_TEX_CACHE_KEEP` | Off, shelved. | #451 saw no gain. |
| `DKR_SKIP_INVISIBLE` | On. | The screen fader's alpha 0 quad over the whole screen in every race frame. |
| `DKR_SKIP_DRAW_KINDS` / `DKR_SKIP_KINDS` | Bench cost rows only (the rows were removed in #463). | |
| `DKR_BOSS_STEP30` | On. | Bubbler ran about 20% faster at 60 fps than at 30 fps in both fights (2026-11-11). |
| `PICA_ALL` | Always defined (plat.h). | Everything on the PICA200 on master since PR #468. The GPU vertex maths (`DKR_GPU_TRANSFORM`, `DKR_NATIVE_TRACK`, `DKR_GPU_LIGHT`) are off by default: it lost on the Old 3DS (issue #406); owner decision 2026-11-05. `DKR_TEX_VRAM` default is the backend's (`tex_vram_default`): on for the Old 3DS since v28 (benches #432/#436, after the race-start stalls of issues #406 and #407), off elsewhere. |
| 16-bit colour and depth buffers | Removed 2026-10-29. | Changed the picture, fog among it. |
| README "Status" figures | Moved to the release notes. | Played on a New 3DS since 2026-10-17: 30 fps with 4x anti-aliasing, about 60 fps with anti-aliasing off or 2x on most tracks (WizPig 1 and Walrus Cove 2 were GPU-bound at 60 fps with 2x). |
| README switch bench cost rows | Removed. | The bench had four rows leaving a kind undrawn (`DKR_SKIP_KINDS`, a bit per kind) and, on a New 3DS, rows for anti-aliasing off, colour boost off and 400 pixels. |
| Removed in simplify batch 5 (2026-11-15): `DKR_SKY_LAST` (P41) | Code removed (default was off). | The sky's batches were kept and drawn last at the farthest depth: about 1.9 ms of a New 3DS's GPU time at 800 pixels with anti-aliasing (switch bench #309), but the bench found it "saved nothing"; left off and the rows dropped. The backend's `GFX_DEPTH_FAR` decal path still exists until batch 4. |
| `DKR_WELD` (v39, shared corners, Old 3DS) | Code removed (default was 0). | A racer's model sends ~300 vertices for ~130 different ones; a table of 256 lines of 32 bytes reused the transformed, lit and fogged vertices. Bench #481: GPU and renderer 0.6 ms worse (racers only), every piece 2 ms more renderer and 97%; its rows went. |
| `DKR_SKIP_KINDS`, `DKR_NOTEX_KINDS` (walk side) | Code removed. | Bench cost rows only (a bit per kind, kinds not drawn or drawn without textures); the rows went in #463. `DKR_SKIP_DRAW_KINDS` in gfx_c3d.c is another variable and stays until batch 4. |
| `DKR_STALL_EACH` | Code removed. | Every command timed (a system call each on the 3DS) to find a stall: the bench verdict was "the stall finder only costs". The frame-step stall marks stay in debug builds. |
| `DKR_KCOND` (v37, roadmap P50) | Always on. | Bench #472: libctru's condition variable had 10-34 late wakes a race (lean quick test #467: 45 wakes 4.2-5.7 ms late in 35 s, the 38-39 ms worst frame of every track) against none with the kernel lock's own waits. |
| Settled native renderer switches: `DKR_TRI_REJECT`, `DKR_NMAT_FAST`, `DKR_NATIVE_DYNAMIC`, `DKR_NATIVE_ONCE`, `DKR_NATIVE_BUCKET`, `DKR_NATIVE_VFP`, `DKR_FAST_OUTCODE`, `DKR_AXIS_FAST`, `DKR_SCREEN_ONCE`, `DKR_NATIVE_HASH`, `DKR_SPRITE_AXES`, `DKR_REMOTE_FLUSH`, `DKR_REMOTE_SHARE`, `DKR_TRIS_SKIP`, `DKR_TEX_FREE_QUEUE`, `DKR_NATIVE_BIG` | Always on (the old side removed). | Each defaulted to 1 and was kept by a bench: #432/#436/#435 (no row beat them), #443 (`REMOTE_FLUSH`, `REMOTE_SHARE`), #444 (`TRIS_SKIP`), #459/#460 (`TEX_FREE_QUEUE`). `DKR_NATIVE_BIG` (v28): the mesh table's limit is three quarters of 2048 and the pool 640 KB, as the old 384 meshes made all be forgotten and remade every ~100 frames in a Whale Bay race (~590 different triangle lists in 700 frames). `DKR_NATIVE_VFP` is still ANDed with `DKR_VFP_VECTORS`. |

Roadmap item tags that were removed from comments (P7 native texture formats, P30/P31/P33/P34 GPU vertex maths and
meshes, G1 mipmaps, G3 widescreen, G6 frame rate cap, G9 race HUD on the bottom screen, O5/P9 Sound page, O8 Old 3DS
defaults, O9 code layout, P29 experimental switches, P36 bottom screen thread, v31 system core call) are described in
`proposals/3ds-roadmap.md` in the project files.
