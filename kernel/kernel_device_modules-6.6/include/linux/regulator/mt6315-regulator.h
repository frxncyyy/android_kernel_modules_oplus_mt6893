/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __LINUX_REGULATOR_MT6315_H
#define __LINUX_REGULATOR_MT6315_H

#define MT6315_SLAVE_ID_3	3
#define MT6315_SLAVE_ID_6	6
#define MT6315_SLAVE_ID_7	7
#define MT6315_SLAVE_ID_8	8
#define MT6315_SLAVE_ID_15	15

/*
 * op6893 6.6 bring-up: these bound the probe loop in mt6315-regulator.c, which
 * stops at index _MAX, and the indices are the shared enum below --
 * VBUCK1 = 0, VBUCK2 = 1, VBUCK3 = 2, VBUCK4 = 3.  The stock values of 3
 * therefore cover only VBUCK1..VBUCK3, and since this board's DTB defines
 * vbuck1, vbuck3 and vbuck4 and no vbuck2 at all, the rail the loop drops is
 * always vbuck4.  On S3 that is VSRAM_MD, the modem's SRAM rail: 3_vbuck4 does
 * not exist on 6.6 (it does on 4.19), so md_cd_power_on() cannot reach the one
 * rail the MD's bootrom most depends on.  4 is the correct bound.
 *
 * 6 and 7 are left alone for now: raising them would additionally register
 * 6_vbuck4 and 7_vbuck4, which is also what 4.19 does, but neither has been
 * needed yet and this port's VGPU work was tuned against the current set.
 */
#define MT6315_ID_3_MAX		4	/* VBUCK1, VBUCK2, VBUCK3, VBUCK4 */
#define MT6315_ID_6_MAX		3
#define MT6315_ID_7_MAX		3
#define MT6315_ID_8_MAX		3
#define MT6315_ID_15_MAX	2

enum {
	MT6315_ID_VBUCK1 = 0,
	MT6315_ID_VBUCK2,
	MT6315_ID_VBUCK3,
	MT6315_ID_VBUCK4,
	MT6315_ID_MAX,
};

/* Register */
#define MT6315_SWCID_H				0xB
#define MT6315_TOP2_ELR7			0x139
#define MT6315_TOP_TMA_KEY			0x39F
#define MT6315_TOP_TMA_KEY_H			0x3A0
#define MT6315_BUCK_TOP_CON0			0x1440
#define MT6315_BUCK_TOP_CON1			0x1443
#define MT6315_BUCK_TOP_ELR0			0x1449
#define MT6315_BUCK_TOP_ELR2			0x144B
#define MT6315_BUCK_TOP_ELR4			0x144D
#define MT6315_BUCK_TOP_ELR6			0x144F
#define MT6315_VBUCK1_DBG0			0x1499
#define MT6315_VBUCK1_DBG4			0x149D
#define MT6315_VBUCK2_DBG0			0x1519
#define MT6315_VBUCK2_DBG4			0x151D
#define MT6315_VBUCK3_DBG0			0x1599
#define MT6315_VBUCK3_DBG4			0x159D
#define MT6315_VBUCK4_DBG0			0x1619
#define MT6315_VBUCK4_DBG4			0x161D
#define MT6315_BUCK_TOP_4PHASE_ANA_CON42	0x16B1

#define PROTECTION_KEY_H			0x9C
#define PROTECTION_KEY				0xEA

#endif /* __LINUX_REGULATOR_MT6315_H */
