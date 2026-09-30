# Building

## Toolchain

| Tool | Version used | Notes |
|---|---|---|
| Python | 3.11 | recompiler and tools; stdlib only |
| Visual Studio | 2022 Community, C++ workload | `tools/build.ps1` calls `vcvars64.bat` from the default install path |
| vcpkg | at `C:\vcpkg`, triplet `x64-windows` | `sdl2 sdl2-image sdl2-mixer imgui` |
| ffmpeg | on `PATH` | only for `--record` |

## tools/build.ps1

```
tools\build.ps1 -Gen <generated dir> -Out <exe> [-Extra <profile dir>] [-NoMenu] [-VcpkgRoot C:\vcpkg]
```

1. C, `/O2 /MD /W3 /std:c11`: the generated `*.c` plus `gm_core.c gm_funcs.c
   gm_host.c`. `/wd4102` because lifted code has labels that no `goto`
   uses (the end of every entry, the exit of every `with`). `/MD` matches
   vcpkg's dynamic CRT.
2. C++: `devmenu.cpp`, the vendored ImGui SDL2 and SDLRenderer2 backends
   (`runtime/third_party/imgui_backends`), and the profile, which is the
   `-Extra` dir's `*.cpp` or else `profile_none.cpp`.
3. Link against `SDL2 SDL2main SDL2_image SDL2_mixer imgui`, with the console
   subsystem so the log shows. The SDL DLLs are copied next to the exe.

`-NoMenu` swaps in `devmenu_stub.c` and needs no ImGui.

## Paths

`-Out` is resolved against the PowerShell location, not the process working
directory. The two differ once a script has done `cd`, and the first version
wrote the objects into one folder and linked from another (`LNK1104`).
