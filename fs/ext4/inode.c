// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext4/inode.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *	(jj@sunsite.ms.mff.cuni.cz)
 *
 *  Assorted race fixes, rewrite of ext4_get_block() by Al Viro, 2000
 */

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/time.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/dax.h>
#include <linux/quotaops.h>
#include <linux/string.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/pagevec.h>
#include <linux/mpage.h>
#include <linux/rmap.h>
#include <linux/namei.h>
#include <linux/uio.h>
#include <linux/bio.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/bitops.h>
#include <linux/iomap.h>
#include <linux/iversion.h>

#include "ext4_jbd2.h"
#include "xattr.h"
#include "acl.h"
#include "truncate.h"

#include <trace/events/ext4.h>

static void ext4_journalled_zero_new_buffers(handle_t *handle,
					    struct inode *inode,
					    struct folio *folio,
					    unsigned from, unsigned to);

static __u32 ext4_inode_csum(struct inode *inode, struct ext4_inode *raw,
			      struct ext4_inode_info *ei)
{
	__u32 csum;
	__u16 dummy_csum = 0;
	int offset = offsetof(struct ext4_inode, i_checksum_lo);
	unsigned int csum_size = sizeof(dummy_csum);

	csum = ext4_chksum(ei->i_csum_seed, (__u8 *)raw, offset);
	csum = ext4_chksum(csum, (__u8 *)&dummy_csum, csum_size);
	offset += csum_size;
	csum = ext4_chksum(csum, (__u8 *)raw + offset,
			   EXT4_GOOD_OLD_INODE_SIZE - offset);

	if (EXT4_INODE_SIZE(inode->i_sb) > EXT4_GOOD_OLD_INODE_SIZE) {
		offset = offsetof(struct ext4_inode, i_checksum_hi);
		csum = ext4_chksum(csum, (__u8 *)raw + EXT4_GOOD_OLD_INODE_SIZE,
				   offset - EXT4_GOOD_OLD_INODE_SIZE);
		if (EXT4_FITS_IN_INODE(raw, ei, i_checksum_hi)) {
			csum = ext4_chksum(csum, (__u8 *)&dummy_csum,
					   csum_size);
			offset += csum_size;
		}
		csum = ext4_chksum(csum, (__u8 *)raw + offset,
				   EXT4_INODE_SIZE(inode->i_sb) - offset);
	}

	return csum;
}

static int ext4_inode_csum_verify(struct inode *inode, struct ext4_inode *raw,
				  struct ext4_inode_info *ei)
{
	__u32 provided, calculated;

	if (EXT4_SB(inode->i_sb)->s_es->s_creator_os !=
	    cpu_to_le32(EXT4_OS_LINUX) ||
	    !ext4_has_feature_metadata_csum(inode->i_sb))
		return 1;

	provided = le16_to_cpu(raw->i_checksum_lo);
	calculated = ext4_inode_csum(inode, raw, ei);
	if (EXT4_INODE_SIZE(inode->i_sb) > EXT4_GOOD_OLD_INODE_SIZE &&
	    EXT4_FITS_IN_INODE(raw, ei, i_checksum_hi))
		provided |= ((__u32)le16_to_cpu(raw->i_checksum_hi)) << 16;
	else
		calculated &= 0xFFFF;

	return provided == calculated;
}

void ext4_inode_csum_set(struct inode *inode, struct ext4_inode *raw,
			 struct ext4_inode_info *ei)
{
	__u32 csum;

	if (EXT4_SB(inode->i_sb)->s_es->s_creator_os !=
	    cpu_to_le32(EXT4_OS_LINUX) ||
	    !ext4_has_feature_metadata_csum(inode->i_sb))
		return;

	csum = ext4_inode_csum(inode, raw, ei);
	raw->i_checksum_lo = cpu_to_le16(csum & 0xFFFF);
	if (EXT4_INODE_SIZE(inode->i_sb) > EXT4_GOOD_OLD_INODE_SIZE &&
	    EXT4_FITS_IN_INODE(raw, ei, i_checksum_hi))
		raw->i_checksum_hi = cpu_to_le16(csum >> 16);
}

static inline int ext4_begin_ordered_truncate(struct inode *inode,
					      loff_t new_size)
{
	trace_ext4_begin_ordered_truncate(inode, new_size);
	/*
	 * If jinode is zero, then we never opened the file for
	 * writing, so there's no need to call
	 * jbd2_journal_begin_ordered_truncate() since there's no
	 * outstanding writes we need to flush.
	 */
	if (!EXT4_I(inode)->jinode)
		return 0;
	return jbd2_journal_begin_ordered_truncate(EXT4_JOURNAL(inode),
						   EXT4_I(inode)->jinode,
						   new_size);
}

/*
 * Test whether an inode is a fast symlink.
 * A fast symlink has its symlink data stored in ext4_inode_info->i_data.
 */
int ext4_inode_is_fast_symlink(struct inode *inode)
{
	if (!(EXT4_I(inode)->i_flags & EXT4_EA_INODE_FL)) {
		int ea_blocks = EXT4_I(inode)->i_file_acl ?
				EXT4_CLUSTER_SIZE(inode->i_sb) >> 9 : 0;

		if (ext4_has_inline_data(inode))
			return 0;

		return (S_ISLNK(inode->i_mode) && inode->i_blocks - ea_blocks == 0);
	}
	return S_ISLNK(inode->i_mode) && inode->i_size &&
	       (inode->i_size < EXT4_N_BLOCKS * 4);
}

/*
 * Called at the last iput() if i_nlink is zero.
 */
void ext4_evict_inode(struct inode *inode)
{
	handle_t *handle;
	int err;
	/*
	 * Credits for final inode cleanup and freeing:
	 * sb + inode (ext4_orphan_del()), block bitmap, group descriptor
	 * (xattr block freeing), bitmap, group descriptor (inode freeing)
	 */
	int extra_credits = 6;
	struct ext4_xattr_inode_array *ea_inode_array = NULL;
	bool freeze_protected = false;

	trace_ext4_evict_inode(inode);

	dax_break_layout_final(inode);

	if (EXT4_I(inode)->i_flags & EXT4_EA_INODE_FL)
		ext4_evict_ea_inode(inode);
	if (inode->i_nlink) {
		truncate_inode_pages_final(&inode->i_data);

		goto no_delete;
	}

	if (is_bad_inode(inode))
		goto no_delete;
	dquot_initialize(inode);

	if (ext4_should_order_data(inode))
		ext4_begin_ordered_truncate(inode, 0);
	truncate_inode_pages_final(&inode->i_data);

	/*
	 * For inodes with journalled data, transaction commit could have
	 * dirtied the inode. And for inodes with dioread_nolock, unwritten
	 * extents converting worker could merge extents and also have dirtied
	 * the inode. Flush worker is ignoring it because of I_FREEING flag but
	 * we still need to remove the inode from the writeback lists.
	 */
	if (!list_empty_careful(&inode->i_io_list))
		inode_io_list_del(inode);

	/*
	 * Protect us against freezing - iput() caller didn't have to have any
	 * protection against it. When we are in a running transaction though,
	 * we are already protected against freezing and we cannot grab further
	 * protection due to lock ordering constraints.
	 */
	if (!ext4_journal_current_handle()) {
		sb_start_intwrite(inode->i_sb);
		freeze_protected = true;
	}

	if (!IS_NOQUOTA(inode))
		extra_credits += EXT4_MAXQUOTAS_DEL_BLOCKS(inode->i_sb);

	/*
	 * Block bitmap, group descriptor, and inode are accounted in both
	 * ext4_blocks_for_truncate() and extra_credits. So subtract 3.
	 */
	handle = ext4_journal_start(inode, EXT4_HT_TRUNCATE,
			 ext4_blocks_for_truncate(inode) + extra_credits - 3);
	if (IS_ERR(handle)) {
		ext4_std_error(inode->i_sb, PTR_ERR(handle));
		/*
		 * If we're going to skip the normal cleanup, we still need to
		 * make sure that the in-core orphan linked list is properly
		 * cleaned up.
		 */
		ext4_orphan_del(NULL, inode);
		if (freeze_protected)
			sb_end_intwrite(inode->i_sb);
		goto no_delete;
	}

	if (IS_SYNC(inode))
		ext4_handle_sync(handle);

	/*
	 * Set inode->i_size to 0 before calling ext4_truncate(). We need
	 * special handling of symlinks here because i_size is used to
	 * determine whether ext4_inode_info->i_data contains symlink data or
	 * block mappings. Setting i_size to 0 will remove its fast symlink
	 * status. Erase i_data so that it becomes a valid empty block map.
	 */
	if (ext4_inode_is_fast_symlink(inode))
		memset(EXT4_I(inode)->i_data, 0, sizeof(EXT4_I(inode)->i_data));
	inode->i_size = 0;
	err = ext4_mark_inode_dirty(handle, inode);
	if (err) {
		ext4_warning(inode->i_sb,
			     "couldn't mark inode dirty (err %d)", err);
		goto stop_handle;
	}
	if (inode->i_blocks) {
		err = ext4_truncate(inode);
		if (err) {
			ext4_error_err(inode->i_sb, -err,
				       "couldn't truncate inode %lu (err %d)",
				       inode->i_ino, err);
			goto stop_handle;
		}
	}

	/* Remove xattr references. */
	err = ext4_xattr_delete_inode(handle, inode, &ea_inode_array,
				      extra_credits);
	if (err) {
		ext4_warning(inode->i_sb, "xattr delete (err %d)", err);
stop_handle:
		ext4_journal_stop(handle);
		ext4_orphan_del(NULL, inode);
		if (freeze_protected)
			sb_end_intwrite(inode->i_sb);
		ext4_xattr_inode_array_free(ea_inode_array);
		goto no_delete;
	}

	/*
	 * Kill off the orphan record which ext4_truncate created.
	 * AKPM: I think this can be inside the above `if'.
	 * Note that ext4_orphan_del() has to be able to cope with the
	 * deletion of a non-existent orphan - this is because we don't
	 * know if ext4_truncate() actually created an orphan record.
	 * (Well, we could do this if we need to, but heck - it works)
	 */
	ext4_orphan_del(handle, inode);
	EXT4_I(inode)->i_dtime	= (__u32)ktime_get_real_seconds();

	/*
	 * One subtle ordering requirement: if anything has gone wrong
	 * (transaction abort, IO errors, whatever), then we can still
	 * do these next steps (the fs will already have been marked as
	 * having errors), but we can't free the inode if the mark_dirty
	 * fails.
	 */
	if (ext4_mark_inode_dirty(handle, inode))
		/* If that failed, just do the required in-core inode clear. */
		ext4_clear_inode(inode);
	else
		ext4_free_inode(handle, inode);
	ext4_journal_stop(handle);
	if (freeze_protected)
		sb_end_intwrite(inode->i_sb);
	ext4_xattr_inode_array_free(ea_inode_array);
	return;
no_delete:
	/*
	 * Check out some where else accidentally dirty the evicting inode,
	 * which may probably cause inode use-after-free issues later.
	 */
	WARN_ON_ONCE(!list_empty_careful(&inode->i_io_list));

	if (!list_empty(&EXT4_I(inode)->i_fc_list))
		ext4_fc_mark_ineligible(inode->i_sb, EXT4_FC_REASON_NOMEM, NULL);
	ext4_clear_inode(inode);	/* We must guarantee clearing of inode... */
}

#ifdef CONFIG_QUOTA
qsize_t *ext4_get_reserved_space(struct inode *inode)
{
	return &EXT4_I(inode)->i_reserved_quota;
}
#endif

/*
 * Called with i_data_sem down, which is important since we can call
 * ext4_discard_preallocations() from here.
 */
void ext4_da_update_reserve_space(struct inode *inode,
					int used, int quota_claim)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_inode_info *ei = EXT4_I(inode);

	spin_lock(&ei->i_block_reservation_lock);
	trace_ext4_da_update_reserve_space(inode, used, quota_claim);
	if (unlikely(used > ei->i_reserved_data_blocks)) {
		ext4_warning(inode->i_sb, "%s: ino %lu, used %d "
			 "with only %d reserved data blocks",
			 __func__, inode->i_ino, used,
			 ei->i_reserved_data_blocks);
		WARN_ON(1);
		used = ei->i_reserved_data_blocks;
	}

	/* Update per-inode reservations */
	ei->i_reserved_data_blocks -= used;
	percpu_counter_sub(&sbi->s_dirtyclusters_counter, used);

	spin_unlock(&ei->i_block_reservation_lock);

	/* Update quota subsystem for data blocks */
	if (quota_claim)
		dquot_claim_block(inode, EXT4_C2B(sbi, used));
	else {
		/*
		 * We did fallocate with an offset that is already delayed
		 * allocated. So on delayed allocated writeback we should
		 * not re-claim the quota for fallocated blocks.
		 */
		dquot_release_reservation_block(inode, EXT4_C2B(sbi, used));
	}

	/*
	 * If we have done all the pending block allocations and if
	 * there aren't any writers on the inode, we can discard the
	 * inode's preallocations.
	 */
	if ((ei->i_reserved_data_blocks == 0) &&
	    !inode_is_open_for_write(inode))
		ext4_discard_preallocations(inode);
}

static int __check_block_validity(struct inode *inode, const char *func,
				unsigned int line,
				struct ext4_map_blocks *map)
{
	journal_t *journal = EXT4_SB(inode->i_sb)->s_journal;

	if (journal && inode == journal->j_inode)
		return 0;

	if (!ext4_inode_block_valid(inode, map->m_pblk, map->m_len)) {
		ext4_error_inode(inode, func, line, map->m_pblk,
				 "lblock %lu mapped to illegal pblock %llu "
				 "(length %d)", (unsigned long) map->m_lblk,
				 map->m_pblk, map->m_len);
		return -EFSCORRUPTED;
	}
	return 0;
}

int ext4_issue_zeroout(struct inode *inode, ext4_lblk_t lblk, ext4_fsblk_t pblk,
		       ext4_lblk_t len)
{
	int ret;

	if (IS_ENCRYPTED(inode) && S_ISREG(inode->i_mode))
		return fscrypt_zeroout_range(inode, lblk, pblk, len);

	ret = sb_issue_zeroout(inode->i_sb, pblk, len, GFP_NOFS);
	if (ret > 0)
		ret = 0;

	return ret;
}

/*
 * For generic regular files, when updating the extent tree, Ext4 should
 * hold the i_rwsem and invalidate_lock exclusively. This ensures
 * exclusion against concurrent page faults, as well as reads and writes.
 */
#ifdef CONFIG_EXT4_DEBUG
void ext4_check_map_extents_env(struct inode *inode)
{
	if (EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
		return;

	if (!S_ISREG(inode->i_mode) ||
	    IS_NOQUOTA(inode) || IS_VERITY(inode) ||
	    is_special_ino(inode->i_sb, inode->i_ino) ||
	    (inode->i_state & (I_FREEING | I_WILL_FREE | I_NEW)) ||
	    ext4_test_inode_flag(inode, EXT4_INODE_EA_INODE) ||
	    ext4_verity_in_progress(inode))
		return;

	WARN_ON_ONCE(!inode_is_locked(inode) &&
		     !rwsem_is_locked(&inode->i_mapping->invalidate_lock));
}
#else
void ext4_check_map_extents_env(struct inode *inode) {}
#endif

#define check_block_validity(inode, map)	\
	__check_block_validity((inode), __func__, __LINE__, (map))

#ifdef ES_AGGRESSIVE_TEST
static void ext4_map_blocks_es_recheck(handle_t *handle,
				       struct inode *inode,
				       struct ext4_map_blocks *es_map,
				       struct ext4_map_blocks *map,
				       int flags)
{
	int retval;

	map->m_flags = 0;
	/*
	 * There is a race window that the result is not the same.
	 * e.g. xfstests #223 when dioread_nolock enables.  The reason
	 * is that we lookup a block mapping in extent status tree with
	 * out taking i_data_sem.  So at the time the unwritten extent
	 * could be converted.
	 */
	down_read(&EXT4_I(inode)->i_data_sem);
	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)) {
		retval = ext4_ext_map_blocks(handle, inode, map, 0);
	} else {
		retval = ext4_ind_map_blocks(handle, inode, map, 0);
	}
	up_read((&EXT4_I(inode)->i_data_sem));

	/*
	 * We don't check m_len because extent will be collpased in status
	 * tree.  So the m_len might not equal.
	 */
	if (es_map->m_lblk != map->m_lblk ||
	    es_map->m_flags != map->m_flags ||
	    es_map->m_pblk != map->m_pblk) {
		printk("ES cache assertion failed for inode: %lu "
		       "es_cached ex [%d/%d/%llu/%x] != "
		       "found ex [%d/%d/%llu/%x] retval %d flags %x\n",
		       inode->i_ino, es_map->m_lblk, es_map->m_len,
		       es_map->m_pblk, es_map->m_flags, map->m_lblk,
		       map->m_len, map->m_pblk, map->m_flags,
		       retval, flags);
	}
}
#endif /* ES_AGGRESSIVE_TEST */

static int ext4_map_query_blocks_next_in_leaf(handle_t *handle,
			struct inode *inode, struct ext4_map_blocks *map,
			unsigned int orig_mlen)
{
	struct ext4_map_blocks map2;
	unsigned int status, status2;
	int retval;

	status = map->m_flags & EXT4_MAP_UNWRITTEN ?
		EXTENT_STATUS_UNWRITTEN : EXTENT_STATUS_WRITTEN;

	WARN_ON_ONCE(!(map->m_flags & EXT4_MAP_QUERY_LAST_IN_LEAF));
	WARN_ON_ONCE(orig_mlen <= map->m_len);

	/* Prepare map2 for lookup in next leaf block */
	map2.m_lblk = map->m_lblk + map->m_len;
	map2.m_len = orig_mlen - map->m_len;
	map2.m_flags = 0;
	retval = ext4_ext_map_blocks(handle, inode, &map2, 0);

	if (retval <= 0) {
		ext4_es_insert_extent(inode, map->m_lblk, map->m_len,
				      map->m_pblk, status, false);
		return map->m_len;
	}

	if (unlikely(retval != map2.m_len)) {
		ext4_warning(inode->i_sb,
			     "ES len assertion failed for inode "
			     "%lu: retval %d != map->m_len %d",
			     inode->i_ino, retval, map2.m_len);
		WARN_ON(1);
	}

	status2 = map2.m_flags & EXT4_MAP_UNWRITTEN ?
		EXTENT_STATUS_UNWRITTEN : EXTENT_STATUS_WRITTEN;

	/*
	 * If map2 is contiguous with map, then let's insert it as a single
	 * extent in es cache and return the combined length of both the maps.
	 */
	if (map->m_pblk + map->m_len == map2.m_pblk &&
			status == status2) {
		ext4_es_insert_extent(inode, map->m_lblk,
				      map->m_len + map2.m_len, map->m_pblk,
				      status, false);
		map->m_len += map2.m_len;
	} else {
		ext4_es_insert_extent(inode, map->m_lblk, map->m_len,
				      map->m_pblk, status, false);
	}

	return map->m_len;
}

static int ext4_map_query_blocks(handle_t *handle, struct inode *inode,
				 struct ext4_map_blocks *map, int flags)
{
	unsigned int status;
	int retval;
	unsigned int orig_mlen = map->m_len;

	flags &= EXT4_EX_QUERY_FILTER;
	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		retval = ext4_ext_map_blocks(handle, inode, map, flags);
	else
		retval = ext4_ind_map_blocks(handle, inode, map, flags);

	if (retval <= 0)
		return retval;

	if (unlikely(retval != map->m_len)) {
		ext4_warning(inode->i_sb,
			     "ES len assertion failed for inode "
			     "%lu: retval %d != map->m_len %d",
			     inode->i_ino, retval, map->m_len);
		WARN_ON(1);
	}

	/*
	 * No need to query next in leaf:
	 * - if returned extent is not last in leaf or
	 * - if the last in leaf is the full requested range
	 */
	if (!(map->m_flags & EXT4_MAP_QUERY_LAST_IN_LEAF) ||
			map->m_len == orig_mlen) {
		status = map->m_flags & EXT4_MAP_UNWRITTEN ?
				EXTENT_STATUS_UNWRITTEN : EXTENT_STATUS_WRITTEN;
		ext4_es_insert_extent(inode, map->m_lblk, map->m_len,
				      map->m_pblk, status, false);
		return retval;
	}

	return ext4_map_query_blocks_next_in_leaf(handle, inode, map,
						  orig_mlen);
}

static int ext4_map_create_blocks(handle_t *handle, struct inode *inode,
				  struct ext4_map_blocks *map, int flags)
{
	struct extent_status es;
	unsigned int status;
	int err, retval = 0;

	/*
	 * We pass in the magic EXT4_GET_BLOCKS_DELALLOC_RESERVE
	 * indicates that the blocks and quotas has already been
	 * checked when the data was copied into the page cache.
	 */
	if (map->m_flags & EXT4_MAP_DELAYED)
		flags |= EXT4_GET_BLOCKS_DELALLOC_RESERVE;

	/*
	 * Here we clear m_flags because after allocating an new extent,
	 * it will be set again.
	 */
	map->m_flags &= ~EXT4_MAP_FLAGS;

	/*
	 * We need to check for EXT4 here because migrate could have
	 * changed the inode type in between.
	 */
	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)) {
		retval = ext4_ext_map_blocks(handle, inode, map, flags);
	} else {
		retval = ext4_ind_map_blocks(handle, inode, map, flags);

		/*
		 * We allocated new blocks which will result in i_data's
		 * format changing. Force the migrate to fail by clearing
		 * migrate flags.
		 */
		if (retval > 0 && map->m_flags & EXT4_MAP_NEW)
			ext4_clear_inode_state(inode, EXT4_STATE_EXT_MIGRATE);
	}
	if (retval <= 0)
		return retval;

	if (unlikely(retval != map->m_len)) {
		ext4_warning(inode->i_sb,
			     "ES len assertion failed for inode %lu: "
			     "retval %d != map->m_len %d",
			     inode->i_ino, retval, map->m_len);
		WARN_ON(1);
	}

	/*
	 * We have to zeroout blocks before inserting them into extent
	 * status tree. Otherwise someone could look them up there and
	 * use them before they are really zeroed. We also have to
	 * unmap metadata before zeroing as otherwise writeback can
	 * overwrite zeros with stale data from block device.
	 */
	if (flags & EXT4_GET_BLOCKS_ZERO &&
	    map->m_flags & EXT4_MAP_MAPPED && map->m_flags & EXT4_MAP_NEW) {
		err = ext4_issue_zeroout(inode, map->m_lblk, map->m_pblk,
					 map->m_len);
		if (err)
			return err;
	}

	/*
	 * If the extent has been zeroed out, we don't need to update
	 * extent status tree.
	 */
	if (flags & EXT4_GET_BLOCKS_PRE_IO &&
	    ext4_es_lookup_extent(inode, map->m_lblk, NULL, &es)) {
		if (ext4_es_is_written(&es))
			return retval;
	}

	status = map->m_flags & EXT4_MAP_UNWRITTEN ?
			EXTENT_STATUS_UNWRITTEN : EXTENT_STATUS_WRITTEN;
	ext4_es_insert_extent(inode, map->m_lblk, map->m_len, map->m_pblk,
			      status, flags & EXT4_GET_BLOCKS_DELALLOC_RESERVE);

	return retval;
}

/*
 * The ext4_map_blocks() function tries to look up the requested blocks,
 * and returns if the blocks are already mapped.
 *
 * Otherwise it takes the write lock of the i_data_sem and allocate blocks
 * and store the allocated blocks in the result buffer head and mark it
 * mapped.
 *
 * If file type is extents based, it will call ext4_ext_map_blocks(),
 * Otherwise, call with ext4_ind_map_blocks() to handle indirect mapping
 * based files
 *
 * On success, it returns the number of blocks being mapped or allocated.
 * If flags doesn't contain EXT4_GET_BLOCKS_CREATE the blocks are
 * pre-allocated and unwritten, the resulting @map is marked as unwritten.
 * If the flags contain EXT4_GET_BLOCKS_CREATE, it will mark @map as mapped.
 *
 * It returns 0 if plain look up failed (blocks have not been allocated), in
 * that case, @map is returned as unmapped but we still do fill map->m_len to
 * indicate the length of a hole starting at map->m_lblk.
 *
 * It returns the error in case of allocation failure.
 */
int ext4_map_blocks(handle_t *handle, struct inode *inode,
		    struct ext4_map_blocks *map, int flags)
{
	struct extent_status es;
	int retval;
	int ret = 0;
	unsigned int orig_mlen = map->m_len;
#ifdef ES_AGGRESSIVE_TEST
	struct ext4_map_blocks orig_map;

	memcpy(&orig_map, map, sizeof(*map));
#endif

	map->m_flags = 0;
	ext_debug(inode, "flag 0x%x, max_blocks %u, logical block %lu\n",
		  flags, map->m_len, (unsigned long) map->m_lblk);

	/*
	 * ext4_map_blocks returns an int, and m_len is an unsigned int
	 */
	if (unlikely(map->m_len > INT_MAX))
		map->m_len = INT_MAX;

	/* We can handle the block number less than EXT_MAX_BLOCKS */
	if (unlikely(map->m_lblk >= EXT_MAX_BLOCKS))
		return -EFSCORRUPTED;

	/*
	 * Callers from the context of data submission are the only exceptions
	 * for regular files that do not hold the i_rwsem or invalidate_lock.
	 * However, caching unrelated ranges is not permitted.
	 */
	if (flags & EXT4_GET_BLOCKS_IO_SUBMIT)
		WARN_ON_ONCE(!(flags & EXT4_EX_NOCACHE));
	else
		ext4_check_map_extents_env(inode);

	/* Lookup extent status tree firstly */
	if (ext4_es_lookup_extent(inode, map->m_lblk, NULL, &es)) {
		if (ext4_es_is_written(&es) || ext4_es_is_unwritten(&es)) {
			map->m_pblk = ext4_es_pblock(&es) +
					map->m_lblk - es.es_lblk;
			map->m_flags |= ext4_es_is_written(&es) ?
					EXT4_MAP_MAPPED : EXT4_MAP_UNWRITTEN;
			retval = es.es_len - (map->m_lblk - es.es_lblk);
			if (retval > map->m_len)
				retval = map->m_len;
			map->m_len = retval;
		} else if (ext4_es_is_delayed(&es) || ext4_es_is_hole(&es)) {
			map->m_pblk = 0;
			map->m_flags |= ext4_es_is_delayed(&es) ?
					EXT4_MAP_DELAYED : 0;
			retval = es.es_len - (map->m_lblk - es.es_lblk);
			if (retval > map->m_len)
				retval = map->m_len;
			map->m_len = retval;
			retval = 0;
		} else {
			BUG();
		}

		if (flags & EXT4_GET_BLOCKS_CACHED_NOWAIT)
			return retval;
#ifdef ES_AGGRESSIVE_TEST
		ext4_map_blocks_es_recheck(handle, inode, map,
					   &orig_map, flags);
#endif
		if (!(flags & EXT4_GET_BLOCKS_QUERY_LAST_IN_LEAF) ||
				orig_mlen == map->m_len)
			goto found;

		map->m_len = orig_mlen;
	}
	/*
	 * In the query cache no-wait mode, nothing we can do more if we
	 * cannot find extent in the cache.
	 */
	if (flags & EXT4_GET_BLOCKS_CACHED_NOWAIT)
		return 0;

	/*
	 * Try to see if we can get the block without requesting a new
	 * file system block.
	 */
	down_read(&EXT4_I(inode)->i_data_sem);
	retval = ext4_map_query_blocks(handle, inode, map, flags);
	up_read((&EXT4_I(inode)->i_data_sem));

found:
	if (retval > 0 && map->m_flags & EXT4_MAP_MAPPED) {
		ret = check_block_validity(inode, map);
		if (ret != 0)
			return ret;
	}

	/* If it is only a block(s) look up */
	if ((flags & EXT4_GET_BLOCKS_CREATE) == 0)
		return retval;

	/*
	 * Returns if the blocks have already allocated
	 *
	 * Note that if blocks have been preallocated
	 * ext4_ext_map_blocks() returns with buffer head unmapped
	 */
	if (retval > 0 && map->m_flags & EXT4_MAP_MAPPED)
		/*
		 * If we need to convert extent to unwritten
		 * we continue and do the actual work in
		 * ext4_ext_map_blocks()
		 */
		if (!(flags & EXT4_GET_BLOCKS_CONVERT_UNWRITTEN))
			return retval;


	ext4_fc_track_inode(handle, inode);
	/*
	 * New blocks allocate and/or writing to unwritten extent
	 * will possibly result in updating i_data, so we take
	 * the write lock of i_data_sem, and call get_block()
	 * with create == 1 flag.
	 */
	down_write(&EXT4_I(inode)->i_data_sem);
	retval = ext4_map_create_blocks(handle, inode, map, flags);
	up_write((&EXT4_I(inode)->i_data_sem));
	if (retval > 0 && map->m_flags & EXT4_MAP_MAPPED) {
		ret = check_block_validity(inode, map);
		if (ret != 0)
			return ret;

		/*
		 * Inodes with freshly allocated blocks where contents will be
		 * visible after transaction commit must be on transaction's
		 * ordered data list.
		 */
		if (map->m_flags & EXT4_MAP_NEW &&
		    !(map->m_flags & EXT4_MAP_UNWRITTEN) &&
		    !(flags & EXT4_GET_BLOCKS_ZERO) &&
		    !ext4_is_quota_file(inode) &&
		    ext4_should_order_data(inode)) {
			loff_t start_byte =
				(loff_t)map->m_lblk << inode->i_blkbits;
			loff_t length = (loff_t)map->m_len << inode->i_blkbits;

			if (flags & EXT4_GET_BLOCKS_IO_SUBMIT)
				ret = ext4_jbd2_inode_add_wait(handle, inode,
						start_byte, length);
			else
				ret = ext4_jbd2_inode_add_write(handle, inode,
						start_byte, length);
			if (ret)
				return ret;
		}
	}
	if (retval > 0 && (map->m_flags & EXT4_MAP_UNWRITTEN ||
				map->m_flags & EXT4_MAP_MAPPED))
		ext4_fc_track_range(handle, inode, map->m_lblk,
					map->m_lblk + map->m_len - 1);
	if (retval < 0)
		ext_debug(inode, "failed with err %d\n", retval);
	return retval;
}

/*
 * Update EXT4_MAP_FLAGS in bh->b_state. For buffer heads attached to pages
 * we have to be careful as someone else may be manipulating b_state as well.
 */
static void ext4_update_bh_state(struct buffer_head *bh, unsigned long flags)
{
	unsigned long old_state;
	unsigned long new_state;

	flags &= EXT4_MAP_FLAGS;

	/* Dummy buffer_head? Set non-atomically. */
	if (!bh->b_folio) {
		bh->b_state = (bh->b_state & ~EXT4_MAP_FLAGS) | flags;
		return;
	}
	/*
	 * Someone else may be modifying b_state. Be careful! This is ugly but
	 * once we get rid of using bh as a container for mapping information
	 * to pass to / from get_block functions, this can go away.
	 */
	old_state = READ_ONCE(bh->b_state);
	do {
		new_state = (old_state & ~EXT4_MAP_FLAGS) | flags;
	} while (unlikely(!try_cmpxchg(&bh->b_state, &old_state, new_state)));
}

/*
 * Make sure that the current journal transaction has enough credits to map
 * one extent. Return -EAGAIN if it cannot extend the current running
 * transaction.
 */
static inline int ext4_journal_ensure_extent_credits(handle_t *handle,
						     struct inode *inode)
{
	int credits;
	int ret;

	/* Called from ext4_da_write_begin() which has no handle started? */
	if (!handle)
		return 0;

	credits = ext4_chunk_trans_blocks(inode, 1);
	ret = __ext4_journal_ensure_credits(handle, credits, credits, 0);
	return ret <= 0 ? ret : -EAGAIN;
}

static int _ext4_get_block(struct inode *inode, sector_t iblock,
			   struct buffer_head *bh, int flags)
{
	struct ext4_map_blocks map;
	int ret = 0;

	if (ext4_has_inline_data(inode))
		return -ERANGE;

	map.m_lblk = iblock;
	map.m_len = bh->b_size >> inode->i_blkbits;

	ret = ext4_map_blocks(ext4_journal_current_handle(), inode, &map,
			      flags);
	if (ret > 0) {
		map_bh(bh, inode->i_sb, map.m_pblk);
		ext4_update_bh_state(bh, map.m_flags);
		bh->b_size = inode->i_sb->s_blocksize * map.m_len;
		ret = 0;
	} else if (ret == 0) {
		/* hole case, need to fill in bh->b_size */
		bh->b_size = inode->i_sb->s_blocksize * map.m_len;
	}
	return ret;
}

int ext4_get_block(struct inode *inode, sector_t iblock,
		   struct buffer_head *bh, int create)
{
	return _ext4_get_block(inode, iblock, bh,
			       create ? EXT4_GET_BLOCKS_CREATE : 0);
}

/*
 * Get block function used when preparing for buffered write if we require
 * creating an unwritten extent if blocks haven't been allocated.  The extent
 * will be converted to written after the IO is complete.
 */
int ext4_get_block_unwritten(struct inode *inode, sector_t iblock,
			     struct buffer_head *bh_result, int create)
{
	int ret = 0;

	ext4_debug("ext4_get_block_unwritten: inode %lu, create flag %d\n",
		   inode->i_ino, create);
	ret = _ext4_get_block(inode, iblock, bh_result,
			       EXT4_GET_BLOCKS_CREATE_UNWRIT_EXT);

	/*
	 * If the buffer is marked unwritten, mark it as new to make sure it is
	 * zeroed out correctly in case of partial writes. Otherwise, there is
	 * a chance of stale data getting exposed.
	 */
	if (ret == 0 && buffer_unwritten(bh_result))
		set_buffer_new(bh_result);

	return ret;
}

/* Maximum number of blocks we map for direct IO at once. */
#define DIO_MAX_BLOCKS 4096

/*
 * `handle' can be NULL if create is zero
 */
struct buffer_head *ext4_getblk(handle_t *handle, struct inode *inode,
				ext4_lblk_t block, int map_flags)
{
	struct ext4_map_blocks map;
	struct buffer_head *bh;
	int create = map_flags & EXT4_GET_BLOCKS_CREATE;
	bool nowait = map_flags & EXT4_GET_BLOCKS_CACHED_NOWAIT;
	int err;

	ASSERT((EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
		    || handle != NULL || create == 0);
	ASSERT(create == 0 || !nowait);

	map.m_lblk = block;
	map.m_len = 1;
	err = ext4_map_blocks(handle, inode, &map, map_flags);

	if (err == 0)
		return create ? ERR_PTR(-ENOSPC) : NULL;
	if (err < 0)
		return ERR_PTR(err);

	if (nowait)
		return sb_find_get_block(inode->i_sb, map.m_pblk);

	/*
	 * Since bh could introduce extra ref count such as referred by
	 * journal_head etc. Try to avoid using __GFP_MOVABLE here
	 * as it may fail the migration when journal_head remains.
	 */
	bh = getblk_unmovable(inode->i_sb->s_bdev, map.m_pblk,
				inode->i_sb->s_blocksize);

	if (unlikely(!bh))
		return ERR_PTR(-ENOMEM);
	if (map.m_flags & EXT4_MAP_NEW) {
		ASSERT(create != 0);
		ASSERT((EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
			    || (handle != NULL));

		/*
		 * Now that we do not always journal data, we should
		 * keep in mind whether this should always journal the
		 * new buffer as metadata.  For now, regular file
		 * writes use ext4_get_block instead, so it's not a
		 * problem.
		 */
		lock_buffer(bh);
		BUFFER_TRACE(bh, "call get_create_access");
		err = ext4_journal_get_create_access(handle, inode->i_sb, bh,
						     EXT4_JTR_NONE);
		if (unlikely(err)) {
			unlock_buffer(bh);
			goto errout;
		}
		if (!buffer_uptodate(bh)) {
			memset(bh->b_data, 0, inode->i_sb->s_blocksize);
			set_buffer_uptodate(bh);
		}
		unlock_buffer(bh);
		BUFFER_TRACE(bh, "call ext4_handle_dirty_metadata");
		err = ext4_handle_dirty_metadata(handle, inode, bh);
		if (unlikely(err))
			goto errout;
	} else
		BUFFER_TRACE(bh, "not a new buffer");
	return bh;
errout:
	brelse(bh);
	return ERR_PTR(err);
}

struct buffer_head *ext4_bread(handle_t *handle, struct inode *inode,
			       ext4_lblk_t block, int map_flags)
{
	struct buffer_head *bh;
	int ret;

	bh = ext4_getblk(handle, inode, block, map_flags);
	if (IS_ERR(bh))
		return bh;
	if (!bh || ext4_buffer_uptodate(bh))
		return bh;

	ret = ext4_read_bh_lock(bh, REQ_META | REQ_PRIO, true);
	if (ret) {
		put_bh(bh);
		return ERR_PTR(ret);
	}
	return bh;
}

/* Read a contiguous batch of blocks. */
int ext4_bread_batch(struct inode *inode, ext4_lblk_t block, int bh_count,
		     bool wait, struct buffer_head **bhs)
{
	int i, err;

	for (i = 0; i < bh_count; i++) {
		bhs[i] = ext4_getblk(NULL, inode, block + i, 0 /* map_flags */);
		if (IS_ERR(bhs[i])) {
			err = PTR_ERR(bhs[i]);
			bh_count = i;
			goto out_brelse;
		}
	}

	for (i = 0; i < bh_count; i++)
		/* Note that NULL bhs[i] is valid because of holes. */
		if (bhs[i] && !ext4_buffer_uptodate(bhs[i]))
			ext4_read_bh_lock(bhs[i], REQ_META | REQ_PRIO, false);

	if (!wait)
		return 0;

	for (i = 0; i < bh_count; i++)
		if (bhs[i])
			wait_on_buffer(bhs[i]);

	for (i = 0; i < bh_count; i++) {
		if (bhs[i] && !buffer_uptodate(bhs[i])) {
			err = -EIO;
			goto out_brelse;
		}
	}
	return 0;

out_brelse:
	for (i = 0; i < bh_count; i++) {
		brelse(bhs[i]);
		bhs[i] = NULL;
	}
	return err;
}

int ext4_walk_page_buffers(handle_t *handle, struct inode *inode,
			   struct buffer_head *head,
			   unsigned from,
			   unsigned to,
			   int *partial,
			   int (*fn)(handle_t *handle, struct inode *inode,
				     struct buffer_head *bh))
{
	struct buffer_head *bh;
	unsigned block_start, block_end;
	unsigned blocksize = head->b_size;
	int err, ret = 0;
	struct buffer_head *next;

	for (bh = head, block_start = 0;
	     ret == 0 && (bh != head || !block_start);
	     block_start = block_end, bh = next) {
		next = bh->b_this_page;
		block_end = block_start + blocksize;
		if (block_end <= from || block_start >= to) {
			if (partial && !buffer_uptodate(bh))
				*partial = 1;
			continue;
		}
		err = (*fn)(handle, inode, bh);
		if (!ret)
			ret = err;
	}
	return ret;
}

/*
 * Helper for handling dirtying of journalled data. We also mark the folio as
 * dirty so that writeback code knows about this page (and inode) contains
 * dirty data. ext4_writepages() then commits appropriate transaction to
 * make data stable.
 */
static int ext4_dirty_journalled_data(handle_t *handle, struct buffer_head *bh)
{
	struct folio *folio = bh->b_folio;
	struct inode *inode = folio->mapping->host;

	/* only regular files have a_ops */
	if (S_ISREG(inode->i_mode))
		folio_mark_dirty(folio);
	return ext4_handle_dirty_metadata(handle, NULL, bh);
}

int do_journal_get_write_access(handle_t *handle, struct inode *inode,
				struct buffer_head *bh)
{
	if (!buffer_mapped(bh) || buffer_freed(bh))
		return 0;
	BUFFER_TRACE(bh, "get write access");
	return ext4_journal_get_write_access(handle, inode->i_sb, bh,
					    EXT4_JTR_NONE);
}

int ext4_block_write_begin(handle_t *handle, struct folio *folio,
			   loff_t pos, unsigned len,
			   get_block_t *get_block)
{
	unsigned int from = offset_in_folio(folio, pos);
	unsigned to = from + len;
	struct inode *inode = folio->mapping->host;
	unsigned block_start, block_end;
	sector_t block;
	int err = 0;
	unsigned blocksize = inode->i_sb->s_blocksize;
	unsigned bbits;
	struct buffer_head *bh, *head, *wait[2];
	int nr_wait = 0;
	int i;
	bool should_journal_data = ext4_should_journal_data(inode);

	BUG_ON(!folio_test_locked(folio));
	BUG_ON(to > folio_size(folio));
	BUG_ON(from > to);

	head = folio_buffers(folio);
	if (!head)
		head = create_empty_buffers(folio, blocksize, 0);
	bbits = ilog2(blocksize);
	block = (sector_t)folio->index << (PAGE_SHIFT - bbits);

	for (bh = head, block_start = 0; bh != head || !block_start;
	    block++, block_start = block_end, bh = bh->b_this_page) {
		block_end = block_start + blocksize;
		if (block_end <= from || block_start >= to) {
			if (folio_test_uptodate(folio)) {
				set_buffer_uptodate(bh);
			}
			continue;
		}
		if (WARN_ON_ONCE(buffer_new(bh)))
			clear_buffer_new(bh);
		if (!buffer_mapped(bh)) {
			WARN_ON(bh->b_size != blocksize);
			err = ext4_journal_ensure_extent_credits(handle, inode);
			if (!err)
				err = get_block(inode, block, bh, 1);
			if (err)
				break;
			if (buffer_new(bh)) {
				/*
				 * We may be zeroing partial buffers or all new
				 * buffers in case of failure. Prepare JBD2 for
				 * that.
				 */
				if (should_journal_data)
					do_journal_get_write_access(handle,
								    inode, bh);
				if (folio_test_uptodate(folio)) {
					/*
					 * Unlike __block_write_begin() we leave
					 * dirtying of new uptodate buffers to
					 * ->write_end() time or
					 * folio_zero_new_buffers().
					 */
					set_buffer_uptodate(bh);
					continue;
				}
				if (block_end > to || block_start < from)
					folio_zero_segments(folio, to,
							    block_end,
							    block_start, from);
				continue;
			}
		}
		if (folio_test_uptodate(folio)) {
			set_buffer_uptodate(bh);
			continue;
		}
		if (!buffer_uptodate(bh) && !buffer_delay(bh) &&
		    !buffer_unwritten(bh) &&
		    (block_start < from || block_end > to)) {
			ext4_read_bh_lock(bh, 0, false);
			wait[nr_wait++] = bh;
		}
	}
	/*
	 * If we issued read requests, let them complete.
	 */
	for (i = 0; i < nr_wait; i++) {
		wait_on_buffer(wait[i]);
		if (!buffer_uptodate(wait[i]))
			err = -EIO;
	}
	if (unlikely(err)) {
		if (should_journal_data)
			ext4_journalled_zero_new_buffers(handle, inode, folio,
							 from, to);
		else
			folio_zero_new_buffers(folio, from, to);
	} else if (fscrypt_inode_uses_fs_layer_crypto(inode)) {
		for (i = 0; i < nr_wait; i++) {
			int err2;

			err2 = fscrypt_decrypt_pagecache_blocks(folio,
						blocksize, bh_offset(wait[i]));
			if (err2) {
				clear_buffer_uptodate(wait[i]);
				err = err2;
			}
		}
	}

	return err;
}

/*
 * To preserve ordering, it is essential that the hole instantiation and
 * the data write be encapsulated in a single transaction.  We cannot
 * close off a transaction and start a new one between the ext4_get_block()
 * and the ext4_write_end().  So doing the jbd2_journal_start at the start of
 * ext4_write_begin() is the right place.
 */
static int ext4_write_begin(const struct kiocb *iocb,
			    struct address_space *mapping,
			    loff_t pos, unsigned len,
			    struct folio **foliop, void **fsdata)
{
	struct inode *inode = mapping->host;
	int ret, needed_blocks;
	handle_t *handle;
	int retries = 0;
	struct folio *folio;
	pgoff_t index;
	unsigned from, to;

	ret = ext4_emergency_state(inode->i_sb);
	if (unlikely(ret))
		return ret;

	trace_ext4_write_begin(inode, pos, len);
	/*
	 * Reserve one block more for addition to orphan list in case
	 * we allocate blocks but write fails for some reason
	 */
	needed_blocks = ext4_chunk_trans_extent(inode,
			ext4_journal_blocks_per_folio(inode)) + 1;
	index = pos >> PAGE_SHIFT;

	if (ext4_test_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA)) {
		ret = ext4_try_to_write_inline_data(mapping, inode, pos, len,
						    foliop);
		if (ret < 0)
			return ret;
		if (ret == 1)
			return 0;
	}

	/*
	 * write_begin_get_folio() can take a long time if the
	 * system is thrashing due to memory pressure, or if the folio
	 * is being written back.  So grab it first before we start
	 * the transaction handle.  This also allows us to allocate
	 * the folio (if needed) without using GFP_NOFS.
	 */
retry_grab:
	folio = write_begin_get_folio(iocb, mapping, index, len);
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	if (pos + len > folio_pos(folio) + folio_size(folio))
		len = folio_pos(folio) + folio_size(folio) - pos;

	from = offset_in_folio(folio, pos);
	to = from + len;

	/*
	 * The same as page allocation, we prealloc buffer heads before
	 * starting the handle.
	 */
	if (!folio_buffers(folio))
		create_empty_buffers(folio, inode->i_sb->s_blocksize, 0);

	folio_unlock(folio);

retry_journal:
	handle = ext4_journal_start(inode, EXT4_HT_WRITE_PAGE, needed_blocks);
	if (IS_ERR(handle)) {
		folio_put(folio);
		return PTR_ERR(handle);
	}

	folio_lock(folio);
	if (folio->mapping != mapping) {
		/* The folio got truncated from under us */
		folio_unlock(folio);
		folio_put(folio);
		ext4_journal_stop(handle);
		goto retry_grab;
	}
	/* In case writeback began while the folio was unlocked */
	folio_wait_stable(folio);

	if (ext4_should_dioread_nolock(inode))
		ret = ext4_block_write_begin(handle, folio, pos, len,
					     ext4_get_block_unwritten);
	else
		ret = ext4_block_write_begin(handle, folio, pos, len,
					     ext4_get_block);
	if (!ret && ext4_should_journal_data(inode)) {
		ret = ext4_walk_page_buffers(handle, inode,
					     folio_buffers(folio), from, to,
					     NULL, do_journal_get_write_access);
	}

	if (ret) {
		bool extended = (pos + len > inode->i_size) &&
				!ext4_verity_in_progress(inode);

		folio_unlock(folio);
		/*
		 * ext4_block_write_begin may have instantiated a few blocks
		 * outside i_size.  Trim these off again. Don't need
		 * i_size_read because we hold i_rwsem.
		 *
		 * Add inode to orphan list in case we crash before
		 * truncate finishes
		 */
		if (extended && ext4_can_truncate(inode))
			ext4_orphan_add(handle, inode);

		ext4_journal_stop(handle);
		if (extended) {
			ext4_truncate_failed_write(inode);
			/*
			 * If truncate failed early the inode might
			 * still be on the orphan list; we need to
			 * make sure the inode is removed from the
			 * orphan list in that case.
			 */
			if (inode->i_nlink)
				ext4_orphan_del(NULL, inode);
		}

		if (ret == -EAGAIN ||
		    (ret == -ENOSPC &&
		     ext4_should_retry_alloc(inode->i_sb, &retries)))
			goto retry_journal;
		folio_put(folio);
		return ret;
	}
	*foliop = folio;
	return ret;
}

/* For write_end() in data=journal mode */
static int write_end_fn(handle_t *handle, struct inode *inode,
			struct buffer_head *bh)
{
	int ret;
	if (!buffer_mapped(bh) || buffer_freed(bh))
		return 0;
	set_buffer_uptodate(bh);
	ret = ext4_dirty_journalled_data(handle, bh);
	clear_buffer_meta(bh);
	clear_buffer_prio(bh);
	clear_buffer_new(bh);
	return ret;
}

/*
 * We need to pick up the new inode size which generic_commit_write gave us
 * `iocb` can be NULL - eg, when called from page_symlink().
 *
 * ext4 never places buffers on inode->i_mapping->i_private_list.  metadata
 * buffers are managed internally.
 */
static int ext4_write_end(const struct kiocb *iocb,
			  struct address_space *mapping,
			  loff_t pos, unsigned len, unsigned copied,
			  struct folio *folio, void *fsdata)
{
	handle_t *handle = ext4_journal_current_handle();
	struct inode *inode = mapping->host;
	loff_t old_size = inode->i_size;
	int ret = 0, ret2;
	int i_size_changed = 0;
	bool verity = ext4_verity_in_progress(inode);

	trace_ext4_write_end(inode, pos, len, copied);

	if (ext4_has_inline_data(inode) &&
	    ext4_test_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA))
		return ext4_write_inline_data_end(inode, pos, len, copied,
						  folio);

	copied = block_write_end(pos, len, copied, folio);
	/*
	 * it's important to update i_size while still holding folio lock:
	 * page writeout could otherwise come in and zero beyond i_size.
	 *
	 * If FS_IOC_ENABLE_VERITY is running on this inode, then Merkle tree
	 * blocks are being written past EOF, so skip the i_size update.
	 */
	if (!verity)
		i_size_changed = ext4_update_inode_size(inode, pos + copied);
	folio_unlock(folio);
	folio_put(folio);

	if (old_size < pos && !verity) {
		pagecache_isize_extended(inode, old_size, pos);
		ext4_zero_partial_blocks(handle, inode, old_size, pos - old_size);
	}
	/*
	 * Don't mark the inode dirty under folio lock. First, it unnecessarily
	 * makes the holding time of folio lock longer. Second, it forces lock
	 * ordering of folio lock and transaction start for journaling
	 * filesystems.
	 */
	if (i_size_changed)
		ret = ext4_mark_inode_dirty(handle, inode);

	if (pos + len > inode->i_size && !verity && ext4_can_truncate(inode))
		/* if we have allocated more blocks and copied
		 * less. We will have blocks allocated outside
		 * inode->i_size. So truncate them
		 */
		ext4_orphan_add(handle, inode);

	ret2 = ext4_journal_stop(handle);
	if (!ret)
		ret = ret2;

	if (pos + len > inode->i_size && !verity) {
		ext4_truncate_failed_write(inode);
		/*
		 * If truncate failed early the inode might still be
		 * on the orphan list; we need to make sure the inode
		 * is removed from the orphan list in that case.
		 */
		if (inode->i_nlink)
			ext4_orphan_del(NULL, inode);
	}

	return ret ? ret : copied;
}

/*
 * This is a private version of folio_zero_new_buffers() which doesn't
 * set the buffer to be dirty, since in data=journalled mode we need
 * to call ext4_dirty_journalled_data() instead.
 */
static void ext4_journalled_zero_new_buffers(handle_t *handle,
					    struct inode *inode,
					    struct folio *folio,
					    unsigned from, unsigned to)
{
	unsigned int block_start = 0, block_end;
	struct buffer_head *head, *bh;

	bh = head = folio_buffers(folio);
	do {
		block_end = block_start + bh->b_size;
		if (buffer_new(bh)) {
			if (block_end > from && block_start < to) {
				if (!folio_test_uptodate(folio)) {
					unsigned start, size;

					start = max(from, block_start);
					size = min(to, block_end) - start;

					folio_zero_range(folio, start, size);
				}
				clear_buffer_new(bh);
				write_end_fn(handle, inode, bh);
			}
		}
		block_start = block_end;
		bh = bh->b_this_page;
	} while (bh != head);
}

static int ext4_journalled_write_end(const struct kiocb *iocb,
				     struct address_space *mapping,
				     loff_t pos, unsigned len, unsigned copied,
				     struct folio *folio, void *fsdata)
{
	handle_t *handle = ext4_journal_current_handle();
	struct inode *inode = mapping->host;
	loff_t old_size = inode->i_size;
	int ret = 0, ret2;
	int partial = 0;
	unsigned from, to;
	int size_changed = 0;
	bool verity = ext4_verity_in_progress(inode);

	trace_ext4_journalled_write_end(inode, pos, len, copied);
	from = pos & (PAGE_SIZE - 1);
	to = from + len;

	BUG_ON(!ext4_handle_valid(handle));

	if (ext4_has_inline_data(inode))
		return ext4_write_inline_data_end(inode, pos, len, copied,
						  folio);

	if (unlikely(copied < len) && !folio_test_uptodate(folio)) {
		copied = 0;
		ext4_journalled_zero_new_buffers(handle, inode, folio,
						 from, to);
	} else {
		if (unlikely(copied < len))
			ext4_journalled_zero_new_buffers(handle, inode, folio,
							 from + copied, to);
		ret = ext4_walk_page_buffers(handle, inode,
					     folio_buffers(folio),
					     from, from + copied, &partial,
					     write_end_fn);
		if (!partial)
			folio_mark_uptodate(folio);
	}
	if (!verity)
		size_changed = ext4_update_inode_size(inode, pos + copied);
	EXT4_I(inode)->i_datasync_tid = handle->h_transaction->t_tid;
	folio_unlock(folio);
	folio_put(folio);

	if (old_size < pos && !verity) {
		pagecache_isize_extended(inode, old_size, pos);
		ext4_zero_partial_blocks(handle, inode, old_size, pos - old_size);
	}

	if (size_changed) {
		ret2 = ext4_mark_inode_dirty(handle, inode);
		if (!ret)
			ret = ret2;
	}

	if (pos + len > inode->i_size && !verity && ext4_can_truncate(inode))
		/* if we have allocated more blocks and copied
		 * less. We will have blocks allocated outside
		 * inode->i_size. So truncate them
		 */
		ext4_orphan_add(handle, inode);

	ret2 = ext4_journal_stop(handle);
	if (!ret)
		ret = ret2;
	if (pos + len > inode->i_size && !verity) {
		ext4_truncate_failed_write(inode);
		/*
		 * If truncate failed early the inode might still be
		 * on the orphan list; we need to make sure the inode
		 * is removed from the orphan list in that case.
		 */
		if (inode->i_nlink)
			ext4_orphan_del(NULL, inode);
	}

	return ret ? ret : copied;
}

/*
 * Reserve space for 'nr_resv' clusters
 */
static int ext4_da_reserve_space(struct inode *inode, int nr_resv)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_inode_info *ei = EXT4_I(inode);
	int ret;

	/*
	 * We will charge metadata quota at writeout time; this saves
	 * us from metadata over-estimation, though we may go over by
	 * a small amount in the end.  Here we just reserve for data.
	 */
	ret = dquot_reserve_block(inode, EXT4_C2B(sbi, nr_resv));
	if (ret)
		return ret;

	spin_lock(&ei->i_block_reservation_lock);
	if (ext4_claim_free_clusters(sbi, nr_resv, 0)) {
		spin_unlock(&ei->i_block_reservation_lock);
		dquot_release_reservation_block(inode, EXT4_C2B(sbi, nr_resv));
		return -ENOSPC;
	}
	ei->i_reserved_data_blocks += nr_resv;
	trace_ext4_da_reserve_space(inode, nr_resv);
	spin_unlock(&ei->i_block_reservation_lock);

	return 0;       /* success */
}

void ext4_da_release_space(struct inode *inode, int to_free)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (!to_free)
		return;		/* Nothing to release, exit */

	spin_lock(&EXT4_I(inode)->i_block_reservation_lock);

	trace_ext4_da_release_space(inode, to_free);
	if (unlikely(to_free > ei->i_reserved_data_blocks)) {
		/*
		 * if there aren't enough reserved blocks, then the
		 * counter is messed up somewhere.  Since this
		 * function is called from invalidate page, it's
		 * harmless to return without any action.
		 */
		ext4_warning(inode->i_sb, "ext4_da_release_space: "
			 "ino %lu, to_free %d with only %d reserved "
			 "data blocks", inode->i_ino, to_free,
			 ei->i_reserved_data_blocks);
		WARN_ON(1);
		to_free = ei->i_reserved_data_blocks;
	}
	ei->i_reserved_data_blocks -= to_free;

	/* update fs dirty data blocks counter */
	percpu_counter_sub(&sbi->s_dirtyclusters_counter, to_free);

	spin_unlock(&EXT4_I(inode)->i_block_reservation_lock);

	dquot_release_reservation_block(inode, EXT4_C2B(sbi, to_free));
}

/*
 * Delayed allocation stuff
 */

struct mpage_da_data {
	/* These are input fields for ext4_do_writepages() */
	struct inode *inode;
	struct writeback_control *wbc;
	unsigned int can_map:1;	/* Can writepages call map blocks? */

	/* These are internal state of ext4_do_writepages() */
	loff_t start_pos;	/* The start pos to write */
	loff_t next_pos;	/* Current pos to examine */
	loff_t end_pos;		/* Last pos to examine */

	/*
	 * Extent to map - this can be after start_pos because that can be
	 * fully mapped. We somewhat abuse m_flags to store whether the extent
	 * is delalloc or unwritten.
	 */
	struct ext4_map_blocks map;
	struct ext4_io_submit io_submit;	/* IO submission data */
	unsigned int do_map:1;
	unsigned int scanned_until_end:1;
	unsigned int journalled_more_data:1;
};

static void mpage_release_unused_pages(struct mpage_da_data *mpd,
				       bool invalidate)
{
	unsigned nr, i;
	pgoff_t index, end;
	struct folio_batch fbatch;
	struct inode *inode = mpd->inode;
	struct address_space *mapping = inode->i_mapping;

	/* This is necessary when next_pos == 0. */
	if (mpd->start_pos >= mpd->next_pos)
		return;

	mpd->scanned_until_end = 0;
	if (invalidate) {
		ext4_lblk_t start, last;
		start = EXT4_B_TO_LBLK(inode, mpd->start_pos);
		last = mpd->next_pos >> inode->i_blkbits;

		/*
		 * avoid racing with extent status tree scans made by
		 * ext4_insert_delayed_block()
		 */
		down_write(&EXT4_I(inode)->i_data_sem);
		ext4_es_remove_extent(inode, start, last - start);
		up_write(&EXT4_I(inode)->i_data_sem);
	}

	folio_batch_init(&fbatch);
	index = mpd->start_pos >> PAGE_SHIFT;
	end = mpd->next_pos >> PAGE_SHIFT;
	while (index < end) {
		nr = filemap_get_folios(mapping, &index, end - 1, &fbatch);
		if (nr == 0)
			break;
		for (i = 0; i < nr; i++) {
			struct folio *folio = fbatch.folios[i];

			if (folio_pos(folio) < mpd->start_pos)
				continue;
			if (folio_next_index(folio) > end)
				continue;
			BUG_ON(!folio_test_locked(folio));
			BUG_ON(folio_test_writeback(folio));
			if (invalidate) {
				if (folio_mapped(folio))
					folio_clear_dirty_for_io(folio);
				block_invalidate_folio(folio, 0,
						folio_size(folio));
				folio_clear_uptodate(folio);
			}
			folio_unlock(folio);
		}
		folio_batch_release(&fbatch);
	}
}

static void ext4_print_free_blocks(struct inode *inode)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct super_block *sb = inode->i_sb;
	struct ext4_inode_info *ei = EXT4_I(inode);

	ext4_msg(sb, KERN_CRIT, "Total free blocks count %lld",
	       EXT4_C2B(EXT4_SB(inode->i_sb),
			ext4_count_free_clusters(sb)));
	ext4_msg(sb, KERN_CRIT, "Free/Dirty block details");
	ext4_msg(sb, KERN_CRIT, "free_blocks=%lld",
	       (long long) EXT4_C2B(EXT4_SB(sb),
		percpu_counter_sum(&sbi->s_freeclusters_counter)));
	ext4_msg(sb, KERN_CRIT, "dirty_blocks=%lld",
	       (long long) EXT4_C2B(EXT4_SB(sb),
		percpu_counter_sum(&sbi->s_dirtyclusters_counter)));
	ext4_msg(sb, KERN_CRIT, "Block reservation details");
	ext4_msg(sb, KERN_CRIT, "i_reserved_data_blocks=%u",
		 ei->i_reserved_data_blocks);
	return;
}

/*
 * Check whether the cluster containing lblk has been allocated or has
 * delalloc reservation.
 *
 * Returns 0 if the cluster doesn't have either, 1 if it has delalloc
 * reservation, 2 if it's already been allocated, negative error code on
 * failure.
 */
static int ext4_clu_alloc_state(struct inode *inode, ext4_lblk_t lblk)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	int ret;

	/* Has delalloc reservation? */
	if (ext4_es_scan_clu(inode, &ext4_es_is_delayed, lblk))
		return 1;

	/* Already been allocated? */
	if (ext4_es_scan_clu(inode, &ext4_es_is_mapped, lblk))
		return 2;
	ret = ext4_clu_mapped(inode, EXT4_B2C(sbi, lblk));
	if (ret < 0)
		return ret;
	if (ret > 0)
		return 2;

	return 0;
}

/*
 * ext4_insert_delayed_blocks - adds a multiple delayed blocks to the extents
 *                              status tree, incrementing the reserved
 *                              cluster/block count or making pending
 *                              reservations where needed
 *
 * @inode - file containing the newly added block
 * @lblk - start logical block to be added
 * @len - length of blocks to be added
 *
 * Returns 0 on success, negative error code on failure.
 */
static int ext4_insert_delayed_blocks(struct inode *inode, ext4_lblk_t lblk,
				      ext4_lblk_t len)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	int ret;
	bool lclu_allocated = false;
	bool end_allocated = false;
	ext4_lblk_t resv_clu;
	ext4_lblk_t end = lblk + len - 1;

	/*
	 * If the cluster containing lblk or end is shared with a delayed,
	 * written, or unwritten extent in a bigalloc file system, it's
	 * already been accounted for and does not need to be reserved.
	 * A pending reservation must be made for the cluster if it's
	 * shared with a written or unwritten extent and doesn't already
	 * have one.  Written and unwritten extents can be purged from the
	 * extents status tree if the system is under memory pressure, so
	 * it's necessary to examine the extent tree if a search of the
	 * extents status tree doesn't get a match.
	 */
	if (sbi->s_cluster_ratio == 1) {
		ret = ext4_da_reserve_space(inode, len);
		if (ret != 0)   /* ENOSPC */
			return ret;
	} else {   /* bigalloc */
		resv_clu = EXT4_B2C(sbi, end) - EXT4_B2C(sbi, lblk) + 1;

		ret = ext4_clu_alloc_state(inode, lblk);
		if (ret < 0)
			return ret;
		if (ret > 0) {
			resv_clu--;
			lclu_allocated = (ret == 2);
		}

		if (EXT4_B2C(sbi, lblk) != EXT4_B2C(sbi, end)) {
			ret = ext4_clu_alloc_state(inode, end);
			if (ret < 0)
				return ret;
			if (ret > 0) {
				resv_clu--;
				end_allocated = (ret == 2);
			}
		}

		if (resv_clu) {
			ret = ext4_da_reserve_space(inode, resv_clu);
			if (ret != 0)   /* ENOSPC */
				return ret;
		}
	}

	ext4_es_insert_delayed_extent(inode, lblk, len, lclu_allocated,
				      end_allocated);
	return 0;
}

/*
 * Looks up the requested blocks and sets the delalloc extent map.
 * First try to look up for the extent entry that contains the requested
 * blocks in the extent status tree without i_data_sem, then try to look
 * up for the ondisk extent mapping with i_data_sem in read mode,
 * finally hold i_data_sem in write mode, looks up again and add a
 * delalloc extent entry if it still couldn't find any extent. Pass out
 * the mapped extent through @map and return 0 on success.
 */
static int ext4_da_map_blocks(struct inode *inode, struct ext4_map_blocks *map)
{
	struct extent_status es;
	int retval;
#ifdef ES_AGGRESSIVE_TEST
	struct ext4_map_blocks orig_map;

	memcpy(&orig_map, map, sizeof(*map));
#endif

	map->m_flags = 0;
	ext_debug(inode, "max_blocks %u, logical block %lu\n", map->m_len,
		  (unsigned long) map->m_lblk);

	ext4_check_map_extents_env(inode);

	/* Lookup extent status tree firstly */
	if (ext4_es_lookup_extent(inode, map->m_lblk, NULL, &es)) {
		map->m_len = min_t(unsigned int, map->m_len,
				   es.es_len - (map->m_lblk - es.es_lblk));

		if (ext4_es_is_hole(&es))
			goto add_delayed;

found:
		/*
		 * Delayed extent could be allocated by fallocate.
		 * So we need to check it.
		 */
		if (ext4_es_is_delayed(&es)) {
			map->m_flags |= EXT4_MAP_DELAYED;
			return 0;
		}

		map->m_pblk = ext4_es_pblock(&es) + map->m_lblk - es.es_lblk;
		if (ext4_es_is_written(&es))
			map->m_flags |= EXT4_MAP_MAPPED;
		else if (ext4_es_is_unwritten(&es))
			map->m_flags |= EXT4_MAP_UNWRITTEN;
		else
			BUG();

#ifdef ES_AGGRESSIVE_TEST
		ext4_map_blocks_es_recheck(NULL, inode, map, &orig_map, 0);
#endif
		return 0;
	}

	/*
	 * Try to see if we can get the block without requesting a new
	 * file system block.
	 */
	down_read(&EXT4_I(inode)->i_data_sem);
	if (ext4_has_inline_data(inode))
		retval = 0;
	else
		retval = ext4_map_query_blocks(NULL, inode, map, 0);
	up_read(&EXT4_I(inode)->i_data_sem);
	if (retval)
		return retval < 0 ? retval : 0;

add_delayed:
	down_write(&EXT4_I(inode)->i_data_sem);
	/*
	 * Page fault path (ext4_page_mkwrite does not take i_rwsem)
	 * and fallocate path (no folio lock) can race. Make sure we
	 * lookup the extent status tree here again while i_data_sem
	 * is held in write mode, before inserting a new da entry in
	 * the extent status tree.
	 */
	if (ext4_es_lookup_extent(inode, map->m_lblk, NULL, &es)) {
		map->m_len = min_t(unsigned int, map->m_len,
				   es.es_len - (map->m_lblk - es.es_lblk));

		if (!ext4_es_is_hole(&es)) {
			up_write(&EXT4_I(inode)->i_data_sem);
			goto found;
		}
	} else if (!ext4_has_inline_data(inode)) {
		retval = ext4_map_query_blocks(NULL, inode, map, 0);
		if (retval) {
			up_write(&EXT4_I(inode)->i_data_sem);
			return retval < 0 ? retval : 0;
		}
	}

	map->m_flags |= EXT4_MAP_DELAYED;
	retval = ext4_insert_delayed_blocks(inode, map->m_lblk, map->m_len);
	up_write(&EXT4_I(inode)->i_data_sem);

	return retval;
}

/*
 * This is a special get_block_t callback which is used by
 * ext4_da_write_begin().  It will either return mapped block or
 * reserve space for a single block.
 *
 * For delayed buffer_head we have BH_Mapped, BH_New, BH_Delay set.
 * We also have b_blocknr = -1 and b_bdev initialized properly
 *
 * For unwritten buffer_head we have BH_Mapped, BH_New, BH_Unwritten set.
 * We also have b_blocknr = physicalblock mapping unwritten extent and b_bdev
 * initialized properly.
 */
int ext4_da_get_block_prep(struct inode *inode, sector_t iblock,
			   struct buffer_head *bh, int create)
{
	struct ext4_map_blocks map;
	sector_t invalid_block = ~((sector_t) 0xffff);
	int ret = 0;

	BUG_ON(create == 0);
	BUG_ON(bh->b_size != inode->i_sb->s_blocksize);

	if (invalid_block < ext4_blocks_count(EXT4_SB(inode->i_sb)->s_es))
		invalid_block = ~0;

	map.m_lblk = iblock;
	map.m_len = 1;

	/*
	 * first, we need to know whether the block is allocated already
	 * preallocated blocks are unmapped but should treated
	 * the same as allocated blocks.
	 */
	ret = ext4_da_map_blocks(inode, &map);
	if (ret < 0)
		return ret;

	if (map.m_flags & EXT4_MAP_DELAYED) {
		map_bh(bh, inode->i_sb, invalid_block);
		set_buffer_new(bh);
		set_buffer_delay(bh);
		return 0;
	}

	map_bh(bh, inode->i_sb, map.m_pblk);
	ext4_update_bh_state(bh, map.m_flags);

	if (buffer_unwritten(bh)) {
		/* A delayed write to unwritten bh should be marked
		 * new and mapped.  Mapped ensures that we don't do
		 * get_block multiple times when we write to the same
		 * offset and new ensures that we do proper zero out
		 * for partial write.
		 */
		set_buffer_new(bh);
		set_buffer_mapped(bh);
	}
	return 0;
}

static void mpage_folio_done(struct mpage_da_data *mpd, struct folio *folio)
{
	mpd->start_pos += folio_size(folio);
	mpd->wbc->nr_to_write -= folio_nr_pages(folio);
	folio_unlock(folio);
}

static int mpage_submit_folio(struct mpage_da_data *mpd, struct folio *folio)
{
	size_t len;
	loff_t size;
	int err;

	WARN_ON_ONCE(folio_pos(folio) != mpd->start_pos);
	folio_clear_dirty_for_io(folio);
	/*
	 * We have to be very careful here!  Nothing protects writeback path
	 * against i_size changes and the page can be writeably mapped into
	 * page tables. So an application can be growing i_size and writing
	 * data through mmap while writeback runs. folio_clear_dirty_for_io()
	 * write-protects our page in page tables and the page cannot get
	 * written to again until we release folio lock. So only after
	 * folio_clear_dirty_for_io() we are safe to sample i_size for
	 * ext4_bio_write_folio() to zero-out tail of the written page. We rely
	 * on the barrier provided by folio_test_clear_dirty() in
	 * folio_clear_dirty_for_io() to make sure i_size is really sampled only
	 * after page tables are updated.
	 */
	size = i_size_read(mpd->inode);
	len = folio_size(folio);
	if (folio_pos(folio) + len > size &&
	    !ext4_verity_in_progress(mpd->inode))
		len = size & (len - 1);
	err = ext4_bio_write_folio(&mpd->io_submit, folio, len);

	return err;
}

#define BH_FLAGS (BIT(BH_Unwritten) | BIT(BH_Delay))

/*
 * mballoc gives us at most this number of blocks...
 * XXX: That seems to be only a limitation of ext4_mb_normalize_request().
 * The rest of mballoc seems to handle chunks up to full group size.
 */
#define MAX_WRITEPAGES_EXTENT_LEN 2048

/*
 * mpage_add_bh_to_extent - try to add bh to extent of blocks to map
 *
 * @mpd - extent of blocks
 * @lblk - logical number of the block in the file
 * @bh - buffer head we want to add to the extent
 *
 * The function is used to collect contig. blocks in the same state. If the
 * buffer doesn't require mapping for writeback and we haven't started the
 * extent of buffers to map yet, the function returns 'true' immediately - the
 * caller can write the buffer right away. Otherwise the function returns true
 * if the block has been added to the extent, false if the block couldn't be
 * added.
 */
static bool mpage_add_bh_to_extent(struct mpage_da_data *mpd, ext4_lblk_t lblk,
				   struct buffer_head *bh)
{
	struct ext4_map_blocks *map = &mpd->map;

	/* Buffer that doesn't need mapping for writeback? */
	if (!buffer_dirty(bh) || !buffer_mapped(bh) ||
	    (!buffer_delay(bh) && !buffer_unwritten(bh))) {
		/* So far no extent to map => we write the buffer right away */
		if (map->m_len == 0)
			return true;
		return false;
	}

	/* First block in the extent? */
	if (map->m_len == 0) {
		/* We cannot map unless handle is started... */
		if (!mpd->do_map)
			return false;
		map->m_lblk = lblk;
		map->m_len = 1;
		map->m_flags = bh->b_state & BH_FLAGS;
		return true;
	}

	/* Don't go larger than mballoc is willing to allocate */
	if (map->m_len >= MAX_WRITEPAGES_EXTENT_LEN)
		return false;

	/* Can we merge the block to our big extent? */
	if (lblk == map->m_lblk + map->m_len &&
	    (bh->b_state & BH_FLAGS) == map->m_flags) {
		map->m_len++;
		return true;
	}
	return false;
}

/*
 * mpage_process_page_bufs - submit page buffers for IO or add them to extent
 *
 * @mpd - extent of blocks for mapping
 * @head - the first buffer in the page
 * @bh - buffer we should start processing from
 * @lblk - logical number of the block in the file corresponding to @bh
 *
 * Walk through page buffers from @bh upto @head (exclusive) and either submit
 * the page for IO if all buffers in this page were mapped and there's no
 * accumulated extent of buffers to map or add buffers in the page to the
 * extent of buffers to map. The function returns 1 if the caller can continue
 * by processing the next page, 0 if it should stop adding buffers to the
 * extent to map because we cannot extend it anymore. It can also return value
 * < 0 in case of error during IO submission.
 */
static int mpage_process_page_bufs(struct mpage_da_data *mpd,
				   struct buffer_head *head,
				   struct buffer_head *bh,
				   ext4_lblk_t lblk)
{
	struct inode *inode = mpd->inode;
	int err;
	ext4_lblk_t blocks = (i_size_read(inode) + i_blocksize(inode) - 1)
							>> inode->i_blkbits;

	if (ext4_verity_in_progress(inode))
		blocks = EXT_MAX_BLOCKS;

	do {
		BUG_ON(buffer_locked(bh));

		if (lblk >= blocks || !mpage_add_bh_to_extent(mpd, lblk, bh)) {
			/* Found extent to map? */
			if (mpd->map.m_len)
				return 0;
			/* Buffer needs mapping and handle is not started? */
			if (!mpd->do_map)
				return 0;
			/* Everything mapped so far and we hit EOF */
			break;
		}
	} while (lblk++, (bh = bh->b_this_page) != head);
	/* So far everything mapped? Submit the page for IO. */
	if (mpd->map.m_len == 0) {
		err = mpage_submit_folio(mpd, head->b_folio);
		if (err < 0)
			return err;
		mpage_folio_done(mpd, head->b_folio);
	}
	if (lblk >= blocks) {
		mpd->scanned_until_end = 1;
		return 0;
	}
	return 1;
}

/*
 * mpage_process_folio - update folio buffers corresponding to changed extent
 *			 and may submit fully mapped page for IO
 * @mpd: description of extent to map, on return next extent to map
 * @folio: Contains these buffers.
 * @m_lblk: logical block mapping.
 * @m_pblk: corresponding physical mapping.
 * @map_bh: determines on return whether this page requires any further
 *		  mapping or not.
 *
 * Scan given folio buffers corresponding to changed extent and update buffer
 * state according to new extent state.
 * We map delalloc buffers to their physical location, clear unwritten bits.
 * If the given folio is not fully mapped, we update @mpd to the next extent in
 * the given folio that needs mapping & return @map_bh as true.
 */
static int mpage_process_folio(struct mpage_da_data *mpd, struct folio *folio,
			      ext4_lblk_t *m_lblk, ext4_fsblk_t *m_pblk,
			      bool *map_bh)
{
	struct buffer_head *head, *bh;
	ext4_io_end_t *io_end = mpd->io_submit.io_end;
	ext4_lblk_t lblk = *m_lblk;
	ext4_fsblk_t pblock = *m_pblk;
	int err = 0;
	int blkbits = mpd->inode->i_blkbits;
	ssize_t io_end_size = 0;
	struct ext4_io_end_vec *io_end_vec = ext4_last_io_end_vec(io_end);

	bh = head = folio_buffers(folio);
	do {
		if (lblk < mpd->map.m_lblk)
			continue;
		if (lblk >= mpd->map.m_lblk + mpd->map.m_len) {
			/*
			 * Buffer after end of mapped extent.
			 * Find next buffer in the folio to map.
			 */
			mpd->map.m_len = 0;
			mpd->map.m_flags = 0;
			io_end_vec->size += io_end_size;

			err = mpage_process_page_bufs(mpd, head, bh, lblk);
			if (err > 0)
				err = 0;
			if (!err && mpd->map.m_len && mpd->map.m_lblk > lblk) {
				io_end_vec = ext4_alloc_io_end_vec(io_end);
				if (IS_ERR(io_end_vec)) {
					err = PTR_ERR(io_end_vec);
					goto out;
				}
				io_end_vec->offset = (loff_t)mpd->map.m_lblk << blkbits;
			}
			*map_bh = true;
			goto out;
		}
		if (buffer_delay(bh)) {
			clear_buffer_delay(bh);
			bh->b_blocknr = pblock++;
		}
		clear_buffer_unwritten(bh);
		io_end_size += (1 << blkbits);
	} while (lblk++, (bh = bh->b_this_page) != head);

	io_end_vec->size += io_end_size;
	*map_bh = false;
out:
	*m_lblk = lblk;
	*m_pblk = pblock;
	return err;
}

/*
 * mpage_map_buffers - update buffers corresponding to changed extent and
 *		       submit fully mapped pages for IO
 *
 * @mpd - description of extent to map, on return next extent to map
 *
 * Scan buffers corresponding to changed extent (we expect corresponding pages
 * to be already locked) and update buffer state according to new extent state.
 * We map delalloc buffers to their physical location, clear unwritten bits,
 * and mark buffers as uninit when we perform writes to unwritten extents
 * and do extent conversion after IO is finished. If the last page is not fully
 * mapped, we update @map to the next extent in the last page that needs
 * mapping. Otherwise we submit the page for IO.
 */
static int mpage_map_and_submit_buffers(struct mpage_da_data *mpd)
{
	struct folio_batch fbatch;
	unsigned nr, i;
	struct inode *inode = mpd->inode;
	int bpp_bits = PAGE_SHIFT - inode->i_blkbits;
	pgoff_t start, end;
	ext4_lblk_t lblk;
	ext4_fsblk_t pblock;
	int err;
	bool map_bh = false;

	start = mpd->map.m_lblk >> bpp_bits;
	end = (mpd->map.m_lblk + mpd->map.m_len - 1) >> bpp_bits;
	pblock = mpd->map.m_pblk;

	folio_batch_init(&fbatch);
	while (start <= end) {
		nr = filemap_get_folios(inode->i_mapping, &start, end, &fbatch);
		if (nr == 0)
			break;
		for (i = 0; i < nr; i++) {
			struct folio *folio = fbatch.folios[i];

			lblk = folio->index << bpp_bits;
			err = mpage_process_folio(mpd, folio, &lblk, &pblock,
						 &map_bh);
			/*
			 * If map_bh is true, means page may require further bh
			 * mapping, or maybe the page was submitted for IO.
			 * So we return to call further extent mapping.
			 */
			if (err < 0 || map_bh)
				goto out;
			/* Page fully mapped - let IO run! */
			err = mpage_submit_folio(mpd, folio);
			if (err < 0)
				goto out;
			mpage_folio_done(mpd, folio);
		}
		folio_batch_release(&fbatch);
	}
	/* Extent fully mapped and matches with page boundary. We are done. */
	mpd->map.m_len = 0;
	mpd->map.m_flags = 0;
	return 0;
out:
	folio_batch_release(&fbatch);
	return err;
}

static int mpage_map_one_extent(handle_t *handle, struct mpage_da_data *mpd)
{
	struct inode *inode = mpd->inode;
	struct ext4_map_blocks *map = &mpd->map;
	int get_blocks_flags;
	int err, dioread_nolock;

	/* Make sure transaction has enough credits for this extent */
	err = ext4_journal_ensure_extent_credits(handle, inode);
	if (err < 0)
		return err;

	trace_ext4_da_write_pages_extent(inode, map);
	/*
	 * Call ext4_map_blocks() to allocate any delayed allocation blocks, or
	 * to convert an unwritten extent to be initialized (in the case
	 * where we have written into one or more preallocated blocks).  It is
	 * possible that we're going to need more metadata blocks than
	 * previously reserved. However we must not fail because we're in
	 * writeback and there is nothing we can do about it so it might result
	 * in data loss.  So use reserved blocks to allocate metadata if
	 * possible. In addition, do not cache any unrelated extents, as it
	 * only holds the folio lock but does not hold the i_rwsem or
	 * invalidate_lock, which could corrupt the extent status tree.
	 */
	get_blocks_flags = EXT4_GET_BLOCKS_CREATE |
			   EXT4_GET_BLOCKS_METADATA_NOFAIL |
			   EXT4_GET_BLOCKS_IO_SUBMIT |
			   EXT4_EX_NOCACHE;

	dioread_nolock = ext4_should_dioread_nolock(inode);
	if (dioread_nolock)
		get_blocks_flags |= EXT4_GET_BLOCKS_IO_CREATE_EXT;

	err = ext4_map_blocks(handle, inode, map, get_blocks_flags);
	if (err < 0)
		return err;
	if (dioread_nolock && (map->m_flags & EXT4_MAP_UNWRITTEN)) {
		if (!mpd->io_submit.io_end->handle &&
		    ext4_handle_valid(handle)) {
			mpd->io_submit.io_end->handle = handle->h_rsv_handle;
			handle->h_rsv_handle = NULL;
		}
		ext4_set_io_unwritten_flag(mpd->io_submit.io_end);
	}

	BUG_ON(map->m_len == 0);
	return 0;
}

/*
 * This is used to submit mapped buffers in a single folio that is not fully
 * mapped for various reasons, such as insufficient space or journal credits.
 */
static int mpage_submit_partial_folio(struct mpage_da_data *mpd)
{
	struct inode *inode = mpd->inode;
	struct folio *folio;
	loff_t pos;
	int ret;

	folio = filemap_get_folio(inode->i_mapping,
				  mpd->start_pos >> PAGE_SHIFT);
	if (IS_ERR(folio))
		return PTR_ERR(folio);
	/*
	 * The mapped position should be within the current processing folio
	 * but must not be the folio start position.
	 */
	pos = ((loff_t)mpd->map.m_lblk) << inode->i_blkbits;
	if (WARN_ON_ONCE((folio_pos(folio) == pos) ||
			 !folio_contains(folio, pos >> PAGE_SHIFT)))
		return -EINVAL;

	ret = mpage_submit_folio(mpd, folio);
	if (ret)
		goto out;
	/*
	 * Update start_pos to prevent this folio from being released in
	 * mpage_release_unused_pages(), it will be reset to the aligned folio
	 * pos when this folio is written again in the next round. Additionally,
	 * do not update wbc->nr_to_write here, as it will be updated once the
	 * entire folio has finished processing.
	 */
	mpd->start_pos = pos;
out:
	folio_unlock(folio);
	folio_put(folio);
	return ret;
}

/*
 * mpage_map_and_submit_extent - map extent starting at mpd->lblk of length
 *				 mpd->len and submit pages underlying it for IO
 *
 * @handle - handle for journal operations
 * @mpd - extent to map
 * @give_up_on_write - we set this to true iff there is a fatal error and there
 *                     is no hope of writing the data. The caller should discard
 *                     dirty pages to avoid infinite loops.
 *
 * The function maps extent starting at mpd->lblk of length mpd->len. If it is
 * delayed, blocks are allocated, if it is unwritten, we may need to convert
 * them to initialized or split the described range from larger unwritten
 * extent. Note that we need not map all the described range since allocation
 * can return less blocks or the range is covered by more unwritten extents. We
 * cannot map more because we are limited by reserved transaction credits. On
 * the other hand we always make sure that the last touched page is fully
 * mapped so that it can be written out (and thus forward progress is
 * guaranteed). After mapping we submit all mapped pages for IO.
 */
static int mpage_map_and_submit_extent(handle_t *handle,
				       struct mpage_da_data *mpd,
				       bool *give_up_on_write)
{
	struct inode *inode = mpd->inode;
	struct ext4_map_blocks *map = &mpd->map;
	int err;
	loff_t disksize;
	int progress = 0;
	ext4_io_end_t *io_end = mpd->io_submit.io_end;
	struct ext4_io_end_vec *io_end_vec;

	io_end_vec = ext4_alloc_io_end_vec(io_end);
	if (IS_ERR(io_end_vec))
		return PTR_ERR(io_end_vec);
	io_end_vec->offset = ((loff_t)map->m_lblk) << inode->i_blkbits;
	do {
		err = mpage_map_one_extent(handle, mpd);
		if (err < 0) {
			struct super_block *sb = inode->i_sb;

			if (ext4_emergency_state(sb))
				goto invalidate_dirty_pages;
			/*
			 * Let the uper layers retry transient errors.
			 * In the case of ENOSPC, if ext4_count_free_blocks()
			 * is non-zero, a commit should free up blocks.
			 */
			if ((err == -ENOMEM) || (err == -EAGAIN) ||
			    (err == -ENOSPC && ext4_count_free_clusters(sb))) {
				/*
				 * We may have already allocated extents for
				 * some bhs inside the folio, issue the
				 * corresponding data to prevent stale data.
				 */
				if (progress) {
					if (mpage_submit_partial_folio(mpd))
						goto invalidate_dirty_pages;
					goto update_disksize;
				}
				return err;
			}
			ext4_msg(sb, KERN_CRIT,
				 "Delayed block allocation failed for "
				 "inode %lu at logical offset %llu with"
				 " max blocks %u with error %d",
				 inode->i_ino,
				 (unsigned long long)map->m_lblk,
				 (unsigned)map->m_len, -err);
			ext4_msg(sb, KERN_CRIT,
				 "This should not happen!! Data will "
				 "be lost\n");
			if (err == -ENOSPC)
				ext4_print_free_blocks(inode);
		invalidate_dirty_pages:
			*give_up_on_write = true;
			return err;
		}
		progress = 1;
		/*
		 * Update buffer state, submit mapped pages, and get us new
		 * extent to map
		 */
		err = mpage_map_and_submit_buffers(mpd);
		if (err < 0)
			goto update_disksize;
	} while (map->m_len);

update_disksize:
	/*
	 * Update on-disk size after IO is submitted.  Races with
	 * truncate are avoided by checking i_size under i_data_sem.
	 */
	disksize = mpd->start_pos;
	if (disksize > READ_ONCE(EXT4_I(inode)->i_disksize)) {
		int err2;
		loff_t i_size;

		down_write(&EXT4_I(inode)->i_data_sem);
		i_size = i_size_read(inode);
		if (disksize > i_size)
			disksize = i_size;
		if (disksize > EXT4_I(inode)->i_disksize)
			EXT4_I(inode)->i_disksize = disksize;
		up_write(&EXT4_I(inode)->i_data_sem);
		err2 = ext4_mark_inode_dirty(handle, inode);
		if (err2) {
			ext4_error_err(inode->i_sb, -err2,
				       "Failed to mark inode %lu dirty",
				       inode->i_ino);
		}
		if (!err)
			err = err2;
	}
	return err;
}

static int ext4_journal_folio_buffers(handle_t *handle, struct folio *folio,
				     size_t len)
{
	struct buffer_head *page_bufs = folio_buffers(folio);
	struct inode *inode = folio->mapping->host;
	int ret, err;

	ret = ext4_walk_page_buffers(handle, inode, page_bufs, 0, len,
				     NULL, do_journal_get_write_access);
	err = ext4_walk_page_buffers(handle, inode, page_bufs, 0, len,
				     NULL, write_end_fn);
	if (ret == 0)
		ret = err;
	err = ext4_jbd2_inode_add_write(handle, inode, folio_pos(folio), len);
	if (ret == 0)
		ret = err;
	EXT4_I(inode)->i_datasync_tid = handle->h_transaction->t_tid;

	return ret;
}

static int mpage_journal_page_buffers(handle_t *handle,
				      struct mpage_da_data *mpd,
				      struct folio *folio)
{
	struct inode *inode = mpd->inode;
	loff_t size = i_size_read(inode);
	size_t len = folio_size(folio);

	folio_clear_checked(folio);
	mpd->wbc->nr_to_write -= folio_nr_pages(folio);

	if (folio_pos(folio) + len > size &&
	    !ext4_verity_in_progress(inode))
		len = size & (len - 1);

	return ext4_journal_folio_buffers(handle, folio, len);
}

/*
 * mpage_prepare_extent_to_map - find & lock contiguous range of dirty pages
 * 				 needing mapping, submit mapped pages
 *
 * @mpd - where to look for pages
 *
 * Walk dirty pages in the mapping. If they are fully mapped, submit them for
 * IO immediately. If we cannot map blocks, we submit just already mapped
 * buffers in the page for IO and keep page dirty. When we can map blocks and
 * we find a page which isn't mapped we start accumulating extent of buffers
 * underlying these pages that needs mapping (formed by either delayed or
 * unwritten buffers). We also lock the pages containing these buffers. The
 * extent found is returned in @mpd structure (starting at mpd->lblk with
 * length mpd->len blocks).
 *
 * Note that this function can attach bios to one io_end structure which are
 * neither logically nor physically contiguous. Although it may seem as an
 * unnecessary complication, it is actually inevitable in blocksize < pagesize
 * case as we need to track IO to all buffers underlying a page in one io_end.
 */
static int mpage_prepare_extent_to_map(struct mpage_da_data *mpd)
{
	struct address_space *mapping = mpd->inode->i_mapping;
	struct folio_batch fbatch;
	unsigned int nr_folios;
	pgoff_t index = mpd->start_pos >> PAGE_SHIFT;
	pgoff_t end = mpd->end_pos >> PAGE_SHIFT;
	xa_mark_t tag;
	int i, err = 0;
	int blkbits = mpd->inode->i_blkbits;
	ext4_lblk_t lblk;
	struct buffer_head *head;
	handle_t *handle = NULL;
	int bpp = ext4_journal_blocks_per_folio(mpd->inode);

	if (mpd->wbc->sync_mode == WB_SYNC_ALL || mpd->wbc->tagged_writepages)
		tag = PAGECACHE_TAG_TOWRITE;
	else
		tag = PAGECACHE_TAG_DIRTY;

	mpd->map.m_len = 0;
	mpd->next_pos = mpd->start_pos;
	if (ext4_should_journal_data(mpd->inode)) {
		handle = ext4_journal_start(mpd->inode, EXT4_HT_WRITE_PAGE,
					    bpp);
		if (IS_ERR(handle))
			return PTR_ERR(handle);
	}
	folio_batch_init(&fbatch);
	while (index <= end) {
		nr_folios = filemap_get_folios_tag(mapping, &index, end,
				tag, &fbatch);
		if (nr_folios == 0)
			break;

		for (i = 0; i < nr_folios; i++) {
			struct folio *folio = fbatch.folios[i];

			/*
			 * Accumulated enough dirty pages? This doesn't apply
			 * to WB_SYNC_ALL mode. For integrity sync we have to
			 * keep going because someone may be concurrently
			 * dirtying pages, and we might have synced a lot of
			 * newly appeared dirty pages, but have not synced all
			 * of the old dirty pages.
			 */
			if (mpd->wbc->sync_mode == WB_SYNC_NONE &&
			    mpd->wbc->nr_to_write <=
			    mpd->map.m_len >> (PAGE_SHIFT - blkbits))
				goto out;

			/* If we can't merge this page, we are done. */
			if (mpd->map.m_len > 0 &&
			    mpd->next_pos != folio_pos(folio))
				goto out;

			if (handle) {
				err = ext4_journal_ensure_credits(handle, bpp,
								  0);
				if (err < 0)
					goto out;
			}

			folio_lock(folio);
			/*
			 * If the page is no longer dirty, or its mapping no
			 * longer corresponds to inode we are writing (which
			 * means it has been truncated or invalidated), or the
			 * page is already under writeback and we are not doing
			 * a data integrity writeback, skip the page
			 */
			if (!folio_test_dirty(folio) ||
			    (folio_test_writeback(folio) &&
			     (mpd->wbc->sync_mode == WB_SYNC_NONE)) ||
			    unlikely(folio->mapping != mapping)) {
				folio_unlock(folio);
				continue;
			}

			folio_wait_writeback(folio);
			BUG_ON(folio_test_writeback(folio));

			/*
			 * Should never happen but for buggy code in
			 * other subsystems that call
			 * set_page_dirty() without properly warning
			 * the file system first.  See [1] for more
			 * information.
			 *
			 * [1] https://lore.kernel.org/linux-mm/20180103100430.GE4911@quack2.suse.cz
			 */
			if (!folio_buffers(folio)) {
				ext4_warning_inode(mpd->inode, "page %lu does not have buffers attached", folio->index);
				folio_clear_dirty(folio);
				folio_unlock(folio);
				continue;
			}

			if (mpd->map.m_len == 0)
				mpd->start_pos = folio_pos(folio);
			mpd->next_pos = folio_pos(folio) + folio_size(folio);
			/*
			 * Writeout when we cannot modify metadata is simple.
			 * Just submit the page. For data=journal mode we
			 * first handle writeout of the page for checkpoint and
			 * only after that handle delayed page dirtying. This
			 * makes sure current data is checkpointed to the final
			 * location before possibly journalling it again which
			 * is desirable when the page is frequently dirtied
			 * through a pin.
			 */
			if (!mpd->can_map) {
				err = mpage_submit_folio(mpd, folio);
				if (err < 0)
					goto out;
				/* Pending dirtying of journalled data? */
				if (folio_test_checked(folio)) {
					err = mpage_journal_page_buffers(handle,
						mpd, folio);
					if (err < 0)
						goto out;
					mpd->journalled_more_data = 1;
				}
				mpage_folio_done(mpd, folio);
			} else {
				/* Add all dirty buffers to mpd */
				lblk = ((ext4_lblk_t)folio->index) <<
					(PAGE_SHIFT - blkbits);
				head = folio_buffers(folio);
				err = mpage_process_page_bufs(mpd, head, head,
						lblk);
				if (err <= 0)
					goto out;
				err = 0;
			}
		}
		folio_batch_release(&fbatch);
		cond_resched();
	}
	mpd->scanned_until_end = 1;
	if (handle)
		ext4_journal_stop(handle);
	return 0;
out:
	folio_batch_release(&fbatch);
	if (handle)
		ext4_journal_stop(handle);
	return err;
}

static int ext4_do_writepages(struct mpage_da_data *mpd)
{
	struct writeback_control *wbc = mpd->wbc;
	pgoff_t	writeback_index = 0;
	long nr_to_write = wbc->nr_to_write;
	int range_whole = 0;
	int cycled = 1;
	handle_t *handle = NULL;
	struct inode *inode = mpd->inode;
	struct address_space *mapping = inode->i_mapping;
	int needed_blocks, rsv_blocks = 0, ret = 0;
	struct ext4_sb_info *sbi = EXT4_SB(mapping->host->i_sb);
	struct blk_plug plug;
	bool give_up_on_write = false;

	trace_ext4_writepages(inode, wbc);

	/*
	 * No pages to write? This is mainly a kludge to avoid starting
	 * a transaction for special inodes like journal inode on last iput()
	 * because that could violate lock ordering on umount
	 */
	if (!mapping->nrpages || !mapping_tagged(mapping, PAGECACHE_TAG_DIRTY))
		goto out_writepages;

	/*
	 * If the filesystem has aborted, it is read-only, so return
	 * right away instead of dumping stack traces later on that
	 * will obscure the real source of the problem.  We test
	 * fs shutdown state instead of sb->s_flag's SB_RDONLY because
	 * the latter could be true if the filesystem is mounted
	 * read-only, and in that case, ext4_writepages should
	 * *never* be called, so if that ever happens, we would want
	 * the stack trace.
	 */
	ret = ext4_emergency_state(mapping->host->i_sb);
	if (unlikely(ret))
		goto out_writepages;

	/*
	 * If we have inline data and arrive here, it means that
	 * we will soon create the block for the 1st page, so
	 * we'd better clear the inline data here.
	 */
	if (ext4_has_inline_data(inode)) {
		/* Just inode will be modified... */
		handle = ext4_journal_start(inode, EXT4_HT_INODE, 1);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			goto out_writepages;
		}
		BUG_ON(ext4_test_inode_state(inode,
				EXT4_STATE_MAY_INLINE_DATA));
		ext4_destroy_inline_data(handle, inode);
		ext4_journal_stop(handle);
	}

	/*
	 * data=journal mode does not do delalloc so we just need to writeout /
	 * journal already mapped buffers. On the other hand we need to commit
	 * transaction to make data stable. We expect all the data to be
	 * already in the journal (the only exception are DMA pinned pages
	 * dirtied behind our back) so we commit transaction here and run the
	 * writeback loop to checkpoint them. The checkpointing is not actually
	 * necessary to make data persistent *but* quite a few places (extent
	 * shifting operations, fsverity, ...) depend on being able to drop
	 * pagecache pages after calling filemap_write_and_wait() and for that
	 * checkpointing needs to happen.
	 */
	if (ext4_should_journal_data(inode)) {
		mpd->can_map = 0;
		if (wbc->sync_mode == WB_SYNC_ALL)
			ext4_fc_commit(sbi->s_journal,
				       EXT4_I(inode)->i_datasync_tid);
	}
	mpd->journalled_more_data = 0;

	if (ext4_should_dioread_nolock(inode)) {
		int bpf = ext4_journal_blocks_per_folio(inode);
		/*
		 * We may need to convert up to one extent per block in
		 * the folio and we may dirty the inode.
		 */
		rsv_blocks = 1 + ext4_ext_index_trans_blocks(inode, bpf);
	}

	if (wbc->range_start == 0 && wbc->range_end == LLONG_MAX)
		range_whole = 1;

	if (wbc->range_cyclic) {
		writeback_index = mapping->writeback_index;
		if (writeback_index)
			cycled = 0;
		mpd->start_pos = writeback_index << PAGE_SHIFT;
		mpd->end_pos = LLONG_MAX;
	} else {
		mpd->start_pos = wbc->range_start;
		mpd->end_pos = wbc->range_end;
	}

	ext4_io_submit_init(&mpd->io_submit, wbc);
retry:
	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
		tag_pages_for_writeback(mapping, mpd->start_pos >> PAGE_SHIFT,
					mpd->end_pos >> PAGE_SHIFT);
	blk_start_plug(&plug);

	/*
	 * First writeback pages that don't need mapping - we can avoid
	 * starting a transaction unnecessarily and also avoid being blocked
	 * in the block layer on device congestion while having transaction
	 * started.
	 */
	mpd->do_map = 0;
	mpd->scanned_until_end = 0;
	mpd->io_submit.io_end = ext4_init_io_end(inode, GFP_KERNEL);
	if (!mpd->io_submit.io_end) {
		ret = -ENOMEM;
		goto unplug;
	}
	ret = mpage_prepare_extent_to_map(mpd);
	/* Unlock pages we didn't use */
	mpage_release_unused_pages(mpd, false);
	/* Submit prepared bio */
	ext4_io_submit(&mpd->io_submit);
	ext4_put_io_end_defer(mpd->io_submit.io_end);
	mpd->io_submit.io_end = NULL;
	if (ret < 0)
		goto unplug;

	while (!mpd->scanned_until_end && wbc->nr_to_write > 0) {
		/* For each extent of pages we use new io_end */
		mpd->io_submit.io_end = ext4_init_io_end(inode, GFP_KERNEL);
		if (!mpd->io_submit.io_end) {
			ret = -ENOMEM;
			break;
		}

		WARN_ON_ONCE(!mpd->can_map);
		/*
		 * We have two constraints: We find one extent to map and we
		 * must always write out whole page (makes a difference when
		 * blocksize < pagesize) so that we don't block on IO when we
		 * try to write out the rest of the page. Journalled mode is
		 * not supported by delalloc.
		 */
		BUG_ON(ext4_should_journal_data(inode));
		/*
		 * Calculate the number of credits needed to reserve for one
		 * extent of up to MAX_WRITEPAGES_EXTENT_LEN blocks. It will
		 * attempt to extend the transaction or start a new iteration
		 * if the reserved credits are insufficient.
		 */
		needed_blocks = ext4_chunk_trans_blocks(inode,
						MAX_WRITEPAGES_EXTENT_LEN);
		/* start a new transaction */
		handle = ext4_journal_start_with_reserve(inode,
				EXT4_HT_WRITE_PAGE, needed_blocks, rsv_blocks);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			ext4_msg(inode->i_sb, KERN_CRIT, "%s: jbd2_start: "
			       "%ld pages, ino %lu; err %d", __func__,
				wbc->nr_to_write, inode->i_ino, ret);
			/* Release allocated io_end */
			ext4_put_io_end(mpd->io_submit.io_end);
			mpd->io_submit.io_end = NULL;
			break;
		}
		mpd->do_map = 1;

		trace_ext4_da_write_folios_start(inode, mpd->start_pos,
				mpd->next_pos, wbc);
		ret = mpage_prepare_extent_to_map(mpd);
		if (!ret && mpd->map.m_len)
			ret = mpage_map_and_submit_extent(handle, mpd,
					&give_up_on_write);
		/*
		 * Caution: If the handle is synchronous,
		 * ext4_journal_stop() can wait for transaction commit
		 * to finish which may depend on writeback of pages to
		 * complete or on page lock to be released.  In that
		 * case, we have to wait until after we have
		 * submitted all the IO, released page locks we hold,
		 * and dropped io_end reference (for extent conversion
		 * to be able to complete) before stopping the handle.
		 */
		if (!ext4_handle_valid(handle) || handle->h_sync == 0) {
			ext4_journal_stop(handle);
			handle = NULL;
			mpd->do_map = 0;
		}
		/* Unlock pages we didn't use */
		mpage_release_unused_pages(mpd, give_up_on_write);
		/* Submit prepared bio */
		ext4_io_submit(&mpd->io_submit);

		/*
		 * Drop our io_end reference we got from init. We have
		 * to be careful and use deferred io_end finishing if
		 * we are still holding the transaction as we can
		 * release the last reference to io_end which may end
		 * up doing unwritten extent conversion.
		 */
		if (handle) {
			ext4_put_io_end_defer(mpd->io_submit.io_end);
			ext4_journal_stop(handle);
		} else
			ext4_put_io_end(mpd->io_submit.io_end);
		mpd->io_submit.io_end = NULL;
		trace_ext4_da_write_folios_end(inode, mpd->start_pos,
				mpd->next_pos, wbc, ret);

		if (ret == -ENOSPC && sbi->s_journal) {
			/*
			 * Commit the transaction which would
			 * free blocks released in the transaction
			 * and try again
			 */
			jbd2_journal_force_commit_nested(sbi->s_journal);
			ret = 0;
			continue;
		}
		if (ret == -EAGAIN)
			ret = 0;
		/* Fatal error - ENOMEM, EIO... */
		if (ret)
			break;
	}
unplug:
	blk_finish_plug(&plug);
	if (!ret && !cycled && wbc->nr_to_write > 0) {
		cycled = 1;
		mpd->end_pos = (writeback_index << PAGE_SHIFT) - 1;
		mpd->start_pos = 0;
		goto retry;
	}

	/* Update index */
	if (wbc->range_cyclic || (range_whole && wbc->nr_to_write > 0))
		/*
		 * Set the writeback_index so that range_cyclic
		 * mode will write it back later
		 */
		mapping->writeback_index = mpd->start_pos >> PAGE_SHIFT;

out_writepages:
	trace_ext4_writepages_result(inode, wbc, ret,
				     nr_to_write - wbc->nr_to_write);
	return ret;
}

static int ext4_writepages(struct address_space *mapping,
			   struct writeback_control *wbc)
{
	struct super_block *sb = mapping->host->i_sb;
	struct mpage_da_data mpd = {
		.inode = mapping->host,
		.wbc = wbc,
		.can_map = 1,
	};
	int ret;
	int alloc_ctx;

	ret = ext4_emergency_state(sb);
	if (unlikely(ret))
		return ret;

	alloc_ctx = ext4_writepages_down_read(sb);
	ret = ext4_do_writepages(&mpd);
	/*
	 * For data=journal writeback we could have come across pages marked
	 * for delayed dirtying (PageChecked) which were just added to the
	 * running transaction. Try once more to get them to stable storage.
	 */
	if (!ret && mpd.journalled_more_data)
		ret = ext4_do_writepages(&mpd);
	ext4_writepages_up_read(sb, alloc_ctx);

	return ret;
}

int ext4_normal_submit_inode_data_buffers(struct jbd2_inode *jinode)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = LONG_MAX,
		.range_start = jinode->i_dirty_start,
		.range_end = jinode->i_dirty_end,
	};
	struct mpage_da_data mpd = {
		.inode = jinode->i_vfs_inode,
		.wbc = &wbc,
		.can_map = 0,
	};
	return ext4_do_writepages(&mpd);
}

static int ext4_dax_writepages(struct address_space *mapping,
			       struct writeback_control *wbc)
{
	int ret;
	long nr_to_write = wbc->nr_to_write;
	struct inode *inode = mapping->host;
	int alloc_ctx;

	ret = ext4_emergency_state(inode->i_sb);
	if (unlikely(ret))
		return ret;

	alloc_ctx = ext4_writepages_down_read(inode->i_sb);
	trace_ext4_writepages(inode, wbc);

	ret = dax_writeback_mapping_range(mapping,
					  EXT4_SB(inode->i_sb)->s_daxdev, wbc);
	trace_ext4_writepages_result(inode, wbc, ret,
				     nr_to_write - wbc->nr_to_write);
	ext4_writepages_up_read(inode->i_sb, alloc_ctx);
	return ret;
}

static int ext4_nonda_switch(struct super_block *sb)
{
	s64 free_clusters, dirty_clusters;
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	/*
	 * switch to non delalloc mode if we are running low
	 * on free block. The free block accounting via percpu
	 * counters can get slightly wrong with percpu_counter_batch getting
	 * accumulated on each CPU without updating global counters
	 * Delalloc need an accurate free block accounting. So switch
	 * to non delalloc when we are near to error range.
	 */
	free_clusters =
		percpu_counter_read_positive(&sbi->s_freeclusters_counter);
	dirty_clusters =
		percpu_counter_read_positive(&sbi->s_dirtyclusters_counter);
	/*
	 * Start pushing delalloc when 1/2 of free blocks are dirty.
	 */
	if (dirty_clusters && (free_clusters < 2 * dirty_clusters))
		try_to_writeback_inodes_sb(sb, WB_REASON_FS_FREE_SPACE);

	if (2 * free_clusters < 3 * dirty_clusters ||
	    free_clusters < (dirty_clusters + EXT4_FREECLUSTERS_WATERMARK)) {
		/*
		 * free block count is less than 150% of dirty blocks
		 * or free blocks is less than watermark
		 */
		return 1;
	}
	return 0;
}

static int ext4_da_write_begin(const struct kiocb *iocb,
			       struct address_space *mapping,
			       loff_t pos, unsigned len,
			       struct folio **foliop, void **fsdata)
{
	int ret, retries = 0;
	struct folio *folio;
	pgoff_t index;
	struct inode *inode = mapping->host;

	ret = ext4_emergency_state(inode->i_sb);
	if (unlikely(ret))
		return ret;

	index = pos >> PAGE_SHIFT;

	if (ext4_nonda_switch(inode->i_sb) || ext4_verity_in_progress(inode)) {
		*fsdata = (void *)FALL_BACK_TO_NONDELALLOC;
		return ext4_write_begin(iocb, mapping, pos,
					len, foliop, fsdata);
	}
	*fsdata = (void *)0;
	trace_ext4_da_write_begin(inode, pos, len);

	if (ext4_test_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA)) {
		ret = ext4_generic_write_inline_data(mapping, inode, pos, len,
						     foliop, fsdata, true);
		if (ret < 0)
			return ret;
		if (ret == 1)
			return 0;
	}

retry:
	folio = write_begin_get_folio(iocb, mapping, index, len);
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	if (pos + len > folio_pos(folio) + folio_size(folio))
		len = folio_pos(folio) + folio_size(folio) - pos;

	ret = ext4_block_write_begin(NULL, folio, pos, len,
				     ext4_da_get_block_prep);
	if (ret < 0) {
		folio_unlock(folio);
		folio_put(folio);
		/*
		 * block_write_begin may have instantiated a few blocks
		 * outside i_size.  Trim these off again. Don't need
		 * i_size_read because we hold inode lock.
		 */
		if (pos + len > inode->i_size)
			ext4_truncate_failed_write(inode);

		if (ret == -ENOSPC &&
		    ext4_should_retry_alloc(inode->i_sb, &retries))
			goto retry;
		return ret;
	}

	*foliop = folio;
	return ret;
}

/*
 * Check if we should update i_disksize
 * when write to the end of file but not require block allocation
 */
static int ext4_da_should_update_i_disksize(struct folio *folio,
					    unsigned long offset)
{
	struct buffer_head *bh;
	struct inode *inode = folio->mapping->host;
	unsigned int idx;
	int i;

	bh = folio_buffers(folio);
	idx = offset >> inode->i_blkbits;

	for (i = 0; i < idx; i++)
		bh = bh->b_this_page;

	if (!buffer_mapped(bh) || (buffer_delay(bh)) || buffer_unwritten(bh))
		return 0;
	return 1;
}

static int ext4_da_do_write_end(struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct folio *folio)
{
	struct inode *inode = mapping->host;
	loff_t old_size = inode->i_size;
	bool disksize_changed = false;
	loff_t new_i_size, zero_len = 0;
	handle_t *handle;

	if (unlikely(!folio_buffers(folio))) {
		folio_unlock(folio);
		folio_put(folio);
		return -EIO;
	}
	/*
	 * block_write_end() will mark the inode as dirty with I_DIRTY_PAGES
	 * flag, which all that's needed to trigger page writeback.
	 */
	copied = block_write_end(pos, len, copied, folio);
	new_i_size = pos + copied;

	/*
	 * It's important to update i_size while still holding folio lock,
	 * because folio writeout could otherwise come in and zero beyond
	 * i_size.
	 *
	 * Since we are holding inode lock, we are sure i_disksize <=
	 * i_size. We also know that if i_disksize < i_size, there are
	 * delalloc writes pending in the range up to i_size. If the end of
	 * the current write is <= i_size, there's no need to touch
	 * i_disksize since writeback will push i_disksize up to i_size
	 * eventually. If the end of the current write is > i_size and
	 * inside an allocated block which ext4_da_should_update_i_disksize()
	 * checked, we need to update i_disksize here as certain
	 * ext4_writepages() paths not allocating blocks and update i_disksize.
	 */
	if (new_i_size > inode->i_size) {
		unsigned long end;

		i_size_write(inode, new_i_size);
		end = offset_in_folio(folio, new_i_size - 1);
		if (copied && ext4_da_should_update_i_disksize(folio, end)) {
			ext4_update_i_disksize(inode, new_i_size);
			disksize_changed = true;
		}
	}

	folio_unlock(folio);
	folio_put(folio);

	if (pos > old_size) {
		pagecache_isize_extended(inode, old_size, pos);
		zero_len = pos - old_size;
	}

	if (!disksize_changed && !zero_len)
		return copied;

	handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (zero_len)
		ext4_zero_partial_blocks(handle, inode, old_size, zero_len);
	ext4_mark_inode_dirty(handle, inode);
	ext4_journal_stop(handle);

	return copied;
}

static int ext4_da_write_end(const struct kiocb *iocb,
			     struct address_space *mapping,
			     loff_t pos, unsigned len, unsigned copied,
			     struct folio *folio, void *fsdata)
{
	struct inode *inode = mapping->host;
	int write_mode = (int)(unsigned long)fsdata;

	if (write_mode == FALL_BACK_TO_NONDELALLOC)
		return ext4_write_end(iocb, mapping, pos,
				      len, copied, folio, fsdata);

	trace_ext4_da_write_end(inode, pos, len, copied);

	if (write_mode != CONVERT_INLINE_DATA &&
	    ext4_test_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA) &&
	    ext4_has_inline_data(inode))
		return ext4_write_inline_data_end(inode, pos, len, copied,
						  folio);

	if (unlikely(copied < len) && !folio_test_uptodate(folio))
		copied = 0;

	return ext4_da_do_write_end(mapping, pos, len, copied, folio);
}

/*
 * Force all delayed allocation blocks to be allocated for a given inode.
 */
int ext4_alloc_da_blocks(struct inode *inode)
{
	trace_ext4_alloc_da_blocks(inode);

	if (!EXT4_I(inode)->i_reserved_data_blocks)
		return 0;

	/*
	 * We do something simple for now.  The filemap_flush() will
	 * also start triggering a write of the data blocks, which is
	 * not strictly speaking necessary (and for users of
	 * laptop_mode, not even desirable).  However, to do otherwise
	 * would require replicating code paths in:
	 *
	 * ext4_writepages() ->
	 *    write_cache_pages() ---> (via passed in callback function)
	 *        __mpage_da_writepage() -->
	 *           mpage_add_bh_to_extent()
	 *           mpage_da_map_blocks()
	 *
	 * The problem is that write_cache_pages(), located in
	 * mm/page-writeback.c, marks pages clean in preparation for
	 * doing I/O, which is not desirable if we're not planning on
	 * doing I/O at all.
	 *
	 * We could call write_cache_pages(), and then redirty all of
	 * the pages by calling redirty_page_for_writepage() but that
	 * would be ugly in the extreme.  So instead we would need to
	 * replicate parts of the code in the above functions,
	 * simplifying them because we wouldn't actually intend to
	 * write out the pages, but rather only collect contiguous
	 * logical block extents, call the multi-block allocator, and
	 * then update the buffer heads with the block allocations.
	 *
	 * For now, though, we'll cheat by calling filemap_flush(),
	 * which will map the blocks, and start the I/O, but not
	 * actually wait for the I/O to complete.
	 */
	return filemap_flush(inode->i_mapping);
}

/*
 * bmap() is special.  It gets used by applications such as lilo and by
 * the swapper to find the on-disk block of a specific piece of data.
 *
 * Naturally, this is dangerous if the block concerned is still in the
 * journal.  If somebody makes a swapfile on an ext4 data-journaling
 * filesystem and enables swap, then they may get a nasty shock when the
 * data getting swapped to that swapfile suddenly gets overwritten by
 * the original zero's written out previously to the journal and
 * awaiting writeback in the kernel's buffer cache.
 *
 * So, if we see any bmap calls here on a modified, data-journaled file,
 * take extra steps to flush any blocks which might be in the cache.
 */
static sector_t ext4_bmap(struct address_space *mapping, sector_t block)
{
	struct inode *inode = mapping->host;
	sector_t ret = 0;

	inode_lock_shared(inode);
	/*
	 * We can get here for an inline file via the FIBMAP ioctl
	 */
	if (ext4_has_inline_data(inode))
		goto out;

	if (mapping_tagged(mapping, PAGECACHE_TAG_DIRTY) &&
	    (test_opt(inode->i_sb, DELALLOC) ||
	     ext4_should_journal_data(inode))) {
		/*
		 * With delalloc or journalled data we want to sync the file so
		 * that we can make sure we allocate blocks for file and data
		 * is in place for the user to see it
		 */
		filemap_write_and_wait(mapping);
	}

	ret = iomap_bmap(mapping, block, &ext4_iomap_ops);

out:
	inode_unlock_shared(inode);
	return ret;
}

static int ext4_read_folio(struct file *file, struct folio *folio)
{
	int ret = -EAGAIN;
	struct inode *inode = folio->mapping->host;

	trace_ext4_read_folio(inode, folio);

	if (ext4_has_inline_data(inode))
		ret = ext4_readpage_inline(inode, folio);

	if (ret == -EAGAIN)
		return ext4_mpage_readpages(inode, NULL, folio);

	return ret;
}

static void ext4_readahead(struct readahead_control *rac)
{
	struct inode *inode = rac->mapping->host;

	/* If the file has inline data, no need to do readahead. */
	if (ext4_has_inline_data(inode))
		return;

	ext4_mpage_readpages(inode, rac, NULL);
}

static void ext4_invalidate_folio(struct folio *folio, size_t offset,
				size_t length)
{
	trace_ext4_invalidate_folio(folio, offset, length);

	/* No journalling happens on data buffers when this function is used */
	WARN_ON(folio_buffers(folio) && buffer_jbd(folio_buffers(folio)));

	block_invalidate_folio(folio, offset, length);
}

static int __ext4_journalled_invalidate_folio(struct folio *folio,
					    size_t offset, size_t length)
{
	journal_t *journal = EXT4_JOURNAL(folio->mapping->host);

	trace_ext4_journalled_invalidate_folio(folio, offset, length);

	/*
	 * If it's a full truncate we just forget about the pending dirtying
	 */
	if (offset == 0 && length == folio_size(folio))
		folio_clear_checked(folio);

	return jbd2_journal_invalidate_folio(journal, folio, offset, length);
}

/* Wrapper for aops... */
static void ext4_journalled_invalidate_folio(struct folio *folio,
					   size_t offset,
					   size_t length)
{
	WARN_ON(__ext4_journalled_invalidate_folio(folio, offset, length) < 0);
}

static bool ext4_release_folio(struct folio *folio, gfp_t wait)
{
	struct inode *inode = folio->mapping->host;
	journal_t *journal = EXT4_JOURNAL(inode);

	trace_ext4_release_folio(inode, folio);

	/* Page has dirty journalled data -> cannot release */
	if (folio_test_checked(folio))
		return false;
	if (journal)
		return jbd2_journal_try_to_free_buffers(journal, folio);
	else
		return try_to_free_buffers(folio);
}

static bool ext4_inode_datasync_dirty(struct inode *inode)
{
	journal_t *journal = EXT4_SB(inode->i_sb)->s_journal;

	if (journal) {
		if (jbd2_transaction_committed(journal,
			EXT4_I(inode)->i_datasync_tid))
			return false;
		if (test_opt2(inode->i_sb, JOURNAL_FAST_COMMIT))
			return !list_empty(&EXT4_I(inode)->i_fc_list);
		return true;
	}

	/* Any metadata buffers to write? */
	if (!list_empty(&inode->i_mapping->i_private_list))
		return true;
	return inode->i_state & I_DIRTY_DATASYNC;
}

static void ext4_set_iomap(struct inode *inode, struct iomap *iomap,
			   struct ext4_map_blocks *map, loff_t offset,
			   loff_t length, unsigned int flags)
{
	u8 blkbits = inode->i_blkbits;

	/*
	 * Writes that span EOF might trigger an I/O size update on completion,
	 * so consider them to be dirty for the purpose of O_DSYNC, even if
	 * there is no other metadata changes being made or are pending.
	 */
	iomap->flags = 0;
	if (ext4_inode_datasync_dirty(inode) ||
	    offset + length > i_size_read(inode))
		iomap->flags |= IOMAP_F_DIRTY;

	if (map->m_flags & EXT4_MAP_NEW)
		iomap->flags |= IOMAP_F_NEW;

	/* HW-offload atomics are always used */
	if (flags & IOMAP_ATOMIC)
		iomap->flags |= IOMAP_F_ATOMIC_BIO;

	if (flags & IOMAP_DAX)
		iomap->dax_dev = EXT4_SB(inode->i_sb)->s_daxdev;
	else
		iomap->bdev = inode->i_sb->s_bdev;
	iomap->offset = (u64) map->m_lblk << blkbits;
	iomap->length = (u64) map->m_len << blkbits;

	if ((map->m_flags & EXT4_MAP_MAPPED) &&
	    !ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		iomap->flags |= IOMAP_F_MERGED;

	/*
	 * Flags passed to ext4_map_blocks() for direct I/O writes can result
	 * in m_flags having both EXT4_MAP_MAPPED and EXT4_MAP_UNWRITTEN bits
	 * set. In order for any allocated unwritten extents to be converted
	 * into written extents correctly within the ->end_io() handler, we
	 * need to ensure that the iomap->type is set appropriately. Hence, the
	 * reason why we need to check whether the EXT4_MAP_UNWRITTEN bit has
	 * been set first.
	 */
	if (map->m_flags & EXT4_MAP_UNWRITTEN) {
		iomap->type = IOMAP_UNWRITTEN;
		iomap->addr = (u64) map->m_pblk << blkbits;
		if (flags & IOMAP_DAX)
			iomap->addr += EXT4_SB(inode->i_sb)->s_dax_part_off;
	} else if (map->m_flags & EXT4_MAP_MAPPED) {
		iomap->type = IOMAP_MAPPED;
		iomap->addr = (u64) map->m_pblk << blkbits;
		if (flags & IOMAP_DAX)
			iomap->addr += EXT4_SB(inode->i_sb)->s_dax_part_off;
	} else if (map->m_flags & EXT4_MAP_DELAYED) {
		iomap->type = IOMAP_DELALLOC;
		iomap->addr = IOMAP_NULL_ADDR;
	} else {
		iomap->type = IOMAP_HOLE;
		iomap->addr = IOMAP_NULL_ADDR;
	}
}

static int ext4_map_blocks_atomic_write_slow(handle_t *handle,
			struct inode *inode, struct ext4_map_blocks *map)
{
	ext4_lblk_t m_lblk = map->m_lblk;
	unsigned int m_len = map->m_len;
	unsigned int mapped_len = 0, m_flags = 0;
	ext4_fsblk_t next_pblk;
	bool check_next_pblk = false;
	int ret = 0;

	WARN_ON_ONCE(!ext4_has_feature_bigalloc(inode->i_sb));

	/*
	 * This is a slow path in case of mixed mapping. We use
	 * EXT4_GET_BLOCKS_CREATE_ZERO flag here to make sure we get a single
	 * contiguous mapped mapping. This will ensure any unwritten or hole
	 * regions within the requested range is zeroed out and we return
	 * a single contiguous mapped extent.
	 */
	m_flags = EXT4_GET_BLOCKS_CREATE_ZERO;

	do {
		ret = ext4_map_blocks(handle, inode, map, m_flags);
		if (ret < 0 && ret != -ENOSPC)
			goto out_err;
		/*
		 * This should never happen, but let's return an error code to
		 * avoid an infinite loop in here.
		 */
		if (ret == 0) {
			ret = -EFSCORRUPTED;
			ext4_warning_inode(inode,
				"ext4_map_blocks() couldn't allocate blocks m_flags: 0x%x, ret:%d",
				m_flags, ret);
			goto out_err;
		}
		/*
		 * With bigalloc we should never get ENOSPC nor discontiguous
		 * physical extents.
		 */
		if ((check_next_pblk && next_pblk != map->m_pblk) ||
				ret == -ENOSPC) {
			ext4_warning_inode(inode,
				"Non-contiguous allocation detected: expected %llu, got %llu, "
				"or ext4_map_blocks() returned out of space ret: %d",
				next_pblk, map->m_pblk, ret);
			ret = -EFSCORRUPTED;
			goto out_err;
		}
		next_pblk = map->m_pblk + map->m_len;
		check_next_pblk = true;

		mapped_len += map->m_len;
		map->m_lblk += map->m_len;
		map->m_len = m_len - mapped_len;
	} while (mapped_len < m_len);

	/*
	 * We might have done some work in above loop, so we need to query the
	 * start of the physical extent, based on the origin m_lblk and m_len.
	 * Let's also ensure we were able to allocate the required range for
	 * mixed mapping case.
	 */
	map->m_lblk = m_lblk;
	map->m_len = m_len;
	map->m_flags = 0;

	ret = ext4_map_blocks(handle, inode, map,
			      EXT4_GET_BLOCKS_QUERY_LAST_IN_LEAF);
	if (ret != m_len) {
		ext4_warning_inode(inode,
			"allocation failed for atomic write request m_lblk:%u, m_len:%u, ret:%d\n",
			m_lblk, m_len, ret);
		ret = -EINVAL;
	}
	return ret;

out_err:
	/* reset map before returning an error */
	map->m_lblk = m_lblk;
	map->m_len = m_len;
	map->m_flags = 0;
	return ret;
}

/*
 * ext4_map_blocks_atomic: Helper routine to ensure the entire requested
 * range in @map [lblk, lblk + len) is one single contiguous extent with no
 * mixed mappings.
 *
 * We first use m_flags passed to us by our caller (ext4_iomap_alloc()).
 * We only call EXT4_GET_BLOCKS_ZERO in the slow path, when the underlying
 * physical extent for the requested range does not have a single contiguous
 * mapping type i.e. (Hole, Mapped, or Unwritten) throughout.
 * In that case we will loop over the requested range to allocate and zero out
 * the unwritten / holes in between, to get a single mapped extent from
 * [m_lblk, m_lblk +  m_len). Note that this is only possible because we know
 * this can be called only with bigalloc enabled filesystem where the underlying
 * cluster is already allocated. This avoids allocating discontiguous extents
 * in the slow path due to multiple calls to ext4_map_blocks().
 * The slow path is mostly non-performance critical path, so it should be ok to
 * loop using ext4_map_blocks() with appropriate flags to allocate & zero the
 * underlying short holes/unwritten extents within the requested range.
 */
static int ext4_map_blocks_atomic_write(handle_t *handle, struct inode *inode,
				struct ext4_map_blocks *map, int m_flags,
				bool *force_commit)
{
	ext4_lblk_t m_lblk = map->m_lblk;
	unsigned int m_len = map->m_len;
	int ret = 0;

	WARN_ON_ONCE(m_len > 1 && !ext4_has_feature_bigalloc(inode->i_sb));

	ret = ext4_map_blocks(handle, inode, map, m_flags);
	if (ret < 0 || ret == m_len)
		goto out;
	/*
	 * This is a mixed mapping case where we were not able to allocate
	 * a single contiguous extent. In that case let's reset requested
	 * mapping and call the slow path.
	 */
	map->m_lblk = m_lblk;
	map->m_len = m_len;
	map->m_flags = 0;

	/*
	 * slow path means we have mixed mapping, that means we will need
	 * to force txn commit.
	 */
	*force_commit = true;
	return ext4_map_blocks_atomic_write_slow(handle, inode, map);
out:
	return ret;
}

static int ext4_iomap_alloc(struct inode *inode, struct ext4_map_blocks *map,
			    unsigned int flags)
{
	handle_t *handle;
	u8 blkbits = inode->i_blkbits;
	int ret, dio_credits, m_flags = 0, retries = 0;
	bool force_commit = false;

	/*
	 * Trim the mapping request to the maximum value that we can map at
	 * once for direct I/O.
	 */
	if (map->m_len > DIO_MAX_BLOCKS)
		map->m_len = DIO_MAX_BLOCKS;

	/*
	 * journal credits estimation for atomic writes. We call
	 * ext4_map_blocks(), to find if there could be a mixed mapping. If yes,
	 * then let's assume the no. of pextents required can be m_len i.e.
	 * every alternate block can be unwritten and hole.
	 */
	if (flags & IOMAP_ATOMIC) {
		unsigned int orig_mlen = map->m_len;

		ret = ext4_map_blocks(NULL, inode, map, 0);
		if (ret < 0)
			return ret;
		if (map->m_len < orig_mlen) {
			map->m_len = orig_mlen;
			dio_credits = ext4_meta_trans_blocks(inode, orig_mlen,
							     map->m_len);
		} else {
			dio_credits = ext4_chunk_trans_blocks(inode,
							      map->m_len);
		}
	} else {
		dio_credits = ext4_chunk_trans_blocks(inode, map->m_len);
	}

retry:
	/*
	 * Either we allocate blocks and then don't get an unwritten extent, so
	 * in that case we have reserved enough credits. Or, the blocks are
	 * already allocated and unwritten. In that case, the extent conversion
	 * fits into the credits as well.
	 */
	handle = ext4_journal_start(inode, EXT4_HT_MAP_BLOCKS, dio_credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	/*
	 * DAX and direct I/O are the only two operations that are currently
	 * supported with IOMAP_WRITE.
	 */
	WARN_ON(!(flags & (IOMAP_DAX | IOMAP_DIRECT)));
	if (flags & IOMAP_DAX)
		m_flags = EXT4_GET_BLOCKS_CREATE_ZERO;
	/*
	 * We use i_size instead of i_disksize here because delalloc writeback
	 * can complete at any point during the I/O and subsequently push the
	 * i_disksize out to i_size. This could be beyond where direct I/O is
	 * happening and thus expose allocated blocks to direct I/O reads.
	 */
	else if (((loff_t)map->m_lblk << blkbits) >= i_size_read(inode))
		m_flags = EXT4_GET_BLOCKS_CREATE;
	else if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		m_flags = EXT4_GET_BLOCKS_IO_CREATE_EXT;

	if (flags & IOMAP_ATOMIC)
		ret = ext4_map_blocks_atomic_write(handle, inode, map, m_flags,
						   &force_commit);
	else
		ret = ext4_map_blocks(handle, inode, map, m_flags);

	/*
	 * We cannot fill holes in indirect tree based inodes as that could
	 * expose stale data in the case of a crash. Use the magic error code
	 * to fallback to buffered I/O.
	 */
	if (!m_flags && !ret)
		ret = -ENOTBLK;

	ext4_journal_stop(handle);
	if (ret == -ENOSPC && ext4_should_retry_alloc(inode->i_sb, &retries))
		goto retry;

	/*
	 * Force commit the current transaction if the allocation spans a mixed
	 * mapping range. This ensures any pending metadata updates (like
	 * unwritten to written extents conversion) in this range are in
	 * consistent state with the file data blocks, before performing the
	 * actual write I/O. If the commit fails, the whole I/O must be aborted
	 * to prevent any possible torn writes.
	 */
	if (ret > 0 && force_commit) {
		int ret2;

		ret2 = ext4_force_commit(inode->i_sb);
		if (ret2)
			return ret2;
	}

	return ret;
}


static int ext4_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		unsigned flags, struct iomap *iomap, struct iomap *srcmap)
{
	int ret;
	struct ext4_map_blocks map;
	u8 blkbits = inode->i_blkbits;
	unsigned int orig_mlen;

	if ((offset >> blkbits) > EXT4_MAX_LOGICAL_BLOCK)
		return -EINVAL;

	if (WARN_ON_ONCE(ext4_has_inline_data(inode)))
		return -ERANGE;

	/*
	 * Calculate the first and last logical blocks respectively.
	 */
	map.m_lblk = offset >> blkbits;
	map.m_len = min_t(loff_t, (offset + length - 1) >> blkbits,
			  EXT4_MAX_LOGICAL_BLOCK) - map.m_lblk + 1;
	orig_mlen = map.m_len;

	if (flags & IOMAP_WRITE) {
		/*
		 * We check here if the blocks are already allocated, then we
		 * don't need to start a journal txn and we can directly return
		 * the mapping information. This could boost performance
		 * especially in multi-threaded overwrite requests.
		 */
		if (offset + length <= i_size_read(inode)) {
			ret = ext4_map_blocks(NULL, inode, &map, 0);
			/*
			 * For atomic writes the entire requested length should
			 * be mapped.
			 */
			if (map.m_flags & EXT4_MAP_MAPPED) {
				if ((!(flags & IOMAP_ATOMIC) && ret > 0) ||
				   (flags & IOMAP_ATOMIC && ret >= orig_mlen))
					goto out;
			}
			map.m_len = orig_mlen;
		}
		ret = ext4_iomap_alloc(inode, &map, flags);
	} else {
		/*
		 * This can be called for overwrites path from
		 * ext4_iomap_overwrite_begin().
		 */
		ret = ext4_map_blocks(NULL, inode, &map, 0);
	}

	if (ret < 0)
		return ret;
out:
	/*
	 * When inline encryption is enabled, sometimes I/O to an encrypted file
	 * has to be broken up to guarantee DUN contiguity.  Handle this by
	 * limiting the length of the mapping returned.
	 */
	map.m_len = fscrypt_limit_io_blocks(inode, map.m_lblk, map.m_len);

	/*
	 * Before returning to iomap, let's ensure the allocated mapping
	 * covers the entire requested length for atomic writes.
	 */
	if (flags & IOMAP_ATOMIC) {
		if (map.m_len < (length >> blkbits)) {
			WARN_ON_ONCE(1);
			return -EINVAL;
		}
	}
	ext4_set_iomap(inode, iomap, &map, offset, length, flags);

	return 0;
}

static int ext4_iomap_overwrite_begin(struct inode *inode, loff_t offset,
		loff_t length, unsigned flags, struct iomap *iomap,
		struct iomap *srcmap)
{
	int ret;

	/*
	 * Even for writes we don't need to allocate blocks, so just pretend
	 * we are reading to save overhead of starting a transaction.
	 */
	flags &= ~IOMAP_WRITE;
	ret = ext4_iomap_begin(inode, offset, length, flags, iomap, srcmap);
	WARN_ON_ONCE(!ret && iomap->type != IOMAP_MAPPED);
	return ret;
}

static inline bool ext4_want_directio_fallback(unsigned flags, ssize_t written)
{
	/* must be a directio to fall back to buffered */
	if ((flags & (IOMAP_WRITE | IOMAP_DIRECT)) !=
		    (IOMAP_WRITE | IOMAP_DIRECT))
		return false;

	/* atomic writes are all-or-nothing */
	if (flags & IOMAP_ATOMIC)
		return false;

	/* can only try again if we wrote nothing */
	return written == 0;
}

static int ext4_iomap_end(struct inode *inode, loff_t offset, loff_t length,
			  ssize_t written, unsigned flags, struct iomap *iomap)
{
	/*
	 * Check to see whether an error occurred while writing out the data to
	 * the allocated blocks. If so, return the magic error code for
	 * non-atomic write so that we fallback to buffered I/O and attempt to
	 * complete the remainder of the I/O.
	 * For non-atomic writes, any blocks that may have been
	 * allocated in preparation for the direct I/O will be reused during
	 * buffered I/O. For atomic write, we never fallback to buffered-io.
	 */
	if (ext4_want_directio_fallback(flags, written))
		return -ENOTBLK;

	return 0;
}

const struct iomap_ops ext4_iomap_ops = {
	.iomap_begin		= ext4_iomap_begin,
	.iomap_end		= ext4_iomap_end,
};

const struct iomap_ops ext4_iomap_overwrite_ops = {
	.iomap_begin		= ext4_iomap_overwrite_begin,
	.iomap_end		= ext4_iomap_end,
};

static int ext4_iomap_begin_report(struct inode *inode, loff_t offset,
				   loff_t length, unsigned int flags,
				   struct iomap *iomap, struct iomap *srcmap)
{
	int ret;
	struct ext4_map_blocks map;
	u8 blkbits = inode->i_blkbits;

	if ((offset >> blkbits) > EXT4_MAX_LOGICAL_BLOCK)
		return -EINVAL;

	if (ext4_has_inline_data(inode)) {
		ret = ext4_inline_data_iomap(inode, iomap);
		if (ret != -EAGAIN) {
			if (ret == 0 && offset >= iomap->length)
				ret = -ENOENT;
			return ret;
		}
	}

	/*
	 * Calculate the first and last logical block respectively.
	 */
	map.m_lblk = offset >> blkbits;
	map.m_len = min_t(loff_t, (offset + length - 1) >> blkbits,
			  EXT4_MAX_LOGICAL_BLOCK) - map.m_lblk + 1;

	/*
	 * Fiemap callers may call for offset beyond s_bitmap_maxbytes.
	 * So handle it here itself instead of querying ext4_map_blocks().
	 * Since ext4_map_blocks() will warn about it and will return
	 * -EIO error.
	 */
	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))) {
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

		if (offset >= sbi->s_bitmap_maxbytes) {
			map.m_flags = 0;
			goto set_iomap;
		}
	}

	ret = ext4_map_blocks(NULL, inode, &map, 0);
	if (ret < 0)
		return ret;
set_iomap:
	ext4_set_iomap(inode, iomap, &map, offset, length, flags);

	return 0;
}

const struct iomap_ops ext4_iomap_report_ops = {
	.iomap_begin = ext4_iomap_begin_report,
};

/*
 * For data=journal mode, folio should be marked dirty only when it was
 * writeably mapped. When that happens, it was already attached to the
 * transaction and marked as jbddirty (we take care of this in
 * ext4_page_mkwrite()). On transaction commit, we writeprotect page mappings
 * so we should have nothing to do here, except for the case when someone
 * had the page pinned and dirtied the page through this pin (e.g. by doing
 * direct IO to it). In that case we'd need to attach buffers here to the
 * transaction but we cannot due to lock ordering.  We cannot just dirty the
 * folio and leave attached buffers clean, because the buffers' dirty state is
 * "definitive".  We cannot just set the buffers dirty or jbddirty because all
 * the journalling code will explode.  So what we do is to mark the folio
 * "pending dirty" and next time ext4_writepages() is called, attach buffers
 * to the transaction appropriately.
 */
static bool ext4_journalled_dirty_folio(struct address_space *mapping,
		struct folio *folio)
{
	WARN_ON_ONCE(!folio_buffers(folio));
	if (folio_maybe_dma_pinned(folio))
		folio_set_checked(folio);
	return filemap_dirty_folio(mapping, folio);
}

static bool ext4_dirty_folio(struct address_space *mapping, struct folio *folio)
{
	WARN_ON_ONCE(!folio_test_locked(folio) && !folio_test_dirty(folio));
	WARN_ON_ONCE(!folio_buffers(folio));
	return block_dirty_folio(mapping, folio);
}

static int ext4_iomap_swap_activate(struct swap_info_struct *sis,
				    struct file *file, sector_t *span)
{
	return iomap_swapfile_activate(sis, file, span,
				       &ext4_iomap_report_ops);
}

static const struct address_space_operations ext4_aops = {
	.read_folio		= ext4_read_folio,
	.readahead		= ext4_readahead,
	.writepages		= ext4_writepages,
	.write_begin		= ext4_write_begin,
	.write_end		= ext4_write_end,
	.dirty_folio		= ext4_dirty_folio,
	.bmap			= ext4_bmap,
	.invalidate_folio	= ext4_invalidate_folio,
	.release_folio		= ext4_release_folio,
	.migrate_folio		= buffer_migrate_folio,
	.is_partially_uptodate  = block_is_partially_uptodate,
	.error_remove_folio	= generic_error_remove_folio,
	.swap_activate		= ext4_iomap_swap_activate,
};

static const struct address_space_operations ext4_journalled_aops = {
	.read_folio		= ext4_read_folio,
	.readahead		= ext4_readahead,
	.writepages		= ext4_writepages,
	.write_begin		= ext4_write_begin,
	.write_end		= ext4_journalled_write_end,
	.dirty_folio		= ext4_journalled_dirty_folio,
	.bmap			= ext4_bmap,
	.invalidate_folio	= ext4_journalled_invalidate_folio,
	.release_folio		= ext4_release_folio,
	.migrate_folio		= buffer_migrate_folio_norefs,
	.is_partially_uptodate  = block_is_partially_uptodate,
	.error_remove_folio	= generic_error_remove_folio,
	.swap_activate		= ext4_iomap_swap_activate,
};

static const struct address_space_operations ext4_da_aops = {
	.read_folio		= ext4_read_folio,
	.readahead		= ext4_readahead,
	.writepages		= ext4_writepages,
	.write_begin		= ext4_da_write_begin,
	.write_end		= ext4_da_write_end,
	.dirty_folio		= ext4_dirty_folio,
	.bmap			= ext4_bmap,
	.invalidate_folio	= ext4_invalidate_folio,
	.release_folio		= ext4_release_folio,
	.migrate_folio		= buffer_migrate_folio,
	.is_partially_uptodate  = block_is_partially_uptodate,
	.error_remove_folio	= generic_error_remove_folio,
	.swap_activate		= ext4_iomap_swap_activate,
};

static const struct address_space_operations ext4_dax_aops = {
	.writepages		= ext4_dax_writepages,
	.dirty_folio		= noop_dirty_folio,
	.bmap			= ext4_bmap,
	.swap_activate		= ext4_iomap_swap_activate,
};

void ext4_set_aops(struct inode *inode)
{
	switch (ext4_inode_journal_mode(inode)) {
	case EXT4_INODE_ORDERED_DATA_MODE:
	case EXT4_INODE_WRITEBACK_DATA_MODE:
		break;
	case EXT4_INODE_JOURNAL_DATA_MODE:
		inode->i_mapping->a_ops = &ext4_journalled_aops;
		return;
	default:
		BUG();
	}
	if (IS_DAX(inode))
		inode->i_mapping->a_ops = &ext4_dax_aops;
	else if (test_opt(inode->i_sb, DELALLOC))
		inode->i_mapping->a_ops = &ext4_da_aops;
	else
		inode->i_mapping->a_ops = &ext4_aops;
}

/*
 * Here we can't skip an unwritten buffer even though it usually reads zero
 * because it might have data in pagecache (eg, if called from ext4_zero_range,
 * ext4_punch_hole, etc) which needs to be properly zeroed out. Otherwise a
 * racing writeback can come later and flush the stale pagecache to disk.
 */
static int __ext4_block_zero_page_range(handle_t *handle,
		struct address_space *mapping, loff_t from, loff_t length)
{
	unsigned int offset, blocksize, pos;
	ext4_lblk_t iblock;
	struct inode *inode = mapping->host;
	struct buffer_head *bh;
	struct folio *folio;
	int err = 0;

	folio = __filemap_get_folio(mapping, from >> PAGE_SHIFT,
				    FGP_LOCK | FGP_ACCESSED | FGP_CREAT,
				    mapping_gfp_constraint(mapping, ~__GFP_FS));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	blocksize = inode->i_sb->s_blocksize;

	iblock = folio->index << (PAGE_SHIFT - inode->i_sb->s_blocksize_bits);

	bh = folio_buffers(folio);
	if (!bh)
		bh = create_empty_buffers(folio, blocksize, 0);

	/* Find the buffer that contains "offset" */
	offset = offset_in_folio(folio, from);
	pos = blocksize;
	while (offset >= pos) {
		bh = bh->b_this_page;
		iblock++;
		pos += blocksize;
	}
	if (buffer_freed(bh)) {
		BUFFER_TRACE(bh, "freed: skip");
		goto unlock;
	}
	if (!buffer_mapped(bh)) {
		BUFFER_TRACE(bh, "unmapped");
		ext4_get_block(inode, iblock, bh, 0);
		/* unmapped? It's a hole - nothing to do */
		if (!buffer_mapped(bh)) {
			BUFFER_TRACE(bh, "still unmapped");
			goto unlock;
		}
	}

	/* Ok, it's mapped. Make sure it's up-to-date */
	if (folio_test_uptodate(folio))
		set_buffer_uptodate(bh);

	if (!buffer_uptodate(bh)) {
		err = ext4_read_bh_lock(bh, 0, true);
		if (err)
			goto unlock;
		if (fscrypt_inode_uses_fs_layer_crypto(inode)) {
			/* We expect the key to be set. */
			BUG_ON(!fscrypt_has_encryption_key(inode));
			err = fscrypt_decrypt_pagecache_blocks(folio,
							       blocksize,
							       bh_offset(bh));
			if (err) {
				clear_buffer_uptodate(bh);
				goto unlock;
			}
		}
	}
	if (ext4_should_journal_data(inode)) {
		BUFFER_TRACE(bh, "get write access");
		err = ext4_journal_get_write_access(handle, inode->i_sb, bh,
						    EXT4_JTR_NONE);
		if (err)
			goto unlock;
	}
	folio_zero_range(folio, offset, length);
	BUFFER_TRACE(bh, "zeroed end of block");

	if (ext4_should_journal_data(inode)) {
		err = ext4_dirty_journalled_data(handle, bh);
	} else {
		err = 0;
		mark_buffer_dirty(bh);
		if (ext4_should_order_data(inode))
			err = ext4_jbd2_inode_add_write(handle, inode, from,
					length);
	}

unlock:
	folio_unlock(folio);
	folio_put(folio);
	return err;
}

/*
 * ext4_block_zero_page_range() zeros out a mapping of length 'length'
 * starting from file offset 'from'.  The range to be zero'd must
 * be contained with in one block.  If the specified range exceeds
 * the end of the block it will be shortened to end of the block
 * that corresponds to 'from'
 */
static int ext4_block_zero_page_range(handle_t *handle,
		struct address_space *mapping, loff_t from, loff_t length)
{
	struct inode *inode = mapping->host;
	unsigned offset = from & (PAGE_SIZE-1);
	unsigned blocksize = inode->i_sb->s_blocksize;
	unsigned max = blocksize - (offset & (blocksize - 1));

	/*
	 * correct length if it does not fall between
	 * 'from' and the end of the block
	 */
	if (length > max || length < 0)
		length = max;

	if (IS_DAX(inode)) {
		return dax_zero_range(inode, from, length, NULL,
				      &ext4_iomap_ops);
	}
	return __ext4_block_zero_page_range(handle, mapping, from, length);
}

/*
 * ext4_block_truncate_page() zeroes out a mapping from file offset `from'
 * up to the end of the block which corresponds to `from'.
 * This required during truncate. We need to physically zero the tail end
 * of that block so it doesn't yield old data if the file is later grown.
 */
static int ext4_block_truncate_page(handle_t *handle,
		struct address_space *mapping, loff_t from)
{
	unsigned offset = from & (PAGE_SIZE-1);
	unsigned length;
	unsigned blocksize;
	struct inode *inode = mapping->host;

	/* If we are processing an encrypted inode during orphan list handling */
	if (IS_ENCRYPTED(inode) && !fscrypt_has_encryption_key(inode))
		return 0;

	blocksize = inode->i_sb->s_blocksize;
	length = blocksize - (offset & (blocksize - 1));

	return ext4_block_zero_page_range(handle, mapping, from, length);
}

int ext4_zero_partial_blocks(handle_t *handle, struct inode *inode,
			     loff_t lstart, loff_t length)
{
	struct super_block *sb = inode->i_sb;
	struct address_space *mapping = inode->i_mapping;
	unsigned partial_start, partial_end;
	ext4_fsblk_t start, end;
	loff_t byte_end = (lstart + length - 1);
	int err = 0;

	partial_start = lstart & (sb->s_blocksize - 1);
	partial_end = byte_end & (sb->s_blocksize - 1);

	start = lstart >> sb->s_blocksize_bits;
	end = byte_end >> sb->s_blocksize_bits;

	/* Handle partial zero within the single block */
	if (start == end &&
	    (partial_start || (partial_end != sb->s_blocksize - 1))) {
		err = ext4_block_zero_page_range(handle, mapping,
						 lstart, length);
		return err;
	}
	/* Handle partial zero out on the start of the range */
	if (partial_start) {
		err = ext4_block_zero_page_range(handle, mapping,
						 lstart, sb->s_blocksize);
		if (err)
			return err;
	}
	/* Handle partial zero out on the end of the range */
	if (partial_end != sb->s_blocksize - 1)
		err = ext4_block_zero_page_range(handle, mapping,
						 byte_end - partial_end,
						 partial_end + 1);
	return err;
}

int ext4_can_truncate(struct inode *inode)
{
	if (S_ISREG(inode->i_mode))
		return 1;
	if (S_ISDIR(inode->i_mode))
		return 1;
	if (S_ISLNK(inode->i_mode))
		return !ext4_inode_is_fast_symlink(inode);
	return 0;
}

/*
 * We have to make sure i_disksize gets properly updated before we truncate
 * page cache due to hole punching or zero range. Otherwise i_disksize update
 * can get lost as it may have been postponed to submission of writeback but
 * that will never happen after we truncate page cache.
 */
int ext4_update_disksize_before_punch(struct inode *inode, loff_t offset,
				      loff_t len)
{
	handle_t *handle;
	int ret;

	loff_t size = i_size_read(inode);

	WARN_ON(!inode_is_locked(inode));
	if (offset > size || offset + len < size)
		return 0;

	if (EXT4_I(inode)->i_disksize >= size)
		return 0;

	handle = ext4_journal_start(inode, EXT4_HT_MISC, 1);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	ext4_update_i_disksize(inode, size);
	ret = ext4_mark_inode_dirty(handle, inode);
	ext4_journal_stop(handle);

	return ret;
}

static inline void ext4_truncate_folio(struct inode *inode,
				       loff_t start, loff_t end)
{
	unsigned long blocksize = i_blocksize(inode);
	struct folio *folio;

	/* Nothing to be done if no complete block needs to be truncated. */
	if (round_up(start, blocksize) >= round_down(end, blocksize))
		return;

	folio = filemap_lock_folio(inode->i_mapping, start >> PAGE_SHIFT);
	if (IS_ERR(folio))
		return;

	if (folio_mkclean(folio))
		folio_mark_dirty(folio);
	folio_unlock(folio);
	folio_put(folio);
}

int ext4_truncate_page_cache_block_range(struct inode *inode,
					 loff_t start, loff_t end)
{
	unsigned long blocksize = i_blocksize(inode);
	int ret;

	/*
	 * For journalled data we need to write (and checkpoint) pages
	 * before discarding page cache to avoid inconsitent data on disk
	 * in case of crash before freeing or unwritten converting trans
	 * is committed.
	 */
	if (ext4_should_journal_data(inode)) {
		ret = filemap_write_and_wait_range(inode->i_mapping, start,
						   end - 1);
		if (ret)
			return ret;
		goto truncate_pagecache;
	}

	/*
	 * If the block size is less than the page size, the file's mapped
	 * blocks within one page could be freed or converted to unwritten.
	 * So it's necessary to remove writable userspace mappings, and then
	 * ext4_page_mkwrite() can be called during subsequent write access
	 * to these partial folios.
	 */
	if (!IS_ALIGNED(start | end, PAGE_SIZE) &&
	    blocksize < PAGE_SIZE && start < inode->i_size) {
		loff_t page_boundary = round_up(start, PAGE_SIZE);

		ext4_truncate_folio(inode, start, min(page_boundary, end));
		if (end > page_boundary)
			ext4_truncate_folio(inode,
					    round_down(end, PAGE_SIZE), end);
	}

truncate_pagecache:
	truncate_pagecache_range(inode, start, end - 1);
	return 0;
}

static void ext4_wait_dax_page(struct inode *inode)
{
	filemap_invalidate_unlock(inode->i_mapping);
	schedule();
	filemap_invalidate_lock(inode->i_mapping);
}

int ext4_break_layouts(struct inode *inode)
{
	if (WARN_ON_ONCE(!rwsem_is_locked(&inode->i_mapping->invalidate_lock)))
		return -EINVAL;

	return dax_break_layout_inode(inode, ext4_wait_dax_page);
}

/*
 * ext4_punch_hole: punches a hole in a file by releasing the blocks
 * associated with the given offset and length
 *
 * @inode:  File inode
 * @offset: The offset where the hole will begin
 * @len:    The length of the hole
 *
 * Returns: 0 on success or negative on failure
 */

int ext4_punch_hole(struct file *file, loff_t offset, loff_t length)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	ext4_lblk_t start_lblk, end_lblk;
	loff_t max_end = sb->s_maxbytes;
	loff_t end = offset + length;
	handle_t *handle;
	unsigned int credits;
	int ret;

	trace_ext4_punch_hole(inode, offset, length, 0);
	WARN_ON_ONCE(!inode_is_locked(inode));

	/*
	 * For indirect-block based inodes, make sure that the hole within
	 * one block before last range.
	 */
	if (!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		max_end = EXT4_SB(sb)->s_bitmap_maxbytes - sb->s_blocksize;

	/* No need to punch hole beyond i_size */
	if (offset >= inode->i_size || offset >= max_end)
		return 0;

	/*
	 * If the hole extends beyond i_size, set the hole to end after
	 * the page that contains i_size.
	 */
	if (end > inode->i_size)
		end = round_up(inode->i_size, PAGE_SIZE);
	if (end > max_end)
		end = max_end;
	length = end - offset;

	/*
	 * Attach jinode to inode for jbd2 if we do any zeroing of partial
	 * block.
	 */
	if (!IS_ALIGNED(offset | end, sb->s_blocksize)) {
		ret = ext4_inode_attach_jinode(inode);
		if (ret < 0)
			return ret;
	}


	ret = ext4_update_disksize_before_punch(inode, offset, length);
	if (ret)
		return ret;

	/* Now release the pages and zero block aligned part of pages*/
	ret = ext4_truncate_page_cache_block_range(inode, offset, end);
	if (ret)
		return ret;

	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		credits = ext4_chunk_trans_extent(inode, 2);
	else
		credits = ext4_blocks_for_truncate(inode);
	handle = ext4_journal_start(inode, EXT4_HT_TRUNCATE, credits);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		ext4_std_error(sb, ret);
		return ret;
	}

	ret = ext4_zero_partial_blocks(handle, inode, offset, length);
	if (ret)
		goto out_handle;

	/* If there are blocks to remove, do it */
	start_lblk = EXT4_B_TO_LBLK(inode, offset);
	end_lblk = end >> inode->i_blkbits;

	if (end_lblk > start_lblk) {
		ext4_lblk_t hole_len = end_lblk - start_lblk;

		ext4_fc_track_inode(handle, inode);
		ext4_check_map_extents_env(inode);
		down_write(&EXT4_I(inode)->i_data_sem);
		ext4_discard_preallocations(inode);

		ext4_es_remove_extent(inode, start_lblk, hole_len);

		if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
			ret = ext4_ext_remove_space(inode, start_lblk,
						    end_lblk - 1);
		else
			ret = ext4_ind_remove_space(handle, inode, start_lblk,
						    end_lblk);
		if (ret) {
			up_write(&EXT4_I(inode)->i_data_sem);
			goto out_handle;
		}

		ext4_es_insert_extent(inode, start_lblk, hole_len, ~0,
				      EXTENT_STATUS_HOLE, 0);
		up_write(&EXT4_I(inode)->i_data_sem);
	}
	ext4_fc_track_range(handle, inode, start_lblk, end_lblk);

	ret = ext4_mark_inode_dirty(handle, inode);
	if (unlikely(ret))
		goto out_handle;

	ext4_update_inode_fsync_trans(handle, inode, 1);
	if (IS_SYNC(inode))
		ext4_handle_sync(handle);
out_handle:
	ext4_journal_stop(handle);
	return ret;
}

int ext4_inode_attach_jinode(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct jbd2_inode *jinode;

	if (ei->jinode || !EXT4_SB(inode->i_sb)->s_journal)
		return 0;

	jinode = jbd2_alloc_inode(GFP_KERNEL);
	spin_lock(&inode->i_lock);
	if (!ei->jinode) {
		if (!jinode) {
			spin_unlock(&inode->i_lock);
			return -ENOMEM;
		}
		ei->jinode = jinode;
		jbd2_journal_init_jbd_inode(ei->jinode, inode);
		jinode = NULL;
	}
	spin_unlock(&inode->i_lock);
	if (unlikely(jinode != NULL))
		jbd2_free_inode(jinode);
	return 0;
}

/*
 * ext4_truncate()
 *
 * We block out ext4_get_block() block instantiations across the entire
 * transaction, and VFS/VM ensures that ext4_truncate() cannot run
 * simultaneously on behalf of the same inode.
 *
 * As we work through the truncate and commit bits of it to the journal there
 * is one core, guiding principle: the file's tree must always be consistent on
 * disk.  We must be able to restart the truncate after a crash.
 *
 * The file's tree may be transiently inconsistent in memory (although it
 * probably isn't), but whenever we close off and commit a journal transaction,
 * the contents of (the filesystem + the journal) must be consistent and
 * restartable.  It's pretty simple, really: bottom up, right to left (although
 * left-to-right works OK too).
 *
 * Note that at recovery time, journal replay occurs *before* the restart of
 * truncate against the orphan inode list.
 *
 * The committed inode has the new, desired i_size (which is the same as
 * i_disksize in this case).  After a crash, ext4_orphan_cleanup() will see
 * that this inode's truncate did not complete and it will again call
 * ext4_truncate() to have another go.  So there will be instantiated blocks
 * to the right of the truncation point in a crashed ext4 filesystem.  But
 * that's fine - as long as they are linked from the inode, the post-crash
 * ext4_truncate() run will find them and release them.
 */
int ext4_truncate(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	unsigned int credits;
	int err = 0, err2;
	handle_t *handle;
	struct address_space *mapping = inode->i_mapping;

	/*
	 * There is a possibility that we're either freeing the inode
	 * or it's a completely new inode. In those cases we might not
	 * have i_rwsem locked because it's not necessary.
	 */
	if (!(inode->i_state & (I_NEW|I_FREEING)))
		WARN_ON(!inode_is_locked(inode));
	trace_ext4_truncate_enter(inode);

	if (!ext4_can_truncate(inode))
		goto out_trace;

	if (inode->i_size == 0 && !test_opt(inode->i_sb, NO_AUTO_DA_ALLOC))
		ext4_set_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE);

	if (ext4_has_inline_data(inode)) {
		int has_inline = 1;

		err = ext4_inline_data_truncate(inode, &has_inline);
		if (err || has_inline)
			goto out_trace;
	}

	/* If we zero-out tail of the page, we have to create jinode for jbd2 */
	if (inode->i_size & (inode->i_sb->s_blocksize - 1)) {
		err = ext4_inode_attach_jinode(inode);
		if (err)
			goto out_trace;
	}

	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		credits = ext4_chunk_trans_extent(inode, 1);
	else
		credits = ext4_blocks_for_truncate(inode);

	handle = ext4_journal_start(inode, EXT4_HT_TRUNCATE, credits);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_trace;
	}

	if (inode->i_size & (inode->i_sb->s_blocksize - 1))
		ext4_block_truncate_page(handle, mapping, inode->i_size);

	/*
	 * We add the inode to the orphan list, so that if this
	 * truncate spans multiple transactions, and we crash, we will
	 * resume the truncate when the filesystem recovers.  It also
	 * marks the inode dirty, to catch the new size.
	 *
	 * Implication: the file must always be in a sane, consistent
	 * truncatable state while each transaction commits.
	 */
	err = ext4_orphan_add(handle, inode);
	if (err)
		goto out_stop;

	ext4_fc_track_inode(handle, inode);
	ext4_check_map_extents_env(inode);

	down_write(&EXT4_I(inode)->i_data_sem);
	ext4_discard_preallocations(inode);

	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		err = ext4_ext_truncate(handle, inode);
	else
		ext4_ind_truncate(handle, inode);

	up_write(&ei->i_data_sem);
	if (err)
		goto out_stop;

	if (IS_SYNC(inode))
		ext4_handle_sync(handle);

out_stop:
	/*
	 * If this was a simple ftruncate() and the file will remain alive,
	 * then we need to clear up the orphan record which we created above.
	 * However, if this was a real unlink then we were called by
	 * ext4_evict_inode(), and we allow that function to clean up the
	 * orphan info for us.
	 */
	if (inode->i_nlink)
		ext4_orphan_del(handle, inode);

	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	err2 = ext4_mark_inode_dirty(handle, inode);
	if (unlikely(err2 && !err))
		err = err2;
	ext4_journal_stop(handle);

out_trace:
	trace_ext4_truncate_exit(inode);
	return err;
}

static inline u64 ext4_inode_peek_iversion(const struct inode *inode)
{
	if (unlikely(EXT4_I(inode)->i_flags & EXT4_EA_INODE_FL))
		return inode_peek_iversion_raw(inode);
	else
		return inode_peek_iversion(inode);
}

static int ext4_inode_blocks_set(struct ext4_inode *raw_inode,
				 struct ext4_inode_info *ei)
{
	struct inode *inode = &(ei->vfs_inode);
	u64 i_blocks = READ_ONCE(inode->i_blocks);
	struct super_block *sb = inode->i_sb;

	if (i_blocks <= ~0U) {
		/*
		 * i_blocks can be represented in a 32 bit variable
		 * as multiple of 512 bytes
		 */
		raw_inode->i_blocks_lo   = cpu_to_le32(i_blocks);
		raw_inode->i_blocks_high = 0;
		ext4_clear_inode_flag(inode, EXT4_INODE_HUGE_FILE);
		return 0;
	}

	/*
	 * This should never happen since sb->s_maxbytes should not have
	 * allowed this, sb->s_maxbytes was set according to the huge_file
	 * feature in ext4_fill_super().
	 */
	if (!ext4_has_feature_huge_file(sb))
		return -EFSCORRUPTED;

	if (i_blocks <= 0xffffffffffffULL) {
		/*
		 * i_blocks can be represented in a 48 bit variable
		 * as multiple of 512 bytes
		 */
		raw_inode->i_blocks_lo   = cpu_to_le32(i_blocks);
		raw_inode->i_blocks_high = cpu_to_le16(i_blocks >> 32);
		ext4_clear_inode_flag(inode, EXT4_INODE_HUGE_FILE);
	} else {
		ext4_set_inode_flag(inode, EXT4_INODE_HUGE_FILE);
		/* i_block is stored in file system block size */
		i_blocks = i_blocks >> (inode->i_blkbits - 9);
		raw_inode->i_blocks_lo   = cpu_to_le32(i_blocks);
		raw_inode->i_blocks_high = cpu_to_le16(i_blocks >> 32);
	}
	return 0;
}

static int ext4_fill_raw_inode(struct inode *inode, struct ext4_inode *raw_inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	uid_t i_uid;
	gid_t i_gid;
	projid_t i_projid;
	int block;
	int err;

	err = ext4_inode_blocks_set(raw_inode, ei);

	raw_inode->i_mode = cpu_to_le16(inode->i_mode);
	i_uid = i_uid_read(inode);
	i_gid = i_gid_read(inode);
	i_projid = from_kprojid(&init_user_ns, ei->i_projid);
	if (!(test_opt(inode->i_sb, NO_UID32))) {
		raw_inode->i_uid_low = cpu_to_le16(low_16_bits(i_uid));
		raw_inode->i_gid_low = cpu_to_le16(low_16_bits(i_gid));
		/*
		 * Fix up interoperability with old kernels. Otherwise,
		 * old inodes get re-used with the upper 16 bits of the
		 * uid/gid intact.
		 */
		if (ei->i_dtime && list_empty(&ei->i_orphan)) {
			raw_inode->i_uid_high = 0;
			raw_inode->i_gid_high = 0;
		} else {
			raw_inode->i_uid_high =
				cpu_to_le16(high_16_bits(i_uid));
			raw_inode->i_gid_high =
				cpu_to_le16(high_16_bits(i_gid));
		}
	} else {
		raw_inode->i_uid_low = cpu_to_le16(fs_high2lowuid(i_uid));
		raw_inode->i_gid_low = cpu_to_le16(fs_high2lowgid(i_gid));
		raw_inode->i_uid_high = 0;
		raw_inode->i_gid_high = 0;
	}
	raw_inode->i_links_count = cpu_to_le16(inode->i_nlink);

	EXT4_INODE_SET_CTIME(inode, raw_inode);
	EXT4_INODE_SET_MTIME(inode, raw_inode);
	EXT4_INODE_SET_ATIME(inode, raw_inode);
	EXT4_EINODE_SET_XTIME(i_crtime, ei, raw_inode);

	raw_inode->i_dtime = cpu_to_le32(ei->i_dtime);
	raw_inode->i_flags = cpu_to_le32(ei->i_flags & 0xFFFFFFFF);
	if (likely(!test_opt2(inode->i_sb, HURD_COMPAT)))
		raw_inode->i_file_acl_high =
			cpu_to_le16(ei->i_file_acl >> 32);
	raw_inode->i_file_acl_lo = cpu_to_le32(ei->i_file_acl);
	ext4_isize_set(raw_inode, ei->i_disksize);

	raw_inode->i_generation = cpu_to_le32(inode->i_generation);
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		if (old_valid_dev(inode->i_rdev)) {
			raw_inode->i_block[0] =
				cpu_to_le32(old_encode_dev(inode->i_rdev));
			raw_inode->i_block[1] = 0;
		} else {
			raw_inode->i_block[0] = 0;
			raw_inode->i_block[1] =
				cpu_to_le32(new_encode_dev(inode->i_rdev));
			raw_inode->i_block[2] = 0;
		}
	} else if (!ext4_has_inline_data(inode)) {
		for (block = 0; block < EXT4_N_BLOCKS; block++)
			raw_inode->i_block[block] = ei->i_data[block];
	}

	if (likely(!test_opt2(inode->i_sb, HURD_COMPAT))) {
		u64 ivers = ext4_inode_peek_iversion(inode);

		raw_inode->i_disk_version = cpu_to_le32(ivers);
		if (ei->i_extra_isize) {
			if (EXT4_FITS_IN_INODE(raw_inode, ei, i_version_hi))
				raw_inode->i_version_hi =
					cpu_to_le32(ivers >> 32);
			raw_inode->i_extra_isize =
				cpu_to_le16(ei->i_extra_isize);
		}
	}

	if (i_projid != EXT4_DEF_PROJID &&
	    !ext4_has_feature_project(inode->i_sb))
		err = err ?: -EFSCORRUPTED;

	if (EXT4_INODE_SIZE(inode->i_sb) > EXT4_GOOD_OLD_INODE_SIZE &&
	    EXT4_FITS_IN_INODE(raw_inode, ei, i_projid))
		raw_inode->i_projid = cpu_to_le32(i_projid);

	ext4_inode_csum_set(inode, raw_inode, ei);
	return err;
}

/*
 * ext4_get_inode_loc returns with an extra refcount against the inode's
 * underlying buffer_head on success. If we pass 'inode' and it does not
 * have in-inode xattr, we have all inode data in memory that is needed
 * to recreate the on-disk version of this inode.
 */
static int __ext4_get_inode_loc(struct super_block *sb, unsigned long ino,
				struct inode *inode, struct ext4_iloc *iloc,
				ext4_fsblk_t *ret_block)
{
	struct ext4_group_desc	*gdp;
	struct buffer_head	*bh;
	ext4_fsblk_t		block;
	struct blk_plug		plug;
	int			inodes_per_block, inode_offset;

	iloc->bh = NULL;
	if (ino < EXT4_ROOT_INO ||
	    ino > le32_to_cpu(EXT4_SB(sb)->s_es->s_inodes_count))
		return -EFSCORRUPTED;

	iloc->block_group = (ino - 1) / EXT4_INODES_PER_GROUP(sb);
	gdp = ext4_get_group_desc(sb, iloc->block_group, NULL);
	if (!gdp)
		return -EIO;

	/*
	 * Figure out the offset within the block group inode table
	 */
	inodes_per_block = EXT4_SB(sb)->s_inodes_per_block;
	inode_offset = ((ino - 1) %
			EXT4_INODES_PER_GROUP(sb));
	iloc->offset = (inode_offset % inodes_per_block) * EXT4_INODE_SIZE(sb);

	block = ext4_inode_table(sb, gdp);
	if ((block <= le32_to_cpu(EXT4_SB(sb)->s_es->s_first_data_block)) ||
	    (block >= ext4_blocks_count(EXT4_SB(sb)->s_es))) {
		ext4_error(sb, "Invalid inode table block %llu in "
			   "block_group %u", block, iloc->block_group);
		return -EFSCORRUPTED;
	}
	block += (inode_offset / inodes_per_block);

	bh = sb_getblk(sb, block);
	if (unlikely(!bh))
		return -ENOMEM;
	if (ext4_buffer_uptodate(bh))
		goto has_buffer;

	lock_buffer(bh);
	if (ext4_buffer_uptodate(bh)) {
		/* Someone brought it uptodate while we waited */
		unlock_buffer(bh);
		goto has_buffer;
	}

	/*
	 * If we have all information of the inode in memory and this
	 * is the only valid inode in the block, we need not read the
	 * block.
	 */
	if (inode && !ext4_test_inode_state(inode, EXT4_STATE_XATTR)) {
		struct buffer_head *bitmap_bh;
		int i, start;

		start = inode_offset & ~(inodes_per_block - 1);

		/* Is the inode bitmap in cache? */
		bitmap_bh = sb_getblk(sb, ext4_inode_bitmap(sb, gdp));
		if (unlikely(!bitmap_bh))
			goto make_io;

		/*
		 * If the inode bitmap isn't in cache then the
		 * optimisation may end up performing two reads instead
		 * of one, so skip it.
		 */
		if (!buffer_uptodate(bitmap_bh)) {
			brelse(bitmap_bh);
			goto make_io;
		}
		for (i = start; i < start + inodes_per_block; i++) {
			if (i == inode_offset)
				continue;
			if (ext4_test_bit(i, bitmap_bh->b_data))
				break;
		}
		brelse(bitmap_bh);
		if (i == start + inodes_per_block) {
			struct ext4_inode *raw_inode =
				(struct ext4_inode *) (bh->b_data + iloc->offset);

			/* all other inodes are free, so skip I/O */
			memset(bh->b_data, 0, bh->b_size);
			if (!ext4_test_inode_state(inode, EXT4_STATE_NEW))
				ext4_fill_raw_inode(inode, raw_inode);
			set_buffer_uptodate(bh);
			unlock_buffer(bh);
			goto has_buffer;
		}
	}

make_io:
	/*
	 * If we need to do any I/O, try to pre-readahead extra
	 * blocks from the inode table.
	 */
	blk_start_plug(&plug);
	if (EXT4_SB(sb)->s_inode_readahead_blks) {
		ext4_fsblk_t b, end, table;
		unsigned num;
		__u32 ra_blks = EXT4_SB(sb)->s_inode_readahead_blks;

		table = ext4_inode_table(sb, gdp);
		/* s_inode_readahead_blks is always a power of 2 */
		b = block & ~((ext4_fsblk_t) ra_blks - 1);
		if (table > b)
			b = table;
		end = b + ra_blks;
		num = EXT4_INODES_PER_GROUP(sb);
		if (ext4_has_group_desc_csum(sb))
			num -= ext4_itable_unused_count(sb, gdp);
		table += num / inodes_per_block;
		if (end > table)
			end = table;
		while (b <= end)
			ext4_sb_breadahead_unmovable(sb, b++);
	}

	/*
	 * There are other valid inodes in the buffer, this inode
	 * has in-inode xattrs, or we don't have this inode in memory.
	 * Read the block from disk.
	 */
	trace_ext4_load_inode(sb, ino);
	ext4_read_bh_nowait(bh, REQ_META | REQ_PRIO, NULL,
			    ext4_simulate_fail(sb, EXT4_SIM_INODE_EIO));
	blk_finish_plug(&plug);
	wait_on_buffer(bh);
	if (!buffer_uptodate(bh)) {
		if (ret_block)
			*ret_block = block;
		brelse(bh);
		return -EIO;
	}
has_buffer:
	iloc->bh = bh;
	return 0;
}

static int __ext4_get_inode_loc_noinmem(struct inode *inode,
					struct ext4_iloc *iloc)
{
	ext4_fsblk_t err_blk = 0;
	int ret;

	ret = __ext4_get_inode_loc(inode->i_sb, inode->i_ino, NULL, iloc,
					&err_blk);

	if (ret == -EIO)
		ext4_error_inode_block(inode, err_blk, EIO,
					"unable to read itable block");

	return ret;
}

int ext4_get_inode_loc(struct inode *inode, struct ext4_iloc *iloc)
{
	ext4_fsblk_t err_blk = 0;
	int ret;

	ret = __ext4_get_inode_loc(inode->i_sb, inode->i_ino, inode, iloc,
					&err_blk);

	if (ret == -EIO)
		ext4_error_inode_block(inode, err_blk, EIO,
					"unable to read itable block");

	return ret;
}


int ext4_get_fc_inode_loc(struct super_block *sb, unsigned long ino,
			  struct ext4_iloc *iloc)
{
	return __ext4_get_inode_loc(sb, ino, NULL, iloc, NULL);
}

static bool ext4_should_enable_dax(struct inode *inode)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

	if (test_opt2(inode->i_sb, DAX_NEVER))
		return false;
	if (!S_ISREG(inode->i_mode))
		return false;
	if (ext4_should_journal_data(inode))
		return false;
	if (ext4_has_inline_data(inode))
		return false;
	if (ext4_test_inode_flag(inode, EXT4_INODE_ENCRYPT))
		return false;
	if (ext4_test_inode_flag(inode, EXT4_INODE_VERITY))
		return false;
	if (!test_bit(EXT4_FLAGS_BDEV_IS_DAX, &sbi->s_ext4_flags))
		return false;
	if (test_opt(inode->i_sb, DAX_ALWAYS))
		return true;

	return ext4_test_inode_flag(inode, EXT4_INODE_DAX);
}

void ext4_set_inode_flags(struct inode *inode, bool init)
{
	unsigned int flags = EXT4_I(inode)->i_flags;
	unsigned int new_fl = 0;

	WARN_ON_ONCE(IS_DAX(inode) && init);

	if (flags & EXT4_SYNC_FL)
		new_fl |= S_SYNC;
	if (flags & EXT4_APPEND_FL)
		new_fl |= S_APPEND;
	if (flags & EXT4_IMMUTABLE_FL)
		new_fl |= S_IMMUTABLE;
	if (flags & EXT4_NOATIME_FL)
		new_fl |= S_NOATIME;
	if (flags & EXT4_DIRSYNC_FL)
		new_fl |= S_DIRSYNC;

	/* Because of the way inode_set_flags() works we must preserve S_DAX
	 * here if already set. */
	new_fl |= (inode->i_flags & S_DAX);
	if (init && ext4_should_enable_dax(inode))
		new_fl |= S_DAX;

	if (flags & EXT4_ENCRYPT_FL)
		new_fl |= S_ENCRYPTED;
	if (flags & EXT4_CASEFOLD_FL)
		new_fl |= S_CASEFOLD;
	if (flags & EXT4_VERITY_FL)
		new_fl |= S_VERITY;
	inode_set_flags(inode, new_fl,
			S_SYNC|S_APPEND|S_IMMUTABLE|S_NOATIME|S_DIRSYNC|S_DAX|
			S_ENCRYPTED|S_CASEFOLD|S_VERITY);
}

static blkcnt_t ext4_inode_blocks(struct ext4_inode *raw_inode,
				  struct ext4_inode_info *ei)
{
	blkcnt_t i_blocks ;
	struct inode *inode = &(ei->vfs_inode);
	struct super_block *sb = inode->i_sb;

	if (ext4_has_feature_huge_file(sb)) {
		/* we are using combined 48 bit field */
		i_blocks = ((u64)le16_to_cpu(raw_inode->i_blocks_high)) << 32 |
					le32_to_cpu(raw_inode->i_blocks_lo);
		if (ext4_test_inode_flag(inode, EXT4_INODE_HUGE_FILE)) {
			/* i_blocks represent file system block size */
			return i_blocks  << (inode->i_blkbits - 9);
		} else {
			return i_blocks;
		}
	} else {
		return le32_to_cpu(raw_inode->i_blocks_lo);
	}
}

static inline int ext4_iget_extra_inode(struct inode *inode,
					 struct ext4_inode *raw_inode,
					 struct ext4_inode_info *ei)
{
	__le32 *magic = (void *)raw_inode +
			EXT4_GOOD_OLD_INODE_SIZE + ei->i_extra_isize;

	if (EXT4_INODE_HAS_XATTR_SPACE(inode)  &&
	    *magic == cpu_to_le32(EXT4_XATTR_MAGIC)) {
		int err;

		err = xattr_check_inode(inode, IHDR(inode, raw_inode),
					ITAIL(inode, raw_inode));
		if (err)
			return err;

		ext4_set_inode_state(inode, EXT4_STATE_XATTR);
		err = ext4_find_inline_data_nolock(inode);
		if (!err && ext4_has_inline_data(inode))
			ext4_set_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);
		return err;
	} else
		EXT4_I(inode)->i_inline_off = 0;
	return 0;
}

int ext4_get_projid(struct inode *inode, kprojid_t *projid)
{
	if (!ext4_has_feature_project(inode->i_sb))
		return -EOPNOTSUPP;
	*projid = EXT4_I(inode)->i_projid;
	return 0;
}

/*
 * ext4 has self-managed i_version for ea inodes, it stores the lower 32bit of
 * refcount in i_version, so use raw values if inode has EXT4_EA_INODE_FL flag
 * set.
 */
static inline void ext4_inode_set_iversion_queried(struct inode *inode, u64 val)
{
	if (unlikely(EXT4_I(inode)->i_flags & EXT4_EA_INODE_FL))
		inode_set_iversion_raw(inode, val);
	else
		inode_set_iversion_queried(inode, val);
}

static int check_igot_inode(struct inode *inode, ext4_iget_flags flags,
			    const char *function, unsigned int line)
{
	const char *err_str;

	if (flags & EXT4_IGET_EA_INODE) {
		if (!(EXT4_I(inode)->i_flags & EXT4_EA_INODE_FL)) {
			err_str = "missing EA_INODE flag";
			goto error;
		}
		if (ext4_test_inode_state(inode, EXT4_STATE_XATTR) ||
		    EXT4_I(inode)->i_file_acl) {
			err_str = "ea_inode with extended attributes";
			goto error;
		}
	} else {
		if ((EXT4_I(inode)->i_flags & EXT4_EA_INODE_FL)) {
			/*
			 * open_by_handle_at() could provide an old inode number
			 * that has since been reused for an ea_inode; this does
			 * not indicate filesystem corruption
			 */
			if (flags & EXT4_IGET_HANDLE)
				return -ESTALE;
			err_str = "unexpected EA_INODE flag";
			goto error;
		}
	}
	if (is_bad_inode(inode) && !(flags & EXT4_IGET_BAD)) {
		err_str = "unexpected bad inode w/o EXT4_IGET_BAD";
		goto error;
	}
	return 0;

error:
	ext4_error_inode(inode, function, line, 0, "%s", err_str);
	return -EFSCORRUPTED;
}

static bool ext4_should_enable_large_folio(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	if (!S_ISREG(inode->i_mode))
		return false;
	if (test_opt(sb, DATA_FLAGS) == EXT4_MOUNT_JOURNAL_DATA ||
	    ext4_test_inode_flag(inode, EXT4_INODE_JOURNAL_DATA))
		return false;
	if (ext4_has_feature_verity(sb))
		return false;
	if (ext4_has_feature_encrypt(sb))
		return false;

	return true;
}

/*
 * Limit the maximum folio order to 2048 blocks to prevent overestimation
 * of reserve handle credits during the folio writeback in environments
 * where the PAGE_SIZE exceeds 4KB.
 */
#define EXT4_MAX_PAGECACHE_ORDER(i)		\
		umin(MAX_PAGECACHE_ORDER, (11 + (i)->i_blkbits - PAGE_SHIFT))
void ext4_set_inode_mapping_order(struct inode *inode)
{
	if (!ext4_should_enable_large_folio(inode))
		return;

	mapping_set_folio_order_range(inode->i_mapping, 0,
				      EXT4_MAX_PAGECACHE_ORDER(inode));
}

struct inode *__ext4_iget(struct super_block *sb, unsigned long ino,
			  ext4_iget_flags flags, const char *function,
			  unsigned int line)
{
	struct ext4_iloc iloc;
	struct ext4_inode *raw_inode;
	struct ext4_inode_info *ei;
	struct ext4_super_block *es = EXT4_SB(sb)->s_es;
	struct inode *inode;
	journal_t *journal = EXT4_SB(sb)->s_journal;
	long ret;
	loff_t size;
	int block;
	uid_t i_uid;
	gid_t i_gid;
	projid_t i_projid;

	if ((!(flags & EXT4_IGET_SPECIAL) && is_special_ino(sb, ino)) ||
	    (ino < EXT4_ROOT_INO) ||
	    (ino > le32_to_cpu(es->s_inodes_count))) {
		if (flags & EXT4_IGET_HANDLE)
			return ERR_PTR(-ESTALE);
		__ext4_error(sb, function, line, false, EFSCORRUPTED, 0,
			     "inode #%lu: comm %s: iget: illegal inode #",
			     ino, current->comm);
		return ERR_PTR(-EFSCORRUPTED);
	}

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW)) {
		ret = check_igot_inode(inode, flags, function, line);
		if (ret) {
			iput(inode);
			return ERR_PTR(ret);
		}
		return inode;
	}

	ei = EXT4_I(inode);
	iloc.bh = NULL;

	ret = __ext4_get_inode_loc_noinmem(inode, &iloc);
	if (ret < 0)
		goto bad_inode;
	raw_inode = ext4_raw_inode(&iloc);

	if ((flags & EXT4_IGET_HANDLE) &&
	    (raw_inode->i_links_count == 0) && (raw_inode->i_mode == 0)) {
		ret = -ESTALE;
		goto bad_inode;
	}

	if (EXT4_INODE_SIZE(inode->i_sb) > EXT4_GOOD_OLD_INODE_SIZE) {
		ei->i_extra_isize = le16_to_cpu(raw_inode->i_extra_isize);
		if (EXT4_GOOD_OLD_INODE_SIZE + ei->i_extra_isize >
			EXT4_INODE_SIZE(inode->i_sb) ||
		    (ei->i_extra_isize & 3)) {
			ext4_error_inode(inode, function, line, 0,
					 "iget: bad extra_isize %u "
					 "(inode size %u)",
					 ei->i_extra_isize,
					 EXT4_INODE_SIZE(inode->i_sb));
			ret = -EFSCORRUPTED;
			goto bad_inode;
		}
	} else
		ei->i_extra_isize = 0;

	/* Precompute checksum seed for inode metadata */
	if (ext4_has_feature_metadata_csum(sb)) {
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
		__u32 csum;
		__le32 inum = cpu_to_le32(inode->i_ino);
		__le32 gen = raw_inode->i_generation;
		csum = ext4_chksum(sbi->s_csum_seed, (__u8 *)&inum,
				   sizeof(inum));
		ei->i_csum_seed = ext4_chksum(csum, (__u8 *)&gen, sizeof(gen));
	}

	if ((!ext4_inode_csum_verify(inode, raw_inode, ei) ||
	    ext4_simulate_fail(sb, EXT4_SIM_INODE_CRC)) &&
	     (!(EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY))) {
		ext4_error_inode_err(inode, function, line, 0,
				EFSBADCRC, "iget: checksum invalid");
		ret = -EFSBADCRC;
		goto bad_inode;
	}

	inode->i_mode = le16_to_cpu(raw_inode->i_mode);
	i_uid = (uid_t)le16_to_cpu(raw_inode->i_uid_low);
	i_gid = (gid_t)le16_to_cpu(raw_inode->i_gid_low);
	if (ext4_has_feature_project(sb) &&
	    EXT4_INODE_SIZE(sb) > EXT4_GOOD_OLD_INODE_SIZE &&
	    EXT4_FITS_IN_INODE(raw_inode, ei, i_projid))
		i_projid = (projid_t)le32_to_cpu(raw_inode->i_projid);
	else
		i_projid = EXT4_DEF_PROJID;

	if (!(test_opt(inode->i_sb, NO_UID32))) {
		i_uid |= le16_to_cpu(raw_inode->i_uid_high) << 16;
		i_gid |= le16_to_cpu(raw_inode->i_gid_high) << 16;
	}
	i_uid_write(inode, i_uid);
	i_gid_write(inode, i_gid);
	ei->i_projid = make_kprojid(&init_user_ns, i_projid);
	set_nlink(inode, le16_to_cpu(raw_inode->i_links_count));

	ext4_clear_state_flags(ei);	/* Only relevant on 32-bit archs */
	ei->i_inline_off = 0;
	ei->i_dir_start_lookup = 0;
	ei->i_dtime = le32_to_cpu(raw_inode->i_dtime);
	/* We now have enough fields to check if the inode was active or not.
	 * This is needed because nfsd might try to access dead inodes
	 * the test is that same one that e2fsck uses
	 * NeilBrown 1999oct15
	 */
	if (inode->i_nlink == 0) {
		if ((inode->i_mode == 0 || flags & EXT4_IGET_SPECIAL ||
		     !(EXT4_SB(inode->i_sb)->s_mount_state & EXT4_ORPHAN_FS)) &&
		    ino != EXT4_BOOT_LOADER_INO) {
			/* this inode is deleted or unallocated */
			if (flags & EXT4_IGET_SPECIAL) {
				ext4_error_inode(inode, function, line, 0,
						 "iget: special inode unallocated");
				ret = -EFSCORRUPTED;
			} else
				ret = -ESTALE;
			goto bad_inode;
		}
		/* The only unlinked inodes we let through here have
		 * valid i_mode and are being read by the orphan
		 * recovery code: that's fine, we're about to complete
		 * the process of deleting those.
		 * OR it is the EXT4_BOOT_LOADER_INO which is
		 * not initialized on a new filesystem. */
	}
	ei->i_flags = le32_to_cpu(raw_inode->i_flags);
	ext4_set_inode_flags(inode, true);
	inode->i_blocks = ext4_inode_blocks(raw_inode, ei);
	ei->i_file_acl = le32_to_cpu(raw_inode->i_file_acl_lo);
	if (ext4_has_feature_64bit(sb))
		ei->i_file_acl |=
			((__u64)le16_to_cpu(raw_inode->i_file_acl_high)) << 32;
	inode->i_size = ext4_isize(sb, raw_inode);
	size = i_size_read(inode);
	if (size < 0 || size > ext4_get_maxbytes(inode)) {
		ext4_error_inode(inode, function, line, 0,
				 "iget: bad i_size value: %lld", size);
		ret = -EFSCORRUPTED;
		goto bad_inode;
	}
	/*
	 * If dir_index is not enabled but there's dir with INDEX flag set,
	 * we'd normally treat htree data as empty space. But with metadata
	 * checksumming that corrupts checksums so forbid that.
	 */
	if (!ext4_has_feature_dir_index(sb) &&
	    ext4_has_feature_metadata_csum(sb) &&
	    ext4_test_inode_flag(inode, EXT4_INODE_INDEX)) {
		ext4_error_inode(inode, function, line, 0,
			 "iget: Dir with htree data on filesystem without dir_index feature.");
		ret = -EFSCORRUPTED;
		goto bad_inode;
	}
	ei->i_disksize = inode->i_size;
#ifdef CONFIG_QUOTA
	ei->i_reserved_quota = 0;
#endif
	inode->i_generation = le32_to_cpu(raw_inode->i_generation);
	ei->i_block_group = iloc.block_group;
	ei->i_last_alloc_group = ~0;
	/*
	 * NOTE! The in-memory inode i_data array is in little-endian order
	 * even on big-endian machines: we do NOT byteswap the block numbers!
	 */
	for (block = 0; block < EXT4_N_BLOCKS; block++)
		ei->i_data[block] = raw_inode->i_block[block];
	INIT_LIST_HEAD(&ei->i_orphan);
	ext4_fc_init_inode(&ei->vfs_inode);

	/*
	 * Set transaction id's of transactions that have to be committed
	 * to finish f[data]sync. We set them to currently running transaction
	 * as we cannot be sure that the inode or some of its metadata isn't
	 * part of the transaction - the inode could have been reclaimed and
	 * now it is reread from disk.
	 */
	if (journal) {
		transaction_t *transaction;
		tid_t tid;

		read_lock(&journal->j_state_lock);
		if (journal->j_running_transaction)
			transaction = journal->j_running_transaction;
		else
			transaction = journal->j_committing_transaction;
		if (transaction)
			tid = transaction->t_tid;
		else
			tid = journal->j_commit_sequence;
		read_unlock(&journal->j_state_lock);
		ei->i_sync_tid = tid;
		ei->i_datasync_tid = tid;
	}

	if (EXT4_INODE_SIZE(inode->i_sb) > EXT4_GOOD_OLD_INODE_SIZE) {
		if (ei->i_extra_isize == 0) {
			/* The extra space is currently unused. Use it. */
			BUILD_BUG_ON(sizeof(struct ext4_inode) & 3);
			ei->i_extra_isize = sizeof(struct ext4_inode) -
					    EXT4_GOOD_OLD_INODE_SIZE;
		} else {
			ret = ext4_iget_extra_inode(inode, raw_inode, ei);
			if (ret)
				goto bad_inode;
		}
	}

	EXT4_INODE_GET_CTIME(inode, raw_inode);
	EXT4_INODE_GET_ATIME(inode, raw_inode);
	EXT4_INODE_GET_MTIME(inode, raw_inode);
	EXT4_EINODE_GET_XTIME(i_crtime, ei, raw_inode);

	if (likely(!test_opt2(inode->i_sb, HURD_COMPAT))) {
		u64 ivers = le32_to_cpu(raw_inode->i_disk_version);

		if (EXT4_INODE_SIZE(inode->i_sb) > EXT4_GOOD_OLD_INODE_SIZE) {
			if (EXT4_FITS_IN_INODE(raw_inode, ei, i_version_hi))
				ivers |=
		    (__u64)(le32_to_cpu(raw_inode->i_version_hi)) << 32;
		}
		ext4_inode_set_iversion_queried(inode, ivers);
	}

	ret = 0;
	if (ei->i_file_acl &&
	    !ext4_inode_block_valid(inode, ei->i_file_acl, 1)) {
		ext4_error_inode(inode, function, line, 0,
				 "iget: bad extended attribute block %llu",
				 ei->i_file_acl);
		ret = -EFSCORRUPTED;
		goto bad_inode;
	} else if (!ext4_has_inline_data(inode)) {
		/* validate the block references in the inode */
		if (!(EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY) &&
			(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
			(S_ISLNK(inode->i_mode) &&
			!ext4_inode_is_fast_symlink(inode)))) {
			if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
				ret = ext4_ext_check_inode(inode);
			else
				ret = ext4_ind_check_inode(inode);
		}
	}
	if (ret)
		goto bad_inode;

	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &ext4_file_inode_operations;
		inode->i_fop = &ext4_file_operations;
		ext4_set_aops(inode);
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &ext4_dir_inode_operations;
		inode->i_fop = &ext4_dir_operations;
	} else if (S_ISLNK(inode->i_mode)) {
		/* VFS does not allow setting these so must be corruption */
		if (IS_APPEND(inode) || IS_IMMUTABLE(inode)) {
			ext4_error_inode(inode, function, line, 0,
					 "iget: immutable or append flags "
					 "not allowed on symlinks");
			ret = -EFSCORRUPTED;
			goto bad_inode;
		}
		if (IS_ENCRYPTED(inode)) {
			inode->i_op = &ext4_encrypted_symlink_inode_operations;
		} else if (ext4_inode_is_fast_symlink(inode)) {
			inode->i_op = &ext4_fast_symlink_inode_operations;
			if (inode->i_size == 0 ||
			    inode->i_size >= sizeof(ei->i_data) ||
			    strnlen((char *)ei->i_data, inode->i_size + 1) !=
								inode->i_size) {
				ext4_error_inode(inode, function, line, 0,
					"invalid fast symlink length %llu",
					 (unsigned long long)inode->i_size);
				ret = -EFSCORRUPTED;
				goto bad_inode;
			}
			inode_set_cached_link(inode, (char *)ei->i_data,
					      inode->i_size);
		} else {
			inode->i_op = &ext4_symlink_inode_operations;
		}
	} else if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode) ||
	      S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
		inode->i_op = &ext4_special_inode_operations;
		if (raw_inode->i_block[0])
			init_special_inode(inode, inode->i_mode,
			   old_decode_dev(le32_to_cpu(raw_inode->i_block[0])));
		else
			init_special_inode(inode, inode->i_mode,
			   new_decode_dev(le32_to_cpu(raw_inode->i_block[1])));
	} else if (ino == EXT4_BOOT_LOADER_INO) {
		make_bad_inode(inode);
	} else {
		ret = -EFSCORRUPTED;
		ext4_error_inode(inode, function, line, 0,
				 "iget: bogus i_mode (%o)", inode->i_mode);
		goto bad_inode;
	}
	if (IS_CASEFOLDED(inode) && !ext4_has_feature_casefold(inode->i_sb)) {
		ext4_error_inode(inode, function, line, 0,
				 "casefold flag without casefold feature");
		ret = -EFSCORRUPTED;
		goto bad_inode;
	}

	ext4_set_inode_mapping_order(inode);

	ret = check_igot_inode(inode, flags, function, line);
	/*
	 * -ESTALE here means there is nothing inherently wrong with the inode,
	 * it's just not an inode we can return for an fhandle lookup.
	 */
	if (ret == -ESTALE) {
		brelse(iloc.bh);
		unlock_new_inode(inode);
		iput(inode);
		return ERR_PTR(-ESTALE);
	}
	if (ret)
		goto bad_inode;
	brelse(iloc.bh);

	unlock_new_inode(inode);
	return inode;

bad_inode:
	brelse(iloc.bh);
	iget_failed(inode);
	return ERR_PTR(ret);
}

static void __ext4_update_other_inode_time(struct super_block *sb,
					   unsigned long orig_ino,
					   unsigned long ino,
					   struct ext4_inode *raw_inode)
{
	struct inode *inode;

	inode = find_inode_by_ino_rcu(sb, ino);
	if (!inode)
		return;

	if (!inode_is_dirtytime_only(inode))
		return;

	spin_lock(&inode->i_lock);
	if (inode_is_dirtytime_only(inode)) {
		struct ext4_inode_info	*ei = EXT4_I(inode);

		inode->i_state &= ~I_DIRTY_TIME;
		spin_unlock(&inode->i_lock);

		spin_lock(&ei->i_raw_lock);
		EXT4_INODE_SET_CTIME(inode, raw_inode);
		EXT4_INODE_SET_MTIME(inode, raw_inode);
		EXT4_INODE_SET_ATIME(inode, raw_inode);
		ext4_inode_csum_set(inode, raw_inode, ei);
		spin_unlock(&ei->i_raw_lock);
		trace_ext4_other_inode_update_time(inode, orig_ino);
		return;
	}
	spin_unlock(&inode->i_lock);
}

/*
 * Opportunistically update the other time fields for other inodes in
 * the same inode table block.
 */
static void ext4_update_other_inodes_time(struct super_block *sb,
					  unsigned long orig_ino, char *buf)
{
	unsigned long ino;
	int i, inodes_per_block = EXT4_SB(sb)->s_inodes_per_block;
	int inode_size = EXT4_INODE_SIZE(sb);

	/*
	 * Calculate the first inode in the inode table block.  Inode
	 * numbers are one-based.  That is, the first inode in a block
	 * (assuming 4k blocks and 256 byte inodes) is (n*16 + 1).
	 */
	ino = ((orig_ino - 1) & ~(inodes_per_block - 1)) + 1;
	rcu_read_lock();
	for (i = 0; i < inodes_per_block; i++, ino++, buf += inode_size) {
		if (ino == orig_ino)
			continue;
		__ext4_update_other_inode_time(sb, orig_ino, ino,
					       (struct ext4_inode *)buf);
	}
	rcu_read_unlock();
}

/*
 * Post the struct inode info into an on-disk inode location in the
 * buffer-cache.  This gobbles the caller's reference to the
 * buffer_head in the inode location struct.
 *
 * The caller must have write access to iloc->bh.
 */
static int ext4_do_update_inode(handle_t *handle,
				struct inode *inode,
				struct ext4_iloc *iloc)
{
	struct ext4_inode *raw_inode = ext4_raw_inode(iloc);
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct buffer_head *bh = iloc->bh;
	struct super_block *sb = inode->i_sb;
	int err;
	int need_datasync = 0, set_large_file = 0;

	spin_lock(&ei->i_raw_lock);

	/*
	 * For fields not tracked in the in-memory inode, initialise them
	 * to zero for new inodes.
	 */
	if (ext4_test_inode_state(inode, EXT4_STATE_NEW))
		memset(raw_inode, 0, EXT4_SB(inode->i_sb)->s_inode_size);

	if (READ_ONCE(ei->i_disksize) != ext4_isize(inode->i_sb, raw_inode))
		need_datasync = 1;
	if (ei->i_disksize > 0x7fffffffULL) {
		if (!ext4_has_feature_large_file(sb) ||
		    EXT4_SB(sb)->s_es->s_rev_level == cpu_to_le32(EXT4_GOOD_OLD_REV))
			set_large_file = 1;
	}

	err = ext4_fill_raw_inode(inode, raw_inode);
	spin_unlock(&ei->i_raw_lock);
	if (err) {
		EXT4_ERROR_INODE(inode, "corrupted inode contents");
		goto out_brelse;
	}

	if (inode->i_sb->s_flags & SB_LAZYTIME)
		ext4_update_other_inodes_time(inode->i_sb, inode->i_ino,
					      bh->b_data);

	BUFFER_TRACE(bh, "call ext4_handle_dirty_metadata");
	err = ext4_handle_dirty_metadata(handle, NULL, bh);
	if (err)
		goto out_error;
	ext4_clear_inode_state(inode, EXT4_STATE_NEW);
	if (set_large_file) {
		BUFFER_TRACE(EXT4_SB(sb)->s_sbh, "get write access");
		err = ext4_journal_get_write_access(handle, sb,
						    EXT4_SB(sb)->s_sbh,
						    EXT4_JTR_NONE);
		if (err)
			goto out_error;
		lock_buffer(EXT4_SB(sb)->s_sbh);
		ext4_set_feature_large_file(sb);
		ext4_superblock_csum_set(sb);
		unlock_buffer(EXT4_SB(sb)->s_sbh);
		ext4_handle_sync(handle);
		err = ext4_handle_dirty_metadata(handle, NULL,
						 EXT4_SB(sb)->s_sbh);
	}
	ext4_update_inode_fsync_trans(handle, inode, need_datasync);
out_error:
	ext4_std_error(inode->i_sb, err);
out_brelse:
	brelse(bh);
	return err;
}

/*
 * ext4_write_inode()
 *
 * We are called from a few places:
 *
 * - Within generic_file_aio_write() -> generic_write_sync() for O_SYNC files.
 *   Here, there will be no transaction running. We wait for any running
 *   transaction to commit.
 *
 * - Within flush work (sys_sync(), kupdate and such).
 *   We wait on commit, if told to.
 *
 * - Within iput_final() -> write_inode_now()
 *   We wait on commit, if told to.
 *
 * In all cases it is actually safe for us to return without doing anything,
 * because the inode has been copied into a raw inode buffer in
 * ext4_mark_inode_dirty().  This is a correctness thing for WB_SYNC_ALL
 * writeback.
 *
 * Note that we are absolutely dependent upon all inode dirtiers doing the
 * right thing: they *must* call mark_inode_dirty() after dirtying info in
 * which we are interested.
 *
 * It would be a bug for them to not do this.  The code:
 *
 *	mark_inode_dirty(inode)
 *	stuff();
 *	inode->i_size = expr;
 *
 * is in error because write_inode() could occur while `stuff()' is running,
 * and the new i_size will be lost.  Plus the inode will no longer be on the
 * superblock's dirty inode list.
 */
int ext4_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int err;

	if (WARN_ON_ONCE(current->flags & PF_MEMALLOC))
		return 0;

	err = ext4_emergency_state(inode->i_sb);
	if (unlikely(err))
		return err;

	if (EXT4_SB(inode->i_sb)->s_journal) {
		if (ext4_journal_current_handle()) {
			ext4_debug("called recursively, non-PF_MEMALLOC!\n");
			dump_stack();
			return -EIO;
		}

		/*
		 * No need to force transaction in WB_SYNC_NONE mode. Also
		 * ext4_sync_fs() will force the commit after everything is
		 * written.
		 */
		if (wbc->sync_mode != WB_SYNC_ALL || wbc->for_sync)
			return 0;

		err = ext4_fc_commit(EXT4_SB(inode->i_sb)->s_journal,
						EXT4_I(inode)->i_sync_tid);
	} else {
		struct ext4_iloc iloc;

		err = __ext4_get_inode_loc_noinmem(inode, &iloc);
		if (err)
			return err;
		/*
		 * sync(2) will flush the whole buffer cache. No need to do
		 * it here separately for each inode.
		 */
		if (wbc->sync_mode == WB_SYNC_ALL && !wbc->for_sync)
			sync_dirty_buffer(iloc.bh);
		if (buffer_req(iloc.bh) && !buffer_uptodate(iloc.bh)) {
			ext4_error_inode_block(inode, iloc.bh->b_blocknr, EIO,
					       "IO error syncing inode");
			err = -EIO;
		}
		brelse(iloc.bh);
	}
	return err;
}

/*
 * In data=journal mode ext4_journalled_invalidate_folio() may fail to invalidate
 * buffers that are attached to a folio straddling i_size and are undergoing
 * commit. In that case we have to wait for commit to finish and try again.
 */
static void ext4_wait_for_tail_page_commit(struct inode *inode)
{
	unsigned offset;
	journal_t *journal = EXT4_SB(inode->i_sb)->s_journal;
	tid_t commit_tid;
	int ret;
	bool has_transaction;

	offset = inode->i_size & (PAGE_SIZE - 1);
	/*
	 * If the folio is fully truncated, we don't need to wait for any commit
	 * (and we even should not as __ext4_journalled_invalidate_folio() may
	 * strip all buffers from the folio but keep the folio dirty which can then
	 * confuse e.g. concurrent ext4_writepages() seeing dirty folio without
	 * buffers). Also we don't need to wait for any commit if all buffers in
	 * the folio remain valid. This is most beneficial for the common case of
	 * blocksize == PAGESIZE.
	 */
	if (!offset || offset > (PAGE_SIZE - i_blocksize(inode)))
		return;
	while (1) {
		struct folio *folio = filemap_lock_folio(inode->i_mapping,
				      inode->i_size >> PAGE_SHIFT);
		if (IS_ERR(folio))
			return;
		ret = __ext4_journalled_invalidate_folio(folio, offset,
						folio_size(folio) - offset);
		folio_unlock(folio);
		folio_put(folio);
		if (ret != -EBUSY)
			return;
		has_transaction = false;
		read_lock(&journal->j_state_lock);
		if (journal->j_committing_transaction) {
			commit_tid = journal->j_committing_transaction->t_tid;
			has_transaction = true;
		}
		read_unlock(&journal->j_state_lock);
		if (has_transaction)
			jbd2_log_wait_commit(journal, commit_tid);
	}
}

/*
 * ext4_setattr()
 *
 * Called from notify_change.
 *
 * We want to trap VFS attempts to truncate the file as soon as
 * possible.  In particular, we want to make sure that when the VFS
 * shrinks i_size, we put the inode on the orphan list and modify
 * i_disksize immediately, so that during the subsequent flushing of
 * dirty pages and freeing of disk blocks, we can guarantee that any
 * commit will leave the blocks being flushed in an unused state on
 * disk.  (On recovery, the inode will get truncated and the blocks will
 * be freed, so we have a strong guarantee that no future commit will
 * leave these blocks visible to the user.)
 *
 * Another thing we have to assure is that if we are in ordered mode
 * and inode is still attached to the committing transaction, we must
 * we start writeout of all the dirty pages which are being truncated.
 * This way we are sure that all the data written in the previous
 * transaction are already on disk (truncate waits for pages under
 * writeback).
 *
 * Called with inode->i_rwsem down.
 */
int ext4_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int error, rc = 0;
	int orphan = 0;
	const unsigned int ia_valid = attr->ia_valid;
	bool inc_ivers = true;

	error = ext4_emergency_state(inode->i_sb);
	if (unlikely(error))
		return error;

	if (unlikely(IS_IMMUTABLE(inode)))
		return -EPERM;

	if (unlikely(IS_APPEND(inode) &&
		     (ia_valid & (ATTR_MODE | ATTR_UID |
				  ATTR_GID | ATTR_TIMES_SET))))
		return -EPERM;

	error = setattr_prepare(idmap, dentry, attr);
	if (error)
		return error;

	error = fscrypt_prepare_setattr(dentry, attr);
	if (error)
		return error;

	error = fsverity_prepare_setattr(dentry, attr);
	if (error)
		return error;

	if (is_quota_modification(idmap, inode, attr)) {
		error = dquot_initialize(inode);
		if (error)
			return error;
	}

	if (i_uid_needs_update(idmap, attr, inode) ||
	    i_gid_needs_update(idmap, attr, inode)) {
		handle_t *handle;

		/* (user+group)*(old+new) structure, inode write (sb,
		 * inode block, ? - but truncate inode update has it) */
		handle = ext4_journal_start(inode, EXT4_HT_QUOTA,
			(EXT4_MAXQUOTAS_INIT_BLOCKS(inode->i_sb) +
			 EXT4_MAXQUOTAS_DEL_BLOCKS(inode->i_sb)) + 3);
		if (IS_ERR(handle)) {
			error = PTR_ERR(handle);
			goto err_out;
		}

		/* dquot_transfer() calls back ext4_get_inode_usage() which
		 * counts xattr inode references.
		 */
		down_read(&EXT4_I(inode)->xattr_sem);
		error = dquot_transfer(idmap, inode, attr);
		up_read(&EXT4_I(inode)->xattr_sem);

		if (error) {
			ext4_journal_stop(handle);
			return error;
		}
		/* Update corresponding info in inode so that everything is in
		 * one transaction */
		i_uid_update(idmap, attr, inode);
		i_gid_update(idmap, attr, inode);
		error = ext4_mark_inode_dirty(handle, inode);
		ext4_journal_stop(handle);
		if (unlikely(error)) {
			return error;
		}
	}

	if (attr->ia_valid & ATTR_SIZE) {
		handle_t *handle;
		loff_t oldsize = inode->i_size;
		loff_t old_disksize;
		int shrink = (attr->ia_size < inode->i_size);

		if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))) {
			struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

			if (attr->ia_size > sbi->s_bitmap_maxbytes) {
				return -EFBIG;
			}
		}
		if (!S_ISREG(inode->i_mode)) {
			return -EINVAL;
		}

		if (attr->ia_size == inode->i_size)
			inc_ivers = false;

		if (shrink) {
			if (ext4_should_order_data(inode)) {
				error = ext4_begin_ordered_truncate(inode,
							    attr->ia_size);
				if (error)
					goto err_out;
			}
			/*
			 * Blocks are going to be removed from the inode. Wait
			 * for dio in flight.
			 */
			inode_dio_wait(inode);
		}

		filemap_invalidate_lock(inode->i_mapping);

		rc = ext4_break_layouts(inode);
		if (rc) {
			filemap_invalidate_unlock(inode->i_mapping);
			goto err_out;
		}

		if (attr->ia_size != inode->i_size) {
			/* attach jbd2 jinode for EOF folio tail zeroing */
			if (attr->ia_size & (inode->i_sb->s_blocksize - 1) ||
			    oldsize & (inode->i_sb->s_blocksize - 1)) {
				error = ext4_inode_attach_jinode(inode);
				if (error)
					goto out_mmap_sem;
			}

			handle = ext4_journal_start(inode, EXT4_HT_INODE, 3);
			if (IS_ERR(handle)) {
				error = PTR_ERR(handle);
				goto out_mmap_sem;
			}
			if (ext4_handle_valid(handle) && shrink) {
				error = ext4_orphan_add(handle, inode);
				orphan = 1;
			}
			/*
			 * Update c/mtime and tail zero the EOF folio on
			 * truncate up. ext4_truncate() handles the shrink case
			 * below.
			 */
			if (!shrink) {
				inode_set_mtime_to_ts(inode,
						      inode_set_ctime_current(inode));
				if (oldsize & (inode->i_sb->s_blocksize - 1))
					ext4_block_truncate_page(handle,
							inode->i_mapping, oldsize);
			}

			if (shrink)
				ext4_fc_track_range(handle, inode,
					(attr->ia_size > 0 ? attr->ia_size - 1 : 0) >>
					inode->i_sb->s_blocksize_bits,
					EXT_MAX_BLOCKS - 1);
			else
				ext4_fc_track_range(
					handle, inode,
					(oldsize > 0 ? oldsize - 1 : oldsize) >>
					inode->i_sb->s_blocksize_bits,
					(attr->ia_size > 0 ? attr->ia_size - 1 : 0) >>
					inode->i_sb->s_blocksize_bits);

			down_write(&EXT4_I(inode)->i_data_sem);
			old_disksize = EXT4_I(inode)->i_disksize;
			EXT4_I(inode)->i_disksize = attr->ia_size;

			/*
			 * We have to update i_size under i_data_sem together
			 * with i_disksize to avoid races with writeback code
			 * running ext4_wb_update_i_disksize().
			 */
			if (!error)
				i_size_write(inode, attr->ia_size);
			else
				EXT4_I(inode)->i_disksize = old_disksize;
			up_write(&EXT4_I(inode)->i_data_sem);
			rc = ext4_mark_inode_dirty(handle, inode);
			if (!error)
				error = rc;
			ext4_journal_stop(handle);
			if (error)
				goto out_mmap_sem;
			if (!shrink) {
				pagecache_isize_extended(inode, oldsize,
							 inode->i_size);
			} else if (ext4_should_journal_data(inode)) {
				ext4_wait_for_tail_page_commit(inode);
			}
		}

		/*
		 * Truncate pagecache after we've waited for commit
		 * in data=journal mode to make pages freeable.
		 */
		truncate_pagecache(inode, inode->i_size);
		/*
		 * Call ext4_truncate() even if i_size didn't change to
		 * truncate possible preallocated blocks.
		 */
		if (attr->ia_size <= oldsize) {
			rc = ext4_truncate(inode);
			if (rc)
				error = rc;
		}
out_mmap_sem:
		filemap_invalidate_unlock(inode->i_mapping);
	}

	if (!error) {
		if (inc_ivers)
			inode_inc_iversion(inode);
		setattr_copy(idmap, inode, attr);
		mark_inode_dirty(inode);
	}

	/*
	 * If the call to ext4_truncate failed to get a transaction handle at
	 * all, we need to clean up the in-core orphan list manually.
	 */
	if (orphan && inode->i_nlink)
		ext4_orphan_del(NULL, inode);

	if (!error && (ia_valid & ATTR_MODE))
		rc = posix_acl_chmod(idmap, dentry, inode->i_mode);

err_out:
	if  (error)
		ext4_std_error(inode->i_sb, error);
	if (!error)
		error = rc;
	return error;
}

u32 ext4_dio_alignment(struct inode *inode)
{
	if (fsverity_active(inode))
		return 0;
	if (ext4_should_journal_data(inode))
		return 0;
	if (ext4_has_inline_data(inode))
		return 0;
	if (IS_ENCRYPTED(inode)) {
		if (!fscrypt_dio_supported(inode))
			return 0;
		return i_blocksize(inode);
	}
	return 1; /* use the iomap defaults */
}

int ext4_getattr(struct mnt_idmap *idmap, const struct path *path,
		 struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct ext4_inode *raw_inode;
	struct ext4_inode_info *ei = EXT4_I(inode);
	unsigned int flags;

	if ((request_mask & STATX_BTIME) &&
	    EXT4_FITS_IN_INODE(raw_inode, ei, i_crtime)) {
		stat->result_mask |= STATX_BTIME;
		stat->btime.tv_sec = ei->i_crtime.tv_sec;
		stat->btime.tv_nsec = ei->i_crtime.tv_nsec;
	}

	/*
	 * Return the DIO alignment restrictions if requested.  We only return
	 * this information when requested, since on encrypted files it might
	 * take a fair bit of work to get if the file wasn't opened recently.
	 */
	if ((request_mask & STATX_DIOALIGN) && S_ISREG(inode->i_mode)) {
		u32 dio_align = ext4_dio_alignment(inode);

		stat->result_mask |= STATX_DIOALIGN;
		if (dio_align == 1) {
			struct block_device *bdev = inode->i_sb->s_bdev;

			/* iomap defaults */
			stat->dio_mem_align = bdev_dma_alignment(bdev) + 1;
			stat->dio_offset_align = bdev_logical_block_size(bdev);
		} else {
			stat->dio_mem_align = dio_align;
			stat->dio_offset_align = dio_align;
		}
	}

	if ((request_mask & STATX_WRITE_ATOMIC) && S_ISREG(inode->i_mode)) {
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
		unsigned int awu_min = 0, awu_max = 0;

		if (ext4_inode_can_atomic_write(inode)) {
			awu_min = sbi->s_awu_min;
			awu_max = sbi->s_awu_max;
		}

		generic_fill_statx_atomic_writes(stat, awu_min, awu_max, 0);
	}

	flags = ei->i_flags & EXT4_FL_USER_VISIBLE;
	if (flags & EXT4_APPEND_FL)
		stat->attributes |= STATX_ATTR_APPEND;
	if (flags & EXT4_COMPR_FL)
		stat->attributes |= STATX_ATTR_COMPRESSED;
	if (flags & EXT4_ENCRYPT_FL)
		stat->attributes |= STATX_ATTR_ENCRYPTED;
	if (flags & EXT4_IMMUTABLE_FL)
		stat->attributes |= STATX_ATTR_IMMUTABLE;
	if (flags & EXT4_NODUMP_FL)
		stat->attributes |= STATX_ATTR_NODUMP;
	if (flags & EXT4_VERITY_FL)
		stat->attributes |= STATX_ATTR_VERITY;

	stat->attributes_mask |= (STATX_ATTR_APPEND |
				  STATX_ATTR_COMPRESSED |
				  STATX_ATTR_ENCRYPTED |
				  STATX_ATTR_IMMUTABLE |
				  STATX_ATTR_NODUMP |
				  STATX_ATTR_VERITY);

	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

int ext4_file_getattr(struct mnt_idmap *idmap,
		      const struct path *path, struct kstat *stat,
		      u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	u64 delalloc_blocks;

	ext4_getattr(idmap, path, stat, request_mask, query_flags);

	/*
	 * If there is inline data in the inode, the inode will normally not
	 * have data blocks allocated (it may have an external xattr block).
	 * Report at least one sector for such files, so tools like tar, rsync,
	 * others don't incorrectly think the file is completely sparse.
	 */
	if (unlikely(ext4_has_inline_data(inode)))
		stat->blocks += (stat->size + 511) >> 9;

	/*
	 * We can't update i_blocks if the block allocation is delayed
	 * otherwise in the case of system crash before the real block
	 * allocation is done, we will have i_blocks inconsistent with
	 * on-disk file blocks.
	 * We always keep i_blocks updated together with real
	 * allocation. But to not confuse with user, stat
	 * will return the blocks that include the delayed allocation
	 * blocks for this file.
	 */
	delalloc_blocks = EXT4_C2B(EXT4_SB(inode->i_sb),
				   EXT4_I(inode)->i_reserved_data_blocks);
	stat->blocks += delalloc_blocks << (inode->i_sb->s_blocksize_bits - 9);
	return 0;
}

static int ext4_index_trans_blocks(struct inode *inode, int lblocks,
				   int pextents)
{
	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)))
		return ext4_ind_trans_blocks(inode, lblocks);
	return ext4_ext_index_trans_blocks(inode, pextents);
}

/*
 * Account for index blocks, block groups bitmaps and block group
 * descriptor blocks if modify datablocks and index blocks
 * worse case, the indexs blocks spread over different block groups
 *
 * If datablocks are discontiguous, they are possible to spread over
 * different block groups too. If they are contiguous, with flexbg,
 * they could still across block group boundary.
 *
 * Also account for superblock, inode, quota and xattr blocks
 */
int ext4_meta_trans_blocks(struct inode *inode, int lblocks, int pextents)
{
	ext4_group_t groups, ngroups = ext4_get_groups_count(inode->i_sb);
	int gdpblocks;
	int idxblocks;
	int ret;

	/*
	 * How many index and leaf blocks need to touch to map @lblocks
	 * logical blocks to @pextents physical extents?
	 */
	idxblocks = ext4_index_trans_blocks(inode, lblocks, pextents);

	/*
	 * Now let's see how many group bitmaps and group descriptors need
	 * to account
	 */
	groups = idxblocks + pextents;
	gdpblocks = groups;
	if (groups > ngroups)
		groups = ngroups;
	if (groups > EXT4_SB(inode->i_sb)->s_gdb_count)
		gdpblocks = EXT4_SB(inode->i_sb)->s_gdb_count;

	/* bitmaps and block group descriptor blocks */
	ret = idxblocks + groups + gdpblocks;

	/* Blocks for super block, inode, quota and xattr blocks */
	ret += EXT4_META_TRANS_BLOCKS(inode->i_sb);

	return ret;
}

/*
 * Calculate the journal credits for modifying the number of blocks
 * in a single extent within one transaction. 'nrblocks' is used only
 * for non-extent inodes. For extent type inodes, 'nrblocks' can be
 * zero if the exact number of blocks is unknown.
 */
int ext4_chunk_trans_extent(struct inode *inode, int nrblocks)
{
	int ret;

	ret = ext4_meta_trans_blocks(inode, nrblocks, 1);
	/* Account for data blocks for journalled mode */
	if (ext4_should_journal_data(inode))
		ret += nrblocks;
	return ret;
}

/*
 * Calculate the journal credits for a chunk of data modification.
 *
 * This is called from DIO, fallocate or whoever calling
 * ext4_map_blocks() to map/allocate a chunk of contiguous disk blocks.
 *
 * journal buffers for data blocks are not included here, as DIO
 * and fallocate do no need to journal data buffers.
 */
int ext4_chunk_trans_blocks(struct inode *inode, int nrblocks)
{
	return ext4_meta_trans_blocks(inode, nrblocks, 1);
}

/*
 * The caller must have previously called ext4_reserve_inode_write().
 * Give this, we know that the caller already has write access to iloc->bh.
 */
int ext4_mark_iloc_dirty(handle_t *handle,
			 struct inode *inode, struct ext4_iloc *iloc)
{
	int err = 0;

	err = ext4_emergency_state(inode->i_sb);
	if (unlikely(err)) {
		put_bh(iloc->bh);
		return err;
	}
	ext4_fc_track_inode(handle, inode);

	/* the do_update_inode consumes one bh->b_count */
	get_bh(iloc->bh);

	/* ext4_do_update_inode() does jbd2_journal_dirty_metadata */
	err = ext4_do_update_inode(handle, inode, iloc);
	put_bh(iloc->bh);
	return err;
}

/*
 * On success, We end up with an outstanding reference count against
 * iloc->bh.  This _must_ be cleaned up later.
 */

int
ext4_reserve_inode_write(handle_t *handle, struct inode *inode,
			 struct ext4_iloc *iloc)
{
	int err;

	err = ext4_emergency_state(inode->i_sb);
	if (unlikely(err))
		return err;

	err = ext4_get_inode_loc(inode, iloc);
	if (!err) {
		BUFFER_TRACE(iloc->bh, "get_write_access");
		err = ext4_journal_get_write_access(handle, inode->i_sb,
						    iloc->bh, EXT4_JTR_NONE);
		if (err) {
			brelse(iloc->bh);
			iloc->bh = NULL;
		}
		ext4_fc_track_inode(handle, inode);
	}
	ext4_std_error(inode->i_sb, err);
	return err;
}

static int __ext4_expand_extra_isize(struct inode *inode,
				     unsigned int new_extra_isize,
				     struct ext4_iloc *iloc,
				     handle_t *handle, int *no_expand)
{
	struct ext4_inode *raw_inode;
	struct ext4_xattr_ibody_header *header;
	unsigned int inode_size = EXT4_INODE_SIZE(inode->i_sb);
	struct ext4_inode_info *ei = EXT4_I(inode);
	int error;

	/* this was checked at iget time, but double check for good measure */
	if ((EXT4_GOOD_OLD_INODE_SIZE + ei->i_extra_isize > inode_size) ||
	    (ei->i_extra_isize & 3)) {
		EXT4_ERROR_INODE(inode, "bad extra_isize %u (inode size %u)",
				 ei->i_extra_isize,
				 EXT4_INODE_SIZE(inode->i_sb));
		return -EFSCORRUPTED;
	}
	if ((new_extra_isize < ei->i_extra_isize) ||
	    (new_extra_isize < 4) ||
	    (new_extra_isize > inode_size - EXT4_GOOD_OLD_INODE_SIZE))
		return -EINVAL;	/* Should never happen */

	raw_inode = ext4_raw_inode(iloc);

	header = IHDR(inode, raw_inode);

	/* No extended attributes present */
	if (!ext4_test_inode_state(inode, EXT4_STATE_XATTR) ||
	    header->h_magic != cpu_to_le32(EXT4_XATTR_MAGIC)) {
		memset((void *)raw_inode + EXT4_GOOD_OLD_INODE_SIZE +
		       EXT4_I(inode)->i_extra_isize, 0,
		       new_extra_isize - EXT4_I(inode)->i_extra_isize);
		EXT4_I(inode)->i_extra_isize = new_extra_isize;
		return 0;
	}

	/*
	 * We may need to allocate external xattr block so we need quotas
	 * initialized. Here we can be called with various locks held so we
	 * cannot affort to initialize quotas ourselves. So just bail.
	 */
	if (dquot_initialize_needed(inode))
		return -EAGAIN;

	/* try to expand with EAs present */
	error = ext4_expand_extra_isize_ea(inode, new_extra_isize,
					   raw_inode, handle);
	if (error) {
		/*
		 * Inode size expansion failed; don't try again
		 */
		*no_expand = 1;
	}

	return error;
}

/*
 * Expand an inode by new_extra_isize bytes.
 * Returns 0 on success or negative error number on failure.
 */
static int ext4_try_to_expand_extra_isize(struct inode *inode,
					  unsigned int new_extra_isize,
					  struct ext4_iloc iloc,
					  handle_t *handle)
{
	int no_expand;
	int error;

	if (ext4_test_inode_state(inode, EXT4_STATE_NO_EXPAND))
		return -EOVERFLOW;

	/*
	 * In nojournal mode, we can immediately attempt to expand
	 * the inode.  When journaled, we first need to obtain extra
	 * buffer credits since we may write into the EA block
	 * with this same handle. If journal_extend fails, then it will
	 * only result in a minor loss of functionality for that inode.
	 * If this is felt to be critical, then e2fsck should be run to
	 * force a large enough s_min_extra_isize.
	 */
	if (ext4_journal_extend(handle,
				EXT4_DATA_TRANS_BLOCKS(inode->i_sb), 0) != 0)
		return -ENOSPC;

	if (ext4_write_trylock_xattr(inode, &no_expand) == 0)
		return -EBUSY;

	error = __ext4_expand_extra_isize(inode, new_extra_isize, &iloc,
					  handle, &no_expand);
	ext4_write_unlock_xattr(inode, &no_expand);

	return error;
}

int ext4_expand_extra_isize(struct inode *inode,
			    unsigned int new_extra_isize,
			    struct ext4_iloc *iloc)
{
	handle_t *handle;
	int no_expand;
	int error, rc;

	if (ext4_test_inode_state(inode, EXT4_STATE_NO_EXPAND)) {
		brelse(iloc->bh);
		return -EOVERFLOW;
	}

	handle = ext4_journal_start(inode, EXT4_HT_INODE,
				    EXT4_DATA_TRANS_BLOCKS(inode->i_sb));
	if (IS_ERR(handle)) {
		error = PTR_ERR(handle);
		brelse(iloc->bh);
		return error;
	}

	ext4_write_lock_xattr(inode, &no_expand);

	BUFFER_TRACE(iloc->bh, "get_write_access");
	error = ext4_journal_get_write_access(handle, inode->i_sb, iloc->bh,
					      EXT4_JTR_NONE);
	if (error) {
		brelse(iloc->bh);
		goto out_unlock;
	}

	error = __ext4_expand_extra_isize(inode, new_extra_isize, iloc,
					  handle, &no_expand);

	rc = ext4_mark_iloc_dirty(handle, inode, iloc);
	if (!error)
		error = rc;

out_unlock:
	ext4_write_unlock_xattr(inode, &no_expand);
	ext4_journal_stop(handle);
	return error;
}

/*
 * What we do here is to mark the in-core inode as clean with respect to inode
 * dirtiness (it may still be data-dirty).
 * This means that the in-core inode may be reaped by prune_icache
 * without having to perform any I/O.  This is a very good thing,
 * because *any* task may call prune_icache - even ones which
 * have a transaction open against a different journal.
 *
 * Is this cheating?  Not really.  Sure, we haven't written the
 * inode out, but prune_icache isn't a user-visible syncing function.
 * Whenever the user wants stuff synced (sys_sync, sys_msync, sys_fsync)
 * we start and wait on commits.
 */
int __ext4_mark_inode_dirty(handle_t *handle, struct inode *inode,
				const char *func, unsigned int line)
{
	struct ext4_iloc iloc;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	int err;

	might_sleep();
	trace_ext4_mark_inode_dirty(inode, _RET_IP_);
	err = ext4_reserve_inode_write(handle, inode, &iloc);
	if (err)
		goto out;

	if (EXT4_I(inode)->i_extra_isize < sbi->s_want_extra_isize)
		ext4_try_to_expand_extra_isize(inode, sbi->s_want_extra_isize,
					       iloc, handle);

	err = ext4_mark_iloc_dirty(handle, inode, &iloc);
out:
	if (unlikely(err))
		ext4_error_inode_err(inode, func, line, 0, err,
					"mark_inode_dirty error");
	return err;
}

/*
 * ext4_dirty_inode() is called from __mark_inode_dirty()
 *
 * We're really interested in the case where a file is being extended.
 * i_size has been changed by generic_commit_write() and we thus need
 * to include the updated inode in the current transaction.
 *
 * Also, dquot_alloc_block() will always dirty the inode when blocks
 * are allocated to the file.
 *
 * If the inode is marked synchronous, we don't honour that here - doing
 * so would cause a commit on atime updates, which we don't bother doing.
 * We handle synchronous inodes at the highest possible level.
 */
void ext4_dirty_inode(struct inode *inode, int flags)
{
	handle_t *handle;

	handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
	if (IS_ERR(handle))
		return;
	ext4_mark_inode_dirty(handle, inode);
	ext4_journal_stop(handle);
}

int ext4_change_inode_journal_flag(struct inode *inode, int val)
{
	journal_t *journal;
	handle_t *handle;
	int err;
	int alloc_ctx;

	/*
	 * We have to be very careful here: changing a data block's
	 * journaling status dynamically is dangerous.  If we write a
	 * data block to the journal, change the status and then delete
	 * that block, we risk forgetting to revoke the old log record
	 * from the journal and so a subsequent replay can corrupt data.
	 * So, first we make sure that the journal is empty and that
	 * nobody is changing anything.
	 */

	journal = EXT4_JOURNAL(inode);
	if (!journal)
		return 0;
	if (is_journal_aborted(journal))
		return -EROFS;

	/* Wait for all existing dio workers */
	inode_dio_wait(inode);

	/*
	 * Before flushing the journal and switching inode's aops, we have
	 * to flush all dirty data the inode has. There can be outstanding
	 * delayed allocations, there can be unwritten extents created by
	 * fallocate or buffered writes in dioread_nolock mode covered by
	 * dirty data which can be converted only after flushing the dirty
	 * data (and journalled aops don't know how to handle these cases).
	 */
	if (val) {
		filemap_invalidate_lock(inode->i_mapping);
		err = filemap_write_and_wait(inode->i_mapping);
		if (err < 0) {
			filemap_invalidate_unlock(inode->i_mapping);
			return err;
		}
	}

	alloc_ctx = ext4_writepages_down_write(inode->i_sb);
	jbd2_journal_lock_updates(journal);

	/*
	 * OK, there are no updates running now, and all cached data is
	 * synced to disk.  We are now in a completely consistent state
	 * which doesn't have anything in the journal, and we know that
	 * no filesystem updates are running, so it is safe to modify
	 * the inode's in-core data-journaling state flag now.
	 */

	if (val)
		ext4_set_inode_flag(inode, EXT4_INODE_JOURNAL_DATA);
	else {
		err = jbd2_journal_flush(journal, 0);
		if (err < 0) {
			jbd2_journal_unlock_updates(journal);
			ext4_writepages_up_write(inode->i_sb, alloc_ctx);
			return err;
		}
		ext4_clear_inode_flag(inode, EXT4_INODE_JOURNAL_DATA);
	}
	ext4_set_aops(inode);

	jbd2_journal_unlock_updates(journal);
	ext4_writepages_up_write(inode->i_sb, alloc_ctx);

	if (val)
		filemap_invalidate_unlock(inode->i_mapping);

	/* Finally we can mark the inode as dirty. */

	handle = ext4_journal_start(inode, EXT4_HT_INODE, 1);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	ext4_fc_mark_ineligible(inode->i_sb,
		EXT4_FC_REASON_JOURNAL_FLAG_CHANGE, handle);
	err = ext4_mark_inode_dirty(handle, inode);
	ext4_handle_sync(handle);
	ext4_journal_stop(handle);
	ext4_std_error(inode->i_sb, err);

	return err;
}

static int ext4_bh_unmapped(handle_t *handle, struct inode *inode,
			    struct buffer_head *bh)
{
	return !buffer_mapped(bh);
}

static int ext4_block_page_mkwrite(struct inode *inode, struct folio *folio,
				   get_block_t get_block)
{
	handle_t *handle;
	loff_t size;
	unsigned long len;
	int credits;
	int ret;

	credits = ext4_chunk_trans_extent(inode,
			ext4_journal_blocks_per_folio(inode));
	handle = ext4_journal_start(inode, EXT4_HT_WRITE_PAGE, credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	folio_lock(folio);
	size = i_size_read(inode);
	/* Page got truncated from under us? */
	if (folio->mapping != inode->i_mapping || folio_pos(folio) > size) {
		ret = -EFAULT;
		goto out_error;
	}

	len = folio_size(folio);
	if (folio_pos(folio) + len > size)
		len = size - folio_pos(folio);

	ret = ext4_block_write_begin(handle, folio, 0, len, get_block);
	if (ret)
		goto out_error;

	if (!ext4_should_journal_data(inode)) {
		block_commit_write(folio, 0, len);
		folio_mark_dirty(folio);
	} else {
		ret = ext4_journal_folio_buffers(handle, folio, len);
		if (ret)
			goto out_error;
	}
	ext4_journal_stop(handle);
	folio_wait_stable(folio);
	return ret;

out_error:
	folio_unlock(folio);
	ext4_journal_stop(handle);
	return ret;
}

vm_fault_t ext4_page_mkwrite(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct folio *folio = page_folio(vmf->page);
	loff_t size;
	unsigned long len;
	int err;
	vm_fault_t ret;
	struct file *file = vma->vm_file;
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	get_block_t *get_block = ext4_get_block;
	int retries = 0;

	if (unlikely(IS_IMMUTABLE(inode)))
		return VM_FAULT_SIGBUS;

	sb_start_pagefault(inode->i_sb);
	file_update_time(vma->vm_file);

	filemap_invalidate_lock_shared(mapping);

	err = ext4_convert_inline_data(inode);
	if (err)
		goto out_ret;

	/*
	 * On data journalling we skip straight to the transaction handle:
	 * there's no delalloc; page truncated will be checked later; the
	 * early return w/ all buffers mapped (calculates size/len) can't
	 * be used; and there's no dioread_nolock, so only ext4_get_block.
	 */
	if (ext4_should_journal_data(inode))
		goto retry_alloc;

	/* Delalloc case is easy... */
	if (test_opt(inode->i_sb, DELALLOC) &&
	    !ext4_nonda_switch(inode->i_sb)) {
		do {
			err = block_page_mkwrite(vma, vmf,
						   ext4_da_get_block_prep);
		} while (err == -ENOSPC &&
		       ext4_should_retry_alloc(inode->i_sb, &retries));
		goto out_ret;
	}

	folio_lock(folio);
	size = i_size_read(inode);
	/* Page got truncated from under us? */
	if (folio->mapping != mapping || folio_pos(folio) > size) {
		folio_unlock(folio);
		ret = VM_FAULT_NOPAGE;
		goto out;
	}

	len = folio_size(folio);
	if (folio_pos(folio) + len > size)
		len = size - folio_pos(folio);
	/*
	 * Return if we have all the buffers mapped. This avoids the need to do
	 * journal_start/journal_stop which can block and take a long time
	 *
	 * This cannot be done for data journalling, as we have to add the
	 * inode to the transaction's list to writeprotect pages on commit.
	 */
	if (folio_buffers(folio)) {
		if (!ext4_walk_page_buffers(NULL, inode, folio_buffers(folio),
					    0, len, NULL,
					    ext4_bh_unmapped)) {
			/* Wait so that we don't change page under IO */
			folio_wait_stable(folio);
			ret = VM_FAULT_LOCKED;
			goto out;
		}
	}
	folio_unlock(folio);
	/* OK, we need to fill the hole... */
	if (ext4_should_dioread_nolock(inode))
		get_block = ext4_get_block_unwritten;
retry_alloc:
	/* Start journal and allocate blocks */
	err = ext4_block_page_mkwrite(inode, folio, get_block);
	if (err == -EAGAIN ||
	    (err == -ENOSPC && ext4_should_retry_alloc(inode->i_sb, &retries)))
		goto retry_alloc;
out_ret:
	ret = vmf_fs_error(err);
out:
	filemap_invalidate_unlock_shared(mapping);
	sb_end_pagefault(inode->i_sb);
	return ret;
}
