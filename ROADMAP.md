# Roadmap

## Next
- More GMS 1.4 titles in the corpus. Each new title will show which built-ins are missing; add
  them to the runtime when a game calls them.
- Views, tiles and persistent rooms (no corpus title uses them yet).
- Rotated collision masks (`image_angle` is ignored for collision today).
- `Setup.cmd` in the toolkit itself; for now the game repos carry one.

## Later
- Bytecode 16 and 17 (late GMS 1.4 / early GMS 2): new instance types, `pushref`, stacktop pops.
- A replay recorder for regression runs: record inputs, then check that the replay reaches the
  same globals.

## Out of scope
- GMS 2.3+ (bytecode 17 with functions and structs) until a title needs it.
- YYC (natively compiled) games. There's no bytecode in them, so pcrecomp is the tool.
- Shipping generated code or any game data.
