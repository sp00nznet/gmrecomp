# Architecture

```
data.win ──► tools/gmdata.py ──► tools/gmrecomp.py ──► <gen>/code.c, tables.c
 (yours)      parse chunks,        lift each code          one C function per
              resolve refs         entry, emit tables      code entry + tables
                                                                │
runtime/*.c, devmenu.cpp, <game repo>/profile/*.cpp ────────────┤ tools/build.ps1 (MSVC)
                                                                ▼
                                                  game.exe --game <folder with data.win>
                                                    reads textures, audio and masks
                                                    from data.win at recorded offsets
```

| Part | Language | Owns |
|---|---|---|
| `tools/gmdata.py` | Python | the only `data.win` parser |
| `tools/gmdis.py` | Python | instruction decoding (shared with the lifter), listings |
| `tools/gmrecomp.py` | Python | code generation and tables; stack checks; `choose()` analysis |
| `tools/conformance.py` | Python | corpus counts vs. baseline |
| `runtime/gm_core.c` | C | values, variables, instances, events, rooms, collision, savestates |
| `runtime/gm_funcs.c` | C | GML built-in functions |
| `runtime/gm_host.c` | C | SDL2 window, drawing, audio, input, main loop, headless capture |
| `runtime/devmenu.cpp` | C++ | ImGui menu, Globals/Instances/Luck/Controls windows, profile helpers |
| game repo `profile/` | C++ | that game's cheats (`devprofile.h`) |

The interface between generated code and runtime is `runtime/gmrt.h`. The
interface between runtime and dev menu is `runtime/gmhost.h`. The interface
between a game repo and the menu is `runtime/devprofile.h`.

Why Python writes the tables instead of the runtime parsing `data.win`: one
parser means one place to get the format wrong. The runtime only needs raw
bytes (PNG pages, WAV blobs, mask bits), and the table records where they sit.
