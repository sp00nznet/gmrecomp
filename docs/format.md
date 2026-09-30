# data.win, bytecode 15

What `tools/gmdata.py` and `tools/gmdis.py` rely on. It was checked against Urban
Pirate (GMS 1.4.1757), the only title in the corpus so far. The layouts match the
public notes in UndertaleModTool; where they differ between versions, this
page lists what bytecode 15 does.

## Container

`FORM` + u32 size, then chunks of 4-byte name + u32 size. Lists inside chunks
are `u32 count` followed by `count` absolute file offsets. A string reference
points at the text itself; its u32 length sits 4 bytes before it.

Urban Pirate's chunks:

```
GEN8 OPTN EXTN SOND AGRP SPRT BGND PATH SCPT SHDR FONT TMLN OBJT ROOM DAFL
TPAG CODE VARI FUNC STRG TXTR AUDO
```

## GEN8

The bytecode version is the byte at +1, and 15 is the only one accepted. Offsets
used: name at +0x28, build numbers at +0x2C..+0x38, window size at +0x3C/+0x40,
and the room order list at +0x80.

## CODE

Each entry is: name, length, u16 locals, u16 args (top bit is a flag), i32
relative address, u32 offset. The bytecode starts at *(address of the relative
field) + relative + offset*.

## VARI / FUNC: reference chains

Instructions that name a variable or function carry a second word that is not
an index in the file. It's the byte distance to the next instruction that uses
the same variable (low 27 bits), with the variable kind in the top byte (`& 0xF8`:
`0x00` array, `0x80` stacktop, `0xA0` normal). Each VARI entry (name, instance
type, id, occurrences, first address) and each FUNC entry (name, occurrences,
first address) gives the head of its chain. In bytecode 15 the addresses point
at the **instruction**, not its operand word. `gmdata._vari` walks every chain
into an address → variable map.

VARI's header is three u32s: global count, instance-variable count, max locals.
Built-in variables have id `-6`. Their name picks the runtime slot (the list
`BUILTINS` in `tools/gmrecomp.py`). Two variables can share a name: Urban Pirate
has a builtin `room` and also a game global `global.room`.

## Instructions

32-bit words, with the opcode in bits 24-31, type 1 in bits 16-19, type 2 in
bits 20-23, and a 16-bit value in the low half. Bytecode 15 uses the modern
opcode numbering:

| op | name | notes |
|---|---|---|
| 07 | conv | type1 → type2; only `.v.b` (truthiness) changes the value in this runtime |
| 08-14 | mul div rem mod add sub and or xor neg not shl shr | `rem` is GML `div` (truncated) |
| 15 | cmp | kind in bits 8-15: 1 lt 2 le 3 eq 4 ne 5 ge 6 gt |
| 45 | pop | value16 = instance; + reference word |
| 84 | pushi | int16 in value16 |
| 86 | dup | extra in low byte: duplicate extra+1 entries |
| 9C/9D | ret / exit | |
| 9E | popz | discard |
| B6/B7/B8 | b / bt / bf | 23-bit signed word offset from the instruction |
| BA/BB | pushenv / popenv | `with`; popenv `0xF00000` = break out |
| C0-C3 | push, pushloc, pushglb, pushbltn | operand size by type: d/l 8 bytes, f/i/b/v/s 4, e (int16) 0 |
| D9 | call | value16 = argc; + reference word |

Instance values: `-1` self, `-2` other, `-3` all, `-4` noone, `-5` global,
`-7` local, `>= 0` object index, `>= 100000` instance id.

Stack order:

- `call f(a, b, c)` pushes `c, b, a`: the first argument is on top.
- An array read `a[i]` pushes the instance, then the index, then `push.v [array]`.
- An array write `a[i] = v` pushes `v`, the instance, the index, then `pop.v.v [array]`.
  The instance field inside the instruction is ignored for arrays.
- `&&` / `||` compile to `A; bf La; B; b Lb; La: push.e 0; Lb:`, so the stack
  depth is only consistent along each path, not as a straight-line sum.
- `pushenv T` jumps to its `popenv`. The `popenv` jumps back to the body.

## Objects, rooms, assets

- OBJT: 8 fields, then a physics block with the vertex count at +64 and
  vertices from +80, then 12 event lists (Create, Destroy, Alarm, Step,
  Collision, Keyboard, Mouse, Other, Draw, KeyPress, KeyRelease, Trigger).
  Each event holds actions, and an action's code id is at +32. In Urban Pirate
  every action is compiled GML (kind 7), applied to self, not relative.
- ROOM: size, speed and colour, then lists of backgrounds, views, instances
  (x, y, obj, id, creation code, scale, colour, rotation) and tiles.
- SPRT: size, bbox (left, right, bottom, top), origin, a TPAG list, then masks
  (1 bit per pixel, MSB first, rows padded to bytes).
- TPAG: 11 u16s: source x/y/w/h, target x/y/w/h, bounding w/h, texture page.
- TXTR: `u32 scaled, u32 png offset`, and each page is a whole PNG.
- SOND / AUDO: embedded sounds are AUDO blobs (WAV). Sounds with audio id
  `-1` stream from a file next to `data.win` (Urban Pirate's `snd_*.ogg` music).
