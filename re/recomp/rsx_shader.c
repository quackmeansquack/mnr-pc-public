/* Decoding and disassembly of RSX vertex and fragment program microcode. */
#include <stdio.h>
#include <string.h>
#include "runtime.h"
#include "rsx_shader.h"

#define BITS(w, lo, n) (((w) >> (lo)) & ((1u << (n)) - 1))

/* ------------------------------------------------------------------ vertex programs
 * word 0: addr_swz[0:2] cond_swz(w,z,y,x)[2:10] cond[10:13] cond_test[13] cond_update0[14]
 *         dst_tmp[15:21] src0_abs[21] src1_abs[22] src2_abs[23] addr_reg_sel1[24] cond_reg_sel1[25]
 *         saturate[26] index_input[27] cond_update1[29] vec_result[30]
 * word 1: src0_hi[0:8] input_src[8:12] const_src[12:22] vec_op[22:27] sca_op[27:32]
 * word 2: src2_hi[0:6] src1[6:23] src0_lo[23:32]
 * word 3: end[0] index_const[1] dst[2:7] sca_dst_tmp[7:13] vec_mask(w,z,y,x)[13:17]
 *         sca_mask(w,z,y,x)[17:21] src2_lo[21:32]
 * source (17 bits): type[0:2] tmp[2:8] swz(w,z,y,x)[8:16] neg[16] */
static void vp_src(uint32_t s, int abs, vp_src_t *o) {
    o->type = BITS(s, 0, 2); o->index = BITS(s, 2, 6);
    o->swz[3] = BITS(s, 8, 2); o->swz[2] = BITS(s, 10, 2); o->swz[1] = BITS(s, 12, 2); o->swz[0] = BITS(s, 14, 2);
    o->neg = BITS(s, 16, 1); o->abs = (uint8_t)abs;
}
void vp_decode(const uint32_t w[4], vp_insn_t *o) {
    memset(o, 0, sizeof *o);
    uint32_t d0 = w[0], d1 = w[1], d2 = w[2], d3 = w[3];
    o->addr_swz = BITS(d0, 0, 2);
    o->cond_swz[3] = BITS(d0, 2, 2); o->cond_swz[2] = BITS(d0, 4, 2); o->cond_swz[1] = BITS(d0, 6, 2); o->cond_swz[0] = BITS(d0, 8, 2);
    o->cond = BITS(d0, 10, 3); o->cond_test = BITS(d0, 13, 1);
    o->cond_update = BITS(d0, 14, 1) | BITS(d0, 29, 1);
    o->vec_tmp = BITS(d0, 15, 6);
    o->addr_reg = BITS(d0, 24, 1); o->cond_reg = BITS(d0, 25, 1);
    o->saturate = BITS(d0, 26, 1); o->vec_result = BITS(d0, 30, 1);
    o->input_index = BITS(d1, 8, 4); o->const_index = BITS(d1, 12, 10);
    o->vec_op = BITS(d1, 22, 5); o->sca_op = BITS(d1, 27, 5);
    uint32_t s0 = BITS(d1, 0, 8) << 9 | BITS(d2, 23, 9);
    uint32_t s1 = BITS(d2, 6, 17);
    uint32_t s2 = BITS(d2, 0, 6) << 11 | BITS(d3, 21, 11);
    vp_src(s0, BITS(d0, 21, 1), &o->src[0]);
    vp_src(s1, BITS(d0, 22, 1), &o->src[1]);
    vp_src(s2, BITS(d0, 23, 1), &o->src[2]);
    o->end = BITS(d3, 0, 1); o->index_const = BITS(d3, 1, 1);
    o->out_reg = BITS(d3, 2, 5); o->sca_tmp = BITS(d3, 7, 6);
    o->vec_mask = BITS(d3, 13, 4);   /* stored w,z,y,x from bit 13 up, so bit 3 = x ... bit 0 = w */
    o->sca_mask = BITS(d3, 17, 4);
    o->tex_num = BITS(d2, 8, 2);
}

static const char *vp_vec_names[32] = {"NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN", "MAX", "SLT", "SGE",
    "ARL", "FRC", "FLR", "SEQ", "SFL", "SGT", "SLE", "SNE", "STR", "SSG", "v23", "TXL"};
static const char *vp_sca_names[32] = {"NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", "BRA", "BRI", "CAL", "CLI", "RET",
    "LG2", "EX2", "SIN", "COS", "BRB", "CLB", "PSH", "POP"};

static int src_str(char *b, int cap, const vp_insn_t *i, int n) {
    const vp_src_t *s = &i->src[n];
    static const char sw[] = "xyzw";
    char reg[24];
    if (s->type == 1) snprintf(reg, sizeof reg, "R%u", s->index);
    else if (s->type == 2) snprintf(reg, sizeof reg, "v[%u]", i->input_index);
    else if (s->type == 3 && i->index_const) snprintf(reg, sizeof reg, "c[A%u.%c+%u]", i->addr_reg, sw[i->addr_swz], i->const_index);
    else if (s->type == 3) snprintf(reg, sizeof reg, "c[%u]", i->const_index);
    else snprintf(reg, sizeof reg, "?%u", s->type);
    return snprintf(b, cap, "%s%s%s.%c%c%c%c%s", s->neg ? "-" : "", s->abs ? "|" : "", reg,
                    sw[s->swz[0]], sw[s->swz[1]], sw[s->swz[2]], sw[s->swz[3]], s->abs ? "|" : "");
}
static void mask_str(char *b, uint8_t m) {   /* m: bit 3 = x */
    int k = 0;
    if (m & 8) b[k++] = 'x'; if (m & 4) b[k++] = 'y'; if (m & 2) b[k++] = 'z'; if (m & 1) b[k++] = 'w';
    b[k] = 0;
}
int vp_disasm(const vp_insn_t *i, char *b, int cap) {
    int n = 0;
    char m[8], s[3][48];
    for (int k = 0; k < 3; k++) src_str(s[k], sizeof s[k], i, k);
    if (i->vec_op) {
        mask_str(m, i->vec_mask);
        char dst[32] = "";
        if (i->vec_result && i->out_reg != 0x1F) snprintf(dst, sizeof dst, "o[%u]", i->out_reg);
        else if (i->vec_tmp != 0x3F) snprintf(dst, sizeof dst, "R%u", i->vec_tmp);
        else snprintf(dst, sizeof dst, "_");
        n += snprintf(b + n, cap - n, "%s%s %s.%s, %s, %s, %s; ", vp_vec_names[i->vec_op] ? vp_vec_names[i->vec_op] : "v?",
                      i->saturate ? "_sat" : "", dst, m, s[0], s[1], s[2]);
    }
    if (i->sca_op) {
        mask_str(m, i->sca_mask);
        char dst[32] = "";
        if (!i->vec_result && i->out_reg != 0x1F) snprintf(dst, sizeof dst, "o[%u]", i->out_reg);
        else if (i->sca_tmp != 0x3F) snprintf(dst, sizeof dst, "R%u", i->sca_tmp);
        else snprintf(dst, sizeof dst, "_");
        n += snprintf(b + n, cap - n, "%s %s.%s, %s; ", vp_sca_names[i->sca_op] ? vp_sca_names[i->sca_op] : "s?", dst, m, s[2]);
    }
    if (i->cond_test) n += snprintf(b + n, cap - n, "(cc%u test %u) ", i->cond_reg, i->cond);
    if (i->end) n += snprintf(b + n, cap - n, "END");
    return n;
}

/* ------------------------------------------------------------------ fragment programs
 * Each 32-bit word is stored with its 16-bit halves swapped.
 * word 0 (dest): end[0] dst[1:7] fp16[7] set_cond[8] mask x,y,z,w[9:13] input[13:17] tex[17:21]
 *                bx2[21] prec[22:24] opcode[24:30] no_dest[30] saturate[31]
 * word 1 (src0): type[0:2] index[2:8] fp16[8] swz x,y,z,w[9:17] neg[17] exec lt,eq,gt[18:21]
 *                cond_swz x,y,z,w[21:29] abs[29] cond_mod_reg[30] cond_reg[31]
 * word 2 (src1): type/index/fp16/swz/neg as src0, abs[18], scale[28:31], branch[31]
 * word 3 (src2): type/index/fp16/swz/neg as src0, abs[18], perspective[31]
 * Any constant source puts four constant words (also halfword-swapped) right after the instruction. */
static __thread const uint8_t *fp_buf; static __thread uint32_t fp_buf_size;   /* fp_decode_buf: decode from this copy */
static uint32_t fp_word(uint32_t a) {
    uint32_t v;
    if (fp_buf) v = a + 4 <= fp_buf_size ? (uint32_t)fp_buf[a] << 24 | (uint32_t)fp_buf[a + 1] << 16 | (uint32_t)fp_buf[a + 2] << 8 | fp_buf[a + 3] : 0;
    else v = ld_u32(a);
    return v >> 16 | v << 16;
}
static void fp_src(uint32_t s, fp_src_t *o, int abs_bit) {
    o->type = BITS(s, 0, 2); o->index = BITS(s, 2, 6); o->fp16 = BITS(s, 8, 1);
    o->swz[0] = BITS(s, 9, 2); o->swz[1] = BITS(s, 11, 2); o->swz[2] = BITS(s, 13, 2); o->swz[3] = BITS(s, 15, 2);
    o->neg = BITS(s, 17, 1); o->abs = BITS(s, abs_bit, 1);
}
int fp_decode(uint32_t a, fp_insn_t *out, int max) {
    int n = 0;
    while (n < max) {
        fp_insn_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        uint32_t d = fp_word(a), s0 = fp_word(a + 4), s1 = fp_word(a + 8), s2 = fp_word(a + 12);
        o->end = BITS(d, 0, 1); o->dst = BITS(d, 1, 6); o->dst_fp16 = BITS(d, 7, 1); o->set_cond = BITS(d, 8, 1);
        o->mask = (uint8_t)(BITS(d, 9, 1) << 3 | BITS(d, 10, 1) << 2 | BITS(d, 11, 1) << 1 | BITS(d, 12, 1));
        o->input = BITS(d, 13, 4); o->tex = BITS(d, 17, 4); o->bx2 = BITS(d, 21, 1); o->prec = BITS(d, 22, 2);
        o->op = (uint8_t)(BITS(d, 24, 6) | BITS(s1, 31, 1) << 6);
        o->no_dest = BITS(d, 30, 1); o->saturate = BITS(d, 31, 1);
        fp_src(s0, &o->src[0], 29); fp_src(s1, &o->src[1], 18); fp_src(s2, &o->src[2], 18);
        o->src[0].prec = BITS(s1, 19, 3); o->src[1].prec = BITS(s1, 22, 3); o->src[2].prec = BITS(s1, 25, 3);   /* (as RPCS3's SRC1.srcN_prec_mod) */
        o->exec_lt = BITS(s0, 18, 1); o->exec_eq = BITS(s0, 19, 1); o->exec_gt = BITS(s0, 20, 1);
        o->cond_swz[0] = BITS(s0, 21, 2); o->cond_swz[1] = BITS(s0, 23, 2); o->cond_swz[2] = BITS(s0, 25, 2); o->cond_swz[3] = BITS(s0, 27, 2);
        o->cond_mod_reg = BITS(s0, 30, 1); o->cond_reg = BITS(s0, 31, 1);
        o->scale = BITS(s1, 28, 3); o->persp = BITS(s2, 31, 1);
        if (o->op & 0x40) {   /* flow control: offsets/counters live in the source words */
            o->else_off = (uint16_t)BITS(s1, 0, 31); o->end_off = (uint16_t)BITS(s2, 0, 31);
            o->rep_init = BITS(s1, 2, 8); o->rep_inc = BITS(s1, 10, 8); o->rep_count = BITS(s1, 19, 8);
        }
        a += 16;
        if (!(o->op & 0x40) && (o->src[0].type == 2 || o->src[1].type == 2 || o->src[2].type == 2)) {
            o->has_const = 1;
            for (int k = 0; k < 4; k++) { uint32_t v = fp_word(a + 4 * k); memcpy(&o->constant[k], &v, 4); }
            a += 16;
        }
        if (o->end) break;
    }
    return n;
}
/* the same, from a copy of the program's bytes as they sit in guest memory (shader precompile at startup) */
int fp_decode_buf(const uint8_t *buf, uint32_t size, fp_insn_t *out, int max) {
    fp_buf = buf; fp_buf_size = size;
    int n = fp_decode(0, out, max);
    fp_buf = NULL; fp_buf_size = 0;
    return n;
}
/* bytes a decoded program takes in memory (16 per instruction, 16 more after one with an inline constant) */
uint32_t fp_bytes(const fp_insn_t *prog, int n) { uint32_t b = 0; for (int i = 0; i < n; i++) b += prog[i].has_const ? 32 : 16; return b; }

static const char *fp_names[128] = {
    "NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DP4", "DST", "MIN", "MAX", "SLT", "SGE", "SLE", "SGT", "SNE", "SEQ",
    "FRC", "FLR", "KIL", "PK4", "UP4", "DDX", "DDY", "TEX", "TXP", "TXD", "RCP", "RSQ", "EX2", "LG2", "LIT", "LRP",
    "STR", "SFL", "COS", "SIN", "PK2", "UP2", "POW", "PKB", "UPB", "PK16", "UP16", "BEM", "PKG", "UPG", "DP2A", "TXL",
    "op30", "TXB", "op32", "TEXBEM", "TXPBEM", "BEMLUM", "REFL", "TIMESWTEX", "DP2", "NRM", "DIV", "DIVSQ", "LIF", "FENCT", "FENCB", "op3F",
    "BRK", "CAL", "IFE", "LOOP", "REP", "RET"};
static const char *fp_inputs[16] = {"WPOS", "COL0", "COL1", "FOGC", "TEX0", "TEX1", "TEX2", "TEX3", "TEX4", "TEX5", "TEX6", "TEX7", "TEX8", "TEX9", "SSA", "in15"};
int fp_disasm(const fp_insn_t *i, char *b, int cap) {
    static const char sw[] = "xyzw";
    int n = snprintf(b, cap, "%s%s%s", fp_names[i->op] ? fp_names[i->op] : "op?", i->saturate ? "_sat" : "", i->op == 0x17 || i->op == 0x18 || i->op == 0x2F || i->op == 0x31 ? "" : "");
    if (i->op & 0x40) return n + snprintf(b + n, cap - n, " else %u end %u%s", i->else_off, i->end_off, i->end ? " END" : "");
    char m[8]; int k = 0;
    if (i->mask & 8) m[k++] = 'x'; if (i->mask & 4) m[k++] = 'y'; if (i->mask & 2) m[k++] = 'z'; if (i->mask & 1) m[k++] = 'w';
    m[k] = 0;
    if (i->no_dest) n += snprintf(b + n, cap - n, " _");
    else n += snprintf(b + n, cap - n, " %c%u.%s", i->dst_fp16 ? 'H' : 'R', i->dst, m);
    for (int s = 0; s < 3; s++) {
        const fp_src_t *x = &i->src[s];
        char reg[24];
        if (x->type == 0) snprintf(reg, sizeof reg, "%c%u", x->fp16 ? 'H' : 'R', x->index);
        else if (x->type == 1) snprintf(reg, sizeof reg, "f[%s]", fp_inputs[i->input]);
        else if (x->type == 2) snprintf(reg, sizeof reg, "{%g,%g,%g,%g}", i->constant[0], i->constant[1], i->constant[2], i->constant[3]);
        else snprintf(reg, sizeof reg, "?3");
        static const char *pm[8] = {"", "h", "x12", "x9", "sat", "p5", "p6", "p7"};
        n += snprintf(b + n, cap - n, ", %s%s%s%s%s.%c%c%c%c%s%s", x->neg ? "-" : "", x->prec ? pm[x->prec & 7] : "", x->prec ? "(" : "", x->abs ? "|" : "", reg,
                      sw[x->swz[0]], sw[x->swz[1]], sw[x->swz[2]], sw[x->swz[3]], x->abs ? "|" : "", x->prec ? ")" : "");
    }
    if (i->op >= 0x17 && i->op <= 0x19) n += snprintf(b + n, cap - n, " tex%u", i->tex);
    if (i->op == 0x2F || i->op == 0x31) n += snprintf(b + n, cap - n, " tex%u", i->tex);
    if (i->end) n += snprintf(b + n, cap - n, " END");
    return n;
}
