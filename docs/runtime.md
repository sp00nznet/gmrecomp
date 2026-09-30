# Runtime

`runtime/gm_core.c` covers values, instances, events, rooms and savestates.
`gm_funcs.c` has the built-in functions and `gm_host.c` the SDL2 host (drawing,
audio, input, main loop). `gmrt.h` is the contract with the generated code.

## Values

`Val` is a real, a string, an array or undefined. Reals compare with an
epsilon of 1e-5, GMS 1.4's `math_get_epsilon` default. Without it, stat
checks like `sanity == 10` after float arithmetic misfire. Runtime strings are
interned forever, which is bounded by the distinct strings a game builds (score
readouts). This is marked as a ponytail shortcut in the code. `string(x)`
formats integers bare and other reals with 2 decimals, as GMS 1.4 does.

## One step

`gm_frame()`, in this order:

1. pending room change (`room_goto` takes effect here, not mid-event)
2. `xprevious`/`yprevious` = position
3. Begin Step
4. alarms: every alarm above 0 counts down, and on reaching 0 it's set to -1
   and fires (so the event can re-arm it)
5. Keyboard (held), Key Press, Key Release, then "any key" (sub 1)
6. Step
7. `gm_premove_hook` (profiles use this to steer movement, see devmenu.md)
8. friction, gravity, then `x += hspeed`, `y += vspeed`
9. Collision events. If either instance is solid, the mover goes back to its
   previous position before its event runs.
10. End Step
11. animation: `image_index += image_speed`, and wrapping fires Animation End (Other 7)
12. destroyed instances are freed

Each pass is a snapshot: instances created during a pass wait for the next one.
`game_load` swaps the whole world, so it runs after the step, not inside the
event that called it.

Draw: room colour, background layers, then visible instances by depth
(deepest first, ties by id). An instance with a Draw event runs it; otherwise
its sprite is drawn. Foreground layers go last.

## Rooms

Entering a room fires Room End on the old instances and frees the
non-persistent ones. It then creates the room's instances with their saved
ids, runs each one's Create event and then its creation code, runs the room
creation code, fires Game Start (first room only), and fires Room Start.

## Collision

Bounding box first, then precise masks when a sprite has them (Urban Pirate's
coastlines are precise masks). `image_angle` is ignored for collision, which
is marked as a ponytail shortcut in the code. Nothing in the corpus rotates a
collider.

## Audio

SDL_mixer, with 64 channels. Embedded sounds load from `data.win`, and
streamed ones (audio id -1) from the game folder. Music is decoded whole, and
idle tracks are freed when another loads, so only a few are in memory at
once. Legacy `sound_loop` / `action_sound` restart a sound instead of layering
it. `audio_sound_gain` fades per step. Volume is the sound's own volume ×
gain × the menu's effects or music slider.

## Saves

`game_save` / `game_load` and the dev menu's savestates share one format
(`gm_save_state`): globals, then every live instance with its variables. It is
**not** the YoYo runner's format, so saves from the original game don't carry
over. The files live in `%LOCALAPPDATA%\gmrecomp\<game name>\`, next to
`host.cfg` (window, volume and bindings) and `devmenu.ini` (window layout).

## Not implemented

Only what the corpus calls exists (see `funcs.txt` per game): no views, tiles,
paths, timelines, physics, surfaces, shaders or mouse events. Persistent rooms
are logged and reloaded fresh. Steam achievements report locked, and unlocks
are logged (`achievement: NAME`).
