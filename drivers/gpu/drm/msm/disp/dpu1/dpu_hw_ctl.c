// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/delay.h>

#include <drm/drm_managed.h>

#include "dpu_hwio.h"
#include "dpu_hw_ctl.h"
#include "dpu_kms.h"
#include "dpu_trace.h"

#define   CTL_LAYER(lm)                 \
	(((lm) == LM_5) ? (0x024) : (((lm) - LM_0) * 0x004))
#define   CTL_LAYER_EXT(lm)             \
	(0x40 + (((lm) - LM_0) * 0x004))
#define   CTL_LAYER_EXT2(lm)             \
	(0x70 + (((lm) - LM_0) * 0x004))
#define   CTL_LAYER_EXT3(lm)             \
	(0xA0 + (((lm) - LM_0) * 0x004))
#define CTL_LAYER_EXT4(lm)             \
	(0xB8 + (((lm) - LM_0) * 0x004))
#define   CTL_TOP                       0x014
#define   CTL_FLUSH                     0x018
#define   CTL_START                     0x01C
#define   CTL_PREPARE                   0x0d0
#define   CTL_SW_RESET                  0x030
#define   CTL_LAYER_EXTN_OFFSET         0x40
#define   CTL_MERGE_3D_ACTIVE           0x0E4
#define   CTL_DSC_ACTIVE                0x0E8
#define   CTL_WB_ACTIVE                 0x0EC
#define   CTL_CWB_ACTIVE                0x0F0
#define   CTL_INTF_ACTIVE               0x0F4
#define   CTL_CDM_ACTIVE                0x0F8
#define   CTL_FETCH_PIPE_ACTIVE         0x0FC
#define   CTL_MERGE_3D_FLUSH            0x100
#define   CTL_DSC_FLUSH                0x104
#define   CTL_WB_FLUSH                  0x108
#define   CTL_CWB_FLUSH                 0x10C
#define   CTL_INTF_FLUSH                0x110
#define   CTL_CDM_FLUSH                0x114
#define   CTL_PERIPH_FLUSH              0x128
#define   CTL_PIPE_ACTIVE               0x12c
#define   CTL_LAYER_ACTIVE              0x130
#define   CTL_INTF_MASTER               0x134
#define   CTL_DSPP_n_FLUSH(n)           ((0x13C) + ((n) * 4))

#define CTL_MIXER_BORDER_OUT            BIT(24)
#define CTL_FLUSH_MASK_CTL              BIT(17)

#define DPU_REG_RESET_TIMEOUT_US        2000
#define  MERGE_3D_IDX   23
#define  DSC_IDX        22
#define CDM_IDX         26
#define  PERIPH_IDX     30
#define  INTF_IDX       31
#define WB_IDX          16
#define CWB_IDX         28
#define  DSPP_IDX       29  /* From DPU hw rev 7.x.x */
#define CTL_INVALID_BIT                 0xffff
#define CTL_DEFAULT_GROUP_ID		0xf

static const u32 fetch_tbl[SSPP_MAX] = {CTL_INVALID_BIT, 16, 17, 18, 19,
	CTL_INVALID_BIT, CTL_INVALID_BIT, CTL_INVALID_BIT, CTL_INVALID_BIT, 0,
	1, 2, 3, 4, 5};

static const u32 lm_tbl[LM_MAX] = {CTL_INVALID_BIT, 0, 1, 2, 3, 4, 5, 6, 7};

static int _mixer_stages(const struct dpu_lm_cfg *mixer, int count,
		enum dpu_lm lm)
{
	int i;
	int stages = -EINVAL;

	for (i = 0; i < count; i++) {
		if (lm == mixer[i].id) {
			stages = mixer[i].sblk->maxblendstages;
			break;
		}
	}

	return stages;
}

static inline u32 dpu_hw_ctl_get_flush_register(struct dpu_hw_ctl *ctx)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;

	return DPU_REG_READ(c, CTL_FLUSH);
}

static inline void dpu_hw_ctl_trigger_start(struct dpu_hw_ctl *ctx)
{
	trace_dpu_hw_ctl_trigger_start(ctx->pending_flush_mask,
				       dpu_hw_ctl_get_flush_register(ctx));
	DPU_REG_WRITE(&ctx->hw, CTL_START, 0x1);
}

static inline bool dpu_hw_ctl_is_started(struct dpu_hw_ctl *ctx)
{
	return !!(DPU_REG_READ(&ctx->hw, CTL_START) & BIT(0));
}

static inline void dpu_hw_ctl_trigger_pending(struct dpu_hw_ctl *ctx)
{
	trace_dpu_hw_ctl_trigger_prepare(ctx->pending_flush_mask,
					 dpu_hw_ctl_get_flush_register(ctx));
	DPU_REG_WRITE(&ctx->hw, CTL_PREPARE, 0x1);
}

static inline void dpu_hw_ctl_clear_pending_flush(struct dpu_hw_ctl *ctx)
{
	trace_dpu_hw_ctl_clear_pending_flush(ctx->pending_flush_mask,
				     dpu_hw_ctl_get_flush_register(ctx));
	ctx->pending_flush_mask = 0x0;
	ctx->pending_intf_flush_mask = 0;
	ctx->pending_wb_flush_mask = 0;
	ctx->pending_cwb_flush_mask = 0;
	ctx->pending_merge_3d_flush_mask = 0;
	ctx->pending_dsc_flush_mask = 0;
	ctx->pending_cdm_flush_mask = 0;

	memset(ctx->pending_dspp_flush_mask, 0,
		sizeof(ctx->pending_dspp_flush_mask));
}

static inline void dpu_hw_ctl_update_pending_flush(struct dpu_hw_ctl *ctx,
		u32 flushbits)
{
	trace_dpu_hw_ctl_update_pending_flush(flushbits,
					      ctx->pending_flush_mask);
	ctx->pending_flush_mask |= flushbits;
}

static u32 dpu_hw_ctl_get_pending_flush(struct dpu_hw_ctl *ctx)
{
	return ctx->pending_flush_mask;
}

static inline void dpu_hw_ctl_trigger_flush_v1(struct dpu_hw_ctl *ctx)
{
	int dspp;

	if (ctx->pending_flush_mask & BIT(MERGE_3D_IDX))
		DPU_REG_WRITE(&ctx->hw, CTL_MERGE_3D_FLUSH,
				ctx->pending_merge_3d_flush_mask);
	if (ctx->pending_flush_mask & BIT(INTF_IDX))
		DPU_REG_WRITE(&ctx->hw, CTL_INTF_FLUSH,
				ctx->pending_intf_flush_mask);
	if (ctx->pending_flush_mask & BIT(WB_IDX))
		DPU_REG_WRITE(&ctx->hw, CTL_WB_FLUSH,
				ctx->pending_wb_flush_mask);
	if (ctx->pending_flush_mask & BIT(CWB_IDX))
		DPU_REG_WRITE(&ctx->hw, CTL_CWB_FLUSH,
				ctx->pending_cwb_flush_mask);

	if (ctx->pending_flush_mask & BIT(DSPP_IDX))
		for (dspp = DSPP_0; dspp < DSPP_MAX; dspp++) {
			if (ctx->pending_dspp_flush_mask[dspp - DSPP_0])
				DPU_REG_WRITE(&ctx->hw,
				CTL_DSPP_n_FLUSH(dspp - DSPP_0),
				ctx->pending_dspp_flush_mask[dspp - DSPP_0]);
		}

	if (ctx->pending_flush_mask & BIT(PERIPH_IDX))
		DPU_REG_WRITE(&ctx->hw, CTL_PERIPH_FLUSH,
			      ctx->pending_periph_flush_mask);

	if (ctx->pending_flush_mask & BIT(DSC_IDX))
		DPU_REG_WRITE(&ctx->hw, CTL_DSC_FLUSH,
			      ctx->pending_dsc_flush_mask);

	if (ctx->pending_flush_mask & BIT(CDM_IDX))
		DPU_REG_WRITE(&ctx->hw, CTL_CDM_FLUSH,
			      ctx->pending_cdm_flush_mask);

	DPU_REG_WRITE(&ctx->hw, CTL_FLUSH, ctx->pending_flush_mask);
}

static inline void dpu_hw_ctl_trigger_flush(struct dpu_hw_ctl *ctx)
{
	trace_dpu_hw_ctl_trigger_pending_flush(ctx->pending_flush_mask,
				     dpu_hw_ctl_get_flush_register(ctx));
	DPU_REG_WRITE(&ctx->hw, CTL_FLUSH, ctx->pending_flush_mask);
}

static void dpu_hw_ctl_update_pending_flush_sspp(struct dpu_hw_ctl *ctx,
	enum dpu_sspp sspp)
{
	switch (sspp) {
	case SSPP_VIG0:
		ctx->pending_flush_mask |=  BIT(0);
		break;
	case SSPP_VIG1:
		ctx->pending_flush_mask |= BIT(1);
		break;
	case SSPP_VIG2:
		ctx->pending_flush_mask |= BIT(2);
		break;
	case SSPP_VIG3:
		ctx->pending_flush_mask |= BIT(18);
		break;
	case SSPP_RGB0:
		ctx->pending_flush_mask |= BIT(3);
		break;
	case SSPP_RGB1:
		ctx->pending_flush_mask |= BIT(4);
		break;
	case SSPP_RGB2:
		ctx->pending_flush_mask |= BIT(5);
		break;
	case SSPP_RGB3:
		ctx->pending_flush_mask |= BIT(19);
		break;
	case SSPP_DMA0:
		ctx->pending_flush_mask |= BIT(11);
		break;
	case SSPP_DMA1:
		ctx->pending_flush_mask |= BIT(12);
		break;
	case SSPP_DMA2:
		ctx->pending_flush_mask |= BIT(24);
		break;
	case SSPP_DMA3:
		ctx->pending_flush_mask |= BIT(25);
		break;
	case SSPP_DMA4:
		ctx->pending_flush_mask |= BIT(13);
		break;
	case SSPP_DMA5:
		ctx->pending_flush_mask |= BIT(14);
		break;
	case SSPP_CURSOR0:
		ctx->pending_flush_mask |= BIT(22);
		break;
	case SSPP_CURSOR1:
		ctx->pending_flush_mask |= BIT(23);
		break;
	default:
		break;
	}
}

static void dpu_hw_ctl_update_pending_flush_mixer(struct dpu_hw_ctl *ctx,
	enum dpu_lm lm)
{
	switch (lm) {
	case LM_0:
		ctx->pending_flush_mask |= BIT(6);
		break;
	case LM_1:
		ctx->pending_flush_mask |= BIT(7);
		break;
	case LM_2:
		ctx->pending_flush_mask |= BIT(8);
		break;
	case LM_3:
		ctx->pending_flush_mask |= BIT(9);
		break;
	case LM_4:
		ctx->pending_flush_mask |= BIT(10);
		break;
	case LM_5:
		ctx->pending_flush_mask |= BIT(20);
		break;
	case LM_6:
		ctx->pending_flush_mask |= BIT(21);
		break;
	case LM_7:
		ctx->pending_flush_mask |= BIT(27);
		break;
	default:
		break;
	}

	ctx->pending_flush_mask |= CTL_FLUSH_MASK_CTL;
}

static void dpu_hw_ctl_update_pending_flush_intf(struct dpu_hw_ctl *ctx,
		enum dpu_intf intf)
{
	switch (intf) {
	case INTF_0:
		ctx->pending_flush_mask |= BIT(31);
		break;
	case INTF_1:
		ctx->pending_flush_mask |= BIT(30);
		break;
	case INTF_2:
		ctx->pending_flush_mask |= BIT(29);
		break;
	case INTF_3:
		ctx->pending_flush_mask |= BIT(28);
		break;
	default:
		break;
	}
}

static void dpu_hw_ctl_update_pending_flush_wb(struct dpu_hw_ctl *ctx,
		enum dpu_wb wb)
{
	switch (wb) {
	case WB_0:
	case WB_1:
	case WB_2:
		ctx->pending_flush_mask |= BIT(WB_IDX);
		break;
	default:
		break;
	}
}

static void dpu_hw_ctl_update_pending_flush_cdm(struct dpu_hw_ctl *ctx, enum dpu_cdm cdm_num)
{
	/* update pending flush only if CDM_0 is flushed */
	if (cdm_num == CDM_0)
		ctx->pending_flush_mask |= BIT(CDM_IDX);
}

static void dpu_hw_ctl_update_pending_flush_wb_v1(struct dpu_hw_ctl *ctx,
		enum dpu_wb wb)
{
	ctx->pending_wb_flush_mask |= BIT(wb - WB_0);
	ctx->pending_flush_mask |= BIT(WB_IDX);
}

static void dpu_hw_ctl_update_pending_flush_cwb_v1(struct dpu_hw_ctl *ctx,
		enum dpu_cwb cwb)
{
	ctx->pending_cwb_flush_mask |= BIT(cwb - CWB_0);
	ctx->pending_flush_mask |= BIT(CWB_IDX);
}

static void dpu_hw_ctl_update_pending_flush_intf_v1(struct dpu_hw_ctl *ctx,
		enum dpu_intf intf)
{
	ctx->pending_intf_flush_mask |= BIT(intf - INTF_0);
	ctx->pending_flush_mask |= BIT(INTF_IDX);
}

static void dpu_hw_ctl_update_pending_flush_periph_v1(struct dpu_hw_ctl *ctx,
						      enum dpu_intf intf)
{
	ctx->pending_periph_flush_mask |= BIT(intf - INTF_0);
	ctx->pending_flush_mask |= BIT(PERIPH_IDX);
}

static void dpu_hw_ctl_update_pending_flush_merge_3d_v1(struct dpu_hw_ctl *ctx,
		enum dpu_merge_3d merge_3d)
{
	ctx->pending_merge_3d_flush_mask |= BIT(merge_3d - MERGE_3D_0);
	ctx->pending_flush_mask |= BIT(MERGE_3D_IDX);
}

static void dpu_hw_ctl_update_pending_flush_dsc_v1(struct dpu_hw_ctl *ctx,
						   enum dpu_dsc dsc_num)
{
	ctx->pending_dsc_flush_mask |= BIT(dsc_num - DSC_0);
	ctx->pending_flush_mask |= BIT(DSC_IDX);
}

static void dpu_hw_ctl_update_pending_flush_cdm_v1(struct dpu_hw_ctl *ctx, enum dpu_cdm cdm_num)
{
	ctx->pending_cdm_flush_mask |= BIT(cdm_num - CDM_0);
	ctx->pending_flush_mask |= BIT(CDM_IDX);
}

static void dpu_hw_ctl_update_pending_flush_dspp(struct dpu_hw_ctl *ctx,
	enum dpu_dspp dspp, u32 dspp_sub_blk)
{
	switch (dspp) {
	case DSPP_0:
		ctx->pending_flush_mask |= BIT(13);
		break;
	case DSPP_1:
		ctx->pending_flush_mask |= BIT(14);
		break;
	case DSPP_2:
		ctx->pending_flush_mask |= BIT(15);
		break;
	case DSPP_3:
		ctx->pending_flush_mask |= BIT(21);
		break;
	default:
		break;
	}
}

static void dpu_hw_ctl_update_pending_flush_dspp_sub_blocks(
	struct dpu_hw_ctl *ctx,	enum dpu_dspp dspp, u32 dspp_sub_blk)
{
	if (dspp >= DSPP_MAX)
		return;

	switch (dspp_sub_blk) {
	case DPU_DSPP_PCC:
		ctx->pending_dspp_flush_mask[dspp - DSPP_0] |= BIT(4);
		break;
	default:
		return;
	}

	ctx->pending_flush_mask |= BIT(DSPP_IDX);
}

static u32 dpu_hw_ctl_poll_reset_status(struct dpu_hw_ctl *ctx, u32 timeout_us)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	ktime_t timeout;
	u32 status;

	timeout = ktime_add_us(ktime_get(), timeout_us);

	/*
	 * it takes around 30us to have mdp finish resetting its ctl path
	 * poll every 50us so that reset should be completed at 1st poll
	 */
	do {
		status = DPU_REG_READ(c, CTL_SW_RESET);
		status &= 0x1;
		if (status)
			usleep_range(20, 50);
	} while (status && ktime_compare_safe(ktime_get(), timeout) < 0);

	return status;
}

static int dpu_hw_ctl_reset_control(struct dpu_hw_ctl *ctx)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;

	pr_debug("issuing hw ctl reset for ctl:%d\n", ctx->idx);
	DPU_REG_WRITE(c, CTL_SW_RESET, 0x1);
	if (dpu_hw_ctl_poll_reset_status(ctx, DPU_REG_RESET_TIMEOUT_US))
		return -EINVAL;

	return 0;
}

static int dpu_hw_ctl_wait_reset_status(struct dpu_hw_ctl *ctx)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	u32 status;

	status = DPU_REG_READ(c, CTL_SW_RESET);
	status &= 0x01;
	if (!status)
		return 0;

	pr_debug("hw ctl reset is set for ctl:%d\n", ctx->idx);
	if (dpu_hw_ctl_poll_reset_status(ctx, DPU_REG_RESET_TIMEOUT_US)) {
		pr_err("hw recovery is not complete for ctl:%d\n", ctx->idx);
		return -EINVAL;
	}

	return 0;
}

static void dpu_hw_ctl_clear_all_blendstages(struct dpu_hw_ctl *ctx)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	int i;

	for (i = 0; i < ctx->mixer_count; i++) {
		enum dpu_lm mixer_id = ctx->mixer_hw_caps[i].id;

		DPU_REG_WRITE(c, CTL_LAYER(mixer_id), 0);
		DPU_REG_WRITE(c, CTL_LAYER_EXT(mixer_id), 0);
		DPU_REG_WRITE(c, CTL_LAYER_EXT2(mixer_id), 0);
		DPU_REG_WRITE(c, CTL_LAYER_EXT3(mixer_id), 0);
	}

	DPU_REG_WRITE(c, CTL_FETCH_PIPE_ACTIVE, 0);
}

struct ctl_blend_config {
	int idx, shift, ext_shift;
};

static const struct ctl_blend_config ctl_blend_config[][2] = {
	[SSPP_NONE] = { { -1 }, { -1 } },
	[SSPP_MAX] =  { { -1 }, { -1 } },
	[SSPP_VIG0] = { { 0, 0,  0  }, { 3, 0 } },
	[SSPP_VIG1] = { { 0, 3,  2  }, { 3, 4 } },
	[SSPP_VIG2] = { { 0, 6,  4  }, { 3, 8 } },
	[SSPP_VIG3] = { { 0, 26, 6  }, { 3, 12 } },
	[SSPP_RGB0] = { { 0, 9,  8  }, { -1 } },
	[SSPP_RGB1] = { { 0, 12, 10 }, { -1 } },
	[SSPP_RGB2] = { { 0, 15, 12 }, { -1 } },
	[SSPP_RGB3] = { { 0, 29, 14 }, { -1 } },
	[SSPP_DMA0] = { { 0, 18, 16 }, { 2, 8 } },
	[SSPP_DMA1] = { { 0, 21, 18 }, { 2, 12 } },
	[SSPP_DMA2] = { { 2, 0      }, { 2, 16 } },
	[SSPP_DMA3] = { { 2, 4      }, { 2, 20 } },
	[SSPP_DMA4] = { { 4, 0      }, { 4, 8 } },
	[SSPP_DMA5] = { { 4, 4      }, { 4, 12 } },
	[SSPP_CURSOR0] =  { { 1, 20 }, { -1 } },
	[SSPP_CURSOR1] =  { { 1, 26 }, { -1 } },
};

static void dpu_hw_ctl_setup_blendstage(struct dpu_hw_ctl *ctx,
	enum dpu_lm lm, struct dpu_hw_stage_cfg *stage_cfg)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	u32 mix, ext, mix_ext;
	u32 mixercfg[5] = { 0 };
	int i, j;
	int stages;
	int pipes_per_stage;

	stages = _mixer_stages(ctx->mixer_hw_caps, ctx->mixer_count, lm);
	if (stages < 0)
		return;

	if (test_bit(DPU_MIXER_SOURCESPLIT,
		&ctx->mixer_hw_caps->features))
		pipes_per_stage = PIPES_PER_STAGE;
	else
		pipes_per_stage = 1;

	mixercfg[0] = CTL_MIXER_BORDER_OUT; /* always set BORDER_OUT */

	if (!stage_cfg)
		goto exit;

	for (i = 0; i <= stages; i++) {
		/* overflow to ext register if 'i + 1 > 7' */
		mix = (i + 1) & 0x7;
		ext = i >= 7;
		mix_ext = (i + 1) & 0xf;

		for (j = 0 ; j < pipes_per_stage; j++) {
			enum dpu_sspp_multirect_index rect_index =
				stage_cfg->multirect_index[i][j];
			enum dpu_sspp pipe = stage_cfg->stage[i][j];
			const struct ctl_blend_config *cfg =
				&ctl_blend_config[pipe][rect_index == DPU_SSPP_RECT_1];

			/*
			 * CTL_LAYER has 3-bit field (and extra bits in EXT register),
			 * all EXT registers has 4-bit fields.
			 */
			if (cfg->idx == -1) {
				continue;
			} else if (cfg->idx == 0) {
				mixercfg[0] |= mix << cfg->shift;
				mixercfg[1] |= ext << cfg->ext_shift;
			} else {
				mixercfg[cfg->idx] |= mix_ext << cfg->shift;
			}
		}
	}

exit:
	DPU_REG_WRITE(c, CTL_LAYER(lm), mixercfg[0]);
	DPU_REG_WRITE(c, CTL_LAYER_EXT(lm), mixercfg[1]);
	DPU_REG_WRITE(c, CTL_LAYER_EXT2(lm), mixercfg[2]);
	DPU_REG_WRITE(c, CTL_LAYER_EXT3(lm), mixercfg[3]);
	if (ctx->mdss_ver->core_major_ver >= 9)
		DPU_REG_WRITE(c, CTL_LAYER_EXT4(lm), mixercfg[4]);
}


static void dpu_hw_ctl_intf_cfg_v1(struct dpu_hw_ctl *ctx,
		struct dpu_hw_intf_cfg *cfg)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	u32 intf_active = 0;
	u32 dsc_active = 0;
	u32 wb_active = 0;
	u32 cwb_active = 0;
	u32 mode_sel = 0;
	u32 merge_3d_active = 0;

	/* CTL_TOP[31:28] carries group_id to collate CTL paths
	 * per VM. Explicitly disable it until VM support is
	 * added in SW. Power on reset value is not disable.
	 */
	if (ctx->mdss_ver->core_major_ver >= 7)
		mode_sel = CTL_DEFAULT_GROUP_ID  << 28;

	if (cfg->intf_mode_sel == DPU_CTL_MODE_SEL_CMD)
		mode_sel |= BIT(17);

	intf_active = DPU_REG_READ(c, CTL_INTF_ACTIVE);
	wb_active = DPU_REG_READ(c, CTL_WB_ACTIVE);
	cwb_active = DPU_REG_READ(c, CTL_CWB_ACTIVE);
	dsc_active = DPU_REG_READ(c, CTL_DSC_ACTIVE);
	merge_3d_active = DPU_REG_READ(c, CTL_MERGE_3D_ACTIVE);

	if (cfg->intf)
		intf_active |= BIT(cfg->intf - INTF_0);

	if (cfg->wb)
		wb_active |= BIT(cfg->wb - WB_0);

	if (cfg->cwb)
		cwb_active |= cfg->cwb;

	if (cfg->dsc)
		dsc_active |= cfg->dsc;

	if (cfg->merge_3d)
		merge_3d_active |= BIT(cfg->merge_3d - MERGE_3D_0);

	DPU_REG_WRITE(c, CTL_TOP, mode_sel);
	DPU_REG_WRITE(c, CTL_INTF_ACTIVE, intf_active);
	DPU_REG_WRITE(c, CTL_WB_ACTIVE, wb_active);
	DPU_REG_WRITE(c, CTL_CWB_ACTIVE, cwb_active);
	DPU_REG_WRITE(c, CTL_DSC_ACTIVE, dsc_active);
	DPU_REG_WRITE(c, CTL_MERGE_3D_ACTIVE, merge_3d_active);

	if (cfg->intf_master)
		DPU_REG_WRITE(c, CTL_INTF_MASTER, BIT(cfg->intf_master - INTF_0));

	if (cfg->cdm)
		DPU_REG_WRITE(c, CTL_CDM_ACTIVE, cfg->cdm);
}

static void dpu_hw_ctl_intf_cfg(struct dpu_hw_ctl *ctx,
		struct dpu_hw_intf_cfg *cfg)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	u32 intf_cfg = 0;

	intf_cfg |= (cfg->intf & 0xF) << 4;

	if (cfg->mode_3d) {
		intf_cfg |= BIT(19);
		intf_cfg |= (cfg->mode_3d - 0x1) << 20;
	}

	if (cfg->wb)
		intf_cfg |= (cfg->wb & 0x3) + 2;

	switch (cfg->intf_mode_sel) {
	case DPU_CTL_MODE_SEL_VID:
		intf_cfg &= ~BIT(17);
		intf_cfg &= ~(0x3 << 15);
		break;
	case DPU_CTL_MODE_SEL_CMD:
		intf_cfg |= BIT(17);
		intf_cfg |= ((cfg->stream_sel & 0x3) << 15);
		break;
	default:
		pr_err("unknown interface type %d\n", cfg->intf_mode_sel);
		return;
	}

	DPU_REG_WRITE(c, CTL_TOP, intf_cfg);
}

static void dpu_hw_ctl_reset_intf_cfg_v1(struct dpu_hw_ctl *ctx,
		struct dpu_hw_intf_cfg *cfg)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	u32 intf_active = 0;
	u32 intf_master = 0;
	u32 wb_active = 0;
	u32 cwb_active = 0;
	u32 merge3d_active = 0;
	u32 dsc_active;
	u32 cdm_active;

	/*
	 * This API resets each portion of the CTL path namely,
	 * clearing the sspps staged on the lm, merge_3d block,
	 * interfaces , writeback etc to ensure clean teardown of the pipeline.
	 * This will be used for writeback to begin with to have a
	 * proper teardown of the writeback session but upon further
	 * validation, this can be extended to all interfaces.
	 */
	if (cfg->merge_3d) {
		merge3d_active = DPU_REG_READ(c, CTL_MERGE_3D_ACTIVE);
		merge3d_active &= ~BIT(cfg->merge_3d - MERGE_3D_0);
		DPU_REG_WRITE(c, CTL_MERGE_3D_ACTIVE,
				merge3d_active);
	}

	if (ctx->ops.clear_all_blendstages)
		ctx->ops.clear_all_blendstages(ctx);

	if (ctx->ops.set_active_lms)
		ctx->ops.set_active_lms(ctx, NULL);

	if (ctx->ops.set_active_fetch_pipes)
		ctx->ops.set_active_fetch_pipes(ctx, NULL);

	if (ctx->ops.set_active_pipes)
		ctx->ops.set_active_pipes(ctx, NULL);

	if (cfg->intf) {
		intf_active = DPU_REG_READ(c, CTL_INTF_ACTIVE);
		intf_active &= ~BIT(cfg->intf - INTF_0);
		DPU_REG_WRITE(c, CTL_INTF_ACTIVE, intf_active);

		intf_master = DPU_REG_READ(c, CTL_INTF_MASTER);

		/* Unset this intf as master, if it is the current master */
		if (intf_master == BIT(cfg->intf - INTF_0)) {
			DPU_DEBUG_DRIVER("Unsetting INTF_%d master\n", cfg->intf - INTF_0);
			DPU_REG_WRITE(c, CTL_INTF_MASTER, 0);
		}
	}

	if (cfg->cwb) {
		cwb_active = DPU_REG_READ(c, CTL_CWB_ACTIVE);
		cwb_active &= ~cfg->cwb;
		DPU_REG_WRITE(c, CTL_CWB_ACTIVE, cwb_active);
	}

	if (cfg->wb) {
		wb_active = DPU_REG_READ(c, CTL_WB_ACTIVE);
		wb_active &= ~BIT(cfg->wb - WB_0);
		DPU_REG_WRITE(c, CTL_WB_ACTIVE, wb_active);
	}

	if (cfg->dsc) {
		dsc_active = DPU_REG_READ(c, CTL_DSC_ACTIVE);
		dsc_active &= ~cfg->dsc;
		DPU_REG_WRITE(c, CTL_DSC_ACTIVE, dsc_active);
	}

	if (cfg->cdm) {
		cdm_active = DPU_REG_READ(c, CTL_CDM_ACTIVE);
		cdm_active &= ~cfg->cdm;
		DPU_REG_WRITE(c, CTL_CDM_ACTIVE, cdm_active);
	}
}

static void dpu_hw_ctl_set_active_fetch_pipes(struct dpu_hw_ctl *ctx,
					      unsigned long *fetch_active)
{
	int i;
	u32 val = 0;

	if (fetch_active) {
		for (i = 0; i < SSPP_MAX; i++) {
			if (test_bit(i, fetch_active) &&
				fetch_tbl[i] != CTL_INVALID_BIT)
				val |= BIT(fetch_tbl[i]);
		}
	}

	DPU_REG_WRITE(&ctx->hw, CTL_FETCH_PIPE_ACTIVE, val);
}

static void dpu_hw_ctl_set_active_pipes(struct dpu_hw_ctl *ctx,
					unsigned long *active_pipes)
{
	int i;
	u32 val = 0;

	if (active_pipes) {
		for (i = 0; i < SSPP_MAX; i++) {
			if (test_bit(i, active_pipes) &&
			    fetch_tbl[i] != CTL_INVALID_BIT)
				val |= BIT(fetch_tbl[i]);
		}
	}

	DPU_REG_WRITE(&ctx->hw, CTL_PIPE_ACTIVE, val);
}

static void dpu_hw_ctl_set_active_lms(struct dpu_hw_ctl *ctx,
				      unsigned long *active_lms)
{
	int i;
	u32 val = 0;

	if (active_lms) {
		for (i = LM_0; i < LM_MAX; i++) {
			if (test_bit(i, active_lms) &&
			    lm_tbl[i] != CTL_INVALID_BIT)
				val |= BIT(lm_tbl[i]);
		}
	}

	DPU_REG_WRITE(&ctx->hw, CTL_LAYER_ACTIVE, val);
}

/**
 * dpu_hw_ctl_init() - Initializes the ctl_path hw driver object.
 * Should be called before accessing any ctl_path register.
 * @dev:  Corresponding device for devres management
 * @cfg:  ctl_path catalog entry for which driver object is required
 * @addr: mapped register io address of MDP
 * @mdss_ver: dpu core's major and minor versions
 * @mixer_count: Number of mixers in @mixer
 * @mixer: Pointer to an array of Layer Mixers defined in the catalog
 */
struct dpu_hw_ctl *dpu_hw_ctl_init(struct drm_device *dev,
				   const struct dpu_ctl_cfg *cfg,
				   void __iomem *addr,
				   const struct dpu_mdss_version *mdss_ver,
				   u32 mixer_count,
				   const struct dpu_lm_cfg *mixer)
{
	struct dpu_hw_ctl *c;

	c = drmm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);

	c->hw.blk_addr = addr + cfg->base;
	c->hw.log_mask = DPU_DBG_MASK_CTL;

	c->caps = cfg;
	c->mdss_ver = mdss_ver;

	if (mdss_ver->core_major_ver >= 5) {
		c->ops.trigger_flush = dpu_hw_ctl_trigger_flush_v1;
		c->ops.setup_intf_cfg = dpu_hw_ctl_intf_cfg_v1;
		c->ops.reset_intf_cfg = dpu_hw_ctl_reset_intf_cfg_v1;
		c->ops.update_pending_flush_intf =
			dpu_hw_ctl_update_pending_flush_intf_v1;

		c->ops.update_pending_flush_periph =
			dpu_hw_ctl_update_pending_flush_periph_v1;

		c->ops.update_pending_flush_merge_3d =
			dpu_hw_ctl_update_pending_flush_merge_3d_v1;
		c->ops.update_pending_flush_wb = dpu_hw_ctl_update_pending_flush_wb_v1;
		c->ops.update_pending_flush_cwb = dpu_hw_ctl_update_pending_flush_cwb_v1;
		c->ops.update_pending_flush_dsc =
			dpu_hw_ctl_update_pending_flush_dsc_v1;
		c->ops.update_pending_flush_cdm = dpu_hw_ctl_update_pending_flush_cdm_v1;
	} else {
		c->ops.trigger_flush = dpu_hw_ctl_trigger_flush;
		c->ops.setup_intf_cfg = dpu_hw_ctl_intf_cfg;
		c->ops.update_pending_flush_intf =
			dpu_hw_ctl_update_pending_flush_intf;
		c->ops.update_pending_flush_wb = dpu_hw_ctl_update_pending_flush_wb;
		c->ops.update_pending_flush_cdm = dpu_hw_ctl_update_pending_flush_cdm;
	}
	c->ops.clear_pending_flush = dpu_hw_ctl_clear_pending_flush;
	c->ops.update_pending_flush = dpu_hw_ctl_update_pending_flush;
	c->ops.get_pending_flush = dpu_hw_ctl_get_pending_flush;
	c->ops.get_flush_register = dpu_hw_ctl_get_flush_register;
	c->ops.trigger_start = dpu_hw_ctl_trigger_start;
	c->ops.is_started = dpu_hw_ctl_is_started;
	c->ops.trigger_pending = dpu_hw_ctl_trigger_pending;
	c->ops.reset = dpu_hw_ctl_reset_control;
	c->ops.wait_reset_status = dpu_hw_ctl_wait_reset_status;
	if (mdss_ver->core_major_ver < 12) {
		c->ops.clear_all_blendstages = dpu_hw_ctl_clear_all_blendstages;
		c->ops.setup_blendstage = dpu_hw_ctl_setup_blendstage;
	} else {
		c->ops.set_active_pipes = dpu_hw_ctl_set_active_pipes;
		c->ops.set_active_lms = dpu_hw_ctl_set_active_lms;
	}
	c->ops.update_pending_flush_sspp = dpu_hw_ctl_update_pending_flush_sspp;
	c->ops.update_pending_flush_mixer = dpu_hw_ctl_update_pending_flush_mixer;
	if (mdss_ver->core_major_ver >= 7)
		c->ops.update_pending_flush_dspp = dpu_hw_ctl_update_pending_flush_dspp_sub_blocks;
	else
		c->ops.update_pending_flush_dspp = dpu_hw_ctl_update_pending_flush_dspp;

	if (mdss_ver->core_major_ver >= 7)
		c->ops.set_active_fetch_pipes = dpu_hw_ctl_set_active_fetch_pipes;

	c->idx = cfg->id;
	c->mixer_count = mixer_count;
	c->mixer_hw_caps = mixer;

	return c;
}
