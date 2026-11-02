/* SPU interpreter for SPURS task programs (spu.c). */
#pragma once
#include <stdint.h>
/* run the SPU ELF at guest address `elf` with the 16-byte task argument; 0 when it stopped normally */
int spu_run_task(uint32_t elf, const uint32_t arg[4], uint64_t taskset_arg, uint64_t max_steps);
