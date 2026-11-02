"""Static recompiler: Cell PPU (PowerPC 64, big-endian) machine code from EBOOT.elf -> C.

Each guest function becomes `void f_XXXXXXXX(ctx_t *c)` (see runtime.h). Within a function,
branch targets become labels; branches leaving the function become calls (bl) or tail calls (b).
Indirect calls (bctrl/blrl) and out-of-function bctr go through `dispatch()`. A bctr inside a
function (jump tables) becomes a switch over every instruction address of that function.
Calls to PS3 import stubs become `hle_call(c, "<Library::name>")`. `sc` calls `hle_syscall`.
Instructions that aren't translated emit `unimpl(c, addr, insn)`, which stops with the address.

Usage: python -I ppu2c.py <EBOOT.elf> <functions.tsv> <out dir> <lo> <hi>
  Recompiles every function with entry in [lo, hi) plus the closure of their direct callees.
"""
import bisect
import os
import struct
import sys

TEXT_LO, TEXT_HI = 0x10200, 0xD30A28


# ----------------------------------------------------------------------------- image / functions

class Image:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        phoff, = struct.unpack('>Q', d[0x20:0x28])
        phnum, = struct.unpack('>H', d[0x38:0x3a])
        self.segs = []
        for i in range(phnum):
            t, _, off, va, _, fsz, msz, _ = struct.unpack('>IIQQQQQQ', d[phoff + i * 56:phoff + i * 56 + 56])
            if t == 1 and msz:
                self.segs.append((va, off, fsz, msz))

    def u32(self, va):
        for sva, off, fsz, _ in self.segs:
            if sva <= va < sva + fsz - 3:
                return struct.unpack_from('>I', self.d, off + va - sva)[0]
        return None


def load_functions(path):
    funcs = []
    with open(path, encoding='utf-8', errors='replace') as f:
        next(f)
        for line in f:
            c = line.rstrip('\n').split('\t')
            funcs.append((int(c[0], 16), c[1], [int(x, 16) for x in c[3].split(',') if x]))
    funcs.sort()
    return funcs


# ----------------------------------------------------------------------------- decoding helpers

def bits(x, a, b):
    """PowerPC bit numbering: bits a..b (0 = MSB) of a 32-bit word."""
    return (x >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def mask64(mb, me):
    """PowerPC MASK(mb, me) over 64 bits (bit 0 = MSB); wraps when mb > me."""
    def m(a, b):
        return ((1 << (64 - a)) - 1) & ~((1 << (63 - b)) - 1) & 0xFFFFFFFFFFFFFFFF
    if mb <= me:
        return m(mb, me)
    return (m(mb, 63) | m(0, me)) & 0xFFFFFFFFFFFFFFFF


def ra0(ra):
    return '0' if ra == 0 else f'R({ra})'


class Fn:
    def __init__(self, entry, end, name):
        self.entry, self.end, self.name = entry, end, name
        self.labels = set()
        self.has_bctr = False


# ----------------------------------------------------------------------------- translator

class Translator:
    def __init__(self, img, funcs, stub_names):
        self.img = img
        self.entries = {a for a, _, _ in funcs}
        self.names = {a: n for a, n, _ in funcs}
        self.stub_names = stub_names  # import stub address -> 'Library::name'
        self.unhandled = {}

    # --- per-instruction emission (returns C statement string)
    def cr0(self, expr):
        return f'set_cr0(c, (int64_t)({expr}));'

    def insn(self, fn, a, w):
        op = bits(w, 0, 5)
        rt, ra, rb = bits(w, 6, 10), bits(w, 11, 15), bits(w, 16, 20)
        rs = rt
        d16 = sext(w & 0xFFFF, 16)
        u16 = w & 0xFFFF
        rc = w & 1
        R = lambda n: f'R({n})'  # noqa: E731
        ea_d = f'(uint32_t)({ra0(ra)} + {d16})'

        if op == 14:   # addi / li
            return f'{R(rt)} = {ra0(ra)} + (int64_t){d16};'
        if op == 15:   # addis / lis
            return f'{R(rt)} = {ra0(ra)} + ((int64_t){d16} << 16);'
        if op == 12 or op == 13:  # addic(.)
            s = f'{{ uint64_t a_ = {R(ra)}; {R(rt)} = a_ + (int64_t){d16}; XER_CA = {R(rt)} < a_; }}'
            return s + (self.cr0(R(rt)) if op == 13 else '')
        if op == 8:    # subfic
            return f'{{ uint64_t a_ = {R(ra)}; {R(rt)} = (uint64_t)(int64_t){d16} - a_; XER_CA = a_ <= (uint64_t)(int64_t){d16} || a_ == 0; }}'
        if op == 7:    # mulli
            return f'{R(rt)} = (uint64_t)((int64_t){R(ra)} * (int64_t){d16});'
        if op == 11:   # cmpi
            bf, L = bits(w, 6, 8), bits(w, 10, 10)
            lhs = f'(int64_t){R(ra)}' if L else f'(int64_t)(int32_t){R(ra)}'
            return f'set_crf_s(c, {bf}, {lhs}, (int64_t){d16});'
        if op == 10:   # cmpli
            bf, L = bits(w, 6, 8), bits(w, 10, 10)
            lhs = f'{R(ra)}' if L else f'(uint64_t)(uint32_t){R(ra)}'
            return f'set_crf_u(c, {bf}, {lhs}, (uint64_t){u16});'
        if op == 24:
            return f'{R(ra)} = {R(rs)} | {u16}ULL;'
        if op == 25:
            return f'{R(ra)} = {R(rs)} | ({u16}ULL << 16);'
        if op == 26:
            return f'{R(ra)} = {R(rs)} ^ {u16}ULL;'
        if op == 27:
            return f'{R(ra)} = {R(rs)} ^ ({u16}ULL << 16);'
        if op == 28:
            return f'{R(ra)} = {R(rs)} & {u16}ULL; ' + self.cr0(R(ra))
        if op == 29:
            return f'{R(ra)} = {R(rs)} & ({u16}ULL << 16); ' + self.cr0(R(ra))
        if op in (20, 21, 23):  # rlwimi / rlwinm / rlwnm
            sh, mb, me = bits(w, 16, 20), bits(w, 21, 25), bits(w, 26, 30)
            m = mask64(mb + 32, me + 32)
            rot = f'rotl32d({R(rs)}, {sh})' if op != 23 else f'rotl32d({R(rs)}, (unsigned)({R(rb)} & 31))'
            if op == 20:
                s = f'{R(ra)} = ({rot} & 0x{m:x}ULL) | ({R(ra)} & ~0x{m:x}ULL);'
            else:
                s = f'{R(ra)} = {rot} & 0x{m:x}ULL;'
            return s + (self.cr0(R(ra)) if rc else '')
        if op == 30:
            return self.op30(w, rs, ra, rb, rc)
        if op == 4:
            return self.op4(w)
        if op == 31:
            return self.op31(fn, a, w, rt, ra, rb, rc)
        loads = {32: ('ld_u32', 4, False), 33: ('ld_u32', 4, True), 34: ('ld_u8', 1, False),
                 35: ('ld_u8', 1, True), 40: ('ld_u16', 2, False), 41: ('ld_u16', 2, True),
                 42: ('ld_s16', 2, False), 43: ('ld_s16', 2, True)}
        if op in loads:
            fn_, _, upd = loads[op]
            if upd:
                return f'{{ uint32_t ea_ = {ea_d}; {R(rt)} = (uint64_t){fn_}(ea_); {R(ra)} = ea_; }}'
            return f'{R(rt)} = (uint64_t){fn_}({ea_d});'
        stores = {36: ('st_u32', False), 37: ('st_u32', True), 38: ('st_u8', False), 39: ('st_u8', True),
                  44: ('st_u16', False), 45: ('st_u16', True)}
        if op in stores:
            fn_, upd = stores[op]
            if upd:
                return f'{{ uint32_t ea_ = {ea_d}; {fn_}(ea_, {R(rs)}); {R(ra)} = ea_; }}'
            return f'{fn_}({ea_d}, {R(rs)});'
        if op == 46:   # lmw
            return f'{{ uint32_t ea_ = {ea_d}; for (int i_ = {rt}; i_ < 32; i_++, ea_ += 4) c->r[i_] = ld_u32(ea_); }}'
        if op == 47:   # stmw
            return f'{{ uint32_t ea_ = {ea_d}; for (int i_ = {rs}; i_ < 32; i_++, ea_ += 4) st_u32(ea_, c->r[i_]); }}'
        if op in (48, 49):  # lfs(u)
            s = f'{{ uint32_t ea_ = {ea_d}; F({rt}) = (double)ld_f32(ea_);'
            return s + (f' {R(ra)} = ea_; }}' if op == 49 else ' }')
        if op in (50, 51):  # lfd(u)
            s = f'{{ uint32_t ea_ = {ea_d}; FU({rt}) = ld_u64(ea_);'
            return s + (f' {R(ra)} = ea_; }}' if op == 51 else ' }')
        if op in (52, 53):  # stfs(u)
            s = f'{{ uint32_t ea_ = {ea_d}; st_f32(ea_, (float)F({rs}));'
            return s + (f' {R(ra)} = ea_; }}' if op == 53 else ' }')
        if op in (54, 55):  # stfd(u)
            s = f'{{ uint32_t ea_ = {ea_d}; st_u64(ea_, FU({rs}));'
            return s + (f' {R(ra)} = ea_; }}' if op == 55 else ' }')
        if op == 58:   # ld / ldu / lwa
            ds = sext(w & 0xFFFC, 16)
            xo = w & 3
            ea = f'(uint32_t)({ra0(ra)} + {ds})'
            if xo == 0:
                return f'{R(rt)} = ld_u64({ea});'
            if xo == 1:
                return f'{{ uint32_t ea_ = {ea}; {R(rt)} = ld_u64(ea_); {R(ra)} = ea_; }}'
            if xo == 2:
                return f'{R(rt)} = (uint64_t)(int64_t)(int32_t)ld_u32({ea});'
        if op == 62:   # std / stdu
            ds = sext(w & 0xFFFC, 16)
            xo = w & 3
            ea = f'(uint32_t)({ra0(ra)} + {ds})'
            if xo == 0:
                return f'st_u64({ea}, {R(rs)});'
            if xo == 1:
                return f'{{ uint32_t ea_ = {ea}; st_u64(ea_, {R(rs)}); {R(ra)} = ea_; }}'
        if op == 59:
            return self.op59(w, rt, ra, rb, rc)
        if op == 63:
            return self.op63(w, rt, ra, rb, rc)
        if op == 17:   # sc
            return 'hle_syscall(c);'
        if op == 2 or op == 3:  # tdi / twi
            return f'/* trap-imm {w:08x} ignored unless always */' + ('trap_always(c, 0x%x);' % a if bits(w, 6, 10) == 31 else '')
        return None

    def op30(self, w, rs, ra, rb, rc):
        R = lambda n: f'R({n})'  # noqa: E731
        xo3 = bits(w, 27, 29)
        sh = bits(w, 16, 20) | (bits(w, 30, 30) << 5)
        mbe = bits(w, 21, 26)
        mbe = (mbe >> 1) | ((mbe & 1) << 5)
        if xo3 == 0:   # rldicl
            s = f'{R(ra)} = rotl64({R(rs)}, {sh}) & 0x{mask64(mbe, 63):x}ULL;'
        elif xo3 == 1:  # rldicr
            s = f'{R(ra)} = rotl64({R(rs)}, {sh}) & 0x{mask64(0, mbe):x}ULL;'
        elif xo3 == 2:  # rldic
            s = f'{R(ra)} = rotl64({R(rs)}, {sh}) & 0x{mask64(mbe, 63 - sh):x}ULL;'
        elif xo3 == 3:  # rldimi
            m = mask64(mbe, 63 - sh)
            s = f'{R(ra)} = (rotl64({R(rs)}, {sh}) & 0x{m:x}ULL) | ({R(ra)} & ~0x{m:x}ULL);'
        else:
            xo4 = bits(w, 27, 30)
            if xo4 == 8:    # rldcl
                s = f'{R(ra)} = rotl64({R(rs)}, (unsigned)({R(rb)} & 63)) & 0x{mask64(mbe, 63):x}ULL;'
            elif xo4 == 9:  # rldcr
                s = f'{R(ra)} = rotl64({R(rs)}, (unsigned)({R(rb)} & 63)) & 0x{mask64(0, mbe):x}ULL;'
            else:
                return None
        return s + (self.cr0(R(ra)) if rc else '')

    def op31(self, fn, a, w, rt, ra, rb, rc):
        R = lambda n: f'R({n})'  # noqa: E731
        xo = bits(w, 21, 30)
        xo9 = bits(w, 22, 30)
        rs = rt
        ea_x = f'(uint32_t)({ra0(ra)} + {R(rb)})'
        cr = lambda reg: self.cr0(reg) if rc else ''  # noqa: E731
        # XO-form arithmetic (OE bit ignored: no overflow tracking)
        arith = {
            266: f'{R(rt)} = {R(ra)} + {R(rb)};',
            40: f'{R(rt)} = {R(rb)} - {R(ra)};',
            10: f'{{ uint64_t a_ = {R(ra)}; {R(rt)} = a_ + {R(rb)}; XER_CA = {R(rt)} < a_; }}',
            8: f'{{ uint64_t a_ = {R(ra)}, b_ = {R(rb)}; {R(rt)} = b_ - a_; XER_CA = b_ >= a_; }}',
            138: f'{{ uint64_t a_ = {R(ra)}, b_ = {R(rb)}, ci_ = XER_CA; {R(rt)} = a_ + b_ + ci_; XER_CA = ci_ ? ({R(rt)} <= a_) : ({R(rt)} < a_); }}',
            136: f'{{ uint64_t a_ = ~{R(ra)}, b_ = {R(rb)}, ci_ = XER_CA; {R(rt)} = a_ + b_ + ci_; XER_CA = ci_ ? ({R(rt)} <= a_) : ({R(rt)} < a_); }}',
            202: f'{{ uint64_t a_ = {R(ra)}; {R(rt)} = a_ + XER_CA; XER_CA = {R(rt)} < a_; }}',
            200: f'{{ uint64_t a_ = ~{R(ra)}; {R(rt)} = a_ + XER_CA; XER_CA = {R(rt)} < a_ || (XER_CA && a_ == ~0ULL); }}',
            234: f'{{ uint64_t a_ = {R(ra)}, ci_ = XER_CA; {R(rt)} = a_ + ci_ - 1; XER_CA = a_ != 0 || ci_; }}',
            232: f'{{ uint64_t a_ = ~{R(ra)}, ci_ = XER_CA; {R(rt)} = a_ + ci_ - 1; XER_CA = a_ != 0 || ci_; }}',
            104: f'{R(rt)} = (uint64_t)(-(int64_t){R(ra)});',
            235: f'{R(rt)} = (uint64_t)((int64_t)(int32_t){R(ra)} * (int64_t)(int32_t){R(rb)});',
            233: f'{R(rt)} = (uint64_t)((int64_t){R(ra)} * (int64_t){R(rb)});',
            75: f'{R(rt)} = (uint64_t)(uint32_t)(((int64_t)(int32_t){R(ra)} * (int64_t)(int32_t){R(rb)}) >> 32);',
            11: f'{R(rt)} = (uint64_t)(uint32_t)(((uint64_t)(uint32_t){R(ra)} * (uint64_t)(uint32_t){R(rb)}) >> 32);',
            73: f'{R(rt)} = (uint64_t)(((__int128)(int64_t){R(ra)} * (int64_t){R(rb)}) >> 64);',
            9: f'{R(rt)} = (uint64_t)(((unsigned __int128){R(ra)} * {R(rb)}) >> 64);',
            491: f'{R(rt)} = (uint64_t)(uint32_t)div_s32((int32_t){R(ra)}, (int32_t){R(rb)});',
            459: f'{R(rt)} = (uint64_t)div_u32((uint32_t){R(ra)}, (uint32_t){R(rb)});',
            489: f'{R(rt)} = (uint64_t)div_s64((int64_t){R(ra)}, (int64_t){R(rb)});',
            457: f'{R(rt)} = div_u64({R(ra)}, {R(rb)});',
        }
        if xo9 in arith:
            # XO-form: bit 21 is OE (overflow enable, not modelled), so the 9-bit XO decides.
            # No X-form opcode in use has a low 9-bit field equal to one of these codes.
            return arith[xo9] + cr(R(rt))
        logic = {
            28: f'{R(ra)} = {R(rs)} & {R(rb)};', 60: f'{R(ra)} = {R(rs)} & ~{R(rb)};',
            444: f'{R(ra)} = {R(rs)} | {R(rb)};', 412: f'{R(ra)} = {R(rs)} | ~{R(rb)};',
            316: f'{R(ra)} = {R(rs)} ^ {R(rb)};', 124: f'{R(ra)} = ~({R(rs)} | {R(rb)});',
            476: f'{R(ra)} = ~({R(rs)} & {R(rb)});', 284: f'{R(ra)} = ~({R(rs)} ^ {R(rb)});',
            954: f'{R(ra)} = (uint64_t)(int64_t)(int8_t){R(rs)};',
            922: f'{R(ra)} = (uint64_t)(int64_t)(int16_t){R(rs)};',
            986: f'{R(ra)} = (uint64_t)(int64_t)(int32_t){R(rs)};',
            26: f'{R(ra)} = (uint64_t)clz32((uint32_t){R(rs)});',
            58: f'{R(ra)} = (uint64_t)clz64({R(rs)});',
            24: f'{R(ra)} = ({R(rb)} & 32) ? 0 : (uint64_t)(uint32_t)((uint32_t){R(rs)} << ({R(rb)} & 31));',
            536: f'{R(ra)} = ({R(rb)} & 32) ? 0 : (uint64_t)((uint32_t){R(rs)} >> ({R(rb)} & 31));',
            27: f'{R(ra)} = ({R(rb)} & 64) ? 0 : {R(rs)} << ({R(rb)} & 63);',
            539: f'{R(ra)} = ({R(rb)} & 64) ? 0 : {R(rs)} >> ({R(rb)} & 63);',
            792: f'sraw(c, {ra}, {rs}, (unsigned)({R(rb)} & 63));',
            794: f'srad(c, {ra}, {rs}, (unsigned)({R(rb)} & 127));',
            824: f'sraw(c, {ra}, {rs}, {rb});',
        }
        if xo in logic:
            return logic[xo] + cr(R(ra))
        if xo in (826, 827):   # sradi
            sh = rb | (bits(w, 30, 30) << 5)
            return f'srad(c, {ra}, {rs}, {sh});' + cr(R(ra))
        if xo == 0 or xo == 32:  # cmp / cmpl
            bf, L = bits(w, 6, 8), bits(w, 10, 10)
            if xo == 0:
                l_, r_ = ((f'(int64_t){R(ra)}', f'(int64_t){R(rb)}') if L else
                          (f'(int64_t)(int32_t){R(ra)}', f'(int64_t)(int32_t){R(rb)}'))
                return f'set_crf_s(c, {bf}, {l_}, {r_});'
            l_, r_ = ((R(ra), R(rb)) if L else (f'(uint64_t)(uint32_t){R(ra)}', f'(uint64_t)(uint32_t){R(rb)}'))
            return f'set_crf_u(c, {bf}, {l_}, {r_});'
        xloads = {23: 'ld_u32', 55: 'ld_u32', 87: 'ld_u8', 119: 'ld_u8', 279: 'ld_u16', 311: 'ld_u16',
                  343: 'ld_s16', 375: 'ld_s16', 21: 'ld_u64', 53: 'ld_u64',
                  341: 'ld_s32', 373: 'ld_s32', 534: 'ld_u32le', 790: 'ld_u16le'}
        if xo in xloads:
            upd = xo in (55, 119, 311, 375, 53, 373)
            fn_ = xloads[xo]
            if upd:
                return f'{{ uint32_t ea_ = {ea_x}; {R(rt)} = (uint64_t){fn_}(ea_); {R(ra)} = ea_; }}'
            return f'{R(rt)} = (uint64_t){fn_}({ea_x});'
        xstores = {151: 'st_u32', 183: 'st_u32', 215: 'st_u8', 247: 'st_u8', 407: 'st_u16', 439: 'st_u16',
                   149: 'st_u64', 181: 'st_u64', 662: 'st_u32le', 918: 'st_u16le'}
        if xo in xstores:
            upd = xo in (183, 247, 439, 181)
            fn_ = xstores[xo]
            if upd:
                return f'{{ uint32_t ea_ = {ea_x}; {fn_}(ea_, {R(rs)}); {R(ra)} = ea_; }}'
            return f'{fn_}({ea_x}, {R(rs)});'
        vmem = {6: 'v_lvsl(c, {t}, {ea});', 38: 'v_lvsr(c, {t}, {ea});',
                7: 'v_lve(c, {t}, {ea}, 1);', 39: 'v_lve(c, {t}, {ea}, 2);', 71: 'v_lve(c, {t}, {ea}, 4);',
                135: 'v_stve(c, {t}, {ea}, 1);', 167: 'v_stve(c, {t}, {ea}, 2);', 199: 'v_stve(c, {t}, {ea}, 4);',
                519: 'v_lvlx(c, {t}, {ea});', 775: 'v_lvlx(c, {t}, {ea});',
                551: 'v_lvrx(c, {t}, {ea});', 807: 'v_lvrx(c, {t}, {ea});',
                647: 'v_stvlx(c, {t}, {ea});', 903: 'v_stvlx(c, {t}, {ea});',
                679: 'v_stvrx(c, {t}, {ea});', 935: 'v_stvrx(c, {t}, {ea});'}
        if xo in vmem:   # AltiVec/Cell vector memory forms (vmx.h)
            return vmem[xo].format(t=rt, ea=ea_x)
        if xo in (103, 359):   # lvx / lvxl: 16-byte aligned vector load
            return f'memcpy(c->v[{rt}], gp((uint32_t)({ra0(ra)} + {R(rb)}) & ~15u), 16);'
        if xo in (231, 487):   # stvx / stvxl
            return f'memcpy(gp((uint32_t)({ra0(ra)} + {R(rb)}) & ~15u), c->v[{rs}], 16);'
        if xo == 20:    # lwarx: load and reserve
            return f'{R(rt)} = lwarx(c, {ea_x});'
        if xo == 84:    # ldarx
            return f'{R(rt)} = ldarx(c, {ea_x});'
        if xo == 150:   # stwcx.: atomic compare-and-swap against the reserved value; CR0.EQ = success
            return f'set_cr_field(c, 0, (stwcx(c, {ea_x}, (uint32_t){R(rs)}) ? 0x2 : 0) | XER_SO);'
        if xo == 214:   # stdcx.
            return f'set_cr_field(c, 0, (stdcx(c, {ea_x}, {R(rs)}) ? 0x2 : 0) | XER_SO);'
        fx = {535: ('lfs', False), 567: ('lfs', True), 599: ('lfd', False), 631: ('lfd', True),
              663: ('stfs', False), 695: ('stfs', True), 727: ('stfd', False), 759: ('stfd', True)}
        if xo in fx:
            kind, upd = fx[xo]
            body = {'lfs': f'F({rt}) = (double)ld_f32(ea_);', 'lfd': f'FU({rt}) = ld_u64(ea_);',
                    'stfs': f'st_f32(ea_, (float)F({rs}));', 'stfd': f'st_u64(ea_, FU({rs}));'}[kind]
            return f'{{ uint32_t ea_ = {ea_x}; {body}' + (f' {R(ra)} = ea_; }}' if upd else ' }')
        if xo == 983:   # stfiwx
            return f'st_u32({ea_x}, (uint32_t)FU({rs}));'
        if xo == 339:   # mfspr
            spr = bits(w, 16, 20) << 5 | bits(w, 11, 15)
            src = {1: 'XER_GET(c)', 8: 'c->lr', 9: 'c->ctr', 256: 'c->vrsave', 268: 'host_timebase()'}.get(spr)
            return f'{R(rt)} = {src};' if src else None
        if xo == 467:   # mtspr
            spr = bits(w, 16, 20) << 5 | bits(w, 11, 15)
            dst = {1: f'XER_SET(c, {R(rs)});', 8: f'c->lr = {R(rs)};', 9: f'c->ctr = {R(rs)};',
                   256: f'c->vrsave = (uint32_t){R(rs)};'}.get(spr)
            return dst
        if xo == 371:   # mftb
            return f'{R(rt)} = host_timebase();'
        if xo == 19:    # mfcr / mfocrf
            return f'{R(rt)} = c->cr;'
        if xo == 144:   # mtcrf / mtocrf
            fxm = bits(w, 12, 19)
            m = 0
            for i in range(8):
                if fxm & (0x80 >> i):
                    m |= 0xF << (28 - 4 * i)
            return f'c->cr = (c->cr & ~0x{m:x}u) | ((uint32_t){R(rs)} & 0x{m:x}u);'
        if xo in (54, 86, 246, 278, 470, 598, 854, 982):  # dcbst dcbf dcbtst dcbt dcbi sync eieio icbi
            return '/* cache/sync */'
        if xo == 1014:  # dcbz: zero a 128-byte cache line
            return f'mem_zero((uint32_t)({ra0(ra)} + {R(rb)}) & ~127u, 128);'
        if xo in (4, 68):  # tw / td
            return '/* trap (conditional) ignored */' if rt != 31 else f'trap_always(c, 0x{a:x});'
        if xo == 83:
            return f'{R(rt)} = 0x8000000000000000ULL; /* mfmsr: SF=1 */'
        return None

    # AltiVec (opcode 4): helpers live in vmx.h
    VA_OPS = {46: ('v_vmaddfp', 'acb'), 47: ('v_vnmsubfp', 'acb'), 43: ('v_vperm', 'abc'), 42: ('v_vsel', 'abc'),
              40: ('v_vmsumshm', 'abc'), 38: ('v_vmsumuhm', 'abc')}
    VX_BIN = {10: 'vaddfp', 74: 'vsubfp', 1034: 'vmaxfp', 1098: 'vminfp', 128: 'vadduwm', 1152: 'vsubuwm',
              640: 'vadduws', 1664: 'vsubuws', 896: 'vaddsws', 1920: 'vsubsws', 898: 'vminsw', 386: 'vmaxsw',
              642: 'vminuw', 130: 'vmaxuw', 388: 'vslw', 644: 'vsrw', 900: 'vsraw', 132: 'vrlw', 64: 'vadduhm',
              1088: 'vsubuhm', 576: 'vadduhs', 324: 'vslh', 580: 'vsrh', 1028: 'vand', 1092: 'vandc', 1156: 'vor',
              1284: 'vnor', 1220: 'vxor', 0: 'vaddubm', 1024: 'vsububm', 140: 'vmrghw', 396: 'vmrglw',
              76: 'vmrghh', 332: 'vmrglh', 12: 'vmrghb', 268: 'vmrglb', 78: 'vpkuwum', 14: 'vpkuhum',
              328: 'vmulosh', 72: 'vmulouh', 1928: 'vsumsws'}
    VX_UN = {266: 'vrefp', 330: 'vrsqrtefp', 394: 'vexptefp', 458: 'vlogefp', 590: 'vupkhsh', 718: 'vupklsh'}
    VX_RFI = {522: 0, 586: 1, 650: 2, 714: 3}
    VX_UIMM = {842: 'vcfsx', 778: 'vcfux', 970: 'vctsxs', 906: 'vctuxs', 652: 'vspltw', 588: 'vsplth', 524: 'vspltb'}
    VX_SIMM = {908: 'vspltisw', 844: 'vspltish', 780: 'vspltisb'}
    VC_OPS = {198: 'vcmpeqfp', 454: 'vcmpgefp', 710: 'vcmpgtfp', 966: 'vcmpbfp', 134: 'vcmpequw', 646: 'vcmpgtuw',
              902: 'vcmpgtsw', 582: 'vcmpgtuh', 6: 'vcmpequb'}

    def op4(self, w):
        vd, va, vb, vc = bits(w, 6, 10), bits(w, 11, 15), bits(w, 16, 20), bits(w, 21, 25)
        x6 = w & 0x3F
        if x6 == 44:   # vsldoi
            return f'v_vsldoi(c, {vd}, {va}, {vb}, {bits(w, 22, 25)});'
        if x6 in self.VA_OPS:
            fn, order = self.VA_OPS[x6]
            regs = {'a': va, 'b': vb, 'c': vc}
            return f'{fn}(c, {vd}, {", ".join(str(regs[k]) for k in order)});'
        if (w & 0x3FF) in self.VC_OPS:
            return f'v_{self.VC_OPS[w & 0x3FF]}(c, {vd}, {va}, {vb}, {1 if w & 0x400 else 0});'
        xo = w & 0x7FF
        if xo in self.VX_BIN:
            return f'v_{self.VX_BIN[xo]}(c, {vd}, {va}, {vb});'
        if xo in self.VX_UN:
            return f'v_{self.VX_UN[xo]}(c, {vd}, {vb});'
        if xo in self.VX_RFI:
            return f'v_vrfi(c, {vd}, {vb}, {self.VX_RFI[xo]});'
        if xo in self.VX_UIMM:
            return f'v_{self.VX_UIMM[xo]}(c, {vd}, {vb}, {va});'
        if xo in self.VX_SIMM:
            return f'v_{self.VX_SIMM[xo]}(c, {vd}, {sext(va, 5)});'
        if xo == 1540:   # mfvscr
            return f'memset(c->v[{vd}], 0, 16);'
        if xo == 1604:   # mtvscr
            return '/* mtvscr */'
        return None

    def op59(self, w, rt, ra, rb, rc):
        rcf = bits(w, 21, 25)
        xo5 = bits(w, 26, 30)
        F = lambda n: f'F({n})'  # noqa: E731
        s = {
            18: f'F({rt}) = (double)(float)({F(ra)} / {F(rb)});',
            20: f'F({rt}) = (double)(float)({F(ra)} - {F(rb)});',
            21: f'F({rt}) = (double)(float)({F(ra)} + {F(rb)});',
            22: f'F({rt}) = (double)(float)sqrt({F(rb)});',
            24: f'F({rt}) = (double)(float)(1.0 / {F(rb)});',
            25: f'F({rt}) = (double)(float)({F(ra)} * {F(rcf)});',
            28: f'F({rt}) = (double)(float)({F(ra)} * {F(rcf)} - {F(rb)});',
            29: f'F({rt}) = (double)(float)({F(ra)} * {F(rcf)} + {F(rb)});',
            30: f'F({rt}) = (double)(float)(-({F(ra)} * {F(rcf)} - {F(rb)}));',
            31: f'F({rt}) = (double)(float)(-({F(ra)} * {F(rcf)} + {F(rb)}));',
        }.get(xo5)
        if s is None:
            return None
        return s + (f' set_cr1(c);' if rc else '')

    def op63(self, w, rt, ra, rb, rc):
        rcf = bits(w, 21, 25)
        xo5 = bits(w, 26, 30)
        xo = bits(w, 21, 30)
        F = lambda n: f'F({n})'  # noqa: E731
        a5 = {
            18: f'F({rt}) = {F(ra)} / {F(rb)};', 20: f'F({rt}) = {F(ra)} - {F(rb)};',
            21: f'F({rt}) = {F(ra)} + {F(rb)};', 22: f'F({rt}) = sqrt({F(rb)});',
            23: f'F({rt}) = ({F(ra)} >= 0.0) ? {F(rcf)} : {F(rb)};',
            25: f'F({rt}) = {F(ra)} * {F(rcf)};', 26: f'F({rt}) = 1.0 / sqrt({F(rb)});',
            28: f'F({rt}) = {F(ra)} * {F(rcf)} - {F(rb)};', 29: f'F({rt}) = {F(ra)} * {F(rcf)} + {F(rb)};',
            30: f'F({rt}) = -({F(ra)} * {F(rcf)} - {F(rb)});', 31: f'F({rt}) = -({F(ra)} * {F(rcf)} + {F(rb)});',
        }
        if xo5 in a5 and xo5 >= 18:
            return a5[xo5] + (' set_cr1(c);' if rc else '')
        x = {
            0: f'fcmp(c, {bits(w, 6, 8)}, {F(ra)}, {F(rb)});',
            32: f'fcmp(c, {bits(w, 6, 8)}, {F(ra)}, {F(rb)});',
            12: f'F({rt}) = (double)(float){F(rb)};',
            # fctiw/fctiwz: the Cell leaves the 32-bit result sign-extended in the FPR, and compiled code
            # relies on it (fctiwz + fcfid is the game's floor(); zero-extended, floor(-0.3) was 4294967295)
            14: f'FU({rt}) = (uint64_t)(int64_t)(int32_t)cvt_d2i32({F(rb)}, 0);',
            15: f'FU({rt}) = (uint64_t)(int64_t)(int32_t)cvt_d2i32({F(rb)}, 1);',
            814: f'FU({rt}) = (uint64_t)cvt_d2i64({F(rb)}, 0);',
            815: f'FU({rt}) = (uint64_t)cvt_d2i64({F(rb)}, 1);',
            846: f'F({rt}) = (double)(int64_t)FU({rb});',
            40: f'F({rt}) = -{F(rb)};', 72: f'F({rt}) = {F(rb)};',
            136: f'F({rt}) = -fabs({F(rb)});', 264: f'F({rt}) = fabs({F(rb)});',
            583: f'FU({rt}) = c->fpscr;', 711: f'c->fpscr = (uint32_t)FU({rb});',
            38: '/* mtfsb1 */', 70: '/* mtfsb0 */', 134: '/* mtfsfi */', 64: '/* mcrfs */',
        }.get(xo)
        if x is None:
            return None
        return x + (' set_cr1(c);' if rc and xo not in (0, 32) else '')

    # --- branches
    def cond_expr(self, bo, bi):
        parts = []
        if not bo & 4:
            parts.append('(--c->ctr != 0)' if not bo & 2 else '(--c->ctr == 0)')
        if not bo & 16:
            parts.append(f'(CRBIT({bi}) == {1 if bo & 8 else 0})')
        return ' && '.join(parts) if parts else '1'

    # libc setjmp/longjmp (identified by their register save/restore pattern; used by Lua errors)
    SETJMP, LONGJMP = 0x948A24, 0x948B48

    def call_to(self, target, a):
        if target == self.SETJMP:
            # guest setjmp saves the registers; the host setjmp runs in this C frame so a later
            # longjmp to the same guest jmp_buf resumes right here (r3 then holds the value)
            # (the C runtime's setjmp, not __builtin_setjmp: it restores all callee-saved host
            # registers, which the generated code relies on after resuming)
            return (f'{{ uint32_t jb_ = (uint32_t)R(3); f_{target:08x}(c); '
                    f'if (setjmp(*host_jmpbuf(jb_))) {{ /* resumed by longjmp */ }} }}')
        if target == self.LONGJMP:
            # guest longjmp restores the registers (r1, r2, r13, r14-r31, LR, r3); then unwind host
            return (f'{{ uint32_t jb_ = (uint32_t)R(3); f_{target:08x}(c); '
                    f'longjmp(*host_jmpbuf(jb_), 1); }}')
        if target in self.stub_names:
            return f'hle_call(c, "{self.stub_names[target]}", 0x{target:x});'
        if target in self.entries:
            return f'f_{target:08x}(c);'
        return f'dispatch(c, 0x{target:x}, 0x{a:x});'

    def branch(self, fn, a, w):
        op = bits(w, 0, 5)
        lk = w & 1
        if op == 18:  # b / bl
            aa = w & 2
            li = sext(w & 0x03FFFFFC, 26)
            target = (li if aa else a + li) & 0xFFFFFFFF
            if lk:
                if target == a + 4:  # bl $+4 (get PC)
                    return f'c->lr = 0x{a + 4:x};'
                return f'c->lr = 0x{a + 4:x}; ' + self.call_to(target, a)
            if fn.entry <= target < fn.end and target != fn.entry:
                fn.labels.add(target)
                return f'goto L_{target:08x};'
            return self.call_to(target, a) + ' return;'
        if op == 16:  # bc
            bo, bi = bits(w, 6, 10), bits(w, 11, 15)
            target = (sext(w & 0xFFFC, 16) + (0 if w & 2 else a)) & 0xFFFFFFFF
            cond = self.cond_expr(bo, bi)
            if lk:
                if target == a + 4:
                    return f'c->lr = 0x{a + 4:x};'
                return f'if ({cond}) {{ c->lr = 0x{a + 4:x}; {self.call_to(target, a)} }}'
            if fn.entry <= target < fn.end and target != fn.entry:
                fn.labels.add(target)
                return f'if ({cond}) goto L_{target:08x};'
            return f'if ({cond}) {{ {self.call_to(target, a)} return; }}'
        if op == 19:
            xo = bits(w, 21, 30)
            bo, bi = bits(w, 6, 10), bits(w, 11, 15)
            if xo == 16:   # bclr
                cond = self.cond_expr(bo, bi)
                if lk:
                    return f'if ({cond}) {{ uint64_t t_ = c->lr; c->lr = 0x{a + 4:x}; dispatch(c, (uint32_t)t_, 0x{a:x}); }}'
                return f'if ({cond}) return;' if cond != '1' else 'return;'
            if xo == 528:  # bcctr
                cond = self.cond_expr(bo | 4, bi)
                if lk:
                    return f'if ({cond}) {{ c->lr = 0x{a + 4:x}; dispatch(c, (uint32_t)c->ctr, 0x{a:x}); }}'
                fn.has_bctr = True
                return f'if ({cond}) {{ JUMP_CTR(); }}'
            crops = {257: 'a_ & b_', 449: 'a_ | b_', 193: 'a_ ^ b_', 225: '!(a_ & b_)', 33: '!(a_ | b_)',
                     289: 'a_ == b_', 129: 'a_ & !b_', 417: 'a_ | !b_'}
            if xo in crops:
                bt = bits(w, 6, 10)
                return f'{{ int a_ = CRBIT({bi}), b_ = CRBIT({bits(w, 16, 20)}); SET_CRBIT({bt}, ({crops[xo]}) ? 1 : 0); }}'
            if xo == 0:    # mcrf
                return f'set_cr_field(c, {bits(w, 6, 8)}, (c->cr >> (28 - 4 * {bits(w, 11, 13)})) & 0xF);'
            if xo == 150:  # isync
                return '/* isync */'
        return None

    hooks = {}       # guest function entry -> host function replacing it (from hooks.txt)
    pre_hooks = {}   # guest function entry -> host function called first, then the original body
    gate_hooks = {}  # guest function entry -> host function called first; nonzero return skips the body
    native_hooks = {}   # guest function entry -> host function replacing it; the original body stays as f_<entry>_orig

    def translate(self, fn):
        if fn.entry in self.hooks:
            h = self.hooks[fn.entry]
            return (f'void {h}(ctx_t *c);\n'
                    f'void f_{fn.entry:08x}(ctx_t *c) {{ /* {fn.name}: replaced by host {h} (hooks.txt) */\n'
                    f'  FN_ENTER(0x{fn.entry:x});\n  {h}(c);\n}}')
        body = []
        a = fn.entry
        while a < fn.end:
            w = self.img.u32(a)
            if w is None:
                break
            op = bits(w, 0, 5)
            if op in (16, 18) or (op == 19 and bits(w, 21, 30) in (16, 528, 257, 449, 193, 225, 33, 289, 129, 417, 0, 150)):
                s = self.branch(fn, a, w)
            else:
                s = self.insn(fn, a, w)
            if s is None:
                key = (bits(w, 0, 5), bits(w, 21, 30) if bits(w, 0, 5) in (19, 31, 63) else 0)
                self.unhandled[key] = self.unhandled.get(key, 0) + 1
                s = f'unimpl(c, 0x{a:x}, 0x{w:08x});'
            body.append((a, s))
            a += 4
        native = self.native_hooks.get(fn.entry)
        name = f'f_{fn.entry:08x}_orig' if native else f'f_{fn.entry:08x}'
        lines = [f'void {name}(ctx_t *restrict c) {{ /* {fn.name} */', f'  FN_ENTER(0x{fn.entry:x});']
        if native:   # the host version (which may call the original to check itself)
            lines = [f'void {native}(ctx_t *c);', f'void {name}(ctx_t *restrict c);',
                     f'void f_{fn.entry:08x}(ctx_t *c) {{ /* {fn.name}: native {native} (hooks.txt) */ {native}(c); }}'] + lines
        if fn.entry in self.pre_hooks:
            h = self.pre_hooks[fn.entry]
            lines.insert(0, f'void {h}(ctx_t *c);')
            lines.append(f'  {h}(c);   /* pre-hook (hooks.txt) */')
        if fn.entry in self.gate_hooks:
            h = self.gate_hooks[fn.entry]
            lines.insert(0, f'int {h}(ctx_t *c);')
            lines.append(f'  if ({h}(c)) return;   /* gate hook (hooks.txt) */')
        for addr, s in body:
            if fn.has_bctr or addr in fn.labels:
                lines.append(f'L_{addr:08x}:;')
            lines.append(f'  {s}')
        # Falling off the end: if the last instruction doesn't unconditionally leave, control
        # continues into the next function (created when a split put an entry mid-way).
        last = self.img.u32(fn.end - 4)
        leaves = last is not None and (
            ((last >> 26) == 18 and not last & 1) or
            ((last >> 26) == 19 and bits(last, 21, 30) in (16, 528) and bits(last, 6, 10) & 0x14 == 0x14 and not last & 1))
        if not leaves and fn.end in self.entries:
            lines.append(f'  f_{fn.end:08x}(c);')
        lines.append('  return;')
        if fn.has_bctr:
            lines.append(' jump_ctr: switch ((uint32_t)c->ctr) {')
            lines.extend(f'  case 0x{addr:x}: goto L_{addr:08x};' for addr, _ in body)
            lines.append(f'  default: dispatch(c, (uint32_t)c->ctr, 0x{fn.entry:x}); return; }}')
        lines.append('}')
        return '\n'.join(lines)


# ----------------------------------------------------------------------------- driver

def main():
    global TEXT_HI
    elf, functsv, outdir, lo, hi = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16), int(sys.argv[5], 16)
    img = Image(elf)
    TEXT_HI = hi   # end of the main .text of this build (the range's upper bound)
    entry, = struct.unpack('>Q', img.d[0x18:0x20])
    toc_value = img.u32(entry + 4)   # every descriptor carries the entry's TOC
    print(f'EBOOT entry descriptor 0x{entry:08x}, TOC 0x{toc_value:08x}, text 0x{TEXT_LO:x}..0x{TEXT_HI:x}')
    all_funcs = load_functions(functsv)
    # import stubs live in their own section after .text, so take them from the full list
    stub_names = {a: n for a, n, _ in all_funcs if '::' in n and n.split('::')[0] in IMPORT_LIBS}
    ghidra = {a: n for a, n, _ in all_funcs if TEXT_LO <= a < TEXT_HI}
    # Entry points: Ghidra's functions + every bl target + every OPD code address. Ghidra misses
    # some functions that are only reached by bl (e.g. 0x950E4C).
    extra = set()
    for pc in range(TEXT_LO, TEXT_HI, 4):
        w = img.u32(pc)
        if w is not None and w >> 26 == 18 and w & 1:
            li = sext(w & 0x03FFFFFC, 26)
            t = (li if w & 2 else pc + li) & 0xFFFFFFFF
            if TEXT_LO <= t < TEXT_HI and t != pc + 4:
                extra.add(t)
    for sva, off, fsz, _ in img.segs:
        if sva >= 0xE80000:  # data segment: OPDs {code, toc}
            for o in range(0, fsz - 7, 4):
                code, toc = struct.unpack_from('>II', img.d, off + o)
                if toc == toc_value and TEXT_LO <= code < TEXT_HI:
                    extra.add(code)
    # Jump tables (switch): "lwzx; add; mtctr; bctr" with the table right after the bctr, signed word offsets from the
    # table start. Their targets become entries: once the function is split
    # (by the bl targets above), a case can land in a later piece, which the bctr then reaches through dispatch()
    # (e.g. 0x9A3E88 in the PlayerConnect code, only run online).
    for pc in range(TEXT_LO, TEXT_HI, 4):
        if img.u32(pc) != 0x4E800420:
            continue
        base, targets = pc + 4, []
        while len(targets) < 1024:
            v = img.u32(base + 4 * len(targets))
            if v is None or v == 0 or v & 3 or 0x100000 <= v < 0xFFF00000:   # offsets within +-1 MB (cases can precede the table)
                break
            targets.append((base + v) & 0xFFFFFFFF)
        if len(targets) >= 2:
            extra.update(t for t in targets if TEXT_LO <= t < TEXT_HI)
    names = dict(ghidra)
    for a in extra - set(ghidra):
        names[a] = f'sub_{a:08x}'
    # b/bc that leave their function for code no entry covers (Ghidra truncated the function there,
    # e.g. 0x8252F8): make the target an entry too, repeating until nothing new turns up.
    branch_sites = []
    for pc in range(TEXT_LO, TEXT_HI, 4):
        w = img.u32(pc)
        if w is None or w & 1:
            continue
        if w >> 26 == 18:
            t = (sext(w & 0x03FFFFFC, 26) + (0 if w & 2 else pc)) & 0xFFFFFFFF
        elif w >> 26 == 16:
            t = (sext(w & 0xFFFC, 16) + (0 if w & 2 else pc)) & 0xFFFFFFFF
        else:
            continue
        if TEXT_LO <= t < TEXT_HI:
            branch_sites.append((pc, t))
    while True:
        ents = sorted(names)
        new = set()
        for pc, t in branch_sites:
            i = bisect.bisect_right(ents, pc) - 1
            if i < 0:
                continue
            lo_f, hi_f = ents[i], ents[i + 1] if i + 1 < len(ents) else TEXT_HI
            if not (lo_f <= t < hi_f) and t not in names and t not in stub_names:
                new.add(t)
        if not new:
            break
        for t in new:
            names[t] = f'sub_{t:08x}'
    entries = sorted(names)
    print(f'{len(ghidra)} Ghidra functions + {len(names) - len(ghidra)} extra entries (bl/OPD targets)')
    funcs = [(a, names[a], []) for a in entries]
    end_of = {a: (entries[i + 1] if i + 1 < len(entries) else TEXT_HI) for i, a in enumerate(entries)}
    calls = {a: cs for a, _, cs in all_funcs}

    def direct_targets(a):
        """Function entries reached by bl/bcl or by b/bc leaving the function, found by decoding."""
        out, pc = [], a
        while pc < end_of[a]:
            w = img.u32(pc)
            if w is None:
                break
            op = w >> 26
            t = None
            if op == 18:
                li = sext(w & 0x03FFFFFC, 26)
                t = (li if w & 2 else pc + li) & 0xFFFFFFFF
            elif op == 16:
                t = (sext(w & 0xFFFC, 16) + (0 if w & 2 else pc)) & 0xFFFFFFFF
            if t is not None and t in end_of and not (a < t < end_of[a]):
                out.append(t)
            pc += 4
        return out

    # closure over direct calls (decoded from the instructions), from every function in [lo, hi)
    todo = [a for a in entries if lo <= a < hi]
    chosen = set()
    while todo:
        a = todo.pop()
        if a in chosen or a in stub_names or a not in end_of:
            continue
        chosen.add(a)
        todo.extend(direct_targets(a))

    tr = Translator(img, funcs, stub_names)
    hooks_path = os.environ.get('MNR_HOOKS') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hooks.txt')   # per build: hooks_v100.txt / hooks_v111.txt
    if os.path.exists(hooks_path):   # lines: <hex address> <host function> [pre|gate] [# comment]
        for line in open(hooks_path):
            parts = line.split('#', 1)[0].split()
            if len(parts) >= 3 and parts[2] == 'pre':
                tr.pre_hooks[int(parts[0], 16)] = parts[1]
            elif len(parts) >= 3 and parts[2] == 'gate':
                tr.gate_hooks[int(parts[0], 16)] = parts[1]
            elif len(parts) >= 3 and parts[2] == 'native':
                tr.native_hooks[int(parts[0], 16)] = parts[1]
            elif len(parts) >= 2:
                tr.hooks[int(parts[0], 16)] = parts[1]
    os.makedirs(outdir, exist_ok=True)
    order = sorted(chosen)

    written = []
    def write_if_changed(name, text):
        """Only touch files whose content changed, so the build recompiles just those."""
        path = os.path.join(outdir, name)
        if os.path.exists(path) and open(path).read() == text:
            return
        with open(path, 'w') as f:
            f.write(text)
        written.append(name)

    write_if_changed('decls.h', '#pragma once\n#include "runtime.h"\n' +
                     ''.join(f'void f_{a:08x}(ctx_t *c);\n' for a in order))
    per_file = 600
    names_out = set()
    for i in range(0, len(order), per_file):
        name = f'recomp_{i // per_file:03d}.c'
        names_out.add(name)
        write_if_changed(name, '#include "decls.h"\n\n' + ''.join(
            tr.translate(Fn(a, end_of[a], names[a])) + '\n\n' for a in order[i:i + per_file]))
    for old in os.listdir(outdir):
        if old.startswith('recomp_') and old.endswith('.c') and old not in names_out:
            os.remove(os.path.join(outdir, old))
    write_if_changed('fn_table.c', '#include "decls.h"\n\nconst fn_entry_t fn_table[] = {\n' +
                     ''.join(f'  {{0x{a:x}, f_{a:08x}}},\n' for a in order) +
                     f'}};\nconst unsigned fn_table_count = {len(order)};\n')
    print(f'{len(names_out)} source files; changed: {written or "none"}; {len(tr.hooks)} hooks')
    print(f'{len(order)} functions recompiled ({sum(1 for a in order if lo <= a < hi)} in range, '
          f'{len(order) - sum(1 for a in order if lo <= a < hi)} pulled in by calls)')
    reached = sorted({stub_names[t] for a in order for t in calls.get(a, []) if t in stub_names})
    print(f'import stubs reached: {reached}')
    if tr.unhandled:
        top = sorted(tr.unhandled.items(), key=lambda kv: -kv[1])[:20]
        print('untranslated (opcode, xo): count ->', top)


IMPORT_LIBS = {'cellJpgEnc', 'cellPngEnc', 'cellPngDec', 'cellGcmSys', 'cellSysutil', 'sys_fs', 'cellSysmodule',
               'cellSync', 'cellGame', 'sys_net', 'cellNetCtl', 'cellRtc', 'sceNp', 'cellSpurs', 'sys_io',
               'sceNpTrophy', 'cellPhotoUtility', 'cellUserInfo', 'sceNpCommerce2', 'cellSsl', 'cellHttp',
               'cellOskExtUtility', 'cellFont', 'cellFontFT', 'sysPrxForUser', 'cellVoice', 'cellAudio'}

if __name__ == '__main__':
    main()
