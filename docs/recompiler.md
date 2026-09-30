# How bytecode becomes C

`tools/gmrecomp.py` writes three files into the output folder:

| File | Contains |
|---|---|
| `code.c` | `void gml_N(Inst *self, Inst *other)` for every code entry N |
| `tables.c` | objects and their events, rooms, sprites, texture pages, fonts, sounds, variables, `choose()` sites, file offsets of textures, audio and masks |
| `funcs.txt` | the built-in functions the game calls (the runtime must have each) |

The runtime reads texture, audio and mask bytes directly from `data.win` at the
offsets in `tables.c`. That keeps a single `data.win` parser (the Python one)
instead of a second one in C.

## One function per code entry

Straight-line translation. Each instruction becomes one C statement on a local
operand stack `Val st[STACK_MAX]`, and each branch target becomes a label.
A hand-written example of the shape (not taken from any game) for the GML
`if (hp == 1) hp = 0;` in some object's Step event:

```c
/* gml_Object_example_Step_0 */
void gml_7(Inst *self, Inst *other) {
    ...
    st[sp++] = gm_get(self, other, -1, 12);  /* hp */
    st[sp++] = R(1);
    sp--; st[sp-1] = R(gm_cmp(st[sp-1], st[sp]) == 0);
    if (!gm_truthy(st[--sp])) goto L_1040;
    st[sp++] = R(0);
    sp--; gm_set(self, other, -1, 12, st[sp]);  /* hp */
L_1040:;
```

No fetch-decode loop exists at run time, and MSVC compiles the control flow and
arithmetic natively. Variable access goes through `gm_get` / `gm_set`, because
GML resolves `obj.x` against live instances (the first instance for a read,
every instance for a write). Built-ins map to struct fields in the runtime.

Why an operand stack and not typed temporaries: the bytecode's types are
advisory (`conv.i.v` and friends), and GML values change type at run time. A
uniform `Val` stack is correct by construction. At the scale of these games
(a few thousand bytecode ops per frame) the speed is irrelevant.

## Checks at lift time

- **Stack depth on every path.** `Lifter.max_depth` walks all branches and
  fails on a join where two paths arrive with different depths. A plain linear
  sum overcounts, because short-circuit `&&`/`||` leave one value per path.
  Before this was path-aware, three of Urban Pirate's entries hit false
  depths of 68-130.
- **Branch targets inside the entry.** A branch to the byte after the last
  instruction is the normal "fall off the end" and becomes `L_end`.
- An unknown opcode or an unhandled instruction form makes the entry fail.
  The entry is then left out of `gm_code` (null), reported, and counted by the
  conformance harness.

## `with`

`pushenv` pops a target and collects the matching instance **ids** into a
`GMEnv`. It keeps ids rather than pointers, because the body can destroy or
create instances. The `popenv` at the end of the body advances to the next id
that is still alive and jumps back. `other` inside the body is the outer
`self`. `gm_env_unwind` releases any frames left open by an early exit.

## `choose()` sites

Every call to `choose` becomes `gm_choose(site, args, n)` with a fixed site
number, so the dev menu can force any site. At lift time the recompiler also
records for each site:

- the constant option values;
- the variable the result goes into (`target`, for example `round_1_points_mia`);
- per option, the object its branch creates, when the code has the shape
  `v = choose(...); if (v == K) instance_create(x, y, OBJ)`. Urban Pirate
  decides most outcomes this way (`action_dumpster_result_5` and so on), so
  the Luck window can show what each option does.

## Pseudo-GML

`python tools/gmdis.py --gml data.win [name]` prints a readable version, with
the stack folded back into expressions and `&&`/`||` recovered. For the same
hand-written example:

```
== gml_Object_example_Step_0
    if !(hp == 1) goto L1040
    hp = 0
L1040:
```

It's for reading game logic when writing a profile, and nothing is compiled
from it. Conditions with three or more `&&` terms can come out partly folded.
