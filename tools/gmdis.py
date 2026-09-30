"""GML bytecode-15 decoder and disassembler.

    python gmdis.py data.win [code-name-substring]

decode(g, entry) yields Ins objects the recompiler consumes; the listing is only a
debugging aid and is written to a gitignored path (it is derived from the game).
"""
import struct, sys
from gmdata import GameData

OPS = {0x07: 'conv', 0x08: 'mul', 0x09: 'div', 0x0A: 'rem', 0x0B: 'mod', 0x0C: 'add',
       0x0D: 'sub', 0x0E: 'and', 0x0F: 'or', 0x10: 'xor', 0x11: 'neg', 0x12: 'not',
       0x13: 'shl', 0x14: 'shr', 0x15: 'cmp', 0x45: 'pop', 0x86: 'dup', 0x9C: 'ret',
       0x9D: 'exit', 0x9E: 'popz', 0xB6: 'b', 0xB7: 'bt', 0xB8: 'bf', 0xBA: 'pushenv',
       0xBB: 'popenv', 0xC0: 'push', 0xC1: 'pushloc', 0xC2: 'pushglb', 0xC3: 'pushbltn',
       0x84: 'pushi', 0xD9: 'call', 0xFF: 'break'}
TYPES = 'd f i l b v s inst del undef ui ? ? ? ? e'.split()   # e = int16
TSIZE = {0: 2, 1: 1, 2: 1, 3: 2, 4: 1, 5: 1, 6: 1, 15: 0}
CMP = {1: 'lt', 2: 'le', 3: 'eq', 4: 'ne', 5: 'ge', 6: 'gt'}
VARKIND = {0x00: 'array', 0x80: 'stacktop', 0xA0: 'normal'}

class Ins:
    __slots__ = ('addr', 'op', 't1', 't2', 'val', 'arg', 'ref', 'kind', 'target', 'size')
    def __repr__(self):
        return '%x %s' % (self.addr, fmt(self))

def decode(g, e):
    r = g.r
    a = e['start']; end = a + e['length']
    out = []
    while a < end:
        w = r.u32(a)
        i = Ins(); i.addr = a; i.op = w >> 24; i.t1 = (w >> 16) & 0xF; i.t2 = (w >> 20) & 0xF
        i.val = w & 0xFFFF; i.arg = None; i.ref = None; i.kind = None; i.target = None
        n = 1
        op = i.op
        if op not in OPS:
            raise ValueError('%s: unknown opcode %02x at %x' % (e['name'], op, a))
        if op in (0xB6, 0xB7, 0xB8, 0xBA, 0xBB):
            off = w & 0x7FFFFF
            if off & 0x400000: off -= 0x800000
            i.target = a + off * 4
            if op == 0xBB and (w & 0xFFFFFF) == 0xF00000:
                i.target = None          # popenv drop (break out of with)
        elif op in (0xC0, 0xC1, 0xC2, 0xC3):
            n += TSIZE[i.t1]
            t = i.t1
            if t == 0: i.arg = r.f64(a + 4)
            elif t == 1: i.arg = r.f32(a + 4)
            elif t in (2, 4): i.arg = r.i32(a + 4)
            elif t == 3: i.arg = r.i64(a + 4)
            elif t == 6: i.arg = g.strings[r.u32(a + 4)]
            elif t == 15: i.arg = struct.unpack('<h', struct.pack('<H', i.val))[0]
            elif t == 5:
                i.ref = g.vars[g.var_at[a]]; i.kind = VARKIND[(r.u32(a + 4) >> 24) & 0xF8]
        elif op == 0x84:
            i.arg = struct.unpack('<h', struct.pack('<H', i.val))[0]
        elif op == 0x45:
            n = 2
            i.ref = g.vars[g.var_at[a]]; i.kind = VARKIND[(r.u32(a + 4) >> 24) & 0xF8]
        elif op == 0xD9:
            n = 2
            i.ref = g.funcs[g.func_at[a]]
        i.size = n * 4
        out.append(i)
        a += n * 4
    return out

def inst_name(v):
    v = struct.unpack('<h', struct.pack('<H', v))[0] if v > 0x7fff else v
    return {-1: 'self', -2: 'other', -3: 'all', -4: 'noone', -5: 'global', -6: 'builtin',
            -7: 'local', -9: 'stacktop'}.get(v, 'obj%d' % v)

def fmt(i):
    n = OPS[i.op]
    t = '.' + TYPES[i.t1]
    if i.op in (0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x13, 0x14, 0x15, 0x45):
        t += '.' + TYPES[i.t2]
    if i.op == 0x15: return 'cmp%s %s' % (t, CMP[i.val >> 8])
    if i.target is not None: return '%s %x' % (n, i.target)
    if i.op == 0xD9: return 'call %s(%d)' % (i.ref, i.val)
    if i.ref is not None: return '%s%s [%s]%s.%s' % (n, t, i.kind, inst_name(i.val), i.ref['name'])
    if i.arg is not None: return '%s%s %r' % (n, t, i.arg)
    if i.op == 0x86: return 'dup%s %d' % (t, i.val & 0xff)
    return n + t

def pseudo(g, e):
    """Readable pseudo-GML: the operand stack rebuilt into expressions, control
    flow kept as gotos. For reading game logic, not for recompiling."""
    ins = decode(g, e)
    targets = {i.target for i in ins if i.target is not None}
    st = []; out = []
    bop = {0x08: '*', 0x09: '/', 0x0A: 'div', 0x0B: 'mod', 0x0C: '+', 0x0D: '-', 0x0E: '&',
           0x0F: '|', 0x10: '^', 0x13: '<<', 0x14: '>>'}
    cop = {1: '<', 2: '<=', 3: '==', 4: '!=', 5: '>=', 6: '>'}
    def vname(i, inst=None):
        n = i.ref['name']
        who = inst if inst is not None else inst_name(i.val)
        if who in ('self',): return n
        return '%s.%s' % (g.objects[int(who[3:])]['name'] if who.startswith('obj') else who, n)
    at = {i.addr: k for k, i in enumerate(ins)}
    sc = []          # pending short-circuits: (op, lhs, La, Lb)
    k = 0
    while k < len(ins):
        i = ins[k]; k += 1
        o = i.op
        # A; bf La; B; b Lb; La: push.e 0/1; Lb:  ==>  (A && B) / (A || B)
        if o in (0xB7, 0xB8) and i.target in at:
            j = at[i.target]
            pe, pb = ins[j], ins[j - 1]
            if pe.op == 0xC0 and pe.t1 == 15 and pb.op == 0xB6 and pb.target == pe.addr + pe.size:
                sc.append(('&&' if o == 0xB8 else '||', st.pop(), pe.addr, pb.target))
                continue
        if o == 0xB6 and sc and sc[-1][2] == i.addr + i.size:
            op, lhs, la, lb = sc.pop()
            st.append('(%s %s %s)' % (lhs, op, st.pop()))
            k = at[lb] if lb in at else len(ins)
            continue
        if i.addr in targets and not any(i.addr == x[3] for x in sc):
            out.append('L%x:' % i.addr)
        if o in (0xC0, 0xC1, 0xC2, 0xC3):
            if i.t1 == 5:
                if i.kind == 'array':
                    idx = st.pop(); ins_ = st.pop()
                    st.append('%s[%s]' % (vname(i, inst_name(int(ins_)) if ins_.lstrip('-').isdigit() else ins_), idx))
                else: st.append(vname(i, 'global' if o == 0xC2 else None))
            else: st.append(repr(i.arg) if i.t1 == 6 else ('%g' % i.arg if isinstance(i.arg, float) else str(i.arg)))
        elif o == 0x84: st.append(str(i.arg))
        elif o == 0x45:
            if i.kind == 'array':
                idx = st.pop(); ins_ = st.pop(); v = st.pop()
                out.append('    %s[%s] = %s' % (vname(i, inst_name(int(ins_)) if ins_.lstrip('-').isdigit() else ins_), idx, v))
            else: out.append('    %s = %s' % (vname(i), st.pop()))
        elif o in bop: b = st.pop(); a = st.pop(); st.append('(%s %s %s)' % (a, bop[o], b))
        elif o == 0x15: b = st.pop(); a = st.pop(); st.append('(%s %s %s)' % (a, cop[i.val >> 8], b))
        elif o == 0x12: st.append('!' + st.pop())
        elif o == 0x11: st.append('-' + st.pop())
        elif o == 0x07: pass
        elif o == 0x9E: out.append('    ' + st.pop())
        elif o == 0xD9:
            args = [st.pop() for _ in range(i.val)]
            st.append('%s(%s)' % (i.ref, ', '.join(args)))
        elif o == 0xB6: out.append('    goto L%x' % i.target)
        elif o == 0xB7: out.append('    if %s goto L%x' % (st.pop(), i.target))
        elif o == 0xB8: out.append('    if !%s goto L%x' % (st.pop(), i.target))
        elif o == 0xBA:
            t = st.pop(); t = g.objects[int(t)]['name'] if t.isdigit() else t
            out.append('    with (%s) {' % t)
        elif o == 0xBB: out.append('    }')
        elif o == 0x86: st.extend(st[-((i.val & 0xff) + 1):])
        elif o in (0x9C, 0x9D): out.append('    exit')
    return '\n'.join(out)

def pseudo_main():
    g = GameData(sys.argv[2])
    want = sys.argv[3] if len(sys.argv) > 3 else ''
    for e in g.code:
        if want in e['name']:
            print('==', e['name']); print(pseudo(g, e))

if __name__ == '__main__' and sys.argv[1] == '--gml':
    pseudo_main(); sys.exit()
if __name__ == '__main__':
    g = GameData(sys.argv[1])
    want = sys.argv[2] if len(sys.argv) > 2 else ''
    for e in g.code:
        if want in e['name']:
            print('==', e['name'], 'locals', e['locals'], 'args', e['args'])
            for i in decode(g, e): print('  ', i)

