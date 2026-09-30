"""data.win reader for GameMaker Studio 1.4 (bytecode 15).

Parses the IFF-style FORM container into plain Python objects. The layouts follow
the public format notes from UndertaleModTool; only what bytecode-15 games use is
read. See docs/format.md for the chunk layouts and the reference-chain encoding.
"""
import struct

class R:
    def __init__(self, d):
        self.d = d
    def u8(self, o): return self.d[o]
    def u16(self, o): return struct.unpack_from('<H', self.d, o)[0]
    def i16(self, o): return struct.unpack_from('<h', self.d, o)[0]
    def u32(self, o): return struct.unpack_from('<I', self.d, o)[0]
    def i32(self, o): return struct.unpack_from('<i', self.d, o)[0]
    def f32(self, o): return struct.unpack_from('<f', self.d, o)[0]
    def f64(self, o): return struct.unpack_from('<d', self.d, o)[0]
    def i64(self, o): return struct.unpack_from('<q', self.d, o)[0]
    def ptrs(self, o):
        n = self.u32(o)
        return [self.u32(o + 4 + 4 * i) for i in range(n)]
    def s(self, p):
        # String pointers point at the text; the u32 length sits just before it.
        if p == 0: return None
        n = self.u32(p - 4)
        return self.d[p:p + n].decode('utf-8', 'replace')

EVENT_TYPES = ['Create', 'Destroy', 'Alarm', 'Step', 'Collision', 'Keyboard', 'Mouse',
               'Other', 'Draw', 'KeyPress', 'KeyRelease', 'Trigger']

class GameData:
    def __init__(self, path):
        self.raw = open(path, 'rb').read()
        r = self.r = R(self.raw)
        assert self.raw[:4] == b'FORM'
        self.chunks = {}
        o = 8
        while o < len(self.raw):
            name = self.raw[o:o + 4].decode(); sz = r.u32(o + 4)
            self.chunks[name] = (o + 8, sz)
            o += 8 + sz
        self._gen8(); self._strings(); self._code(); self._vari(); self._func()
        self._sprites(); self._backgrounds(); self._fonts(); self._objects()
        self._rooms(); self._tpag(); self._sounds(); self._textures(); self._audio()

    def _gen8(self):
        r = self.r; o, _ = self.chunks['GEN8']
        self.bytecode = r.u8(o + 1)
        assert self.bytecode == 15, 'only bytecode 15 (GMS 1.4) is supported, got %d' % self.bytecode
        self.game_name = r.s(r.u32(o + 0x28))
        self.window_w = r.u32(o + 0x3c); self.window_h = r.u32(o + 0x40)
        # room order list follows the fixed GEN8 header
        self.room_order = [r.u32(o + 0x80 + 4 + 4 * i) for i in range(r.u32(o + 0x80))]

    def _strings(self):
        r = self.r; o, _ = self.chunks['STRG']
        self.strings = [r.s(p + 4) for p in r.ptrs(o)]

    def _code(self):
        r = self.r; o, _ = self.chunks['CODE']
        self.code = []
        for p in r.ptrs(o):
            length = r.u32(p + 4)
            nloc = r.u16(p + 8); nargs = r.u16(p + 10) & 0x7fff
            start = p + 12 + r.i32(p + 12) + r.u32(p + 16)
            self.code.append(dict(name=r.s(r.u32(p)), start=start, length=length,
                                  locals=nloc, args=nargs))

    def _vari(self):
        r = self.r; o, sz = self.chunks['VARI']
        self.vars = []
        self.var_at = {}          # instruction address -> var index
        p = o + 12
        while p < o + sz:
            name = r.s(r.u32(p)); itype = r.i32(p + 4); vid = r.i32(p + 8)
            occ = r.u32(p + 12); addr = r.u32(p + 16)
            idx = len(self.vars)
            self.vars.append(dict(name=name, itype=itype, id=vid))
            for _ in range(occ):
                self.var_at[addr] = idx
                addr += r.u32(addr + 4) & 0x07ffffff
            p += 20

    def _func(self):
        r = self.r; o, sz = self.chunks['FUNC']
        n = r.u32(o)
        self.funcs = []
        self.func_at = {}
        p = o + 4
        for i in range(n):
            name = r.s(r.u32(p)); occ = r.u32(p + 4); addr = r.u32(p + 8)
            for _ in range(occ):
                self.func_at[addr] = i
                addr += r.u32(addr + 4) & 0x07ffffff
            self.funcs.append(name)
            p += 12
        # code locals: per code entry, the names of its local variables
        self.code_locals = {}
        m = r.u32(p); p += 4
        for _ in range(m):
            cnt = r.u32(p); cname = r.s(r.u32(p + 4)); p += 8
            names = []
            for _ in range(cnt):
                names.append((r.u32(p), r.s(r.u32(p + 4)))); p += 8
            self.code_locals[cname] = names

    def _sprites(self):
        r = self.r; o, _ = self.chunks['SPRT']
        self.sprites = []
        for p in r.ptrs(o):
            s = dict(name=r.s(r.u32(p)), w=r.u32(p + 4), h=r.u32(p + 8),
                     bbox=(r.i32(p + 12), r.i32(p + 16), r.i32(p + 20), r.i32(p + 24)),  # L R B T
                     transparent=r.u32(p + 28), smooth=r.u32(p + 32), preload=r.u32(p + 36),
                     bboxmode=r.u32(p + 40), sepmasks=r.u32(p + 44),
                     ox=r.i32(p + 48), oy=r.i32(p + 52))
            q = p + 56
            s['frames'] = r.ptrs(q)          # TPAG entry addresses
            q += 4 + 4 * len(s['frames'])
            nmask = r.u32(q); q += 4
            stride = (s['w'] + 7) // 8
            masks = []
            for _ in range(nmask):
                masks.append(self.raw[q:q + stride * s['h']])
                q += stride * s['h']
            s['masks'] = masks
            self.sprites.append(s)

    def _backgrounds(self):
        r = self.r; o, _ = self.chunks['BGND']
        self.backgrounds = [dict(name=r.s(r.u32(p)), tpag=r.u32(p + 16)) for p in r.ptrs(o)]

    def _fonts(self):
        r = self.r; o, _ = self.chunks['FONT']
        self.fonts = []
        for p in r.ptrs(o):
            f = dict(name=r.s(r.u32(p)), display=r.s(r.u32(p + 4)), size=r.u32(p + 8),
                     bold=r.u32(p + 12), italic=r.u32(p + 16),
                     first=r.u16(p + 20), last=r.u16(p + 24), tpag=r.u32(p + 28),
                     sx=r.f32(p + 32), sy=r.f32(p + 36))
            glyphs = []
            for g in r.ptrs(p + 40):
                glyphs.append(dict(ch=r.u16(g), x=r.u16(g + 2), y=r.u16(g + 4), w=r.u16(g + 6),
                                   h=r.u16(g + 8), shift=r.i16(g + 10), offset=r.i16(g + 12)))
            f['glyphs'] = glyphs
            self.fonts.append(f)

    def _objects(self):
        r = self.r; o, _ = self.chunks['OBJT']
        self.objects = []
        for p in r.ptrs(o):
            ob = dict(name=r.s(r.u32(p)), sprite=r.i32(p + 4), visible=r.u32(p + 8),
                      solid=r.u32(p + 12), depth=r.i32(p + 16), persistent=r.u32(p + 20),
                      parent=r.i32(p + 24), mask=r.i32(p + 28))
            nverts = r.u32(p + 64)
            q = p + 80 + 8 * nverts          # skip physics block + vertices
            events = {}
            for etype, lp in enumerate(r.ptrs(q)):
                for ep in r.ptrs(lp):
                    sub = r.u32(ep)
                    codes = []
                    for ap in r.ptrs(ep + 4):
                        codes.append(dict(lib=r.u32(ap), id=r.u32(ap + 4), kind=r.u32(ap + 8),
                                          code=r.i32(ap + 32), argc=r.u32(ap + 36),
                                          who=r.i32(ap + 40), relative=r.u32(ap + 44),
                                          isnot=r.u32(ap + 48)))
                    events[(etype, sub)] = codes
            ob['events'] = events
            self.objects.append(ob)

    def _rooms(self):
        r = self.r; o, _ = self.chunks['ROOM']
        self.rooms = []
        for p in r.ptrs(o):
            rm = dict(name=r.s(r.u32(p)), caption=r.s(r.u32(p + 4)), w=r.u32(p + 8),
                      h=r.u32(p + 12), speed=r.u32(p + 16), persistent=r.u32(p + 20),
                      color=r.u32(p + 24), draw_color=r.u32(p + 28), code=r.i32(p + 32),
                      flags=r.u32(p + 36))
            bgs = []
            for b in r.ptrs(r.u32(p + 40)):
                bgs.append(dict(enabled=r.u32(b), fore=r.u32(b + 4), bg=r.i32(b + 8),
                                x=r.i32(b + 12), y=r.i32(b + 16), tilex=r.u32(b + 20),
                                tiley=r.u32(b + 24), hs=r.i32(b + 28), vs=r.i32(b + 32),
                                stretch=r.u32(b + 36)))
            views = []
            for v in r.ptrs(r.u32(p + 44)):
                views.append(dict(enabled=r.u32(v), x=r.i32(v + 4), y=r.i32(v + 8),
                                  w=r.i32(v + 12), h=r.i32(v + 16), px=r.i32(v + 20),
                                  py=r.i32(v + 24), pw=r.i32(v + 28), ph=r.i32(v + 32),
                                  bx=r.u32(v + 36), by=r.u32(v + 40), sx=r.i32(v + 44),
                                  sy=r.i32(v + 48), obj=r.i32(v + 52)))
            insts = []
            for g in r.ptrs(r.u32(p + 48)):
                insts.append(dict(x=r.i32(g), y=r.i32(g + 4), obj=r.i32(g + 8),
                                  id=r.u32(g + 12), code=r.i32(g + 16), sx=r.f32(g + 20),
                                  sy=r.f32(g + 24), color=r.u32(g + 28), rot=r.f32(g + 32)))
            tiles = []
            for t in r.ptrs(r.u32(p + 52)):
                tiles.append(dict(x=r.i32(t), y=r.i32(t + 4), bg=r.i32(t + 8), sx=r.i32(t + 12),
                                  sy=r.i32(t + 16), w=r.u32(t + 20), h=r.u32(t + 24),
                                  depth=r.i32(t + 28), id=r.u32(t + 32), scx=r.f32(t + 36),
                                  scy=r.f32(t + 40), color=r.u32(t + 44)))
            rm.update(bgs=bgs, views=views, insts=insts, tiles=tiles)
            self.rooms.append(rm)

    def _tpag(self):
        r = self.r; o, _ = self.chunks['TPAG']
        self.tpag_index = {}
        self.tpags = []
        for p in r.ptrs(o):
            self.tpag_index[p] = len(self.tpags)
            self.tpags.append(tuple(r.u16(p + 2 * i) for i in range(11)))
            # sx sy sw sh tx ty tw th bw bh tex

    def _sounds(self):
        r = self.r; o, _ = self.chunks['SOND']
        self.sounds = []
        for p in r.ptrs(o):
            self.sounds.append(dict(name=r.s(r.u32(p)), flags=r.u32(p + 4), type=r.s(r.u32(p + 8)),
                                    file=r.s(r.u32(p + 12)), volume=r.f32(p + 20),
                                    group=r.i32(p + 28), audio=r.i32(p + 32)))

    def _textures(self):
        r = self.r; o, _ = self.chunks['TXTR']
        self.textures = []
        self.texture_off = []
        ps = r.ptrs(o)
        datas = [r.u32(p + 4) for p in ps]
        end = o + self.chunks['TXTR'][1]
        for i, a in enumerate(datas):
            b = datas[i + 1] if i + 1 < len(datas) else end
            png = self.raw[a:b]
            # trim to IEND so trailing padding isn't handed to the decoder
            k = png.find(b'IEND')
            self.textures.append(png[:k + 8] if k >= 0 else png)
            self.texture_off.append(a)

    def _audio(self):
        r = self.r; o, _ = self.chunks['AUDO']
        self.audio = []
        for p in r.ptrs(o):
            n = r.u32(p)
            self.audio.append(self.raw[p + 4:p + 4 + n])
