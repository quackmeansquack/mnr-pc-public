/* RSX shader microcode: decoded forms of vertex programs (transform programs) and fragment programs. */
#pragma once
#include <stdint.h>

/* ---- vertex program (NV40-style 128-bit instructions, one vector + one scalar op each) */
typedef struct {
    uint8_t type, index, swz[4], neg, abs;   /* type: 1 temp, 2 input, 3 constant */
} vp_src_t;
typedef struct {
    uint8_t vec_op, sca_op;
    vp_src_t src[3];
    uint16_t const_index;        /* constant register for type-3 sources */
    uint8_t input_index;         /* input attribute for type-2 sources */
    uint8_t index_const;         /* add the address register to const_index */
    uint8_t addr_reg, addr_swz;
    uint8_t vec_tmp, sca_tmp;    /* 0x3F = none */
    uint8_t out_reg;             /* 0x1F = none */
    uint8_t vec_result;          /* 1: the vector unit writes out_reg, 0: the scalar unit does */
    uint8_t vec_mask, sca_mask;  /* bit 3 = x .. bit 0 = w */
    uint8_t saturate, end;
    uint8_t cond_test, cond, cond_swz[4], cond_update, cond_reg;
    uint8_t tex_num;             /* TXL: vertex texture unit (word 2 bits 8..9, inside the unused src1 field) */
} vp_insn_t;
void vp_decode(const uint32_t w[4], vp_insn_t *o);
int vp_disasm(const vp_insn_t *i, char *buf, int cap);

/* ---- fragment program (128-bit instructions, halfword-swapped words, inline constants) */
typedef struct {
    uint8_t type, index, fp16, swz[4], neg, abs;   /* type: 0 temp, 1 input, 2 constant */
    uint8_t prec;   /* precision modifier on the read (in the second source word): 0 none, 1 half, 2 fixed12, 3 fixed9, 4 saturate */
} fp_src_t;
typedef struct {
    uint8_t op, dst, dst_fp16, mask, no_dest, saturate, set_cond, end;
    uint8_t input, tex, prec, scale, bx2;
    uint8_t exec_lt, exec_eq, exec_gt, cond_swz[4], cond_reg, cond_mod_reg;
    fp_src_t src[3];
    float constant[4];
    uint8_t has_const;
    uint8_t persp;
    /* flow control */
    uint16_t else_off, end_off, rep_init, rep_inc, rep_count;
} fp_insn_t;
#define FP_MAX_INSNS 2048   /* fragment program length limit (the avatar outline shader runs past 512) */
int fp_decode(uint32_t guest_addr, fp_insn_t *out, int max);   /* returns instruction count */
int fp_decode_buf(const uint8_t *buf, uint32_t size, fp_insn_t *out, int max);   /* the same from a copy of its bytes */
uint32_t fp_bytes(const fp_insn_t *prog, int n);   /* bytes the program takes in memory */
int fp_disasm(const fp_insn_t *i, char *buf, int cap);
