/* SPU interpreter for SPURS task programs (spu.c). */
#pragma once
#include <stdint.h>
/* run the SPU ELF at guest address `elf` with the 16-byte task argument; 0 when it stopped normally */
int spu_run_task(uint32_t elf, const uint32_t arg[4], uint64_t taskset_arg, uint64_t max_steps);
int spu_run_job(uint32_t job, uint32_t desc_size);   /* SPURS job binary (spu2c.py job: programs) */
void spu_occ_dump(long flip);
extern double spu_seconds;
extern uint64_t spu_steps;
