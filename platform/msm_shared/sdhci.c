/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <platform/iomap.h>
#include <platform/irqs.h>
#include <platform/interrupts.h>
#include <platform/timer.h>
#include <kernel/event.h>
#include <target.h>
#include <string.h>
#include <stdlib.h>
#include <bits.h>
#include <debug.h>
#include <sdhci.h>


/*
 * Function: sdhci int handler
 * Arg     : Event argument
 * Return  : 0
 * Flow:   : 1. Read the power control mask register
 *           2. Check if bus is ON
 *           3. Write success to ack regiser
 * Details : This is power control interrupt handler.
 *           Once we receive the interrupt, we will ack the power control
 *           register that we have successfully completed pmic transactions
 */
enum handler_return sdhci_int_handler(void *arg)
{
	uint32_t ack;
	uint32_t status;

	/*
	 * Read the mask register to check if BUS & IO level
	 * interrupts are enabled
	 */
	status = readl(SDCC_HC_PWRCTL_MASK_REG);

	if (status & (SDCC_HC_BUS_ON | SDCC_HC_BUS_OFF))
		ack = SDCC_HC_BUS_ON_OFF_SUCC;
	if (status & (SDCC_HC_IO_SIG_LOW | SDCC_HC_IO_SIG_HIGH))
		ack |= SDCC_HC_IO_SIG_SUCC;

	/* Write success to power control register */
	writel(ack, SDCC_HC_PWRCTL_CTL_REG);

	event_signal((event_t *)arg, false);

	return 0;
}

/*
 * Function: sdhci error status enable
 * Arg     : Host structure
 * Return  : None
 * Flow:   : Enable command error status
 */
static void sdhci_error_status_enable(struct sdhci_host *host)
{
	/* Enable all interrupt status */
	REG_WRITE16(host, SDHCI_NRML_INT_STS_EN, SDHCI_NRML_INT_STS_EN_REG);
	REG_WRITE16(host, SDHCI_ERR_INT_STS_EN, SDHCI_ERR_INT_STS_EN_REG);
	/* Enable all interrupt signal */
	REG_WRITE16(host, SDHCI_NRML_INT_SIG_EN, SDHCI_NRML_INT_SIG_EN_REG);
	REG_WRITE16(host, SDHCI_ERR_INT_SIG_EN, SDHCI_ERR_INT_SIG_EN_REG);
}

/*
 * Function: sdhci clock supply
 * Arg     : Host structure
 * Return  : 0 on Success, 1 on Failure
 * Flow:   : 1. Calculate the clock divider
 *           2. Set the clock divider
 *           3. Check if clock stable
 *           4. Enable Clock
 */
uint32_t sdhci_clk_supply(struct sdhci_host *host, uint32_t clk)
{
	uint32_t div = 0;
	uint32_t freq = 0;
	uint16_t clk_val = 0;

	if (clk > host->caps.base_clk_rate) {
		dprintf(CRITICAL, "Error: Requested clk freq is more than supported\n");
		return 1;
	}

	if (clk == host->caps.base_clk_rate)
		goto clk_ctrl;

	/* As per the sd spec div should be a multiplier of 2 */
	for (div = 2; div < SDHCI_CLK_MAX_DIV; div += 2) {
		freq = host->caps.base_clk_rate / div;
		if (freq <= clk)
			break;
	}

	div >>= 1;

clk_ctrl:
	/* As per the sdhci spec 3.0, bits 6-7 of the clock
	 * control registers will be mapped to bit 8-9, to
	 * support a 10 bit divider value.
	 * This is needed when the divider value overflows
	 * the 8 bit range.
	 */
	clk_val = ((div & SDHCI_SDCLK_FREQ_MASK) << SDHCI_SDCLK_FREQ_SEL);
	clk_val |= ((div & SDHC_SDCLK_UP_BIT_MASK) >> SDHCI_SDCLK_FREQ_SEL)
				<< SDHCI_SDCLK_UP_BIT_SEL;

	clk_val |= SDHCI_INT_CLK_EN;
	REG_WRITE16(host, clk_val, SDHCI_CLK_CTRL_REG);

	/* Check for clock stable */
	while (!(REG_READ16(host, SDHCI_CLK_CTRL_REG) & SDHCI_CLK_STABLE));

	/* Now clock is stable, enable it */
	clk_val = REG_READ16(host, SDHCI_CLK_CTRL_REG);
	clk_val |= SDHCI_CLK_EN;
	REG_WRITE16(host, clk_val, SDHCI_CLK_CTRL_REG);

	host->cur_clk_rate = freq;

	return 0;
}

/*
 * Function: sdhci stop sdcc clock
 * Arg     : Host structure
 * Return  : 0 on Success, 1 on Failure
 * Flow:   : 1. Stop the clock
 */
static uint32_t sdhci_stop_sdcc_clk(struct sdhci_host *host)
{
	uint32_t reg;

	reg = REG_READ32(host, SDHCI_PRESENT_STATE_REG);

	if (reg & (SDHCI_CMD_ACT | SDHCI_DAT_ACT)) {
		dprintf(CRITICAL, "Error: SDCC command & data line are active\n");
		return 1;
	}

	REG_WRITE16(host, SDHCI_CLK_DIS, SDHCI_CLK_CTRL_REG);

	return 0;
}

/*
 * Function: sdhci change frequency
 * Arg     : Host structure & clock value
 * Return  : 0 on Success, 1 on Failure
 * Flow:   : 1. Stop the clock
 *           2. Star the clock with new frequency
 */
static uint32_t sdhci_change_freq_clk(struct sdhci_host *host, uint32_t clk)
{
	if (sdhci_stop_sdcc_clk(host)) {
		dprintf(CRITICAL, "Error: Card is busy, cannot change frequency\n");
		return 1;
	}

	if (sdhci_clk_supply(host, clk)) {
		dprintf(CRITICAL, "Error: cannot change frequency\n");
		return 1;
	}

	return 0;
}

/*
 * Function: sdhci set bus power
 * Arg     : Host structure
 * Return  : None
 * Flow:   : 1. Set the voltage
 *           2. Set the sd power control register
 */
static void sdhci_set_bus_power_on(struct sdhci_host *host)
{
	uint8_t voltage;

	voltage = host->caps.voltage;

	voltage <<= SDHCI_BUS_VOL_SEL;
	REG_WRITE8(host, voltage, SDHCI_BUS_PWR_EN);

	voltage |= SDHCI_BUS_PWR_EN;

	REG_WRITE8(host, voltage, SDHCI_PWR_CTRL_REG);

}

/*
 * Function: sdhci set SDR mode
 * Arg     : Host structure
 * Return  : None
 * Flow:   : 1. Disable the clock
 *           2. Enable sdr mode
 *           3. Enable the clock
 * Details : SDR50/SDR104 mode is nothing but HS200
 *			 mode SDCC spec refers to it as SDR mode
 *			 & emmc spec refers as HS200 mode.
 */
void sdhci_set_sdr_mode(struct sdhci_host *host)
{
	uint16_t clk;
	uint16_t ctrl = 0;

	/* Disable the clock */
	clk = REG_READ16(host, SDHCI_CLK_CTRL_REG);
	clk &= ~SDHCI_CLK_EN;
	REG_WRITE16(host, clk, SDHCI_CLK_CTRL_REG);

	/* Enable SDR50 mode:
	 * Right now we support only SDR50 mode which runs at
	 * 100 MHZ sdcc clock, we dont need tuning with SDR50
	 * mode
	 */
	ctrl = REG_READ16(host, SDHCI_HOST_CTRL2_REG);

	/* Enable SDR50/SDR104 mode based on the controller
	 * capabilities.
	 */
	if (host->caps.sdr50_support)
		ctrl |= SDHCI_SDR50_MODE_EN;

	REG_WRITE16(host, ctrl, SDHCI_HOST_CTRL2_REG);

	/* Run the clock back */
	sdhci_clk_supply(host, SDHCI_CLK_100MHZ);
}

/*
 * Function: sdhci set ddr mode
 * Arg     : Host structure
 * Return  : None
 * Flow:   : 1. Disable the clock
 *           2. Enable DDR mode
 *           3. Enable the clock
 */
void sdhci_set_ddr_mode(struct sdhci_host *host)
{
	uint16_t clk;
	uint16_t ctrl = 0;

	/* Disable the clock */
	clk = REG_READ16(host, SDHCI_CLK_CTRL_REG);
	clk &= ~SDHCI_CLK_EN;
	REG_WRITE16(host, clk, SDHCI_CLK_CTRL_REG);

	ctrl = REG_READ16(host, SDHCI_HOST_CTRL2_REG);
	ctrl |= SDHCI_DDR_MODE_EN;

	/* Enalbe DDR mode */
	REG_WRITE16(host, ctrl, SDHCI_HOST_CTRL2_REG);

	/* Run the clock back */
	sdhci_clk_supply(host, host->cur_clk_rate);
}

/*
 * Function: sdhci set adma mode
 * Arg     : Host structure
 * Return  : None
 * Flow:   : Set adma mode
 */
static void sdhci_set_adma_mode(struct sdhci_host *host)
{
	/* Select 32 Bit ADMA2 type */
	REG_WRITE8(host, SDHCI_ADMA_32BIT, SDHCI_HOST_CTRL1_REG);
}

/*
 * Function: sdhci set bus width
 * Arg     : Host & width
 * Return  : 0 on Sucess, 1 on Failure
 * Flow:   : Set the bus width for controller
 */
uint8_t sdhci_set_bus_width(struct sdhci_host *host, uint16_t width)
{
	uint16_t reg = 0;

	reg = REG_READ8(host, SDHCI_HOST_CTRL1_REG);

	switch(width) {
		case DATA_BUS_WIDTH_8BIT:
			width = SDHCI_BUS_WITDH_8BIT;
			break;
		case DATA_BUS_WIDTH_4BIT:
			width = SDHCI_BUS_WITDH_4BIT;
			break;
		case DATA_BUS_WIDTH_1BIT:
			width = SDHCI_BUS_WITDH_1BIT;
			break;
		default:
			dprintf(CRITICAL, "Bus width is invalid: %u\n", width);
			return 1;
	}

	REG_WRITE8(host, (reg | width), SDHCI_HOST_CTRL1_REG);

	return 0;
}

/*
 * Function: sdhci command err status
 * Arg     : Host structure
 * Return  : 0 on Sucess, 1 on Failure
 * Flow:   : Look for error status
 */
static uint8_t sdhci_cmd_err_status(struct sdhci_host *host)
{
	uint32_t err;

	err = REG_READ16(host, SDHCI_ERR_INT_STS_REG);

	if (err & SDHCI_CMD_TIMEOUT_MASK) {
		dprintf(CRITICAL, "Error: Command timeout error\n");
		return 1;
	} else if (err & SDHCI_CMD_CRC_MASK) {
		dprintf(CRITICAL, "Error: Command CRC error\n");
		return 1;
	} else if (err & SDHCI_CMD_END_BIT_MASK) {
		dprintf(CRITICAL, "Error: CMD end bit error\n");
		return 1;
	} else if (err & SDHCI_CMD_IDX_MASK) {
		dprintf(CRITICAL, "Error: Command Index error\n");
		return 1;
	} else if (err & SDHCI_DAT_TIMEOUT_MASK) {
		dprintf(CRITICAL, "Error: DATA time out error\n");
		return 1;
	} else if (err & SDHCI_DAT_CRC_MASK) {
		dprintf(CRITICAL, "Error: DATA CRC error\n");
		return 1;
	} else if (err & SDHCI_DAT_END_BIT_MASK) {
		dprintf(CRITICAL, "Error: DATA end bit error\n");
		return 1;
	} else if (err & SDHCI_CUR_LIM_MASK) {
		dprintf(CRITICAL, "Error: Current limit error\n");
		return 1;
	} else if (err & SDHCI_AUTO_CMD12_MASK) {
		dprintf(CRITICAL, "Error: Auto CMD12 error\n");
		return 1;
	} else if (err & SDHCI_ADMA_MASK) {
		dprintf(CRITICAL, "Error: ADMA error\n");
		return 1;
	}

	return 0;
}

/*
 * Function: sdhci command complete
 * Arg     : Host & command structure
 * Return  : 0 on Sucess, 1 on Failure
 * Flow:   : 1. Check for command complete
 *           2. Check for transfer complete
 *           3. Get the command response
 *           4. Check for errors
 */
static uint8_t sdhci_cmd_complete(struct sdhci_host *host, struct mmc_command *cmd)
{
	uint8_t i;
	uint16_t retry = 0;
	uint32_t int_status;

	do {
		int_status = REG_READ16(host, SDHCI_NRML_INT_STS_REG);
		int_status &= SDHCI_INT_STS_CMD_COMPLETE;

		if (int_status == SDHCI_INT_STS_CMD_COMPLETE)
			break;

		retry++;
		udelay(500);
		if (retry == SDHCI_MAX_CMD_RETRY) {
			dprintf(CRITICAL, "Error: Command never completed\n");
			goto err;
		}
	} while(1);

	/* Command is complete, clear the interrupt bit */
	REG_WRITE16(host, SDHCI_INT_STS_CMD_COMPLETE, SDHCI_NRML_INT_STS_REG);

	/* Copy the command response,
	 * The valid bits for R2 response are 0-119, & but the actual response
	 * is stored in bits 8-128. We need to move 8 bits of MSB of each
	 * response to register 8 bits of LSB of next response register.
	 * As:
	 * MSB 8 bits of RESP0 --> LSB 8 bits of RESP1
	 * MSB 8 bits of RESP1 --> LSB 8 bits of RESP2
	 * MSB 8 bits of RESP2 --> LSB 8 bits of RESP3
	 */
	if (cmd->resp_type == SDHCI_CMD_RESP_R2) {
		for (i = 0; i < 4; i++) {
			cmd->resp[i] = REG_READ32(host, SDHCI_RESP_REG + (i * 4));
			cmd->resp[i] <<= SDHCI_RESP_LSHIFT;

			if (i != 0)
				cmd->resp[i] |= (REG_READ32(host, SDHCI_RESP_REG + ((i-1) * 4)) >> SDHCI_RESP_RSHIFT);
		}
	} else
			cmd->resp[0] = REG_READ32(host, SDHCI_RESP_REG);

	retry = 0;

	/*
	 * Clear the transfer complete interrupt
	 */
	if (cmd->data_present || cmd->cmd_index == SDHCI_SWITCH_CMD) {
		do {
			int_status = REG_READ16(host, SDHCI_NRML_INT_STS_REG);
			int_status &= SDHCI_INT_STS_TRANS_COMPLETE;

			if (int_status & SDHCI_INT_STS_TRANS_COMPLETE)
				break;

			retry++;
			udelay(1000);
			if (retry == SDHCI_MAX_TRANS_RETRY) {
				dprintf(CRITICAL, "Error: Transfer never completed\n");
				goto err;
			}
		} while(1);

		/* Transfer is complete, clear the interrupt bit */
		REG_WRITE16(host, SDHCI_INT_STS_TRANS_COMPLETE, SDHCI_NRML_INT_STS_REG);
	}

err:
	/* Look for errors */
	int_status = REG_READ16(host, SDHCI_NRML_INT_STS_REG);
	if (int_status & SDHCI_ERR_INT_STAT_MASK) {
		if (sdhci_cmd_err_status(host)) {
			dprintf(CRITICAL, "Error: Command completed with errors\n");
			return 1;
		}
	}

	/* Reset data & command line */
	if (cmd->data_present)
		REG_WRITE8(host, (SOFT_RESET_CMD | SOFT_RESET_DATA), SDHCI_RESET_REG);

	return 0;
}

/*
 * Function: sdhci prep desc table
 * Arg     : Pointer data & length
 * Return  : Pointer to desc table
 * Flow:   : Prepare the adma table as per the sd spec v 3.0
 */
static struct desc_entry *sdhci_prep_desc_table(void *data, uint32_t len)
{
	struct desc_entry *sg_list;
	uint32_t sg_len = 0;
	uint32_t remain = 0;
	uint32_t i;
	uint32_t table_len = 0;

	if (len <= SDHCI_ADMA_DESC_LINE_SZ) {
		/* Allocate only one descriptor */
		sg_list = (struct desc_entry *) memalign(4, sizeof(struct desc_entry));

		if (!sg_list) {
			dprintf(CRITICAL, "Error allocating memory\n");
			ASSERT(0);
		}

		sg_list[0].addr = data;
		sg_list[0].len = len;
		sg_list[0].tran_att = SDHCI_ADMA_TRANS_VALID | SDHCI_ADMA_TRANS_DATA
							  | SDHCI_ADMA_TRANS_END;

		arch_clean_invalidate_cache_range((addr_t)sg_list, sizeof(struct desc_entry));
	} else {
		/* Calculate the number of entries in desc table */
		sg_len = len / SDHCI_ADMA_DESC_LINE_SZ;
		remain = len - (sg_len * SDHCI_ADMA_DESC_LINE_SZ);
		/* Allocate sg_len + 1 entries */
		if (remain)
			sg_len++;

		table_len = (sg_len * sizeof(struct desc_entry));

		sg_list = (struct desc_entry *) memalign(4, table_len);

		if (!sg_list) {
			dprintf(CRITICAL, "Error allocating memory\n");
			ASSERT(0);
		}

		memset((void *) sg_list, 0, table_len);

		/*
		 * Prepare sglist in the format:
		 *  ___________________________________________________
		 * |Transfer Len | Transfer ATTR | Data Address        |
		 * | (16 bit)    | (16 bit)      | (32 bit)            |
		 * |_____________|_______________|_____________________|
		 */
		for (i = 0; i < (sg_len - 1); i++) {
				sg_list[i].addr = data;
				sg_list[i].len = SDHCI_ADMA_DESC_LINE_SZ;
				sg_list[i].tran_att = SDHCI_ADMA_TRANS_VALID | SDHCI_ADMA_TRANS_DATA;
				data += SDHCI_ADMA_DESC_LINE_SZ;
				len -= SDHCI_ADMA_DESC_LINE_SZ;
			}

			/* Fill the last entry of the table with Valid & End
			 * attributes
			 */
			sg_list[sg_len - 1].addr = data;
			sg_list[sg_len - 1].len = len;
			sg_list[sg_len - 1].tran_att = SDHCI_ADMA_TRANS_VALID | SDHCI_ADMA_TRANS_DATA
										   | SDHCI_ADMA_TRANS_END;
		}

	arch_clean_invalidate_cache_range((addr_t)sg_list, table_len);

	return sg_list;
}

/*
 * Function: sdhci adma transfer
 * Arg     : Host structure & command stucture
 * Return  : Pointer to desc table
 * Flow    : 1. Prepare data transfer properties
 *           2. Write adma register
 *           3. Write transfer mode register
 */
static struct desc_entry *sdhci_adma_transfer(struct sdhci_host *host,
											  struct mmc_command *cmd)
{
	uint32_t num_blks = 0;
	uint32_t sz;
	uint16_t trans_mode = 0;
	void *data;
	struct desc_entry *adma_addr;


	num_blks = cmd->data.num_blocks;
	data = cmd->data.data_ptr;

	sz = num_blks * SDHCI_MMC_BLK_SZ;

	/* Prepare adma descriptor table */
	adma_addr = sdhci_prep_desc_table(data, sz);

	/* Write the block size */
	REG_WRITE16(host, SDHCI_MMC_BLK_SZ, SDHCI_BLKSZ_REG);

	/* Enalbe auto cmd 23 for multi block transfer */
	if (num_blks > 1) {
		trans_mode |= SDHCI_TRANS_MULTI | SDHCI_AUTO_CMD23_EN | SDHCI_BLK_CNT_EN;
		REG_WRITE32(host, num_blks, SDHCI_ARG2_REG);
	}

	/*
	 * Set block count in block count register
	 */

	REG_WRITE16(host, num_blks, SDHCI_BLK_CNT_REG);

	if (cmd->trans_mode == SDHCI_MMC_READ)
		trans_mode |= SDHCI_READ_MODE;

	trans_mode |= SDHCI_DMA_EN;

	/* Write adma address to adma register */
	REG_WRITE32(host, (uint32_t) adma_addr, SDHCI_ADM_ADDR_REG);

	/* Set transfer mode */
	REG_WRITE16(host, trans_mode, SDHCI_TRANS_MODE_REG);

	return adma_addr;
}

/*
 * Function: sdhci send command
 * Arg     : Host structure & command stucture
 * Return  : 0 on Success, 1 on Failure
 * Flow:   : 1. Prepare the command register
 *           2. If data is present, prepare adma table
 *           3. Run the command
 *           4. Check for command results & take action
 */
uint32_t sdhci_send_command(struct sdhci_host *host, struct mmc_command *cmd)
{
	uint8_t retry = 0;
	uint32_t resp_type = 0;
	uint16_t present_state;
	uint32_t flags;
	struct desc_entry *sg_list = NULL;

	if (cmd->data_present)
		ASSERT(cmd->data.data_ptr);

	/*
	 * Assert if the data buffer is not aligned to cache
	 * line size for read operations.
	 * For write operations this function assumes that
	 * the cache is already flushed by the caller. As
	 * the data buffer we receive for write operation
	 * may not be aligned to cache boundary due to
	 * certain image formats like sparse image.
	 */
	if (cmd->trans_mode == SDHCI_READ_MODE)
		ASSERT(IS_CACHE_LINE_ALIGNED(cmd->data.data_ptr));

	do {
		present_state = REG_READ32(host, SDHCI_PRESENT_STATE_REG);
		/* check if CMD & DAT lines are free */
		present_state &= SDHCI_STATE_CMD_DAT_MASK;

		if (!present_state)
			break;
		udelay(1000);
		retry++;
		if (retry == 10) {
			dprintf(CRITICAL, "Error: CMD or DAT lines were never freed\n");
			return 1;
		}
	} while(1);

	switch(cmd->resp_type) {
		case SDHCI_CMD_RESP_R1:
		case SDHCI_CMD_RESP_R3:
		case SDHCI_CMD_RESP_R6:
		case SDHCI_CMD_RESP_R7:
			/* Response of length 48 have 32 bits
			 * of response data stored in RESP0[0:31]
			 */
			resp_type = SDHCI_CMD_RESP_48;
			break;

		case SDHCI_CMD_RESP_R2:
			/* Response of length 136 have 120 bits
			 * of response data stored in RESP0[0:119]
			 */
			resp_type = SDHCI_CMD_RESP_136;
			break;

		case SDHCI_CMD_RESP_R1B:
			/* Response of length 48 have 32 bits
			 * of response data stored in RESP0[0:31]
			 * & set CARD_BUSY status if card is busy
			 */
			resp_type = SDHCI_CMD_RESP_48_BUSY;
			break;

		case SDHCI_CMD_RESP_NONE:
			resp_type = SDHCI_CMD_RESP_NONE;
			break;

		default:
			dprintf(CRITICAL, "Invalid response type for the command\n");
			return 1;
	};

	flags =  (resp_type << SDHCI_CMD_RESP_TYPE_SEL_BIT);
	flags |= (cmd->data_present << SDHCI_CMD_DATA_PRESENT_BIT);
	flags |= (cmd->cmd_type << SDHCI_CMD_CMD_TYPE_BIT);

	/* Set the timeout value */
	REG_WRITE8(host, SDHCI_CMD_TIMEOUT, SDHCI_TIMEOUT_REG);

	/* Check if data needs to be processed */
	if (cmd->data_present)
		sg_list = sdhci_adma_transfer(host, cmd);

	/* Write the argument 1 */
	REG_WRITE32(host, cmd->argument, SDHCI_ARGUMENT_REG);

	/* Write the command register */
	REG_WRITE16(host, SDHCI_PREP_CMD(cmd->cmd_index, flags), SDHCI_CMD_REG);

	/* Command complete sequence */
	if (sdhci_cmd_complete(host, cmd))
		return 1;

	/* Invalidate the cache only for read operations */
	if (cmd->trans_mode == SDHCI_MMC_READ)
		arch_invalidate_cache_range((addr_t)cmd->data.data_ptr, (cmd->data.num_blocks * SDHCI_MMC_BLK_SZ));

	/* Free the scatter/gather list */
	if (sg_list)
		free(sg_list);

	return 0;
}

/*
 * Function: sdhci reset
 * Arg     : Host structure
 * Return  : None
 * Flow:   : Reset the host controller
 */
static void sdhci_reset(struct sdhci_host *host)
{
	uint32_t reg;

	REG_WRITE8(host, SDHCI_SOFT_RESET, SDHCI_RESET_REG);

	/* Wait for the reset to complete */
	do {
		reg = REG_READ8(host, SDHCI_RESET_REG);
		reg &= SDHCI_SOFT_RESET_MASK;

		if (!reg)
			break;
	} while(1);
}

/*
 * Function: sdhci mode enable
 * Arg     : Flag (0/1)
 * Return  : None
 * Flow:   : Enable/Disable Sdhci mode
 */
void sdhci_mode_enable(uint8_t enable)
{
	if (enable)
		writel(SDHCI_HC_MODE_EN, SDCC_MCI_HC_MODE);
	else
		writel(SDHCI_HC_MODE_DIS, SDCC_MCI_HC_MODE);
}

/*
 * Function: sdhci init
 * Arg     : Host structure
 * Return  : None
 * Flow:   : 1. Reset the controller
 *           2. Read the capabilities register & populate the host
 *           controller capabilities for use by other functions
 *           3. Enable the power control
 *           4. Set initial bus width
 *           5. Set Adma mode
 *           6. Enable the error status
 */
void sdhci_init(struct sdhci_host *host)
{
	uint32_t caps[2];
	event_t sdhc_event;

	event_init(&sdhc_event, false, EVENT_FLAG_AUTOUNSIGNAL);

	/*
	 * Reset the controller
	 */
	sdhci_reset(host);

	/* Read the capabilities register & store the info */
	caps[0] = REG_READ32(host, SDHCI_CAPS_REG1);
	caps[1] = REG_READ32(host, SDHCI_CAPS_REG2);

	host->caps.base_clk_rate = (caps[0] & SDHCI_CLK_RATE_MASK) >> SDHCI_CLK_RATE_BIT;
	host->caps.base_clk_rate *= 1000000;

	/* Get the max block length for mmc */
	host->caps.max_blk_len = (caps[0] & SDHCI_BLK_LEN_MASK) >> SDHCI_BLK_LEN_BIT;

	/* 8 bit Bus width */
	if (caps[0] & SDHCI_8BIT_WIDTH_MASK)
		host->caps.bus_width_8bit = 1;

	/* Adma support */
	if (caps[0] & SDHCI_BLK_ADMA_MASK)
		host->caps.adma_support = 1;

	/* Supported voltage */
	if (caps[0] & SDHCI_3_3_VOL_MASK)
		host->caps.voltage = SDHCI_VOL_3_3;
	else if (caps[0] & SDHCI_3_0_VOL_MASK)
		host->caps.voltage = SDHCI_VOL_3_0;
	else if (caps[0] & SDHCI_1_8_VOL_MASK)
		host->caps.voltage = SDHCI_VOL_1_8;

	/* DDR mode support */
	host->caps.ddr_support = (caps[1] & SDHCI_DDR_MODE_MASK) ? 1 : 0;

	/* SDR50 mode support */
	host->caps.sdr50_support = (caps[1] & SDHCI_SDR50_MODE_MASK) ? 1 : 0;

	/*
	 * Register the interrupt handler for pwr irq
	 */
	register_int_handler(SDCC_PWRCTRL_IRQ, sdhci_int_handler, &sdhc_event);
	unmask_interrupt(SDCC_PWRCTRL_IRQ);

	/* Enable pwr control interrupt */
	writel(SDCC_HC_PWR_CTRL_INT, SDCC_HC_PWRCTL_MASK_REG);

	/* Set bus power on */
	sdhci_set_bus_power_on(host);

	/* Wait for power interrupt to be handled */
	event_wait(&sdhc_event);

	/* Set bus width */
	sdhci_set_bus_width(host, SDHCI_BUS_WITDH_1BIT);

	/* Set Adma mode */
	sdhci_set_adma_mode(host);

	/*
	 * Enable error status
	 */
	sdhci_error_status_enable(host);
}
