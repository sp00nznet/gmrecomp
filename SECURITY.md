# Security

gmrecomp reads a GameMaker game you own (`data.win` and the files beside it). Games built with
it write saves, savestates and settings to `%LOCALAPPDATA%\gmrecomp\<game>\`. Nothing opens
network connections or handles credentials.

`data.win` is parsed as untrusted input by the recompiler (Python). A malformed file should make
it fail, never run anything.

To report a problem privately, use GitHub's "Report a vulnerability" on this repository.
