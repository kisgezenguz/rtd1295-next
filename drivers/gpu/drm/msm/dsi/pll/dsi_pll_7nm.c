/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2018, The Linux Foundation
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/iopoll.h>

#include "dsi_pll.h"
#include "dsi.xml.h"

/*
 * DSI PLL 7nm - clock diagram (eg: DSI0): TODO: updated CPHY diagram
 *
 *           dsi0_pll_out_div_clk  dsi0_pll_bit_clk
 *                              |                |
 *                              |                |
 *                 +---------+  |  +----------+  |  +----+
 *  dsi0vco_clk ---| out_div |--o--| divl_3_0 |--o--| /8 |-- dsi0_phy_pll_out_byteclk
 *                 +---------+  |  +----------+  |  +----+
 *                              |                |
 *                              |                |         dsi0_pll_by_2_bit_clk
 *                              |                |          |
 *                              |                |  +----+  |  |\  dsi0_pclk_mux
 *                              |                |--| /2 |--o--| \   |
 *                              |                |  +----+     |  \  |  +---------+
 *                              |                --------------|  |--o--| div_7_4 |-- dsi0_phy_pll_out_dsiclk
 *                              |------------------------------|  /     +---------+
 *                              |          +-----+             | /
 *                              -----------| /4? |--o----------|/
 *                                         +-----+  |           |
 *                                                  |           |dsiclk_sel
 *                                                  |
 *                                                  dsi0_pll_post_out_div_clk
 */

#define DSI_BYTE_PLL_CLK		0
#define DSI_PIXEL_PLL_CLK		1
#define NUM_PROVIDED_CLKS		2

#define VCO_REF_CLK_RATE		19200000

struct dsi_pll_regs {
	u32 pll_prop_gain_rate;
	u32 pll_lockdet_rate;
	u32 decimal_div_start;
	u32 frac_div_start_low;
	u32 frac_div_start_mid;
	u32 frac_div_start_high;
	u32 pll_clock_inverters;
	u32 ssc_stepsize_low;
	u32 ssc_stepsize_high;
	u32 ssc_div_per_low;
	u32 ssc_div_per_high;
	u32 ssc_adjper_low;
	u32 ssc_adjper_high;
	u32 ssc_control;
};

struct dsi_pll_config {
	u32 ref_freq;
	bool div_override;
	u32 output_div;
	bool ignore_frac;
	bool disable_prescaler;
	bool enable_ssc;
	bool ssc_center;
	u32 dec_bits;
	u32 frac_bits;
	u32 lock_timer;
	u32 ssc_freq;
	u32 ssc_offset;
	u32 ssc_adj_per;
	u32 thresh_cycles;
	u32 refclk_cycles;
};

struct pll_7nm_cached_state {
	unsigned long vco_rate;
	u8 bit_clk_div;
	u8 pix_clk_div;
	u8 pll_out_div;
	u8 pll_mux;
};

struct dsi_pll_7nm {
	struct msm_dsi_pll base;

	int id;
	struct platform_device *pdev;

	void __iomem *phy_cmn_mmio;
	void __iomem *mmio;

	u64 vco_ref_clk_rate;
	u64 vco_current_rate;

	/* protects REG_DSI_7nm_PHY_CMN_CLK_CFG0 register */
	spinlock_t postdiv_lock;

	int vco_delay;
	struct dsi_pll_config pll_configuration;
	struct dsi_pll_regs reg_setup;

	/* private clocks: */
	struct clk_hw *out_div_clk_hw;
	struct clk_hw *bit_clk_hw;
	struct clk_hw *byte_clk_hw;
	struct clk_hw *by_2_bit_clk_hw;
	struct clk_hw *post_out_div_clk_hw;
	struct clk_hw *pclk_mux_hw;
	struct clk_hw *out_dsiclk_hw;

	/* clock-provider: */
	struct clk_hw_onecell_data *hw_data;

	struct pll_7nm_cached_state cached_state;

	enum msm_dsi_phy_usecase uc;
	struct dsi_pll_7nm *slave;
};

#define to_pll_7nm(x)	container_of(x, struct dsi_pll_7nm, base)

/*
 * Global list of private DSI PLL struct pointers. We need this for Dual DSI
 * mode, where the master PLL's clk_ops needs access the slave's private data
 */
static struct dsi_pll_7nm *pll_7nm_list[DSI_MAX];

static void dsi_pll_setup_config(struct dsi_pll_7nm *pll)
{
	struct dsi_pll_config *config = &pll->pll_configuration;

	config->ref_freq = pll->vco_ref_clk_rate;
	config->output_div = 1;
	config->dec_bits = 8;
	config->frac_bits = 18;
	config->lock_timer = 64;
	config->ssc_freq = 31500;
	config->ssc_offset = 4800;
	config->ssc_adj_per = 2;
	config->thresh_cycles = 32;
	config->refclk_cycles = 256;

	config->div_override = false;
	config->ignore_frac = false;
	config->disable_prescaler = false;

	/* TODO: ssc enable */
	config->enable_ssc = false;
	config->ssc_center = 0;
}

static void dsi_pll_calc_dec_frac(struct dsi_pll_7nm *pll)
{
	struct dsi_pll_config *config = &pll->pll_configuration;
	struct dsi_pll_regs *regs = &pll->reg_setup;
	u64 fref = pll->vco_ref_clk_rate;
	u64 pll_freq;
	u64 divider;
	u64 dec, dec_multiple;
	u32 frac;
	u64 multiplier;

	pll_freq = pll->vco_current_rate;

	if (config->disable_prescaler)
		divider = fref;
	else
		divider = fref * 2;

	multiplier = 1 << config->frac_bits;
	dec_multiple = div_u64(pll_freq * multiplier, divider);
	div_u64_rem(dec_multiple, multiplier, &frac);

	dec = div_u64(dec_multiple, multiplier);

	if (pll->base.type != MSM_DSI_PHY_7NM_V4_1)
		regs->pll_clock_inverters = 0x28;
	else if (pll_freq <= 1000000000ULL)
		regs->pll_clock_inverters = 0xa0;
	else if (pll_freq <= 2500000000ULL)
		regs->pll_clock_inverters = 0x20;
	else if (pll_freq <= 3020000000ULL)
		regs->pll_clock_inverters = 0x00;
	else
		regs->pll_clock_inverters = 0x40;

	regs->pll_lockdet_rate = config->lock_timer;
	regs->decimal_div_start = dec;
	regs->frac_div_start_low = (frac & 0xff);
	regs->frac_div_start_mid = (frac & 0xff00) >> 8;
	regs->frac_div_start_high = (frac & 0x30000) >> 16;
}

#define SSC_CENTER		BIT(0)
#define SSC_EN			BIT(1)

static void dsi_pll_calc_ssc(struct dsi_pll_7nm *pll)
{
	struct dsi_pll_config *config = &pll->pll_configuration;
	struct dsi_pll_regs *regs = &pll->reg_setup;
	u32 ssc_per;
	u32 ssc_mod;
	u64 ssc_step_size;
	u64 frac;

	if (!config->enable_ssc) {
		DBG("SSC not enabled\n");
		return;
	}

	ssc_per = DIV_ROUND_CLOSEST(config->ref_freq, config->ssc_freq) / 2 - 1;
	ssc_mod = (ssc_per + 1) % (config->ssc_adj_per + 1);
	ssc_per -= ssc_mod;

	frac = regs->frac_div_start_low |
			(regs->frac_div_start_mid << 8) |
			(regs->frac_div_start_high << 16);
	ssc_step_size = regs->decimal_div_start;
	ssc_step_size *= (1 << config->frac_bits);
	ssc_step_size += frac;
	ssc_step_size *= config->ssc_offset;
	ssc_step_size *= (config->ssc_adj_per + 1);
	ssc_step_size = div_u64(ssc_step_size, (ssc_per + 1));
	ssc_step_size = DIV_ROUND_CLOSEST_ULL(ssc_step_size, 1000000);

	regs->ssc_div_per_low = ssc_per & 0xFF;
	regs->ssc_div_per_high = (ssc_per & 0xFF00) >> 8;
	regs->ssc_stepsize_low = (u32)(ssc_step_size & 0xFF);
	regs->ssc_stepsize_high = (u32)((ssc_step_size & 0xFF00) >> 8);
	regs->ssc_adjper_low = config->ssc_adj_per & 0xFF;
	regs->ssc_adjper_high = (config->ssc_adj_per & 0xFF00) >> 8;

	regs->ssc_control = config->ssc_center ? SSC_CENTER : 0;

	pr_debug("SCC: Dec:%d, frac:%llu, frac_bits:%d\n",
		 regs->decimal_div_start, frac, config->frac_bits);
	pr_debug("SSC: div_per:0x%X, stepsize:0x%X, adjper:0x%X\n",
		 ssc_per, (u32)ssc_step_size, config->ssc_adj_per);
}

static void dsi_pll_ssc_commit(struct dsi_pll_7nm *pll)
{
	void __iomem *base = pll->mmio;
	struct dsi_pll_regs *regs = &pll->reg_setup;

	if (pll->pll_configuration.enable_ssc) {
		pr_debug("SSC is enabled\n");

		pll_write(base + REG_DSI_7nm_PHY_PLL_SSC_STEPSIZE_LOW_1,
			  regs->ssc_stepsize_low);
		pll_write(base + REG_DSI_7nm_PHY_PLL_SSC_STEPSIZE_HIGH_1,
			  regs->ssc_stepsize_high);
		pll_write(base + REG_DSI_7nm_PHY_PLL_SSC_DIV_PER_LOW_1,
			  regs->ssc_div_per_low);
		pll_write(base + REG_DSI_7nm_PHY_PLL_SSC_DIV_PER_HIGH_1,
			  regs->ssc_div_per_high);
		pll_write(base + REG_DSI_7nm_PHY_PLL_SSC_ADJPER_LOW_1,
			  regs->ssc_adjper_low);
		pll_write(base + REG_DSI_7nm_PHY_PLL_SSC_ADJPER_HIGH_1,
			  regs->ssc_adjper_high);
		pll_write(base + REG_DSI_7nm_PHY_PLL_SSC_CONTROL,
			  SSC_EN | regs->ssc_control);
	}
}

static void dsi_pll_config_hzindep_reg(struct dsi_pll_7nm *pll)
{
	void __iomem *base = pll->mmio;
	u8 analog_controls_five_1 = 0x01, vco_config_1 = 0x00;

	if (pll->base.type == MSM_DSI_PHY_7NM_V4_1) {
		if (pll->vco_current_rate >= 3100000000ULL)
			analog_controls_five_1 = 0x03;

		if (pll->vco_current_rate < 1520000000ULL)
			vco_config_1 = 0x08;
		else if (pll->vco_current_rate < 2990000000ULL)
			vco_config_1 = 0x01;
	}

	pll_write(base + REG_DSI_7nm_PHY_PLL_ANALOG_CONTROLS_FIVE_1,
		  analog_controls_five_1);
	pll_write(base + REG_DSI_7nm_PHY_PLL_VCO_CONFIG_1, vco_config_1);
	pll_write(base + REG_DSI_7nm_PHY_PLL_ANALOG_CONTROLS_FIVE, 0x01);
	pll_write(base + REG_DSI_7nm_PHY_PLL_ANALOG_CONTROLS_TWO, 0x03);
	pll_write(base + REG_DSI_7nm_PHY_PLL_ANALOG_CONTROLS_THREE, 0x00);
	pll_write(base + REG_DSI_7nm_PHY_PLL_DSM_DIVIDER, 0x00);
	pll_write(base + REG_DSI_7nm_PHY_PLL_FEEDBACK_DIVIDER, 0x4e);
	pll_write(base + REG_DSI_7nm_PHY_PLL_CALIBRATION_SETTINGS, 0x40);
	pll_write(base + REG_DSI_7nm_PHY_PLL_BAND_SEL_CAL_SETTINGS_THREE, 0xba);
	pll_write(base + REG_DSI_7nm_PHY_PLL_FREQ_DETECT_SETTINGS_ONE, 0x0c);
	pll_write(base + REG_DSI_7nm_PHY_PLL_OUTDIV, 0x00);
	pll_write(base + REG_DSI_7nm_PHY_PLL_CORE_OVERRIDE, 0x00);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PLL_DIGITAL_TIMERS_TWO, 0x08);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PLL_PROP_GAIN_RATE_1, 0x0a);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PLL_BAND_SEL_RATE_1, 0xc0);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PLL_INT_GAIN_IFILT_BAND_1, 0x84);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PLL_INT_GAIN_IFILT_BAND_1, 0x82);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PLL_FL_INT_GAIN_PFILT_BAND_1, 0x4c);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PLL_LOCK_OVERRIDE, 0x80);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PFILT, 0x29);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PFILT, 0x2f);
	pll_write(base + REG_DSI_7nm_PHY_PLL_IFILT, 0x2a);
	pll_write(base + REG_DSI_7nm_PHY_PLL_IFILT,
		  pll->base.type == MSM_DSI_PHY_7NM_V4_1 ? 0x3f : 0x22);

	if (pll->base.type == MSM_DSI_PHY_7NM_V4_1) {
		pll_write(base + REG_DSI_7nm_PHY_PLL_PERF_OPTIMIZE, 0x22);
		if (pll->slave)
			pll_write(pll->slave->mmio + REG_DSI_7nm_PHY_PLL_PERF_OPTIMIZE, 0x22);
	}
}

static void dsi_pll_commit(struct dsi_pll_7nm *pll)
{
	void __iomem *base = pll->mmio;
	struct dsi_pll_regs *reg = &pll->reg_setup;

	pll_write(base + REG_DSI_7nm_PHY_PLL_CORE_INPUT_OVERRIDE, 0x12);
	pll_write(base + REG_DSI_7nm_PHY_PLL_DECIMAL_DIV_START_1, reg->decimal_div_start);
	pll_write(base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_LOW_1, reg->frac_div_start_low);
	pll_write(base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_MID_1, reg->frac_div_start_mid);
	pll_write(base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_HIGH_1, reg->frac_div_start_high);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PLL_LOCKDET_RATE_1, 0x40);
	pll_write(base + REG_DSI_7nm_PHY_PLL_PLL_LOCK_DELAY, 0x06);
	pll_write(base + REG_DSI_7nm_PHY_PLL_CMODE_1, 0x10); /* TODO: 0x00 for CPHY */
	pll_write(base + REG_DSI_7nm_PHY_PLL_CLOCK_INVERTERS, reg->pll_clock_inverters);
}

static int dsi_pll_7nm_vco_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	struct msm_dsi_pll *pll = hw_clk_to_pll(hw);
	struct dsi_pll_7nm *pll_7nm = to_pll_7nm(pll);

	DBG("DSI PLL%d rate=%lu, parent's=%lu", pll_7nm->id, rate,
	    parent_rate);

	pll_7nm->vco_current_rate = rate;
	pll_7nm->vco_ref_clk_rate = VCO_REF_CLK_RATE;

	dsi_pll_setup_config(pll_7nm);

	dsi_pll_calc_dec_frac(pll_7nm);

	dsi_pll_calc_ssc(pll_7nm);

	dsi_pll_commit(pll_7nm);

	dsi_pll_config_hzindep_reg(pll_7nm);

	dsi_pll_ssc_commit(pll_7nm);

	/* flush, ensure all register writes are done*/
	wmb();

	return 0;
}

static int dsi_pll_7nm_lock_status(struct dsi_pll_7nm *pll)
{
	int rc;
	u32 status = 0;
	u32 const delay_us = 100;
	u32 const timeout_us = 5000;

	rc = readl_poll_timeout_atomic(pll->mmio +
				       REG_DSI_7nm_PHY_PLL_COMMON_STATUS_ONE,
				       status,
				       ((status & BIT(0)) > 0),
				       delay_us,
				       timeout_us);
	if (rc)
		pr_err("DSI PLL(%d) lock failed, status=0x%08x\n",
		       pll->id, status);

	return rc;
}

static void dsi_pll_disable_pll_bias(struct dsi_pll_7nm *pll)
{
	u32 data = pll_read(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_CTRL_0);

	pll_write(pll->mmio + REG_DSI_7nm_PHY_PLL_SYSTEM_MUXES, 0);
	pll_write(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_CTRL_0, data & ~BIT(5));
	ndelay(250);
}

static void dsi_pll_enable_pll_bias(struct dsi_pll_7nm *pll)
{
	u32 data = pll_read(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_CTRL_0);

	pll_write(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_CTRL_0, data | BIT(5));
	pll_write(pll->mmio + REG_DSI_7nm_PHY_PLL_SYSTEM_MUXES, 0xc0);
	ndelay(250);
}

static void dsi_pll_disable_global_clk(struct dsi_pll_7nm *pll)
{
	u32 data;

	data = pll_read(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_CLK_CFG1);
	pll_write(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_CLK_CFG1, data & ~BIT(5));
}

static void dsi_pll_enable_global_clk(struct dsi_pll_7nm *pll)
{
	u32 data;

	pll_write(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_CTRL_3, 0x04);

	data = pll_read(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_CLK_CFG1);
	pll_write(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_CLK_CFG1,
		  data | BIT(5) | BIT(4));
}

static void dsi_pll_phy_dig_reset(struct dsi_pll_7nm *pll)
{
	/*
	 * Reset the PHY digital domain. This would be needed when
	 * coming out of a CX or analog rail power collapse while
	 * ensuring that the pads maintain LP00 or LP11 state
	 */
	pll_write(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_GLBL_DIGTOP_SPARE4, BIT(0));
	wmb(); /* Ensure that the reset is deasserted */
	pll_write(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_GLBL_DIGTOP_SPARE4, 0x0);
	wmb(); /* Ensure that the reset is deasserted */
}

static int dsi_pll_7nm_vco_prepare(struct clk_hw *hw)
{
	struct msm_dsi_pll *pll = hw_clk_to_pll(hw);
	struct dsi_pll_7nm *pll_7nm = to_pll_7nm(pll);
	int rc;

	dsi_pll_enable_pll_bias(pll_7nm);
	if (pll_7nm->slave)
		dsi_pll_enable_pll_bias(pll_7nm->slave);

	/* Start PLL */
	pll_write(pll_7nm->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_PLL_CNTRL, 0x01);

	/*
	 * ensure all PLL configurations are written prior to checking
	 * for PLL lock.
	 */
	wmb();

	/* Check for PLL lock */
	rc = dsi_pll_7nm_lock_status(pll_7nm);
	if (rc) {
		pr_err("PLL(%d) lock failed\n", pll_7nm->id);
		goto error;
	}

	pll->pll_on = true;

	/*
	 * assert power on reset for PHY digital in case the PLL is
	 * enabled after CX of analog domain power collapse. This needs
	 * to be done before enabling the global clk.
	 */
	dsi_pll_phy_dig_reset(pll_7nm);
	if (pll_7nm->slave)
		dsi_pll_phy_dig_reset(pll_7nm->slave);

	dsi_pll_enable_global_clk(pll_7nm);
	if (pll_7nm->slave)
		dsi_pll_enable_global_clk(pll_7nm->slave);

error:
	return rc;
}

static void dsi_pll_disable_sub(struct dsi_pll_7nm *pll)
{
	pll_write(pll->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_RBUF_CTRL, 0);
	dsi_pll_disable_pll_bias(pll);
}

static void dsi_pll_7nm_vco_unprepare(struct clk_hw *hw)
{
	struct msm_dsi_pll *pll = hw_clk_to_pll(hw);
	struct dsi_pll_7nm *pll_7nm = to_pll_7nm(pll);

	/*
	 * To avoid any stray glitches while abruptly powering down the PLL
	 * make sure to gate the clock using the clock enable bit before
	 * powering down the PLL
	 */
	dsi_pll_disable_global_clk(pll_7nm);
	pll_write(pll_7nm->phy_cmn_mmio + REG_DSI_7nm_PHY_CMN_PLL_CNTRL, 0);
	dsi_pll_disable_sub(pll_7nm);
	if (pll_7nm->slave) {
		dsi_pll_disable_global_clk(pll_7nm->slave);
		dsi_pll_disable_sub(pll_7nm->slave);
	}
	/* flush, ensure all register writes are done */
	wmb();
	pll->pll_on = false;
}

static unsigned long dsi_pll_7nm_vco_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct msm_dsi_pll *pll = hw_clk_to_pll(hw);
	struct dsi_pll_7nm *pll_7nm = to_pll_7nm(pll);
	void __iomem *base = pll_7nm->mmio;
	u64 ref_clk = pll_7nm->vco_ref_clk_rate;
	u64 vco_rate = 0x0;
	u64 multiplier;
	u32 frac;
	u32 dec;
	u64 pll_freq, tmp64;

	dec = pll_read(base + REG_DSI_7nm_PHY_PLL_DECIMAL_DIV_START_1);
	dec &= 0xff;

	frac = pll_read(base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_LOW_1);
	frac |= ((pll_read(base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_MID_1) &
		  0xff) << 8);
	frac |= ((pll_read(base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_HIGH_1) &
		  0x3) << 16);

	/*
	 * TODO:
	 *	1. Assumes prescaler is disabled
	 *	2. Multiplier is 2^18. it should be 2^(num_of_frac_bits)
	 */
	multiplier = 1 << 18;
	pll_freq = dec * (ref_clk * 2);
	tmp64 = (ref_clk * 2 * frac);
	pll_freq += div_u64(tmp64, multiplier);

	vco_rate = pll_freq;

	DBG("DSI PLL%d returning vco rate = %lu, dec = %x, frac = %x",
	    pll_7nm->id, (unsigned long)vco_rate, dec, frac);

	return (unsigned long)vco_rate;
}

static const struct clk_ops clk_ops_dsi_pll_7nm_vco = {
	.round_rate = msm_dsi_pll_helper_clk_round_rate,
	.set_rate = dsi_pll_7nm_vco_set_rate,
	.recalc_rate = dsi_pll_7nm_vco_recalc_rate,
	.prepare = dsi_pll_7nm_vco_prepare,
	.unprepare = dsi_pll_7nm_vco_unprepare,
};

/*
 * PLL Callbacks
 */

static void dsi_pll_7nm_save_state(struct msm_dsi_pll *pll)
{
	struct dsi_pll_7nm *pll_7nm = to_pll_7nm(pll);
	struct pll_7nm_cached_state *cached = &pll_7nm->cached_state;
	void __iomem *phy_base = pll_7nm->phy_cmn_mmio;
	u32 cmn_clk_cfg0, cmn_clk_cfg1;

	cached->pll_out_div = pll_read(pll_7nm->mmio +
				       REG_DSI_7nm_PHY_PLL_PLL_OUTDIV_RATE);
	cached->pll_out_div &= 0x3;

	cmn_clk_cfg0 = pll_read(phy_base + REG_DSI_7nm_PHY_CMN_CLK_CFG0);
	cached->bit_clk_div = cmn_clk_cfg0 & 0xf;
	cached->pix_clk_div = (cmn_clk_cfg0 & 0xf0) >> 4;

	cmn_clk_cfg1 = pll_read(phy_base + REG_DSI_7nm_PHY_CMN_CLK_CFG1);
	cached->pll_mux = cmn_clk_cfg1 & 0x3;

	DBG("DSI PLL%d outdiv %x bit_clk_div %x pix_clk_div %x pll_mux %x",
	    pll_7nm->id, cached->pll_out_div, cached->bit_clk_div,
	    cached->pix_clk_div, cached->pll_mux);
}

static int dsi_pll_7nm_restore_state(struct msm_dsi_pll *pll)
{
	struct dsi_pll_7nm *pll_7nm = to_pll_7nm(pll);
	struct pll_7nm_cached_state *cached = &pll_7nm->cached_state;
	void __iomem *phy_base = pll_7nm->phy_cmn_mmio;
	u32 val;

	val = pll_read(pll_7nm->mmio + REG_DSI_7nm_PHY_PLL_PLL_OUTDIV_RATE);
	val &= ~0x3;
	val |= cached->pll_out_div;
	pll_write(pll_7nm->mmio + REG_DSI_7nm_PHY_PLL_PLL_OUTDIV_RATE, val);

	pll_write(phy_base + REG_DSI_7nm_PHY_CMN_CLK_CFG0,
		  cached->bit_clk_div | (cached->pix_clk_div << 4));

	val = pll_read(phy_base + REG_DSI_7nm_PHY_CMN_CLK_CFG1);
	val &= ~0x3;
	val |= cached->pll_mux;
	pll_write(phy_base + REG_DSI_7nm_PHY_CMN_CLK_CFG1, val);

	DBG("DSI PLL%d", pll_7nm->id);

	return 0;
}

static int dsi_pll_7nm_set_usecase(struct msm_dsi_pll *pll,
				    enum msm_dsi_phy_usecase uc)
{
	struct dsi_pll_7nm *pll_7nm = to_pll_7nm(pll);
	void __iomem *base = pll_7nm->phy_cmn_mmio;
	u32 data = 0x0;	/* internal PLL */

	DBG("DSI PLL%d", pll_7nm->id);

	switch (uc) {
	case MSM_DSI_PHY_STANDALONE:
		break;
	case MSM_DSI_PHY_MASTER:
		pll_7nm->slave = pll_7nm_list[(pll_7nm->id + 1) % DSI_MAX];
		break;
	case MSM_DSI_PHY_SLAVE:
		data = 0x1; /* external PLL */
		break;
	default:
		return -EINVAL;
	}

	/* set PLL src */
	pll_write(base + REG_DSI_7nm_PHY_CMN_CLK_CFG1, (data << 2));

	pll_7nm->uc = uc;

	return 0;
}

static int dsi_pll_7nm_get_provider(struct msm_dsi_pll *pll,
				     struct clk **byte_clk_provider,
				     struct clk **pixel_clk_provider)
{
	struct dsi_pll_7nm *pll_7nm = to_pll_7nm(pll);
	struct clk_hw_onecell_data *hw_data = pll_7nm->hw_data;

	DBG("DSI PLL%d", pll_7nm->id);

	if (byte_clk_provider)
		*byte_clk_provider = hw_data->hws[DSI_BYTE_PLL_CLK]->clk;
	if (pixel_clk_provider)
		*pixel_clk_provider = hw_data->hws[DSI_PIXEL_PLL_CLK]->clk;

	return 0;
}

static void dsi_pll_7nm_destroy(struct msm_dsi_pll *pll)
{
	struct dsi_pll_7nm *pll_7nm = to_pll_7nm(pll);
	struct device *dev = &pll_7nm->pdev->dev;

	DBG("DSI PLL%d", pll_7nm->id);
	of_clk_del_provider(dev->of_node);

	clk_hw_unregister_divider(pll_7nm->out_dsiclk_hw);
	clk_hw_unregister_mux(pll_7nm->pclk_mux_hw);
	clk_hw_unregister_fixed_factor(pll_7nm->post_out_div_clk_hw);
	clk_hw_unregister_fixed_factor(pll_7nm->by_2_bit_clk_hw);
	clk_hw_unregister_fixed_factor(pll_7nm->byte_clk_hw);
	clk_hw_unregister_divider(pll_7nm->bit_clk_hw);
	clk_hw_unregister_divider(pll_7nm->out_div_clk_hw);
	clk_hw_unregister(&pll_7nm->base.clk_hw);
}

/*
 * The post dividers and mux clocks are created using the standard divider and
 * mux API. Unlike the 14nm PHY, the slave PLL doesn't need its dividers/mux
 * state to follow the master PLL's divider/mux state. Therefore, we don't
 * require special clock ops that also configure the slave PLL registers
 */
static int pll_7nm_register(struct dsi_pll_7nm *pll_7nm)
{
	char clk_name[32], parent[32], vco_name[32];
	char parent2[32], parent3[32], parent4[32];
	struct clk_init_data vco_init = {
		.parent_names = (const char *[]){ "bi_tcxo" },
		.num_parents = 1,
		.name = vco_name,
		.flags = CLK_IGNORE_UNUSED,
		.ops = &clk_ops_dsi_pll_7nm_vco,
	};
	struct device *dev = &pll_7nm->pdev->dev;
	struct clk_hw_onecell_data *hw_data;
	struct clk_hw *hw;
	int ret;

	DBG("DSI%d", pll_7nm->id);

	hw_data = devm_kzalloc(dev, sizeof(*hw_data) +
			       NUM_PROVIDED_CLKS * sizeof(struct clk_hw *),
			       GFP_KERNEL);
	if (!hw_data)
		return -ENOMEM;

	snprintf(vco_name, 32, "dsi%dvco_clk", pll_7nm->id);
	pll_7nm->base.clk_hw.init = &vco_init;

	ret = clk_hw_register(dev, &pll_7nm->base.clk_hw);
	if (ret)
		return ret;

	snprintf(clk_name, 32, "dsi%d_pll_out_div_clk", pll_7nm->id);
	snprintf(parent, 32, "dsi%dvco_clk", pll_7nm->id);

	hw = clk_hw_register_divider(dev, clk_name,
				     parent, CLK_SET_RATE_PARENT,
				     pll_7nm->mmio +
				     REG_DSI_7nm_PHY_PLL_PLL_OUTDIV_RATE,
				     0, 2, CLK_DIVIDER_POWER_OF_TWO, NULL);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		goto err_base_clk_hw;
	}

	pll_7nm->out_div_clk_hw = hw;

	snprintf(clk_name, 32, "dsi%d_pll_bit_clk", pll_7nm->id);
	snprintf(parent, 32, "dsi%d_pll_out_div_clk", pll_7nm->id);

	/* BIT CLK: DIV_CTRL_3_0 */
	hw = clk_hw_register_divider(dev, clk_name, parent,
				     CLK_SET_RATE_PARENT,
				     pll_7nm->phy_cmn_mmio +
				     REG_DSI_7nm_PHY_CMN_CLK_CFG0,
				     0, 4, CLK_DIVIDER_ONE_BASED,
				     &pll_7nm->postdiv_lock);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		goto err_out_div_clk_hw;
	}

	pll_7nm->bit_clk_hw = hw;

	snprintf(clk_name, 32, "dsi%d_phy_pll_out_byteclk", pll_7nm->id);
	snprintf(parent, 32, "dsi%d_pll_bit_clk", pll_7nm->id);

	/* DSI Byte clock = VCO_CLK / OUT_DIV / BIT_DIV / 8 */
	hw = clk_hw_register_fixed_factor(dev, clk_name, parent,
					  CLK_SET_RATE_PARENT, 1, 8);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		goto err_bit_clk_hw;
	}

	pll_7nm->byte_clk_hw = hw;
	hw_data->hws[DSI_BYTE_PLL_CLK] = hw;

	snprintf(clk_name, 32, "dsi%d_pll_by_2_bit_clk", pll_7nm->id);
	snprintf(parent, 32, "dsi%d_pll_bit_clk", pll_7nm->id);

	hw = clk_hw_register_fixed_factor(dev, clk_name, parent,
					  0, 1, 2);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		goto err_byte_clk_hw;
	}

	pll_7nm->by_2_bit_clk_hw = hw;

	snprintf(clk_name, 32, "dsi%d_pll_post_out_div_clk", pll_7nm->id);
	snprintf(parent, 32, "dsi%d_pll_out_div_clk", pll_7nm->id);

	hw = clk_hw_register_fixed_factor(dev, clk_name, parent,
					  0, 1, 4);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		goto err_by_2_bit_clk_hw;
	}

	pll_7nm->post_out_div_clk_hw = hw;

	snprintf(clk_name, 32, "dsi%d_pclk_mux", pll_7nm->id);
	snprintf(parent, 32, "dsi%d_pll_bit_clk", pll_7nm->id);
	snprintf(parent2, 32, "dsi%d_pll_by_2_bit_clk", pll_7nm->id);
	snprintf(parent3, 32, "dsi%d_pll_out_div_clk", pll_7nm->id);
	snprintf(parent4, 32, "dsi%d_pll_post_out_div_clk", pll_7nm->id);

	hw = clk_hw_register_mux(dev, clk_name,
				 ((const char *[]){
				 parent, parent2, parent3, parent4
				 }), 4, 0, pll_7nm->phy_cmn_mmio +
				 REG_DSI_7nm_PHY_CMN_CLK_CFG1,
				 0, 2, 0, NULL);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		goto err_post_out_div_clk_hw;
	}

	pll_7nm->pclk_mux_hw = hw;

	snprintf(clk_name, 32, "dsi%d_phy_pll_out_dsiclk", pll_7nm->id);
	snprintf(parent, 32, "dsi%d_pclk_mux", pll_7nm->id);

	/* PIX CLK DIV : DIV_CTRL_7_4*/
	hw = clk_hw_register_divider(dev, clk_name, parent,
				     0, pll_7nm->phy_cmn_mmio +
					REG_DSI_7nm_PHY_CMN_CLK_CFG0,
				     4, 4, CLK_DIVIDER_ONE_BASED,
				     &pll_7nm->postdiv_lock);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		goto err_pclk_mux_hw;
	}

	pll_7nm->out_dsiclk_hw = hw;
	hw_data->hws[DSI_PIXEL_PLL_CLK] = hw;

	hw_data->num = NUM_PROVIDED_CLKS;
	pll_7nm->hw_data = hw_data;

	ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_onecell_get,
				     pll_7nm->hw_data);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to register clk provider: %d\n", ret);
		goto err_dsiclk_hw;
	}

	return 0;

err_dsiclk_hw:
	clk_hw_unregister_divider(pll_7nm->out_dsiclk_hw);
err_pclk_mux_hw:
	clk_hw_unregister_mux(pll_7nm->pclk_mux_hw);
err_post_out_div_clk_hw:
	clk_hw_unregister_fixed_factor(pll_7nm->post_out_div_clk_hw);
err_by_2_bit_clk_hw:
	clk_hw_unregister_fixed_factor(pll_7nm->by_2_bit_clk_hw);
err_byte_clk_hw:
	clk_hw_unregister_fixed_factor(pll_7nm->byte_clk_hw);
err_bit_clk_hw:
	clk_hw_unregister_divider(pll_7nm->bit_clk_hw);
err_out_div_clk_hw:
	clk_hw_unregister_divider(pll_7nm->out_div_clk_hw);
err_base_clk_hw:
	clk_hw_unregister(&pll_7nm->base.clk_hw);

	return ret;
}

struct msm_dsi_pll *msm_dsi_pll_7nm_init(struct platform_device *pdev, int id)
{
	struct dsi_pll_7nm *pll_7nm;
	struct msm_dsi_pll *pll;
	int ret;

	pll_7nm = devm_kzalloc(&pdev->dev, sizeof(*pll_7nm), GFP_KERNEL);
	if (!pll_7nm)
		return ERR_PTR(-ENOMEM);

	DBG("DSI PLL%d", id);

	pll_7nm->pdev = pdev;
	pll_7nm->id = id;
	pll_7nm_list[id] = pll_7nm;

	pll_7nm->phy_cmn_mmio = msm_ioremap(pdev, "dsi_phy", "DSI_PHY");
	if (IS_ERR_OR_NULL(pll_7nm->phy_cmn_mmio)) {
		DRM_DEV_ERROR(&pdev->dev, "failed to map CMN PHY base\n");
		return ERR_PTR(-ENOMEM);
	}

	pll_7nm->mmio = msm_ioremap(pdev, "dsi_pll", "DSI_PLL");
	if (IS_ERR_OR_NULL(pll_7nm->mmio)) {
		DRM_DEV_ERROR(&pdev->dev, "failed to map PLL base\n");
		return ERR_PTR(-ENOMEM);
	}

	spin_lock_init(&pll_7nm->postdiv_lock);

	pll = &pll_7nm->base;
	pll->min_rate = 1000000000UL;
	pll->max_rate = 3500000000UL;
	if (pll->type == MSM_DSI_PHY_7NM_V4_1) {
		pll->min_rate = 600000000UL;
		pll->max_rate = 5000000000UL;
		/* workaround for max rate overflowing on 32-bit builds: */
		pll->max_rate = max(pll->max_rate, 0xffffffffUL);
	}
	pll->get_provider = dsi_pll_7nm_get_provider;
	pll->destroy = dsi_pll_7nm_destroy;
	pll->save_state = dsi_pll_7nm_save_state;
	pll->restore_state = dsi_pll_7nm_restore_state;
	pll->set_usecase = dsi_pll_7nm_set_usecase;

	pll_7nm->vco_delay = 1;

	ret = pll_7nm_register(pll_7nm);
	if (ret) {
		DRM_DEV_ERROR(&pdev->dev, "failed to register PLL: %d\n", ret);
		return ERR_PTR(ret);
	}

	/* TODO: Remove this when we have proper display handover support */
	msm_dsi_pll_save_state(pll);

	return pll;
}
