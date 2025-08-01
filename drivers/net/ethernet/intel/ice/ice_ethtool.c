// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018, Intel Corporation. */

/* ethtool support for ice */

#include "ice.h"
#include "ice_ethtool.h"
#include "ice_flow.h"
#include "ice_fltr.h"
#include "ice_lib.h"
#include "ice_dcb_lib.h"
#include <net/dcbnl.h>

struct ice_stats {
	char stat_string[ETH_GSTRING_LEN];
	int sizeof_stat;
	int stat_offset;
};

#define ICE_STAT(_type, _name, _stat) { \
	.stat_string = _name, \
	.sizeof_stat = sizeof_field(_type, _stat), \
	.stat_offset = offsetof(_type, _stat) \
}

#define ICE_VSI_STAT(_name, _stat) \
		ICE_STAT(struct ice_vsi, _name, _stat)
#define ICE_PF_STAT(_name, _stat) \
		ICE_STAT(struct ice_pf, _name, _stat)

static int ice_q_stats_len(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);

	return ((np->vsi->alloc_txq + np->vsi->alloc_rxq) *
		(sizeof(struct ice_q_stats) / sizeof(u64)));
}

#define ICE_PF_STATS_LEN	ARRAY_SIZE(ice_gstrings_pf_stats)
#define ICE_VSI_STATS_LEN	ARRAY_SIZE(ice_gstrings_vsi_stats)

#define ICE_PFC_STATS_LEN ( \
		(sizeof_field(struct ice_pf, stats.priority_xoff_rx) + \
		 sizeof_field(struct ice_pf, stats.priority_xon_rx) + \
		 sizeof_field(struct ice_pf, stats.priority_xoff_tx) + \
		 sizeof_field(struct ice_pf, stats.priority_xon_tx)) \
		 / sizeof(u64))
#define ICE_ALL_STATS_LEN(n)	(ICE_PF_STATS_LEN + ICE_PFC_STATS_LEN + \
				 ICE_VSI_STATS_LEN + ice_q_stats_len(n))

static const struct ice_stats ice_gstrings_vsi_stats[] = {
	ICE_VSI_STAT("rx_unicast", eth_stats.rx_unicast),
	ICE_VSI_STAT("tx_unicast", eth_stats.tx_unicast),
	ICE_VSI_STAT("rx_multicast", eth_stats.rx_multicast),
	ICE_VSI_STAT("tx_multicast", eth_stats.tx_multicast),
	ICE_VSI_STAT("rx_broadcast", eth_stats.rx_broadcast),
	ICE_VSI_STAT("tx_broadcast", eth_stats.tx_broadcast),
	ICE_VSI_STAT("rx_bytes", eth_stats.rx_bytes),
	ICE_VSI_STAT("tx_bytes", eth_stats.tx_bytes),
	ICE_VSI_STAT("rx_dropped", eth_stats.rx_discards),
	ICE_VSI_STAT("rx_unknown_protocol", eth_stats.rx_unknown_protocol),
	ICE_VSI_STAT("rx_alloc_fail", rx_buf_failed),
	ICE_VSI_STAT("rx_pg_alloc_fail", rx_page_failed),
	ICE_VSI_STAT("tx_errors", eth_stats.tx_errors),
	ICE_VSI_STAT("tx_linearize", tx_linearize),
	ICE_VSI_STAT("tx_busy", tx_busy),
	ICE_VSI_STAT("tx_restart", tx_restart),
};

enum ice_ethtool_test_id {
	ICE_ETH_TEST_REG = 0,
	ICE_ETH_TEST_EEPROM,
	ICE_ETH_TEST_INTR,
	ICE_ETH_TEST_LOOP,
	ICE_ETH_TEST_LINK,
};

static const char ice_gstrings_test[][ETH_GSTRING_LEN] = {
	"Register test  (offline)",
	"EEPROM test    (offline)",
	"Interrupt test (offline)",
	"Loopback test  (offline)",
	"Link test   (on/offline)",
};

#define ICE_TEST_LEN (sizeof(ice_gstrings_test) / ETH_GSTRING_LEN)

/* These PF_STATs might look like duplicates of some NETDEV_STATs,
 * but they aren't. This device is capable of supporting multiple
 * VSIs/netdevs on a single PF. The NETDEV_STATs are for individual
 * netdevs whereas the PF_STATs are for the physical function that's
 * hosting these netdevs.
 *
 * The PF_STATs are appended to the netdev stats only when ethtool -S
 * is queried on the base PF netdev.
 */
static const struct ice_stats ice_gstrings_pf_stats[] = {
	ICE_PF_STAT("rx_bytes.nic", stats.eth.rx_bytes),
	ICE_PF_STAT("tx_bytes.nic", stats.eth.tx_bytes),
	ICE_PF_STAT("rx_unicast.nic", stats.eth.rx_unicast),
	ICE_PF_STAT("tx_unicast.nic", stats.eth.tx_unicast),
	ICE_PF_STAT("rx_multicast.nic", stats.eth.rx_multicast),
	ICE_PF_STAT("tx_multicast.nic", stats.eth.tx_multicast),
	ICE_PF_STAT("rx_broadcast.nic", stats.eth.rx_broadcast),
	ICE_PF_STAT("tx_broadcast.nic", stats.eth.tx_broadcast),
	ICE_PF_STAT("tx_errors.nic", stats.eth.tx_errors),
	ICE_PF_STAT("tx_timeout.nic", tx_timeout_count),
	ICE_PF_STAT("rx_size_64.nic", stats.rx_size_64),
	ICE_PF_STAT("tx_size_64.nic", stats.tx_size_64),
	ICE_PF_STAT("rx_size_127.nic", stats.rx_size_127),
	ICE_PF_STAT("tx_size_127.nic", stats.tx_size_127),
	ICE_PF_STAT("rx_size_255.nic", stats.rx_size_255),
	ICE_PF_STAT("tx_size_255.nic", stats.tx_size_255),
	ICE_PF_STAT("rx_size_511.nic", stats.rx_size_511),
	ICE_PF_STAT("tx_size_511.nic", stats.tx_size_511),
	ICE_PF_STAT("rx_size_1023.nic", stats.rx_size_1023),
	ICE_PF_STAT("tx_size_1023.nic", stats.tx_size_1023),
	ICE_PF_STAT("rx_size_1522.nic", stats.rx_size_1522),
	ICE_PF_STAT("tx_size_1522.nic", stats.tx_size_1522),
	ICE_PF_STAT("rx_size_big.nic", stats.rx_size_big),
	ICE_PF_STAT("tx_size_big.nic", stats.tx_size_big),
	ICE_PF_STAT("link_xon_rx.nic", stats.link_xon_rx),
	ICE_PF_STAT("link_xon_tx.nic", stats.link_xon_tx),
	ICE_PF_STAT("link_xoff_rx.nic", stats.link_xoff_rx),
	ICE_PF_STAT("link_xoff_tx.nic", stats.link_xoff_tx),
	ICE_PF_STAT("tx_dropped_link_down.nic", stats.tx_dropped_link_down),
	ICE_PF_STAT("rx_undersize.nic", stats.rx_undersize),
	ICE_PF_STAT("rx_fragments.nic", stats.rx_fragments),
	ICE_PF_STAT("rx_oversize.nic", stats.rx_oversize),
	ICE_PF_STAT("rx_jabber.nic", stats.rx_jabber),
	ICE_PF_STAT("rx_csum_bad.nic", hw_csum_rx_error),
	ICE_PF_STAT("rx_eipe_error.nic", hw_rx_eipe_error),
	ICE_PF_STAT("rx_dropped.nic", stats.eth.rx_discards),
	ICE_PF_STAT("rx_crc_errors.nic", stats.crc_errors),
	ICE_PF_STAT("illegal_bytes.nic", stats.illegal_bytes),
	ICE_PF_STAT("mac_local_faults.nic", stats.mac_local_faults),
	ICE_PF_STAT("mac_remote_faults.nic", stats.mac_remote_faults),
	ICE_PF_STAT("fdir_sb_match.nic", stats.fd_sb_match),
	ICE_PF_STAT("fdir_sb_status.nic", stats.fd_sb_status),
	ICE_PF_STAT("tx_hwtstamp_skipped", ptp.tx_hwtstamp_skipped),
	ICE_PF_STAT("tx_hwtstamp_timeouts", ptp.tx_hwtstamp_timeouts),
	ICE_PF_STAT("tx_hwtstamp_flushed", ptp.tx_hwtstamp_flushed),
	ICE_PF_STAT("tx_hwtstamp_discarded", ptp.tx_hwtstamp_discarded),
	ICE_PF_STAT("late_cached_phc_updates", ptp.late_cached_phc_updates),
};

static const u32 ice_regs_dump_list[] = {
	PFGEN_STATE,
	PRTGEN_STATUS,
	QRX_CTRL(0),
	QINT_TQCTL(0),
	QINT_RQCTL(0),
	PFINT_OICR_ENA,
	QRX_ITR(0),
#define GLDCB_TLPM_PCI_DM			0x000A0180
	GLDCB_TLPM_PCI_DM,
#define GLDCB_TLPM_TC2PFC			0x000A0194
	GLDCB_TLPM_TC2PFC,
#define TCDCB_TLPM_WAIT_DM(_i)			(0x000A0080 + ((_i) * 4))
	TCDCB_TLPM_WAIT_DM(0),
	TCDCB_TLPM_WAIT_DM(1),
	TCDCB_TLPM_WAIT_DM(2),
	TCDCB_TLPM_WAIT_DM(3),
	TCDCB_TLPM_WAIT_DM(4),
	TCDCB_TLPM_WAIT_DM(5),
	TCDCB_TLPM_WAIT_DM(6),
	TCDCB_TLPM_WAIT_DM(7),
	TCDCB_TLPM_WAIT_DM(8),
	TCDCB_TLPM_WAIT_DM(9),
	TCDCB_TLPM_WAIT_DM(10),
	TCDCB_TLPM_WAIT_DM(11),
	TCDCB_TLPM_WAIT_DM(12),
	TCDCB_TLPM_WAIT_DM(13),
	TCDCB_TLPM_WAIT_DM(14),
	TCDCB_TLPM_WAIT_DM(15),
	TCDCB_TLPM_WAIT_DM(16),
	TCDCB_TLPM_WAIT_DM(17),
	TCDCB_TLPM_WAIT_DM(18),
	TCDCB_TLPM_WAIT_DM(19),
	TCDCB_TLPM_WAIT_DM(20),
	TCDCB_TLPM_WAIT_DM(21),
	TCDCB_TLPM_WAIT_DM(22),
	TCDCB_TLPM_WAIT_DM(23),
	TCDCB_TLPM_WAIT_DM(24),
	TCDCB_TLPM_WAIT_DM(25),
	TCDCB_TLPM_WAIT_DM(26),
	TCDCB_TLPM_WAIT_DM(27),
	TCDCB_TLPM_WAIT_DM(28),
	TCDCB_TLPM_WAIT_DM(29),
	TCDCB_TLPM_WAIT_DM(30),
	TCDCB_TLPM_WAIT_DM(31),
#define GLPCI_WATMK_CLNT_PIPEMON		0x000BFD90
	GLPCI_WATMK_CLNT_PIPEMON,
#define GLPCI_CUR_CLNT_COMMON			0x000BFD84
	GLPCI_CUR_CLNT_COMMON,
#define GLPCI_CUR_CLNT_PIPEMON			0x000BFD88
	GLPCI_CUR_CLNT_PIPEMON,
#define GLPCI_PCIERR				0x0009DEB0
	GLPCI_PCIERR,
#define GLPSM_DEBUG_CTL_STATUS			0x000B0600
	GLPSM_DEBUG_CTL_STATUS,
#define GLPSM0_DEBUG_FIFO_OVERFLOW_DETECT	0x000B0680
	GLPSM0_DEBUG_FIFO_OVERFLOW_DETECT,
#define GLPSM0_DEBUG_FIFO_UNDERFLOW_DETECT	0x000B0684
	GLPSM0_DEBUG_FIFO_UNDERFLOW_DETECT,
#define GLPSM0_DEBUG_DT_OUT_OF_WINDOW		0x000B0688
	GLPSM0_DEBUG_DT_OUT_OF_WINDOW,
#define GLPSM0_DEBUG_INTF_HW_ERROR_DETECT	0x000B069C
	GLPSM0_DEBUG_INTF_HW_ERROR_DETECT,
#define GLPSM0_DEBUG_MISC_HW_ERROR_DETECT	0x000B06A0
	GLPSM0_DEBUG_MISC_HW_ERROR_DETECT,
#define GLPSM1_DEBUG_FIFO_OVERFLOW_DETECT	0x000B0E80
	GLPSM1_DEBUG_FIFO_OVERFLOW_DETECT,
#define GLPSM1_DEBUG_FIFO_UNDERFLOW_DETECT	0x000B0E84
	GLPSM1_DEBUG_FIFO_UNDERFLOW_DETECT,
#define GLPSM1_DEBUG_SRL_FIFO_OVERFLOW_DETECT	0x000B0E88
	GLPSM1_DEBUG_SRL_FIFO_OVERFLOW_DETECT,
#define GLPSM1_DEBUG_SRL_FIFO_UNDERFLOW_DETECT  0x000B0E8C
	GLPSM1_DEBUG_SRL_FIFO_UNDERFLOW_DETECT,
#define GLPSM1_DEBUG_MISC_HW_ERROR_DETECT       0x000B0E90
	GLPSM1_DEBUG_MISC_HW_ERROR_DETECT,
#define GLPSM2_DEBUG_FIFO_OVERFLOW_DETECT       0x000B1680
	GLPSM2_DEBUG_FIFO_OVERFLOW_DETECT,
#define GLPSM2_DEBUG_FIFO_UNDERFLOW_DETECT      0x000B1684
	GLPSM2_DEBUG_FIFO_UNDERFLOW_DETECT,
#define GLPSM2_DEBUG_MISC_HW_ERROR_DETECT       0x000B1688
	GLPSM2_DEBUG_MISC_HW_ERROR_DETECT,
#define GLTDPU_TCLAN_COMP_BOB(_i)               (0x00049ADC + ((_i) * 4))
	GLTDPU_TCLAN_COMP_BOB(1),
	GLTDPU_TCLAN_COMP_BOB(2),
	GLTDPU_TCLAN_COMP_BOB(3),
	GLTDPU_TCLAN_COMP_BOB(4),
	GLTDPU_TCLAN_COMP_BOB(5),
	GLTDPU_TCLAN_COMP_BOB(6),
	GLTDPU_TCLAN_COMP_BOB(7),
	GLTDPU_TCLAN_COMP_BOB(8),
#define GLTDPU_TCB_CMD_BOB(_i)                  (0x0004975C + ((_i) * 4))
	GLTDPU_TCB_CMD_BOB(1),
	GLTDPU_TCB_CMD_BOB(2),
	GLTDPU_TCB_CMD_BOB(3),
	GLTDPU_TCB_CMD_BOB(4),
	GLTDPU_TCB_CMD_BOB(5),
	GLTDPU_TCB_CMD_BOB(6),
	GLTDPU_TCB_CMD_BOB(7),
	GLTDPU_TCB_CMD_BOB(8),
#define GLTDPU_PSM_UPDATE_BOB(_i)               (0x00049B5C + ((_i) * 4))
	GLTDPU_PSM_UPDATE_BOB(1),
	GLTDPU_PSM_UPDATE_BOB(2),
	GLTDPU_PSM_UPDATE_BOB(3),
	GLTDPU_PSM_UPDATE_BOB(4),
	GLTDPU_PSM_UPDATE_BOB(5),
	GLTDPU_PSM_UPDATE_BOB(6),
	GLTDPU_PSM_UPDATE_BOB(7),
	GLTDPU_PSM_UPDATE_BOB(8),
#define GLTCB_CMD_IN_BOB(_i)                    (0x000AE288 + ((_i) * 4))
	GLTCB_CMD_IN_BOB(1),
	GLTCB_CMD_IN_BOB(2),
	GLTCB_CMD_IN_BOB(3),
	GLTCB_CMD_IN_BOB(4),
	GLTCB_CMD_IN_BOB(5),
	GLTCB_CMD_IN_BOB(6),
	GLTCB_CMD_IN_BOB(7),
	GLTCB_CMD_IN_BOB(8),
#define GLLAN_TCLAN_FETCH_CTL_FBK_BOB_CTL(_i)   (0x000FC148 + ((_i) * 4))
	GLLAN_TCLAN_FETCH_CTL_FBK_BOB_CTL(1),
	GLLAN_TCLAN_FETCH_CTL_FBK_BOB_CTL(2),
	GLLAN_TCLAN_FETCH_CTL_FBK_BOB_CTL(3),
	GLLAN_TCLAN_FETCH_CTL_FBK_BOB_CTL(4),
	GLLAN_TCLAN_FETCH_CTL_FBK_BOB_CTL(5),
	GLLAN_TCLAN_FETCH_CTL_FBK_BOB_CTL(6),
	GLLAN_TCLAN_FETCH_CTL_FBK_BOB_CTL(7),
	GLLAN_TCLAN_FETCH_CTL_FBK_BOB_CTL(8),
#define GLLAN_TCLAN_FETCH_CTL_SCHED_BOB_CTL(_i) (0x000FC248 + ((_i) * 4))
	GLLAN_TCLAN_FETCH_CTL_SCHED_BOB_CTL(1),
	GLLAN_TCLAN_FETCH_CTL_SCHED_BOB_CTL(2),
	GLLAN_TCLAN_FETCH_CTL_SCHED_BOB_CTL(3),
	GLLAN_TCLAN_FETCH_CTL_SCHED_BOB_CTL(4),
	GLLAN_TCLAN_FETCH_CTL_SCHED_BOB_CTL(5),
	GLLAN_TCLAN_FETCH_CTL_SCHED_BOB_CTL(6),
	GLLAN_TCLAN_FETCH_CTL_SCHED_BOB_CTL(7),
	GLLAN_TCLAN_FETCH_CTL_SCHED_BOB_CTL(8),
#define GLLAN_TCLAN_CACHE_CTL_BOB_CTL(_i)       (0x000FC1C8 + ((_i) * 4))
	GLLAN_TCLAN_CACHE_CTL_BOB_CTL(1),
	GLLAN_TCLAN_CACHE_CTL_BOB_CTL(2),
	GLLAN_TCLAN_CACHE_CTL_BOB_CTL(3),
	GLLAN_TCLAN_CACHE_CTL_BOB_CTL(4),
	GLLAN_TCLAN_CACHE_CTL_BOB_CTL(5),
	GLLAN_TCLAN_CACHE_CTL_BOB_CTL(6),
	GLLAN_TCLAN_CACHE_CTL_BOB_CTL(7),
	GLLAN_TCLAN_CACHE_CTL_BOB_CTL(8),
#define GLLAN_TCLAN_FETCH_CTL_PROC_BOB_CTL(_i)  (0x000FC188 + ((_i) * 4))
	GLLAN_TCLAN_FETCH_CTL_PROC_BOB_CTL(1),
	GLLAN_TCLAN_FETCH_CTL_PROC_BOB_CTL(2),
	GLLAN_TCLAN_FETCH_CTL_PROC_BOB_CTL(3),
	GLLAN_TCLAN_FETCH_CTL_PROC_BOB_CTL(4),
	GLLAN_TCLAN_FETCH_CTL_PROC_BOB_CTL(5),
	GLLAN_TCLAN_FETCH_CTL_PROC_BOB_CTL(6),
	GLLAN_TCLAN_FETCH_CTL_PROC_BOB_CTL(7),
	GLLAN_TCLAN_FETCH_CTL_PROC_BOB_CTL(8),
#define GLLAN_TCLAN_FETCH_CTL_PCIE_RD_BOB_CTL(_i) (0x000FC288 + ((_i) * 4))
	GLLAN_TCLAN_FETCH_CTL_PCIE_RD_BOB_CTL(1),
	GLLAN_TCLAN_FETCH_CTL_PCIE_RD_BOB_CTL(2),
	GLLAN_TCLAN_FETCH_CTL_PCIE_RD_BOB_CTL(3),
	GLLAN_TCLAN_FETCH_CTL_PCIE_RD_BOB_CTL(4),
	GLLAN_TCLAN_FETCH_CTL_PCIE_RD_BOB_CTL(5),
	GLLAN_TCLAN_FETCH_CTL_PCIE_RD_BOB_CTL(6),
	GLLAN_TCLAN_FETCH_CTL_PCIE_RD_BOB_CTL(7),
	GLLAN_TCLAN_FETCH_CTL_PCIE_RD_BOB_CTL(8),
#define PRTDCB_TCUPM_REG_CM(_i)			(0x000BC360 + ((_i) * 4))
	PRTDCB_TCUPM_REG_CM(0),
	PRTDCB_TCUPM_REG_CM(1),
	PRTDCB_TCUPM_REG_CM(2),
	PRTDCB_TCUPM_REG_CM(3),
#define PRTDCB_TCUPM_REG_DM(_i)			(0x000BC3A0 + ((_i) * 4))
	PRTDCB_TCUPM_REG_DM(0),
	PRTDCB_TCUPM_REG_DM(1),
	PRTDCB_TCUPM_REG_DM(2),
	PRTDCB_TCUPM_REG_DM(3),
#define PRTDCB_TLPM_REG_DM(_i)			(0x000A0000 + ((_i) * 4))
	PRTDCB_TLPM_REG_DM(0),
	PRTDCB_TLPM_REG_DM(1),
	PRTDCB_TLPM_REG_DM(2),
	PRTDCB_TLPM_REG_DM(3),
};

struct ice_priv_flag {
	char name[ETH_GSTRING_LEN];
	u32 bitno;			/* bit position in pf->flags */
};

#define ICE_PRIV_FLAG(_name, _bitno) { \
	.name = _name, \
	.bitno = _bitno, \
}

static const struct ice_priv_flag ice_gstrings_priv_flags[] = {
	ICE_PRIV_FLAG("link-down-on-close", ICE_FLAG_LINK_DOWN_ON_CLOSE_ENA),
	ICE_PRIV_FLAG("fw-lldp-agent", ICE_FLAG_FW_LLDP_AGENT),
	ICE_PRIV_FLAG("vf-true-promisc-support",
		      ICE_FLAG_VF_TRUE_PROMISC_ENA),
	ICE_PRIV_FLAG("mdd-auto-reset-vf", ICE_FLAG_MDD_AUTO_RESET_VF),
	ICE_PRIV_FLAG("vf-vlan-pruning", ICE_FLAG_VF_VLAN_PRUNING),
	ICE_PRIV_FLAG("legacy-rx", ICE_FLAG_LEGACY_RX),
};

#define ICE_PRIV_FLAG_ARRAY_SIZE	ARRAY_SIZE(ice_gstrings_priv_flags)

static const u32 ice_adv_lnk_speed_100[] __initconst = {
	ETHTOOL_LINK_MODE_100baseT_Full_BIT,
};

static const u32 ice_adv_lnk_speed_1000[] __initconst = {
	ETHTOOL_LINK_MODE_1000baseX_Full_BIT,
	ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
	ETHTOOL_LINK_MODE_1000baseKX_Full_BIT,
};

static const u32 ice_adv_lnk_speed_2500[] __initconst = {
	ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
	ETHTOOL_LINK_MODE_2500baseX_Full_BIT,
};

static const u32 ice_adv_lnk_speed_5000[] __initconst = {
	ETHTOOL_LINK_MODE_5000baseT_Full_BIT,
};

static const u32 ice_adv_lnk_speed_10000[] __initconst = {
	ETHTOOL_LINK_MODE_10000baseT_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseKR_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseSR_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseLR_Full_BIT,
};

static const u32 ice_adv_lnk_speed_25000[] __initconst = {
	ETHTOOL_LINK_MODE_25000baseCR_Full_BIT,
	ETHTOOL_LINK_MODE_25000baseSR_Full_BIT,
	ETHTOOL_LINK_MODE_25000baseKR_Full_BIT,
};

static const u32 ice_adv_lnk_speed_40000[] __initconst = {
	ETHTOOL_LINK_MODE_40000baseCR4_Full_BIT,
	ETHTOOL_LINK_MODE_40000baseSR4_Full_BIT,
	ETHTOOL_LINK_MODE_40000baseLR4_Full_BIT,
	ETHTOOL_LINK_MODE_40000baseKR4_Full_BIT,
};

static const u32 ice_adv_lnk_speed_50000[] __initconst = {
	ETHTOOL_LINK_MODE_50000baseCR2_Full_BIT,
	ETHTOOL_LINK_MODE_50000baseKR2_Full_BIT,
	ETHTOOL_LINK_MODE_50000baseSR2_Full_BIT,
};

static const u32 ice_adv_lnk_speed_100000[] __initconst = {
	ETHTOOL_LINK_MODE_100000baseCR4_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseSR4_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseLR4_ER4_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseKR4_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseCR2_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseSR2_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseKR2_Full_BIT,
};

static const u32 ice_adv_lnk_speed_200000[] __initconst = {
	ETHTOOL_LINK_MODE_200000baseKR4_Full_BIT,
	ETHTOOL_LINK_MODE_200000baseSR4_Full_BIT,
	ETHTOOL_LINK_MODE_200000baseLR4_ER4_FR4_Full_BIT,
	ETHTOOL_LINK_MODE_200000baseDR4_Full_BIT,
	ETHTOOL_LINK_MODE_200000baseCR4_Full_BIT,
};

static struct ethtool_forced_speed_map ice_adv_lnk_speed_maps[] __ro_after_init = {
	ETHTOOL_FORCED_SPEED_MAP(ice_adv_lnk_speed, 100),
	ETHTOOL_FORCED_SPEED_MAP(ice_adv_lnk_speed, 1000),
	ETHTOOL_FORCED_SPEED_MAP(ice_adv_lnk_speed, 2500),
	ETHTOOL_FORCED_SPEED_MAP(ice_adv_lnk_speed, 5000),
	ETHTOOL_FORCED_SPEED_MAP(ice_adv_lnk_speed, 10000),
	ETHTOOL_FORCED_SPEED_MAP(ice_adv_lnk_speed, 25000),
	ETHTOOL_FORCED_SPEED_MAP(ice_adv_lnk_speed, 40000),
	ETHTOOL_FORCED_SPEED_MAP(ice_adv_lnk_speed, 50000),
	ETHTOOL_FORCED_SPEED_MAP(ice_adv_lnk_speed, 100000),
	ETHTOOL_FORCED_SPEED_MAP(ice_adv_lnk_speed, 200000),
};

void __init ice_adv_lnk_speed_maps_init(void)
{
	ethtool_forced_speed_maps_init(ice_adv_lnk_speed_maps,
				       ARRAY_SIZE(ice_adv_lnk_speed_maps));
}

static void
__ice_get_drvinfo(struct net_device *netdev, struct ethtool_drvinfo *drvinfo,
		  struct ice_vsi *vsi)
{
	struct ice_pf *pf = vsi->back;
	struct ice_hw *hw = &pf->hw;
	struct ice_orom_info *orom;
	struct ice_nvm_info *nvm;

	nvm = &hw->flash.nvm;
	orom = &hw->flash.orom;

	strscpy(drvinfo->driver, KBUILD_MODNAME, sizeof(drvinfo->driver));

	/* Display NVM version (from which the firmware version can be
	 * determined) which contains more pertinent information.
	 */
	snprintf(drvinfo->fw_version, sizeof(drvinfo->fw_version),
		 "%x.%02x 0x%x %d.%d.%d", nvm->major, nvm->minor,
		 nvm->eetrack, orom->major, orom->build, orom->patch);

	strscpy(drvinfo->bus_info, pci_name(pf->pdev),
		sizeof(drvinfo->bus_info));
}

static void
ice_get_drvinfo(struct net_device *netdev, struct ethtool_drvinfo *drvinfo)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);

	__ice_get_drvinfo(netdev, drvinfo, np->vsi);
	drvinfo->n_priv_flags = ICE_PRIV_FLAG_ARRAY_SIZE;
}

static int ice_get_regs_len(struct net_device __always_unused *netdev)
{
	return (sizeof(ice_regs_dump_list) +
		sizeof(struct ice_regdump_to_ethtool));
}

/**
 * ice_ethtool_get_maxspeed - Get the max speed for given lport
 * @hw: pointer to the HW struct
 * @lport: logical port for which max speed is requested
 * @max_speed: return max speed for input lport
 *
 * Return: 0 on success, negative on failure.
 */
static int ice_ethtool_get_maxspeed(struct ice_hw *hw, u8 lport, u8 *max_speed)
{
	struct ice_aqc_get_port_options_elem options[ICE_AQC_PORT_OPT_MAX] = {};
	bool active_valid = false, pending_valid = true;
	u8 option_count = ICE_AQC_PORT_OPT_MAX;
	u8 active_idx = 0, pending_idx = 0;
	int status;

	status = ice_aq_get_port_options(hw, options, &option_count, lport,
					 true, &active_idx, &active_valid,
					 &pending_idx, &pending_valid);
	if (status)
		return -EIO;
	if (!active_valid)
		return -EINVAL;

	*max_speed = options[active_idx].max_lane_speed & ICE_AQC_PORT_OPT_MAX_LANE_M;
	return 0;
}

/**
 * ice_is_serdes_muxed - returns whether serdes is muxed in hardware
 * @hw: pointer to the HW struct
 *
 * Return: true when serdes is muxed, false when serdes is not muxed.
 */
static bool ice_is_serdes_muxed(struct ice_hw *hw)
{
	u32 reg_value = rd32(hw, GLGEN_SWITCH_MODE_CONFIG);

	return FIELD_GET(GLGEN_SWITCH_MODE_CONFIG_25X4_QUAD_M, reg_value);
}

static int ice_map_port_topology_for_sfp(struct ice_port_topology *port_topology,
					 u8 lport, bool is_muxed)
{
	switch (lport) {
	case 0:
		port_topology->pcs_quad_select = 0;
		port_topology->pcs_port = 0;
		port_topology->primary_serdes_lane = 0;
		break;
	case 1:
		port_topology->pcs_quad_select = 1;
		port_topology->pcs_port = 0;
		if (is_muxed)
			port_topology->primary_serdes_lane = 2;
		else
			port_topology->primary_serdes_lane = 4;
		break;
	case 2:
		port_topology->pcs_quad_select = 0;
		port_topology->pcs_port = 1;
		port_topology->primary_serdes_lane = 1;
		break;
	case 3:
		port_topology->pcs_quad_select = 1;
		port_topology->pcs_port = 1;
		if (is_muxed)
			port_topology->primary_serdes_lane = 3;
		else
			port_topology->primary_serdes_lane = 5;
		break;
	case 4:
		port_topology->pcs_quad_select = 0;
		port_topology->pcs_port = 2;
		port_topology->primary_serdes_lane = 2;
		break;
	case 5:
		port_topology->pcs_quad_select = 1;
		port_topology->pcs_port = 2;
		port_topology->primary_serdes_lane = 6;
		break;
	case 6:
		port_topology->pcs_quad_select = 0;
		port_topology->pcs_port = 3;
		port_topology->primary_serdes_lane = 3;
		break;
	case 7:
		port_topology->pcs_quad_select = 1;
		port_topology->pcs_port = 3;
		port_topology->primary_serdes_lane = 7;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int ice_map_port_topology_for_qsfp(struct ice_port_topology *port_topology,
					  u8 lport, bool is_muxed)
{
	switch (lport) {
	case 0:
		port_topology->pcs_quad_select = 0;
		port_topology->pcs_port = 0;
		port_topology->primary_serdes_lane = 0;
		break;
	case 1:
		port_topology->pcs_quad_select = 1;
		port_topology->pcs_port = 0;
		if (is_muxed)
			port_topology->primary_serdes_lane = 2;
		else
			port_topology->primary_serdes_lane = 4;
		break;
	case 2:
		port_topology->pcs_quad_select = 0;
		port_topology->pcs_port = 1;
		port_topology->primary_serdes_lane = 1;
		break;
	case 3:
		port_topology->pcs_quad_select = 1;
		port_topology->pcs_port = 1;
		if (is_muxed)
			port_topology->primary_serdes_lane = 3;
		else
			port_topology->primary_serdes_lane = 5;
		break;
	case 4:
		port_topology->pcs_quad_select = 0;
		port_topology->pcs_port = 2;
		port_topology->primary_serdes_lane = 2;
		break;
	case 5:
		port_topology->pcs_quad_select = 1;
		port_topology->pcs_port = 2;
		port_topology->primary_serdes_lane = 6;
		break;
	case 6:
		port_topology->pcs_quad_select = 0;
		port_topology->pcs_port = 3;
		port_topology->primary_serdes_lane = 3;
		break;
	case 7:
		port_topology->pcs_quad_select = 1;
		port_topology->pcs_port = 3;
		port_topology->primary_serdes_lane = 7;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * ice_get_port_topology - returns physical topology like pcsquad, pcsport,
 *                         serdes number
 * @hw: pointer to the HW struct
 * @lport: logical port for which physical info requested
 * @port_topology: buffer to hold port topology
 *
 * Return: 0 on success, negative on failure.
 */
static int ice_get_port_topology(struct ice_hw *hw, u8 lport,
				 struct ice_port_topology *port_topology)
{
	struct ice_aqc_get_link_topo cmd = {};
	u16 node_handle = 0;
	u8 cage_type = 0;
	bool is_muxed;
	int err;
	u8 ctx;

	ctx = ICE_AQC_LINK_TOPO_NODE_TYPE_CAGE << ICE_AQC_LINK_TOPO_NODE_TYPE_S;
	ctx |= ICE_AQC_LINK_TOPO_NODE_CTX_PORT << ICE_AQC_LINK_TOPO_NODE_CTX_S;
	cmd.addr.topo_params.node_type_ctx = ctx;

	err = ice_aq_get_netlist_node(hw, &cmd, &cage_type, &node_handle);
	if (err)
		return -EINVAL;

	is_muxed = ice_is_serdes_muxed(hw);

	if (cage_type == 0x11 ||	/* SFP+ */
	    cage_type == 0x12) {	/* SFP28 */
		port_topology->serdes_lane_count = 1;
		err = ice_map_port_topology_for_sfp(port_topology, lport, is_muxed);
		if (err)
			return err;
	} else if (cage_type == 0x13 ||	/* QSFP */
		   cage_type == 0x14) {	/* QSFP28 */
		u8 max_speed = 0;

		err = ice_ethtool_get_maxspeed(hw, lport, &max_speed);
		if (err)
			return err;

		if (max_speed == ICE_AQC_PORT_OPT_MAX_LANE_100G)
			port_topology->serdes_lane_count = 4;
		else if (max_speed == ICE_AQC_PORT_OPT_MAX_LANE_50G ||
			 max_speed == ICE_AQC_PORT_OPT_MAX_LANE_40G)
			port_topology->serdes_lane_count = 2;
		else
			port_topology->serdes_lane_count = 1;

		err = ice_map_port_topology_for_qsfp(port_topology, lport, is_muxed);
		if (err)
			return err;
	} else {
		return -EINVAL;
	}

	return 0;
}

/**
 * ice_get_tx_rx_equa - read serdes tx rx equaliser param
 * @hw: pointer to the HW struct
 * @serdes_num: represents the serdes number
 * @ptr: structure to read all serdes parameter for given serdes
 *
 * Return: all serdes equalization parameter supported per serdes number
 */
static int ice_get_tx_rx_equa(struct ice_hw *hw, u8 serdes_num,
			      struct ice_serdes_equalization_to_ethtool *ptr)
{
	static const int tx = ICE_AQC_OP_CODE_TX_EQU;
	static const int rx = ICE_AQC_OP_CODE_RX_EQU;
	struct {
		int data_in;
		int opcode;
		int *out;
	} aq_params[] = {
		{ ICE_AQC_TX_EQU_PRE1, tx, &ptr->tx_equ_pre1 },
		{ ICE_AQC_TX_EQU_PRE3, tx, &ptr->tx_equ_pre3 },
		{ ICE_AQC_TX_EQU_ATTEN, tx, &ptr->tx_equ_atten },
		{ ICE_AQC_TX_EQU_POST1, tx, &ptr->tx_equ_post1 },
		{ ICE_AQC_TX_EQU_PRE2, tx, &ptr->tx_equ_pre2 },
		{ ICE_AQC_RX_EQU_PRE2, rx, &ptr->rx_equ_pre2 },
		{ ICE_AQC_RX_EQU_PRE1, rx, &ptr->rx_equ_pre1 },
		{ ICE_AQC_RX_EQU_POST1, rx, &ptr->rx_equ_post1 },
		{ ICE_AQC_RX_EQU_BFLF, rx, &ptr->rx_equ_bflf },
		{ ICE_AQC_RX_EQU_BFHF, rx, &ptr->rx_equ_bfhf },
		{ ICE_AQC_RX_EQU_CTLE_GAINHF, rx, &ptr->rx_equ_ctle_gainhf },
		{ ICE_AQC_RX_EQU_CTLE_GAINLF, rx, &ptr->rx_equ_ctle_gainlf },
		{ ICE_AQC_RX_EQU_CTLE_GAINDC, rx, &ptr->rx_equ_ctle_gaindc },
		{ ICE_AQC_RX_EQU_CTLE_BW, rx, &ptr->rx_equ_ctle_bw },
		{ ICE_AQC_RX_EQU_DFE_GAIN, rx, &ptr->rx_equ_dfe_gain },
		{ ICE_AQC_RX_EQU_DFE_GAIN2, rx, &ptr->rx_equ_dfe_gain_2 },
		{ ICE_AQC_RX_EQU_DFE_2, rx, &ptr->rx_equ_dfe_2 },
		{ ICE_AQC_RX_EQU_DFE_3, rx, &ptr->rx_equ_dfe_3 },
		{ ICE_AQC_RX_EQU_DFE_4, rx, &ptr->rx_equ_dfe_4 },
		{ ICE_AQC_RX_EQU_DFE_5, rx, &ptr->rx_equ_dfe_5 },
		{ ICE_AQC_RX_EQU_DFE_6, rx, &ptr->rx_equ_dfe_6 },
		{ ICE_AQC_RX_EQU_DFE_7, rx, &ptr->rx_equ_dfe_7 },
		{ ICE_AQC_RX_EQU_DFE_8, rx, &ptr->rx_equ_dfe_8 },
		{ ICE_AQC_RX_EQU_DFE_9, rx, &ptr->rx_equ_dfe_9 },
		{ ICE_AQC_RX_EQU_DFE_10, rx, &ptr->rx_equ_dfe_10 },
		{ ICE_AQC_RX_EQU_DFE_11, rx, &ptr->rx_equ_dfe_11 },
		{ ICE_AQC_RX_EQU_DFE_12, rx, &ptr->rx_equ_dfe_12 },
	};
	int err;

	for (int i = 0; i < ARRAY_SIZE(aq_params); i++) {
		err = ice_aq_get_phy_equalization(hw, aq_params[i].data_in,
						  aq_params[i].opcode,
						  serdes_num, aq_params[i].out);
		if (err)
			break;
	}

	return err;
}

/**
 * ice_get_extended_regs - returns FEC correctable, uncorrectable stats per
 *                         pcsquad, pcsport
 * @netdev: pointer to net device structure
 * @p: output buffer to fill requested register dump
 *
 * Return: 0 on success, negative on failure.
 */
static int ice_get_extended_regs(struct net_device *netdev, void *p)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_regdump_to_ethtool *ice_prv_regs_buf;
	struct ice_port_topology port_topology = {};
	struct ice_port_info *pi;
	struct ice_pf *pf;
	struct ice_hw *hw;
	unsigned int i;
	int err;

	pf = np->vsi->back;
	hw = &pf->hw;
	pi = np->vsi->port_info;

	/* Serdes parameters are not supported if not the PF VSI */
	if (np->vsi->type != ICE_VSI_PF || !pi)
		return -EINVAL;

	err = ice_get_port_topology(hw, pi->lport, &port_topology);
	if (err)
		return -EINVAL;
	if (port_topology.serdes_lane_count > 4)
		return -EINVAL;

	ice_prv_regs_buf = p;

	/* Get serdes equalization parameter for available serdes */
	for (i = 0; i < port_topology.serdes_lane_count; i++) {
		u8 serdes_num = 0;

		serdes_num = port_topology.primary_serdes_lane + i;
		err = ice_get_tx_rx_equa(hw, serdes_num,
					 &ice_prv_regs_buf->equalization[i]);
		if (err)
			return -EINVAL;
	}

	return 0;
}

static void
ice_get_regs(struct net_device *netdev, struct ethtool_regs *regs, void *p)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;
	struct ice_hw *hw = &pf->hw;
	u32 *regs_buf = (u32 *)p;
	unsigned int i;

	regs->version = 2;

	for (i = 0; i < ARRAY_SIZE(ice_regs_dump_list); ++i)
		regs_buf[i] = rd32(hw, ice_regs_dump_list[i]);

	ice_get_extended_regs(netdev, (void *)&regs_buf[i]);
}

static u32 ice_get_msglevel(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

#ifndef CONFIG_DYNAMIC_DEBUG
	if (pf->hw.debug_mask)
		netdev_info(netdev, "hw debug_mask: 0x%llX\n",
			    pf->hw.debug_mask);
#endif /* !CONFIG_DYNAMIC_DEBUG */

	return pf->msg_enable;
}

static void ice_set_msglevel(struct net_device *netdev, u32 data)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

#ifndef CONFIG_DYNAMIC_DEBUG
	if (ICE_DBG_USER & data)
		pf->hw.debug_mask = data;
	else
		pf->msg_enable = data;
#else
	pf->msg_enable = data;
#endif /* !CONFIG_DYNAMIC_DEBUG */
}

static void ice_get_link_ext_stats(struct net_device *netdev,
				   struct ethtool_link_ext_stats *stats)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

	stats->link_down_events = pf->link_down_events;
}

static int ice_get_eeprom_len(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

	return (int)pf->hw.flash.flash_size;
}

static int
ice_get_eeprom(struct net_device *netdev, struct ethtool_eeprom *eeprom,
	       u8 *bytes)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct ice_hw *hw = &pf->hw;
	struct device *dev;
	int ret;
	u8 *buf;

	dev = ice_pf_to_dev(pf);

	eeprom->magic = hw->vendor_id | (hw->device_id << 16);
	netdev_dbg(netdev, "GEEPROM cmd 0x%08x, offset 0x%08x, len 0x%08x\n",
		   eeprom->cmd, eeprom->offset, eeprom->len);

	buf = kzalloc(eeprom->len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = ice_acquire_nvm(hw, ICE_RES_READ);
	if (ret) {
		dev_err(dev, "ice_acquire_nvm failed, err %d aq_err %s\n",
			ret, libie_aq_str(hw->adminq.sq_last_status));
		goto out;
	}

	ret = ice_read_flat_nvm(hw, eeprom->offset, &eeprom->len, buf,
				false);
	if (ret) {
		dev_err(dev, "ice_read_flat_nvm failed, err %d aq_err %s\n",
			ret, libie_aq_str(hw->adminq.sq_last_status));
		goto release;
	}

	memcpy(bytes, buf, eeprom->len);
release:
	ice_release_nvm(hw);
out:
	kfree(buf);
	return ret;
}

/**
 * ice_active_vfs - check if there are any active VFs
 * @pf: board private structure
 *
 * Returns true if an active VF is found, otherwise returns false
 */
static bool ice_active_vfs(struct ice_pf *pf)
{
	bool active = false;
	struct ice_vf *vf;
	unsigned int bkt;

	rcu_read_lock();
	ice_for_each_vf_rcu(pf, bkt, vf) {
		if (test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) {
			active = true;
			break;
		}
	}
	rcu_read_unlock();

	return active;
}

/**
 * ice_link_test - perform a link test on a given net_device
 * @netdev: network interface device structure
 *
 * This function performs one of the self-tests required by ethtool.
 * Returns 0 on success, non-zero on failure.
 */
static u64 ice_link_test(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	bool link_up = false;
	int status;

	netdev_info(netdev, "link test\n");
	status = ice_get_link_status(np->vsi->port_info, &link_up);
	if (status) {
		netdev_err(netdev, "link query error, status = %d\n",
			   status);
		return 1;
	}

	if (!link_up)
		return 2;

	return 0;
}

/**
 * ice_eeprom_test - perform an EEPROM test on a given net_device
 * @netdev: network interface device structure
 *
 * This function performs one of the self-tests required by ethtool.
 * Returns 0 on success, non-zero on failure.
 */
static u64 ice_eeprom_test(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

	netdev_info(netdev, "EEPROM test\n");
	return !!(ice_nvm_validate_checksum(&pf->hw));
}

/**
 * ice_reg_pattern_test
 * @hw: pointer to the HW struct
 * @reg: reg to be tested
 * @mask: bits to be touched
 */
static int ice_reg_pattern_test(struct ice_hw *hw, u32 reg, u32 mask)
{
	struct ice_pf *pf = (struct ice_pf *)hw->back;
	struct device *dev = ice_pf_to_dev(pf);
	static const u32 patterns[] = {
		0x5A5A5A5A, 0xA5A5A5A5,
		0x00000000, 0xFFFFFFFF
	};
	u32 val, orig_val;
	unsigned int i;

	orig_val = rd32(hw, reg);
	for (i = 0; i < ARRAY_SIZE(patterns); ++i) {
		u32 pattern = patterns[i] & mask;

		wr32(hw, reg, pattern);
		val = rd32(hw, reg);
		if (val == pattern)
			continue;
		dev_err(dev, "%s: reg pattern test failed - reg 0x%08x pat 0x%08x val 0x%08x\n"
			, __func__, reg, pattern, val);
		return 1;
	}

	wr32(hw, reg, orig_val);
	val = rd32(hw, reg);
	if (val != orig_val) {
		dev_err(dev, "%s: reg restore test failed - reg 0x%08x orig 0x%08x val 0x%08x\n"
			, __func__, reg, orig_val, val);
		return 1;
	}

	return 0;
}

/**
 * ice_reg_test - perform a register test on a given net_device
 * @netdev: network interface device structure
 *
 * This function performs one of the self-tests required by ethtool.
 * Returns 0 on success, non-zero on failure.
 */
static u64 ice_reg_test(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_hw *hw = np->vsi->port_info->hw;
	u32 int_elements = hw->func_caps.common_cap.num_msix_vectors ?
		hw->func_caps.common_cap.num_msix_vectors - 1 : 1;
	struct ice_diag_reg_test_info {
		u32 address;
		u32 mask;
		u32 elem_num;
		u32 elem_size;
	} ice_reg_list[] = {
		{GLINT_ITR(0, 0), 0x00000fff, int_elements,
			GLINT_ITR(0, 1) - GLINT_ITR(0, 0)},
		{GLINT_ITR(1, 0), 0x00000fff, int_elements,
			GLINT_ITR(1, 1) - GLINT_ITR(1, 0)},
		{GLINT_ITR(0, 0), 0x00000fff, int_elements,
			GLINT_ITR(2, 1) - GLINT_ITR(2, 0)},
		{GLINT_CTL, 0xffff0001, 1, 0}
	};
	unsigned int i;

	netdev_dbg(netdev, "Register test\n");
	for (i = 0; i < ARRAY_SIZE(ice_reg_list); ++i) {
		u32 j;

		for (j = 0; j < ice_reg_list[i].elem_num; ++j) {
			u32 mask = ice_reg_list[i].mask;
			u32 reg = ice_reg_list[i].address +
				(j * ice_reg_list[i].elem_size);

			/* bail on failure (non-zero return) */
			if (ice_reg_pattern_test(hw, reg, mask))
				return 1;
		}
	}

	return 0;
}

/**
 * ice_lbtest_prepare_rings - configure Tx/Rx test rings
 * @vsi: pointer to the VSI structure
 *
 * Function configures rings of a VSI for loopback test without
 * enabling interrupts or informing the kernel about new queues.
 *
 * Returns 0 on success, negative on failure.
 */
static int ice_lbtest_prepare_rings(struct ice_vsi *vsi)
{
	int status;

	status = ice_vsi_setup_tx_rings(vsi);
	if (status)
		goto err_setup_tx_ring;

	status = ice_vsi_setup_rx_rings(vsi);
	if (status)
		goto err_setup_rx_ring;

	status = ice_vsi_cfg_lan(vsi);
	if (status)
		goto err_setup_rx_ring;

	status = ice_vsi_start_all_rx_rings(vsi);
	if (status)
		goto err_start_rx_ring;

	return 0;

err_start_rx_ring:
	ice_vsi_free_rx_rings(vsi);
err_setup_rx_ring:
	ice_vsi_stop_lan_tx_rings(vsi, ICE_NO_RESET, 0);
err_setup_tx_ring:
	ice_vsi_free_tx_rings(vsi);

	return status;
}

/**
 * ice_lbtest_disable_rings - disable Tx/Rx test rings after loopback test
 * @vsi: pointer to the VSI structure
 *
 * Function stops and frees VSI rings after a loopback test.
 * Returns 0 on success, negative on failure.
 */
static int ice_lbtest_disable_rings(struct ice_vsi *vsi)
{
	int status;

	status = ice_vsi_stop_lan_tx_rings(vsi, ICE_NO_RESET, 0);
	if (status)
		netdev_err(vsi->netdev, "Failed to stop Tx rings, VSI %d error %d\n",
			   vsi->vsi_num, status);

	status = ice_vsi_stop_all_rx_rings(vsi);
	if (status)
		netdev_err(vsi->netdev, "Failed to stop Rx rings, VSI %d error %d\n",
			   vsi->vsi_num, status);

	ice_vsi_free_tx_rings(vsi);
	ice_vsi_free_rx_rings(vsi);

	return status;
}

/**
 * ice_lbtest_create_frame - create test packet
 * @pf: pointer to the PF structure
 * @ret_data: allocated frame buffer
 * @size: size of the packet data
 *
 * Function allocates a frame with a test pattern on specific offsets.
 * Returns 0 on success, non-zero on failure.
 */
static int ice_lbtest_create_frame(struct ice_pf *pf, u8 **ret_data, u16 size)
{
	u8 *data;

	if (!pf)
		return -EINVAL;

	data = kzalloc(size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/* Since the ethernet test frame should always be at least
	 * 64 bytes long, fill some octets in the payload with test data.
	 */
	memset(data, 0xFF, size);
	data[32] = 0xDE;
	data[42] = 0xAD;
	data[44] = 0xBE;
	data[46] = 0xEF;

	*ret_data = data;

	return 0;
}

/**
 * ice_lbtest_check_frame - verify received loopback frame
 * @frame: pointer to the raw packet data
 *
 * Function verifies received test frame with a pattern.
 * Returns true if frame matches the pattern, false otherwise.
 */
static bool ice_lbtest_check_frame(u8 *frame)
{
	/* Validate bytes of a frame under offsets chosen earlier */
	if (frame[32] == 0xDE &&
	    frame[42] == 0xAD &&
	    frame[44] == 0xBE &&
	    frame[46] == 0xEF &&
	    frame[48] == 0xFF)
		return true;

	return false;
}

/**
 * ice_diag_send - send test frames to the test ring
 * @tx_ring: pointer to the transmit ring
 * @data: pointer to the raw packet data
 * @size: size of the packet to send
 *
 * Function sends loopback packets on a test Tx ring.
 */
static int ice_diag_send(struct ice_tx_ring *tx_ring, u8 *data, u16 size)
{
	struct ice_tx_desc *tx_desc;
	struct ice_tx_buf *tx_buf;
	dma_addr_t dma;
	u64 td_cmd;

	tx_desc = ICE_TX_DESC(tx_ring, tx_ring->next_to_use);
	tx_buf = &tx_ring->tx_buf[tx_ring->next_to_use];

	dma = dma_map_single(tx_ring->dev, data, size, DMA_TO_DEVICE);
	if (dma_mapping_error(tx_ring->dev, dma))
		return -EINVAL;

	tx_desc->buf_addr = cpu_to_le64(dma);

	/* These flags are required for a descriptor to be pushed out */
	td_cmd = (u64)(ICE_TX_DESC_CMD_EOP | ICE_TX_DESC_CMD_RS);
	tx_desc->cmd_type_offset_bsz =
		cpu_to_le64(ICE_TX_DESC_DTYPE_DATA |
			    (td_cmd << ICE_TXD_QW1_CMD_S) |
			    ((u64)0 << ICE_TXD_QW1_OFFSET_S) |
			    ((u64)size << ICE_TXD_QW1_TX_BUF_SZ_S) |
			    ((u64)0 << ICE_TXD_QW1_L2TAG1_S));

	tx_buf->next_to_watch = tx_desc;

	/* Force memory write to complete before letting h/w know
	 * there are new descriptors to fetch.
	 */
	wmb();

	tx_ring->next_to_use++;
	if (tx_ring->next_to_use >= tx_ring->count)
		tx_ring->next_to_use = 0;

	writel_relaxed(tx_ring->next_to_use, tx_ring->tail);

	/* Wait until the packets get transmitted to the receive queue. */
	usleep_range(1000, 2000);
	dma_unmap_single(tx_ring->dev, dma, size, DMA_TO_DEVICE);

	return 0;
}

#define ICE_LB_FRAME_SIZE 64
/**
 * ice_lbtest_receive_frames - receive and verify test frames
 * @rx_ring: pointer to the receive ring
 *
 * Function receives loopback packets and verify their correctness.
 * Returns number of received valid frames.
 */
static int ice_lbtest_receive_frames(struct ice_rx_ring *rx_ring)
{
	struct ice_rx_buf *rx_buf;
	int valid_frames, i;
	u8 *received_buf;

	valid_frames = 0;

	for (i = 0; i < rx_ring->count; i++) {
		union ice_32b_rx_flex_desc *rx_desc;

		rx_desc = ICE_RX_DESC(rx_ring, i);

		if (!(rx_desc->wb.status_error0 &
		    (cpu_to_le16(BIT(ICE_RX_FLEX_DESC_STATUS0_DD_S)) |
		     cpu_to_le16(BIT(ICE_RX_FLEX_DESC_STATUS0_EOF_S)))))
			continue;

		rx_buf = &rx_ring->rx_buf[i];
		received_buf = page_address(rx_buf->page) + rx_buf->page_offset;

		if (ice_lbtest_check_frame(received_buf))
			valid_frames++;
	}

	return valid_frames;
}

/**
 * ice_loopback_test - perform a loopback test on a given net_device
 * @netdev: network interface device structure
 *
 * This function performs one of the self-tests required by ethtool.
 * Returns 0 on success, non-zero on failure.
 */
static u64 ice_loopback_test(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *orig_vsi = np->vsi, *test_vsi;
	struct ice_pf *pf = orig_vsi->back;
	u8 *tx_frame __free(kfree) = NULL;
	u8 broadcast[ETH_ALEN], ret = 0;
	int num_frames, valid_frames;
	struct ice_tx_ring *tx_ring;
	struct ice_rx_ring *rx_ring;
	int i;

	netdev_info(netdev, "loopback test\n");

	test_vsi = ice_lb_vsi_setup(pf, pf->hw.port_info);
	if (!test_vsi) {
		netdev_err(netdev, "Failed to create a VSI for the loopback test\n");
		return 1;
	}

	test_vsi->netdev = netdev;
	tx_ring = test_vsi->tx_rings[0];
	rx_ring = test_vsi->rx_rings[0];

	if (ice_lbtest_prepare_rings(test_vsi)) {
		ret = 2;
		goto lbtest_vsi_close;
	}

	if (ice_alloc_rx_bufs(rx_ring, rx_ring->count)) {
		ret = 3;
		goto lbtest_rings_dis;
	}

	/* Enable MAC loopback in firmware */
	if (ice_aq_set_mac_loopback(&pf->hw, true, NULL)) {
		ret = 4;
		goto lbtest_mac_dis;
	}

	/* Test VSI needs to receive broadcast packets */
	eth_broadcast_addr(broadcast);
	if (ice_fltr_add_mac(test_vsi, broadcast, ICE_FWD_TO_VSI)) {
		ret = 5;
		goto lbtest_mac_dis;
	}

	if (ice_lbtest_create_frame(pf, &tx_frame, ICE_LB_FRAME_SIZE)) {
		ret = 7;
		goto remove_mac_filters;
	}

	num_frames = min_t(int, tx_ring->count, 32);
	for (i = 0; i < num_frames; i++) {
		if (ice_diag_send(tx_ring, tx_frame, ICE_LB_FRAME_SIZE)) {
			ret = 8;
			goto remove_mac_filters;
		}
	}

	valid_frames = ice_lbtest_receive_frames(rx_ring);
	if (!valid_frames)
		ret = 9;
	else if (valid_frames != num_frames)
		ret = 10;

remove_mac_filters:
	if (ice_fltr_remove_mac(test_vsi, broadcast, ICE_FWD_TO_VSI))
		netdev_err(netdev, "Could not remove MAC filter for the test VSI\n");
lbtest_mac_dis:
	/* Disable MAC loopback after the test is completed. */
	if (ice_aq_set_mac_loopback(&pf->hw, false, NULL))
		netdev_err(netdev, "Could not disable MAC loopback\n");
lbtest_rings_dis:
	if (ice_lbtest_disable_rings(test_vsi))
		netdev_err(netdev, "Could not disable test rings\n");
lbtest_vsi_close:
	test_vsi->netdev = NULL;
	if (ice_vsi_release(test_vsi))
		netdev_err(netdev, "Failed to remove the test VSI\n");

	return ret;
}

/**
 * ice_intr_test - perform an interrupt test on a given net_device
 * @netdev: network interface device structure
 *
 * This function performs one of the self-tests required by ethtool.
 * Returns 0 on success, non-zero on failure.
 */
static u64 ice_intr_test(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;
	u16 swic_old = pf->sw_int_count;

	netdev_info(netdev, "interrupt test\n");

	wr32(&pf->hw, GLINT_DYN_CTL(pf->oicr_irq.index),
	     GLINT_DYN_CTL_SW_ITR_INDX_M |
	     GLINT_DYN_CTL_INTENA_MSK_M |
	     GLINT_DYN_CTL_SWINT_TRIG_M);

	usleep_range(1000, 2000);
	return (swic_old == pf->sw_int_count);
}

/**
 * ice_self_test - handler function for performing a self-test by ethtool
 * @netdev: network interface device structure
 * @eth_test: ethtool_test structure
 * @data: required by ethtool.self_test
 *
 * This function is called after invoking 'ethtool -t devname' command where
 * devname is the name of the network device on which ethtool should operate.
 * It performs a set of self-tests to check if a device works properly.
 */
static void
ice_self_test(struct net_device *netdev, struct ethtool_test *eth_test,
	      u64 *data)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	bool if_running = netif_running(netdev);
	struct ice_pf *pf = np->vsi->back;
	struct device *dev;

	dev = ice_pf_to_dev(pf);

	if (eth_test->flags == ETH_TEST_FL_OFFLINE) {
		netdev_info(netdev, "offline testing starting\n");

		set_bit(ICE_TESTING, pf->state);

		if (ice_active_vfs(pf)) {
			dev_warn(dev, "Please take active VFs and Netqueues offline and restart the adapter before running NIC diagnostics\n");
			data[ICE_ETH_TEST_REG] = 1;
			data[ICE_ETH_TEST_EEPROM] = 1;
			data[ICE_ETH_TEST_INTR] = 1;
			data[ICE_ETH_TEST_LOOP] = 1;
			data[ICE_ETH_TEST_LINK] = 1;
			eth_test->flags |= ETH_TEST_FL_FAILED;
			clear_bit(ICE_TESTING, pf->state);
			goto skip_ol_tests;
		}
		/* If the device is online then take it offline */
		if (if_running)
			/* indicate we're in test mode */
			ice_stop(netdev);

		data[ICE_ETH_TEST_LINK] = ice_link_test(netdev);
		data[ICE_ETH_TEST_EEPROM] = ice_eeprom_test(netdev);
		data[ICE_ETH_TEST_INTR] = ice_intr_test(netdev);
		data[ICE_ETH_TEST_LOOP] = ice_loopback_test(netdev);
		data[ICE_ETH_TEST_REG] = ice_reg_test(netdev);

		if (data[ICE_ETH_TEST_LINK] ||
		    data[ICE_ETH_TEST_EEPROM] ||
		    data[ICE_ETH_TEST_LOOP] ||
		    data[ICE_ETH_TEST_INTR] ||
		    data[ICE_ETH_TEST_REG])
			eth_test->flags |= ETH_TEST_FL_FAILED;

		clear_bit(ICE_TESTING, pf->state);

		if (if_running) {
			int status = ice_open(netdev);

			if (status) {
				dev_err(dev, "Could not open device %s, err %d\n",
					pf->int_name, status);
			}
		}
	} else {
		/* Online tests */
		netdev_info(netdev, "online testing starting\n");

		data[ICE_ETH_TEST_LINK] = ice_link_test(netdev);
		if (data[ICE_ETH_TEST_LINK])
			eth_test->flags |= ETH_TEST_FL_FAILED;

		/* Offline only tests, not run in online; pass by default */
		data[ICE_ETH_TEST_REG] = 0;
		data[ICE_ETH_TEST_EEPROM] = 0;
		data[ICE_ETH_TEST_INTR] = 0;
		data[ICE_ETH_TEST_LOOP] = 0;
	}

skip_ol_tests:
	netdev_info(netdev, "testing finished\n");
}

static void
__ice_get_strings(struct net_device *netdev, u32 stringset, u8 *data,
		  struct ice_vsi *vsi)
{
	unsigned int i;
	u8 *p = data;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < ICE_VSI_STATS_LEN; i++)
			ethtool_puts(&p, ice_gstrings_vsi_stats[i].stat_string);

		if (ice_is_port_repr_netdev(netdev))
			return;

		ice_for_each_alloc_txq(vsi, i) {
			ethtool_sprintf(&p, "tx_queue_%u_packets", i);
			ethtool_sprintf(&p, "tx_queue_%u_bytes", i);
		}

		ice_for_each_alloc_rxq(vsi, i) {
			ethtool_sprintf(&p, "rx_queue_%u_packets", i);
			ethtool_sprintf(&p, "rx_queue_%u_bytes", i);
		}

		if (vsi->type != ICE_VSI_PF)
			return;

		for (i = 0; i < ICE_PF_STATS_LEN; i++)
			ethtool_puts(&p, ice_gstrings_pf_stats[i].stat_string);

		for (i = 0; i < ICE_MAX_USER_PRIORITY; i++) {
			ethtool_sprintf(&p, "tx_priority_%u_xon.nic", i);
			ethtool_sprintf(&p, "tx_priority_%u_xoff.nic", i);
		}
		for (i = 0; i < ICE_MAX_USER_PRIORITY; i++) {
			ethtool_sprintf(&p, "rx_priority_%u_xon.nic", i);
			ethtool_sprintf(&p, "rx_priority_%u_xoff.nic", i);
		}
		break;
	case ETH_SS_TEST:
		memcpy(data, ice_gstrings_test, ICE_TEST_LEN * ETH_GSTRING_LEN);
		break;
	case ETH_SS_PRIV_FLAGS:
		for (i = 0; i < ICE_PRIV_FLAG_ARRAY_SIZE; i++)
			ethtool_puts(&p, ice_gstrings_priv_flags[i].name);
		break;
	default:
		break;
	}
}

static void ice_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);

	__ice_get_strings(netdev, stringset, data, np->vsi);
}

static int
ice_set_phys_id(struct net_device *netdev, enum ethtool_phys_id_state state)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	bool led_active;

	switch (state) {
	case ETHTOOL_ID_ACTIVE:
		led_active = true;
		break;
	case ETHTOOL_ID_INACTIVE:
		led_active = false;
		break;
	default:
		return -EINVAL;
	}

	if (ice_aq_set_port_id_led(np->vsi->port_info, !led_active, NULL))
		return -EIO;

	return 0;
}

/**
 * ice_set_fec_cfg - Set link FEC options
 * @netdev: network interface device structure
 * @req_fec: FEC mode to configure
 */
static int ice_set_fec_cfg(struct net_device *netdev, enum ice_fec_mode req_fec)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_aqc_set_phy_cfg_data config = { 0 };
	struct ice_vsi *vsi = np->vsi;
	struct ice_port_info *pi;

	pi = vsi->port_info;
	if (!pi)
		return -EOPNOTSUPP;

	/* Changing the FEC parameters is not supported if not the PF VSI */
	if (vsi->type != ICE_VSI_PF) {
		netdev_info(netdev, "Changing FEC parameters only supported for PF VSI\n");
		return -EOPNOTSUPP;
	}

	/* Proceed only if requesting different FEC mode */
	if (pi->phy.curr_user_fec_req == req_fec)
		return 0;

	/* Copy the current user PHY configuration. The current user PHY
	 * configuration is initialized during probe from PHY capabilities
	 * software mode, and updated on set PHY configuration.
	 */
	memcpy(&config, &pi->phy.curr_user_phy_cfg, sizeof(config));

	ice_cfg_phy_fec(pi, &config, req_fec);
	config.caps |= ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;

	if (ice_aq_set_phy_cfg(pi->hw, pi, &config, NULL))
		return -EAGAIN;

	/* Save requested FEC config */
	pi->phy.curr_user_fec_req = req_fec;

	return 0;
}

/**
 * ice_set_fecparam - Set FEC link options
 * @netdev: network interface device structure
 * @fecparam: Ethtool structure to retrieve FEC parameters
 */
static int
ice_set_fecparam(struct net_device *netdev, struct ethtool_fecparam *fecparam)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	enum ice_fec_mode fec;

	switch (fecparam->fec) {
	case ETHTOOL_FEC_AUTO:
		fec = ICE_FEC_AUTO;
		break;
	case ETHTOOL_FEC_RS:
		fec = ICE_FEC_RS;
		break;
	case ETHTOOL_FEC_BASER:
		fec = ICE_FEC_BASER;
		break;
	case ETHTOOL_FEC_OFF:
	case ETHTOOL_FEC_NONE:
		fec = ICE_FEC_NONE;
		break;
	default:
		dev_warn(ice_pf_to_dev(vsi->back), "Unsupported FEC mode: %d\n",
			 fecparam->fec);
		return -EINVAL;
	}

	return ice_set_fec_cfg(netdev, fec);
}

/**
 * ice_get_fecparam - Get link FEC options
 * @netdev: network interface device structure
 * @fecparam: Ethtool structure to retrieve FEC parameters
 */
static int
ice_get_fecparam(struct net_device *netdev, struct ethtool_fecparam *fecparam)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_aqc_get_phy_caps_data *caps;
	struct ice_link_status *link_info;
	struct ice_vsi *vsi = np->vsi;
	struct ice_port_info *pi;
	int err;

	pi = vsi->port_info;

	if (!pi)
		return -EOPNOTSUPP;
	link_info = &pi->phy.link_info;

	/* Set FEC mode based on negotiated link info */
	switch (link_info->fec_info) {
	case ICE_AQ_LINK_25G_KR_FEC_EN:
		fecparam->active_fec = ETHTOOL_FEC_BASER;
		break;
	case ICE_AQ_LINK_25G_RS_528_FEC_EN:
	case ICE_AQ_LINK_25G_RS_544_FEC_EN:
		fecparam->active_fec = ETHTOOL_FEC_RS;
		break;
	default:
		fecparam->active_fec = ETHTOOL_FEC_OFF;
		break;
	}

	caps = kzalloc(sizeof(*caps), GFP_KERNEL);
	if (!caps)
		return -ENOMEM;

	err = ice_aq_get_phy_caps(pi, false, ICE_AQC_REPORT_TOPO_CAP_MEDIA,
				  caps, NULL);
	if (err)
		goto done;

	/* Set supported/configured FEC modes based on PHY capability */
	if (caps->caps & ICE_AQC_PHY_EN_AUTO_FEC)
		fecparam->fec |= ETHTOOL_FEC_AUTO;
	if (caps->link_fec_options & ICE_AQC_PHY_FEC_10G_KR_40G_KR4_EN ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_10G_KR_40G_KR4_REQ ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_KR_CLAUSE74_EN ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_KR_REQ)
		fecparam->fec |= ETHTOOL_FEC_BASER;
	if (caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_528_REQ ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_544_REQ ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_CLAUSE91_EN)
		fecparam->fec |= ETHTOOL_FEC_RS;
	if (caps->link_fec_options == 0)
		fecparam->fec |= ETHTOOL_FEC_OFF;

done:
	kfree(caps);
	return err;
}

/**
 * ice_nway_reset - restart autonegotiation
 * @netdev: network interface device structure
 */
static int ice_nway_reset(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	int err;

	/* If VSI state is up, then restart autoneg with link up */
	if (!test_bit(ICE_DOWN, vsi->back->state))
		err = ice_set_link(vsi, true);
	else
		err = ice_set_link(vsi, false);

	return err;
}

/**
 * ice_get_priv_flags - report device private flags
 * @netdev: network interface device structure
 *
 * The get string set count and the string set should be matched for each
 * flag returned.  Add new strings for each flag to the ice_gstrings_priv_flags
 * array.
 *
 * Returns a u32 bitmap of flags.
 */
static u32 ice_get_priv_flags(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	u32 i, ret_flags = 0;

	for (i = 0; i < ICE_PRIV_FLAG_ARRAY_SIZE; i++) {
		const struct ice_priv_flag *priv_flag;

		priv_flag = &ice_gstrings_priv_flags[i];

		if (test_bit(priv_flag->bitno, pf->flags))
			ret_flags |= BIT(i);
	}

	return ret_flags;
}

/**
 * ice_set_priv_flags - set private flags
 * @netdev: network interface device structure
 * @flags: bit flags to be set
 */
static int ice_set_priv_flags(struct net_device *netdev, u32 flags)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	DECLARE_BITMAP(change_flags, ICE_PF_FLAGS_NBITS);
	DECLARE_BITMAP(orig_flags, ICE_PF_FLAGS_NBITS);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct device *dev;
	int ret = 0;
	u32 i;

	if (flags > BIT(ICE_PRIV_FLAG_ARRAY_SIZE))
		return -EINVAL;

	dev = ice_pf_to_dev(pf);
	set_bit(ICE_FLAG_ETHTOOL_CTXT, pf->flags);

	bitmap_copy(orig_flags, pf->flags, ICE_PF_FLAGS_NBITS);
	for (i = 0; i < ICE_PRIV_FLAG_ARRAY_SIZE; i++) {
		const struct ice_priv_flag *priv_flag;

		priv_flag = &ice_gstrings_priv_flags[i];

		if (flags & BIT(i))
			set_bit(priv_flag->bitno, pf->flags);
		else
			clear_bit(priv_flag->bitno, pf->flags);
	}

	bitmap_xor(change_flags, pf->flags, orig_flags, ICE_PF_FLAGS_NBITS);

	/* Do not allow change to link-down-on-close when Total Port Shutdown
	 * is enabled.
	 */
	if (test_bit(ICE_FLAG_LINK_DOWN_ON_CLOSE_ENA, change_flags) &&
	    test_bit(ICE_FLAG_TOTAL_PORT_SHUTDOWN_ENA, pf->flags)) {
		dev_err(dev, "Setting link-down-on-close not supported on this port\n");
		set_bit(ICE_FLAG_LINK_DOWN_ON_CLOSE_ENA, pf->flags);
		ret = -EINVAL;
		goto ethtool_exit;
	}

	if (test_bit(ICE_FLAG_FW_LLDP_AGENT, change_flags)) {
		if (!test_bit(ICE_FLAG_FW_LLDP_AGENT, pf->flags)) {
			int status;

			/* Disable FW LLDP engine */
			status = ice_cfg_lldp_mib_change(&pf->hw, false);

			/* If unregistering for LLDP events fails, this is
			 * not an error state, as there shouldn't be any
			 * events to respond to.
			 */
			if (status)
				dev_info(dev, "Failed to unreg for LLDP events\n");

			/* The AQ call to stop the FW LLDP agent will generate
			 * an error if the agent is already stopped.
			 */
			status = ice_aq_stop_lldp(&pf->hw, true, true, NULL);
			if (status)
				dev_warn(dev, "Fail to stop LLDP agent\n");
			/* Use case for having the FW LLDP agent stopped
			 * will likely not need DCB, so failure to init is
			 * not a concern of ethtool
			 */
			status = ice_init_pf_dcb(pf, true);
			if (status)
				dev_warn(dev, "Fail to init DCB\n");

			pf->dcbx_cap &= ~DCB_CAP_DCBX_LLD_MANAGED;
			pf->dcbx_cap |= DCB_CAP_DCBX_HOST;
		} else {
			bool dcbx_agent_status;
			int status;

			if (ice_get_pfc_mode(pf) == ICE_QOS_MODE_DSCP) {
				clear_bit(ICE_FLAG_FW_LLDP_AGENT, pf->flags);
				dev_err(dev, "QoS in L3 DSCP mode, FW Agent not allowed to start\n");
				ret = -EOPNOTSUPP;
				goto ethtool_exit;
			}

			/* Remove rule to direct LLDP packets to default VSI.
			 * The FW LLDP engine will now be consuming them.
			 */
			ice_cfg_sw_rx_lldp(vsi->back, false);

			/* AQ command to start FW LLDP agent will return an
			 * error if the agent is already started
			 */
			status = ice_aq_start_lldp(&pf->hw, true, NULL);
			if (status)
				dev_warn(dev, "Fail to start LLDP Agent\n");

			/* AQ command to start FW DCBX agent will fail if
			 * the agent is already started
			 */
			status = ice_aq_start_stop_dcbx(&pf->hw, true,
							&dcbx_agent_status,
							NULL);
			if (status)
				dev_dbg(dev, "Failed to start FW DCBX\n");

			dev_info(dev, "FW DCBX agent is %s\n",
				 dcbx_agent_status ? "ACTIVE" : "DISABLED");

			/* Failure to configure MIB change or init DCB is not
			 * relevant to ethtool.  Print notification that
			 * registration/init failed but do not return error
			 * state to ethtool
			 */
			status = ice_init_pf_dcb(pf, true);
			if (status)
				dev_dbg(dev, "Fail to init DCB\n");

			/* Register for MIB change events */
			status = ice_cfg_lldp_mib_change(&pf->hw, true);
			if (status)
				dev_dbg(dev, "Fail to enable MIB change events\n");

			pf->dcbx_cap &= ~DCB_CAP_DCBX_HOST;
			pf->dcbx_cap |= DCB_CAP_DCBX_LLD_MANAGED;

			ice_nway_reset(netdev);
		}
	}
	if (test_bit(ICE_FLAG_LEGACY_RX, change_flags)) {
		/* down and up VSI so that changes of Rx cfg are reflected. */
		ice_down_up(vsi);
	}
	/* don't allow modification of this flag when a single VF is in
	 * promiscuous mode because it's not supported
	 */
	if (test_bit(ICE_FLAG_VF_TRUE_PROMISC_ENA, change_flags) &&
	    ice_is_any_vf_in_unicast_promisc(pf)) {
		dev_err(dev, "Changing vf-true-promisc-support flag while VF(s) are in promiscuous mode not supported\n");
		/* toggle bit back to previous state */
		change_bit(ICE_FLAG_VF_TRUE_PROMISC_ENA, pf->flags);
		ret = -EAGAIN;
	}

	if (test_bit(ICE_FLAG_VF_VLAN_PRUNING, change_flags) &&
	    ice_has_vfs(pf)) {
		dev_err(dev, "vf-vlan-pruning: VLAN pruning cannot be changed while VFs are active.\n");
		/* toggle bit back to previous state */
		change_bit(ICE_FLAG_VF_VLAN_PRUNING, pf->flags);
		ret = -EOPNOTSUPP;
	}
ethtool_exit:
	clear_bit(ICE_FLAG_ETHTOOL_CTXT, pf->flags);
	return ret;
}

static int ice_get_sset_count(struct net_device *netdev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		/* The number (and order) of strings reported *must* remain
		 * constant for a given netdevice. This function must not
		 * report a different number based on run time parameters
		 * (such as the number of queues in use, or the setting of
		 * a private ethtool flag). This is due to the nature of the
		 * ethtool stats API.
		 *
		 * Userspace programs such as ethtool must make 3 separate
		 * ioctl requests, one for size, one for the strings, and
		 * finally one for the stats. Since these cross into
		 * userspace, changes to the number or size could result in
		 * undefined memory access or incorrect string<->value
		 * correlations for statistics.
		 *
		 * Even if it appears to be safe, changes to the size or
		 * order of strings will suffer from race conditions and are
		 * not safe.
		 */
		return ICE_ALL_STATS_LEN(netdev);
	case ETH_SS_TEST:
		return ICE_TEST_LEN;
	case ETH_SS_PRIV_FLAGS:
		return ICE_PRIV_FLAG_ARRAY_SIZE;
	default:
		return -EOPNOTSUPP;
	}
}

static void
__ice_get_ethtool_stats(struct net_device *netdev,
			struct ethtool_stats __always_unused *stats, u64 *data,
			struct ice_vsi *vsi)
{
	struct ice_pf *pf = vsi->back;
	struct ice_tx_ring *tx_ring;
	struct ice_rx_ring *rx_ring;
	unsigned int j;
	int i = 0;
	char *p;

	ice_update_pf_stats(pf);
	ice_update_vsi_stats(vsi);

	for (j = 0; j < ICE_VSI_STATS_LEN; j++) {
		p = (char *)vsi + ice_gstrings_vsi_stats[j].stat_offset;
		data[i++] = (ice_gstrings_vsi_stats[j].sizeof_stat ==
			     sizeof(u64)) ? *(u64 *)p : *(u32 *)p;
	}

	if (ice_is_port_repr_netdev(netdev))
		return;

	/* populate per queue stats */
	rcu_read_lock();

	ice_for_each_alloc_txq(vsi, j) {
		tx_ring = READ_ONCE(vsi->tx_rings[j]);
		if (tx_ring && tx_ring->ring_stats) {
			data[i++] = tx_ring->ring_stats->stats.pkts;
			data[i++] = tx_ring->ring_stats->stats.bytes;
		} else {
			data[i++] = 0;
			data[i++] = 0;
		}
	}

	ice_for_each_alloc_rxq(vsi, j) {
		rx_ring = READ_ONCE(vsi->rx_rings[j]);
		if (rx_ring && rx_ring->ring_stats) {
			data[i++] = rx_ring->ring_stats->stats.pkts;
			data[i++] = rx_ring->ring_stats->stats.bytes;
		} else {
			data[i++] = 0;
			data[i++] = 0;
		}
	}

	rcu_read_unlock();

	if (vsi->type != ICE_VSI_PF)
		return;

	for (j = 0; j < ICE_PF_STATS_LEN; j++) {
		p = (char *)pf + ice_gstrings_pf_stats[j].stat_offset;
		data[i++] = (ice_gstrings_pf_stats[j].sizeof_stat ==
			     sizeof(u64)) ? *(u64 *)p : *(u32 *)p;
	}

	for (j = 0; j < ICE_MAX_USER_PRIORITY; j++) {
		data[i++] = pf->stats.priority_xon_tx[j];
		data[i++] = pf->stats.priority_xoff_tx[j];
	}

	for (j = 0; j < ICE_MAX_USER_PRIORITY; j++) {
		data[i++] = pf->stats.priority_xon_rx[j];
		data[i++] = pf->stats.priority_xoff_rx[j];
	}
}

static void
ice_get_ethtool_stats(struct net_device *netdev,
		      struct ethtool_stats __always_unused *stats, u64 *data)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);

	__ice_get_ethtool_stats(netdev, stats, data, np->vsi);
}

#define ICE_PHY_TYPE_LOW_MASK_MIN_1G	(ICE_PHY_TYPE_LOW_100BASE_TX | \
					 ICE_PHY_TYPE_LOW_100M_SGMII)

#define ICE_PHY_TYPE_LOW_MASK_MIN_25G	(ICE_PHY_TYPE_LOW_MASK_MIN_1G | \
					 ICE_PHY_TYPE_LOW_1000BASE_T | \
					 ICE_PHY_TYPE_LOW_1000BASE_SX | \
					 ICE_PHY_TYPE_LOW_1000BASE_LX | \
					 ICE_PHY_TYPE_LOW_1000BASE_KX | \
					 ICE_PHY_TYPE_LOW_1G_SGMII | \
					 ICE_PHY_TYPE_LOW_2500BASE_T | \
					 ICE_PHY_TYPE_LOW_2500BASE_X | \
					 ICE_PHY_TYPE_LOW_2500BASE_KX | \
					 ICE_PHY_TYPE_LOW_5GBASE_T | \
					 ICE_PHY_TYPE_LOW_5GBASE_KR | \
					 ICE_PHY_TYPE_LOW_10GBASE_T | \
					 ICE_PHY_TYPE_LOW_10G_SFI_DA | \
					 ICE_PHY_TYPE_LOW_10GBASE_SR | \
					 ICE_PHY_TYPE_LOW_10GBASE_LR | \
					 ICE_PHY_TYPE_LOW_10GBASE_KR_CR1 | \
					 ICE_PHY_TYPE_LOW_10G_SFI_AOC_ACC | \
					 ICE_PHY_TYPE_LOW_10G_SFI_C2C)

#define ICE_PHY_TYPE_LOW_MASK_100G	(ICE_PHY_TYPE_LOW_100GBASE_CR4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_SR4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_LR4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_KR4 | \
					 ICE_PHY_TYPE_LOW_100G_CAUI4_AOC_ACC | \
					 ICE_PHY_TYPE_LOW_100G_CAUI4 | \
					 ICE_PHY_TYPE_LOW_100G_AUI4_AOC_ACC | \
					 ICE_PHY_TYPE_LOW_100G_AUI4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_CR_PAM4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_KR_PAM4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_CP2 | \
					 ICE_PHY_TYPE_LOW_100GBASE_SR2 | \
					 ICE_PHY_TYPE_LOW_100GBASE_DR)

#define ICE_PHY_TYPE_HIGH_MASK_100G	(ICE_PHY_TYPE_HIGH_100GBASE_KR2_PAM4 | \
					 ICE_PHY_TYPE_HIGH_100G_CAUI2_AOC_ACC |\
					 ICE_PHY_TYPE_HIGH_100G_CAUI2 | \
					 ICE_PHY_TYPE_HIGH_100G_AUI2_AOC_ACC | \
					 ICE_PHY_TYPE_HIGH_100G_AUI2)

#define ICE_PHY_TYPE_HIGH_MASK_200G	(ICE_PHY_TYPE_HIGH_200G_CR4_PAM4 | \
					 ICE_PHY_TYPE_HIGH_200G_SR4 | \
					 ICE_PHY_TYPE_HIGH_200G_FR4 | \
					 ICE_PHY_TYPE_HIGH_200G_LR4 | \
					 ICE_PHY_TYPE_HIGH_200G_DR4 | \
					 ICE_PHY_TYPE_HIGH_200G_KR4_PAM4 | \
					 ICE_PHY_TYPE_HIGH_200G_AUI4_AOC_ACC | \
					 ICE_PHY_TYPE_HIGH_200G_AUI4)

/**
 * ice_mask_min_supported_speeds
 * @hw: pointer to the HW structure
 * @phy_types_high: PHY type high
 * @phy_types_low: PHY type low to apply minimum supported speeds mask
 *
 * Apply minimum supported speeds mask to PHY type low. These are the speeds
 * for ethtool supported link mode.
 */
static void
ice_mask_min_supported_speeds(struct ice_hw *hw,
			      u64 phy_types_high, u64 *phy_types_low)
{
	/* if QSFP connection with 100G speed, minimum supported speed is 25G */
	if ((*phy_types_low & ICE_PHY_TYPE_LOW_MASK_100G) ||
	    (phy_types_high & ICE_PHY_TYPE_HIGH_MASK_100G) ||
	    (phy_types_high & ICE_PHY_TYPE_HIGH_MASK_200G))
		*phy_types_low &= ~ICE_PHY_TYPE_LOW_MASK_MIN_25G;
	else if (!ice_is_100m_speed_supported(hw))
		*phy_types_low &= ~ICE_PHY_TYPE_LOW_MASK_MIN_1G;
}

/**
 * ice_linkmode_set_bit - set link mode bit
 * @phy_to_ethtool: PHY type to ethtool link mode struct to set
 * @ks: ethtool link ksettings struct to fill out
 * @req_speeds: speed requested by user
 * @advert_phy_type: advertised PHY type
 * @phy_type: PHY type
 */
static void
ice_linkmode_set_bit(const struct ice_phy_type_to_ethtool *phy_to_ethtool,
		     struct ethtool_link_ksettings *ks, u32 req_speeds,
		     u64 advert_phy_type, u32 phy_type)
{
	linkmode_set_bit(phy_to_ethtool->link_mode, ks->link_modes.supported);

	if (req_speeds & phy_to_ethtool->aq_link_speed ||
	    (!req_speeds && advert_phy_type & BIT(phy_type)))
		linkmode_set_bit(phy_to_ethtool->link_mode,
				 ks->link_modes.advertising);
}

/**
 * ice_phy_type_to_ethtool - convert the phy_types to ethtool link modes
 * @netdev: network interface device structure
 * @ks: ethtool link ksettings struct to fill out
 */
static void
ice_phy_type_to_ethtool(struct net_device *netdev,
			struct ethtool_link_ksettings *ks)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	u64 advert_phy_type_lo = 0;
	u64 advert_phy_type_hi = 0;
	u64 phy_types_high = 0;
	u64 phy_types_low = 0;
	u32 req_speeds;
	u32 i;

	req_speeds = vsi->port_info->phy.link_info.req_speeds;

	/* Check if lenient mode is supported and enabled, or in strict mode.
	 *
	 * In lenient mode the Supported link modes are the PHY types without
	 * media. The Advertising link mode is either 1. the user requested
	 * speed, 2. the override PHY mask, or 3. the PHY types with media.
	 *
	 * In strict mode Supported link mode are the PHY type with media,
	 * and Advertising link modes are the media PHY type or the speed
	 * requested by user.
	 */
	if (test_bit(ICE_FLAG_LINK_LENIENT_MODE_ENA, pf->flags)) {
		phy_types_low = le64_to_cpu(pf->nvm_phy_type_lo);
		phy_types_high = le64_to_cpu(pf->nvm_phy_type_hi);

		ice_mask_min_supported_speeds(&pf->hw, phy_types_high,
					      &phy_types_low);
		/* determine advertised modes based on link override only
		 * if it's supported and if the FW doesn't abstract the
		 * driver from having to account for link overrides
		 */
		if (ice_fw_supports_link_override(&pf->hw) &&
		    !ice_fw_supports_report_dflt_cfg(&pf->hw)) {
			struct ice_link_default_override_tlv *ldo;

			ldo = &pf->link_dflt_override;
			/* If override enabled and PHY mask set, then
			 * Advertising link mode is the intersection of the PHY
			 * types without media and the override PHY mask.
			 */
			if (ldo->options & ICE_LINK_OVERRIDE_EN &&
			    (ldo->phy_type_low || ldo->phy_type_high)) {
				advert_phy_type_lo =
					le64_to_cpu(pf->nvm_phy_type_lo) &
					ldo->phy_type_low;
				advert_phy_type_hi =
					le64_to_cpu(pf->nvm_phy_type_hi) &
					ldo->phy_type_high;
			}
		}
	} else {
		/* strict mode */
		phy_types_low = vsi->port_info->phy.phy_type_low;
		phy_types_high = vsi->port_info->phy.phy_type_high;
	}

	/* If Advertising link mode PHY type is not using override PHY type,
	 * then use PHY type with media.
	 */
	if (!advert_phy_type_lo && !advert_phy_type_hi) {
		advert_phy_type_lo = vsi->port_info->phy.phy_type_low;
		advert_phy_type_hi = vsi->port_info->phy.phy_type_high;
	}

	linkmode_zero(ks->link_modes.supported);
	linkmode_zero(ks->link_modes.advertising);

	for (i = 0; i < ARRAY_SIZE(phy_type_low_lkup); i++) {
		if (phy_types_low & BIT_ULL(i))
			ice_linkmode_set_bit(&phy_type_low_lkup[i], ks,
					     req_speeds, advert_phy_type_lo,
					     i);
	}

	for (i = 0; i < ARRAY_SIZE(phy_type_high_lkup); i++) {
		if (phy_types_high & BIT_ULL(i))
			ice_linkmode_set_bit(&phy_type_high_lkup[i], ks,
					     req_speeds, advert_phy_type_hi,
					     i);
	}
}

#define TEST_SET_BITS_TIMEOUT	50
#define TEST_SET_BITS_SLEEP_MAX	2000
#define TEST_SET_BITS_SLEEP_MIN	1000

/**
 * ice_get_settings_link_up - Get Link settings for when link is up
 * @ks: ethtool ksettings to fill in
 * @netdev: network interface device structure
 */
static void
ice_get_settings_link_up(struct ethtool_link_ksettings *ks,
			 struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_port_info *pi = np->vsi->port_info;
	struct ice_link_status *link_info;
	struct ice_vsi *vsi = np->vsi;

	link_info = &vsi->port_info->phy.link_info;

	/* Get supported and advertised settings from PHY ability with media */
	ice_phy_type_to_ethtool(netdev, ks);

	switch (link_info->link_speed) {
	case ICE_AQ_LINK_SPEED_200GB:
		ks->base.speed = SPEED_200000;
		break;
	case ICE_AQ_LINK_SPEED_100GB:
		ks->base.speed = SPEED_100000;
		break;
	case ICE_AQ_LINK_SPEED_50GB:
		ks->base.speed = SPEED_50000;
		break;
	case ICE_AQ_LINK_SPEED_40GB:
		ks->base.speed = SPEED_40000;
		break;
	case ICE_AQ_LINK_SPEED_25GB:
		ks->base.speed = SPEED_25000;
		break;
	case ICE_AQ_LINK_SPEED_20GB:
		ks->base.speed = SPEED_20000;
		break;
	case ICE_AQ_LINK_SPEED_10GB:
		ks->base.speed = SPEED_10000;
		break;
	case ICE_AQ_LINK_SPEED_5GB:
		ks->base.speed = SPEED_5000;
		break;
	case ICE_AQ_LINK_SPEED_2500MB:
		ks->base.speed = SPEED_2500;
		break;
	case ICE_AQ_LINK_SPEED_1000MB:
		ks->base.speed = SPEED_1000;
		break;
	case ICE_AQ_LINK_SPEED_100MB:
		ks->base.speed = SPEED_100;
		break;
	default:
		netdev_info(netdev, "WARNING: Unrecognized link_speed (0x%x).\n",
			    link_info->link_speed);
		break;
	}
	ks->base.duplex = DUPLEX_FULL;

	if (link_info->an_info & ICE_AQ_AN_COMPLETED)
		ethtool_link_ksettings_add_link_mode(ks, lp_advertising,
						     Autoneg);

	/* Set flow control negotiated Rx/Tx pause */
	switch (pi->fc.current_mode) {
	case ICE_FC_FULL:
		ethtool_link_ksettings_add_link_mode(ks, lp_advertising, Pause);
		break;
	case ICE_FC_TX_PAUSE:
		ethtool_link_ksettings_add_link_mode(ks, lp_advertising, Pause);
		ethtool_link_ksettings_add_link_mode(ks, lp_advertising,
						     Asym_Pause);
		break;
	case ICE_FC_RX_PAUSE:
		ethtool_link_ksettings_add_link_mode(ks, lp_advertising,
						     Asym_Pause);
		break;
	case ICE_FC_PFC:
	default:
		ethtool_link_ksettings_del_link_mode(ks, lp_advertising, Pause);
		ethtool_link_ksettings_del_link_mode(ks, lp_advertising,
						     Asym_Pause);
		break;
	}
}

/**
 * ice_get_settings_link_down - Get the Link settings when link is down
 * @ks: ethtool ksettings to fill in
 * @netdev: network interface device structure
 *
 * Reports link settings that can be determined when link is down
 */
static void
ice_get_settings_link_down(struct ethtool_link_ksettings *ks,
			   struct net_device *netdev)
{
	/* link is down and the driver needs to fall back on
	 * supported PHY types to figure out what info to display
	 */
	ice_phy_type_to_ethtool(netdev, ks);

	/* With no link, speed and duplex are unknown */
	ks->base.speed = SPEED_UNKNOWN;
	ks->base.duplex = DUPLEX_UNKNOWN;
}

/**
 * ice_get_link_ksettings - Get Link Speed and Duplex settings
 * @netdev: network interface device structure
 * @ks: ethtool ksettings
 *
 * Reports speed/duplex settings based on media_type
 */
static int
ice_get_link_ksettings(struct net_device *netdev,
		       struct ethtool_link_ksettings *ks)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_aqc_get_phy_caps_data *caps;
	struct ice_link_status *hw_link_info;
	struct ice_vsi *vsi = np->vsi;
	int err;

	ethtool_link_ksettings_zero_link_mode(ks, supported);
	ethtool_link_ksettings_zero_link_mode(ks, advertising);
	ethtool_link_ksettings_zero_link_mode(ks, lp_advertising);
	hw_link_info = &vsi->port_info->phy.link_info;

	/* set speed and duplex */
	if (hw_link_info->link_info & ICE_AQ_LINK_UP)
		ice_get_settings_link_up(ks, netdev);
	else
		ice_get_settings_link_down(ks, netdev);

	/* set autoneg settings */
	ks->base.autoneg = (hw_link_info->an_info & ICE_AQ_AN_COMPLETED) ?
		AUTONEG_ENABLE : AUTONEG_DISABLE;

	/* set media type settings */
	switch (vsi->port_info->phy.media_type) {
	case ICE_MEDIA_FIBER:
		ethtool_link_ksettings_add_link_mode(ks, supported, FIBRE);
		ks->base.port = PORT_FIBRE;
		break;
	case ICE_MEDIA_BASET:
		ethtool_link_ksettings_add_link_mode(ks, supported, TP);
		ethtool_link_ksettings_add_link_mode(ks, advertising, TP);
		ks->base.port = PORT_TP;
		break;
	case ICE_MEDIA_BACKPLANE:
		ethtool_link_ksettings_add_link_mode(ks, supported, Backplane);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Backplane);
		ks->base.port = PORT_NONE;
		break;
	case ICE_MEDIA_DA:
		ethtool_link_ksettings_add_link_mode(ks, supported, FIBRE);
		ethtool_link_ksettings_add_link_mode(ks, advertising, FIBRE);
		ks->base.port = PORT_DA;
		break;
	default:
		ks->base.port = PORT_OTHER;
		break;
	}

	/* flow control is symmetric and always supported */
	ethtool_link_ksettings_add_link_mode(ks, supported, Pause);

	caps = kzalloc(sizeof(*caps), GFP_KERNEL);
	if (!caps)
		return -ENOMEM;

	err = ice_aq_get_phy_caps(vsi->port_info, false,
				  ICE_AQC_REPORT_ACTIVE_CFG, caps, NULL);
	if (err)
		goto done;

	/* Set the advertised flow control based on the PHY capability */
	if ((caps->caps & ICE_AQC_PHY_EN_TX_LINK_PAUSE) &&
	    (caps->caps & ICE_AQC_PHY_EN_RX_LINK_PAUSE)) {
		ethtool_link_ksettings_add_link_mode(ks, advertising, Pause);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Asym_Pause);
	} else if (caps->caps & ICE_AQC_PHY_EN_TX_LINK_PAUSE) {
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Asym_Pause);
	} else if (caps->caps & ICE_AQC_PHY_EN_RX_LINK_PAUSE) {
		ethtool_link_ksettings_add_link_mode(ks, advertising, Pause);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Asym_Pause);
	} else {
		ethtool_link_ksettings_del_link_mode(ks, advertising, Pause);
		ethtool_link_ksettings_del_link_mode(ks, advertising,
						     Asym_Pause);
	}

	/* Set advertised FEC modes based on PHY capability */
	ethtool_link_ksettings_add_link_mode(ks, advertising, FEC_NONE);

	if (caps->link_fec_options & ICE_AQC_PHY_FEC_10G_KR_40G_KR4_REQ ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_KR_REQ)
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     FEC_BASER);
	if (caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_528_REQ ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_544_REQ)
		ethtool_link_ksettings_add_link_mode(ks, advertising, FEC_RS);

	err = ice_aq_get_phy_caps(vsi->port_info, false,
				  ICE_AQC_REPORT_TOPO_CAP_MEDIA, caps, NULL);
	if (err)
		goto done;

	/* Set supported FEC modes based on PHY capability */
	ethtool_link_ksettings_add_link_mode(ks, supported, FEC_NONE);

	if (caps->link_fec_options & ICE_AQC_PHY_FEC_10G_KR_40G_KR4_EN ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_KR_CLAUSE74_EN)
		ethtool_link_ksettings_add_link_mode(ks, supported, FEC_BASER);
	if (caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_CLAUSE91_EN)
		ethtool_link_ksettings_add_link_mode(ks, supported, FEC_RS);

	/* Set supported and advertised autoneg */
	if (ice_is_phy_caps_an_enabled(caps)) {
		ethtool_link_ksettings_add_link_mode(ks, supported, Autoneg);
		ethtool_link_ksettings_add_link_mode(ks, advertising, Autoneg);
	}

done:
	kfree(caps);
	return err;
}

/**
 * ice_speed_to_aq_link - Get AQ link speed by Ethtool forced speed
 * @speed: ethtool forced speed
 */
static u16 ice_speed_to_aq_link(int speed)
{
	int aq_speed;

	switch (speed) {
	case SPEED_10:
		aq_speed = ICE_AQ_LINK_SPEED_10MB;
		break;
	case SPEED_100:
		aq_speed = ICE_AQ_LINK_SPEED_100MB;
		break;
	case SPEED_1000:
		aq_speed = ICE_AQ_LINK_SPEED_1000MB;
		break;
	case SPEED_2500:
		aq_speed = ICE_AQ_LINK_SPEED_2500MB;
		break;
	case SPEED_5000:
		aq_speed = ICE_AQ_LINK_SPEED_5GB;
		break;
	case SPEED_10000:
		aq_speed = ICE_AQ_LINK_SPEED_10GB;
		break;
	case SPEED_20000:
		aq_speed = ICE_AQ_LINK_SPEED_20GB;
		break;
	case SPEED_25000:
		aq_speed = ICE_AQ_LINK_SPEED_25GB;
		break;
	case SPEED_40000:
		aq_speed = ICE_AQ_LINK_SPEED_40GB;
		break;
	case SPEED_50000:
		aq_speed = ICE_AQ_LINK_SPEED_50GB;
		break;
	case SPEED_100000:
		aq_speed = ICE_AQ_LINK_SPEED_100GB;
		break;
	default:
		aq_speed = ICE_AQ_LINK_SPEED_UNKNOWN;
		break;
	}
	return aq_speed;
}

/**
 * ice_ksettings_find_adv_link_speed - Find advertising link speed
 * @ks: ethtool ksettings
 */
static u16
ice_ksettings_find_adv_link_speed(const struct ethtool_link_ksettings *ks)
{
	const struct ethtool_forced_speed_map *map;
	u16 adv_link_speed = 0;

	for (u32 i = 0; i < ARRAY_SIZE(ice_adv_lnk_speed_maps); i++) {
		map = ice_adv_lnk_speed_maps + i;
		if (linkmode_intersects(ks->link_modes.advertising, map->caps))
			adv_link_speed |= ice_speed_to_aq_link(map->speed);
	}

	return adv_link_speed;
}

/**
 * ice_setup_autoneg
 * @p: port info
 * @ks: ethtool_link_ksettings
 * @config: configuration that will be sent down to FW
 * @autoneg_enabled: autonegotiation is enabled or not
 * @autoneg_changed: will there a change in autonegotiation
 * @netdev: network interface device structure
 *
 * Setup PHY autonegotiation feature
 */
static int
ice_setup_autoneg(struct ice_port_info *p, struct ethtool_link_ksettings *ks,
		  struct ice_aqc_set_phy_cfg_data *config,
		  u8 autoneg_enabled, u8 *autoneg_changed,
		  struct net_device *netdev)
{
	int err = 0;

	*autoneg_changed = 0;

	/* Check autoneg */
	if (autoneg_enabled == AUTONEG_ENABLE) {
		/* If autoneg was not already enabled */
		if (!(p->phy.link_info.an_info & ICE_AQ_AN_COMPLETED)) {
			/* If autoneg is not supported, return error */
			if (!ethtool_link_ksettings_test_link_mode(ks,
								   supported,
								   Autoneg)) {
				netdev_info(netdev, "Autoneg not supported on this phy.\n");
				err = -EINVAL;
			} else {
				/* Autoneg is allowed to change */
				config->caps |= ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;
				*autoneg_changed = 1;
			}
		}
	} else {
		/* If autoneg is currently enabled */
		if (p->phy.link_info.an_info & ICE_AQ_AN_COMPLETED) {
			/* If autoneg is supported 10GBASE_T is the only PHY
			 * that can disable it, so otherwise return error
			 */
			if (ethtool_link_ksettings_test_link_mode(ks,
								  supported,
								  Autoneg)) {
				netdev_info(netdev, "Autoneg cannot be disabled on this phy\n");
				err = -EINVAL;
			} else {
				/* Autoneg is allowed to change */
				config->caps &= ~ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;
				*autoneg_changed = 1;
			}
		}
	}

	return err;
}

/**
 * ice_set_phy_type_from_speed - set phy_types based on speeds
 * and advertised modes
 * @ks: ethtool link ksettings struct
 * @phy_type_low: pointer to the lower part of phy_type
 * @phy_type_high: pointer to the higher part of phy_type
 * @adv_link_speed: targeted link speeds bitmap
 */
static void
ice_set_phy_type_from_speed(const struct ethtool_link_ksettings *ks,
			    u64 *phy_type_low, u64 *phy_type_high,
			    u16 adv_link_speed)
{
	/* Handle 1000M speed in a special way because ice_update_phy_type
	 * enables all link modes, but having mixed copper and optical
	 * standards is not supported.
	 */
	adv_link_speed &= ~ICE_AQ_LINK_SPEED_1000MB;

	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  1000baseT_Full))
		*phy_type_low |= ICE_PHY_TYPE_LOW_1000BASE_T |
				 ICE_PHY_TYPE_LOW_1G_SGMII;

	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  1000baseKX_Full))
		*phy_type_low |= ICE_PHY_TYPE_LOW_1000BASE_KX;

	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  1000baseX_Full))
		*phy_type_low |= ICE_PHY_TYPE_LOW_1000BASE_SX |
				 ICE_PHY_TYPE_LOW_1000BASE_LX;

	ice_update_phy_type(phy_type_low, phy_type_high, adv_link_speed);
}

/**
 * ice_set_link_ksettings - Set Speed and Duplex
 * @netdev: network interface device structure
 * @ks: ethtool ksettings
 *
 * Set speed/duplex per media_types advertised/forced
 */
static int
ice_set_link_ksettings(struct net_device *netdev,
		       const struct ethtool_link_ksettings *ks)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	u8 autoneg, timeout = TEST_SET_BITS_TIMEOUT;
	struct ethtool_link_ksettings copy_ks = *ks;
	struct ethtool_link_ksettings safe_ks = {};
	struct ice_aqc_get_phy_caps_data *phy_caps;
	struct ice_aqc_set_phy_cfg_data config;
	u16 adv_link_speed, curr_link_speed;
	struct ice_pf *pf = np->vsi->back;
	struct ice_port_info *pi;
	u8 autoneg_changed = 0;
	u64 phy_type_high = 0;
	u64 phy_type_low = 0;
	bool linkup;
	int err;

	pi = np->vsi->port_info;

	if (!pi)
		return -EIO;

	if (pi->phy.media_type != ICE_MEDIA_BASET &&
	    pi->phy.media_type != ICE_MEDIA_FIBER &&
	    pi->phy.media_type != ICE_MEDIA_BACKPLANE &&
	    pi->phy.media_type != ICE_MEDIA_DA &&
	    pi->phy.link_info.link_info & ICE_AQ_LINK_UP)
		return -EOPNOTSUPP;

	phy_caps = kzalloc(sizeof(*phy_caps), GFP_KERNEL);
	if (!phy_caps)
		return -ENOMEM;

	/* Get the PHY capabilities based on media */
	if (ice_fw_supports_report_dflt_cfg(pi->hw))
		err = ice_aq_get_phy_caps(pi, false, ICE_AQC_REPORT_DFLT_CFG,
					  phy_caps, NULL);
	else
		err = ice_aq_get_phy_caps(pi, false, ICE_AQC_REPORT_TOPO_CAP_MEDIA,
					  phy_caps, NULL);
	if (err)
		goto done;

	/* save autoneg out of ksettings */
	autoneg = copy_ks.base.autoneg;

	/* Get link modes supported by hardware.*/
	ice_phy_type_to_ethtool(netdev, &safe_ks);

	/* and check against modes requested by user.
	 * Return an error if unsupported mode was set.
	 */
	if (!bitmap_subset(copy_ks.link_modes.advertising,
			   safe_ks.link_modes.supported,
			   __ETHTOOL_LINK_MODE_MASK_NBITS)) {
		if (!test_bit(ICE_FLAG_LINK_LENIENT_MODE_ENA, pf->flags))
			netdev_info(netdev, "The selected speed is not supported by the current media. Please select a link speed that is supported by the current media.\n");
		err = -EOPNOTSUPP;
		goto done;
	}

	/* get our own copy of the bits to check against */
	memset(&safe_ks, 0, sizeof(safe_ks));
	safe_ks.base.cmd = copy_ks.base.cmd;
	safe_ks.base.link_mode_masks_nwords =
		copy_ks.base.link_mode_masks_nwords;
	ice_get_link_ksettings(netdev, &safe_ks);

	/* set autoneg back to what it currently is */
	copy_ks.base.autoneg = safe_ks.base.autoneg;
	/* we don't compare the speed */
	copy_ks.base.speed = safe_ks.base.speed;

	/* If copy_ks.base and safe_ks.base are not the same now, then they are
	 * trying to set something that we do not support.
	 */
	if (memcmp(&copy_ks.base, &safe_ks.base, sizeof(copy_ks.base))) {
		err = -EOPNOTSUPP;
		goto done;
	}

	while (test_and_set_bit(ICE_CFG_BUSY, pf->state)) {
		timeout--;
		if (!timeout) {
			err = -EBUSY;
			goto done;
		}
		usleep_range(TEST_SET_BITS_SLEEP_MIN, TEST_SET_BITS_SLEEP_MAX);
	}

	/* Copy the current user PHY configuration. The current user PHY
	 * configuration is initialized during probe from PHY capabilities
	 * software mode, and updated on set PHY configuration.
	 */
	config = pi->phy.curr_user_phy_cfg;

	config.caps |= ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;

	/* Check autoneg */
	err = ice_setup_autoneg(pi, &safe_ks, &config, autoneg, &autoneg_changed,
				netdev);

	if (err)
		goto done;

	/* Call to get the current link speed */
	pi->phy.get_link_info = true;
	err = ice_get_link_status(pi, &linkup);
	if (err)
		goto done;

	curr_link_speed = pi->phy.curr_user_speed_req;
	adv_link_speed = ice_ksettings_find_adv_link_speed(ks);

	/* If speed didn't get set, set it to what it currently is.
	 * This is needed because if advertise is 0 (as it is when autoneg
	 * is disabled) then speed won't get set.
	 */
	if (!adv_link_speed)
		adv_link_speed = curr_link_speed;

	/* Convert the advertise link speeds to their corresponded PHY_TYPE */
	ice_set_phy_type_from_speed(ks, &phy_type_low, &phy_type_high,
				    adv_link_speed);

	if (!autoneg_changed && adv_link_speed == curr_link_speed) {
		netdev_info(netdev, "Nothing changed, exiting without setting anything.\n");
		goto done;
	}

	/* save the requested speeds */
	pi->phy.link_info.req_speeds = adv_link_speed;

	/* set link and auto negotiation so changes take effect */
	config.caps |= ICE_AQ_PHY_ENA_LINK;

	/* check if there is a PHY type for the requested advertised speed */
	if (!(phy_type_low || phy_type_high)) {
		netdev_info(netdev, "The selected speed is not supported by the current media. Please select a link speed that is supported by the current media.\n");
		err = -EOPNOTSUPP;
		goto done;
	}

	/* intersect requested advertised speed PHY types with media PHY types
	 * for set PHY configuration
	 */
	config.phy_type_high = cpu_to_le64(phy_type_high) &
			phy_caps->phy_type_high;
	config.phy_type_low = cpu_to_le64(phy_type_low) &
			phy_caps->phy_type_low;

	if (!(config.phy_type_high || config.phy_type_low)) {
		/* If there is no intersection and lenient mode is enabled, then
		 * intersect the requested advertised speed with NVM media type
		 * PHY types.
		 */
		if (test_bit(ICE_FLAG_LINK_LENIENT_MODE_ENA, pf->flags)) {
			config.phy_type_high = cpu_to_le64(phy_type_high) &
					       pf->nvm_phy_type_hi;
			config.phy_type_low = cpu_to_le64(phy_type_low) &
					      pf->nvm_phy_type_lo;
		} else {
			netdev_info(netdev, "The selected speed is not supported by the current media. Please select a link speed that is supported by the current media.\n");
			err = -EOPNOTSUPP;
			goto done;
		}
	}

	/* If link is up put link down */
	if (pi->phy.link_info.link_info & ICE_AQ_LINK_UP) {
		/* Tell the OS link is going down, the link will go
		 * back up when fw says it is ready asynchronously
		 */
		ice_print_link_msg(np->vsi, false);
		netif_carrier_off(netdev);
		netif_tx_stop_all_queues(netdev);
	}

	/* make the aq call */
	err = ice_aq_set_phy_cfg(&pf->hw, pi, &config, NULL);
	if (err) {
		netdev_info(netdev, "Set phy config failed,\n");
		goto done;
	}

	/* Save speed request */
	pi->phy.curr_user_speed_req = adv_link_speed;
done:
	kfree(phy_caps);
	clear_bit(ICE_CFG_BUSY, pf->state);

	return err;
}

static u32 ice_parse_hdrs(const struct ethtool_rxfh_fields *nfc)
{
	u32 hdrs = ICE_FLOW_SEG_HDR_NONE;

	switch (nfc->flow_type) {
	case TCP_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_TCP | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case UDP_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_UDP | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case SCTP_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_SCTP | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case GTPU_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPU_IP | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case GTPC_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPC | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case GTPC_TEID_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPC_TEID | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case GTPU_EH_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPU_EH | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case GTPU_UL_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPU_UP | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case GTPU_DL_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPU_DWN | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case TCP_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_TCP | ICE_FLOW_SEG_HDR_IPV6;
		break;
	case UDP_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_UDP | ICE_FLOW_SEG_HDR_IPV6;
		break;
	case SCTP_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_SCTP | ICE_FLOW_SEG_HDR_IPV6;
		break;
	case GTPU_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPU_IP | ICE_FLOW_SEG_HDR_IPV6;
		break;
	case GTPC_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPC | ICE_FLOW_SEG_HDR_IPV6;
		break;
	case GTPC_TEID_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPC_TEID | ICE_FLOW_SEG_HDR_IPV6;
		break;
	case GTPU_EH_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPU_EH | ICE_FLOW_SEG_HDR_IPV6;
		break;
	case GTPU_UL_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPU_UP | ICE_FLOW_SEG_HDR_IPV6;
		break;
	case GTPU_DL_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_GTPU_DWN | ICE_FLOW_SEG_HDR_IPV6;
		break;
	default:
		break;
	}
	return hdrs;
}

static u64 ice_parse_hash_flds(const struct ethtool_rxfh_fields *nfc, bool symm)
{
	u64 hfld = ICE_HASH_INVALID;

	if (nfc->data & RXH_IP_SRC || nfc->data & RXH_IP_DST) {
		switch (nfc->flow_type) {
		case TCP_V4_FLOW:
		case UDP_V4_FLOW:
		case SCTP_V4_FLOW:
		case GTPU_V4_FLOW:
		case GTPC_V4_FLOW:
		case GTPC_TEID_V4_FLOW:
		case GTPU_EH_V4_FLOW:
		case GTPU_UL_V4_FLOW:
		case GTPU_DL_V4_FLOW:
			if (nfc->data & RXH_IP_SRC)
				hfld |= ICE_FLOW_HASH_FLD_IPV4_SA;
			if (nfc->data & RXH_IP_DST)
				hfld |= ICE_FLOW_HASH_FLD_IPV4_DA;
			break;
		case TCP_V6_FLOW:
		case UDP_V6_FLOW:
		case SCTP_V6_FLOW:
		case GTPU_V6_FLOW:
		case GTPC_V6_FLOW:
		case GTPC_TEID_V6_FLOW:
		case GTPU_EH_V6_FLOW:
		case GTPU_UL_V6_FLOW:
		case GTPU_DL_V6_FLOW:
			if (nfc->data & RXH_IP_SRC)
				hfld |= ICE_FLOW_HASH_FLD_IPV6_SA;
			if (nfc->data & RXH_IP_DST)
				hfld |= ICE_FLOW_HASH_FLD_IPV6_DA;
			break;
		default:
			break;
		}
	}

	if (nfc->data & RXH_L4_B_0_1 || nfc->data & RXH_L4_B_2_3) {
		switch (nfc->flow_type) {
		case TCP_V4_FLOW:
		case TCP_V6_FLOW:
			if (nfc->data & RXH_L4_B_0_1)
				hfld |= ICE_FLOW_HASH_FLD_TCP_SRC_PORT;
			if (nfc->data & RXH_L4_B_2_3)
				hfld |= ICE_FLOW_HASH_FLD_TCP_DST_PORT;
			break;
		case UDP_V4_FLOW:
		case UDP_V6_FLOW:
			if (nfc->data & RXH_L4_B_0_1)
				hfld |= ICE_FLOW_HASH_FLD_UDP_SRC_PORT;
			if (nfc->data & RXH_L4_B_2_3)
				hfld |= ICE_FLOW_HASH_FLD_UDP_DST_PORT;
			break;
		case SCTP_V4_FLOW:
		case SCTP_V6_FLOW:
			if (nfc->data & RXH_L4_B_0_1)
				hfld |= ICE_FLOW_HASH_FLD_SCTP_SRC_PORT;
			if (nfc->data & RXH_L4_B_2_3)
				hfld |= ICE_FLOW_HASH_FLD_SCTP_DST_PORT;
			break;
		default:
			break;
		}
	}

	if (nfc->data & RXH_GTP_TEID) {
		switch (nfc->flow_type) {
		case GTPC_TEID_V4_FLOW:
		case GTPC_TEID_V6_FLOW:
			hfld |= ICE_FLOW_HASH_FLD_GTPC_TEID;
			break;
		case GTPU_V4_FLOW:
		case GTPU_V6_FLOW:
			hfld |= ICE_FLOW_HASH_FLD_GTPU_IP_TEID;
			break;
		case GTPU_EH_V4_FLOW:
		case GTPU_EH_V6_FLOW:
			hfld |= ICE_FLOW_HASH_FLD_GTPU_EH_TEID;
			break;
		case GTPU_UL_V4_FLOW:
		case GTPU_UL_V6_FLOW:
			hfld |= ICE_FLOW_HASH_FLD_GTPU_UP_TEID;
			break;
		case GTPU_DL_V4_FLOW:
		case GTPU_DL_V6_FLOW:
			hfld |= ICE_FLOW_HASH_FLD_GTPU_DWN_TEID;
			break;
		default:
			break;
		}
	}

	return hfld;
}

static int
ice_set_rxfh_fields(struct net_device *netdev,
		    const struct ethtool_rxfh_fields *nfc,
		    struct netlink_ext_ack *extack)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct ice_rss_hash_cfg cfg;
	struct device *dev;
	u64 hashed_flds;
	int status;
	bool symm;
	u32 hdrs;

	dev = ice_pf_to_dev(pf);
	if (ice_is_safe_mode(pf)) {
		dev_dbg(dev, "Advanced RSS disabled. Package download failed, vsi num = %d\n",
			vsi->vsi_num);
		return -EINVAL;
	}

	symm = !!(vsi->rss_hfunc == ICE_AQ_VSI_Q_OPT_RSS_HASH_SYM_TPLZ);
	hashed_flds = ice_parse_hash_flds(nfc, symm);
	if (hashed_flds == ICE_HASH_INVALID) {
		dev_dbg(dev, "Invalid hash fields, vsi num = %d\n",
			vsi->vsi_num);
		return -EINVAL;
	}

	hdrs = ice_parse_hdrs(nfc);
	if (hdrs == ICE_FLOW_SEG_HDR_NONE) {
		dev_dbg(dev, "Header type is not valid, vsi num = %d\n",
			vsi->vsi_num);
		return -EINVAL;
	}

	cfg.hash_flds = hashed_flds;
	cfg.addl_hdrs = hdrs;
	cfg.hdr_type = ICE_RSS_ANY_HEADERS;
	cfg.symm = symm;

	status = ice_add_rss_cfg(&pf->hw, vsi, &cfg);
	if (status) {
		dev_dbg(dev, "ice_add_rss_cfg failed, vsi num = %d, error = %d\n",
			vsi->vsi_num, status);
		return status;
	}

	return 0;
}

static int
ice_get_rxfh_fields(struct net_device *netdev, struct ethtool_rxfh_fields *nfc)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct device *dev;
	u64 hash_flds;
	bool symm;
	u32 hdrs;

	dev = ice_pf_to_dev(pf);

	nfc->data = 0;
	if (ice_is_safe_mode(pf)) {
		dev_dbg(dev, "Advanced RSS disabled. Package download failed, vsi num = %d\n",
			vsi->vsi_num);
		return 0;
	}

	hdrs = ice_parse_hdrs(nfc);
	if (hdrs == ICE_FLOW_SEG_HDR_NONE) {
		dev_dbg(dev, "Header type is not valid, vsi num = %d\n",
			vsi->vsi_num);
		return 0;
	}

	hash_flds = ice_get_rss_cfg(&pf->hw, vsi->idx, hdrs, &symm);
	if (hash_flds == ICE_HASH_INVALID) {
		dev_dbg(dev, "No hash fields found for the given header type, vsi num = %d\n",
			vsi->vsi_num);
		return 0;
	}

	if (hash_flds & ICE_FLOW_HASH_FLD_IPV4_SA ||
	    hash_flds & ICE_FLOW_HASH_FLD_IPV6_SA)
		nfc->data |= (u64)RXH_IP_SRC;

	if (hash_flds & ICE_FLOW_HASH_FLD_IPV4_DA ||
	    hash_flds & ICE_FLOW_HASH_FLD_IPV6_DA)
		nfc->data |= (u64)RXH_IP_DST;

	if (hash_flds & ICE_FLOW_HASH_FLD_TCP_SRC_PORT ||
	    hash_flds & ICE_FLOW_HASH_FLD_UDP_SRC_PORT ||
	    hash_flds & ICE_FLOW_HASH_FLD_SCTP_SRC_PORT)
		nfc->data |= (u64)RXH_L4_B_0_1;

	if (hash_flds & ICE_FLOW_HASH_FLD_TCP_DST_PORT ||
	    hash_flds & ICE_FLOW_HASH_FLD_UDP_DST_PORT ||
	    hash_flds & ICE_FLOW_HASH_FLD_SCTP_DST_PORT)
		nfc->data |= (u64)RXH_L4_B_2_3;

	if (hash_flds & ICE_FLOW_HASH_FLD_GTPC_TEID ||
	    hash_flds & ICE_FLOW_HASH_FLD_GTPU_IP_TEID ||
	    hash_flds & ICE_FLOW_HASH_FLD_GTPU_EH_TEID ||
	    hash_flds & ICE_FLOW_HASH_FLD_GTPU_UP_TEID ||
	    hash_flds & ICE_FLOW_HASH_FLD_GTPU_DWN_TEID)
		nfc->data |= (u64)RXH_GTP_TEID;

	return 0;
}

/**
 * ice_set_rxnfc - command to set Rx flow rules.
 * @netdev: network interface device structure
 * @cmd: ethtool rxnfc command
 *
 * Returns 0 for success and negative values for errors
 */
static int ice_set_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		return ice_add_fdir_ethtool(vsi, cmd);
	case ETHTOOL_SRXCLSRLDEL:
		return ice_del_fdir_ethtool(vsi, cmd);
	default:
		break;
	}
	return -EOPNOTSUPP;
}

/**
 * ice_get_rxnfc - command to get Rx flow classification rules
 * @netdev: network interface device structure
 * @cmd: ethtool rxnfc command
 * @rule_locs: buffer to rturn Rx flow classification rules
 *
 * Returns Success if the command is supported.
 */
static int
ice_get_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd,
	      u32 __always_unused *rule_locs)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	int ret = -EOPNOTSUPP;
	struct ice_hw *hw;

	hw = &vsi->back->hw;

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		cmd->data = vsi->rss_size;
		ret = 0;
		break;
	case ETHTOOL_GRXCLSRLCNT:
		cmd->rule_cnt = hw->fdir_active_fltr;
		/* report total rule count */
		cmd->data = ice_get_fdir_cnt_all(hw);
		ret = 0;
		break;
	case ETHTOOL_GRXCLSRULE:
		ret = ice_get_ethtool_fdir_entry(hw, cmd);
		break;
	case ETHTOOL_GRXCLSRLALL:
		ret = ice_get_fdir_fltr_ids(hw, cmd, (u32 *)rule_locs);
		break;
	default:
		break;
	}

	return ret;
}

static void
ice_get_ringparam(struct net_device *netdev, struct ethtool_ringparam *ring,
		  struct kernel_ethtool_ringparam *kernel_ring,
		  struct netlink_ext_ack *extack)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;

	ring->rx_max_pending = ICE_MAX_NUM_DESC;
	ring->tx_max_pending = ICE_MAX_NUM_DESC;
	if (vsi->tx_rings && vsi->rx_rings) {
		ring->rx_pending = vsi->rx_rings[0]->count;
		ring->tx_pending = vsi->tx_rings[0]->count;
	} else {
		ring->rx_pending = 0;
		ring->tx_pending = 0;
	}

	/* Rx mini and jumbo rings are not supported */
	ring->rx_mini_max_pending = 0;
	ring->rx_jumbo_max_pending = 0;
	ring->rx_mini_pending = 0;
	ring->rx_jumbo_pending = 0;
}

static int
ice_set_ringparam(struct net_device *netdev, struct ethtool_ringparam *ring,
		  struct kernel_ethtool_ringparam *kernel_ring,
		  struct netlink_ext_ack *extack)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_tx_ring *xdp_rings = NULL;
	struct ice_tx_ring *tx_rings = NULL;
	struct ice_rx_ring *rx_rings = NULL;
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	int i, timeout = 50, err = 0;
	u16 new_rx_cnt, new_tx_cnt;

	if (ring->tx_pending > ICE_MAX_NUM_DESC ||
	    ring->tx_pending < ICE_MIN_NUM_DESC ||
	    ring->rx_pending > ICE_MAX_NUM_DESC ||
	    ring->rx_pending < ICE_MIN_NUM_DESC) {
		netdev_err(netdev, "Descriptors requested (Tx: %d / Rx: %d) out of range [%d-%d] (increment %d)\n",
			   ring->tx_pending, ring->rx_pending,
			   ICE_MIN_NUM_DESC, ICE_MAX_NUM_DESC,
			   ICE_REQ_DESC_MULTIPLE);
		return -EINVAL;
	}

	/* Return if there is no rings (device is reloading) */
	if (!vsi->tx_rings || !vsi->rx_rings)
		return -EBUSY;

	new_tx_cnt = ALIGN(ring->tx_pending, ICE_REQ_DESC_MULTIPLE);
	if (new_tx_cnt != ring->tx_pending)
		netdev_info(netdev, "Requested Tx descriptor count rounded up to %d\n",
			    new_tx_cnt);
	new_rx_cnt = ALIGN(ring->rx_pending, ICE_REQ_DESC_MULTIPLE);
	if (new_rx_cnt != ring->rx_pending)
		netdev_info(netdev, "Requested Rx descriptor count rounded up to %d\n",
			    new_rx_cnt);

	/* if nothing to do return success */
	if (new_tx_cnt == vsi->tx_rings[0]->count &&
	    new_rx_cnt == vsi->rx_rings[0]->count) {
		netdev_dbg(netdev, "Nothing to change, descriptor count is same as requested\n");
		return 0;
	}

	/* If there is a AF_XDP UMEM attached to any of Rx rings,
	 * disallow changing the number of descriptors -- regardless
	 * if the netdev is running or not.
	 */
	if (ice_xsk_any_rx_ring_ena(vsi))
		return -EBUSY;

	while (test_and_set_bit(ICE_CFG_BUSY, pf->state)) {
		timeout--;
		if (!timeout)
			return -EBUSY;
		usleep_range(1000, 2000);
	}

	/* set for the next time the netdev is started */
	if (!netif_running(vsi->netdev)) {
		ice_for_each_alloc_txq(vsi, i)
			vsi->tx_rings[i]->count = new_tx_cnt;
		ice_for_each_alloc_rxq(vsi, i)
			vsi->rx_rings[i]->count = new_rx_cnt;
		if (ice_is_xdp_ena_vsi(vsi))
			ice_for_each_xdp_txq(vsi, i)
				vsi->xdp_rings[i]->count = new_tx_cnt;
		vsi->num_tx_desc = (u16)new_tx_cnt;
		vsi->num_rx_desc = (u16)new_rx_cnt;
		netdev_dbg(netdev, "Link is down, descriptor count change happens when link is brought up\n");
		goto done;
	}

	if (new_tx_cnt == vsi->tx_rings[0]->count)
		goto process_rx;

	/* alloc updated Tx resources */
	netdev_info(netdev, "Changing Tx descriptor count from %d to %d\n",
		    vsi->tx_rings[0]->count, new_tx_cnt);

	tx_rings = kcalloc(vsi->num_txq, sizeof(*tx_rings), GFP_KERNEL);
	if (!tx_rings) {
		err = -ENOMEM;
		goto done;
	}

	ice_for_each_txq(vsi, i) {
		/* clone ring and setup updated count */
		tx_rings[i] = *vsi->tx_rings[i];
		tx_rings[i].count = new_tx_cnt;
		tx_rings[i].desc = NULL;
		tx_rings[i].tx_buf = NULL;
		tx_rings[i].tx_tstamps = &pf->ptp.port.tx;
		err = ice_setup_tx_ring(&tx_rings[i]);
		if (err) {
			while (i--)
				ice_clean_tx_ring(&tx_rings[i]);
			kfree(tx_rings);
			goto done;
		}
	}

	if (!ice_is_xdp_ena_vsi(vsi))
		goto process_rx;

	/* alloc updated XDP resources */
	netdev_info(netdev, "Changing XDP descriptor count from %d to %d\n",
		    vsi->xdp_rings[0]->count, new_tx_cnt);

	xdp_rings = kcalloc(vsi->num_xdp_txq, sizeof(*xdp_rings), GFP_KERNEL);
	if (!xdp_rings) {
		err = -ENOMEM;
		goto free_tx;
	}

	ice_for_each_xdp_txq(vsi, i) {
		/* clone ring and setup updated count */
		xdp_rings[i] = *vsi->xdp_rings[i];
		xdp_rings[i].count = new_tx_cnt;
		xdp_rings[i].desc = NULL;
		xdp_rings[i].tx_buf = NULL;
		err = ice_setup_tx_ring(&xdp_rings[i]);
		if (err) {
			while (i--)
				ice_clean_tx_ring(&xdp_rings[i]);
			kfree(xdp_rings);
			goto free_tx;
		}
		ice_set_ring_xdp(&xdp_rings[i]);
	}

process_rx:
	if (new_rx_cnt == vsi->rx_rings[0]->count)
		goto process_link;

	/* alloc updated Rx resources */
	netdev_info(netdev, "Changing Rx descriptor count from %d to %d\n",
		    vsi->rx_rings[0]->count, new_rx_cnt);

	rx_rings = kcalloc(vsi->num_rxq, sizeof(*rx_rings), GFP_KERNEL);
	if (!rx_rings) {
		err = -ENOMEM;
		goto done;
	}

	ice_for_each_rxq(vsi, i) {
		/* clone ring and setup updated count */
		rx_rings[i] = *vsi->rx_rings[i];
		rx_rings[i].count = new_rx_cnt;
		rx_rings[i].cached_phctime = pf->ptp.cached_phc_time;
		rx_rings[i].desc = NULL;
		rx_rings[i].rx_buf = NULL;
		/* this is to allow wr32 to have something to write to
		 * during early allocation of Rx buffers
		 */
		rx_rings[i].tail = vsi->back->hw.hw_addr + PRTGEN_STATUS;

		err = ice_setup_rx_ring(&rx_rings[i]);
		if (err)
			goto rx_unwind;

		/* allocate Rx buffers */
		err = ice_alloc_rx_bufs(&rx_rings[i],
					ICE_RX_DESC_UNUSED(&rx_rings[i]));
rx_unwind:
		if (err) {
			while (i) {
				i--;
				ice_free_rx_ring(&rx_rings[i]);
			}
			kfree(rx_rings);
			err = -ENOMEM;
			goto free_tx;
		}
	}

process_link:
	/* Bring interface down, copy in the new ring info, then restore the
	 * interface. if VSI is up, bring it down and then back up
	 */
	if (!test_and_set_bit(ICE_VSI_DOWN, vsi->state)) {
		ice_down(vsi);

		if (tx_rings) {
			ice_for_each_txq(vsi, i) {
				ice_free_tx_ring(vsi->tx_rings[i]);
				*vsi->tx_rings[i] = tx_rings[i];
			}
			kfree(tx_rings);
		}

		if (rx_rings) {
			ice_for_each_rxq(vsi, i) {
				ice_free_rx_ring(vsi->rx_rings[i]);
				/* copy the real tail offset */
				rx_rings[i].tail = vsi->rx_rings[i]->tail;
				/* this is to fake out the allocation routine
				 * into thinking it has to realloc everything
				 * but the recycling logic will let us re-use
				 * the buffers allocated above
				 */
				rx_rings[i].next_to_use = 0;
				rx_rings[i].next_to_clean = 0;
				rx_rings[i].next_to_alloc = 0;
				*vsi->rx_rings[i] = rx_rings[i];
			}
			kfree(rx_rings);
		}

		if (xdp_rings) {
			ice_for_each_xdp_txq(vsi, i) {
				ice_free_tx_ring(vsi->xdp_rings[i]);
				*vsi->xdp_rings[i] = xdp_rings[i];
			}
			kfree(xdp_rings);
		}

		vsi->num_tx_desc = new_tx_cnt;
		vsi->num_rx_desc = new_rx_cnt;
		ice_up(vsi);
	}
	goto done;

free_tx:
	/* error cleanup if the Rx allocations failed after getting Tx */
	if (tx_rings) {
		ice_for_each_txq(vsi, i)
			ice_free_tx_ring(&tx_rings[i]);
		kfree(tx_rings);
	}

done:
	clear_bit(ICE_CFG_BUSY, pf->state);
	return err;
}

/**
 * ice_get_pauseparam - Get Flow Control status
 * @netdev: network interface device structure
 * @pause: ethernet pause (flow control) parameters
 *
 * Get requested flow control status from PHY capability.
 * If autoneg is true, then ethtool will send the ETHTOOL_GSET ioctl which
 * is handled by ice_get_link_ksettings. ice_get_link_ksettings will report
 * the negotiated Rx/Tx pause via lp_advertising.
 */
static void
ice_get_pauseparam(struct net_device *netdev, struct ethtool_pauseparam *pause)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_port_info *pi = np->vsi->port_info;
	struct ice_aqc_get_phy_caps_data *pcaps;
	struct ice_dcbx_cfg *dcbx_cfg;
	int status;

	/* Initialize pause params */
	pause->rx_pause = 0;
	pause->tx_pause = 0;

	dcbx_cfg = &pi->qos_cfg.local_dcbx_cfg;

	pcaps = kzalloc(sizeof(*pcaps), GFP_KERNEL);
	if (!pcaps)
		return;

	/* Get current PHY config */
	status = ice_aq_get_phy_caps(pi, false, ICE_AQC_REPORT_ACTIVE_CFG, pcaps,
				     NULL);
	if (status)
		goto out;

	pause->autoneg = ice_is_phy_caps_an_enabled(pcaps) ? AUTONEG_ENABLE :
							     AUTONEG_DISABLE;

	if (dcbx_cfg->pfc.pfcena)
		/* PFC enabled so report LFC as off */
		goto out;

	if (pcaps->caps & ICE_AQC_PHY_EN_TX_LINK_PAUSE)
		pause->tx_pause = 1;
	if (pcaps->caps & ICE_AQC_PHY_EN_RX_LINK_PAUSE)
		pause->rx_pause = 1;

out:
	kfree(pcaps);
}

/**
 * ice_set_pauseparam - Set Flow Control parameter
 * @netdev: network interface device structure
 * @pause: return Tx/Rx flow control status
 */
static int
ice_set_pauseparam(struct net_device *netdev, struct ethtool_pauseparam *pause)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_aqc_get_phy_caps_data *pcaps;
	struct ice_link_status *hw_link_info;
	struct ice_pf *pf = np->vsi->back;
	struct ice_dcbx_cfg *dcbx_cfg;
	struct ice_vsi *vsi = np->vsi;
	struct ice_hw *hw = &pf->hw;
	struct ice_port_info *pi;
	u8 aq_failures;
	bool link_up;
	u32 is_an;
	int err;

	pi = vsi->port_info;
	hw_link_info = &pi->phy.link_info;
	dcbx_cfg = &pi->qos_cfg.local_dcbx_cfg;
	link_up = hw_link_info->link_info & ICE_AQ_LINK_UP;

	/* Changing the port's flow control is not supported if this isn't the
	 * PF VSI
	 */
	if (vsi->type != ICE_VSI_PF) {
		netdev_info(netdev, "Changing flow control parameters only supported for PF VSI\n");
		return -EOPNOTSUPP;
	}

	/* Get pause param reports configured and negotiated flow control pause
	 * when ETHTOOL_GLINKSETTINGS is defined. Since ETHTOOL_GLINKSETTINGS is
	 * defined get pause param pause->autoneg reports SW configured setting,
	 * so compare pause->autoneg with SW configured to prevent the user from
	 * using set pause param to chance autoneg.
	 */
	pcaps = kzalloc(sizeof(*pcaps), GFP_KERNEL);
	if (!pcaps)
		return -ENOMEM;

	/* Get current PHY config */
	err = ice_aq_get_phy_caps(pi, false, ICE_AQC_REPORT_ACTIVE_CFG, pcaps,
				  NULL);
	if (err) {
		kfree(pcaps);
		return err;
	}

	is_an = ice_is_phy_caps_an_enabled(pcaps) ? AUTONEG_ENABLE :
						    AUTONEG_DISABLE;

	kfree(pcaps);

	if (pause->autoneg != is_an) {
		netdev_info(netdev, "To change autoneg please use: ethtool -s <dev> autoneg <on|off>\n");
		return -EOPNOTSUPP;
	}

	/* If we have link and don't have autoneg */
	if (!test_bit(ICE_DOWN, pf->state) &&
	    !(hw_link_info->an_info & ICE_AQ_AN_COMPLETED)) {
		/* Send message that it might not necessarily work*/
		netdev_info(netdev, "Autoneg did not complete so changing settings may not result in an actual change.\n");
	}

	if (dcbx_cfg->pfc.pfcena) {
		netdev_info(netdev, "Priority flow control enabled. Cannot set link flow control.\n");
		return -EOPNOTSUPP;
	}
	if (pause->rx_pause && pause->tx_pause)
		pi->fc.req_mode = ICE_FC_FULL;
	else if (pause->rx_pause && !pause->tx_pause)
		pi->fc.req_mode = ICE_FC_RX_PAUSE;
	else if (!pause->rx_pause && pause->tx_pause)
		pi->fc.req_mode = ICE_FC_TX_PAUSE;
	else if (!pause->rx_pause && !pause->tx_pause)
		pi->fc.req_mode = ICE_FC_NONE;
	else
		return -EINVAL;

	/* Set the FC mode and only restart AN if link is up */
	err = ice_set_fc(pi, &aq_failures, link_up);

	if (aq_failures & ICE_SET_FC_AQ_FAIL_GET) {
		netdev_info(netdev, "Set fc failed on the get_phy_capabilities call with err %d aq_err %s\n",
			    err, libie_aq_str(hw->adminq.sq_last_status));
		err = -EAGAIN;
	} else if (aq_failures & ICE_SET_FC_AQ_FAIL_SET) {
		netdev_info(netdev, "Set fc failed on the set_phy_config call with err %d aq_err %s\n",
			    err, libie_aq_str(hw->adminq.sq_last_status));
		err = -EAGAIN;
	} else if (aq_failures & ICE_SET_FC_AQ_FAIL_UPDATE) {
		netdev_info(netdev, "Set fc failed on the get_link_info call with err %d aq_err %s\n",
			    err, libie_aq_str(hw->adminq.sq_last_status));
		err = -EAGAIN;
	}

	return err;
}

/**
 * ice_get_rxfh_key_size - get the RSS hash key size
 * @netdev: network interface device structure
 *
 * Returns the table size.
 */
static u32 ice_get_rxfh_key_size(struct net_device __always_unused *netdev)
{
	return ICE_VSIQF_HKEY_ARRAY_SIZE;
}

/**
 * ice_get_rxfh_indir_size - get the Rx flow hash indirection table size
 * @netdev: network interface device structure
 *
 * Returns the table size.
 */
static u32 ice_get_rxfh_indir_size(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);

	return np->vsi->rss_table_size;
}

/**
 * ice_get_rxfh - get the Rx flow hash indirection table
 * @netdev: network interface device structure
 * @rxfh: pointer to param struct (indir, key, hfunc)
 *
 * Reads the indirection table directly from the hardware.
 */
static int
ice_get_rxfh(struct net_device *netdev, struct ethtool_rxfh_param *rxfh)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	u16 qcount, offset;
	int err, i;
	u8 *lut;

	if (!test_bit(ICE_FLAG_RSS_ENA, pf->flags)) {
		netdev_warn(netdev, "RSS is not supported on this VSI!\n");
		return -EOPNOTSUPP;
	}

	qcount = vsi->mqprio_qopt.qopt.count[0];
	offset = vsi->mqprio_qopt.qopt.offset[0];

	rxfh->hfunc = ETH_RSS_HASH_TOP;
	if (vsi->rss_hfunc == ICE_AQ_VSI_Q_OPT_RSS_HASH_SYM_TPLZ)
		rxfh->input_xfrm |= RXH_XFRM_SYM_XOR;

	if (!rxfh->indir)
		return 0;

	lut = kzalloc(vsi->rss_table_size, GFP_KERNEL);
	if (!lut)
		return -ENOMEM;

	err = ice_get_rss_key(vsi, rxfh->key);
	if (err)
		goto out;

	err = ice_get_rss_lut(vsi, lut, vsi->rss_table_size);
	if (err)
		goto out;

	if (ice_is_adq_active(pf)) {
		for (i = 0; i < vsi->rss_table_size; i++)
			rxfh->indir[i] = offset + lut[i] % qcount;
		goto out;
	}

	for (i = 0; i < vsi->rss_table_size; i++)
		rxfh->indir[i] = lut[i];

out:
	kfree(lut);
	return err;
}

/**
 * ice_set_rxfh - set the Rx flow hash indirection table
 * @netdev: network interface device structure
 * @rxfh: pointer to param struct (indir, key, hfunc)
 * @extack: extended ACK from the Netlink message
 *
 * Returns -EINVAL if the table specifies an invalid queue ID, otherwise
 * returns 0 after programming the table.
 */
static int
ice_set_rxfh(struct net_device *netdev, struct ethtool_rxfh_param *rxfh,
	     struct netlink_ext_ack *extack)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	u8 hfunc = ICE_AQ_VSI_Q_OPT_RSS_HASH_TPLZ;
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct device *dev;
	int err;

	dev = ice_pf_to_dev(pf);
	if (rxfh->hfunc != ETH_RSS_HASH_NO_CHANGE &&
	    rxfh->hfunc != ETH_RSS_HASH_TOP)
		return -EOPNOTSUPP;

	if (!test_bit(ICE_FLAG_RSS_ENA, pf->flags)) {
		/* RSS not supported return error here */
		netdev_warn(netdev, "RSS is not configured on this VSI!\n");
		return -EIO;
	}

	if (ice_is_adq_active(pf)) {
		netdev_err(netdev, "Cannot change RSS params with ADQ configured.\n");
		return -EOPNOTSUPP;
	}

	/* Update the VSI's hash function */
	if (rxfh->input_xfrm & RXH_XFRM_SYM_XOR)
		hfunc = ICE_AQ_VSI_Q_OPT_RSS_HASH_SYM_TPLZ;

	err = ice_set_rss_hfunc(vsi, hfunc);
	if (err)
		return err;

	if (rxfh->key) {
		if (!vsi->rss_hkey_user) {
			vsi->rss_hkey_user =
				devm_kzalloc(dev, ICE_VSIQF_HKEY_ARRAY_SIZE,
					     GFP_KERNEL);
			if (!vsi->rss_hkey_user)
				return -ENOMEM;
		}
		memcpy(vsi->rss_hkey_user, rxfh->key,
		       ICE_VSIQF_HKEY_ARRAY_SIZE);

		err = ice_set_rss_key(vsi, vsi->rss_hkey_user);
		if (err)
			return err;
	}

	if (!vsi->rss_lut_user) {
		vsi->rss_lut_user = devm_kzalloc(dev, vsi->rss_table_size,
						 GFP_KERNEL);
		if (!vsi->rss_lut_user)
			return -ENOMEM;
	}

	/* Each 32 bits pointed by 'indir' is stored with a lut entry */
	if (rxfh->indir) {
		int i;

		for (i = 0; i < vsi->rss_table_size; i++)
			vsi->rss_lut_user[i] = (u8)(rxfh->indir[i]);
	} else {
		ice_fill_rss_lut(vsi->rss_lut_user, vsi->rss_table_size,
				 vsi->rss_size);
	}

	err = ice_set_rss_lut(vsi, vsi->rss_lut_user, vsi->rss_table_size);
	if (err)
		return err;

	return 0;
}

static int
ice_get_ts_info(struct net_device *dev, struct kernel_ethtool_ts_info *info)
{
	struct ice_pf *pf = ice_netdev_to_pf(dev);

	/* only report timestamping if PTP is enabled */
	if (pf->ptp.state != ICE_PTP_READY)
		return ethtool_op_get_ts_info(dev, info);

	info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
				SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE;

	info->phc_index = ice_ptp_clock_index(pf);

	info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);

	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) | BIT(HWTSTAMP_FILTER_ALL);

	return 0;
}

/**
 * ice_get_max_txq - return the maximum number of Tx queues for in a PF
 * @pf: PF structure
 */
static int ice_get_max_txq(struct ice_pf *pf)
{
	return min(num_online_cpus(), pf->hw.func_caps.common_cap.num_txq);
}

/**
 * ice_get_max_rxq - return the maximum number of Rx queues for in a PF
 * @pf: PF structure
 */
static int ice_get_max_rxq(struct ice_pf *pf)
{
	return min(num_online_cpus(), pf->hw.func_caps.common_cap.num_rxq);
}

/**
 * ice_get_combined_cnt - return the current number of combined channels
 * @vsi: PF VSI pointer
 *
 * Go through all queue vectors and count ones that have both Rx and Tx ring
 * attached
 */
static u32 ice_get_combined_cnt(struct ice_vsi *vsi)
{
	u32 combined = 0;
	int q_idx;

	ice_for_each_q_vector(vsi, q_idx) {
		struct ice_q_vector *q_vector = vsi->q_vectors[q_idx];

		combined += min(q_vector->num_ring_tx, q_vector->num_ring_rx);
	}

	return combined;
}

/**
 * ice_get_channels - get the current and max supported channels
 * @dev: network interface device structure
 * @ch: ethtool channel data structure
 */
static void
ice_get_channels(struct net_device *dev, struct ethtool_channels *ch)
{
	struct ice_netdev_priv *np = netdev_priv(dev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;

	/* report maximum channels */
	ch->max_rx = ice_get_max_rxq(pf);
	ch->max_tx = ice_get_max_txq(pf);
	ch->max_combined = min_t(int, ch->max_rx, ch->max_tx);

	/* report current channels */
	ch->combined_count = ice_get_combined_cnt(vsi);
	ch->rx_count = vsi->num_rxq - ch->combined_count;
	ch->tx_count = vsi->num_txq - ch->combined_count;

	/* report other queues */
	ch->other_count = test_bit(ICE_FLAG_FD_ENA, pf->flags) ? 1 : 0;
	ch->max_other = ch->other_count;
}

/**
 * ice_get_valid_rss_size - return valid number of RSS queues
 * @hw: pointer to the HW structure
 * @new_size: requested RSS queues
 */
static int ice_get_valid_rss_size(struct ice_hw *hw, int new_size)
{
	struct ice_hw_common_caps *caps = &hw->func_caps.common_cap;

	return min_t(int, new_size, BIT(caps->rss_table_entry_width));
}

/**
 * ice_vsi_set_dflt_rss_lut - set default RSS LUT with requested RSS size
 * @vsi: VSI to reconfigure RSS LUT on
 * @req_rss_size: requested range of queue numbers for hashing
 *
 * Set the VSI's RSS parameters, configure the RSS LUT based on these.
 */
static int ice_vsi_set_dflt_rss_lut(struct ice_vsi *vsi, int req_rss_size)
{
	struct ice_pf *pf = vsi->back;
	struct device *dev;
	struct ice_hw *hw;
	int err;
	u8 *lut;

	dev = ice_pf_to_dev(pf);
	hw = &pf->hw;

	if (!req_rss_size)
		return -EINVAL;

	lut = kzalloc(vsi->rss_table_size, GFP_KERNEL);
	if (!lut)
		return -ENOMEM;

	/* set RSS LUT parameters */
	if (!test_bit(ICE_FLAG_RSS_ENA, pf->flags))
		vsi->rss_size = 1;
	else
		vsi->rss_size = ice_get_valid_rss_size(hw, req_rss_size);

	/* create/set RSS LUT */
	ice_fill_rss_lut(lut, vsi->rss_table_size, vsi->rss_size);
	err = ice_set_rss_lut(vsi, lut, vsi->rss_table_size);
	if (err)
		dev_err(dev, "Cannot set RSS lut, err %d aq_err %s\n", err,
			libie_aq_str(hw->adminq.sq_last_status));

	kfree(lut);
	return err;
}

/**
 * ice_set_channels - set the number channels
 * @dev: network interface device structure
 * @ch: ethtool channel data structure
 */
static int ice_set_channels(struct net_device *dev, struct ethtool_channels *ch)
{
	struct ice_netdev_priv *np = netdev_priv(dev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	int new_rx = 0, new_tx = 0;
	bool locked = false;
	int ret = 0;

	/* do not support changing channels in Safe Mode */
	if (ice_is_safe_mode(pf)) {
		netdev_err(dev, "Changing channel in Safe Mode is not supported\n");
		return -EOPNOTSUPP;
	}
	/* do not support changing other_count */
	if (ch->other_count != (test_bit(ICE_FLAG_FD_ENA, pf->flags) ? 1U : 0U))
		return -EINVAL;

	if (ice_is_adq_active(pf)) {
		netdev_err(dev, "Cannot set channels with ADQ configured.\n");
		return -EOPNOTSUPP;
	}

	if (test_bit(ICE_FLAG_FD_ENA, pf->flags) && pf->hw.fdir_active_fltr) {
		netdev_err(dev, "Cannot set channels when Flow Director filters are active\n");
		return -EOPNOTSUPP;
	}

	if (ch->rx_count && ch->tx_count) {
		netdev_err(dev, "Dedicated RX or TX channels cannot be used simultaneously\n");
		return -EINVAL;
	}

	new_rx = ch->combined_count + ch->rx_count;
	new_tx = ch->combined_count + ch->tx_count;

	if (new_rx < vsi->tc_cfg.numtc) {
		netdev_err(dev, "Cannot set less Rx channels, than Traffic Classes you have (%u)\n",
			   vsi->tc_cfg.numtc);
		return -EINVAL;
	}
	if (new_tx < vsi->tc_cfg.numtc) {
		netdev_err(dev, "Cannot set less Tx channels, than Traffic Classes you have (%u)\n",
			   vsi->tc_cfg.numtc);
		return -EINVAL;
	}
	if (new_rx > ice_get_max_rxq(pf)) {
		netdev_err(dev, "Maximum allowed Rx channels is %d\n",
			   ice_get_max_rxq(pf));
		return -EINVAL;
	}
	if (new_tx > ice_get_max_txq(pf)) {
		netdev_err(dev, "Maximum allowed Tx channels is %d\n",
			   ice_get_max_txq(pf));
		return -EINVAL;
	}

	if (pf->cdev_info && pf->cdev_info->adev) {
		mutex_lock(&pf->adev_mutex);
		device_lock(&pf->cdev_info->adev->dev);
		locked = true;
		if (pf->cdev_info->adev->dev.driver) {
			netdev_err(dev, "Cannot change channels when RDMA is active\n");
			ret = -EBUSY;
			goto adev_unlock;
		}
	}

	ice_vsi_recfg_qs(vsi, new_rx, new_tx, locked);

	if (!netif_is_rxfh_configured(dev)) {
		ret = ice_vsi_set_dflt_rss_lut(vsi, new_rx);
		goto adev_unlock;
	}

	/* Update rss_size due to change in Rx queues */
	vsi->rss_size = ice_get_valid_rss_size(&pf->hw, new_rx);

adev_unlock:
	if (locked) {
		device_unlock(&pf->cdev_info->adev->dev);
		mutex_unlock(&pf->adev_mutex);
	}
	return ret;
}

/**
 * ice_get_wol - get current Wake on LAN configuration
 * @netdev: network interface device structure
 * @wol: Ethtool structure to retrieve WoL settings
 */
static void ice_get_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

	if (np->vsi->type != ICE_VSI_PF)
		netdev_warn(netdev, "Wake on LAN is not supported on this interface!\n");

	/* Get WoL settings based on the HW capability */
	if (ice_is_wol_supported(&pf->hw)) {
		wol->supported = WAKE_MAGIC;
		wol->wolopts = pf->wol_ena ? WAKE_MAGIC : 0;
	} else {
		wol->supported = 0;
		wol->wolopts = 0;
	}
}

/**
 * ice_set_wol - set Wake on LAN on supported device
 * @netdev: network interface device structure
 * @wol: Ethtool structure to set WoL
 */
static int ice_set_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;

	if (vsi->type != ICE_VSI_PF || !ice_is_wol_supported(&pf->hw))
		return -EOPNOTSUPP;

	/* only magic packet is supported */
	if (wol->wolopts && wol->wolopts != WAKE_MAGIC)
		return -EOPNOTSUPP;

	/* Set WoL only if there is a new value */
	if (pf->wol_ena != !!wol->wolopts) {
		pf->wol_ena = !!wol->wolopts;
		device_set_wakeup_enable(ice_pf_to_dev(pf), pf->wol_ena);
		netdev_dbg(netdev, "WoL magic packet %sabled\n",
			   pf->wol_ena ? "en" : "dis");
	}

	return 0;
}

/**
 * ice_get_rc_coalesce - get ITR values for specific ring container
 * @ec: ethtool structure to fill with driver's coalesce settings
 * @rc: ring container that the ITR values will come from
 *
 * Query the device for ice_ring_container specific ITR values. This is
 * done per ice_ring_container because each q_vector can have 1 or more rings
 * and all of said ring(s) will have the same ITR values.
 *
 * Returns 0 on success, negative otherwise.
 */
static int
ice_get_rc_coalesce(struct ethtool_coalesce *ec, struct ice_ring_container *rc)
{
	if (!rc->rx_ring)
		return -EINVAL;

	switch (rc->type) {
	case ICE_RX_CONTAINER:
		ec->use_adaptive_rx_coalesce = ITR_IS_DYNAMIC(rc);
		ec->rx_coalesce_usecs = rc->itr_setting;
		ec->rx_coalesce_usecs_high = rc->rx_ring->q_vector->intrl;
		break;
	case ICE_TX_CONTAINER:
		ec->use_adaptive_tx_coalesce = ITR_IS_DYNAMIC(rc);
		ec->tx_coalesce_usecs = rc->itr_setting;
		break;
	default:
		dev_dbg(ice_pf_to_dev(rc->rx_ring->vsi->back), "Invalid c_type %d\n", rc->type);
		return -EINVAL;
	}

	return 0;
}

/**
 * ice_get_q_coalesce - get a queue's ITR/INTRL (coalesce) settings
 * @vsi: VSI associated to the queue for getting ITR/INTRL (coalesce) settings
 * @ec: coalesce settings to program the device with
 * @q_num: update ITR/INTRL (coalesce) settings for this queue number/index
 *
 * Return 0 on success, and negative under the following conditions:
 * 1. Getting Tx or Rx ITR/INTRL (coalesce) settings failed.
 * 2. The q_num passed in is not a valid number/index for Tx and Rx rings.
 */
static int
ice_get_q_coalesce(struct ice_vsi *vsi, struct ethtool_coalesce *ec, int q_num)
{
	if (q_num < vsi->num_rxq && q_num < vsi->num_txq) {
		if (ice_get_rc_coalesce(ec,
					&vsi->rx_rings[q_num]->q_vector->rx))
			return -EINVAL;
		if (ice_get_rc_coalesce(ec,
					&vsi->tx_rings[q_num]->q_vector->tx))
			return -EINVAL;
	} else if (q_num < vsi->num_rxq) {
		if (ice_get_rc_coalesce(ec,
					&vsi->rx_rings[q_num]->q_vector->rx))
			return -EINVAL;
	} else if (q_num < vsi->num_txq) {
		if (ice_get_rc_coalesce(ec,
					&vsi->tx_rings[q_num]->q_vector->tx))
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	return 0;
}

/**
 * __ice_get_coalesce - get ITR/INTRL values for the device
 * @netdev: pointer to the netdev associated with this query
 * @ec: ethtool structure to fill with driver's coalesce settings
 * @q_num: queue number to get the coalesce settings for
 *
 * If the caller passes in a negative q_num then we return coalesce settings
 * based on queue number 0, else use the actual q_num passed in.
 */
static int
__ice_get_coalesce(struct net_device *netdev, struct ethtool_coalesce *ec,
		   int q_num)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;

	if (q_num < 0)
		q_num = 0;

	if (ice_get_q_coalesce(vsi, ec, q_num))
		return -EINVAL;

	return 0;
}

static int ice_get_coalesce(struct net_device *netdev,
			    struct ethtool_coalesce *ec,
			    struct kernel_ethtool_coalesce *kernel_coal,
			    struct netlink_ext_ack *extack)
{
	return __ice_get_coalesce(netdev, ec, -1);
}

static int
ice_get_per_q_coalesce(struct net_device *netdev, u32 q_num,
		       struct ethtool_coalesce *ec)
{
	return __ice_get_coalesce(netdev, ec, q_num);
}

/**
 * ice_set_rc_coalesce - set ITR values for specific ring container
 * @ec: ethtool structure from user to update ITR settings
 * @rc: ring container that the ITR values will come from
 * @vsi: VSI associated to the ring container
 *
 * Set specific ITR values. This is done per ice_ring_container because each
 * q_vector can have 1 or more rings and all of said ring(s) will have the same
 * ITR values.
 *
 * Returns 0 on success, negative otherwise.
 */
static int
ice_set_rc_coalesce(struct ethtool_coalesce *ec,
		    struct ice_ring_container *rc, struct ice_vsi *vsi)
{
	const char *c_type_str = (rc->type == ICE_RX_CONTAINER) ? "rx" : "tx";
	u32 use_adaptive_coalesce, coalesce_usecs;
	struct ice_pf *pf = vsi->back;
	u16 itr_setting;

	if (!rc->rx_ring)
		return -EINVAL;

	switch (rc->type) {
	case ICE_RX_CONTAINER:
	{
		struct ice_q_vector *q_vector = rc->rx_ring->q_vector;

		if (ec->rx_coalesce_usecs_high > ICE_MAX_INTRL ||
		    (ec->rx_coalesce_usecs_high &&
		     ec->rx_coalesce_usecs_high < pf->hw.intrl_gran)) {
			netdev_info(vsi->netdev, "Invalid value, %s-usecs-high valid values are 0 (disabled), %d-%d\n",
				    c_type_str, pf->hw.intrl_gran,
				    ICE_MAX_INTRL);
			return -EINVAL;
		}
		if (ec->rx_coalesce_usecs_high != q_vector->intrl &&
		    (ec->use_adaptive_rx_coalesce || ec->use_adaptive_tx_coalesce)) {
			netdev_info(vsi->netdev, "Invalid value, %s-usecs-high cannot be changed if adaptive-tx or adaptive-rx is enabled\n",
				    c_type_str);
			return -EINVAL;
		}
		if (ec->rx_coalesce_usecs_high != q_vector->intrl)
			q_vector->intrl = ec->rx_coalesce_usecs_high;

		use_adaptive_coalesce = ec->use_adaptive_rx_coalesce;
		coalesce_usecs = ec->rx_coalesce_usecs;

		break;
	}
	case ICE_TX_CONTAINER:
		use_adaptive_coalesce = ec->use_adaptive_tx_coalesce;
		coalesce_usecs = ec->tx_coalesce_usecs;

		break;
	default:
		dev_dbg(ice_pf_to_dev(pf), "Invalid container type %d\n",
			rc->type);
		return -EINVAL;
	}

	itr_setting = rc->itr_setting;
	if (coalesce_usecs != itr_setting && use_adaptive_coalesce) {
		netdev_info(vsi->netdev, "%s interrupt throttling cannot be changed if adaptive-%s is enabled\n",
			    c_type_str, c_type_str);
		return -EINVAL;
	}

	if (coalesce_usecs > ICE_ITR_MAX) {
		netdev_info(vsi->netdev, "Invalid value, %s-usecs range is 0-%d\n",
			    c_type_str, ICE_ITR_MAX);
		return -EINVAL;
	}

	if (use_adaptive_coalesce) {
		rc->itr_mode = ITR_DYNAMIC;
	} else {
		rc->itr_mode = ITR_STATIC;
		/* store user facing value how it was set */
		rc->itr_setting = coalesce_usecs;
		/* write the change to the register */
		ice_write_itr(rc, coalesce_usecs);
		/* force writes to take effect immediately, the flush shouldn't
		 * be done in the functions above because the intent is for
		 * them to do lazy writes.
		 */
		ice_flush(&pf->hw);
	}

	return 0;
}

/**
 * ice_set_q_coalesce - set a queue's ITR/INTRL (coalesce) settings
 * @vsi: VSI associated to the queue that need updating
 * @ec: coalesce settings to program the device with
 * @q_num: update ITR/INTRL (coalesce) settings for this queue number/index
 *
 * Return 0 on success, and negative under the following conditions:
 * 1. Setting Tx or Rx ITR/INTRL (coalesce) settings failed.
 * 2. The q_num passed in is not a valid number/index for Tx and Rx rings.
 */
static int
ice_set_q_coalesce(struct ice_vsi *vsi, struct ethtool_coalesce *ec, int q_num)
{
	if (q_num < vsi->num_rxq && q_num < vsi->num_txq) {
		if (ice_set_rc_coalesce(ec,
					&vsi->rx_rings[q_num]->q_vector->rx,
					vsi))
			return -EINVAL;

		if (ice_set_rc_coalesce(ec,
					&vsi->tx_rings[q_num]->q_vector->tx,
					vsi))
			return -EINVAL;
	} else if (q_num < vsi->num_rxq) {
		if (ice_set_rc_coalesce(ec,
					&vsi->rx_rings[q_num]->q_vector->rx,
					vsi))
			return -EINVAL;
	} else if (q_num < vsi->num_txq) {
		if (ice_set_rc_coalesce(ec,
					&vsi->tx_rings[q_num]->q_vector->tx,
					vsi))
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	return 0;
}

/**
 * ice_print_if_odd_usecs - print message if user tries to set odd [tx|rx]-usecs
 * @netdev: netdev used for print
 * @itr_setting: previous user setting
 * @use_adaptive_coalesce: if adaptive coalesce is enabled or being enabled
 * @coalesce_usecs: requested value of [tx|rx]-usecs
 * @c_type_str: either "rx" or "tx" to match user set field of [tx|rx]-usecs
 */
static void
ice_print_if_odd_usecs(struct net_device *netdev, u16 itr_setting,
		       u32 use_adaptive_coalesce, u32 coalesce_usecs,
		       const char *c_type_str)
{
	if (use_adaptive_coalesce)
		return;

	if (itr_setting != coalesce_usecs && (coalesce_usecs % 2))
		netdev_info(netdev, "User set %s-usecs to %d, device only supports even values. Rounding down and attempting to set %s-usecs to %d\n",
			    c_type_str, coalesce_usecs, c_type_str,
			    ITR_REG_ALIGN(coalesce_usecs));
}

/**
 * __ice_set_coalesce - set ITR/INTRL values for the device
 * @netdev: pointer to the netdev associated with this query
 * @ec: ethtool structure to fill with driver's coalesce settings
 * @q_num: queue number to get the coalesce settings for
 *
 * If the caller passes in a negative q_num then we set the coalesce settings
 * for all Tx/Rx queues, else use the actual q_num passed in.
 */
static int
__ice_set_coalesce(struct net_device *netdev, struct ethtool_coalesce *ec,
		   int q_num)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;

	if (q_num < 0) {
		struct ice_q_vector *q_vector = vsi->q_vectors[0];
		int v_idx;

		if (q_vector) {
			ice_print_if_odd_usecs(netdev, q_vector->rx.itr_setting,
					       ec->use_adaptive_rx_coalesce,
					       ec->rx_coalesce_usecs, "rx");

			ice_print_if_odd_usecs(netdev, q_vector->tx.itr_setting,
					       ec->use_adaptive_tx_coalesce,
					       ec->tx_coalesce_usecs, "tx");
		}

		ice_for_each_q_vector(vsi, v_idx) {
			/* In some cases if DCB is configured the num_[rx|tx]q
			 * can be less than vsi->num_q_vectors. This check
			 * accounts for that so we don't report a false failure
			 */
			if (v_idx >= vsi->num_rxq && v_idx >= vsi->num_txq)
				goto set_complete;

			if (ice_set_q_coalesce(vsi, ec, v_idx))
				return -EINVAL;

			ice_set_q_vector_intrl(vsi->q_vectors[v_idx]);
		}
		goto set_complete;
	}

	if (ice_set_q_coalesce(vsi, ec, q_num))
		return -EINVAL;

	ice_set_q_vector_intrl(vsi->q_vectors[q_num]);

set_complete:
	return 0;
}

static int ice_set_coalesce(struct net_device *netdev,
			    struct ethtool_coalesce *ec,
			    struct kernel_ethtool_coalesce *kernel_coal,
			    struct netlink_ext_ack *extack)
{
	return __ice_set_coalesce(netdev, ec, -1);
}

static int
ice_set_per_q_coalesce(struct net_device *netdev, u32 q_num,
		       struct ethtool_coalesce *ec)
{
	return __ice_set_coalesce(netdev, ec, q_num);
}

static void
ice_repr_get_drvinfo(struct net_device *netdev,
		     struct ethtool_drvinfo *drvinfo)
{
	struct ice_repr *repr = ice_netdev_to_repr(netdev);

	if (repr->ops.ready(repr))
		return;

	__ice_get_drvinfo(netdev, drvinfo, repr->src_vsi);
}

static void
ice_repr_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	struct ice_repr *repr = ice_netdev_to_repr(netdev);

	/* for port representors only ETH_SS_STATS is supported */
	if (repr->ops.ready(repr) || stringset != ETH_SS_STATS)
		return;

	__ice_get_strings(netdev, stringset, data, repr->src_vsi);
}

static void
ice_repr_get_ethtool_stats(struct net_device *netdev,
			   struct ethtool_stats __always_unused *stats,
			   u64 *data)
{
	struct ice_repr *repr = ice_netdev_to_repr(netdev);

	if (repr->ops.ready(repr))
		return;

	__ice_get_ethtool_stats(netdev, stats, data, repr->src_vsi);
}

static int ice_repr_get_sset_count(struct net_device *netdev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return ICE_VSI_STATS_LEN;
	default:
		return -EOPNOTSUPP;
	}
}

#define ICE_I2C_EEPROM_DEV_ADDR		0xA0
#define ICE_I2C_EEPROM_DEV_ADDR2	0xA2
#define ICE_MODULE_TYPE_SFP		0x03
#define ICE_MODULE_TYPE_QSFP_PLUS	0x0D
#define ICE_MODULE_TYPE_QSFP28		0x11
#define ICE_MODULE_SFF_ADDR_MODE	0x04
#define ICE_MODULE_SFF_DIAG_CAPAB	0x40
#define ICE_MODULE_REVISION_ADDR	0x01
#define ICE_MODULE_SFF_8472_COMP	0x5E
#define ICE_MODULE_SFF_8472_SWAP	0x5C
#define ICE_MODULE_QSFP_MAX_LEN		640

/**
 * ice_get_module_info - get SFF module type and revision information
 * @netdev: network interface device structure
 * @modinfo: module EEPROM size and layout information structure
 */
static int
ice_get_module_info(struct net_device *netdev,
		    struct ethtool_modinfo *modinfo)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct ice_hw *hw = &pf->hw;
	u8 sff8472_comp = 0;
	u8 sff8472_swap = 0;
	u8 sff8636_rev = 0;
	u8 value = 0;
	int status;

	status = ice_aq_sff_eeprom(hw, 0, ICE_I2C_EEPROM_DEV_ADDR, 0x00, 0x00,
				   0, &value, 1, 0, NULL);
	if (status)
		return status;

	switch (value) {
	case ICE_MODULE_TYPE_SFP:
		status = ice_aq_sff_eeprom(hw, 0, ICE_I2C_EEPROM_DEV_ADDR,
					   ICE_MODULE_SFF_8472_COMP, 0x00, 0,
					   &sff8472_comp, 1, 0, NULL);
		if (status)
			return status;
		status = ice_aq_sff_eeprom(hw, 0, ICE_I2C_EEPROM_DEV_ADDR,
					   ICE_MODULE_SFF_8472_SWAP, 0x00, 0,
					   &sff8472_swap, 1, 0, NULL);
		if (status)
			return status;

		if (sff8472_swap & ICE_MODULE_SFF_ADDR_MODE) {
			modinfo->type = ETH_MODULE_SFF_8079;
			modinfo->eeprom_len = ETH_MODULE_SFF_8079_LEN;
		} else if (sff8472_comp &&
			   (sff8472_swap & ICE_MODULE_SFF_DIAG_CAPAB)) {
			modinfo->type = ETH_MODULE_SFF_8472;
			modinfo->eeprom_len = ETH_MODULE_SFF_8472_LEN;
		} else {
			modinfo->type = ETH_MODULE_SFF_8079;
			modinfo->eeprom_len = ETH_MODULE_SFF_8079_LEN;
		}
		break;
	case ICE_MODULE_TYPE_QSFP_PLUS:
	case ICE_MODULE_TYPE_QSFP28:
		status = ice_aq_sff_eeprom(hw, 0, ICE_I2C_EEPROM_DEV_ADDR,
					   ICE_MODULE_REVISION_ADDR, 0x00, 0,
					   &sff8636_rev, 1, 0, NULL);
		if (status)
			return status;
		/* Check revision compliance */
		if (sff8636_rev > 0x02) {
			/* Module is SFF-8636 compliant */
			modinfo->type = ETH_MODULE_SFF_8636;
			modinfo->eeprom_len = ICE_MODULE_QSFP_MAX_LEN;
		} else {
			modinfo->type = ETH_MODULE_SFF_8436;
			modinfo->eeprom_len = ICE_MODULE_QSFP_MAX_LEN;
		}
		break;
	default:
		netdev_warn(netdev, "SFF Module Type not recognized.\n");
		return -EINVAL;
	}
	return 0;
}

/**
 * ice_get_module_eeprom - fill buffer with SFF EEPROM contents
 * @netdev: network interface device structure
 * @ee: EEPROM dump request structure
 * @data: buffer to be filled with EEPROM contents
 */
static int
ice_get_module_eeprom(struct net_device *netdev,
		      struct ethtool_eeprom *ee, u8 *data)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
#define SFF_READ_BLOCK_SIZE 8
	u8 value[SFF_READ_BLOCK_SIZE] = { 0 };
	u8 addr = ICE_I2C_EEPROM_DEV_ADDR;
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct ice_hw *hw = &pf->hw;
	bool is_sfp = false;
	unsigned int i, j;
	u16 offset = 0;
	u8 page = 0;
	int status;

	if (!ee || !ee->len || !data)
		return -EINVAL;

	status = ice_aq_sff_eeprom(hw, 0, addr, offset, page, 0, value, 1, 0,
				   NULL);
	if (status)
		return status;

	if (value[0] == ICE_MODULE_TYPE_SFP)
		is_sfp = true;

	memset(data, 0, ee->len);
	for (i = 0; i < ee->len; i += SFF_READ_BLOCK_SIZE) {
		offset = i + ee->offset;
		page = 0;

		/* Check if we need to access the other memory page */
		if (is_sfp) {
			if (offset >= ETH_MODULE_SFF_8079_LEN) {
				offset -= ETH_MODULE_SFF_8079_LEN;
				addr = ICE_I2C_EEPROM_DEV_ADDR2;
			}
		} else {
			while (offset >= ETH_MODULE_SFF_8436_LEN) {
				/* Compute memory page number and offset. */
				offset -= ETH_MODULE_SFF_8436_LEN / 2;
				page++;
			}
		}

		/* Bit 2 of EEPROM address 0x02 declares upper
		 * pages are disabled on QSFP modules.
		 * SFP modules only ever use page 0.
		 */
		if (page == 0 || !(data[0x2] & 0x4)) {
			u32 copy_len;

			/* If i2c bus is busy due to slow page change or
			 * link management access, call can fail. This is normal.
			 * So we retry this a few times.
			 */
			for (j = 0; j < 4; j++) {
				status = ice_aq_sff_eeprom(hw, 0, addr, offset, page,
							   !is_sfp, value,
							   SFF_READ_BLOCK_SIZE,
							   0, NULL);
				netdev_dbg(netdev, "SFF %02X %02X %02X %X = %02X%02X%02X%02X.%02X%02X%02X%02X (%X)\n",
					   addr, offset, page, is_sfp,
					   value[0], value[1], value[2], value[3],
					   value[4], value[5], value[6], value[7],
					   status);
				if (status) {
					usleep_range(1500, 2500);
					memset(value, 0, SFF_READ_BLOCK_SIZE);
					continue;
				}
				break;
			}

			/* Make sure we have enough room for the new block */
			copy_len = min_t(u32, SFF_READ_BLOCK_SIZE, ee->len - i);
			memcpy(data + i, value, copy_len);
		}
	}
	return 0;
}

/**
 * ice_get_port_fec_stats - returns FEC correctable, uncorrectable stats per
 *                          pcsquad, pcsport
 * @hw: pointer to the HW struct
 * @pcs_quad: pcsquad for input port
 * @pcs_port: pcsport for input port
 * @fec_stats: buffer to hold FEC statistics for given port
 *
 * Return: 0 on success, negative on failure.
 */
static int ice_get_port_fec_stats(struct ice_hw *hw, u16 pcs_quad, u16 pcs_port,
				  struct ethtool_fec_stats *fec_stats)
{
	u32 fec_uncorr_low_val = 0, fec_uncorr_high_val = 0;
	u32 fec_corr_low_val = 0, fec_corr_high_val = 0;
	int err;

	if (pcs_quad > 1 || pcs_port > 3)
		return -EINVAL;

	err = ice_aq_get_fec_stats(hw, pcs_quad, pcs_port, ICE_FEC_CORR_LOW,
				   &fec_corr_low_val);
	if (err)
		return err;

	err = ice_aq_get_fec_stats(hw, pcs_quad, pcs_port, ICE_FEC_CORR_HIGH,
				   &fec_corr_high_val);
	if (err)
		return err;

	err = ice_aq_get_fec_stats(hw, pcs_quad, pcs_port,
				   ICE_FEC_UNCORR_LOW,
				   &fec_uncorr_low_val);
	if (err)
		return err;

	err = ice_aq_get_fec_stats(hw, pcs_quad, pcs_port,
				   ICE_FEC_UNCORR_HIGH,
				   &fec_uncorr_high_val);
	if (err)
		return err;

	fec_stats->corrected_blocks.total = (fec_corr_high_val << 16) +
					     fec_corr_low_val;
	fec_stats->uncorrectable_blocks.total = (fec_uncorr_high_val << 16) +
						 fec_uncorr_low_val;
	return 0;
}

/**
 * ice_get_fec_stats - returns FEC correctable, uncorrectable stats per netdev
 * @netdev: network interface device structure
 * @fec_stats: buffer to hold FEC statistics for given port
 *
 */
static void ice_get_fec_stats(struct net_device *netdev,
			      struct ethtool_fec_stats *fec_stats)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_port_topology port_topology;
	struct ice_port_info *pi;
	struct ice_pf *pf;
	struct ice_hw *hw;
	int err;

	pf = np->vsi->back;
	hw = &pf->hw;
	pi = np->vsi->port_info;

	/* Serdes parameters are not supported if not the PF VSI */
	if (np->vsi->type != ICE_VSI_PF || !pi)
		return;

	err = ice_get_port_topology(hw, pi->lport, &port_topology);
	if (err) {
		netdev_info(netdev, "Extended register dump failed Lport %d\n",
			    pi->lport);
		return;
	}

	/* Get FEC correctable, uncorrectable counter */
	err = ice_get_port_fec_stats(hw, port_topology.pcs_quad_select,
				     port_topology.pcs_port, fec_stats);
	if (err)
		netdev_info(netdev, "FEC stats get failed Lport %d Err %d\n",
			    pi->lport, err);
}

#define ICE_ETHTOOL_PFR (ETH_RESET_IRQ | ETH_RESET_DMA | \
	ETH_RESET_FILTER | ETH_RESET_OFFLOAD)

#define ICE_ETHTOOL_CORER ((ICE_ETHTOOL_PFR | ETH_RESET_RAM) << \
	ETH_RESET_SHARED_SHIFT)

#define ICE_ETHTOOL_GLOBR (ICE_ETHTOOL_CORER | \
	(ETH_RESET_MAC << ETH_RESET_SHARED_SHIFT) | \
	(ETH_RESET_PHY << ETH_RESET_SHARED_SHIFT))

#define ICE_ETHTOOL_VFR ICE_ETHTOOL_PFR

/**
 * ice_ethtool_reset - triggers a given type of reset
 * @dev: network interface device structure
 * @flags: set of reset flags
 *
 * Return: 0 on success, -EOPNOTSUPP when using unsupported set of flags.
 */
static int ice_ethtool_reset(struct net_device *dev, u32 *flags)
{
	struct ice_netdev_priv *np = netdev_priv(dev);
	struct ice_pf *pf = np->vsi->back;
	enum ice_reset_req reset;

	switch (*flags) {
	case ICE_ETHTOOL_CORER:
		reset = ICE_RESET_CORER;
		break;
	case ICE_ETHTOOL_GLOBR:
		reset = ICE_RESET_GLOBR;
		break;
	case ICE_ETHTOOL_PFR:
		reset = ICE_RESET_PFR;
		break;
	default:
		netdev_info(dev, "Unsupported set of ethtool flags");
		return -EOPNOTSUPP;
	}

	ice_schedule_reset(pf, reset);

	*flags = 0;

	return 0;
}

/**
 * ice_repr_ethtool_reset - triggers a VF reset
 * @dev: network interface device structure
 * @flags: set of reset flags
 *
 * Return: 0 on success,
 * -EOPNOTSUPP when using unsupported set of flags
 * -EBUSY when VF is not ready for reset.
 */
static int ice_repr_ethtool_reset(struct net_device *dev, u32 *flags)
{
	struct ice_repr *repr = ice_netdev_to_repr(dev);
	struct ice_vf *vf;

	if (repr->type != ICE_REPR_TYPE_VF ||
	    *flags != ICE_ETHTOOL_VFR)
		return -EOPNOTSUPP;

	vf = repr->vf;

	if (ice_check_vf_ready_for_cfg(vf))
		return -EBUSY;

	*flags = 0;

	return ice_reset_vf(vf, ICE_VF_RESET_VFLR | ICE_VF_RESET_LOCK);
}

static const struct ethtool_ops ice_ethtool_ops = {
	.supported_coalesce_params = ETHTOOL_COALESCE_USECS |
				     ETHTOOL_COALESCE_USE_ADAPTIVE |
				     ETHTOOL_COALESCE_RX_USECS_HIGH,
	.supported_input_xfrm	= RXH_XFRM_SYM_XOR,
	.get_link_ksettings	= ice_get_link_ksettings,
	.set_link_ksettings	= ice_set_link_ksettings,
	.get_fec_stats		= ice_get_fec_stats,
	.get_drvinfo		= ice_get_drvinfo,
	.get_regs_len		= ice_get_regs_len,
	.get_regs		= ice_get_regs,
	.get_wol		= ice_get_wol,
	.set_wol		= ice_set_wol,
	.get_msglevel		= ice_get_msglevel,
	.set_msglevel		= ice_set_msglevel,
	.self_test		= ice_self_test,
	.get_link		= ethtool_op_get_link,
	.get_link_ext_stats	= ice_get_link_ext_stats,
	.get_eeprom_len		= ice_get_eeprom_len,
	.get_eeprom		= ice_get_eeprom,
	.get_coalesce		= ice_get_coalesce,
	.set_coalesce		= ice_set_coalesce,
	.get_strings		= ice_get_strings,
	.set_phys_id		= ice_set_phys_id,
	.get_ethtool_stats      = ice_get_ethtool_stats,
	.get_priv_flags		= ice_get_priv_flags,
	.set_priv_flags		= ice_set_priv_flags,
	.get_sset_count		= ice_get_sset_count,
	.get_rxnfc		= ice_get_rxnfc,
	.set_rxnfc		= ice_set_rxnfc,
	.get_ringparam		= ice_get_ringparam,
	.set_ringparam		= ice_set_ringparam,
	.nway_reset		= ice_nway_reset,
	.get_pauseparam		= ice_get_pauseparam,
	.set_pauseparam		= ice_set_pauseparam,
	.reset			= ice_ethtool_reset,
	.get_rxfh_key_size	= ice_get_rxfh_key_size,
	.get_rxfh_indir_size	= ice_get_rxfh_indir_size,
	.get_rxfh		= ice_get_rxfh,
	.set_rxfh		= ice_set_rxfh,
	.get_rxfh_fields	= ice_get_rxfh_fields,
	.set_rxfh_fields	= ice_set_rxfh_fields,
	.get_channels		= ice_get_channels,
	.set_channels		= ice_set_channels,
	.get_ts_info		= ice_get_ts_info,
	.get_per_queue_coalesce	= ice_get_per_q_coalesce,
	.set_per_queue_coalesce	= ice_set_per_q_coalesce,
	.get_fecparam		= ice_get_fecparam,
	.set_fecparam		= ice_set_fecparam,
	.get_module_info	= ice_get_module_info,
	.get_module_eeprom	= ice_get_module_eeprom,
};

static const struct ethtool_ops ice_ethtool_safe_mode_ops = {
	.get_link_ksettings	= ice_get_link_ksettings,
	.set_link_ksettings	= ice_set_link_ksettings,
	.get_drvinfo		= ice_get_drvinfo,
	.get_regs_len		= ice_get_regs_len,
	.get_regs		= ice_get_regs,
	.get_wol		= ice_get_wol,
	.set_wol		= ice_set_wol,
	.get_msglevel		= ice_get_msglevel,
	.set_msglevel		= ice_set_msglevel,
	.get_link		= ethtool_op_get_link,
	.get_eeprom_len		= ice_get_eeprom_len,
	.get_eeprom		= ice_get_eeprom,
	.get_strings		= ice_get_strings,
	.get_ethtool_stats	= ice_get_ethtool_stats,
	.get_sset_count		= ice_get_sset_count,
	.get_ringparam		= ice_get_ringparam,
	.set_ringparam		= ice_set_ringparam,
	.nway_reset		= ice_nway_reset,
	.get_channels		= ice_get_channels,
};

/**
 * ice_set_ethtool_safe_mode_ops - setup safe mode ethtool ops
 * @netdev: network interface device structure
 */
void ice_set_ethtool_safe_mode_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &ice_ethtool_safe_mode_ops;
}

static const struct ethtool_ops ice_ethtool_repr_ops = {
	.get_drvinfo		= ice_repr_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_strings		= ice_repr_get_strings,
	.get_ethtool_stats      = ice_repr_get_ethtool_stats,
	.get_sset_count		= ice_repr_get_sset_count,
	.reset			= ice_repr_ethtool_reset,
};

/**
 * ice_set_ethtool_repr_ops - setup VF's port representor ethtool ops
 * @netdev: network interface device structure
 */
void ice_set_ethtool_repr_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &ice_ethtool_repr_ops;
}

/**
 * ice_set_ethtool_ops - setup netdev ethtool ops
 * @netdev: network interface device structure
 *
 * setup netdev ethtool ops with ice specific ops
 */
void ice_set_ethtool_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &ice_ethtool_ops;
}
