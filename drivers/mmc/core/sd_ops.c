/*
 *  linux/drivers/mmc/core/sd_ops.h
 *
 *  Copyright 2006-2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/slab.h>
#include <linux/types.h>
#include <linux/export.h>
#include <linux/scatterlist.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>

#include "core.h"
#include "sd_ops.h"

int mmc_app_cmd(struct mmc_host *host, struct mmc_card *card,
	struct mmc_command *next_cmd)
{
	int err;
	struct mmc_command cmd = {0};

	BUG_ON(!host);
	BUG_ON(card && (card->host != host));

	if (card && mmc_card_uhsii(card)) {
		next_cmd->app_cmd = true;
		return 0;
	}

	cmd.opcode = MMC_APP_CMD;

	if (card) {
		cmd.arg = card->rca << 16;
		cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;
	} else {
		cmd.arg = 0;
		cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_BCR;
	}

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err)
		return err;

	/* Check that card supported application commands */
	if (!mmc_host_is_spi(host) && !(cmd.resp[0] & R1_APP_CMD))
		return -EOPNOTSUPP;

	return 0;
}
EXPORT_SYMBOL_GPL(mmc_app_cmd);

/**
 *	mmc_wait_for_app_cmd - start an application command and wait for
 			       completion
 *	@host: MMC host to start command
 *	@card: Card to send MMC_APP_CMD to
 *	@cmd: MMC command to start
 *	@retries: maximum number of retries
 *
 *	Sends a MMC_APP_CMD, checks the card response, sends the command
 *	in the parameter and waits for it to complete. Return any error
 *	that occurred while the command was executing.  Do not attempt to
 *	parse the response.
 */
int mmc_wait_for_app_cmd(struct mmc_host *host, struct mmc_card *card,
	struct mmc_command *cmd, int retries)
{
	struct mmc_request mrq = {NULL};

	int i, err;

	BUG_ON(!cmd);
	BUG_ON(retries < 0);

	err = -EIO;

	/*
	 * We have to resend MMC_APP_CMD for each attempt so
	 * we cannot use the retries field in mmc_command.
	 */
	for (i = 0;i <= retries;i++) {
		err = mmc_app_cmd(host, card, cmd);
		if (err) {
			/* no point in retrying; no APP commands allowed */
			if (mmc_host_is_spi(host)) {
				if (cmd->resp[0] & R1_SPI_ILLEGAL_COMMAND)
					break;
			}
			continue;
		}

		memset(&mrq, 0, sizeof(struct mmc_request));

		memset(cmd->resp, 0, sizeof(cmd->resp));
		cmd->retries = 0;

		mrq.cmd = cmd;
		cmd->data = NULL;

		mmc_wait_for_req(host, &mrq);

		err = cmd->error;
		if (!cmd->error)
			break;

		/* no point in retrying illegal APP commands */
		if (mmc_host_is_spi(host)) {
			if (cmd->resp[0] & R1_SPI_ILLEGAL_COMMAND)
				break;
		}
	}

	return err;
}

EXPORT_SYMBOL(mmc_wait_for_app_cmd);

int mmc_app_set_bus_width(struct mmc_card *card, int width)
{
	int err;
	struct mmc_command cmd = {0};

	BUG_ON(!card);
	BUG_ON(!card->host);

	cmd.opcode = SD_APP_SET_BUS_WIDTH;
	cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

	switch (width) {
	case MMC_BUS_WIDTH_1:
		cmd.arg = SD_BUS_WIDTH_1;
		break;
	case MMC_BUS_WIDTH_4:
		cmd.arg = SD_BUS_WIDTH_4;
		break;
	default:
		return -EINVAL;
	}

	err = mmc_wait_for_app_cmd(card->host, card, &cmd, MMC_CMD_RETRIES);
	if (err)
		return err;

	return 0;
}

int mmc_send_app_op_cond(struct mmc_host *host, u32 ocr, u32 *rocr)
{
	struct mmc_command cmd = {0};
	int i, err = 0;

	BUG_ON(!host);

	cmd.opcode = SD_APP_OP_COND;
	if (mmc_host_is_spi(host))
		cmd.arg = ocr & (1 << 30); /* SPI only defines one bit */
	else
		cmd.arg = ocr;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R3 | MMC_CMD_BCR;

	for (i = 100; i; i--) {
		err = mmc_wait_for_app_cmd(host, host->card,
			&cmd, MMC_CMD_RETRIES);
		if (err)
			break;

		/* if we're just probing, do a single pass */
		if (ocr == 0)
			break;

		/* otherwise wait until reset completes */
		if (mmc_host_is_spi(host)) {
			if (!(cmd.resp[0] & R1_SPI_IDLE))
				break;
		} else {
			if (cmd.resp[0] & MMC_CARD_BUSY)
				break;
		}

		err = -ETIMEDOUT;

		mmc_delay(10);
	}

	if (!i)
		pr_err("%s: card never left busy state\n", mmc_hostname(host));

	if (rocr && !mmc_host_is_spi(host))
		*rocr = cmd.resp[0];

	return err;
}

int mmc_send_if_cond(struct mmc_host *host, u32 ocr)
{
	struct mmc_command cmd = {0};
	int err;
	static const u8 test_pattern = 0xAA;
	u8 result_pattern;

	/*
	 * To support SD 2.0 cards, we must always invoke SD_SEND_IF_COND
	 * before SD_APP_OP_COND. This command will harmlessly fail for
	 * SD 1.0 cards.
	 */
	cmd.opcode = SD_SEND_IF_COND;
	cmd.arg = ((ocr & 0xFF8000) != 0) << 8 | test_pattern;
	cmd.flags = MMC_RSP_SPI_R7 | MMC_RSP_R7 | MMC_CMD_BCR;

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err)
		return err;

	if (mmc_host_is_spi(host))
		result_pattern = cmd.resp[1] & 0xFF;
	else
		result_pattern = cmd.resp[0] & 0xFF;

	if (result_pattern != test_pattern)
		return -EIO;

	return 0;
}

int mmc_send_relative_addr(struct mmc_host *host, unsigned int *rca)
{
	int err;
	struct mmc_command cmd = {0};

	BUG_ON(!host);
	BUG_ON(!rca);

	cmd.opcode = SD_SEND_RELATIVE_ADDR;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_R6 | MMC_CMD_BCR;

	err = mmc_wait_for_cmd(host, &cmd, MMC_CMD_RETRIES);
	if (err)
		return err;

	*rca = cmd.resp[0] >> 16;

	return 0;
}

int mmc_app_send_scr(struct mmc_card *card, u32 *scr)
{
	int err;
	struct mmc_request mrq = {NULL};
	struct mmc_command cmd = {0};
	struct mmc_data data = {0};
	struct scatterlist sg;
	void *data_buf;

	BUG_ON(!card);
	BUG_ON(!card->host);
	BUG_ON(!scr);

	/* NOTE: caller guarantees scr is heap-allocated */

	err = mmc_app_cmd(card->host, card, &cmd);
	if (err)
		return err;

	/* dma onto stack is unsafe/nonportable, but callers to this
	 * routine normally provide temporary on-stack buffers ...
	 */
	data_buf = kmalloc(sizeof(card->raw_scr), GFP_KERNEL);
	if (data_buf == NULL)
		return -ENOMEM;

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = SD_APP_SEND_SCR;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;

	data.blksz = 8;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	sg_init_one(&sg, data_buf, 8);

	mmc_set_data_timeout(&data, card);

	mmc_wait_for_req(card->host, &mrq);

	memcpy(scr, data_buf, sizeof(card->raw_scr));
	kfree(data_buf);

	if (cmd.error)
		return cmd.error;
	if (data.error)
		return data.error;

	scr[0] = be32_to_cpu(scr[0]);
	scr[1] = be32_to_cpu(scr[1]);

	return 0;
}

int mmc_sd_switch(struct mmc_card *card, int mode, int group,
	u8 value, u8 *resp)
{
	struct mmc_request mrq = {NULL};
	struct mmc_command cmd = {0};
	struct mmc_data data = {0};
	struct scatterlist sg;

	BUG_ON(!card);
	BUG_ON(!card->host);

	/* NOTE: caller guarantees resp is heap-allocated */

	mode = !!mode;
	value &= 0xF;

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = SD_SWITCH;
	cmd.arg = mode << 31 | 0x00FFFFFF;
	cmd.arg &= ~(0xF << (group * 4));
	cmd.arg |= value << (group * 4);
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;

	data.blksz = 64;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	sg_init_one(&sg, resp, 64);

	mmc_set_data_timeout(&data, card);

	mmc_wait_for_req(card->host, &mrq);

	if (cmd.error)
		return cmd.error;
	if (data.error)
		return data.error;

	return 0;
}

int mmc_app_sd_status(struct mmc_card *card, void *ssr)
{
	int err;
	struct mmc_request mrq = {NULL};
	struct mmc_command cmd = {0};
	struct mmc_data data = {0};
	struct scatterlist sg;

	BUG_ON(!card);
	BUG_ON(!card->host);
	BUG_ON(!ssr);

	/* NOTE: caller guarantees ssr is heap-allocated */

	err = mmc_app_cmd(card->host, card, &cmd);
	if (err)
		return err;

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = SD_APP_SD_STATUS;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1 | MMC_CMD_ADTC;

	data.blksz = 64;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	sg_init_one(&sg, ssr, 64);

	mmc_set_data_timeout(&data, card);

	mmc_wait_for_req(card->host, &mrq);

	if (cmd.error)
		return cmd.error;
	if (data.error)
		return data.error;

	return 0;
}

int mmc_sd_send_device_init_ccmd(struct mmc_card *card)
{
	struct mmc_host *host = card->host;
	struct mmc_request mrq = {NULL};
	struct mmc_tlp tlp = {NULL};
	struct mmc_tlp_block cmd = {0}, resp = {0};
	int i;
	u8 gd = 0, gap;
	u16 ioadr = UHSII_IOADR(SD40_IOADR_CMD_BASE, SD40_DEVICE_INIT);
	u32 payload;

	payload = SD40_CF | SD40_GAP(host->max_gap) | SD40_DAP(host->max_dap);

	mrq.tlp = &tlp;
	tlp.tlp_send = &cmd;
	tlp.tlp_back = &resp;
	tlp.retries = 0;

	tlp.tlp_send->header = UHSII_HD_NP | UHSII_HD_TYP_CCMD;
	tlp.tlp_send->argument = UHSII_ARG_DIR_WRITE |
		(1 << UHSII_ARG_PLEN_SHIFT) | (ioadr & UHSII_ARG_IOADR_MASK);

	for (i = 0; i < 30; i++) {
		tlp.tlp_send->payload[0] = payload;

		mmc_wait_for_req(host, &mrq);

		if (tlp.error)
			return tlp.error;

		if (tlp.tlp_back->payload[0] & SD40_CF)
			return 0;

		gap = UHSII_GET_GAP(tlp.tlp_back);
		if (gap == host->max_gap) {
			gd++;
			payload |= (gd & SD40_GD_MASK) << SD40_GD_SHIFT;
		}
	}

	return -ETIMEDOUT;
}

int mmc_sd_send_enumerate_ccmd(struct mmc_card *card)
{
	struct mmc_host *host = card->host;
	struct mmc_request mrq = {NULL};
	struct mmc_tlp tlp = {NULL};
	struct mmc_tlp_block cmd = {0}, resp = {0};
	u16 ioadr = UHSII_IOADR(SD40_IOADR_CMD_BASE, SD40_ENUMERATE);

	mrq.tlp = &tlp;
	tlp.tlp_send = &cmd;
	tlp.tlp_back = &resp;
	tlp.retries = 0;

	tlp.tlp_send->header = UHSII_HD_NP | UHSII_HD_TYP_CCMD;
	tlp.tlp_send->argument = UHSII_ARG_DIR_WRITE |
		(1 << UHSII_ARG_PLEN_SHIFT) | (ioadr & UHSII_ARG_IOADR_MASK);
	tlp.tlp_send->payload[0] = 0;

	mmc_wait_for_req(host, &mrq);

	if (tlp.error)
		return tlp.error;

	card->node_id = (tlp.tlp_back->payload[0] & SD40_IDL_MASK)
		>> SD40_IDL_SHIFT;

	return 0;
}

int mmc_sd_send_go_dormant_state_ccmd(struct mmc_card *card, int hibernate)
{
	struct mmc_host *host = card->host;
	struct mmc_request mrq = {NULL};
	struct mmc_tlp tlp = {NULL};
	struct mmc_tlp_block cmd = {0}, resp = {0};
	u16 ioadr = UHSII_IOADR(SD40_IOADR_CMD_BASE, SD40_GO_DORMANT_STATE);

	mrq.tlp = &tlp;
	tlp.tlp_send = &cmd;
	tlp.tlp_back = &resp;
	tlp.retries = 0;
	tlp.cmd_type = UHSII_COMMAND_GO_DORMANT;

	tlp.tlp_send->header = UHSII_HD_NP | UHSII_HD_TYP_CCMD |
		UHSII_HD_DID(card->node_id);
	tlp.tlp_send->argument = UHSII_ARG_DIR_WRITE |
		(1 << UHSII_ARG_PLEN_SHIFT) | (ioadr & UHSII_ARG_IOADR_MASK);
	if (hibernate)
		tlp.tlp_send->payload[0] = 0x80000000;
	else
		tlp.tlp_send->payload[0] = 0;

	mmc_wait_for_req(host, &mrq);

	if (tlp.error)
		return tlp.error;

	return 0;
}

int mmc_sd_read_cfg_ccmd(struct mmc_card *card, u8 offset, u8 plen, u32 *buf)
{
	struct mmc_host *host = card->host;
	struct mmc_request mrq = {NULL};
	struct mmc_tlp tlp = {NULL};
	struct mmc_tlp_block cmd = {0}, resp = {0};
	u16 ioadr = UHSII_IOADR(SD40_IOADR_CFG_BASE, offset);
	int i;

	mrq.tlp = &tlp;
	tlp.tlp_send = &cmd;
	tlp.tlp_back = &resp;
	tlp.retries = 0;

	tlp.tlp_send->header = UHSII_HD_NP | UHSII_HD_TYP_CCMD |
		UHSII_HD_DID(card->node_id);
	tlp.tlp_send->argument = UHSII_ARG_DIR_READ |
		(plen << UHSII_ARG_PLEN_SHIFT) |
		(ioadr & UHSII_ARG_IOADR_MASK);

	mmc_wait_for_req(host, &mrq);

	if (tlp.error)
		return tlp.error;

	for (i = 0; i < plen; i++)
		buf[i] = tlp.tlp_back->payload[i];

	return 0;
}

int mmc_sd_write_cfg_ccmd(struct mmc_card *card, u8 offset, u8 plen, u32 *buf)
{
	struct mmc_host *host = card->host;
	struct mmc_request mrq = {NULL};
	struct mmc_tlp tlp = {NULL};
	struct mmc_tlp_block cmd = {0}, resp = {0};
	u16 ioadr = UHSII_IOADR(SD40_IOADR_CFG_BASE, offset);
	int i;

	mrq.tlp = &tlp;
	tlp.tlp_send = &cmd;
	tlp.tlp_back = &resp;
	tlp.retries = 0;

	tlp.tlp_send->header = UHSII_HD_NP | UHSII_HD_TYP_CCMD |
		UHSII_HD_DID(card->node_id);
	tlp.tlp_send->argument = UHSII_ARG_DIR_WRITE |
		(plen << UHSII_ARG_PLEN_SHIFT) |
		(ioadr & UHSII_ARG_IOADR_MASK);
	for (i = 0; i < plen; i++)
		tlp.tlp_send->payload[i] = buf[i];

	mmc_wait_for_req(host, &mrq);

	if (tlp.error)
		return tlp.error;

	return 0;
}

void mmc_sd_tran_pack_ccmd(struct mmc_card *card, struct mmc_command *cmd)
{
	struct mmc_tlp_block *tlp_send = &cmd->tlp_send;

	tlp_send->header = UHSII_HD_TYP_CCMD | UHSII_HD_DID(card->node_id);

	tlp_send->argument = cmd->opcode & UHSII_ARG_CMD_INDEX_MASK;
	if (cmd->app_cmd)
		tlp_send->argument |= UHSII_ARG_APP_CMD;

	tlp_send->payload[0] = cmd->arg;

	pr_debug("%s: SDTRAN CCMD header = 0x%04x, arg = 0x%04x\n",
		mmc_hostname(card->host), tlp_send->header, tlp_send->argument);
}

void mmc_sd_tran_pack_dcmd(struct mmc_card *card, struct mmc_command *cmd)
{
	u8 tmode = 0;
	u32 tlen = 0;
	struct mmc_tlp_block *tlp_send = &cmd->tlp_send;

	tlp_send->header = UHSII_HD_TYP_DCMD | UHSII_HD_DID(card->node_id);

	tlp_send->argument = cmd->opcode & UHSII_ARG_CMD_INDEX_MASK;
	if (cmd->app_cmd)
		tlp_send->argument |= UHSII_ARG_APP_CMD;
	if (cmd->data && (cmd->data->flags & MMC_DATA_WRITE))
		tlp_send->argument |= UHSII_ARG_DIR_WRITE;
	if (mmc_op_multi(cmd->opcode)) {
		tmode |= UHSII_TMODE_LM_SPECIFIED;
		if (card->lane_mode & SD40_LANE_MODE_2L_HD)
			tmode |= UHSII_TMODE_DM_HD;
		if (cmd->data)
			tlen = cmd->data->blocks;
	}
	tlp_send->argument |= tmode << UHSII_ARG_TMODE_SHIFT;

	tlp_send->payload[0] = cmd->arg;
	tlp_send->payload[1] = tlen;

	pr_debug("%s: SDTRAN DCMD header = 0x%04x, arg = 0x%04x, TLEN = %d\n",
		mmc_hostname(card->host), tlp_send->header, tlp_send->argument,
		tlen);
}
