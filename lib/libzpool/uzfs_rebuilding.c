/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

#include <sys/dmu_objset.h>
#include <sys/uzfs_zvol.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_destroy.h>
#include <sys/dmu_tx.h>
#include <uzfs_io.h>
#include <uzfs_mgmt.h>
#include <uzfs_rebuilding.h>
#include <zrepl_mgmt.h>

#define	ADD_TO_IO_CHUNK_LIST(list, e_offset, e_len, count)		\
	do {    							\
		uzfs_io_chunk_list_t  *node;				\
		node = umem_alloc(sizeof (*node), UMEM_NOFAIL);         \
		node->offset = e_offset;                                \
		node->len = e_len;                                      \
		list_insert_tail(list, node);                           \
		count++;                                                \
	} while (0)

int
compare_blk_metadata(blk_metadata_t *first, blk_metadata_t *second)
{
	if (first->io_num < second->io_num)
		return (-1);
	if (first->io_num == second->io_num)
		return (0);
	return (1);
}

boolean_t
iszero(blk_metadata_t *md)
{
	if (md->io_num == 0)
		return (B_TRUE);
	return (B_FALSE);
}

#define	EXECUTE_DIFF_CALLBACK(last_lun_offset, diff_count, buf, 	\
    last_index, arg, last_md, zv, func, ret)				\
		do {							\
			ret = func(last_lun_offset, diff_count * 	\
			    zv->zv_metavolblocksize, (blk_metadata_t *) \
			    (buf + last_index), zv, arg);		\
			diff_count = 0;					\
			last_index = 0;					\
			last_md = NULL;					\
		} while (0)

int
get_snapshot_zv(zvol_state_t *zv, char *snap_name, zvol_state_t **snap_zv,
    boolean_t fail_exists, boolean_t fail_notexists)
{
	char *dataset;
	int ret = 0;

	dataset = kmem_asprintf("%s@%s", strchr(zv->zv_name, '/') + 1,
	    snap_name);

	ret = uzfs_open_dataset(zv->zv_spa, dataset, snap_zv);
	if (ret == ENOENT) {
		if (fail_notexists) {
			LOG_ERR("fail on unavailable snapshot %s",
			    dataset);
			strfree(dataset);
			ret = SET_ERROR(ENOENT);
			return (ret);
		}
		ret = dmu_objset_snapshot_one(zv->zv_name, snap_name);
		if (ret) {
			LOG_ERR("Failed to create snapshot %s: %d",
			    dataset, ret);
			strfree(dataset);
			return (ret);
		}

		ret = uzfs_open_dataset(zv->zv_spa, dataset, snap_zv);
		if (ret == 0) {
			ret = uzfs_hold_dataset(*snap_zv);
			if (ret != 0) {
				LOG_ERR("Failed to hold snapshot: %s %d",
				    dataset, ret);
				uzfs_close_dataset(*snap_zv);
				*snap_zv = NULL;
			}
		}
		else
			LOG_ERR("Failed to open snapshot: %d", ret);
	} else if (ret == 0) {
		if (fail_exists) {
			LOG_ERR("fail on already available snapshot %s",
			    dataset);
			uzfs_close_dataset(*snap_zv);
			*snap_zv = NULL;
			ret = SET_ERROR(EEXIST);
		} else {
			LOG_INFO("holding already available snapshot %s",
			    dataset);
			ret = uzfs_hold_dataset(*snap_zv);
			if (ret != 0) {
				LOG_ERR("Failed to hold already existing "
				    "snapshot %s: %d", dataset, ret);
				uzfs_close_dataset(*snap_zv);
				*snap_zv = NULL;
			}
		}
	} else {
		LOG_ERR("Failed to open snapshot: %d", ret);
	}

	strfree(dataset);
	return (ret);
}

/*
 * Creates internal snapshot on given zv using current time as part of snapname.
 * If snap already exists, waits and attempts to create again.
 * Returns zv of snapshot in snap_zv.
 */
int
uzfs_zvol_create_internal_snapshot(zvol_state_t *zv, zvol_state_t **snap_zv,
    uint64_t io_num, char **snap)
{
	int ret = 0;
	char *snap_name;
	time_t now;
again:
	now = time(NULL);
	snap_name = kmem_asprintf("%s%llu.%llu", IO_DIFF_SNAPNAME, io_num, now);
	ret = get_snapshot_zv(zv, snap_name, snap_zv, B_TRUE, B_FALSE);
	if (ret == EEXIST) {
		strfree(snap_name);
		sleep(1);
		LOG_INFO("Waiting to create internal snapshot");
		goto again;
	}
	if (ret != 0) {
		LOG_ERR("Failed to get info about %s@%s",
		    zv->zv_name, snap_name);
		strfree(snap_name);
	} else {
		*snap = snap_name;
	}
	return (ret);
}


void
destroy_snapshot_zv(zvol_state_t *zv, char *snap_name)
{
	char *dataset;

	dataset = kmem_asprintf("%s@%s", zv->zv_name, snap_name);
	(void) dsl_destroy_snapshot(dataset, B_FALSE);
	strfree(dataset);
}

int
uzfs_get_io_diff(zvol_state_t *zv, blk_metadata_t *low,
    uzfs_get_io_diff_cb_t *func, off_t lun_offset, size_t lun_len, void *arg)
{
	uint64_t blocksize = zv->zv_volmetablocksize;
	uint64_t metadata_read_chunk_size = 10 * blocksize;
	uint64_t metaobjectsize = (zv->zv_volsize / zv->zv_metavolblocksize) *
	    zv->zv_volmetadatasize;
	uint64_t metadatasize = zv->zv_volmetadatasize;
	char *buf, *snap_name = NULL;
	uint64_t i, read;
	uint64_t offset, len, end;
	int ret = 0;
	int diff_count = 0, last_index = 0;
	uint64_t last_lun_offset = 0;
	blk_metadata_t *last_md;
	zvol_state_t *snap_zv;
	metaobj_blk_offset_t snap_metablk;

	if (!func || (lun_offset + lun_len) > zv->zv_volsize)
		return (EINVAL);

	get_zv_metaobj_block_details(&snap_metablk, zv, lun_offset, lun_len);
	offset = snap_metablk.m_offset;
	end = snap_metablk.m_offset + snap_metablk.m_len;

	if (end > metaobjectsize)
		end = metaobjectsize;

	ret = uzfs_zvol_create_internal_snapshot(zv, &snap_zv,
	    low->io_num, &snap_name);

	if (ret != 0) {
		LOG_ERR("Failed to create snapshot for vol %s io_num %lu",
		    zv->zv_name, low->io_num);
		return (ret);
	}

	metadata_read_chunk_size = (metadata_read_chunk_size / metadatasize) *
	    metadatasize;
	buf = umem_alloc(metadata_read_chunk_size, KM_SLEEP);
	len = metadata_read_chunk_size;

	for (; offset < end && !ret; offset += len) {
		read = 0;
		len = metadata_read_chunk_size;

		if ((offset + len) > end)
			len = (end - offset);

		ret = uzfs_read_metadata(snap_zv, buf, offset, len, &read);

		if (read != len || ret)
			break;

		lun_offset = (offset / metadatasize) * zv->zv_metavolblocksize;
		for (i = 0; i < len && !ret; i += sizeof (blk_metadata_t)) {
			if (!iszero((blk_metadata_t *)(buf+i)) &&
			    (compare_blk_metadata((blk_metadata_t *)(buf + i),
			    low) > 0)) {
				/*
				 * We will keep track of last lun_offset having
				 * metadata lesser than incoming_metadata and
				 * join adjacent chunk with the same on_disk
				 * io_number.
				 */
				if (diff_count == 0) {
					last_lun_offset = lun_offset;
					last_md = (blk_metadata_t *)(buf+i);
					last_index = i;
				}

				if (diff_count &&
				    compare_blk_metadata((blk_metadata_t *)
				    (buf + i), last_md) != 0) {
					/*
					 * Execute callback function with last
					 * metadata and diff_count if
					 * last compared metadata is changed
					 */
					EXECUTE_DIFF_CALLBACK(last_lun_offset,
					    diff_count, buf, last_index, arg,
					    last_md, snap_zv, func, ret);
					if (ret != 0)
						break;
					last_lun_offset = lun_offset;
					last_md = (blk_metadata_t *)(buf+i);
					last_index = i;
					diff_count++;
				} else {
					/*
					 * increament diff_count with 1 if
					 * metadata is same
					 */
					diff_count++;
				}
			} else if (diff_count) {
				EXECUTE_DIFF_CALLBACK(last_lun_offset,
				    diff_count, buf, last_index, arg, last_md,
				    snap_zv, func, ret);
				if (ret != 0)
					break;
			}

			lun_offset += zv->zv_metavolblocksize;
		}
		if (!ret && diff_count) {
			EXECUTE_DIFF_CALLBACK(last_lun_offset, diff_count, buf,
			    last_index, arg, last_md, snap_zv, func, ret);
			if (ret != 0)
				break;
		}
	}

	uzfs_close_dataset(snap_zv);

	/*
	 * TODO: if we failed to destroy snapshot here then
	 * this should be handled separately from application.
	 */
	if (end == metaobjectsize)
		destroy_snapshot_zv(zv, snap_name);

	umem_free(buf, metadata_read_chunk_size);
	strfree(snap_name);
	return (ret);
}

int
uzfs_get_nonoverlapping_ondisk_blks(zvol_state_t *zv, uint64_t offset,
    uint64_t len, blk_metadata_t *incoming_md, void **list)
{
	char *ondisk_metadata_buf;
	uint64_t rd_rlen;
	metaobj_blk_offset_t ondisk_metablk;
	blk_metadata_t *ondisk_md;
	int diff_count = 0;
	int count = 0;
	int ret = 0;
	int i = 0;
	uint64_t lun_offset = 0, last_lun_offset = 0;
	list_t *chunk_list = NULL;
	uint64_t metavolblocksize = zv->zv_metavolblocksize;
	uint64_t metadatasize = zv->zv_volmetadatasize;

	get_zv_metaobj_block_details(&ondisk_metablk, zv, offset, len);
	ondisk_metadata_buf = umem_alloc(ondisk_metablk.m_len, UMEM_NOFAIL);

	ret = uzfs_read_metadata(zv, ondisk_metadata_buf,
	    ondisk_metablk.m_offset, ondisk_metablk.m_len, &rd_rlen);
	if (ret || rd_rlen != ondisk_metablk.m_len) {
		LOG_ERR("Failed to read metadata");
		goto exit;
	}

	chunk_list = umem_alloc(sizeof (*chunk_list), UMEM_NOFAIL);
	list_create(chunk_list, sizeof (uzfs_io_chunk_list_t),
	    offsetof(uzfs_io_chunk_list_t, link));

	for (i = 0; i < ondisk_metablk.m_len; i += sizeof (blk_metadata_t)) {
		ondisk_md = (blk_metadata_t *)(ondisk_metadata_buf + i);
		lun_offset = ((ondisk_metablk.m_offset + i) *
		    metavolblocksize) / metadatasize;
		ret = compare_blk_metadata(ondisk_md, incoming_md);
		if (ret == -1) {
			// on_disk io number < incoming io number
			if (diff_count == 0)
				last_lun_offset = lun_offset;

			diff_count++;
		} else {
			// on_disk io number >= incoming io number
			if (diff_count != 0) {
				ADD_TO_IO_CHUNK_LIST(chunk_list,
				    last_lun_offset, diff_count *
				    metavolblocksize, count);
				diff_count = 0;
			}
		}
	}

	if (diff_count != 0)
		ADD_TO_IO_CHUNK_LIST(chunk_list, last_lun_offset,
		    diff_count * metavolblocksize, count);

exit:
	umem_free(ondisk_metadata_buf, ondisk_metablk.m_len);
	*list = chunk_list;
	return (count);
}

int
uzfs_zvol_destroy_snaprebuild_clone(zvol_state_t *zv,
    zvol_state_t *snap_zv)
{
	int ret = 0;
	char *clonename;

	clonename = kmem_asprintf("%s/%s_%s", spa_name(zv->zv_spa),
	    strchr(zv->zv_name, '/') + 1,
	    REBUILD_SNAPSHOT_CLONENAME);

	/* Close dataset */
	uzfs_close_dataset(snap_zv);

	/* Destroy clone */
	ret = dsl_destroy_head(clonename);

	/* Destroy snapshot */
	destroy_snapshot_zv(zv, REBUILD_SNAPSHOT_SNAPNAME);
	strfree(clonename);

	return (ret);
}

/*
 * Create snapshot and create clone from that snapshot
 */
int
uzfs_zvol_create_snaprebuild_clone(zvol_state_t *zv,
    zvol_state_t **snap_zv)
{
	int ret = 0;
	char *snapname = NULL;
	char *clonename = NULL;

	ret = get_snapshot_zv(zv, REBUILD_SNAPSHOT_SNAPNAME, snap_zv,
	    B_FALSE, B_FALSE);
	if (ret != 0) {
		LOG_ERR("Failed to get info about %s@%s",
		    zv->zv_name, REBUILD_SNAPSHOT_SNAPNAME);
		return (ret);
	}

	snapname = kmem_asprintf("%s@%s", zv->zv_name,
	    REBUILD_SNAPSHOT_SNAPNAME);

	clonename = kmem_asprintf("%s/%s_%s", spa_name(zv->zv_spa),
	    strchr(zv->zv_name, '/') + 1,
	    REBUILD_SNAPSHOT_CLONENAME);

	ret = dmu_objset_clone(clonename, snapname);
	if (ret == EEXIST) {
		LOG_INFO("Volume:%s already has clone for snap rebuild",
		    zv->zv_name);
	} else if (ret != 0) {
		uzfs_close_dataset(*snap_zv);
		destroy_snapshot_zv(zv, REBUILD_SNAPSHOT_SNAPNAME);
		*snap_zv = NULL;
	}

	strfree(snapname);
	strfree(clonename);
	return (ret);
}
