// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2009 - 2019 Broadcom */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/pci-ecam.h>
#include <linux/printk.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "../pci.h"

/* BRCM_PCIE_CAP_REGS - Offset for the mandatory capability config regs */
#define BRCM_PCIE_CAP_REGS				0x00ac

/* Broadcom STB PCIe Register Offsets */
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1				0x0188
#define  PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK	0xc
#define  PCIE_RC_CFG_VENDOR_SPECIFIC_REG1_LITTLE_ENDIAN			0x0

#define PCIE_RC_CFG_PRIV1_ID_VAL3			0x043c
#define  PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK	0xffffff

#define PCIE_RC_CFG_PRIV1_LINK_CAPABILITY			0x04dc
#define  PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_MAX_LINK_WIDTH_MASK	0x1f0
#define  PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_ASPM_SUPPORT_MASK	0xc00

#define PCIE_RC_CFG_PRIV1_ROOT_CAP			0x4f8
#define  PCIE_RC_CFG_PRIV1_ROOT_CAP_L1SS_MODE_MASK	0xf8

#define PCIE_RC_DL_MDIO_ADDR				0x1100
#define PCIE_RC_DL_MDIO_WR_DATA				0x1104
#define PCIE_RC_DL_MDIO_RD_DATA				0x1108

#define PCIE_RC_PL_REG_PHY_CTL_1			0x1804
#define  PCIE_RC_PL_REG_PHY_CTL_1_REG_P2_POWERDOWN_ENA_NOSYNC_MASK	0x8

#define PCIE_RC_PL_PHY_CTL_15				0x184c
#define  PCIE_RC_PL_PHY_CTL_15_DIS_PLL_PD_MASK		0x400000
#define  PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK	0xff

#define PCIE_MISC_MISC_CTRL				0x4008
#define  PCIE_MISC_MISC_CTRL_PCIE_RCB_64B_MODE_MASK	0x80
#define  PCIE_MISC_MISC_CTRL_PCIE_RCB_MPS_MODE_MASK	0x400
#define  PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK		0x1000
#define  PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_MASK	0x2000
#define  PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK	0x300000

#define  PCIE_MISC_MISC_CTRL_SCB0_SIZE_MASK		0xf8000000
#define  PCIE_MISC_MISC_CTRL_SCB1_SIZE_MASK		0x07c00000
#define  PCIE_MISC_MISC_CTRL_SCB2_SIZE_MASK		0x0000001f
#define  SCB_SIZE_MASK(x) PCIE_MISC_MISC_CTRL_SCB ## x ## _SIZE_MASK

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO		0x400c
#define PCIE_MEM_WIN0_LO(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO + ((win) * 8)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI		0x4010
#define PCIE_MEM_WIN0_HI(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI + ((win) * 8)

/*
 * NOTE: You may see the term "BAR" in a number of register names used by
 *   this driver.  The term is an artifact of when the HW core was an
 *   endpoint device (EP).  Now it is a root complex (RC) and anywhere a
 *   register has the term "BAR" it is related to an inbound window.
 */

#define PCIE_BRCM_MAX_INBOUND_WINS			16
#define PCIE_MISC_RC_BAR1_CONFIG_LO			0x402c
#define  PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK		0x1f

#define PCIE_MISC_RC_BAR4_CONFIG_LO			0x40d4


#define PCIE_MISC_MSI_BAR_CONFIG_LO			0x4044
#define PCIE_MISC_MSI_BAR_CONFIG_HI			0x4048

#define PCIE_MISC_MSI_DATA_CONFIG			0x404c
#define  PCIE_MISC_MSI_DATA_CONFIG_VAL_32		0xffe06540
#define  PCIE_MISC_MSI_DATA_CONFIG_VAL_8		0xfff86540

#define PCIE_MISC_PCIE_CTRL				0x4064
#define  PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_MASK	0x1
#define PCIE_MISC_PCIE_CTRL_PCIE_PERSTB_MASK		0x4

#define PCIE_MISC_PCIE_STATUS				0x4068
#define  PCIE_MISC_PCIE_STATUS_PCIE_PORT_MASK		0x80
#define  PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_MASK	0x20
#define  PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_MASK	0x10
#define  PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK	0x40

#define PCIE_MISC_REVISION				0x406c
#define  BRCM_PCIE_HW_REV_33				0x0303
#define  BRCM_PCIE_HW_REV_3_20				0x0320

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT		0x4070
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK	0xfff00000
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK	0xfff0
#define PCIE_MEM_WIN0_BASE_LIMIT(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT + ((win) * 4)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI			0x4080
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_MASK	0xff
#define PCIE_MEM_WIN0_BASE_HI(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI + ((win) * 8)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI			0x4084
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK	0xff
#define PCIE_MEM_WIN0_LIMIT_HI(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI + ((win) * 8)

#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK	0x2
#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK		0x200000
#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK		0x08000000
#define  PCIE_BMIPS_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK		0x00800000
#define  PCIE_CLKREQ_MASK \
	  (PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK | \
	   PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK)

#define PCIE_MISC_UBUS_BAR1_CONFIG_REMAP			0x40ac
#define  PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_ACCESS_EN_MASK	BIT(0)
#define PCIE_MISC_UBUS_BAR4_CONFIG_REMAP			0x410c

#define PCIE_MSI_INTR2_BASE		0x4500

/* Offsets from INTR2_CPU and MSI_INTR2 BASE offsets */
#define  MSI_INT_STATUS			0x0
#define  MSI_INT_CLR			0x8
#define  MSI_INT_MASK_SET		0x10
#define  MSI_INT_MASK_CLR		0x14

#define  PCIE_RGR1_SW_INIT_1_PERST_MASK			0x1
#define  PCIE_RGR1_SW_INIT_1_PERST_SHIFT		0x0

#define RGR1_SW_INIT_1_INIT_GENERIC_MASK		0x2
#define RGR1_SW_INIT_1_INIT_GENERIC_SHIFT		0x1
#define RGR1_SW_INIT_1_INIT_7278_MASK			0x1
#define RGR1_SW_INIT_1_INIT_7278_SHIFT			0x0

/* PCIe parameters */
#define BRCM_NUM_PCIE_OUT_WINS		0x4
#define BRCM_INT_PCI_MSI_NR		32
#define BRCM_INT_PCI_MSI_LEGACY_NR	8
#define BRCM_INT_PCI_MSI_SHIFT		0
#define BRCM_INT_PCI_MSI_MASK		GENMASK(BRCM_INT_PCI_MSI_NR - 1, 0)
#define BRCM_INT_PCI_MSI_LEGACY_MASK	GENMASK(31, \
						32 - BRCM_INT_PCI_MSI_LEGACY_NR)

/* MSI target addresses */
#define BRCM_MSI_TARGET_ADDR_LT_4GB	0x0fffffffcULL
#define BRCM_MSI_TARGET_ADDR_GT_4GB	0xffffffffcULL

/* MDIO registers */
#define MDIO_PORT0			0x0
#define MDIO_DATA_MASK			0x7fffffff
#define MDIO_PORT_MASK			0xf0000
#define MDIO_PORT_EXT_MASK		0x200000
#define MDIO_REGAD_MASK			0xffff
#define MDIO_CMD_MASK			0x00100000
#define MDIO_CMD_READ			0x1
#define MDIO_CMD_WRITE			0x0
#define MDIO_DATA_DONE_MASK		0x80000000
#define MDIO_RD_DONE(x)			(((x) & MDIO_DATA_DONE_MASK) ? 1 : 0)
#define MDIO_WT_DONE(x)			(((x) & MDIO_DATA_DONE_MASK) ? 0 : 1)
#define SSC_REGS_ADDR			0x1100
#define SET_ADDR_OFFSET			0x1f
#define SSC_CNTL_OFFSET			0x2
#define SSC_CNTL_OVRD_EN_MASK		0x8000
#define SSC_CNTL_OVRD_VAL_MASK		0x4000
#define SSC_STATUS_OFFSET		0x1
#define SSC_STATUS_SSC_MASK		0x400
#define SSC_STATUS_PLL_LOCK_MASK	0x800
#define PCIE_BRCM_MAX_MEMC		3

#define IDX_ADDR(pcie)			((pcie)->cfg->offsets[EXT_CFG_INDEX])
#define DATA_ADDR(pcie)			((pcie)->cfg->offsets[EXT_CFG_DATA])
#define PCIE_RGR1_SW_INIT_1(pcie)	((pcie)->cfg->offsets[RGR1_SW_INIT_1])
#define HARD_DEBUG(pcie)		((pcie)->cfg->offsets[PCIE_HARD_DEBUG])
#define INTR2_CPU_BASE(pcie)		((pcie)->cfg->offsets[PCIE_INTR2_CPU_BASE])

/* Rescal registers */
#define PCIE_DVT_PMU_PCIE_PHY_CTRL				0xc700
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS			0x3
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_MASK		0x4
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_SHIFT	0x2
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_MASK		0x2
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_SHIFT		0x1
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_MASK		0x1
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_SHIFT		0x0

/* Forward declarations */
struct brcm_pcie;

enum {
	RGR1_SW_INIT_1,
	EXT_CFG_INDEX,
	EXT_CFG_DATA,
	PCIE_HARD_DEBUG,
	PCIE_INTR2_CPU_BASE,
};

enum pcie_soc_base {
	GENERIC,
	BCM2711,
	BCM4908,
	BCM7278,
	BCM7425,
	BCM7435,
	BCM7712,
};

struct inbound_win {
	u64 size;
	u64 pci_offset;
	u64 cpu_addr;
};

/*
 * The RESCAL block is tied to PCIe controller #1, regardless of the number of
 * controllers, and turning off PCIe controller #1 prevents access to the RESCAL
 * register blocks, therefore no other controller can access this register
 * space, and depending upon the bus fabric we may get a timeout (UBUS/GISB),
 * or a hang (AXI).
 */
#define CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN		BIT(0)

struct pcie_cfg_data {
	const int *offsets;
	const enum pcie_soc_base soc_base;
	const bool has_phy;
	const u32 quirks;
	u8 num_inbound_wins;
	int (*perst_set)(struct brcm_pcie *pcie, u32 val);
	int (*bridge_sw_init_set)(struct brcm_pcie *pcie, u32 val);
	int (*post_setup)(struct brcm_pcie *pcie);
};

struct subdev_regulators {
	unsigned int num_supplies;
	struct regulator_bulk_data supplies[];
};

struct brcm_msi {
	struct device		*dev;
	void __iomem		*base;
	struct device_node	*np;
	struct irq_domain	*inner_domain;
	struct mutex		lock; /* guards the alloc/free operations */
	u64			target_addr;
	int			irq;
	DECLARE_BITMAP(used, BRCM_INT_PCI_MSI_NR);
	bool			legacy;
	/* Some chips have MSIs in bits [31..24] of a shared register. */
	int			legacy_shift;
	int			nr; /* No. of MSI available, depends on chip */
	/* This is the base pointer for interrupt status/set/clr regs */
	void __iomem		*intr_base;
};

/* Internal PCIe Host Controller Information.*/
struct brcm_pcie {
	struct device		*dev;
	void __iomem		*base;
	struct clk		*clk;
	struct device_node	*np;
	bool			ssc;
	int			gen;
	u64			msi_target_addr;
	struct brcm_msi		*msi;
	struct reset_control	*rescal;
	struct reset_control	*perst_reset;
	struct reset_control	*bridge_reset;
	struct reset_control	*swinit_reset;
	int			num_memc;
	u64			memc_size[PCIE_BRCM_MAX_MEMC];
	u32			hw_rev;
	struct subdev_regulators *sr;
	bool			ep_wakeup_capable;
	const struct pcie_cfg_data	*cfg;
};

static inline bool is_bmips(const struct brcm_pcie *pcie)
{
	return pcie->cfg->soc_base == BCM7435 || pcie->cfg->soc_base == BCM7425;
}

/*
 * This is to convert the size of the inbound "BAR" region to the
 * non-linear values of PCIE_X_MISC_RC_BAR[123]_CONFIG_LO.SIZE
 */
static int brcm_pcie_encode_ibar_size(u64 size)
{
	int log2_in = ilog2(size);

	if (log2_in >= 12 && log2_in <= 15)
		/* Covers 4KB to 32KB (inclusive) */
		return (log2_in - 12) + 0x1c;
	else if (log2_in >= 16 && log2_in <= 36)
		/* Covers 64KB to 64GB, (inclusive) */
		return log2_in - 15;
	/* Something is awry so disable */
	return 0;
}

static u32 brcm_pcie_mdio_form_pkt(int port, int regad, int cmd)
{
	u32 pkt = 0;

	pkt |= FIELD_PREP(MDIO_PORT_EXT_MASK, port >> 4);
	pkt |= FIELD_PREP(MDIO_PORT_MASK, port);
	pkt |= FIELD_PREP(MDIO_REGAD_MASK, regad);
	pkt |= FIELD_PREP(MDIO_CMD_MASK, cmd);

	return pkt;
}

/* negative return value indicates error */
static int brcm_pcie_mdio_read(void __iomem *base, u8 port, u8 regad, u32 *val)
{
	u32 data;
	int err;

	writel(brcm_pcie_mdio_form_pkt(port, regad, MDIO_CMD_READ),
		   base + PCIE_RC_DL_MDIO_ADDR);
	readl(base + PCIE_RC_DL_MDIO_ADDR);
	err = readl_poll_timeout_atomic(base + PCIE_RC_DL_MDIO_RD_DATA, data,
					MDIO_RD_DONE(data), 10, 100);
	*val = FIELD_GET(MDIO_DATA_MASK, data);

	return err;
}

/* negative return value indicates error */
static int brcm_pcie_mdio_write(void __iomem *base, u8 port,
				u8 regad, u16 wrdata)
{
	u32 data;
	int err;

	writel(brcm_pcie_mdio_form_pkt(port, regad, MDIO_CMD_WRITE),
		   base + PCIE_RC_DL_MDIO_ADDR);
	readl(base + PCIE_RC_DL_MDIO_ADDR);
	writel(MDIO_DATA_DONE_MASK | wrdata, base + PCIE_RC_DL_MDIO_WR_DATA);

	err = readl_poll_timeout_atomic(base + PCIE_RC_DL_MDIO_WR_DATA, data,
					MDIO_WT_DONE(data), 10, 100);
	return err;
}

/*
 * Configures device for Spread Spectrum Clocking (SSC) mode; a negative
 * return value indicates error.
 */
static int brcm_pcie_set_ssc(struct brcm_pcie *pcie)
{
	int pll, ssc;
	int ret;
	u32 tmp;

	ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, SET_ADDR_OFFSET,
				   SSC_REGS_ADDR);
	if (ret < 0)
		return ret;

	ret = brcm_pcie_mdio_read(pcie->base, MDIO_PORT0,
				  SSC_CNTL_OFFSET, &tmp);
	if (ret < 0)
		return ret;

	u32p_replace_bits(&tmp, 1, SSC_CNTL_OVRD_EN_MASK);
	u32p_replace_bits(&tmp, 1, SSC_CNTL_OVRD_VAL_MASK);
	ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0,
				   SSC_CNTL_OFFSET, tmp);
	if (ret < 0)
		return ret;

	usleep_range(1000, 2000);
	ret = brcm_pcie_mdio_read(pcie->base, MDIO_PORT0,
				  SSC_STATUS_OFFSET, &tmp);
	if (ret < 0)
		return ret;

	ssc = FIELD_GET(SSC_STATUS_SSC_MASK, tmp);
	pll = FIELD_GET(SSC_STATUS_PLL_LOCK_MASK, tmp);

	return ssc && pll ? 0 : -EIO;
}

/* Limits operation to a specific generation (1, 2, or 3) */
static void brcm_pcie_set_gen(struct brcm_pcie *pcie, int gen)
{
	u16 lnkctl2 = readw(pcie->base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2);
	u32 lnkcap = readl(pcie->base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY);

	u32p_replace_bits(&lnkcap, gen, PCI_EXP_LNKCAP_SLS);
	writel(lnkcap, pcie->base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY);

	u16p_replace_bits(&lnkctl2, gen, PCI_EXP_LNKCTL2_TLS);
	writew(lnkctl2, pcie->base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2);
}

static void brcm_pcie_set_outbound_win(struct brcm_pcie *pcie,
				       u8 win, u64 cpu_addr,
				       u64 pcie_addr, u64 size)
{
	u32 cpu_addr_mb_high, limit_addr_mb_high;
	phys_addr_t cpu_addr_mb, limit_addr_mb;
	int high_addr_shift;
	u32 tmp;

	/* Set the base of the pcie_addr window */
	writel(lower_32_bits(pcie_addr), pcie->base + PCIE_MEM_WIN0_LO(win));
	writel(upper_32_bits(pcie_addr), pcie->base + PCIE_MEM_WIN0_HI(win));

	/* Write the addr base & limit lower bits (in MBs) */
	cpu_addr_mb = cpu_addr / SZ_1M;
	limit_addr_mb = (cpu_addr + size - 1) / SZ_1M;

	tmp = readl(pcie->base + PCIE_MEM_WIN0_BASE_LIMIT(win));
	u32p_replace_bits(&tmp, cpu_addr_mb,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK);
	u32p_replace_bits(&tmp, limit_addr_mb,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK);
	writel(tmp, pcie->base + PCIE_MEM_WIN0_BASE_LIMIT(win));

	if (is_bmips(pcie))
		return;

	/* Write the cpu & limit addr upper bits */
	high_addr_shift =
		HWEIGHT32(PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK);

	cpu_addr_mb_high = cpu_addr_mb >> high_addr_shift;
	tmp = readl(pcie->base + PCIE_MEM_WIN0_BASE_HI(win));
	u32p_replace_bits(&tmp, cpu_addr_mb_high,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_MASK);
	writel(tmp, pcie->base + PCIE_MEM_WIN0_BASE_HI(win));

	limit_addr_mb_high = limit_addr_mb >> high_addr_shift;
	tmp = readl(pcie->base + PCIE_MEM_WIN0_LIMIT_HI(win));
	u32p_replace_bits(&tmp, limit_addr_mb_high,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK);
	writel(tmp, pcie->base + PCIE_MEM_WIN0_LIMIT_HI(win));
}

#define BRCM_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				 MSI_FLAG_USE_DEF_CHIP_OPS	| \
				 MSI_FLAG_NO_AFFINITY)

#define BRCM_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				  MSI_FLAG_MULTI_PCI_MSI)

static const struct msi_parent_ops brcm_msi_parent_ops = {
	.required_flags		= BRCM_MSI_FLAGS_REQUIRED,
	.supported_flags	= BRCM_MSI_FLAGS_SUPPORTED,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,
	.prefix			= "BRCM-",
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

static void brcm_pcie_msi_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long status;
	struct brcm_msi *msi;
	struct device *dev;
	u32 bit;

	chained_irq_enter(chip, desc);
	msi = irq_desc_get_handler_data(desc);
	dev = msi->dev;

	status = readl(msi->intr_base + MSI_INT_STATUS);
	status >>= msi->legacy_shift;

	for_each_set_bit(bit, &status, msi->nr) {
		int ret;
		ret = generic_handle_domain_irq(msi->inner_domain, bit);
		if (ret)
			dev_dbg(dev, "unexpected MSI\n");
	}

	chained_irq_exit(chip, desc);
}

static void brcm_msi_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct brcm_msi *msi = irq_data_get_irq_chip_data(data);

	msg->address_lo = lower_32_bits(msi->target_addr);
	msg->address_hi = upper_32_bits(msi->target_addr);
	msg->data = (0xffff & PCIE_MISC_MSI_DATA_CONFIG_VAL_32) | data->hwirq;
}

static void brcm_msi_ack_irq(struct irq_data *data)
{
	struct brcm_msi *msi = irq_data_get_irq_chip_data(data);
	const int shift_amt = data->hwirq + msi->legacy_shift;

	writel(1 << shift_amt, msi->intr_base + MSI_INT_CLR);
}


static struct irq_chip brcm_msi_bottom_irq_chip = {
	.name			= "BRCM STB MSI",
	.irq_compose_msi_msg	= brcm_msi_compose_msi_msg,
	.irq_ack                = brcm_msi_ack_irq,
};

static int brcm_msi_alloc(struct brcm_msi *msi, unsigned int nr_irqs)
{
	int hwirq;

	mutex_lock(&msi->lock);
	hwirq = bitmap_find_free_region(msi->used, msi->nr,
					order_base_2(nr_irqs));
	mutex_unlock(&msi->lock);

	return hwirq;
}

static void brcm_msi_free(struct brcm_msi *msi, unsigned long hwirq,
			  unsigned int nr_irqs)
{
	mutex_lock(&msi->lock);
	bitmap_release_region(msi->used, hwirq, order_base_2(nr_irqs));
	mutex_unlock(&msi->lock);
}

static int brcm_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *args)
{
	struct brcm_msi *msi = domain->host_data;
	int hwirq, i;

	hwirq = brcm_msi_alloc(msi, nr_irqs);

	if (hwirq < 0)
		return hwirq;

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_info(domain, virq + i, (irq_hw_number_t)hwirq + i,
				    &brcm_msi_bottom_irq_chip, domain->host_data,
				    handle_edge_irq, NULL, NULL);
	return 0;
}

static void brcm_irq_domain_free(struct irq_domain *domain,
				 unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct brcm_msi *msi = irq_data_get_irq_chip_data(d);

	brcm_msi_free(msi, d->hwirq, nr_irqs);
}

static const struct irq_domain_ops msi_domain_ops = {
	.alloc	= brcm_irq_domain_alloc,
	.free	= brcm_irq_domain_free,
};

static int brcm_allocate_domains(struct brcm_msi *msi)
{
	struct device *dev = msi->dev;

	struct irq_domain_info info = {
		.fwnode		= of_fwnode_handle(msi->np),
		.ops		= &msi_domain_ops,
		.host_data	= msi,
		.size		= msi->nr,
	};

	msi->inner_domain = msi_create_parent_irq_domain(&info, &brcm_msi_parent_ops);
	if (!msi->inner_domain) {
		dev_err(dev, "failed to create MSI domain\n");
		return -ENOMEM;
	}

	return 0;
}

static void brcm_free_domains(struct brcm_msi *msi)
{
	irq_domain_remove(msi->inner_domain);
}

static void brcm_msi_remove(struct brcm_pcie *pcie)
{
	struct brcm_msi *msi = pcie->msi;

	if (!msi)
		return;
	irq_set_chained_handler_and_data(msi->irq, NULL, NULL);
	brcm_free_domains(msi);
}

static void brcm_msi_set_regs(struct brcm_msi *msi)
{
	u32 val = msi->legacy ? BRCM_INT_PCI_MSI_LEGACY_MASK :
				BRCM_INT_PCI_MSI_MASK;

	writel(val, msi->intr_base + MSI_INT_MASK_CLR);
	writel(val, msi->intr_base + MSI_INT_CLR);

	/*
	 * The 0 bit of PCIE_MISC_MSI_BAR_CONFIG_LO is repurposed to MSI
	 * enable, which we set to 1.
	 */
	writel(lower_32_bits(msi->target_addr) | 0x1,
	       msi->base + PCIE_MISC_MSI_BAR_CONFIG_LO);
	writel(upper_32_bits(msi->target_addr),
	       msi->base + PCIE_MISC_MSI_BAR_CONFIG_HI);

	val = msi->legacy ? PCIE_MISC_MSI_DATA_CONFIG_VAL_8 : PCIE_MISC_MSI_DATA_CONFIG_VAL_32;
	writel(val, msi->base + PCIE_MISC_MSI_DATA_CONFIG);
}

static int brcm_pcie_enable_msi(struct brcm_pcie *pcie)
{
	struct brcm_msi *msi;
	int irq, ret;
	struct device *dev = pcie->dev;

	irq = irq_of_parse_and_map(dev->of_node, 1);
	if (irq <= 0) {
		dev_err(dev, "cannot map MSI interrupt\n");
		return -ENODEV;
	}

	msi = devm_kzalloc(dev, sizeof(struct brcm_msi), GFP_KERNEL);
	if (!msi)
		return -ENOMEM;

	mutex_init(&msi->lock);
	msi->dev = dev;
	msi->base = pcie->base;
	msi->np = pcie->np;
	msi->target_addr = pcie->msi_target_addr;
	msi->irq = irq;
	msi->legacy = pcie->hw_rev < BRCM_PCIE_HW_REV_33;

	/*
	 * Sanity check to make sure that the 'used' bitmap in struct brcm_msi
	 * is large enough.
	 */
	BUILD_BUG_ON(BRCM_INT_PCI_MSI_LEGACY_NR > BRCM_INT_PCI_MSI_NR);

	if (msi->legacy) {
		msi->intr_base = msi->base + INTR2_CPU_BASE(pcie);
		msi->nr = BRCM_INT_PCI_MSI_LEGACY_NR;
		msi->legacy_shift = 24;
	} else {
		msi->intr_base = msi->base + PCIE_MSI_INTR2_BASE;
		msi->nr = BRCM_INT_PCI_MSI_NR;
		msi->legacy_shift = 0;
	}

	ret = brcm_allocate_domains(msi);
	if (ret)
		return ret;

	irq_set_chained_handler_and_data(msi->irq, brcm_pcie_msi_isr, msi);

	brcm_msi_set_regs(msi);
	pcie->msi = msi;

	return 0;
}

/* The controller is capable of serving in both RC and EP roles */
static bool brcm_pcie_rc_mode(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	u32 val = readl(base + PCIE_MISC_PCIE_STATUS);

	return !!FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_PORT_MASK, val);
}

static bool brcm_pcie_link_up(struct brcm_pcie *pcie)
{
	u32 val = readl(pcie->base + PCIE_MISC_PCIE_STATUS);
	u32 dla = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_MASK, val);
	u32 plu = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_MASK, val);

	return dla && plu;
}

static void __iomem *brcm_pcie_map_bus(struct pci_bus *bus,
				       unsigned int devfn, int where)
{
	struct brcm_pcie *pcie = bus->sysdata;
	void __iomem *base = pcie->base;
	int idx;

	/* Accesses to the RC go right to the RC registers if !devfn */
	if (pci_is_root_bus(bus))
		return devfn ? NULL : base + PCIE_ECAM_REG(where);

	/* An access to our HW w/o link-up will cause a CPU Abort */
	if (!brcm_pcie_link_up(pcie))
		return NULL;

	/* For devices, write to the config space index register */
	idx = PCIE_ECAM_OFFSET(bus->number, devfn, 0);
	writel(idx, base + IDX_ADDR(pcie));
	return base + DATA_ADDR(pcie) + PCIE_ECAM_REG(where);
}

static void __iomem *brcm7425_pcie_map_bus(struct pci_bus *bus,
					   unsigned int devfn, int where)
{
	struct brcm_pcie *pcie = bus->sysdata;
	void __iomem *base = pcie->base;
	int idx;

	/* Accesses to the RC go right to the RC registers if !devfn */
	if (pci_is_root_bus(bus))
		return devfn ? NULL : base + PCIE_ECAM_REG(where);

	/* An access to our HW w/o link-up will cause a CPU Abort */
	if (!brcm_pcie_link_up(pcie))
		return NULL;

	/* For devices, write to the config space index register */
	idx = PCIE_ECAM_OFFSET(bus->number, devfn, where);
	writel(idx, base + IDX_ADDR(pcie));
	return base + DATA_ADDR(pcie);
}

static int brcm_pcie_bridge_sw_init_set_generic(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp, mask = RGR1_SW_INIT_1_INIT_GENERIC_MASK;
	u32 shift = RGR1_SW_INIT_1_INIT_GENERIC_SHIFT;
	int ret = 0;

	if (pcie->bridge_reset) {
		if (val)
			ret = reset_control_assert(pcie->bridge_reset);
		else
			ret = reset_control_deassert(pcie->bridge_reset);

		if (ret)
			dev_err(pcie->dev, "failed to %s 'bridge' reset, err=%d\n",
				val ? "assert" : "deassert", ret);

		return ret;
	}

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1(pcie));
	tmp = (tmp & ~mask) | ((val << shift) & mask);
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1(pcie));

	return ret;
}

static int brcm_pcie_bridge_sw_init_set_7278(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp, mask =  RGR1_SW_INIT_1_INIT_7278_MASK;
	u32 shift = RGR1_SW_INIT_1_INIT_7278_SHIFT;

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1(pcie));
	tmp = (tmp & ~mask) | ((val << shift) & mask);
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1(pcie));

	return 0;
}

static int brcm_pcie_perst_set_4908(struct brcm_pcie *pcie, u32 val)
{
	int ret;

	if (WARN_ONCE(!pcie->perst_reset, "missing PERST# reset controller\n"))
		return -EINVAL;

	if (val)
		ret = reset_control_assert(pcie->perst_reset);
	else
		ret = reset_control_deassert(pcie->perst_reset);

	if (ret)
		dev_err(pcie->dev, "failed to %s 'perst' reset, err=%d\n",
			val ? "assert" : "deassert", ret);
	return ret;
}

static int brcm_pcie_perst_set_7278(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp;

	/* Perst bit has moved and assert value is 0 */
	tmp = readl(pcie->base + PCIE_MISC_PCIE_CTRL);
	u32p_replace_bits(&tmp, !val, PCIE_MISC_PCIE_CTRL_PCIE_PERSTB_MASK);
	writel(tmp, pcie->base +  PCIE_MISC_PCIE_CTRL);

	return 0;
}

static int brcm_pcie_perst_set_generic(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp;

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1(pcie));
	u32p_replace_bits(&tmp, val, PCIE_RGR1_SW_INIT_1_PERST_MASK);
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1(pcie));

	return 0;
}

static int brcm_pcie_post_setup_bcm2712(struct brcm_pcie *pcie)
{
	static const u16 data[] = { 0x50b9, 0xbda1, 0x0094, 0x97b4, 0x5030,
				    0x5030, 0x0007 };
	static const u8 regs[] = { 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0x1e };
	int ret, i;
	u32 tmp;

	/* Allow a 54MHz (xosc) refclk source */
	ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, SET_ADDR_OFFSET, 0x1600);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, regs[i], data[i]);
		if (ret < 0)
			return ret;
	}

	usleep_range(100, 200);

	/*
	 * Set L1SS sub-state timers to avoid lengthy state transitions,
	 * PM clock period is 18.52ns (1/54MHz, round down).
	 */
	tmp = readl(pcie->base + PCIE_RC_PL_PHY_CTL_15);
	tmp &= ~PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK;
	tmp |= 0x12;
	writel(tmp, pcie->base + PCIE_RC_PL_PHY_CTL_15);

	return 0;
}

static void add_inbound_win(struct inbound_win *b, u8 *count, u64 size,
			    u64 cpu_addr, u64 pci_offset)
{
	b->size = size;
	b->cpu_addr = cpu_addr;
	b->pci_offset = pci_offset;
	(*count)++;
}

static int brcm_pcie_get_inbound_wins(struct brcm_pcie *pcie,
				      struct inbound_win inbound_wins[])
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);
	u64 pci_offset, cpu_addr, size = 0, tot_size = 0;
	struct resource_entry *entry;
	struct device *dev = pcie->dev;
	u64 lowest_pcie_addr = ~(u64)0;
	int ret, i = 0;
	u8 n = 0;

	/*
	 * The HW registers (and PCIe) use order-1 numbering for BARs.  As such,
	 * we have inbound_wins[0] unused and BAR1 starts at inbound_wins[1].
	 */
	struct inbound_win *b_begin = &inbound_wins[1];
	struct inbound_win *b = b_begin;

	/*
	 * STB chips beside 7712 disable the first inbound window default.
	 * Rather being mapped to system memory it is mapped to the
	 * internal registers of the SoC.  This feature is deprecated, has
	 * security considerations, and is not implemented in our modern
	 * SoCs.
	 */
	if (pcie->cfg->soc_base != BCM7712)
		add_inbound_win(b++, &n, 0, 0, 0);

	resource_list_for_each_entry(entry, &bridge->dma_ranges) {
		u64 pcie_start = entry->res->start - entry->offset;
		u64 cpu_start = entry->res->start;

		size = resource_size(entry->res);
		tot_size += size;
		if (pcie_start < lowest_pcie_addr)
			lowest_pcie_addr = pcie_start;
		/*
		 * 7712 and newer chips may have many BARs, with each
		 * offering a non-overlapping viewport to system memory.
		 * That being said, each BARs size must still be a power of
		 * two.
		 */
		if (pcie->cfg->soc_base == BCM7712)
			add_inbound_win(b++, &n, size, cpu_start, pcie_start);

		if (n > pcie->cfg->num_inbound_wins)
			break;
	}

	if (lowest_pcie_addr == ~(u64)0) {
		dev_err(dev, "DT node has no dma-ranges\n");
		return -EINVAL;
	}

	/*
	 * 7712 and newer chips do not have an internal memory mapping system
	 * that enables multiple memory controllers.  As such, it can return
	 * now w/o doing special configuration.
	 */
	if (pcie->cfg->soc_base == BCM7712)
		return n;

	ret = of_property_read_variable_u64_array(pcie->np, "brcm,scb-sizes", pcie->memc_size, 1,
						  PCIE_BRCM_MAX_MEMC);
	if (ret <= 0) {
		/* Make an educated guess */
		pcie->num_memc = 1;
		pcie->memc_size[0] = 1ULL << fls64(tot_size - 1);
	} else {
		pcie->num_memc = ret;
	}

	/* Each memc is viewed through a "port" that is a power of 2 */
	for (i = 0, size = 0; i < pcie->num_memc; i++)
		size += pcie->memc_size[i];

	/* Our HW mandates that the window size must be a power of 2 */
	size = 1ULL << fls64(size - 1);

	/*
	 * For STB chips, the BAR2 cpu_addr is hardwired to the start
	 * of system memory, so we set it to 0.
	 */
	cpu_addr = 0;
	pci_offset = lowest_pcie_addr;

	/*
	 * We validate the inbound memory view even though we should trust
	 * whatever the device-tree provides. This is because of an HW issue on
	 * early Raspberry Pi 4's revisions (bcm2711). It turns out its
	 * firmware has to dynamically edit dma-ranges due to a bug on the
	 * PCIe controller integration, which prohibits any access above the
	 * lower 3GB of memory. Given this, we decided to keep the dma-ranges
	 * in check, avoiding hard to debug device-tree related issues in the
	 * future:
	 *
	 * The PCIe host controller by design must set the inbound viewport to
	 * be a contiguous arrangement of all of the system's memory.  In
	 * addition, its size must be a power of two.  To further complicate
	 * matters, the viewport must start on a pcie-address that is aligned
	 * on a multiple of its size.  If a portion of the viewport does not
	 * represent system memory -- e.g. 3GB of memory requires a 4GB
	 * viewport -- we can map the outbound memory in or after 3GB and even
	 * though the viewport will overlap the outbound memory the controller
	 * will know to send outbound memory downstream and everything else
	 * upstream.
	 *
	 * For example:
	 *
	 * - The best-case scenario, memory up to 3GB, is to place the inbound
	 *   region in the first 4GB of pcie-space, as some legacy devices can
	 *   only address 32bits. We would also like to put the MSI under 4GB
	 *   as well, since some devices require a 32bit MSI target address.
	 *
	 * - If the system memory is 4GB or larger we cannot start the inbound
	 *   region at location 0 (since we have to allow some space for
	 *   outbound memory @ 3GB). So instead it will  start at the 1x
	 *   multiple of its size
	 */
	if (!size || (pci_offset & (size - 1)) ||
	    (pci_offset < SZ_4G && pci_offset > SZ_2G)) {
		dev_err(dev, "Invalid inbound_win2_offset/size: size 0x%llx, off 0x%llx\n",
			size, pci_offset);
		return -EINVAL;
	}

	/* Enable inbound window 2, the main inbound window for STB chips */
	add_inbound_win(b++, &n, size, cpu_addr, pci_offset);

	/*
	 * Disable inbound window 3.  On some chips presents the same
	 * window as #2 but the data appears in a settable endianness.
	 */
	add_inbound_win(b++, &n, 0, 0, 0);

	return n;
}

static u32 brcm_bar_reg_offset(int bar)
{
	if (bar <= 3)
		return PCIE_MISC_RC_BAR1_CONFIG_LO + 8 * (bar - 1);
	else
		return PCIE_MISC_RC_BAR4_CONFIG_LO + 8 * (bar - 4);
}

static u32 brcm_ubus_reg_offset(int bar)
{
	if (bar <= 3)
		return PCIE_MISC_UBUS_BAR1_CONFIG_REMAP + 8 * (bar - 1);
	else
		return PCIE_MISC_UBUS_BAR4_CONFIG_REMAP + 8 * (bar - 4);
}

static void set_inbound_win_registers(struct brcm_pcie *pcie,
				      const struct inbound_win *inbound_wins,
				      u8 num_inbound_wins)
{
	void __iomem *base = pcie->base;
	int i;

	for (i = 1; i <= num_inbound_wins; i++) {
		u64 pci_offset = inbound_wins[i].pci_offset;
		u64 cpu_addr = inbound_wins[i].cpu_addr;
		u64 size = inbound_wins[i].size;
		u32 reg_offset = brcm_bar_reg_offset(i);
		u32 tmp = lower_32_bits(pci_offset);

		u32p_replace_bits(&tmp, brcm_pcie_encode_ibar_size(size),
				  PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK);

		/* Write low */
		writel_relaxed(tmp, base + reg_offset);
		/* Write high */
		writel_relaxed(upper_32_bits(pci_offset), base + reg_offset + 4);

		/*
		 * Most STB chips:
		 *     Do nothing.
		 * 7712:
		 *     All of their BARs need to be set.
		 */
		if (pcie->cfg->soc_base == BCM7712) {
			/* BUS remap register settings */
			reg_offset = brcm_ubus_reg_offset(i);
			tmp = lower_32_bits(cpu_addr) & ~0xfff;
			tmp |= PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_ACCESS_EN_MASK;
			writel_relaxed(tmp, base + reg_offset);
			tmp = upper_32_bits(cpu_addr);
			writel_relaxed(tmp, base + reg_offset + 4);
		}
	}
}

static int brcm_pcie_setup(struct brcm_pcie *pcie)
{
	struct inbound_win inbound_wins[PCIE_BRCM_MAX_INBOUND_WINS];
	void __iomem *base = pcie->base;
	struct pci_host_bridge *bridge;
	struct resource_entry *entry;
	u32 tmp, burst, aspm_support, num_lanes, num_lanes_cap;
	u8 num_out_wins = 0;
	int num_inbound_wins = 0;
	int memc, ret;

	/* Reset the bridge */
	ret = pcie->cfg->bridge_sw_init_set(pcie, 1);
	if (ret)
		return ret;

	/* Ensure that PERST# is asserted; some bootloaders may deassert it. */
	if (pcie->cfg->soc_base == BCM2711) {
		ret = pcie->cfg->perst_set(pcie, 1);
		if (ret) {
			pcie->cfg->bridge_sw_init_set(pcie, 0);
			return ret;
		}
	}

	usleep_range(100, 200);

	/* Take the bridge out of reset */
	ret = pcie->cfg->bridge_sw_init_set(pcie, 0);
	if (ret)
		return ret;

	tmp = readl(base + HARD_DEBUG(pcie));
	if (is_bmips(pcie))
		tmp &= ~PCIE_BMIPS_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK;
	else
		tmp &= ~PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK;
	writel(tmp, base + HARD_DEBUG(pcie));
	/* Wait for SerDes to be stable */
	usleep_range(100, 200);

	/*
	 * SCB_MAX_BURST_SIZE is a two bit field.  For GENERIC chips it
	 * is encoded as 0=128, 1=256, 2=512, 3=Rsvd, for BCM7278 it
	 * is encoded as 0=Rsvd, 1=128, 2=256, 3=512.
	 */
	if (is_bmips(pcie))
		burst = 0x1; /* 256 bytes */
	else if (pcie->cfg->soc_base == BCM2711)
		burst = 0x0; /* 128 bytes */
	else if (pcie->cfg->soc_base == BCM7278)
		burst = 0x3; /* 512 bytes */
	else
		burst = 0x2; /* 512 bytes */

	/*
	 * Set SCB_MAX_BURST_SIZE, CFG_READ_UR_MODE, SCB_ACCESS_EN,
	 * RCB_MPS_MODE, RCB_64B_MODE
	 */
	tmp = readl(base + PCIE_MISC_MISC_CTRL);
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK);
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_MASK);
	u32p_replace_bits(&tmp, burst, PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK);
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_PCIE_RCB_MPS_MODE_MASK);
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_PCIE_RCB_64B_MODE_MASK);
	writel(tmp, base + PCIE_MISC_MISC_CTRL);

	num_inbound_wins = brcm_pcie_get_inbound_wins(pcie, inbound_wins);
	if (num_inbound_wins < 0)
		return num_inbound_wins;

	set_inbound_win_registers(pcie, inbound_wins, num_inbound_wins);

	if (!brcm_pcie_rc_mode(pcie)) {
		dev_err(pcie->dev, "PCIe RC controller misconfigured as Endpoint\n");
		return -EINVAL;
	}

	tmp = readl(base + PCIE_MISC_MISC_CTRL);
	for (memc = 0; memc < pcie->num_memc; memc++) {
		u32 scb_size_val = ilog2(pcie->memc_size[memc]) - 15;

		if (memc == 0)
			u32p_replace_bits(&tmp, scb_size_val, SCB_SIZE_MASK(0));
		else if (memc == 1)
			u32p_replace_bits(&tmp, scb_size_val, SCB_SIZE_MASK(1));
		else if (memc == 2)
			u32p_replace_bits(&tmp, scb_size_val, SCB_SIZE_MASK(2));
	}
	writel(tmp, base + PCIE_MISC_MISC_CTRL);

	/*
	 * We ideally want the MSI target address to be located in the 32bit
	 * addressable memory area. Some devices might depend on it. This is
	 * possible either when the inbound window is located above the lower
	 * 4GB or when the inbound area is smaller than 4GB (taking into
	 * account the rounding-up we're forced to perform).
	 */
	if (inbound_wins[2].pci_offset >= SZ_4G ||
	    (inbound_wins[2].size + inbound_wins[2].pci_offset) < SZ_4G)
		pcie->msi_target_addr = BRCM_MSI_TARGET_ADDR_LT_4GB;
	else
		pcie->msi_target_addr = BRCM_MSI_TARGET_ADDR_GT_4GB;


	/* Don't advertise L0s capability if 'aspm-no-l0s' */
	aspm_support = PCIE_LINK_STATE_L1;
	if (!of_property_read_bool(pcie->np, "aspm-no-l0s"))
		aspm_support |= PCIE_LINK_STATE_L0S;
	tmp = readl(base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY);
	u32p_replace_bits(&tmp, aspm_support,
		PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_ASPM_SUPPORT_MASK);
	writel(tmp, base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY);

	/* 'tmp' still holds the contents of PRIV1_LINK_CAPABILITY */
	num_lanes_cap = u32_get_bits(tmp, PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_MAX_LINK_WIDTH_MASK);
	num_lanes = 0;

	/*
	 * Use hardware negotiated Max Link Width value by default.  If the
	 * "num-lanes" DT property is present, assume that the chip's default
	 * link width capability information is incorrect/undesired and use the
	 * specified value instead.
	 */
	if (!of_property_read_u32(pcie->np, "num-lanes", &num_lanes) &&
	    num_lanes && num_lanes <= 4 && num_lanes_cap != num_lanes) {
		u32p_replace_bits(&tmp, num_lanes,
			PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_MAX_LINK_WIDTH_MASK);
		writel(tmp, base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY);
		tmp = readl(base + PCIE_RC_PL_REG_PHY_CTL_1);
		u32p_replace_bits(&tmp, 1,
			PCIE_RC_PL_REG_PHY_CTL_1_REG_P2_POWERDOWN_ENA_NOSYNC_MASK);
		writel(tmp, base + PCIE_RC_PL_REG_PHY_CTL_1);
	}

	/*
	 * For config space accesses on the RC, show the right class for
	 * a PCIe-PCIe bridge (the default setting is to be EP mode).
	 */
	tmp = readl(base + PCIE_RC_CFG_PRIV1_ID_VAL3);
	u32p_replace_bits(&tmp, 0x060400,
			  PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK);
	writel(tmp, base + PCIE_RC_CFG_PRIV1_ID_VAL3);

	bridge = pci_host_bridge_from_priv(pcie);
	resource_list_for_each_entry(entry, &bridge->windows) {
		struct resource *res = entry->res;

		if (resource_type(res) != IORESOURCE_MEM)
			continue;

		if (num_out_wins >= BRCM_NUM_PCIE_OUT_WINS) {
			dev_err(pcie->dev, "too many outbound wins\n");
			return -EINVAL;
		}

		if (is_bmips(pcie)) {
			u64 start = res->start;
			unsigned int j, nwins = resource_size(res) / SZ_128M;

			/* bmips PCIe outbound windows have a 128MB max size */
			if (nwins > BRCM_NUM_PCIE_OUT_WINS)
				nwins = BRCM_NUM_PCIE_OUT_WINS;
			for (j = 0; j < nwins; j++, start += SZ_128M)
				brcm_pcie_set_outbound_win(pcie, j, start,
							   start - entry->offset,
							   SZ_128M);
			break;
		}
		brcm_pcie_set_outbound_win(pcie, num_out_wins, res->start,
					   res->start - entry->offset,
					   resource_size(res));
		num_out_wins++;
	}

	/* PCIe->SCB endian mode for inbound window */
	tmp = readl(base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1);
	u32p_replace_bits(&tmp, PCIE_RC_CFG_VENDOR_SPECIFIC_REG1_LITTLE_ENDIAN,
		PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK);
	writel(tmp, base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1);

	if (pcie->cfg->post_setup) {
		ret = pcie->cfg->post_setup(pcie);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * This extends the timeout period for an access to an internal bus.  This
 * access timeout may occur during L1SS sleep periods, even without the
 * presence of a PCIe access.
 */
static void brcm_extend_rbus_timeout(struct brcm_pcie *pcie)
{
	/* TIMEOUT register is two registers before RGR1_SW_INIT_1 */
	const unsigned int REG_OFFSET = PCIE_RGR1_SW_INIT_1(pcie) - 8;
	u32 timeout_us = 4000000; /* 4 seconds, our setting for L1SS */

	/* 7712 does not have this (RGR1) timer */
	if (pcie->cfg->soc_base == BCM7712)
		return;

	/* Each unit in timeout register is 1/216,000,000 seconds */
	writel(216 * timeout_us, pcie->base + REG_OFFSET);
}

static void brcm_config_clkreq(struct brcm_pcie *pcie)
{
	static const char err_msg[] = "invalid 'brcm,clkreq-mode' DT string\n";
	const char *mode = "default";
	u32 clkreq_cntl;
	int ret, tmp;

	ret = of_property_read_string(pcie->np, "brcm,clkreq-mode", &mode);
	if (ret && ret != -EINVAL) {
		dev_err(pcie->dev, err_msg);
		mode = "safe";
	}

	/* Start out assuming safe mode (both mode bits cleared) */
	clkreq_cntl = readl(pcie->base + HARD_DEBUG(pcie));
	clkreq_cntl &= ~PCIE_CLKREQ_MASK;

	if (strcmp(mode, "no-l1ss") == 0) {
		/*
		 * "no-l1ss" -- Provides Clock Power Management, L0s, and
		 * L1, but cannot provide L1 substate (L1SS) power
		 * savings. If the downstream device connected to the RC is
		 * L1SS capable AND the OS enables L1SS, all PCIe traffic
		 * may abruptly halt, potentially hanging the system.
		 */
		clkreq_cntl |= PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK;
		/*
		 * We want to un-advertise L1 substates because if the OS
		 * tries to configure the controller into using L1 substate
		 * power savings it may fail or hang when the RC HW is in
		 * "no-l1ss" mode.
		 */
		tmp = readl(pcie->base + PCIE_RC_CFG_PRIV1_ROOT_CAP);
		u32p_replace_bits(&tmp, 2, PCIE_RC_CFG_PRIV1_ROOT_CAP_L1SS_MODE_MASK);
		writel(tmp, pcie->base + PCIE_RC_CFG_PRIV1_ROOT_CAP);

	} else if (strcmp(mode, "default") == 0) {
		/*
		 * "default" -- Provides L0s, L1, and L1SS, but not
		 * compliant to provide Clock Power Management;
		 * specifically, may not be able to meet the Tclron max
		 * timing of 400ns as specified in "Dynamic Clock Control",
		 * section 3.2.5.2.2 of the PCIe spec.  This situation is
		 * atypical and should happen only with older devices.
		 */
		clkreq_cntl |= PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK;
		brcm_extend_rbus_timeout(pcie);

	} else {
		/*
		 * "safe" -- No power savings; refclk is driven by RC
		 * unconditionally.
		 */
		if (strcmp(mode, "safe") != 0)
			dev_err(pcie->dev, err_msg);
		mode = "safe";
	}
	writel(clkreq_cntl, pcie->base + HARD_DEBUG(pcie));

	dev_info(pcie->dev, "clkreq-mode set to %s\n", mode);
}

static int brcm_pcie_start_link(struct brcm_pcie *pcie)
{
	struct device *dev = pcie->dev;
	void __iomem *base = pcie->base;
	u16 nlw, cls, lnksta;
	bool ssc_good = false;
	int ret, i;

	/* Limit the generation if specified */
	if (pcie->gen)
		brcm_pcie_set_gen(pcie, pcie->gen);

	/* Unassert the fundamental reset */
	ret = pcie->cfg->perst_set(pcie, 0);
	if (ret)
		return ret;

	msleep(PCIE_RESET_CONFIG_WAIT_MS);

	/*
	 * Give the RC/EP even more time to wake up, before trying to
	 * configure RC.  Intermittently check status for link-up, up to a
	 * total of 100ms.
	 */
	for (i = 0; i < 100 && !brcm_pcie_link_up(pcie); i += 5)
		msleep(5);

	if (!brcm_pcie_link_up(pcie)) {
		dev_err(dev, "link down\n");
		return -ENODEV;
	}

	brcm_config_clkreq(pcie);

	if (pcie->ssc) {
		ret = brcm_pcie_set_ssc(pcie);
		if (ret == 0)
			ssc_good = true;
		else
			dev_err(dev, "failed attempt to enter ssc mode\n");
	}

	lnksta = readw(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKSTA);
	cls = FIELD_GET(PCI_EXP_LNKSTA_CLS, lnksta);
	nlw = FIELD_GET(PCI_EXP_LNKSTA_NLW, lnksta);
	dev_info(dev, "link up, %s x%u %s\n",
		 pci_speed_string(pcie_link_speed[cls]), nlw,
		 ssc_good ? "(SSC)" : "(!SSC)");

	return 0;
}

static const char * const supplies[] = {
	"vpcie3v3",
	"vpcie3v3aux",
	"vpcie12v",
};

static void *alloc_subdev_regulators(struct device *dev)
{
	const size_t size = sizeof(struct subdev_regulators) +
		sizeof(struct regulator_bulk_data) * ARRAY_SIZE(supplies);
	struct subdev_regulators *sr;
	int i;

	sr = devm_kzalloc(dev, size, GFP_KERNEL);
	if (sr) {
		sr->num_supplies = ARRAY_SIZE(supplies);
		for (i = 0; i < ARRAY_SIZE(supplies); i++)
			sr->supplies[i].supply = supplies[i];
	}

	return sr;
}

static int brcm_pcie_add_bus(struct pci_bus *bus)
{
	struct brcm_pcie *pcie = bus->sysdata;
	struct device *dev = &bus->dev;
	struct subdev_regulators *sr;
	int ret;

	if (!bus->parent || !pci_is_root_bus(bus->parent))
		return 0;

	if (dev->of_node) {
		sr = alloc_subdev_regulators(dev);
		if (!sr) {
			dev_info(dev, "Can't allocate regulators for downstream device\n");
			goto no_regulators;
		}

		pcie->sr = sr;

		ret = regulator_bulk_get(dev, sr->num_supplies, sr->supplies);
		if (ret) {
			dev_info(dev, "Did not get regulators, err=%d\n", ret);
			pcie->sr = NULL;
			goto no_regulators;
		}

		ret = regulator_bulk_enable(sr->num_supplies, sr->supplies);
		if (ret) {
			dev_err(dev, "Can't enable regulators for downstream device\n");
			regulator_bulk_free(sr->num_supplies, sr->supplies);
			pcie->sr = NULL;
		}
	}

no_regulators:
	brcm_pcie_start_link(pcie);
	return 0;
}

static void brcm_pcie_remove_bus(struct pci_bus *bus)
{
	struct brcm_pcie *pcie = bus->sysdata;
	struct subdev_regulators *sr = pcie->sr;
	struct device *dev = &bus->dev;

	if (!sr || !bus->parent || !pci_is_root_bus(bus->parent))
		return;

	if (regulator_bulk_disable(sr->num_supplies, sr->supplies))
		dev_err(dev, "Failed to disable regulators for downstream device\n");
	regulator_bulk_free(sr->num_supplies, sr->supplies);
	pcie->sr = NULL;
}

/* L23 is a low-power PCIe link state */
static void brcm_pcie_enter_l23(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	int l23, i;
	u32 tmp;

	/* Assert request for L23 */
	tmp = readl(base + PCIE_MISC_PCIE_CTRL);
	u32p_replace_bits(&tmp, 1, PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_MASK);
	writel(tmp, base + PCIE_MISC_PCIE_CTRL);

	/* Wait up to 36 msec for L23 */
	tmp = readl(base + PCIE_MISC_PCIE_STATUS);
	l23 = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK, tmp);
	for (i = 0; i < 15 && !l23; i++) {
		usleep_range(2000, 2400);
		tmp = readl(base + PCIE_MISC_PCIE_STATUS);
		l23 = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK,
				tmp);
	}

	if (!l23)
		dev_err(pcie->dev, "failed to enter low-power link state\n");
}

static int brcm_phy_cntl(struct brcm_pcie *pcie, const int start)
{
	static const u32 shifts[PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS] = {
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_SHIFT,
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_SHIFT,
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_SHIFT,};
	static const u32 masks[PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS] = {
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_MASK,
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_MASK,
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_MASK,};
	const int beg = start ? 0 : PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS - 1;
	const int end = start ? PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS : -1;
	u32 tmp, combined_mask = 0;
	u32 val;
	void __iomem *base = pcie->base;
	int i, ret;

	for (i = beg; i != end; start ? i++ : i--) {
		val = start ? BIT_MASK(shifts[i]) : 0;
		tmp = readl(base + PCIE_DVT_PMU_PCIE_PHY_CTRL);
		tmp = (tmp & ~masks[i]) | (val & masks[i]);
		writel(tmp, base + PCIE_DVT_PMU_PCIE_PHY_CTRL);
		usleep_range(50, 200);
		combined_mask |= masks[i];
	}

	tmp = readl(base + PCIE_DVT_PMU_PCIE_PHY_CTRL);
	val = start ? combined_mask : 0;

	ret = (tmp & combined_mask) == val ? 0 : -EIO;
	if (ret)
		dev_err(pcie->dev, "failed to %s phy\n", (start ? "start" : "stop"));

	return ret;
}

static inline int brcm_phy_start(struct brcm_pcie *pcie)
{
	return pcie->cfg->has_phy ? brcm_phy_cntl(pcie, 1) : 0;
}

static inline int brcm_phy_stop(struct brcm_pcie *pcie)
{
	return pcie->cfg->has_phy ? brcm_phy_cntl(pcie, 0) : 0;
}

static int brcm_pcie_turn_off(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	int tmp, ret;

	if (brcm_pcie_link_up(pcie))
		brcm_pcie_enter_l23(pcie);
	/* Assert fundamental reset */
	ret = pcie->cfg->perst_set(pcie, 1);
	if (ret)
		return ret;

	/* Deassert request for L23 in case it was asserted */
	tmp = readl(base + PCIE_MISC_PCIE_CTRL);
	u32p_replace_bits(&tmp, 0, PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_MASK);
	writel(tmp, base + PCIE_MISC_PCIE_CTRL);

	/* Turn off SerDes */
	tmp = readl(base + HARD_DEBUG(pcie));
	u32p_replace_bits(&tmp, 1, PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK);
	writel(tmp, base + HARD_DEBUG(pcie));

	if (!(pcie->cfg->quirks & CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN))
		/* Shutdown PCIe bridge */
		ret = pcie->cfg->bridge_sw_init_set(pcie, 1);

	return ret;
}

static int pci_dev_may_wakeup(struct pci_dev *dev, void *data)
{
	bool *ret = data;

	if (device_may_wakeup(&dev->dev)) {
		*ret = true;
		dev_info(&dev->dev, "Possible wake-up device; regulators will not be disabled\n");
	}
	return (int) *ret;
}

static int brcm_pcie_suspend_noirq(struct device *dev)
{
	struct brcm_pcie *pcie = dev_get_drvdata(dev);
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);
	int ret, rret;

	ret = brcm_pcie_turn_off(pcie);
	if (ret)
		return ret;

	/*
	 * If brcm_phy_stop() returns an error, just dev_err(). If we
	 * return the error it will cause the suspend to fail and this is a
	 * forgivable offense that will probably be erased on resume.
	 */
	if (brcm_phy_stop(pcie))
		dev_err(dev, "Could not stop phy for suspend\n");

	ret = reset_control_rearm(pcie->rescal);
	if (ret) {
		dev_err(dev, "Could not rearm rescal reset\n");
		return ret;
	}

	if (pcie->sr) {
		/*
		 * Now turn off the regulators, but if at least one
		 * downstream device is enabled as a wake-up source, do not
		 * turn off regulators.
		 */
		pcie->ep_wakeup_capable = false;
		pci_walk_bus(bridge->bus, pci_dev_may_wakeup,
			     &pcie->ep_wakeup_capable);
		if (!pcie->ep_wakeup_capable) {
			ret = regulator_bulk_disable(pcie->sr->num_supplies,
						     pcie->sr->supplies);
			if (ret) {
				dev_err(dev, "Could not turn off regulators\n");
				rret = reset_control_reset(pcie->rescal);
				if (rret)
					dev_err(dev, "failed to reset 'rascal' controller ret=%d\n",
						rret);
				return ret;
			}
		}
	}
	clk_disable_unprepare(pcie->clk);

	return 0;
}

static int brcm_pcie_resume_noirq(struct device *dev)
{
	struct brcm_pcie *pcie = dev_get_drvdata(dev);
	void __iomem *base;
	u32 tmp;
	int ret, rret;

	base = pcie->base;
	ret = clk_prepare_enable(pcie->clk);
	if (ret)
		return ret;

	ret = reset_control_reset(pcie->rescal);
	if (ret)
		goto err_disable_clk;

	ret = brcm_phy_start(pcie);
	if (ret)
		goto err_reset;

	/* Take bridge out of reset so we can access the SERDES reg */
	pcie->cfg->bridge_sw_init_set(pcie, 0);

	/* SERDES_IDDQ = 0 */
	tmp = readl(base + HARD_DEBUG(pcie));
	u32p_replace_bits(&tmp, 0, PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK);
	writel(tmp, base + HARD_DEBUG(pcie));

	/* wait for serdes to be stable */
	udelay(100);

	ret = brcm_pcie_setup(pcie);
	if (ret)
		goto err_reset;

	if (pcie->sr) {
		if (pcie->ep_wakeup_capable) {
			/*
			 * We are resuming from a suspend.  In the suspend we
			 * did not disable the power supplies, so there is
			 * no need to enable them (and falsely increase their
			 * usage count).
			 */
			pcie->ep_wakeup_capable = false;
		} else {
			ret = regulator_bulk_enable(pcie->sr->num_supplies,
						    pcie->sr->supplies);
			if (ret) {
				dev_err(dev, "Could not turn on regulators\n");
				goto err_reset;
			}
		}
	}

	ret = brcm_pcie_start_link(pcie);
	if (ret)
		goto err_regulator;

	if (pcie->msi)
		brcm_msi_set_regs(pcie->msi);

	return 0;

err_regulator:
	if (pcie->sr)
		regulator_bulk_disable(pcie->sr->num_supplies, pcie->sr->supplies);
err_reset:
	rret = reset_control_rearm(pcie->rescal);
	if (rret)
		dev_err(pcie->dev, "failed to rearm 'rescal' reset, err=%d\n", rret);
err_disable_clk:
	clk_disable_unprepare(pcie->clk);
	return ret;
}

static void __brcm_pcie_remove(struct brcm_pcie *pcie)
{
	brcm_msi_remove(pcie);
	brcm_pcie_turn_off(pcie);
	if (brcm_phy_stop(pcie))
		dev_err(pcie->dev, "Could not stop phy\n");
	if (reset_control_rearm(pcie->rescal))
		dev_err(pcie->dev, "Could not rearm rescal reset\n");
	clk_disable_unprepare(pcie->clk);
}

static void brcm_pcie_remove(struct platform_device *pdev)
{
	struct brcm_pcie *pcie = platform_get_drvdata(pdev);
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);

	pci_stop_root_bus(bridge->bus);
	pci_remove_root_bus(bridge->bus);
	__brcm_pcie_remove(pcie);
}

static const int pcie_offsets[] = {
	[RGR1_SW_INIT_1]	= 0x9210,
	[EXT_CFG_INDEX]		= 0x9000,
	[EXT_CFG_DATA]		= 0x8000,
	[PCIE_HARD_DEBUG]	= 0x4204,
	[PCIE_INTR2_CPU_BASE]	= 0x4300,
};

static const int pcie_offsets_bcm7278[] = {
	[RGR1_SW_INIT_1]	= 0xc010,
	[EXT_CFG_INDEX]		= 0x9000,
	[EXT_CFG_DATA]		= 0x8000,
	[PCIE_HARD_DEBUG]	= 0x4204,
	[PCIE_INTR2_CPU_BASE]	= 0x4300,
};

static const int pcie_offsets_bcm7425[] = {
	[RGR1_SW_INIT_1]	= 0x8010,
	[EXT_CFG_INDEX]		= 0x8300,
	[EXT_CFG_DATA]		= 0x8304,
	[PCIE_HARD_DEBUG]	= 0x4204,
	[PCIE_INTR2_CPU_BASE]	= 0x4300,
};

static const int pcie_offsets_bcm7712[] = {
	[RGR1_SW_INIT_1]	= 0x9210,
	[EXT_CFG_INDEX]		= 0x9000,
	[EXT_CFG_DATA]		= 0x8000,
	[PCIE_HARD_DEBUG]	= 0x4304,
	[PCIE_INTR2_CPU_BASE]	= 0x4400,
};

static const struct pcie_cfg_data generic_cfg = {
	.offsets	= pcie_offsets,
	.soc_base	= GENERIC,
	.perst_set	= brcm_pcie_perst_set_generic,
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic,
	.num_inbound_wins = 3,
};

static const struct pcie_cfg_data bcm2711_cfg = {
	.offsets	= pcie_offsets,
	.soc_base	= BCM2711,
	.perst_set	= brcm_pcie_perst_set_generic,
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic,
	.num_inbound_wins = 3,
};

static const struct pcie_cfg_data bcm2712_cfg = {
	.offsets	= pcie_offsets_bcm7712,
	.soc_base	= BCM7712,
	.perst_set	= brcm_pcie_perst_set_7278,
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic,
	.post_setup	= brcm_pcie_post_setup_bcm2712,
	.quirks		= CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN,
	.num_inbound_wins = 10,
};

static const struct pcie_cfg_data bcm4908_cfg = {
	.offsets	= pcie_offsets,
	.soc_base	= BCM4908,
	.perst_set	= brcm_pcie_perst_set_4908,
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic,
	.num_inbound_wins = 3,
};

static const struct pcie_cfg_data bcm7278_cfg = {
	.offsets	= pcie_offsets_bcm7278,
	.soc_base	= BCM7278,
	.perst_set	= brcm_pcie_perst_set_7278,
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_7278,
	.num_inbound_wins = 3,
};

static const struct pcie_cfg_data bcm7425_cfg = {
	.offsets	= pcie_offsets_bcm7425,
	.soc_base	= BCM7425,
	.perst_set	= brcm_pcie_perst_set_generic,
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic,
	.num_inbound_wins = 3,
};

static const struct pcie_cfg_data bcm7435_cfg = {
	.offsets	= pcie_offsets,
	.soc_base	= BCM7435,
	.perst_set	= brcm_pcie_perst_set_generic,
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic,
	.num_inbound_wins = 3,
};

static const struct pcie_cfg_data bcm7216_cfg = {
	.offsets	= pcie_offsets_bcm7278,
	.soc_base	= BCM7278,
	.perst_set	= brcm_pcie_perst_set_7278,
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_7278,
	.has_phy	= true,
	.num_inbound_wins = 3,
};

static const struct pcie_cfg_data bcm7712_cfg = {
	.offsets	= pcie_offsets_bcm7712,
	.perst_set	= brcm_pcie_perst_set_7278,
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic,
	.soc_base	= BCM7712,
	.num_inbound_wins = 10,
};

static const struct of_device_id brcm_pcie_match[] = {
	{ .compatible = "brcm,bcm2711-pcie", .data = &bcm2711_cfg },
	{ .compatible = "brcm,bcm2712-pcie", .data = &bcm2712_cfg },
	{ .compatible = "brcm,bcm4908-pcie", .data = &bcm4908_cfg },
	{ .compatible = "brcm,bcm7211-pcie", .data = &generic_cfg },
	{ .compatible = "brcm,bcm7216-pcie", .data = &bcm7216_cfg },
	{ .compatible = "brcm,bcm7278-pcie", .data = &bcm7278_cfg },
	{ .compatible = "brcm,bcm7425-pcie", .data = &bcm7425_cfg },
	{ .compatible = "brcm,bcm7435-pcie", .data = &bcm7435_cfg },
	{ .compatible = "brcm,bcm7445-pcie", .data = &generic_cfg },
	{ .compatible = "brcm,bcm7712-pcie", .data = &bcm7712_cfg },
	{},
};

static struct pci_ops brcm_pcie_ops = {
	.map_bus = brcm_pcie_map_bus,
	.read = pci_generic_config_read,
	.write = pci_generic_config_write,
	.add_bus = brcm_pcie_add_bus,
	.remove_bus = brcm_pcie_remove_bus,
};

static struct pci_ops brcm7425_pcie_ops = {
	.map_bus = brcm7425_pcie_map_bus,
	.read = pci_generic_config_read32,
	.write = pci_generic_config_write32,
	.add_bus = brcm_pcie_add_bus,
	.remove_bus = brcm_pcie_remove_bus,
};

static int brcm_pcie_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct pci_host_bridge *bridge;
	const struct pcie_cfg_data *data;
	struct brcm_pcie *pcie;
	int ret;

	bridge = devm_pci_alloc_host_bridge(&pdev->dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;

	data = of_device_get_match_data(&pdev->dev);
	if (!data) {
		pr_err("failed to look up compatible string\n");
		return -EINVAL;
	}

	pcie = pci_host_bridge_priv(bridge);
	pcie->dev = &pdev->dev;
	pcie->np = np;
	pcie->cfg = data;

	pcie->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	pcie->clk = devm_clk_get_optional(&pdev->dev, "sw_pcie");
	if (IS_ERR(pcie->clk))
		return PTR_ERR(pcie->clk);

	ret = of_pci_get_max_link_speed(np);
	pcie->gen = (ret < 0) ? 0 : ret;

	pcie->ssc = of_property_read_bool(np, "brcm,enable-ssc");

	pcie->rescal = devm_reset_control_get_optional_shared(&pdev->dev, "rescal");
	if (IS_ERR(pcie->rescal))
		return PTR_ERR(pcie->rescal);

	pcie->perst_reset = devm_reset_control_get_optional_exclusive(&pdev->dev, "perst");
	if (IS_ERR(pcie->perst_reset))
		return PTR_ERR(pcie->perst_reset);

	pcie->bridge_reset = devm_reset_control_get_optional_exclusive(&pdev->dev, "bridge");
	if (IS_ERR(pcie->bridge_reset))
		return PTR_ERR(pcie->bridge_reset);

	pcie->swinit_reset = devm_reset_control_get_optional_exclusive(&pdev->dev, "swinit");
	if (IS_ERR(pcie->swinit_reset))
		return PTR_ERR(pcie->swinit_reset);

	ret = clk_prepare_enable(pcie->clk);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "could not enable clock\n");

	pcie->cfg->bridge_sw_init_set(pcie, 0);

	if (pcie->swinit_reset) {
		ret = reset_control_assert(pcie->swinit_reset);
		if (ret) {
			clk_disable_unprepare(pcie->clk);
			return dev_err_probe(&pdev->dev, ret,
					     "could not assert reset 'swinit'\n");
		}

		/* HW team recommends 1us for proper sync and propagation of reset */
		udelay(1);

		ret = reset_control_deassert(pcie->swinit_reset);
		if (ret) {
			clk_disable_unprepare(pcie->clk);
			return dev_err_probe(&pdev->dev, ret,
					     "could not de-assert reset 'swinit'\n");
		}
	}

	ret = reset_control_reset(pcie->rescal);
	if (ret) {
		clk_disable_unprepare(pcie->clk);
		return dev_err_probe(&pdev->dev, ret, "failed to deassert 'rescal'\n");
	}

	ret = brcm_phy_start(pcie);
	if (ret) {
		reset_control_rearm(pcie->rescal);
		clk_disable_unprepare(pcie->clk);
		return ret;
	}

	ret = brcm_pcie_setup(pcie);
	if (ret)
		goto fail;

	pcie->hw_rev = readl(pcie->base + PCIE_MISC_REVISION);
	if (pcie->cfg->soc_base == BCM4908 &&
	    pcie->hw_rev >= BRCM_PCIE_HW_REV_3_20) {
		dev_err(pcie->dev, "hardware revision with unsupported PERST# setup\n");
		ret = -ENODEV;
		goto fail;
	}

	if (pci_msi_enabled()) {
		struct device_node *msi_np = of_parse_phandle(pcie->np, "msi-parent", 0);

		if (msi_np == pcie->np)
			ret = brcm_pcie_enable_msi(pcie);

		of_node_put(msi_np);

		if (ret) {
			dev_err(pcie->dev, "probe of internal MSI failed");
			goto fail;
		}
	}

	bridge->ops = pcie->cfg->soc_base == BCM7425 ?
				&brcm7425_pcie_ops : &brcm_pcie_ops;
	bridge->sysdata = pcie;

	platform_set_drvdata(pdev, pcie);

	ret = pci_host_probe(bridge);
	if (!ret && !brcm_pcie_link_up(pcie))
		ret = -ENODEV;

	if (ret) {
		brcm_pcie_remove(pdev);
		return ret;
	}

	return 0;

fail:
	__brcm_pcie_remove(pcie);

	return ret;
}

MODULE_DEVICE_TABLE(of, brcm_pcie_match);

static const struct dev_pm_ops brcm_pcie_pm_ops = {
	.suspend_noirq = brcm_pcie_suspend_noirq,
	.resume_noirq = brcm_pcie_resume_noirq,
};

static struct platform_driver brcm_pcie_driver = {
	.probe = brcm_pcie_probe,
	.remove = brcm_pcie_remove,
	.driver = {
		.name = "brcm-pcie",
		.of_match_table = brcm_pcie_match,
		.pm = &brcm_pcie_pm_ops,
	},
};
module_platform_driver(brcm_pcie_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Broadcom STB PCIe RC driver");
MODULE_AUTHOR("Broadcom");
MODULE_SOFTDEP("pre: irq_bcm2712_mip");
