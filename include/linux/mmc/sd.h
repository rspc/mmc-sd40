/*
 *  include/linux/mmc/sd.h
 *
 *  Copyright (C) 2005-2007 Pierre Ossman, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef LINUX_MMC_SD_H
#define LINUX_MMC_SD_H

/* SD commands                           type  argument     response */
  /* class 0 */
/* This is basically the same command as for MMC with some quirks. */
#define SD_SEND_RELATIVE_ADDR     3   /* bcr                     R6  */
#define SD_SEND_IF_COND           8   /* bcr  [11:0] See below   R7  */
#define SD_SWITCH_VOLTAGE         11  /* ac                      R1  */

  /* class 10 */
#define SD_SWITCH                 6   /* adtc [31:0] See below   R1  */

  /* class 5 */
#define SD_ERASE_WR_BLK_START    32   /* ac   [31:0] data addr   R1  */
#define SD_ERASE_WR_BLK_END      33   /* ac   [31:0] data addr   R1  */

  /* Application commands */
#define SD_APP_SET_BUS_WIDTH      6   /* ac   [1:0] bus width    R1  */
#define SD_APP_SD_STATUS         13   /* adtc                    R1  */
#define SD_APP_SEND_NUM_WR_BLKS  22   /* adtc                    R1  */
#define SD_APP_OP_COND           41   /* bcr  [31:0] OCR         R3  */
#define SD_APP_SEND_SCR          51   /* adtc                    R1  */

/* OCR bit definitions */
#define SD_OCR_S18R		(1 << 24)    /* 1.8V switching request */
#define SD_ROCR_S18A		SD_OCR_S18R  /* 1.8V switching accepted by card */
#define SD_OCR_XPC		(1 << 28)    /* SDXC power control */
#define SD_OCR_CCS		(1 << 30)    /* Card Capacity Status */

/*
 * SD_SWITCH argument format:
 *
 *      [31] Check (0) or switch (1)
 *      [30:24] Reserved (0)
 *      [23:20] Function group 6
 *      [19:16] Function group 5
 *      [15:12] Function group 4
 *      [11:8] Function group 3
 *      [7:4] Function group 2
 *      [3:0] Function group 1
 */

/*
 * SD_SEND_IF_COND argument format:
 *
 *	[31:12] Reserved (0)
 *	[11:8] Host Voltage Supply Flags
 *	[7:0] Check Pattern (0xAA)
 */

/*
 * SCR field definitions
 */

#define SCR_SPEC_VER_0		0	/* Implements system specification 1.0 - 1.01 */
#define SCR_SPEC_VER_1		1	/* Implements system specification 1.10 */
#define SCR_SPEC_VER_2		2	/* Implements system specification 2.00-3.0X */

/*
 * SD bus widths
 */
#define SD_BUS_WIDTH_1		0
#define SD_BUS_WIDTH_4		2

/*
 * SD_SWITCH mode
 */
#define SD_SWITCH_CHECK		0
#define SD_SWITCH_SET		1

/*
 * SD_SWITCH function groups
 */
#define SD_SWITCH_GRP_ACCESS	0

/*
 * SD_SWITCH access modes
 */
#define SD_SWITCH_ACCESS_DEF	0
#define SD_SWITCH_ACCESS_HS	1

#define UHSII_IOADR(base, reg)			(((u16)(base) << 8) | (reg))
#define UHSII_IOADR_BASE(arg)			(((arg) >> 8) & 0x0F)
#define UHSII_IOADR_OFFSET(arg)			((arg) & 0xFF)

/* IOADR of Generic Capabilities Register (DW) */
#define SD40_IOADR_GEN_CAP_L			0x00
#define SD40_IOADR_GEN_CAP_H			0x01

/* IOADR of PHY Capabilities Register (DW)  */
#define SD40_IOADR_PHY_CAP_L			0x02
#define SD40_IOADR_PHY_CAP_H			0x03

/* IOADR LINK/TRAN Capabilities Register (DW) */
#define SD40_IOADR_LINK_CAP_L			0x04
#define SD40_IOADR_LINK_CAP_H			0x05

#define SD40_IOADR_GEN_SET_L			0x08
#define SD40_IOADR_GEN_SET_H			0x09

#define SD40_IOADR_PHY_SET_L			0x0A
#define SD40_IOADR_PHY_SET_H			0x0B

#define SD40_IOADR_LINK_SET_L			0x0C
#define SD40_IOADR_LINK_SET_H			0x0D

/* Node ID (First or Last) ENUMERATE */
#define SD40_IDL_SHIFT				24
#define SD40_IDL_MASK				(0x0F << SD40_IDL_SHIFT)

/* SD40 I/O Address space offset */
#define SD40_IOADR_CFG_BASE			0x00  /*000h : 0FFh*/
#define SD40_IOADR_INT_BASE			0x01  /*100h : 17Fh*/
#define SD40_IOADR_ST_BASE			0x01  /*180h : 1FFh*/
#define SD40_IOADR_CMD_BASE			0x02  /*200h : 2FFh*/
#define SD40_IOADR_VENDOR_BASE			0x0F  /*F00h : FFFh*/

/* Command Register (CMD_REG).  DW, Base on IOADR_CMD_BASE */
#define SD40_FULL_RESET				0x00
#define SD40_GO_DORMANT_STATE			0x01
#define SD40_DEVICE_INIT			0x02
#define SD40_ENUMERATE				0x03
#define SD40_TRANS_ABORT			0x04

/* DEVICE_INIT */
#define SD40_GD_SHIFT				28
#define SD40_GD_MASK				(0x0F << SD40_GD_SHIFT)
#define SD40_GAP_SHIFT				24
#define SD40_GAP_MASK				(0x0F << SD40_GAP_SHIFT)
#define SD40_DAP_SHIFT				20
#define SD40_DAP_MASK				(0x0F << SD40_DAP_SHIFT)
#define SD40_CF					0x80000

#define SD40_GD(val)		(((val) << SD40_GD_SHIFT) & SD40_GD_MASK)
#define SD40_GAP(val)		(((val) << SD40_GAP_SHIFT) & SD40_GAP_MASK)
#define SD40_DAP(val)		(((val) << SD40_DAP_SHIFT) & SD40_DAP_MASK)

/* Generic Capabilities Register */
#define SD40_LANE_MODE_SHIFT			8
#define SD40_LANE_MODE_MASK			(0x3F << SD40_LANE_MODE_SHIFT)
#define SD40_LANE_MODE_2L_HD			0x01
#define SD40_LANE_MODE_2D1U_FD			0x02
#define SD40_LANE_MODE_1D2U_FD			0x04
#define SD40_LANE_MODE_2D2U_FD			0x08

#define SD40_APP_TYPE_MASK			0x07
#define SD40_APP_TYPE_SD_MEMORY			0x01
#define SD40_APP_TYPE_SDIO			0x02
#define SD40_APP_TYPE_EMBEDDED			0x04

/* Generic Settings Register */
#define SD40_CONFIG_COMPLETE			0x80000000
#define SD40_LOW_PWR_MODE			0x01

#endif /* LINUX_MMC_SD_H */
