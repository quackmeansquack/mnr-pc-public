/* Translation of RSX vertex and fragment programs to HLSL (shader model 5) for the Direct3D 11 backend.
 *
 * Vertex shaders fetch their own inputs: raw big-endian vertex data sits in a ByteAddressBuffer and a
 * per-draw constant buffer describes each attribute (type, size, stride, base), so any RSX vertex
 * format works without D3D input layouts. The program's position output is mapped through the RSX
 * viewport (scale/offset) into window coordinates and then into D3D clip space for the bound surface.
 *
 * Fragment programs read interpolants TEXCOORD0..12 (RSX inputs COL0..TEX9), sample textures whose
 * texels are stored raw in A,R,G,B order (surfaces in R,G,B,A) and apply the RSX remap per unit from
 * constants. Inline constants become constant-buffer slots, so patching constants (which games do per
 * draw) doesn't need a new shader. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "runtime.h"
#include "rsx_shader.h"
#include "rsx_hlsl.h"

typedef struct { char *p; size_t cap, len; int overflow; } sb_t;
static void emit(sb_t *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->len < b->cap ? b->cap - b->len : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || b->len + (size_t)n >= b->cap) { b->overflow = 1; b->len = b->cap ? b->cap - 1 : 0; }
    else b->len += (size_t)n;
}
static const char SW[] = "xyzw";

/* ------------------------------------------------------------------ vertex programs */
static void vp_src(sb_t *b, const vp_insn_t *I, int n) {
    const vp_src_t *s = &I->src[n];
    char reg[48];
    if (s->type == 1) snprintf(reg, sizeof reg, "r%u", s->index % 48);
    else if (s->type == 2) snprintf(reg, sizeof reg, "v%u", I->input_index);
    else if (s->type == 3) {
        if (I->index_const) snprintf(reg, sizeof reg, "vc[clamp(a%u.%c + %u, 0, 467)]", I->addr_reg, SW[I->addr_swz], I->const_index);
        else snprintf(reg, sizeof reg, "vc[%u]", I->const_index < 468 ? I->const_index : 467);
    } else snprintf(reg, sizeof reg, "float4(0,0,0,0)");
    emit(b, "%s%s(%s).%c%c%c%c%s", s->neg ? "-" : "", s->abs ? "abs(" : "", reg,
         SW[s->swz[0]], SW[s->swz[1]], SW[s->swz[2]], SW[s->swz[3]], s->abs ? ")" : "");
}
static const char *cc_cmp[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
static void vp_cond(sb_t *b, const vp_insn_t *I, int k) {   /* condition for component k */
    if (I->cond == 0) { emit(b, "false"); return; }
    if (I->cond == 7) { emit(b, "true"); return; }
    emit(b, "(cc%u.%c %s 0)", I->cond_reg, SW[I->cond_swz[k]], cc_cmp[I->cond]);
}
static void vp_write(sb_t *b, const vp_insn_t *I, const char *dst, const char *val, int mask) {
    for (int k = 0; k < 4; k++) {
        if (!(mask >> (3 - k) & 1)) continue;
        if (I->cond_test) { emit(b, "  if ("); vp_cond(b, I, k); emit(b, ") "); } else emit(b, "  ");
        emit(b, "%s.%c = %s.%c;\n", dst, SW[k], val, SW[k]);
    }
}

int hlsl_vp(const uint32_t (*words)[4], uint32_t start, char *out, size_t cap, uint32_t *inputs_used) {
    sb_t b = {out, cap, 0, 0};
    vp_insn_t prog[512]; int n = 0;
    uint32_t used_in = 0;
    for (uint32_t pc = start; pc < 512 && n < 512; pc++) {
        vp_decode(words[pc], &prog[n]);
        vp_insn_t *I = &prog[n++];
        if (I->sca_op >= 8 && I->sca_op <= 12) return -1;   /* branches/calls: not translated yet */
        if (I->sca_op >= 17) return -1;
        for (int k = 0; k < 3; k++) if (I->src[k].type == 2) used_in |= 1u << I->input_index;
        if (I->end) break;
    }
    if (inputs_used) *inputs_used = used_in;
    emit(&b,
"cbuffer VPC : register(b0) { float4 vc[468]; };\n"
"cbuffer DRAW : register(b1) {\n"
"  uint4 afmt[16];     /* type, size, stride, enabled */\n"
"  int4 abase[16];     /* x: byte offset of index 0 in vb */\n"
"  float4 aconst[16];\n"
"  float4 vscale, voff, surf;   /* RSX viewport; surf.xy = surface size */\n"
"  uint ebase;                 /* first element of this draw in el */\n"
"};\n"
"ByteAddressBuffer vb : register(t0);\n"
"StructuredBuffer<uint> el : register(t1);\n"
"uint ld8(int a) { uint w = vb.Load(a & ~3); return (w >> ((a & 3) * 8)) & 0xFF; }\n"
"uint ld16(int a) { return (ld8(a) << 8) | ld8(a + 1); }\n"
"uint ld32(int a) { return (ld8(a) << 24) | (ld8(a + 1) << 16) | (ld8(a + 2) << 8) | ld8(a + 3); }\n"
"int sx16(uint v) { return (int)(v << 16) >> 16; }\n"
"float4 fetch(uint i, uint idx) {\n"
"  uint4 f = afmt[i];\n"
"  if (f.w == 0) return aconst[i];\n"
"  uint fq = asuint(abase[i].y), vi = idx;\n"
"  if (fq > 1) { if (abase[i].z != 0) vi = idx %% fq; else vi = idx / fq; }\n"   /* frequency divider */
"  int a = abase[i].x + (int)(vi * f.z);\n"
"  float4 v = float4(0, 0, 0, 1);\n"
"  if (f.x == 6) { uint w = ld32(a); v.x = (float)((int)(w << 21) >> 21) / 1023.0; v.y = (float)((int)(w << 10) >> 21) / 1023.0; v.z = (float)((int)w >> 22) / 511.0; return v; }\n"
"  [unroll] for (uint k = 0; k < 4; k++) if (k < f.y) {\n"
"    float x = 0;\n"
"    if (f.x == 1) x = max((float)sx16(ld16(a + 2 * k)) / 32767.0, -1.0);\n"
"    else if (f.x == 2) x = asfloat(ld32(a + 4 * k));\n"
"    else if (f.x == 3) x = f16tof32(ld16(a + 2 * k));\n"
"    else if (f.x == 4) x = (float)ld8(a + k) / 255.0;\n"
"    else if (f.x == 5) x = (float)sx16(ld16(a + 2 * k));\n"
"    else if (f.x == 7) x = (float)ld8(a + k);\n"
"    v[k] = x;\n"
"  }\n"
"  return v;\n"
"}\n"
"struct VO { float4 pos : SV_Position;\n");
    for (int i = 0; i < 13; i++) emit(&b, "  float4 t%d : TEXCOORD%d;\n", i, i);
    emit(&b, "};\nVO main(uint vid : SV_VertexID) {\n  uint idx = el[ebase + vid];\n");
    for (int i = 0; i < 16; i++) if (used_in >> i & 1) emit(&b, "  float4 v%d = fetch(%d, idx);\n", i, i);
    emit(&b, "  float4 r[48]; [unroll] for (int ri = 0; ri < 48; ri++) r[ri] = 0;\n");
    emit(&b, "  float4 o[32]; [unroll] for (int oi = 0; oi < 32; oi++) o[oi] = float4(0, 0, 0, 1);\n");
    emit(&b, "  int4 a0 = 0, a1 = 0; float4 cc0 = 0, cc1 = 0; float4 vt, st;\n");
    for (int i = 0; i < n; i++) {
        const vp_insn_t *I = &prog[i];
        /* sources are textual expressions; r0..r47 are array elements r[k] */
        int vec = I->vec_op != 0, sca = I->sca_op != 0;
        if (vec) {
            emit(&b, "  vt = ");
            #define S(k) vp_src(&b, I, k)
            switch (I->vec_op) {
            case 1: S(0); break;
            case 2: emit(&b, "("); S(0); emit(&b, ") * ("); S(1); emit(&b, ")"); break;
            case 3: emit(&b, "("); S(0); emit(&b, ") + ("); S(2); emit(&b, ")"); break;
            case 4: emit(&b, "mad("); S(0); emit(&b, ", "); S(1); emit(&b, ", "); S(2); emit(&b, ")"); break;
            case 5: emit(&b, "dot(("); S(0); emit(&b, ").xyz, ("); S(1); emit(&b, ").xyz).xxxx"); break;
            case 6: emit(&b, "(dot(("); S(0); emit(&b, ").xyz, ("); S(1); emit(&b, ").xyz) + ("); S(1); emit(&b, ").w).xxxx"); break;
            case 7: emit(&b, "dot("); S(0); emit(&b, ", "); S(1); emit(&b, ").xxxx"); break;
            case 8: emit(&b, "float4(1, ("); S(0); emit(&b, ").y * ("); S(1); emit(&b, ").y, ("); S(0); emit(&b, ").z, ("); S(1); emit(&b, ").w)"); break;
            case 9: emit(&b, "min("); S(0); emit(&b, ", "); S(1); emit(&b, ")"); break;
            case 10: emit(&b, "max("); S(0); emit(&b, ", "); S(1); emit(&b, ")"); break;
            case 11: emit(&b, "(float4)(("); S(0); emit(&b, ") < ("); S(1); emit(&b, "))"); break;
            case 12: emit(&b, "(float4)(("); S(0); emit(&b, ") >= ("); S(1); emit(&b, "))"); break;
            case 13: emit(&b, "floor("); S(0); emit(&b, ")"); break;   /* ARL */
            case 14: emit(&b, "frac("); S(0); emit(&b, ")"); break;
            case 15: emit(&b, "floor("); S(0); emit(&b, ")"); break;
            case 16: emit(&b, "(float4)(("); S(0); emit(&b, ") == ("); S(1); emit(&b, "))"); break;
            case 17: emit(&b, "float4(0, 0, 0, 0)"); break;
            case 18: emit(&b, "(float4)(("); S(0); emit(&b, ") > ("); S(1); emit(&b, "))"); break;
            case 19: emit(&b, "(float4)(("); S(0); emit(&b, ") <= ("); S(1); emit(&b, "))"); break;
            case 20: emit(&b, "(float4)(("); S(0); emit(&b, ") != ("); S(1); emit(&b, "))"); break;
            case 21: emit(&b, "float4(1, 1, 1, 1)"); break;
            case 22: emit(&b, "sign("); S(0); emit(&b, ")"); break;
            default: emit(&b, "float4(0, 0, 0, 0)"); break;
            }
            emit(&b, ";\n");
            if (I->saturate && I->vec_op != 13) emit(&b, "  vt = saturate(vt);\n");
        }
        if (sca) {
            emit(&b, "  { float s = ("); vp_src(&b, I, 2); emit(&b, ").x; st = ");
            switch (I->sca_op) {
            case 1: emit(&b, "s.xxxx"); break;
            case 2: emit(&b, "(1.0 / s).xxxx"); break;
            case 3: emit(&b, "(s > 0 ? clamp(1.0 / s, 5.42101e-20, 1.884467e19) : clamp(1.0 / s, -1.884467e19, -5.42101e-20)).xxxx"); break;
            case 4: emit(&b, "rsqrt(abs(s)).xxxx"); break;
            case 5: emit(&b, "float4(exp2(floor(s)), s - floor(s), exp2(s), 1)"); break;
            case 6: emit(&b, "float4(floor(log2(abs(s))), abs(s) / exp2(floor(log2(abs(s)))), log2(abs(s)), 1)"); break;
            case 7: { emit(&b, "float4(1, 0, 0, 1); { float4 l = "); vp_src(&b, I, 2);
                      emit(&b, "; st.y = max(l.x, 0); st.z = l.x > 0 ? pow(max(l.y, 0), clamp(l.w, -127.9961, 127.9961)) : 0; }"); break; }
            case 13: emit(&b, "log2(abs(s)).xxxx"); break;
            case 14: emit(&b, "exp2(s).xxxx"); break;
            case 15: emit(&b, "sin(s).xxxx"); break;
            case 16: emit(&b, "cos(s).xxxx"); break;
            default: emit(&b, "float4(0, 0, 0, 0)"); break;
            }
            emit(&b, "; }\n");
        }
        if (vec) {
            if (I->vec_op == 13) {   /* ARL writes the address register */
                char dst[8]; snprintf(dst, sizeof dst, "a%u", I->addr_reg);
                for (int k = 0; k < 4; k++) if (I->vec_mask >> (3 - k) & 1) emit(&b, "  %s.%c = (int)vt.%c;\n", dst, SW[k], SW[k]);
            } else {
                char dst[16];
                if (I->vec_tmp != 0x3F) { snprintf(dst, sizeof dst, "r[%u]", I->vec_tmp % 48); vp_write(&b, I, dst, "vt", I->vec_mask); }
                if (I->vec_result && I->out_reg != 0x1F) { snprintf(dst, sizeof dst, "o[%u]", I->out_reg); vp_write(&b, I, dst, "vt", I->vec_mask); }
                if (I->cond_update) { snprintf(dst, sizeof dst, "cc%u", I->cond_reg); vp_write(&b, I, dst, "vt", I->vec_mask); }
            }
        }
        if (sca) {
            char dst[16];
            if (I->sca_tmp != 0x3F) { snprintf(dst, sizeof dst, "r[%u]", I->sca_tmp % 48); vp_write(&b, I, dst, "st", I->sca_mask); }
            if (!I->vec_result && I->out_reg != 0x1F) { snprintf(dst, sizeof dst, "o[%u]", I->out_reg); vp_write(&b, I, dst, "st", I->sca_mask); }
        }
        #undef S
    }
    emit(&b,
"  VO OUT;\n"
"  float w = o[0].w; float3 ndc = o[0].xyz / w;\n"
"  float wx = ndc.x * vscale.x + voff.x, wy = ndc.y * vscale.y + voff.y, wz = ndc.z * vscale.z + voff.z;\n"
"  OUT.pos = float4((wx / surf.x * 2 - 1) * w, (1 - wy / surf.y * 2) * w, wz * w, w);\n");
    static const int map[13] = {1, 2, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};   /* COL0 COL1 FOG TEX0..9 */
    for (int i = 0; i < 13; i++) emit(&b, "  OUT.t%d = o[%d];\n", i, map[i]);
    emit(&b, "  return OUT;\n}\n");
    /* the source refers to r<k> through vp_src; rewrite "r<k>" register names into r[k] array accesses */
    if (b.overflow) return -2;
    /* vp_src printed rN: convert to r[N] in place (simple scan; register names never collide with others) */
    char *tmp = malloc(b.len * 2 + 16); size_t o = 0;
    for (size_t i = 0; i < b.len; i++) {
        char ch = b.p[i];
        if (ch == 'r' && i + 1 < b.len && b.p[i + 1] >= '0' && b.p[i + 1] <= '9' && (i == 0 || !((b.p[i - 1] >= 'a' && b.p[i - 1] <= 'z') || (b.p[i - 1] >= 'A' && b.p[i - 1] <= 'Z') || b.p[i - 1] == '_' || (b.p[i - 1] >= '0' && b.p[i - 1] <= '9')))) {
            tmp[o++] = 'r'; tmp[o++] = '[';
            i++;
            while (i < b.len && b.p[i] >= '0' && b.p[i] <= '9') tmp[o++] = b.p[i++];
            tmp[o++] = ']'; i--;
        } else tmp[o++] = ch;
    }
    if (o >= cap) { free(tmp); return -2; }
    memcpy(out, tmp, o); out[o] = 0; free(tmp);
    return 0;
}

/* ------------------------------------------------------------------ fragment programs */
static const char *fp_in_name(int input) {   /* RSX fragment input -> HLSL expression */
    static const char *n[16] = {"wpos", "IN.t0", "IN.t1", "IN.t2", "IN.t3", "IN.t4", "IN.t5", "IN.t6", "IN.t7", "IN.t8", "IN.t9", "IN.t10", "IN.t11", "IN.t12", "ssa", "float4(0,0,0,0)"};
    return n[input & 15];
}
static void fp_src(sb_t *b, const fp_insn_t *I, int n, int const_slot) {
    const fp_src_t *s = &I->src[n];
    char reg[40];
    if (s->type == 0) snprintf(reg, sizeof reg, "%s[%u]", s->fp16 ? "H" : "R", s->fp16 ? s->index % 96 : s->index % 48);
    else if (s->type == 1) snprintf(reg, sizeof reg, "%s", fp_in_name(I->input));
    else if (s->type == 2) snprintf(reg, sizeof reg, "fc[%d]", const_slot);
    else snprintf(reg, sizeof reg, "float4(0,0,0,0)");
    /* As RPCS3 (FragmentProgramDecompiler::GetSRC): the read's precision modifier clamps it, saturate before abs and the
     * others after it, then the negation; colour inputs (COL0, COL1) read clamped to 0..1 and take no modifier. The paint
     * jobs' pattern stamps test "inside the stamp's box" with SEQ(sat(uv), uv); without the clamp the test always passed
     * and a carbon-fibre stamp covered a kart's whole side (2026-11-01). */
    static int off = -1; if (off < 0) { const char *e = getenv("MNR_FP_PREC"); off = e && *e == '0'; }   /* (tests: the old translation) */
    int prec = off ? 0 : s->prec, colour = !off && s->type == 1 && (I->input == 1 || I->input == 2);
    if (colour || (s->type == 1 && I->input >= 14)) prec = 0;   /* (colour inputs saturated below; ssa and the like as read) */
    if (s->type == 0 && s->fp16 && prec == 1) prec = 0;         /* a half register is half already */
    char v[96], w[140];
    snprintf(v, sizeof v, "(%s).%c%c%c%c", reg, SW[s->swz[0]], SW[s->swz[1]], SW[s->swz[2]], SW[s->swz[3]]);
    if (colour || prec == 4) { snprintf(w, sizeof w, "saturate(%s)", v); strcpy(v, w); }
    if (s->abs) { snprintf(w, sizeof w, "abs(%s)", v); strcpy(v, w); }
    if (prec == 1) { snprintf(w, sizeof w, "clamp(%s, -65504.0, 65504.0)", v); strcpy(v, w); }
    else if (prec == 2) { snprintf(w, sizeof w, "clamp(%s, -2.0, 2.0)", v); strcpy(v, w); }
    else if (prec == 3) { snprintf(w, sizeof w, "clamp(%s, -1.0, 1.0)", v); strcpy(v, w); }
    emit(b, "%s%s", s->neg ? "-" : "", v);
}
static void fp_cond(sb_t *b, const fp_insn_t *I, int k) {
    if (I->exec_lt && I->exec_eq && I->exec_gt) { emit(b, "true"); return; }
    if (!I->exec_lt && !I->exec_eq && !I->exec_gt) { emit(b, "false"); return; }
    char c[24]; snprintf(c, sizeof c, "cc%u.%c", I->cond_reg, SW[I->cond_swz[k]]);
    emit(b, "(false");
    if (I->exec_lt) emit(b, " || %s < 0", c);
    if (I->exec_eq) emit(b, " || %s == 0", c);
    if (I->exec_gt) emit(b, " || %s > 0", c);
    emit(b, ")");
}

int hlsl_fp(const fp_insn_t *prog, int n, uint32_t ctl, const uint8_t tex_dims[16], char *out, size_t cap, int *nconst) {
    sb_t b = {out, cap, 0, 0};
    int nc = 0;
    uint32_t tex_used = 0;
    static __thread uint32_t pos[FP_MAX_INSNS];   /* byte position of each instruction (a constant takes another 16 bytes): flow-control offsets are in words */
    if (n > FP_MAX_INSNS) return -1;
    for (int i = 0, p = 0; i < n; i++) { pos[i] = (uint32_t)p; p += prog[i].has_const ? 32 : 16; }
    for (int i = 0; i < n; i++) {
        if (prog[i].op == 0x41) return -1;   /* CAL (subroutines): not translated */
        if (prog[i].has_const) nc++;
        uint8_t op = prog[i].op;
        if (op == 0x17 || op == 0x18 || op == 0x19 || op == 0x2F || op == 0x31) tex_used |= 1u << prog[i].tex;
    }
    if (nconst) *nconst = nc;
    emit(&b,
"cbuffer FPC : register(b0) { float4 fc[%d]; };\n"
"cbuffer FPD : register(b1) { uint4 remap[16]; float4 alpha_ref; uint4 alpha_func; float4 tex_size[16]; };\n"
"float4 apply_remap(float4 raw, uint4 rm) {   /* raw: A,R,G,B; rm.x selectors, rm.y ops, rm.z 1 = raw is R,G,B,A */\n"
"  if (rm.w == 2) { uint u = (uint)round(raw.x * 16777215.0); raw = float4((u >> 16) & 255, (u >> 8) & 255, u & 255, 0) / 255.0; }\n"   /* Z24S8 read as A8R8G8B8: the depth bytes (stencil not available) */
"  else if (rm.w) raw = raw.xxxx;\n"
"  else if (rm.z == 2) raw = raw.xxxx;\n"   /* B8 render target (R8 in D3D): the RSX returns the byte in every channel */
"  else if (rm.z) raw = raw.wxyz;\n"
"  float res[4]; float r4[4] = {raw.x, raw.y, raw.z, raw.w};\n"
"  [unroll] for (uint c = 0; c < 4; c++) { uint op = (rm.y >> (2 * c)) & 3, sel = (rm.x >> (2 * c)) & 3; res[c] = op == 0 ? 0 : op == 1 ? 1 : r4[sel]; }\n"
"  return float4(res[1], res[2], res[3], res[0]);\n"
"}\n", nc > 0 ? nc : 1);
    for (int t = 0; t < 16; t++)
        if (tex_used >> t & 1) {
            const char *ty = tex_dims[t] == 3 ? "Texture3D" : tex_dims[t] == 6 ? "TextureCube" : "Texture2D";
            emit(&b, "%s tex%d : register(t%d); %s smp%d : register(s%d);\n", ty, t, t, tex_dims[t] & 0x80 ? "SamplerComparisonState" : "SamplerState", t, t);
        }
    emit(&b, "struct PI { float4 pos : SV_Position;\n");
    for (int i = 0; i < 13; i++) emit(&b, "  float4 t%d : TEXCOORD%d;\n", i, i);
    emit(&b, "  bool front : SV_IsFrontFace; };\n");
    int depth_out = (ctl & 0x0E) != 0;
    emit(&b, "struct PO { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3;%s };\n",
         depth_out ? " float depth : SV_Depth;" : "");
    /* outputs, depth and the RSX alpha test: in a function so RET can end the program early */
    emit(&b, "PO finish(float4 R0, float4 R1, float4 R2, float4 R3, float4 R4, float4 H0, float4 H4, float4 H6, float4 H8) {\n  PO OUT;\n");
    if (ctl & 0x40) emit(&b, "  OUT.c0 = R0; OUT.c1 = R2; OUT.c2 = R3; OUT.c3 = R4;\n");
    else emit(&b, "  OUT.c0 = H0; OUT.c1 = H4; OUT.c2 = H6; OUT.c3 = H8;\n");
    if (depth_out) emit(&b, "  OUT.depth = saturate(R1.z);\n");
    emit(&b, "  { float a = OUT.c0.w, ref = alpha_ref.x; uint f = alpha_func.x; bool apass = true;\n"
             "    if (alpha_func.y) { apass = f == 0 ? false : f == 1 ? a < ref : f == 2 ? a == ref : f == 3 ? a <= ref : f == 4 ? a > ref : f == 5 ? a != ref : f == 6 ? a >= ref : true; }\n"
             "    if (!apass) discard; }\n");
    { static int flat = -1; if (flat < 0) flat = getenv("MNR_D3D_FLAT") != NULL;   /* debug: every fragment a flat colour */
      if (flat) emit(&b, "  if (alpha_func.z) OUT.c0 = float4(frac(alpha_func.w * float3(0.6180339, 0.3710211, 0.1373)), 1);\n"); }
    emit(&b, "  return OUT;\n}\n");
    size_t wpos_def = b.len;
    emit(&b, "PO main(PI IN) {\n  float4 wpos = float4(IN.pos.xy, IN.pos.z, 1.0 / IN.pos.w);\n  float4 ssa = float4(IN.front ? 1 : -1, 0, 0, 0);\n");
    emit(&b, "  float4 R[48]; float4 H[96]; [unroll] for (int i = 0; i < 48; i++) R[i] = 0; [unroll] for (int j = 0; j < 96; j++) H[j] = 0;\n");
    emit(&b, "  float4 cc0 = 0, cc1 = 0, t;\n");
    int slot = 0;
    static const float scales[8] = {1, 2, 4, 8, 1, 0.5f, 0.25f, 0.125f};
    /* flow control (IFE / LOOP / REP / BRK / RET): blocks close at their end offset, IFE switches at its else offset */
    struct { uint32_t else_pos, end_pos; } open[16]; int nopen = 0, nloops = 0;
    for (int i = 0; i < n; i++) {
        const fp_insn_t *I = &prog[i];
        while (nopen > 0 && open[nopen - 1].end_pos <= pos[i]) { emit(&b, "  }\n"); nopen--; }
        if (nopen > 0 && open[nopen - 1].else_pos == pos[i] && open[nopen - 1].else_pos < open[nopen - 1].end_pos) {
            emit(&b, "  } else {\n"); open[nopen - 1].else_pos = 0xFFFFFFFF; }
        if (I->op & 0x40) {
            switch (I->op) {
            case 0x42:   /* IFE */
                if (nopen == 16) return -1;
                emit(&b, "  if ("); fp_cond(&b, I, 0); emit(&b, ") {\n");
                open[nopen].else_pos = (uint32_t)I->else_off * 4; open[nopen].end_pos = (uint32_t)I->end_off * 4; nopen++;
                break;
            case 0x43: case 0x44:   /* LOOP / REP: rep_count iterations (the loop counter register is not emulated) */
                if (nopen == 16) return -1;
                emit(&b, "  [loop] for (int lc%d = 0; lc%d < %u; lc%d++) {\n", nloops, nloops, I->rep_count, nloops); nloops++;
                open[nopen].else_pos = 0xFFFFFFFF; open[nopen].end_pos = (uint32_t)I->end_off * 4; nopen++;
                break;
            case 0x40:   /* BRK */
                emit(&b, "  if ("); fp_cond(&b, I, 0); emit(&b, ") break;\n");
                break;
            case 0x45:   /* RET: from main, the program ends with the outputs as they are */
                emit(&b, "  if ("); fp_cond(&b, I, 0); emit(&b, ") return finish(R[0], R[1], R[2], R[3], R[4], H[0], H[4], H[6], H[8]);\n");
                break;
            default: return -1;
            }
            continue;
        }
        int cs = I->has_const ? slot++ : 0;
        #define S(k) fp_src(&b, I, k, cs)
        int write = 1;
        const char *bx2pre = I->bx2 ? "(2 * (" : "(", *bx2post = I->bx2 ? ") - 1)" : ")";
        #define S0 do { emit(&b, "%s", bx2pre); S(0); emit(&b, "%s", bx2post); } while (0)
        emit(&b, "  t = ");
        switch (I->op) {
        case 0x00: emit(&b, "0"); write = 0; break;
        case 0x01: S0; break;
        case 0x02: S0; emit(&b, " * "); S(1); break;
        case 0x03: S0; emit(&b, " + "); S(1); break;
        case 0x04: emit(&b, "mad("); S0; emit(&b, ", "); S(1); emit(&b, ", "); S(2); emit(&b, ")"); break;
        case 0x05: emit(&b, "dot(("); S0; emit(&b, ").xyz, ("); S(1); emit(&b, ").xyz).xxxx"); break;
        case 0x06: emit(&b, "dot("); S0; emit(&b, ", "); S(1); emit(&b, ").xxxx"); break;
        case 0x07: emit(&b, "float4(1, ("); S0; emit(&b, ").y * ("); S(1); emit(&b, ").y, ("); S0; emit(&b, ").z, ("); S(1); emit(&b, ").w)"); break;
        case 0x08: emit(&b, "min("); S0; emit(&b, ", "); S(1); emit(&b, ")"); break;
        case 0x09: emit(&b, "max("); S0; emit(&b, ", "); S(1); emit(&b, ")"); break;
        case 0x0A: emit(&b, "(float4)("); S0; emit(&b, " < "); S(1); emit(&b, ")"); break;
        case 0x0B: emit(&b, "(float4)("); S0; emit(&b, " >= "); S(1); emit(&b, ")"); break;
        case 0x0C: emit(&b, "(float4)("); S0; emit(&b, " <= "); S(1); emit(&b, ")"); break;
        case 0x0D: emit(&b, "(float4)("); S0; emit(&b, " > "); S(1); emit(&b, ")"); break;
        case 0x0E: emit(&b, "(float4)("); S0; emit(&b, " != "); S(1); emit(&b, ")"); break;
        case 0x0F: emit(&b, "(float4)("); S0; emit(&b, " == "); S(1); emit(&b, ")"); break;
        case 0x10: emit(&b, "frac("); S0; emit(&b, ")"); break;
        case 0x11: emit(&b, "floor("); S0; emit(&b, ")"); break;
        case 0x12: emit(&b, "0;\n  if (");   /* KIL */
            for (int k = 0; k < 4; k++) { if (k) emit(&b, " || "); fp_cond(&b, I, k); }
            { static int flat = -1; if (flat < 0) flat = getenv("MNR_D3D_FLAT") != NULL; emit(&b, flat ? ") {}" : ") discard"); }
            write = 0; break;
        case 0x15: emit(&b, "ddx("); S0; emit(&b, ")"); break;
        case 0x16: emit(&b, "ddy("); S0; emit(&b, ")"); break;
        case 0x17: case 0x18: case 0x19: case 0x2F: case 0x31: {
            int u = I->tex;
            if (tex_dims[u] & 0x80) {   /* shadow compare: r (projected for TXP) against the depth texel, 0/1 in every channel */
                emit(&b, "apply_remap((float4)tex%d.SampleCmpLevelZero(smp%d, (", u, u);
                if (I->op == 0x18) { S0; emit(&b, ").xy / ("); S0; emit(&b, ").w * tex_size[%d].xy, (", u); S0; emit(&b, ").z / ("); S0; emit(&b, ").w)"); }
                else { S0; emit(&b, ").xy * tex_size[%d].xy, (", u); S0; emit(&b, ").z)"); }
                emit(&b, ", remap[%d])", u);
                break;
            }
            const char *coord = tex_dims[u] == 2 || tex_dims[u] == 0 || tex_dims[u] == 1 ? "xy" : "xyz";
            emit(&b, "apply_remap(");
            /* 2D: coordinates are scaled by tex_size[u].xy (a render-target surface bigger than the texture, AA
             * storage, unnormalized coordinates; 1 otherwise) */
            char sc[48] = "";
            if (coord[2] == 0) snprintf(sc, sizeof sc, " * tex_size[%d].xy", u);
            if (I->op == 0x18) { emit(&b, "tex%d.Sample(smp%d, (", u, u); S0; emit(&b, ").%s / (", coord); S0; emit(&b, ").w%s)", sc); }
            else if (I->op == 0x2F) { emit(&b, "tex%d.SampleLevel(smp%d, (", u, u); S0; emit(&b, ").%s%s, (", coord, sc); S(1); emit(&b, ").x)"); }
            else if (I->op == 0x31) { emit(&b, "tex%d.SampleBias(smp%d, (", u, u); S0; emit(&b, ").%s%s, clamp((", coord, sc); S0; emit(&b, ").w, -15, 15))"); }
            else { emit(&b, "tex%d.Sample(smp%d, (", u, u); S0; emit(&b, ").%s%s)", coord, sc); }
            emit(&b, ", remap[%d])", u);
            break;
        }
        case 0x1A: emit(&b, "(1.0 / ("); S0; emit(&b, ").x).xxxx"); break;
        case 0x1B: emit(&b, "rsqrt(abs(("); S0; emit(&b, ").x)).xxxx"); break;
        case 0x1C: emit(&b, "exp2(("); S0; emit(&b, ").x).xxxx"); break;
        /* LG2 of a negative value: -inf, not log2 of its magnitude. The water shader feeds 1 - depth into a
         * pow(); with abs() deep water got full shoreline foam and turned white (the RSX gives no foam there). */
        case 0x1D: emit(&b, "log2(max(("); S0; emit(&b, ").x, 0)).xxxx"); break;
        case 0x1E: emit(&b, "lit_rsx("); S0; emit(&b, ")"); break;
        case 0x1F: emit(&b, "lerp("); S(2); emit(&b, ", "); S(1); emit(&b, ", "); S0; emit(&b, ")"); break;
        case 0x20: emit(&b, "float4(1, 1, 1, 1)"); break;
        case 0x21: emit(&b, "float4(0, 0, 0, 0)"); break;
        case 0x22: emit(&b, "cos(("); S0; emit(&b, ").x).xxxx"); break;
        case 0x23: emit(&b, "sin(("); S0; emit(&b, ").x).xxxx"); break;
        case 0x26: emit(&b, "pow(("); S0; emit(&b, ").x, ("); S(1); emit(&b, ").x).xxxx"); break;
        case 0x2E: emit(&b, "(dot(("); S0; emit(&b, ").xy, ("); S(1); emit(&b, ").xy) + ("); S(2); emit(&b, ").z).xxxx"); break;
        case 0x36: emit(&b, "refl_rsx("); S0; emit(&b, ", "); S(1); emit(&b, ")"); break;
        case 0x38: emit(&b, "dot(("); S0; emit(&b, ").xy, ("); S(1); emit(&b, ").xy).xxxx"); break;
        case 0x39: emit(&b, "float4(normalize(("); S0; emit(&b, ").xyz), ("); S0; emit(&b, ").w)"); break;
        case 0x3A: S0; emit(&b, " / ("); S(1); emit(&b, ").x"); break;
        case 0x3B: S0; emit(&b, " / sqrt(("); S(1); emit(&b, ").x)"); break;
        case 0x3D: case 0x3E: emit(&b, "0"); write = 0; break;
        default: S0; break;   /* PK/UP and other rare ops: pass-through, like the software renderer */
        }
        emit(&b, ";\n");
        #undef S0
        #undef S
        if (!write) continue;
        if (I->scale) emit(&b, "  t *= %g;\n", scales[I->scale]);
        if (I->saturate) emit(&b, "  t = saturate(t);\n");
        else if (I->prec >= 2) emit(&b, "  t = clamp(t, -2, 2);\n");
        char dst[16];
        if (I->dst_fp16) snprintf(dst, sizeof dst, "H[%u]", I->dst % 96); else snprintf(dst, sizeof dst, "R[%u]", I->dst % 48);
        for (int k = 0; k < 4; k++) {
            if (!(I->mask >> (3 - k) & 1)) continue;
            emit(&b, "  if ("); fp_cond(&b, I, k); emit(&b, ") {");
            if (I->set_cond) emit(&b, " cc%u.%c = t.%c;", I->cond_mod_reg, SW[k], SW[k]);
            if (!I->no_dest) emit(&b, " %s.%c = t.%c;", dst, SW[k], SW[k]);
            emit(&b, " }\n");
        }
    }
    while (nopen > 0) { emit(&b, "  }\n"); nopen--; }
    emit(&b, "  return finish(R[0], R[1], R[2], R[3], R[4], H[0], H[4], H[6], H[8]);\n}\n");
    if (b.overflow) return -2;
    {   /* a scene drawn larger than the game's size (MNR_RESOLUTION): programs that read the screen position get it in the
         * game's pixels (wps, in FPD after tex_size). Only those change, so the others keep their cached compiles. */
        extern int rsx_d3d_res_scaled(void);
        if (rsx_d3d_res_scaled() && strstr(out + wpos_def + 40, "wpos")) {
            static const char *at[2] = {"float4 tex_size[16];", "float4(IN.pos.xy"}, *ins[2] = {" float4 wps;", " * wps.xy"};
            for (int k = 1; k >= 0; k--) {   /* (the later one first: the earlier insert moves it) */
                char *q = k ? strstr(out + wpos_def, at[k]) : strstr(out, at[k]); size_t il = strlen(ins[k]);
                if (!q || b.len + il + 1 > cap) return -2;
                q += strlen(at[k]); memmove(q + il, q, out + b.len + 1 - q); memcpy(q, ins[k], il); b.len += il;
            }
        }
    }
    /* helper functions referenced above, prepended */
    static const char helpers[] =
        "float4 lit_rsx(float4 l) { return float4(1, max(l.x, 0), l.x > 0 ? pow(max(l.y, 0), clamp(l.w, -127.9961, 127.9961)) : 0, 1); }\n"
        "float4 refl_rsx(float4 a, float4 n) { float d = dot(a.xyz, n.xyz), nn = dot(n.xyz, n.xyz); return float4(a.xyz - (nn != 0 ? 2 * d / nn : 0) * n.xyz, a.w); }\n";
    size_t hl = strlen(helpers);
    if (b.len + hl + 1 > cap) return -2;
    memmove(out + hl, out, b.len + 1);
    memcpy(out, helpers, hl);
    return 0;
}
