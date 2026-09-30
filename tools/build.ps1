# build.ps1 - compile a gmrecomp output directory + the runtime into an exe (MSVC).
#
#   tools\build.ps1 -Gen <generated dir> -Out <exe path> [-Extra <dir of extra .c/.cpp>] [-NoMenu]
#
# Requires Visual Studio 2022 (any edition or Build Tools, with C++) and vcpkg (x64-windows) with sdl2, sdl2-image,
# sdl2-mixer and imgui at $VcpkgRoot. See docs/building.md.
param(
    [Parameter(Mandatory = $true)][string]$Gen,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Extra = "",
    [switch]$NoMenu,
    [string]$VcpkgRoot = "C:\vcpkg"
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$rt = Join-Path $root "runtime"
$Gen = (Resolve-Path $Gen).Path
$Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
$outDir = Split-Path $Out -Parent
$obj = Join-Path $outDir "obj"
if (Test-Path $obj) { Remove-Item -Recurse -Force $obj }
New-Item -ItemType Directory -Force $obj | Out-Null

$vi = "$VcpkgRoot\installed\x64-windows"
# any VS 2022 edition or the Build Tools, as long as the C++ tools are in
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { "" }
$vcvars = if ($vs) { Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat" } else { "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" }
if (-not (Test-Path $vcvars)) { throw "Visual Studio C++ tools not found (looked for $vcvars)" }
$inc = "/I `"$rt`" /I `"$Gen`" /I `"$vi\include\SDL2`" /I `"$vi\include`" /I `"$rt\third_party\imgui_backends`""

$csrc = @("`"$Gen\*.c`"", "`"$rt\gm_core.c`"", "`"$rt\gm_funcs.c`"", "`"$rt\gm_host.c`"")
if ($NoMenu) { $csrc += "`"$rt\devmenu_stub.c`"" }
if ($Extra) { Get-ChildItem $Extra -Filter *.c | ForEach-Object { $csrc += "`"$($_.FullName)`"" } }
# /wd4102: unreferenced labels are normal in lifted code (every branch target gets one)
$cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /c /O2 /Zi /MD /W3 /wd4102 /wd4996 /std:c11 $inc /Fo`"$obj\\`" /Fd`"$obj\\`" $($csrc -join ' ')"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "C compile failed" }

$libs = "`"$vi\lib\SDL2.lib`" `"$vi\lib\manual-link\SDL2main.lib`" `"$vi\lib\SDL2_image.lib`" `"$vi\lib\SDL2_mixer.lib`" shell32.lib"
if (-not $NoMenu) {
    $cpp = @("`"$rt\devmenu.cpp`"", "`"$rt\third_party\imgui_backends\*.cpp`"")
    $prof = @()
    if ($Extra) { $prof = @(Get-ChildItem $Extra -Filter *.cpp) }
    if ($prof.Count) { $prof | ForEach-Object { $cpp += "`"$($_.FullName)`"" } } else { $cpp += "`"$rt\profile_none.cpp`"" }
    $cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /c /O2 /Zi /MD /EHsc /W3 /wd4996 $inc /Fo`"$obj\\`" /Fd`"$obj\\`" $($cpp -join ' ')"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "C++ compile failed" }
    $libs += " `"$vi\lib\imgui.lib`""
}
$cmd = "`"$vcvars`" >nul 2>&1 && link /nologo /DEBUG /OUT:`"$Out`" `"$obj\*.obj`" $libs /SUBSYSTEM:CONSOLE"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "link failed" }
# runtime DLLs next to the exe
Get-ChildItem "$vi\bin" -Filter *.dll | Where-Object { $_.Name -match '^(SDL2|SDL2_image|SDL2_mixer|libpng16|zlib1|ogg|vorbis|vorbisfile|wavpackdll|libxmp|FLAC|opus|opusfile|mpg123|imgui)' } |
    ForEach-Object { Copy-Item $_.FullName $outDir -Force }
Write-Host "built $Out"
