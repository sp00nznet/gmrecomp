# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versions: [SemVer](https://semver.org/).

## [Unreleased]

### Added
- Mouse (reviewed as PR #1 in the first, private copy of this repo, which was recreated before
  going public to drop game-derived doc excerpts from history): a click taps the key of the key-listening instance under the cursor (outer thirds of a
  Left/Right selector give Left/Right), otherwise Enter or Esc. `mouse_x`/`mouse_y` builtins.
- Profile hooks `profile_click` and `profile_key` (see keys and clicks first) and
  `host_key_seq` for timed key macros.

### Changed
- `docs/recompiler.md` examples are hand-written instead of excerpts of a game's output.

### Added
- `CONTRIBUTING.md`, with "Where your code comes from" (no UndertaleModTool / GPL code).
- `tools/gmdata.py`: data.win reader for bytecode 15 (GameMaker: Studio 1.4): code, variables,
  functions with their reference chains, objects and events, rooms, sprites and masks, texture
  pages, fonts, sounds.
- `tools/gmdis.py`: bytecode listing and `--gml` pseudo-GML with `&&`/`||` folded back.
- `tools/gmrecomp.py`: one C function per code entry, constant tables, stack depth checked on
  every path, `choose()` sites recorded with option values, target variable and outcome labels.
- Runtime: values, instances, `with`, events in GMS 1.4 order, rooms, precise-mask collision,
  46 built-in functions, SDL2 drawing (sprites, fonts, backgrounds), SDL_mixer audio, keyboard
  and gamepad input, savestates shared with `game_save`/`game_load`.
- Headless mode: `--headless --record out.mp4`, `--keys`, `--shot`, `--seed`, `--room`, and
  `--ui --click` to capture and drive the dev menu offscreen.
- Dev menu (ImGui): savestates, pause/step/speed/turbo, room warp, Luck window over every
  `choose()`, key and gamepad bindings, scale/filter/fullscreen, volumes, Globals and Instances
  editors with freeze, collision boxes, and game profiles (`devprofile.h`).
- `tools/conformance.py` with a count baseline; first corpus title Urban Pirate at
  1691/1691 code entries, 46/46 functions, 14/14 built-in variables.
