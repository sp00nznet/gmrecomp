# Dev menu

`runtime/devmenu.cpp`: Dear ImGui over SDL_Renderer, as a menu bar above the
game with the game scaled below it. It never covers the game except for windows
you open.

## Why it can touch everything

The game is recompiled, so its whole state is plain C data: `gm_globals[]`
for every GML global, and `Inst::vars[]` plus struct fields for every
instance. The Globals and Instances windows edit those directly. A freeze
re-writes a value before every step (`dev_before_step`).

## Hotkeys

| Key | Action |
|---|---|
| F6 / F7 | save / load savestate slot 0 |
| F8 | pause |
| F9 | step one frame |
| Tab (hold) | 4x speed |
| ` | Globals window |

The game keeps every other key, and none of these are used by Urban Pirate.

## Luck

Each `choose()` call site has a force slot (`gm_choose_force[site]`, where -1
means random). The Luck window lists every site with its option values, the
variable it assigns and the object each option creates (see recompiler.md),
along with how often it has run and what it picked last. The "ran" and
"forced" filters narrow the list.

## Controls

- **Extra keys**: any physical key can also press a game key (`key_map`).
  The list offers the keys the game actually listens for, taken from its
  KeyPress, KeyRelease and Keyboard events.
- **Gamepad**: every button maps to a game key, and the left stick acts as the
  d-pad. Defaults: A Enter, B Esc, X Space, Y E, LB Q, RB E, Start Enter, Back Esc.
- Sources are counted per key, so a pad button and a keyboard key on the same
  game key don't release each other.

Bindings are saved in `host.cfg`.

## Profiles: game-specific cheats

Anything that knows a game's variable names belongs in the game repo, not in
this toolkit. A game repo provides a profile (`devprofile.h`):

```cpp
const char *profile_title = "Cheats";   // its menu's name
void profile_menu(void);                // ImGui items in that menu
void profile_windows(void);             // extra windows
void profile_before_step(void);         // every step, before gm_frame
void profile_after_step(void);
```

and builds with `tools/build.ps1 -Extra <profile dir>`. Helpers:

| Helper | Does |
|---|---|
| `dev_get/dev_set/dev_freeze(name)` | read, write or hold a global by name |
| `dev_object(name)`, `dev_first(obj)` | find an object, then its first live instance |
| `dev_luck_prefer(code, objs, n)` | at sites in `code`, force the option whose outcome is earliest in `objs` |
| `dev_luck_value(code, dir)` | force the largest (+1) or smallest (-1) value |
| `dev_sites(code, target, out, max)`, `dev_force`, `dev_force_value` | the same, for one variable's sites |
| `gm_premove_hook` | runs after Step events and before speeds move instances |

Urban Pirate's profile (`urbanpirate/profile/profile_up.cpp`) is the worked
example: stat editors with locks, luck presets built from outcome labels,
loaded dice that follow the craps point, and a movement fix that uses
`gm_premove_hook`.

## Headless capture

`--headless --ui` renders the menu into the offscreen frame, and
`--click F:X,Y` clicks window coordinates. The toolkit's screenshots are made
this way, so the menu can be tested over RDP.
