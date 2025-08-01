/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2017 Imagination Technologies
 * Author: Paul Burton <paul.burton@mips.com>
 */

#ifndef __MIPS_ASM_MIPS_CPS_H__
#define __MIPS_ASM_MIPS_CPS_H__

#include <linux/bitfield.h>
#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/types.h>

extern unsigned long __cps_access_bad_size(void)
	__compiletime_error("Bad size for CPS accessor");

#define CPS_ACCESSOR_A(unit, off, name)					\
static inline void *addr_##unit##_##name(void)				\
{									\
	return mips_##unit##_base + (off);				\
}

#define CPS_ACCESSOR_R(unit, sz, name)					\
static inline uint##sz##_t read_##unit##_##name(void)			\
{									\
	uint64_t val64;							\
									\
	switch (sz) {							\
	case 32:							\
		return __raw_readl(addr_##unit##_##name());		\
									\
	case 64:							\
		if (mips_cm_is64)					\
			return __raw_readq(addr_##unit##_##name());	\
									\
		val64 = __raw_readl(addr_##unit##_##name() + 4);	\
		val64 <<= 32;						\
		val64 |= __raw_readl(addr_##unit##_##name());		\
		return val64;						\
									\
	default:							\
		return __cps_access_bad_size();				\
	}								\
}

#define CPS_ACCESSOR_W(unit, sz, name)					\
static inline void write_##unit##_##name(uint##sz##_t val)		\
{									\
	switch (sz) {							\
	case 32:							\
		__raw_writel(val, addr_##unit##_##name());		\
		break;							\
									\
	case 64:							\
		if (mips_cm_is64) {					\
			__raw_writeq(val, addr_##unit##_##name());	\
			break;						\
		}							\
									\
		__raw_writel((uint64_t)val >> 32,			\
			     addr_##unit##_##name() + 4);		\
		__raw_writel(val, addr_##unit##_##name());		\
		break;							\
									\
	default:							\
		__cps_access_bad_size();				\
		break;							\
	}								\
}

#define CPS_ACCESSOR_M(unit, sz, name)					\
static inline void change_##unit##_##name(uint##sz##_t mask,		\
					  uint##sz##_t val)		\
{									\
	uint##sz##_t reg_val = read_##unit##_##name();			\
	reg_val &= ~mask;						\
	reg_val |= val;							\
	write_##unit##_##name(reg_val);					\
}									\
									\
static inline void set_##unit##_##name(uint##sz##_t val)		\
{									\
	change_##unit##_##name(val, val);				\
}									\
									\
static inline void clear_##unit##_##name(uint##sz##_t val)		\
{									\
	change_##unit##_##name(val, 0);					\
}

#define CPS_ACCESSOR_RO(unit, sz, off, name)				\
	CPS_ACCESSOR_A(unit, off, name)					\
	CPS_ACCESSOR_R(unit, sz, name)

#define CPS_ACCESSOR_WO(unit, sz, off, name)				\
	CPS_ACCESSOR_A(unit, off, name)					\
	CPS_ACCESSOR_W(unit, sz, name)

#define CPS_ACCESSOR_RW(unit, sz, off, name)				\
	CPS_ACCESSOR_A(unit, off, name)					\
	CPS_ACCESSOR_R(unit, sz, name)					\
	CPS_ACCESSOR_W(unit, sz, name)					\
	CPS_ACCESSOR_M(unit, sz, name)

#include <asm/mips-cm.h>
#include <asm/mips-cpc.h>
#include <asm/mips-gic.h>

/**
 * mips_cps_numclusters - return the number of clusters present in the system
 *
 * Returns the number of clusters in the system.
 */
static inline unsigned int mips_cps_numclusters(void)
{
	if (mips_cm_revision() < CM_REV_CM3_5)
		return 1;

	return FIELD_GET(CM_GCR_CONFIG_NUM_CLUSTERS, read_gcr_config());
}

/**
 * mips_cps_cluster_config - return (GCR|CPC)_CONFIG from a cluster
 * @cluster: the ID of the cluster whose config we want
 *
 * Read the value of GCR_CONFIG (or its CPC_CONFIG mirror) from a @cluster.
 *
 * Returns the value of GCR_CONFIG.
 */
static inline uint64_t mips_cps_cluster_config(unsigned int cluster)
{
	uint64_t config;

	if (mips_cm_revision() < CM_REV_CM3_5) {
		/*
		 * Prior to CM 3.5 we don't have the notion of multiple
		 * clusters so we can trivially read the GCR_CONFIG register
		 * within this cluster.
		 */
		WARN_ON(cluster != 0);
		config = read_gcr_config();
	} else {
		/*
		 * From CM 3.5 onwards we read the CPC_CONFIG mirror of
		 * GCR_CONFIG via the redirect region, since the CPC is always
		 * powered up allowing us not to need to power up the CM.
		 */
		mips_cm_lock_other(cluster, 0, 0, CM_GCR_Cx_OTHER_BLOCK_GLOBAL);
		config = read_cpc_redir_config();
		mips_cm_unlock_other();
	}

	return config;
}

/**
 * mips_cps_numcores - return the number of cores present in a cluster
 * @cluster: the ID of the cluster whose core count we want
 *
 * Returns the value of the PCORES field of the GCR_CONFIG register plus 1, or
 * zero if no Coherence Manager is present.
 */
static inline unsigned int mips_cps_numcores(unsigned int cluster)
{
	if (!mips_cm_present())
		return 0;

	/* Add one before masking to handle 0xff indicating no cores */
	return FIELD_GET(CM_GCR_CONFIG_PCORES,
			 mips_cps_cluster_config(cluster) + 1);
}

/**
 * mips_cps_numiocu - return the number of IOCUs present in a cluster
 * @cluster: the ID of the cluster whose IOCU count we want
 *
 * Returns the value of the NUMIOCU field of the GCR_CONFIG register, or zero
 * if no Coherence Manager is present.
 */
static inline unsigned int mips_cps_numiocu(unsigned int cluster)
{
	if (!mips_cm_present())
		return 0;

	return FIELD_GET(CM_GCR_CONFIG_NUMIOCU,
			 mips_cps_cluster_config(cluster));
}

/**
 * mips_cps_numvps - return the number of VPs (threads) supported by a core
 * @cluster: the ID of the cluster containing the core we want to examine
 * @core: the ID of the core whose VP count we want
 *
 * Returns the number of Virtual Processors (VPs, ie. hardware threads) that
 * are supported by the given @core in the given @cluster. If the core or the
 * kernel do not support hardware mutlti-threading this returns 1.
 */
static inline unsigned int mips_cps_numvps(unsigned int cluster, unsigned int core)
{
	unsigned int cfg;

	if (!mips_cm_present())
		return 1;

	if ((!IS_ENABLED(CONFIG_MIPS_MT_SMP) || !cpu_has_mipsmt)
		&& (!IS_ENABLED(CONFIG_CPU_MIPSR6) || !cpu_has_vp))
		return 1;

	mips_cm_lock_other(cluster, core, 0, CM_GCR_Cx_OTHER_BLOCK_LOCAL);

	if (mips_cm_revision() < CM_REV_CM3_5) {
		/*
		 * Prior to CM 3.5 we can only have one cluster & don't have
		 * CPC_Cx_CONFIG, so we read GCR_Cx_CONFIG.
		 */
		cfg = read_gcr_co_config();
	} else {
		/*
		 * From CM 3.5 onwards we read CPC_Cx_CONFIG because the CPC is
		 * always powered, which allows us to not worry about powering
		 * up the cluster's CM here.
		 */
		cfg = read_cpc_co_config();
	}

	mips_cm_unlock_other();

	return FIELD_GET(CM_GCR_Cx_CONFIG_PVPE, cfg + 1);
}

/**
 * mips_cps_multicluster_cpus() - Detect whether CPUs are in multiple clusters
 *
 * Determine whether the system includes CPUs in multiple clusters - ie.
 * whether we can treat the system as single or multi-cluster as far as CPUs
 * are concerned. Note that this is slightly different to simply checking
 * whether multiple clusters are present - it is possible for there to be
 * clusters which contain no CPUs, which this function will effectively ignore.
 *
 * Returns true if CPUs are spread across multiple clusters, else false.
 */
static inline bool mips_cps_multicluster_cpus(void)
{
	unsigned int first_cl, last_cl;

	/*
	 * CPUs are numbered sequentially by cluster - ie. CPUs 0..X will be in
	 * cluster 0, CPUs X+1..Y in cluster 1, CPUs Y+1..Z in cluster 2 etc.
	 *
	 * Thus we can detect multiple clusters trivially by checking whether
	 * the first & last CPUs belong to the same cluster.
	 */
	first_cl = cpu_cluster(&boot_cpu_data);
	last_cl = cpu_cluster(&cpu_data[nr_cpu_ids - 1]);
	return first_cl != last_cl;
}

/**
 * mips_cps_first_online_in_cluster() - Detect if CPU is first online in cluster
 * @first_cpu: The first other online CPU in cluster, or nr_cpu_ids if
 * the function returns true.
 *
 * Determine whether the local CPU is the first to be brought online in its
 * cluster - that is, whether there are any other online CPUs in the local
 * cluster.
 *
 * Returns true if this CPU is first online, else false.
 */
extern unsigned int mips_cps_first_online_in_cluster(int *first_cpu);

#endif /* __MIPS_ASM_MIPS_CPS_H__ */
