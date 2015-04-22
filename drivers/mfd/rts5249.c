/* Driver for Realtek PCI-Express card reader
 *
 * Copyright(c) 2009-2013 Realtek Semiconductor Corp. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author:
 *   Wei WANG <wei_wang@realsil.com.cn>
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/mfd/rtsx_pci.h>

#include "rtsx_pcr.h"

static u8 rts5249_get_ic_version(struct rtsx_pcr *pcr)
{
	u8 val;

	rtsx_pci_read_register(pcr, DUMMY_REG_RESET_0, &val);
	return val & 0x0F;
}

static void rts5249_sd40_auto_K_apm_offset(struct rtsx_pcr *pcr)
{
	u16 val1, val2;

	if (pcr->ic_version == IC_VER_A) {
		rtsx_pci_write_phy_register(pcr, 0x9E, 0x85);
		rtsx_pci_write_phy_register(pcr, 0x85, 0x244B);
		rtsx_pci_write_phy_register(pcr, 0x85, 0x244F);
		rtsx_pci_write_phy_register(pcr, 0x9E, 0x80);
	} else {
		rtsx_pci_read_phy_register(pcr, 0x9E, &val1);
		rtsx_pci_write_phy_register(pcr, 0x9E, val1 | 0x05);
		rtsx_pci_read_phy_register(pcr, 0x85, &val2);
		rtsx_pci_write_phy_register(pcr, 0x85, val2 & 0xFFFB);
		rtsx_pci_write_phy_register(pcr, 0x85, val2 | 0x4);
		rtsx_pci_write_phy_register(pcr, 0x9E, val1 & ~0x05);
	}
}

static void rts5249_fill_driving(struct rtsx_pcr *pcr, u8 voltage)
{
	u8 driving_3v3[4][3] = {
		{0x11, 0x11, 0x18},
		{0x55, 0x55, 0x5C},
		{0xFF, 0xFF, 0xFF},
		{0x96, 0x96, 0x96},
	};
	u8 driving_1v8[4][3] = {
		{0xC4, 0xC4, 0xC4},
		{0x3C, 0x3C, 0x3C},
		{0xFE, 0xFE, 0xFE},
		{0xB3, 0xB3, 0xB3},
	};
	u8 (*driving)[3], drive_sel;

	if (voltage == OUTPUT_3V3) {
		driving = driving_3v3;
		drive_sel = pcr->sd30_drive_sel_3v3;
	} else {
		driving = driving_1v8;
		drive_sel = pcr->sd30_drive_sel_1v8;
	}

	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD30_CLK_DRIVE_SEL,
			0xFF, driving[drive_sel][0]);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD30_CMD_DRIVE_SEL,
			0xFF, driving[drive_sel][1]);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD30_DAT_DRIVE_SEL,
			0xFF, driving[drive_sel][2]);
}

static void rtsx_base_fetch_vendor_settings(struct rtsx_pcr *pcr)
{
	u32 reg;

	rtsx_pci_read_config_dword(pcr, PCR_SETTING_REG1, &reg);
	pcr_dbg(pcr, "Cfg 0x%x: 0x%x\n", PCR_SETTING_REG1, reg);

	if (!rtsx_vendor_setting_valid(reg)) {
		pcr_dbg(pcr, "skip fetch vendor setting\n");
		return;
	}

	pcr->aspm_en = rtsx_reg_to_aspm(reg);
	pcr->sd30_drive_sel_1v8 = rtsx_reg_to_sd30_drive_sel_1v8(reg);
	pcr->card_drive_sel &= 0x3F;
	pcr->card_drive_sel |= rtsx_reg_to_card_drive_sel(reg);

	rtsx_pci_read_config_dword(pcr, PCR_SETTING_REG2, &reg);
	pcr_dbg(pcr, "Cfg 0x%x: 0x%x\n", PCR_SETTING_REG2, reg);
	pcr->sd30_drive_sel_3v3 = rtsx_reg_to_sd30_drive_sel_3v3(reg);
	if (rtsx_reg_check_reverse_socket(reg))
		pcr->flags |= PCR_REVERSE_SOCKET;
}

static void rtsx_base_force_power_down(struct rtsx_pcr *pcr, u8 pm_state)
{
	/* Set relink_time to 0 */
	rtsx_pci_write_register(pcr, AUTOLOAD_CFG_BASE + 1, 0xFF, 0);
	rtsx_pci_write_register(pcr, AUTOLOAD_CFG_BASE + 2, 0xFF, 0);
	rtsx_pci_write_register(pcr, AUTOLOAD_CFG_BASE + 3, 0x01, 0);

	if (pm_state == HOST_ENTER_S3)
		rtsx_pci_write_register(pcr, pcr->reg_pm_ctrl3,
			D3_DELINK_MODE_EN, D3_DELINK_MODE_EN);

	rtsx_pci_write_register(pcr, FPDCTL, 0x03, 0x03);
}

static int rts5249_extra_init_hw(struct rtsx_pcr *pcr)
{
	rtsx_pci_init_cmd(pcr);

	/* Rest L1SUB Config */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, L1SUB_CONFIG3, 0xFF, 0x00);
	/* Configure GPIO as output */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, GPIO_CTL, 0x02, 0x02);
	/* Reset ASPM state to default value */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, ASPM_FORCE_CTL, 0x3F, 0);
	/* Switch LDO3318 source from DV33 to card_3v3 */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, LDO_PWR_SEL, 0x03, 0x00);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, LDO_PWR_SEL, 0x03, 0x01);
	/* LED shine disabled, set initial shine cycle period */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, OLT_LED_CTL, 0x0F, 0x02);
	/* Configure driving */
	rts5249_fill_driving(pcr, OUTPUT_3V3);
	if (pcr->flags & PCR_REVERSE_SOCKET)
		rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PETXCFG, 0xB0, 0xB0);
	else
		rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PETXCFG, 0xB0, 0x80);

	return rtsx_pci_send_cmd(pcr, 100);
}

static int rts5249_optimize_phy(struct rtsx_pcr *pcr)
{
	int err;

	err = rtsx_pci_write_register(pcr, PM_CTRL3, D3_DELINK_MODE_EN, 0x00);
	if (err < 0)
		return err;

	err = rtsx_pci_write_phy_register(pcr, PHY_REV,
			PHY_REV_RESV | PHY_REV_RXIDLE_LATCHED |
			PHY_REV_P1_EN | PHY_REV_RXIDLE_EN |
			PHY_REV_CLKREQ_TX_EN | PHY_REV_RX_PWST |
			PHY_REV_CLKREQ_DT_1_0 | PHY_REV_STOP_CLKRD |
			PHY_REV_STOP_CLKWR);
	if (err < 0)
		return err;

	msleep(1);

	err = rtsx_pci_write_phy_register(pcr, PHY_BPCR,
			PHY_BPCR_IBRXSEL | PHY_BPCR_IBTXSEL |
			PHY_BPCR_IB_FILTER | PHY_BPCR_CMIRROR_EN);
	if (err < 0)
		return err;

	err = rtsx_pci_write_phy_register(pcr, PHY_PCR,
			PHY_PCR_FORCE_CODE | PHY_PCR_OOBS_CALI_50 |
			PHY_PCR_OOBS_VCM_08 | PHY_PCR_OOBS_SEN_90 |
			PHY_PCR_RSSI_EN | PHY_PCR_RX10K);
	if (err < 0)
		return err;

	err = rtsx_pci_write_phy_register(pcr, PHY_RCR2,
			PHY_RCR2_EMPHASE_EN | PHY_RCR2_NADJR |
			PHY_RCR2_CDR_SR_2 | PHY_RCR2_FREQSEL_12 |
			PHY_RCR2_CDR_SC_12P | PHY_RCR2_CALIB_LATE);
	if (err < 0)
		return err;

	err = rtsx_pci_write_phy_register(pcr, PHY_FLD4,
			PHY_FLD4_FLDEN_SEL | PHY_FLD4_REQ_REF |
			PHY_FLD4_RXAMP_OFF | PHY_FLD4_REQ_ADDA |
			PHY_FLD4_BER_COUNT | PHY_FLD4_BER_TIMER |
			PHY_FLD4_BER_CHK_EN);
	if (err < 0)
		return err;
	err = rtsx_pci_write_phy_register(pcr, PHY_RDR,
			PHY_RDR_RXDSEL_1_9 | PHY_SSC_AUTO_PWD);
	if (err < 0)
		return err;
	err = rtsx_pci_write_phy_register(pcr, PHY_RCR1,
			PHY_RCR1_ADP_TIME_4 | PHY_RCR1_VCO_COARSE);
	if (err < 0)
		return err;
	err = rtsx_pci_write_phy_register(pcr, PHY_FLD3,
			PHY_FLD3_TIMER_4 | PHY_FLD3_TIMER_6 |
			PHY_FLD3_RXDELINK);
	if (err < 0)
		return err;

	return rtsx_pci_write_phy_register(pcr, PHY_TUNE,
			PHY_TUNE_TUNEREF_1_0 | PHY_TUNE_VBGSEL_1252 |
			PHY_TUNE_SDBUS_33 | PHY_TUNE_TUNED18 |
			PHY_TUNE_TUNED12 | PHY_TUNE_TUNEA12);
}

static int rtsx_base_turn_on_led(struct rtsx_pcr *pcr)
{
	return rtsx_pci_write_register(pcr, GPIO_CTL, 0x02, 0x02);
}

static int rtsx_base_turn_off_led(struct rtsx_pcr *pcr)
{
	return rtsx_pci_write_register(pcr, GPIO_CTL, 0x02, 0x00);
}

static int rtsx_base_enable_auto_blink(struct rtsx_pcr *pcr)
{
	return rtsx_pci_write_register(pcr, OLT_LED_CTL, 0x08, 0x08);
}

static int rtsx_base_disable_auto_blink(struct rtsx_pcr *pcr)
{
	return rtsx_pci_write_register(pcr, OLT_LED_CTL, 0x08, 0x00);
}

static int rtsx_base_card_power_on_vdd1(struct rtsx_pcr *pcr, int card)
{
	int err;

	rtsx_pci_init_cmd(pcr);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CARD_PWR_CTL,
			SD_POWER_MASK, SD_VCC_PARTIAL_POWER_ON);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PWR_GATE_CTRL,
			LDO3318_PWR_MASK, 0x02);
	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	msleep(5);

	rtsx_pci_init_cmd(pcr);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CARD_PWR_CTL,
			SD_POWER_MASK, SD_VCC_POWER_ON);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PWR_GATE_CTRL,
			LDO3318_PWR_MASK, 0x06);
	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	return 0;
}

static int rtsx_base_card_power_on_vdd2(struct rtsx_pcr *pcr, int card)
{
	int err;

	rtsx_pci_init_cmd(pcr);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CARD_PWR_CTL,
		SD_POWER_MASK | SD_VIO_PWR_MASK,
		SD_VCC_PARTIAL_POWER_ON | SD_VIO_PWR14_ON);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PWR_GATE_CTRL,
			LDO3318_PWR_MASK, 0x02);
	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	msleep(5);

	rtsx_pci_init_cmd(pcr);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CARD_PWR_CTL,
		SD_POWER_MASK | SD_VIO_PWR_MASK,
		SD_VCC_POWER_ON | SD_VIO_PWR_ON);
	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	return 0;
}

static int rtsx_base_card_power_on(struct rtsx_pcr *pcr, int card)
{
	if (pcr->use_vdd2)
		return rtsx_base_card_power_on_vdd2(pcr, card);
	else
		return rtsx_base_card_power_on_vdd1(pcr, card);
}

static int rtsx_base_card_power_off(struct rtsx_pcr *pcr, int card)
{
	rtsx_pci_init_cmd(pcr);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CARD_PWR_CTL,
		SD_POWER_MASK | SD_VIO_PWR_MASK | SD40_PAD_ISO,
		SD_POWER_OFF | SD_VIO_PWR_OFF);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PWR_GATE_CTRL,
			LDO3318_PWR_MASK, 0x00);
	return rtsx_pci_send_cmd(pcr, 100);
}

static int rtsx_base_switch_output_voltage(struct rtsx_pcr *pcr, u8 voltage)
{
	int err;
	u16 append;

	switch (voltage) {
	case OUTPUT_3V3:
		err = rtsx_pci_update_phy(pcr, PHY_TUNE, PHY_TUNE_VOLTAGE_MASK,
			PHY_TUNE_VOLTAGE_3V3);
		if (err < 0)
			return err;
		break;
	case OUTPUT_1V8:
		append = PHY_TUNE_D18_1V8;
		if (CHK_PCI_PID(pcr, 0x5249)) {
			err = rtsx_pci_update_phy(pcr, PHY_BACR,
				PHY_BACR_BASIC_MASK, 0);
			if (err < 0)
				return err;
			append = PHY_TUNE_D18_1V7;
		}

		err = rtsx_pci_update_phy(pcr, PHY_TUNE, PHY_TUNE_VOLTAGE_MASK,
			append);
		if (err < 0)
			return err;
		break;
	default:
		pcr_dbg(pcr, "unknown output voltage %d\n", voltage);
		return -EINVAL;
	}

	/* set pad drive */
	rtsx_pci_init_cmd(pcr);
	rts5249_fill_driving(pcr, voltage);
	return rtsx_pci_send_cmd(pcr, 100);
}

static int rts5249_init_sd40_hw_stage1(struct rtsx_pcr *pcr)
{
	int err;
	u16 val = 0;

	rtsx_pci_write_phy_register(pcr, 0x85, 0x2497);
	rtsx_pci_write_phy_register(pcr, 0x90, 0x09C6);
	rtsx_pci_write_phy_register(pcr, 0x82, 0x2508);
	rtsx_pci_write_phy_register(pcr, 0x85, 0x2497);
	if (pcr->ic_version >= IC_VER_D)
		rtsx_pci_write_phy_register(pcr, 0x87, 0x7469);
	else
		rtsx_pci_write_phy_register(pcr, 0x87, 0x7C69);
	rtsx_pci_write_phy_register(pcr, 0x88, 0x4820);
	rtsx_pci_write_phy_register(pcr, 0x92, 0xE21D);
	if (pcr->ic_version >= IC_VER_D) {
		rtsx_pci_write_phy_register(pcr, 0x8A, 0x61CF);
		rtsx_pci_write_phy_register(pcr, 0x9F, 0xFF24);
		rtsx_pci_read_phy_register(pcr, 0x86, &val);
		rtsx_pci_write_phy_register(pcr, 0x86, val | 0x0C);
	} else {
		rtsx_pci_write_phy_register(pcr, 0x8A, 0x61CD);
		rtsx_pci_write_phy_register(pcr, 0x9F, 0xFF20);
	}

	rtsx_pci_init_cmd(pcr);

	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD40_LDO_CTL, 0xFF, 0xF3);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CARD_PWR_CTL,
			SD40_PAD_ISO, SD40_PAD_ISO);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CARD_CLK_EN,
			SD40_CLK_EN | SD_CLK_EN, SD40_CLK_EN);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CARD_SELECT,
			CARD_MOD_MASK, SD40_MOD_SEL);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, EIDL_IN_OUT_CNT, 0xFF, 0xFF);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD40_PHY_CAP + 4, 0xFF, 0x50);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD40_CTRL,
			0xFF, SD40_PHY_RST | SD40_PHY_ISO);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD40_MISC, SCRAM_DIS_EN, 0);

	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	msleep(100);

	err = rtsx_pci_write_register(pcr, CLK_CTL, 0xFF, CHANGE_SD40_PCLK);
	if (err < 0)
		return err;

	err = rtsx_pci_write_register(pcr, CLK_DIV, 0x0F, 0x03);
	if (err < 0)
		return err;

	msleep(10);

	rts5249_sd40_auto_K_apm_offset(pcr);

	if (pcr->ic_version >= IC_VER_D) {
		u16 auto_k;

		rtsx_pci_read_phy_register(pcr, 0x9D, &val);
		rtsx_pci_write_phy_register(pcr, 0x9D, val | (1 << 9));

		rtsx_pci_read_phy_register(pcr, 0x80, &auto_k);

		rtsx_pci_read_phy_register(pcr, 0x88, &val);
		rtsx_pci_write_phy_register(pcr, 0x88,
				(val & 0xFC3F) | (auto_k & 0x1E00));

		rtsx_pci_read_phy_register(pcr, 0x90, &val);
		rtsx_pci_write_phy_register(pcr, 0x90,
				(val & 0xFC3F) | ((auto_k & 0xF0) << 2));

		rtsx_pci_read_phy_register(pcr, 0x85, &val);
		rtsx_pci_write_phy_register(pcr, 0x85, val & ~0x01);

		rtsx_pci_read_phy_register(pcr, 0x9D, &val);
		rtsx_pci_write_phy_register(pcr, 0x9D, val & ~(1 << 9));

		rtsx_pci_read_phy_register(pcr, 0x9E, &val);
		rtsx_pci_write_phy_register(pcr, 0x9E, val & 0xBFFF);
	}

	return 0;
}

static int rts5249_init_sd40_hw_stage2(struct rtsx_pcr *pcr)
{
	u16 phy = 0;

	rtsx_pci_read_phy_register(pcr, 0x9E, &phy);
	rtsx_pci_write_phy_register(pcr, 0x9E, phy | (1 << 13));

	rtsx_pci_read_phy_register(pcr, 0x9F, &phy);
	rtsx_pci_write_phy_register(pcr, 0x9F, (phy & 0xFF0F) | 0x78);

	return 0;
}

static int rts5249_init_sd40_hw(struct rtsx_pcr *pcr, u8 stage)
{
	int err = 0;

	if (stage == 1)
		err = rts5249_init_sd40_hw_stage1(pcr);
	else if (stage == 2)
		err = rts5249_init_sd40_hw_stage2(pcr);

	return err;
}

static int rts5249_enter_dormant(struct rtsx_pcr *pcr)
{
	u8 val;
	int err;

	rtsx_pci_write_register(pcr, SD40_CTRL,
			SD40_GO_DORMANT, SD40_GO_DORMANT);

	msleep(5);

	rtsx_pci_read_register(pcr, SD40_STATUS3, &val);
	if (val & SD40_DET) {
		dev_dbg(&pcr->pci->dev, "enter dormant failed\n");
	} else {
		if (val & DLSM_STATE_MASK) {
			rtsx_pci_init_cmd(pcr);
			rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD40_CTRL,
					SD40_DLSM_RST, SD40_DLSM_RST);
			rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CLK_CTL,
					CHANGE_SD40_PCLK | CLK_LOW_FREQ,
					CLK_LOW_FREQ);
			rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD40_CTRL,
					SD40_PHY_RST, 0);
			rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD40_CTRL,
					SD40_PHY_RST, SD40_PHY_RST);
			rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CLK_CTL,
					0xFF, CHANGE_SD40_PCLK);
			err = rtsx_pci_send_cmd(pcr, 100);
			if (err)
				return err;
		}

		if (pcr->ic_version >= IC_VER_D) {
			rtsx_pci_update_phy(pcr, 0x9F, 0xF0FB, 0);
			rtsx_pci_update_phy(pcr, 0x86, 0xFFF3, 0);
		}
	}

	return 0;
}

static int rts5249_exit_dormant(struct rtsx_pcr *pcr)
{
	u8 val;
	int err;
	u8 *ptr;

	if (pcr->ic_version >= IC_VER_D) {
		rtsx_pci_update_phy(pcr, 0x9F, 0xFFFF, 0x0F04);
		rtsx_pci_update_phy(pcr, 0x86, 0xFFFF, 0x0C);
	}

	rtsx_pci_read_register(pcr, CARD_PWR_CTL, &val);
	if ((val & SD_POWER_MASK) == SD_POWER_OFF) {
		dev_dbg(&(pcr->pci->dev), "set SD40 power on\n");
		rtsx_pci_write_register(pcr, CARD_PWR_CTL, SD_POWER_MASK,
				SD_VCC_PARTIAL_POWER_ON);
		msleep(5);
		rtsx_pci_write_register(pcr, CARD_PWR_CTL, SD_POWER_MASK,
				SD_POWER_ON);
		msleep(1);
	}

	rtsx_pci_init_cmd(pcr);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD40_TMOUT_1,
		SD40_TIME_OUT_ERR_EN, 0x00);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD40_CTRL,
		SD40_GO_DORMANT | SD40_EXIT_DORMANT,
		SD40_EXIT_DORMANT);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD40_TMOUT_1,
		SD40_TIME_OUT_ERR_EN, SD40_TIME_OUT_ERR_EN);
	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	msleep(5);

	rtsx_pci_init_cmd(pcr);
	rtsx_pci_add_cmd(pcr, READ_REG_CMD, SD40_STATUS1, 0, 0);
	rtsx_pci_add_cmd(pcr, READ_REG_CMD, SD40_TMOUT_0, 0, 0);
	rtsx_pci_add_cmd(pcr, READ_REG_CMD, SD40_TMOUT_1, 0, 0);
	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	ptr = rtsx_pci_get_cmd_data(pcr);

	if ((ptr[0] & (SD40_HEAD_ERR | SD40_FRAM_ERR | SD40_CRC_ERR)) ||
			ptr[1] || (ptr[2] & 0x0F)) {
		dev_dbg(&pcr->pci->dev, "exit dormant failed\n");
		dev_dbg(&pcr->pci->dev, "SD40_STATUS1: 0x%02x\n", ptr[0]);
		dev_dbg(&pcr->pci->dev, "SD40_TMOUT_0: 0x%02x\n", ptr[1]);
		dev_dbg(&pcr->pci->dev, "SD40_TMOUT_1: 0x%02x\n", ptr[2]);
		return -EIO;
	}

	return 0;
}

static int rts5249_set_dormant_state(struct rtsx_pcr *pcr, u8 state)
{
	if (state == RTSX_DORMANT_ENTER)
		return rts5249_enter_dormant(pcr);
	else if (state == RTSX_DORMANT_EXIT)
		return rts5249_exit_dormant(pcr);
	else
		dev_dbg(&pcr->pci->dev, "unknown state\n");

	return -EINVAL;
}

static const struct pcr_ops rts5249_pcr_ops = {
	.fetch_vendor_settings = rtsx_base_fetch_vendor_settings,
	.extra_init_hw = rts5249_extra_init_hw,
	.optimize_phy = rts5249_optimize_phy,
	.turn_on_led = rtsx_base_turn_on_led,
	.turn_off_led = rtsx_base_turn_off_led,
	.enable_auto_blink = rtsx_base_enable_auto_blink,
	.disable_auto_blink = rtsx_base_disable_auto_blink,
	.card_power_on = rtsx_base_card_power_on,
	.card_power_off = rtsx_base_card_power_off,
	.switch_output_voltage = rtsx_base_switch_output_voltage,
	.force_power_down = rtsx_base_force_power_down,
	.init_sd40_hw = rts5249_init_sd40_hw,
	.set_dormant_state = rts5249_set_dormant_state,
};

/* SD Pull Control Enable:
 *     SD_DAT[3:0] ==> pull up
 *     SD_CD       ==> pull up
 *     SD_WP       ==> pull up
 *     SD_CMD      ==> pull up
 *     SD_CLK      ==> pull down
 */
static const u32 rts5249_sd_pull_ctl_enable_tbl[] = {
	RTSX_REG_PAIR(CARD_PULL_CTL1, 0x66),
	RTSX_REG_PAIR(CARD_PULL_CTL2, 0xAA),
	RTSX_REG_PAIR(CARD_PULL_CTL3, 0xE9),
	RTSX_REG_PAIR(CARD_PULL_CTL4, 0xAA),
	0,
};

static const u32 rts5249_sd40_pull_ctl_enable_tbl[] = {
	RTSX_REG_PAIR(CARD_PULL_CTL1, 0x66),
	RTSX_REG_PAIR(CARD_PULL_CTL2, 0x10),
	RTSX_REG_PAIR(CARD_PULL_CTL3, 0xE5),
	0,
};

/* SD Pull Control Disable:
 *     SD_DAT[3:0] ==> pull down
 *     SD_CD       ==> pull up
 *     SD_WP       ==> pull down
 *     SD_CMD      ==> pull down
 *     SD_CLK      ==> pull down
 */
static const u32 rts5249_sd_pull_ctl_disable_tbl[] = {
	RTSX_REG_PAIR(CARD_PULL_CTL1, 0x66),
	RTSX_REG_PAIR(CARD_PULL_CTL2, 0x55),
	RTSX_REG_PAIR(CARD_PULL_CTL3, 0xD5),
	RTSX_REG_PAIR(CARD_PULL_CTL4, 0x55),
	0,
};

/* MS Pull Control Enable:
 *     MS CD       ==> pull up
 *     others      ==> pull down
 */
static const u32 rts5249_ms_pull_ctl_enable_tbl[] = {
	RTSX_REG_PAIR(CARD_PULL_CTL4, 0x55),
	RTSX_REG_PAIR(CARD_PULL_CTL5, 0x55),
	RTSX_REG_PAIR(CARD_PULL_CTL6, 0x15),
	0,
};

/* MS Pull Control Disable:
 *     MS CD       ==> pull up
 *     others      ==> pull down
 */
static const u32 rts5249_ms_pull_ctl_disable_tbl[] = {
	RTSX_REG_PAIR(CARD_PULL_CTL4, 0x55),
	RTSX_REG_PAIR(CARD_PULL_CTL5, 0x55),
	RTSX_REG_PAIR(CARD_PULL_CTL6, 0x15),
	0,
};

void rts5249_init_params(struct rtsx_pcr *pcr)
{
	pcr->extra_caps = EXTRA_CAPS_SD_SDR50 | EXTRA_CAPS_SD_SDR104 |
		EXTRA_CAPS_SD_UHSII | EXTRA_CAPS_SD_UHSII_RANGE_B;
	pcr->num_slots = 2;
	pcr->ops = &rts5249_pcr_ops;

	pcr->flags = 0;
	pcr->card_drive_sel = RTSX_CARD_DRIVE_DEFAULT;
	pcr->sd30_drive_sel_1v8 = CFG_DRIVER_TYPE_B;
	pcr->sd30_drive_sel_3v3 = CFG_DRIVER_TYPE_B;
	pcr->aspm_en = ASPM_L1_EN;
	pcr->tx_initial_phase = SET_CLOCK_PHASE(1, 29, 16);
	pcr->rx_initial_phase = SET_CLOCK_PHASE(24, 6, 5);

	pcr->ic_version = rts5249_get_ic_version(pcr);
	pcr->sd_pull_ctl_enable_tbl = rts5249_sd_pull_ctl_enable_tbl;
	pcr->sd_pull_ctl_disable_tbl = rts5249_sd_pull_ctl_disable_tbl;
	pcr->sd40_pull_ctl_enable_tbl = rts5249_sd40_pull_ctl_enable_tbl;
	pcr->sd40_pull_ctl_disable_tbl = rts5249_sd_pull_ctl_disable_tbl;
	pcr->ms_pull_ctl_enable_tbl = rts5249_ms_pull_ctl_enable_tbl;
	pcr->ms_pull_ctl_disable_tbl = rts5249_ms_pull_ctl_disable_tbl;
	pcr->reg_pm_ctrl3 = PM_CTRL3;
}

static int rts524a_write_phy(struct rtsx_pcr *pcr, u8 addr, u16 val)
{
	addr = addr & 0x80 ? (addr & 0x7F) | 0x40 : addr;

	return __rtsx_pci_write_phy_register(pcr, addr, val);
}

static int rts524a_read_phy(struct rtsx_pcr *pcr, u8 addr, u16 *val)
{
	addr = addr & 0x80 ? (addr & 0x7F) | 0x40 : addr;

	return __rtsx_pci_read_phy_register(pcr, addr, val);
}

static int rts524a_optimize_phy(struct rtsx_pcr *pcr)
{
	int err;

	err = rtsx_pci_write_register(pcr, RTS524A_PM_CTRL3,
		D3_DELINK_MODE_EN, 0x00);
	if (err < 0)
		return err;

	rtsx_pci_write_phy_register(pcr, PHY_PCR,
		PHY_PCR_FORCE_CODE | PHY_PCR_OOBS_CALI_50 |
		PHY_PCR_OOBS_VCM_08 | PHY_PCR_OOBS_SEN_90 | PHY_PCR_RSSI_EN);
	rtsx_pci_write_phy_register(pcr, PHY_SSCCR3,
		PHY_SSCCR3_STEP_IN | PHY_SSCCR3_CHECK_DELAY);

	if (is_version(pcr, 0x524A, IC_VER_A)) {
		rtsx_pci_write_phy_register(pcr, PHY_SSCCR3,
			PHY_SSCCR3_STEP_IN | PHY_SSCCR3_CHECK_DELAY);
		rtsx_pci_write_phy_register(pcr, PHY_SSCCR2,
			PHY_SSCCR2_PLL_NCODE | PHY_SSCCR2_TIME0 |
			PHY_SSCCR2_TIME2_WIDTH);
		rtsx_pci_write_phy_register(pcr, PHY_ANA1A,
			PHY_ANA1A_TXR_LOOPBACK | PHY_ANA1A_RXT_BIST |
			PHY_ANA1A_TXR_BIST | PHY_ANA1A_REV);
		rtsx_pci_write_phy_register(pcr, PHY_ANA1D,
			PHY_ANA1D_DEBUG_ADDR);
		rtsx_pci_write_phy_register(pcr, PHY_DIG1E,
			PHY_DIG1E_REV | PHY_DIG1E_D0_X_D1 |
			PHY_DIG1E_RX_ON_HOST | PHY_DIG1E_RCLK_REF_HOST |
			PHY_DIG1E_RCLK_TX_EN_KEEP |
			PHY_DIG1E_RCLK_TX_TERM_KEEP |
			PHY_DIG1E_RCLK_RX_EIDLE_ON | PHY_DIG1E_TX_TERM_KEEP |
			PHY_DIG1E_RX_TERM_KEEP | PHY_DIG1E_TX_EN_KEEP |
			PHY_DIG1E_RX_EN_KEEP);
	}

	rtsx_pci_write_phy_register(pcr, PHY_ANA08,
		PHY_ANA08_RX_EQ_DCGAIN | PHY_ANA08_SEL_RX_EN |
		PHY_ANA08_RX_EQ_VAL | PHY_ANA08_SCP | PHY_ANA08_SEL_IPI);

	return 0;
}

static int rts524a_extra_init_hw(struct rtsx_pcr *pcr)
{
	rts5249_extra_init_hw(pcr);

	rtsx_pci_write_register(pcr, FUNC_FORCE_CTL,
		FORCE_ASPM_L1_EN, FORCE_ASPM_L1_EN);
	rtsx_pci_write_register(pcr, PM_EVENT_DEBUG, PME_DEBUG_0, PME_DEBUG_0);
	rtsx_pci_write_register(pcr, LDO_VCC_CFG1, LDO_VCC_LMT_EN,
		LDO_VCC_LMT_EN);
	rtsx_pci_write_register(pcr, PCLK_CTL, PCLK_MODE_SEL, PCLK_MODE_SEL);
	if (is_version(pcr, 0x524A, IC_VER_A)) {
		rtsx_pci_write_register(pcr, LDO_DV18_CFG,
			LDO_DV18_SR_MASK, LDO_DV18_SR_DF);
		rtsx_pci_write_register(pcr, LDO_VCC_CFG1,
			LDO_VCC_REF_TUNE_MASK, LDO_VCC_REF_1V2);
		rtsx_pci_write_register(pcr, LDO_VIO_CFG,
			LDO_VIO_REF_TUNE_MASK, LDO_VIO_REF_1V2);
		rtsx_pci_write_register(pcr, LDO_VIO_CFG,
			LDO_VIO_SR_MASK, LDO_VIO_SR_DF);
		rtsx_pci_write_register(pcr, LDO_DV12S_CFG,
			LDO_REF12_TUNE_MASK, LDO_REF12_TUNE_DF);
		rtsx_pci_write_register(pcr, SD40_LDO_CTL1,
			SD40_VIO_TUNE_MASK, SD40_VIO_TUNE_1V7);
	}

	return 0;
}

static const struct pcr_ops rts524a_pcr_ops = {
	.write_phy = rts524a_write_phy,
	.read_phy = rts524a_read_phy,
	.fetch_vendor_settings = rtsx_base_fetch_vendor_settings,
	.extra_init_hw = rts524a_extra_init_hw,
	.optimize_phy = rts524a_optimize_phy,
	.turn_on_led = rtsx_base_turn_on_led,
	.turn_off_led = rtsx_base_turn_off_led,
	.enable_auto_blink = rtsx_base_enable_auto_blink,
	.disable_auto_blink = rtsx_base_disable_auto_blink,
	.card_power_on = rtsx_base_card_power_on,
	.card_power_off = rtsx_base_card_power_off,
	.switch_output_voltage = rtsx_base_switch_output_voltage,
	.force_power_down = rtsx_base_force_power_down,
};

void rts524a_init_params(struct rtsx_pcr *pcr)
{
	rts5249_init_params(pcr);

	pcr->reg_pm_ctrl3 = RTS524A_PM_CTRL3;
	pcr->ops = &rts524a_pcr_ops;
}

static int rts525a_card_power_on(struct rtsx_pcr *pcr, int card)
{
	rtsx_pci_write_register(pcr, LDO_VCC_CFG1,
		LDO_VCC_TUNE_MASK, LDO_VCC_3V3);
	return rtsx_base_card_power_on(pcr, card);
}

static int rts525a_switch_output_voltage(struct rtsx_pcr *pcr, u8 voltage)
{
	switch (voltage) {
	case OUTPUT_3V3:
		rtsx_pci_write_register(pcr, LDO_CONFIG2,
			LDO_D3318_MASK, LDO_D3318_33V);
		rtsx_pci_write_register(pcr, SD_PAD_CTL, SD_IO_USING_1V8, 0);
		break;
	case OUTPUT_1V8:
		rtsx_pci_write_register(pcr, LDO_CONFIG2,
			LDO_D3318_MASK, LDO_D3318_18V);
		rtsx_pci_write_register(pcr, SD_PAD_CTL, SD_IO_USING_1V8,
			SD_IO_USING_1V8);
		break;
	default:
		return -EINVAL;
	}

	rtsx_pci_init_cmd(pcr);
	rts5249_fill_driving(pcr, voltage);
	return rtsx_pci_send_cmd(pcr, 100);
}

static int rts525a_optimize_phy(struct rtsx_pcr *pcr)
{
	int err;

	err = rtsx_pci_write_register(pcr, RTS524A_PM_CTRL3,
		D3_DELINK_MODE_EN, 0x00);
	if (err < 0)
		return err;

	rtsx_pci_write_phy_register(pcr, _PHY_FLD0,
		_PHY_FLD0_CLK_REQ_20C | _PHY_FLD0_RX_IDLE_EN |
		_PHY_FLD0_BIT_ERR_RSTN | _PHY_FLD0_BER_COUNT |
		_PHY_FLD0_BER_TIMER | _PHY_FLD0_CHECK_EN);

	rtsx_pci_write_phy_register(pcr, _PHY_ANA03,
		_PHY_ANA03_TIMER_MAX | _PHY_ANA03_OOBS_DEB_EN |
		_PHY_CMU_DEBUG_EN);

	if (is_version(pcr, 0x525A, IC_VER_A))
		rtsx_pci_write_phy_register(pcr, _PHY_REV0,
			_PHY_REV0_FILTER_OUT | _PHY_REV0_CDR_BYPASS_PFD |
			_PHY_REV0_CDR_RX_IDLE_BYPASS);

	return 0;
}

static int rts525a_extra_init_hw(struct rtsx_pcr *pcr)
{
	rts5249_extra_init_hw(pcr);

	rtsx_pci_write_register(pcr, PCLK_CTL, PCLK_MODE_SEL, PCLK_MODE_SEL);
	if (is_version(pcr, 0x525A, IC_VER_A)) {
		rtsx_pci_write_register(pcr, L1SUB_CONFIG2,
			L1SUB_AUTO_CFG, L1SUB_AUTO_CFG);
		rtsx_pci_write_register(pcr, RREF_CFG,
			RREF_VBGSEL_MASK, RREF_VBGSEL_1V25);
		rtsx_pci_write_register(pcr, LDO_VIO_CFG,
			LDO_VIO_TUNE_MASK, LDO_VIO_1V7);
		rtsx_pci_write_register(pcr, LDO_DV12S_CFG,
			LDO_D12_TUNE_MASK, LDO_D12_TUNE_DF);
		rtsx_pci_write_register(pcr, LDO_AV12S_CFG,
			LDO_AV12S_TUNE_MASK, LDO_AV12S_TUNE_DF);
		rtsx_pci_write_register(pcr, LDO_VCC_CFG0,
			LDO_VCC_LMTVTH_MASK, LDO_VCC_LMTVTH_2A);
		rtsx_pci_write_register(pcr, OOBS_CONFIG,
			OOBS_AUTOK_DIS | OOBS_VAL_MASK, 0x89);
	}

	return 0;
}

static const struct pcr_ops rts525a_pcr_ops = {
	.fetch_vendor_settings = rtsx_base_fetch_vendor_settings,
	.extra_init_hw = rts525a_extra_init_hw,
	.optimize_phy = rts525a_optimize_phy,
	.turn_on_led = rtsx_base_turn_on_led,
	.turn_off_led = rtsx_base_turn_off_led,
	.enable_auto_blink = rtsx_base_enable_auto_blink,
	.disable_auto_blink = rtsx_base_disable_auto_blink,
	.card_power_on = rts525a_card_power_on,
	.card_power_off = rtsx_base_card_power_off,
	.switch_output_voltage = rts525a_switch_output_voltage,
	.force_power_down = rtsx_base_force_power_down,
};

void rts525a_init_params(struct rtsx_pcr *pcr)
{
	rts5249_init_params(pcr);

	pcr->reg_pm_ctrl3 = RTS524A_PM_CTRL3;
	pcr->ops = &rts525a_pcr_ops;
}

