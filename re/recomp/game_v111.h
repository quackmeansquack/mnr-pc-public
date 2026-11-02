/* Guest addresses specific to one build of the EBOOT: the 1.11 patch (bin_v111/EBOOT.elf, from
 * UP9000-BCUS98167_00-MODNATIONKART002-A0111-V0200-PE.pkg). build.py --game v111 copies this file to
 * out_v111/game.h. Mapped from game_v100.h with re/scripts/map_addrs.py (matched code; TOC globals through
 * the instructions that reach them). */
#pragma once
#define G_NAME             "1.11 (patch)"
#define G_TOC              0x00EF7EC0u
#define G_ENTRY_OPD        0x00EE43E0u
#define G_LR_DXT_JOB       0x7C48CCu
#define G_LR_QTASK_A       0x7C693Cu
#define G_LR_QTASK_B       0x7C6C48u
#define G_LR_QTASK_SYNC    0x7C6CA4u
#define G_LR_QTASK_D       0x7C6DECu
#define G_LR_SKINNING      0x7CFBD0u     /* (program 0x13C1E80) */
#define G_QTASK_EMPTY_OPD  0x00ED0100u   /* (empty function 0x2192D8) */
#define G_FN_QTASK_STAT    0x23BAF8u
#define G_HAVOK_ASYNC_FLAG 0x00EFD02Cu   /* (read at 0x1C5698 and 0x1C5808, the hooked functions) */
#define G_SPU2AB_FLAG      0x00F020CDu
#define G_TEXT_END         0x00D53B78u
#define G_PATCH_FILE       "patches_v111.txt"
#define G_FLIP_COUNT       0x01547340u   /* u32 the flip handler (0x7CD690) increments per flip */
#define G_FLIP_TIME        0x01547858u   /* u64 time base the flip handler stores at each flip (read by 0x7C8FE8) */
#define G_AI_CONTROLS_HUMANS 0x0145F270u   /* byte set from the debug option "AIControlsHumans" (FUN_005E0510); FUN_00294D14 gives human karts an AI driver when it changes */
