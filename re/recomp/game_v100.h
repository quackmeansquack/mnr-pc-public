/* Guest addresses specific to one build of the EBOOT: the disc version 1.00 (bin/EBOOT.elf).
 * build.py --game v100 copies this file to out/game.h. Mapped to other builds with re/scripts/map_addrs.py. */
#pragma once
#define G_NAME             "1.00 (disc)"
#define G_TOC              0x00EC7BF0u   /* r2 for every function */
#define G_ENTRY_OPD        0x00EB4130u   /* entry point descriptor {code, toc} */
#define G_LR_DXT_JOB       0x7A352Cu     /* return address of the SPU DXT compression job launch */
#define G_LR_QTASK_A       0x7B42C8u     /* qTask manager SPU launch call sites (return addresses) */
#define G_LR_QTASK_B       0x7B45D4u
#define G_LR_QTASK_SYNC    0x7B4630u     /* "PshSync": the task main is waiting on */
#define G_LR_QTASK_D       0x7B4778u
#define G_LR_SKINNING      0x7B0730u     /* skinning job launch (program 0xF18580) */
#define G_QTASK_EMPTY_OPD  0x00EA0A70u   /* descriptor of an empty function (0x20C9D8) */
#define G_FN_QTASK_STAT    0x22F1C0u     /* qTask function sampled by MNR_QTASK_STATS */
#define G_HAVOK_ASYNC_FLAG 0x00ECB74Cu   /* byte: Havok runs its physics step as an SPU job */
#define G_SPU2AB_FLAG      0x00ED1D95u   /* byte: subsystem 0x2AB uses an SPU job (read by 0x2ABD00 ...) */
#define G_TEXT_END         0x00D30A28u   /* end of the recompiled code (patches below it cannot apply at run time) */
#define G_PATCH_FILE       "patches_v100.txt"
