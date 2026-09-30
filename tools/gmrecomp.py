"""gmrecomp: lift GameMaker Studio 1.4 (bytecode 15) data.win to C.

    python gmrecomp.py <data.win> -o <outdir>

Writes <outdir>/code.c (one C function per bytecode code entry) and
<outdir>/tables.c (objects, events, rooms, sprites, fonts, sounds, variables).
The runtime reads texture/audio/mask bytes straight from data.win at the file
offsets recorded in tables.c, so there is only one data.win parser (this one).

Generated output is derived from the game and is never committed.
Design notes: docs/recompiler.md.
"""
import argparse, os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
from gmdata import GameData
from gmdis import decode, OPS

BUILTINS = ['x', 'y', 'xprevious', 'yprevious', 'xstart', 'ystart', 'hspeed', 'vspeed',
            'speed', 'direction', 'friction', 'gravity', 'gravity_direction', 'sprite_index',
            'image_index', 'image_speed', 'image_xscale', 'image_yscale', 'image_angle',
            'image_alpha', 'image_blend', 'image_number', 'depth', 'visible', 'solid',
            'persistent', 'mask_index', 'object_index', 'id', 'alarm', 'room', 'room_speed',
            'room_width', 'room_height', 'keyboard_key', 'keyboard_lastkey', 'mouse_x',
            'mouse_y', 'fps', 'current_time', 'instance_count', 'bbox_left', 'bbox_right',
            'bbox_top', 'bbox_bottom', 'sprite_width', 'sprite_height', 'score', 'lives',
            'health', 'room_first', 'room_last', 'argument_relative']

def cstr(s):
    out = '"'
    for ch in s.encode('utf-8'):
        if ch in (34, 92): out += '\\' + chr(ch)
        elif 32 <= ch < 127: out += chr(ch)
        else: out += '\\%03o' % ch
    return out + '"'

def cnum(v):
    if v != v: return '(0.0/0.0)'
    if v in (float('inf'), float('-inf')): return '(%s1.0/0.0)' % ('-' if v < 0 else '')
    return repr(float(v))

BINOP = {0x08: 'gm_mul', 0x09: 'gm_div', 0x0A: 'gm_idiv', 0x0B: 'gm_mod', 0x0C: 'gm_add',
         0x0D: 'gm_sub', 0x0E: 'gm_and', 0x0F: 'gm_or', 0x10: 'gm_xor', 0x13: 'gm_shl',
         0x14: 'gm_shr'}
CMPOP = {1: '<', 2: '<=', 3: '==', 4: '!=', 5: '>=', 6: '>'}

class Lifter:
    def __init__(self, g):
        self.g = g
        self.var_index = {id(v): i for i, v in enumerate(g.vars)}
        self.funcs_used = set()
        self.choose_sites = []

    def lift(self, ci, e):
        g = self.g
        ins = decode(g, e)
        self.cur_ins = ins
        end = e['start'] + e['length']
        targets = {i.target for i in ins if i.target is not None}
        after = {}                      # popenv addr -> address after it
        for i in ins:
            if i.op == 0xBB: after[i.addr] = i.addr + i.size
        targets |= set(after.values())
        L = []
        L.append('/* %s */' % e['name'])
        L.append('void gml_%d(Inst *self, Inst *other) {' % ci)
        L.append('    Val st[STACK_MAX]; int sp = 0; GMEnv env[ENV_MAX]; int ep = 0;')
        L.append('    (void)st; (void)sp; (void)env; (void)ep;')
        deltas = {}
        for i in ins:
            if i.addr in targets: L.append('L_%x:;' % i.addr)
            c, d = self.op(ci, e, i, after)
            deltas[i.addr] = d
            L.append('    ' + c)
        maxd = self.max_depth(ins, deltas, after, end)
        L.append('L_%x:;' % end)
        L.append('    gm_env_unwind(env, ep, &self, &other);')
        L.append('}')
        if maxd > 60: raise ValueError('%s: stack depth %d' % (e['name'], maxd))
        body = '\n'.join(L)
        # labels past the end of the entry (branch-to-end) collapse onto L_end
        for t in targets:
            if t >= end and t != end:
                raise ValueError('%s: branch past end %x' % (e['name'], t))
        return body

    def max_depth(self, ins, deltas, after, end):
        # Walk every path so short-circuit joins are measured, not summed.
        idx = {i.addr: k for k, i in enumerate(ins)}
        seen = {}; work = [(ins[0].addr, 0)] if ins else []; maxd = 0
        while work:
            a, d = work.pop()
            if a >= end: continue
            if a in seen:
                if seen[a] != d: raise ValueError('stack mismatch at %x: %d vs %d' % (a, seen[a], d))
                continue
            seen[a] = d
            i = ins[idx[a]]; d2 = d + deltas[a]; maxd = max(maxd, d2)
            nxt = i.addr + i.size
            if i.op == 0xB6: work.append((i.target, d2)); continue
            if i.op in (0x9C, 0x9D): continue
            if i.op in (0xB7, 0xB8): work.append((i.target, d2))
            if i.op == 0xBA: work.append((after[i.target], d2))
            if i.op == 0xBB and i.target is not None: work.append((i.target, d2))
            work.append((nxt, d2))
        return maxd

    def choose_options(self, e, call):
        """Option values of a choose() site, each labelled with the object its
        branch creates when the code has the shape
            v = choose(...); if (v == K) instance_create(x, y, OBJ)
        so the dev menu can say "action_dumpster_result_5" instead of "5"."""
        ins = self.cur_ins
        k = next(j for j, x in enumerate(ins) if x.addr == call.addr)
        vals = []
        j = k - 1
        while j >= 0 and len(vals) < call.val:
            x = ins[j]; j -= 1
            if x.op == 0x07: continue
            if x.op == 0x84 or (x.op == 0xC0 and x.t1 in (0, 1, 2, 3, 15)): vals.append(float(x.arg))
            else: break
        if len(vals) != call.val: return [(None, None)] * call.val
        # vals[0] is the last push = first argument
        labels = {}
        nxt = ins[k + 1] if k + 1 < len(ins) else None
        if nxt is not None and nxt.op == 0x45 and nxt.kind == 'normal':
            var = nxt.ref
            for q in range(k + 2, len(ins) - 4):
                a, b, c, d = ins[q:q + 4]
                if a.op in (0xC0, 0xC2) and a.ref is var and b.op == 0x84 and c.op == 0x15                         and (c.val >> 8) == 3 and d.op == 0xB8:
                    for r in range(q + 4, len(ins)):
                        x = ins[r]
                        if x.addr >= d.target: break
                        if x.op == 0xD9 and x.ref == 'instance_create':
                            first = ins[q + 4]
                            if first.op == 0x84 and 0 <= first.arg < len(self.g.objects):
                                labels.setdefault(float(b.arg), self.g.objects[first.arg]['name'])
                            break
        return [(v, labels.get(v)) for v in vals]

    def varref(self, i):
        return self.var_index[id(i.ref)]

    def op(self, ci, e, i, after):
        o = i.op
        vi = lambda: self.varref(i)
        inst = i.val if i.val < 0x8000 else i.val - 0x10000
        if o in (0xC0, 0xC1, 0xC2, 0xC3):
            if i.t1 == 5:
                if o == 0xC2: inst = -5
                if o == 0xC1: inst = -7
                if i.kind == 'array':
                    return ('sp -= 2; st[sp] = gm_aget(self, other, st[sp], %d, st[sp+1]); sp++;  /* %s */'
                            % (vi(), i.ref['name']), -1)
                if i.kind == 'stacktop':
                    return ('st[sp-1] = gm_get(self, other, gm_inst_of(st[sp-1]), %d);  /* %s */'
                            % (vi(), i.ref['name']), 0)
                return ('st[sp++] = gm_get(self, other, %d, %d);  /* %s */' % (inst, vi(), i.ref['name']), 1)
            if i.t1 == 6: return ('st[sp++] = S(%s);' % cstr(i.arg), 1)
            return ('st[sp++] = R(%s);' % cnum(i.arg), 1)
        if o == 0x84: return ('st[sp++] = R(%d);' % i.arg, 1)
        if o == 0x45:
            if i.kind == 'array':
                return ('sp -= 3; gm_aset(self, other, st[sp+1], %d, st[sp+2], st[sp]);  /* %s */'
                        % (vi(), i.ref['name']), -3)
            if i.kind == 'stacktop':
                return ('sp -= 2; gm_set(self, other, gm_inst_of(st[sp+1]), %d, st[sp]);  /* %s */'
                        % (vi(), i.ref['name']), -2)
            return ('sp--; gm_set(self, other, %d, %d, st[sp]);  /* %s */' % (inst, vi(), i.ref['name']), -1)
        if o == 0x07:
            if i.t2 == 4: return ('st[sp-1] = R(gm_truthy(st[sp-1]));', 0)
            if i.t2 in (0, 1, 2, 3) and i.t1 in (5, 6): return ('st[sp-1] = R(gm_real(st[sp-1]));', 0)
            return ('/* conv */', 0)
        if o in BINOP:
            return ('sp--; st[sp-1] = %s(st[sp-1], st[sp]);' % BINOP[o], -1)
        if o == 0x15:
            return ('sp--; st[sp-1] = R(gm_cmp(st[sp-1], st[sp]) %s 0);' % CMPOP[i.val >> 8], -1)
        if o == 0x11: return ('st[sp-1] = R(-gm_real(st[sp-1]));', 0)
        if o == 0x12:
            if i.t1 == 4: return ('st[sp-1] = R(!gm_truthy(st[sp-1]));', 0)
            return ('st[sp-1] = R((double)(~(int64_t)gm_real(st[sp-1])));', 0)
        if o == 0x86:
            n = (i.val & 0xff) + 1
            return ('for (int k = 0; k < %d; k++) st[sp+k] = st[sp-%d+k]; sp += %d;' % (n, n, n), n)
        if o == 0x9E: return ('sp--;', -1)
        if o in (0x9C, 0x9D): return ('goto L_%x;' % (e['start'] + e['length']), 0)
        if o == 0xB6: return ('goto L_%x;' % i.target, 0)
        if o == 0xB7: return ('if (gm_truthy(st[--sp])) goto L_%x;' % i.target, -1)
        if o == 0xB8: return ('if (!gm_truthy(st[--sp])) goto L_%x;' % i.target, -1)
        if o == 0xBA:
            return ('if (!gm_env_push(&env[ep], &self, &other, st[--sp])) goto L_%x; ep++;'
                    % after[i.target], -1)
        if o == 0xBB:
            if i.target is None:
                return ('gm_env_pop(&env[--ep], &self, &other);', 0)
            return ('if (gm_env_next(&env[ep-1], &self)) goto L_%x; gm_env_pop(&env[--ep], &self, &other);'
                    % i.target, 0)
        if o == 0xD9:
            n = i.val; name = i.ref
            self.funcs_used.add(name)
            args = ' '.join('a_[%d] = st[sp-%d];' % (k, k + 1) for k in range(n))
            if name == 'choose':
                site = len(self.choose_sites)
                k = next(j for j, x in enumerate(self.cur_ins) if x.addr == i.addr)
                nx = self.cur_ins[k + 1] if k + 1 < len(self.cur_ins) else None
                target = nx.ref['name'] if nx is not None and nx.op == 0x45 and nx.kind == 'normal' else ''
                self.choose_sites.append((e['name'], i.addr, self.choose_options(e, i), target))
                return ('{ Val a_[%d]; %s sp -= %d; st[sp++] = gm_choose(%d, a_, %d); }'
                        % (max(n, 1), args, n, site, n), 1 - n)
            return ('{ Val a_[%d]; %s sp -= %d; st[sp++] = gmf_%s(self, other, a_, %d); }'
                    % (max(n, 1), args, n, name, n), 1 - n)
        if o == 0xFF: return ('/* break */', 0)
        raise ValueError('unhandled op %s' % OPS[o])

def tables(g, lifter, code_ok, datapath):
    T = ['#include "gmrt.h"', '']
    T.append('const char *gm_game_name = %s;' % cstr(g.game_name))
    T.append('const char *gm_datawin = %s;' % cstr(os.path.basename(datapath)))
    T.append('int gm_window_w = %d, gm_window_h = %d;' % (g.window_w, g.window_h))
    # code
    for ci in range(len(g.code)):
        if code_ok[ci]: T.append('void gml_%d(Inst *, Inst *);' % ci)
    T.append('const GMCode gm_code[] = {')
    for ci, e in enumerate(g.code):
        T.append('    { %s, %s },' % (cstr(e['name']), 'gml_%d' % ci if code_ok[ci] else '0'))
    T.append('};')
    T.append('const int gm_ncode = %d;' % len(g.code))
    # variables
    T.append('const GMVar gm_vars[] = {')
    for v in g.vars:
        b = BUILTINS.index(v['name']) if v['id'] == -6 and v['name'] in BUILTINS else -1
        if v['id'] == -6 and b < 0: print('warning: unknown builtin', v['name'])
        T.append('    { %s, %d, %d, %d },' % (cstr(v['name']), v['itype'], v['id'], b))
    T.append('};')
    T.append('const int gm_nvars = %d;' % len(g.vars))
    T.append('const int gm_nglobals = %d, gm_ninstvars = %d;' % (
        1 + max([v['id'] for v in g.vars if v['itype'] == -5] or [0]),
        1 + max([v['id'] for v in g.vars if v['itype'] == -1 and v['id'] >= 0] or [0])))
    T.append('const char *gm_builtin_names[] = {%s};' % ', '.join(cstr(b) for b in BUILTINS))
    # sprites
    T.append('const GMTpag gm_tpag[] = {')
    for t in g.tpags: T.append('    { %s },' % ', '.join(map(str, t)))
    T.append('};')
    T.append('const int gm_ntpag = %d;' % len(g.tpags))
    T.append('const GMSprite gm_sprites[] = {')
    fr = []
    for s in g.sprites:
        L, Rr, B, Tp = s['bbox']
        T.append('    { %s, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d },' % (
            cstr(s['name']), s['w'], s['h'], s['ox'], s['oy'], L, Tp, Rr, B,
            len(fr), len(s['frames']), len(s['masks'])))
        fr += [g.tpag_index[p] for p in s['frames']]
    T.append('};')
    T.append('const int gm_nsprites = %d;' % len(g.sprites))
    T.append('const int gm_sprite_frames[] = {%s};' % (', '.join(map(str, fr)) or '0'))
    T.append('const GMBackground gm_backgrounds[] = {')
    for b in g.backgrounds: T.append('    { %s, %d },' % (cstr(b['name']), g.tpag_index.get(b['tpag'], -1)))
    T.append('};')
    T.append('const int gm_nbackgrounds = %d;' % len(g.backgrounds))
    # fonts
    gl = []
    T.append('const GMFont gm_fonts[] = {')
    for f in g.fonts:
        T.append('    { %s, %d, %d, %d, %ff, %ff, %d, %d },' % (
            cstr(f['name']), f['size'], f['first'], g.tpag_index.get(f['tpag'], -1), f['sx'], f['sy'],
            len(gl), len(f['glyphs'])))
        gl += f['glyphs']
    T.append('};')
    T.append('const int gm_nfonts = %d;' % len(g.fonts))
    T.append('const GMGlyph gm_glyphs[] = {')
    for x in gl or [dict(ch=0, x=0, y=0, w=0, h=0, shift=0, offset=0)]:
        T.append('    { %d, %d, %d, %d, %d, %d, %d },' % (x['ch'], x['x'], x['y'], x['w'], x['h'], x['shift'], x['offset']))
    T.append('};')
    # objects + events
    ev = []
    T.append('const GMObject gm_objects[] = {')
    for o in g.objects:
        first = len(ev)
        for (t, sub), acts in sorted(o['events'].items()):
            for a in acts:
                if a['code'] >= 0: ev.append((t, sub, a['code']))
        T.append('    { %s, %d, %d, %d, %d, %d, %d, %d, %d, %d },' % (
            cstr(o['name']), o['sprite'], o['visible'], o['solid'], o['depth'], o['persistent'],
            o['parent'], o['mask'], first, len(ev) - first))
    T.append('};')
    T.append('const int gm_nobjects = %d;' % len(g.objects))
    T.append('const GMEvent gm_events[] = {')
    for t, sub, c in ev or [(0, 0, -1)]: T.append('    { %d, %d, %d },' % (t, sub, c))
    T.append('};')
    # rooms
    ri = []; rb = []
    T.append('const GMRoom gm_rooms[] = {')
    for r in g.rooms:
        bgs = [b for b in r['bgs'] if b['enabled'] and b['bg'] >= 0]
        T.append('    { %s, %d, %d, %d, %d, 0x%x, %d, %d, %d, %d, %d, %d },' % (
            cstr(r['name']), r['w'], r['h'], r['speed'], r['persistent'], r['color'], r['draw_color'],
            r['code'], len(ri), len(r['insts']), len(rb), len(bgs)))
        ri += r['insts']; rb += [(b, ) for b in bgs]
    T.append('};')
    T.append('const int gm_nrooms = %d;' % len(g.rooms))
    T.append('const GMRoomInst gm_room_insts[] = {')
    for i in ri or [dict(x=0, y=0, obj=-1, id=0, code=-1, sx=1, sy=1, color=0, rot=0)]:
        T.append('    { %d, %d, %d, %d, %d, %ff, %ff, 0x%x, %ff },' % (
            i['x'], i['y'], i['obj'], i['id'], i['code'], i['sx'], i['sy'], i['color'], i['rot']))
    T.append('};')
    T.append('const GMRoomBg gm_room_bgs[] = {')
    for (b,) in rb or [(dict(fore=0, bg=-1, x=0, y=0, tilex=0, tiley=0, hs=0, vs=0, stretch=0),)]:
        T.append('    { %d, %d, %d, %d, %d, %d, %d, %d, %d },' % (
            b['fore'], b['bg'], b['x'], b['y'], b['tilex'], b['tiley'], b['hs'], b['vs'], b['stretch']))
    T.append('};')
    T.append('const int gm_room_order[] = {%s};' % ', '.join(map(str, g.room_order)))
    T.append('const int gm_nroom_order = %d;' % len(g.room_order))
    # sounds
    T.append('const GMSound gm_sounds[] = {')
    for s in g.sounds:
        T.append('    { %s, %s, %d, %ff, %d },' % (cstr(s['name']), cstr(s['file'] or ''), s['flags'], s['volume'], s['audio']))
    T.append('};')
    T.append('const int gm_nsounds = %d;' % len(g.sounds))
    # blobs: offsets into data.win
    T.append('const GMBlob gm_textures[] = {')
    for off, t in zip(g.texture_off, g.textures): T.append('    { %d, %d },' % (off, len(t)))
    T.append('};')
    T.append('const int gm_ntextures = %d;' % len(g.textures))
    T.append('const GMBlob gm_audio[] = {')
    o, _ = g.chunks['AUDO']
    for p in g.r.ptrs(o): T.append('    { %d, %d },' % (p + 4, g.r.u32(p)))
    T.append('};')
    T.append('const int gm_naudio = %d;' % len(g.audio))
    # masks: one offset per sprite (frames are consecutive, stride*h bytes each)
    T.append('const int gm_sprite_mask_off[] = {')
    o, _ = g.chunks['SPRT']
    for p, s in zip(g.r.ptrs(o), g.sprites):
        q = p + 56; q += 4 + 4 * len(s['frames']); q += 4
        T.append('    %d,' % (q if s['masks'] else -1))
    T.append('};')
    # choose() sites for the RNG panel
    opts = []
    T.append('const GMSite gm_choose_sites[] = {')
    for name, addr, o, target in lifter.choose_sites or [('', 0, [], '')]:
        T.append('    { %s, 0x%x, %d, %d, %s },' % (cstr(name), addr, len(opts), len(o), cstr(target)))
        opts += o
    T.append('};')
    T.append('const GMSiteOpt gm_choose_opts[] = {')
    for v, lab in opts or [(0, None)]:
        T.append('    { %s, %s },' % (cnum(v) if v is not None else '(0.0/0.0)', cstr(lab) if lab else '0'))
    T.append('};')
    T.append('const int gm_nchoose_sites = %d;' % len(lifter.choose_sites))
    return '\n'.join(T) + '\n'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('datawin'); ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args()
    g = GameData(a.datawin)
    os.makedirs(a.out, exist_ok=True)
    lf = Lifter(g)
    parts = ['/* generated by gmrecomp from %s -- do not commit */' % os.path.basename(a.datawin),
             '#include "gmrt.h"', '']
    ok = []
    fails = 0
    for ci, e in enumerate(g.code):
        try:
            parts.append(lf.lift(ci, e)); ok.append(True)
        except Exception as ex:
            print('skip %s: %s' % (e['name'], ex)); ok.append(False); fails += 1
    open(os.path.join(a.out, 'code.c'), 'w').write('\n\n'.join(parts) + '\n')
    open(os.path.join(a.out, 'tables.c'), 'w').write(tables(g, lf, ok, a.datawin))
    # builtin functions the runtime must provide
    open(os.path.join(a.out, 'funcs.txt'), 'w').write('\n'.join(sorted(lf.funcs_used)) + '\n')
    print('lifted %d/%d code entries, %d functions used, %d choose() sites'
          % (len(g.code) - fails, len(g.code), len(lf.funcs_used), len(lf.choose_sites)))
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
