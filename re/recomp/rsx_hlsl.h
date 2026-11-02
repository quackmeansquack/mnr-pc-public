/* RSX program -> HLSL translation (rsx_hlsl.c). Return 0 on success, -1 unsupported, -2 buffer full. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "rsx_shader.h"

int hlsl_vp(const uint32_t (*words)[4], uint32_t start, char *out, size_t cap, uint32_t *inputs_used);
int hlsl_fp(const fp_insn_t *prog, int n, uint32_t ctl, const uint8_t tex_dims[16], char *out, size_t cap, int *nconst);
