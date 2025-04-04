/*
 * Copyright (C) 2011 STRATO.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include "kerncompat.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "kernel-lib/list.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/backref.h"
#include "kernel-shared/ulist.h"
#include "kernel-shared/messages.h"
#include "kernel-shared/tree-checker.h"
#include "kernel-shared/uapi/btrfs.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "common/internal.h"

#define pr_debug(...) do { } while (0)

struct extent_inode_elem {
	u64 inum;
	u64 offset;
	struct extent_inode_elem *next;
};

static int check_extent_in_eb(struct btrfs_key *key, struct extent_buffer *eb,
				struct btrfs_file_extent_item *fi,
				u64 extent_item_pos,
				struct extent_inode_elem **eie)
{
	u64 offset = 0;
	struct extent_inode_elem *e;

	if (!btrfs_file_extent_compression(eb, fi) &&
	    !btrfs_file_extent_encryption(eb, fi) &&
	    !btrfs_file_extent_other_encoding(eb, fi)) {
		u64 data_offset;
		u64 data_len;

		data_offset = btrfs_file_extent_offset(eb, fi);
		data_len = btrfs_file_extent_num_bytes(eb, fi);

		if (extent_item_pos < data_offset ||
		    extent_item_pos >= data_offset + data_len)
			return 1;
		offset = extent_item_pos - data_offset;
	}

	e = kmalloc(sizeof(*e), GFP_NOFS);
	if (!e)
		return -ENOMEM;

	e->next = *eie;
	e->inum = key->objectid;
	e->offset = key->offset + offset;
	*eie = e;

	return 0;
}

static void free_inode_elem_list(struct extent_inode_elem *eie)
{
	struct extent_inode_elem *eie_next;

	for (; eie; eie = eie_next) {
		eie_next = eie->next;
		kfree(eie);
	}
}

static int find_extent_in_eb(struct extent_buffer *eb, u64 wanted_disk_byte,
				u64 extent_item_pos,
				struct extent_inode_elem **eie)
{
	u64 disk_byte;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	int slot;
	int nritems;
	int extent_type;
	int ret;

	/*
	 * from the shared data ref, we only have the leaf but we need
	 * the key. thus, we must look into all items and see that we
	 * find one (some) with a reference to our extent item.
	 */
	nritems = btrfs_header_nritems(eb);
	for (slot = 0; slot < nritems; ++slot) {
		btrfs_item_key_to_cpu(eb, &key, slot);
		if (key.type != BTRFS_EXTENT_DATA_KEY)
			continue;
		fi = btrfs_item_ptr(eb, slot, struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(eb, fi);
		if (extent_type == BTRFS_FILE_EXTENT_INLINE)
			continue;
		/* don't skip BTRFS_FILE_EXTENT_PREALLOC, we can handle that */
		disk_byte = btrfs_file_extent_disk_bytenr(eb, fi);
		if (disk_byte != wanted_disk_byte)
			continue;

		ret = check_extent_in_eb(&key, eb, fi, extent_item_pos, eie);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * this structure records all encountered refs on the way up to the root
 */
struct __prelim_ref {
	struct list_head list;
	u64 root_id;
	struct btrfs_key key_for_search;
	int level;
	int count;
	struct extent_inode_elem *inode_list;
	u64 parent;
	u64 wanted_disk_byte;
};

static struct __prelim_ref *list_first_pref(struct list_head *head)
{
	return list_first_entry(head, struct __prelim_ref, list);
}

struct pref_state {
	struct list_head pending;
	struct list_head pending_missing_keys;
	struct list_head pending_indirect_refs;
};

static void init_pref_state(struct pref_state *prefstate)
{
	INIT_LIST_HEAD(&prefstate->pending);
	INIT_LIST_HEAD(&prefstate->pending_missing_keys);
	INIT_LIST_HEAD(&prefstate->pending_indirect_refs);
}

/*
 * the rules for all callers of this function are:
 * - obtaining the parent is the goal
 * - if you add a key, you must know that it is a correct key
 * - if you cannot add the parent or a correct key, then we will look into the
 *   block later to set a correct key
 *
 * on disk refs (inline or keyed)
 * ==============================
 *        backref type | shared | indirect | shared | indirect
 * information         |   tree |     tree |   data |     data
 * --------------------+--------+----------+--------+----------
 *      parent logical |    y   |     -    |    y   |     -
 *      key to resolve |    -   |     -    |    -   |     y
 *  tree block logical |    y   |     y    |    y   |     y
 *  root for resolving |    -   |     y    |    y   |     y
 *
 * - column 1, 3: we've the parent -> done
 * - column 2:    we take the first key from the block to find the parent
 *                (see __add_missing_keys)
 * - column 4:    we use the key to find the parent
 *
 * additional information that's available but not required to find the parent
 * block might help in merging entries to gain some speed.
 */

static int __add_prelim_ref(struct pref_state *prefstate, u64 root_id,
			    struct btrfs_key *key, int level,
			    u64 parent, u64 wanted_disk_byte, int count,
			    gfp_t gfp_mask)
{
	struct list_head *head;
	struct __prelim_ref *ref;

	if (root_id == BTRFS_DATA_RELOC_TREE_OBJECTID)
		return 0;

	ref = kmalloc(sizeof(*ref), gfp_mask);
	if (!ref)
		return -ENOMEM;

	ref->root_id = root_id;
	if (key) {
		ref->key_for_search = *key;
		if (parent)
			head = &prefstate->pending;
		else
			head = &prefstate->pending_indirect_refs;
	} else if (parent) {
		memset(&ref->key_for_search, 0, sizeof(ref->key_for_search));
		head = &prefstate->pending;
	} else {
		memset(&ref->key_for_search, 0, sizeof(ref->key_for_search));
		head = &prefstate->pending_missing_keys;
	}

	ref->inode_list = NULL;
	ref->level = level;
	ref->count = count;
	ref->parent = parent;
	ref->wanted_disk_byte = wanted_disk_byte;

	list_add_tail(&ref->list, head);

	return 0;
}

static int add_all_parents(struct btrfs_root *root, struct btrfs_path *path,
			   struct ulist *parents, struct __prelim_ref *ref,
			   int level, u64 time_seq, const u64 *extent_item_pos,
			   u64 total_refs)
{
	int ret = 0;
	int slot;
	struct extent_buffer *eb;
	struct btrfs_key key;
	struct btrfs_key *key_for_search = &ref->key_for_search;
	struct btrfs_file_extent_item *fi;
	struct extent_inode_elem *eie = NULL, *old = NULL;
	u64 disk_byte;
	u64 wanted_disk_byte = ref->wanted_disk_byte;
	u64 count = 0;

	if (level != 0) {
		eb = path->nodes[level];
		ret = ulist_add(parents, eb->start, 0, GFP_NOFS);
		if (ret < 0)
			return ret;
		return 0;
	}

	/*
	 * We normally enter this function with the path already pointing to
	 * the first item to check. But sometimes, we may enter it with
	 * slot==nritems. In that case, go to the next leaf before we continue.
	 */
	if (path->slots[0] >= btrfs_header_nritems(path->nodes[0]))
		ret = btrfs_next_leaf(root, path);

	while (!ret && count < total_refs) {
		eb = path->nodes[0];
		slot = path->slots[0];

		btrfs_item_key_to_cpu(eb, &key, slot);

		if (key.objectid != key_for_search->objectid ||
		    key.type != BTRFS_EXTENT_DATA_KEY)
			break;

		fi = btrfs_item_ptr(eb, slot, struct btrfs_file_extent_item);
		disk_byte = btrfs_file_extent_disk_bytenr(eb, fi);

		if (disk_byte == wanted_disk_byte) {
			eie = NULL;
			old = NULL;
			count++;
			if (extent_item_pos) {
				ret = check_extent_in_eb(&key, eb, fi,
						*extent_item_pos,
						&eie);
				if (ret < 0)
					break;
			}
			if (ret > 0)
				goto next;
			ret = ulist_add_merge_ptr(parents, eb->start,
						  eie, (void **)&old, GFP_NOFS);
			if (ret < 0)
				break;
			if (!ret && extent_item_pos) {
				while (old->next)
					old = old->next;
				old->next = eie;
			}
			eie = NULL;
		}
next:
		ret = btrfs_next_item(root, path);
	}

	if (ret > 0)
		ret = 0;
	else if (ret < 0)
		free_inode_elem_list(eie);
	return ret;
}

/*
 * resolve an indirect backref in the form (root_id, key, level)
 * to a logical address
 */
static int __resolve_indirect_ref(struct btrfs_fs_info *fs_info,
				  struct btrfs_path *path, u64 time_seq,
				  struct __prelim_ref *ref,
				  struct ulist *parents,
				  const u64 *extent_item_pos, u64 total_refs)
{
	struct btrfs_root *root;
	struct btrfs_key root_key;
	struct extent_buffer *eb;
	int ret = 0;
	int root_level;
	int level = ref->level;

	root_key.objectid = ref->root_id;
	root_key.type = BTRFS_ROOT_ITEM_KEY;
	root_key.offset = (u64)-1;

	root = btrfs_read_fs_root(fs_info, &root_key);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto out;
	}

	root_level = btrfs_root_level(&root->root_item);

	if (root_level + 1 == level)
		goto out;

	path->lowest_level = level;
	ret = btrfs_search_slot(NULL, root, &ref->key_for_search, path, 0, 0);

	pr_debug("search slot in root %llu (level %d, ref count %d) returned "
		 "%d for key (%llu %u %llu)\n",
		 ref->root_id, level, ref->count, ret,
		 ref->key_for_search.objectid, ref->key_for_search.type,
		 ref->key_for_search.offset);
	if (ret < 0)
		goto out;

	eb = path->nodes[level];
	while (!eb) {
		if (!level) {
			ret = 1;
			WARN_ON(1);
			goto out;
		}
		level--;
		eb = path->nodes[level];
	}

	ret = add_all_parents(root, path, parents, ref, level, time_seq,
			      extent_item_pos, total_refs);
out:
	path->lowest_level = 0;
	btrfs_release_path(path);
	return ret;
}

/*
 * resolve all indirect backrefs from the list
 */
static int __resolve_indirect_refs(struct btrfs_fs_info *fs_info,
				   struct pref_state *prefstate,
				   struct btrfs_path *path, u64 time_seq,
				   const u64 *extent_item_pos, u64 total_refs)
{
	struct list_head *head = &prefstate->pending_indirect_refs;
	int err;
	int ret = 0;
	struct __prelim_ref *ref;
	struct __prelim_ref *new_ref;
	struct ulist *parents;
	struct ulist_node *node;
	struct ulist_iterator uiter;

	parents = ulist_alloc(GFP_NOFS);
	if (!parents)
		return -ENOMEM;

	while (!list_empty(head)) {
		ref = list_first_pref(head);
		list_move(&ref->list, &prefstate->pending);
		ASSERT(!ref->parent);	/* already direct */
		ASSERT(ref->count);
		err = __resolve_indirect_ref(fs_info, path, time_seq, ref,
					     parents, extent_item_pos,
					     total_refs);
		/*
		 * we can only tolerate ENOENT,otherwise,we should catch error
		 * and return directly.
		 */
		if (err == -ENOENT) {
			continue;
		} else if (err) {
			ret = err;
			goto out;
		}

		/* we put the first parent into the ref at hand */
		ULIST_ITER_INIT(&uiter);
		node = ulist_next(parents, &uiter);
		ref->parent = node ? node->val : 0;
		ref->inode_list = node ?
			(struct extent_inode_elem *)(uintptr_t)node->aux : NULL;

		/* additional parents require new refs being added here */
		while ((node = ulist_next(parents, &uiter))) {
			new_ref = kmalloc(sizeof(*new_ref), GFP_NOFS);
			if (!new_ref) {
				ret = -ENOMEM;
				goto out;
			}
			memcpy(new_ref, ref, sizeof(*ref));
			new_ref->parent = node->val;
			new_ref->inode_list = (struct extent_inode_elem *)
							(uintptr_t)node->aux;
			list_add_tail(&new_ref->list, &prefstate->pending);
		}
		ulist_reinit(parents);
	}
out:
	ulist_free(parents);
	return ret;
}

static inline int ref_for_same_block(struct __prelim_ref *ref1,
				     struct __prelim_ref *ref2)
{
	if (ref1->level != ref2->level)
		return 0;
	if (ref1->root_id != ref2->root_id)
		return 0;
	if (ref1->key_for_search.type != ref2->key_for_search.type)
		return 0;
	if (ref1->key_for_search.objectid != ref2->key_for_search.objectid)
		return 0;
	if (ref1->key_for_search.offset != ref2->key_for_search.offset)
		return 0;
	if (ref1->parent != ref2->parent)
		return 0;

	return 1;
}

/*
 * read tree blocks and add keys where required.
 */
static int __add_missing_keys(struct btrfs_fs_info *fs_info,
			      struct pref_state *prefstate)
{
	struct extent_buffer *eb;

	while (!list_empty(&prefstate->pending_missing_keys)) {
		struct __prelim_ref *ref;
		struct btrfs_tree_parent_check check;

		ref = list_first_pref(&prefstate->pending_missing_keys);

		ASSERT(ref->root_id);
		ASSERT(!ref->parent);
		ASSERT(!ref->key_for_search.type);
		BUG_ON(!ref->wanted_disk_byte);

		check.owner_root = ref->root_id;
		check.transid = 0;
		check.has_first_key = false;
		check.level = ref->level - 1;

		eb = read_tree_block(fs_info, ref->wanted_disk_byte, &check);
		if (!extent_buffer_uptodate(eb)) {
			free_extent_buffer(eb);
			return -EIO;
		}
		if (btrfs_header_level(eb) == 0)
			btrfs_item_key_to_cpu(eb, &ref->key_for_search, 0);
		else
			btrfs_node_key_to_cpu(eb, &ref->key_for_search, 0);
		free_extent_buffer(eb);
		if (ref->parent)
			list_move(&ref->list, &prefstate->pending);
		else
			list_move(&ref->list, &prefstate->pending_indirect_refs);
	}
	return 0;
}

/*
 * merge two lists of backrefs and adjust counts accordingly
 *
 * mode = 1: merge identical keys, if key is set
 *    FIXME: if we add more keys in __add_prelim_ref, we can merge more here.
 *           additionally, we could even add a key range for the blocks we
 *           looked into to merge even more (-> replace unresolved refs by those
 *           having a parent).
 * mode = 2: merge identical parents
 */
static void __merge_refs(struct pref_state *prefstate, int mode)
{
	struct list_head *head = &prefstate->pending;
	struct list_head *pos1;

	list_for_each(pos1, head) {
		struct list_head *n2;
		struct list_head *pos2;
		struct __prelim_ref *ref1;

		ref1 = list_entry(pos1, struct __prelim_ref, list);

		for (pos2 = pos1->next, n2 = pos2->next; pos2 != head;
		     pos2 = n2, n2 = pos2->next) {
			struct __prelim_ref *ref2;
			struct extent_inode_elem *eie;

			ref2 = list_entry(pos2, struct __prelim_ref, list);

			if (mode == 1) {
				if (!ref_for_same_block(ref1, ref2))
					continue;
			} else {
				/*
				 * Parent == 0 means that the ref is tree block
				 * backref or its parent is unresolved.
				 */
				if (!ref1->parent || !ref2->parent)
					continue;
				if (ref1->parent != ref2->parent)
					continue;
			}

			eie = ref1->inode_list;
			while (eie && eie->next)
				eie = eie->next;
			if (eie)
				eie->next = ref2->inode_list;
			else
				ref1->inode_list = ref2->inode_list;
			ref1->count += ref2->count;

			list_del(&ref2->list);
			kfree(ref2);
		}

	}
}

/*
 * add all inline backrefs for bytenr to the list
 */
static int __add_inline_refs(struct btrfs_fs_info *fs_info,
			     struct pref_state *prefstate,
			     struct btrfs_path *path, u64 bytenr,
			     int *info_level, u64 *total_refs)
{
	int ret = 0;
	int slot;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_key found_key;
	unsigned long ptr;
	unsigned long end;
	struct btrfs_extent_item *ei;
	u64 flags;
	u64 item_size;
	/*
	 * enumerate all inline refs
	 */
	leaf = path->nodes[0];
	slot = path->slots[0];

	item_size = btrfs_item_size(leaf, slot);
	BUG_ON(item_size < sizeof(*ei));

	ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);
	flags = btrfs_extent_flags(leaf, ei);
	*total_refs += btrfs_extent_refs(leaf, ei);
	btrfs_item_key_to_cpu(leaf, &found_key, slot);

	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + item_size;

	if (found_key.type == BTRFS_EXTENT_ITEM_KEY &&
	    flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
		struct btrfs_tree_block_info *info;

		info = (struct btrfs_tree_block_info *)ptr;
		*info_level = btrfs_tree_block_level(leaf, info);
		ptr += sizeof(struct btrfs_tree_block_info);
		BUG_ON(ptr > end);
	} else if (found_key.type == BTRFS_METADATA_ITEM_KEY) {
		*info_level = found_key.offset;
	} else {
		BUG_ON(!(flags & BTRFS_EXTENT_FLAG_DATA));
	}

	while (ptr < end) {
		struct btrfs_extent_inline_ref *iref;
		u64 offset;
		int type;

		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(leaf, iref);
		offset = btrfs_extent_inline_ref_offset(leaf, iref);

		switch (type) {
		case BTRFS_SHARED_BLOCK_REF_KEY:
			ret = __add_prelim_ref(prefstate, 0, NULL,
						*info_level + 1, offset,
						bytenr, 1, GFP_NOFS);
			break;
		case BTRFS_SHARED_DATA_REF_KEY: {
			struct btrfs_shared_data_ref *sdref;
			int count;

			sdref = (struct btrfs_shared_data_ref *)(iref + 1);
			count = btrfs_shared_data_ref_count(leaf, sdref);
			ret = __add_prelim_ref(prefstate, 0, NULL, 0, offset,
					       bytenr, count, GFP_NOFS);
			break;
		}
		case BTRFS_TREE_BLOCK_REF_KEY:
			ret = __add_prelim_ref(prefstate, offset, NULL,
					       *info_level + 1, 0,
					       bytenr, 1, GFP_NOFS);
			break;
		case BTRFS_EXTENT_DATA_REF_KEY: {
			struct btrfs_extent_data_ref *dref;
			int count;
			u64 root;

			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			count = btrfs_extent_data_ref_count(leaf, dref);
			key.objectid = btrfs_extent_data_ref_objectid(leaf,
								      dref);
			key.type = BTRFS_EXTENT_DATA_KEY;
			key.offset = btrfs_extent_data_ref_offset(leaf, dref);
			root = btrfs_extent_data_ref_root(leaf, dref);
			ret = __add_prelim_ref(prefstate, root, &key, 0, 0,
					       bytenr, count, GFP_NOFS);
			break;
		}
		default:
			error("invalid backref type: %u", type);
			ret = -EUCLEAN;
		}
		if (ret)
			return ret;
		ptr += btrfs_extent_inline_ref_size(type);
	}

	return 0;
}

/*
 * add all non-inline backrefs for bytenr to the list
 */
static int __add_keyed_refs(struct btrfs_fs_info *fs_info,
			    struct pref_state *prefstate,
			    struct btrfs_path *path, u64 bytenr,
			    int info_level)
{
	struct btrfs_root *extent_root = btrfs_extent_root(fs_info, bytenr);
	int ret;
	int slot;
	struct extent_buffer *leaf;
	struct btrfs_key key;

	while (1) {
		ret = btrfs_next_item(extent_root, path);
		if (ret < 0)
			break;
		if (ret) {
			ret = 0;
			break;
		}

		slot = path->slots[0];
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);

		if (key.objectid != bytenr)
			break;
		if (key.type < BTRFS_TREE_BLOCK_REF_KEY)
			continue;
		if (key.type > BTRFS_SHARED_DATA_REF_KEY)
			break;

		switch (key.type) {
		case BTRFS_SHARED_BLOCK_REF_KEY:
			ret = __add_prelim_ref(prefstate, 0, NULL,
						info_level + 1, key.offset,
						bytenr, 1, GFP_NOFS);
			break;
		case BTRFS_SHARED_DATA_REF_KEY: {
			struct btrfs_shared_data_ref *sdref;
			int count;

			sdref = btrfs_item_ptr(leaf, slot,
					      struct btrfs_shared_data_ref);
			count = btrfs_shared_data_ref_count(leaf, sdref);
			ret = __add_prelim_ref(prefstate, 0, NULL, 0, key.offset,
						bytenr, count, GFP_NOFS);
			break;
		}
		case BTRFS_TREE_BLOCK_REF_KEY:
			ret = __add_prelim_ref(prefstate, key.offset, NULL,
					       info_level + 1, 0,
					       bytenr, 1, GFP_NOFS);
			break;
		case BTRFS_EXTENT_DATA_REF_KEY: {
			struct btrfs_extent_data_ref *dref;
			int count;
			u64 root;

			dref = btrfs_item_ptr(leaf, slot,
					      struct btrfs_extent_data_ref);
			count = btrfs_extent_data_ref_count(leaf, dref);
			key.objectid = btrfs_extent_data_ref_objectid(leaf,
								      dref);
			key.type = BTRFS_EXTENT_DATA_KEY;
			key.offset = btrfs_extent_data_ref_offset(leaf, dref);
			root = btrfs_extent_data_ref_root(leaf, dref);
			ret = __add_prelim_ref(prefstate, root, &key, 0, 0,
					       bytenr, count, GFP_NOFS);
			break;
		}
		default:
			WARN_ON(1);
		}
		if (ret)
			return ret;

	}

	return ret;
}

/*
 * this adds all existing backrefs (inline backrefs, backrefs for the given
 * bytenr to the refs list, merges duplicates and resolves indirect refs to
 * their parent bytenr.
 * When roots are found, they're added to the roots list
 *
 * FIXME some caching might speed things up
 */
static int find_parent_nodes(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info, u64 bytenr,
			     u64 time_seq, struct ulist *refs,
			     struct ulist *roots, const u64 *extent_item_pos)
{
	struct btrfs_root *extent_root = btrfs_extent_root(fs_info, bytenr);
	struct btrfs_key key;
	struct btrfs_path *path;
	int info_level = 0;
	int ret;
	struct pref_state prefstate;
	struct __prelim_ref *ref;
	struct extent_inode_elem *eie = NULL;
	u64 total_refs = 0;

	init_pref_state(&prefstate);

	key.objectid = bytenr;
	if (btrfs_fs_incompat(fs_info, SKINNY_METADATA))
		key.type = BTRFS_METADATA_ITEM_KEY;
	else
		key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_search_slot(trans, extent_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	BUG_ON(ret == 0);

	if (path->slots[0]) {
		struct extent_buffer *leaf;
		int slot;

		path->slots[0]--;
		leaf = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid == bytenr &&
		    (key.type == BTRFS_EXTENT_ITEM_KEY ||
		     key.type == BTRFS_METADATA_ITEM_KEY)) {
			ret = __add_inline_refs(fs_info, &prefstate, path,
						bytenr, &info_level,
						&total_refs);
			if (ret)
				goto out;
			ret = __add_keyed_refs(fs_info, &prefstate, path,
					       bytenr, info_level);
			if (ret)
				goto out;
		}
	}
	btrfs_release_path(path);

	ret = __add_missing_keys(fs_info, &prefstate);
	if (ret)
		goto out;

	__merge_refs(&prefstate, 1);

	ret = __resolve_indirect_refs(fs_info, &prefstate, path, time_seq,
				      extent_item_pos, total_refs);
	if (ret)
		goto out;

	__merge_refs(&prefstate, 2);

	BUG_ON(!list_empty(&prefstate.pending_missing_keys));
	BUG_ON(!list_empty(&prefstate.pending_indirect_refs));

	while (!list_empty(&prefstate.pending)) {
		ref = list_first_pref(&prefstate.pending);
		WARN_ON(ref->count < 0);
		if (roots && ref->count && ref->root_id && ref->parent == 0) {
			/* no parent == root of tree */
			ret = ulist_add(roots, ref->root_id, 0, GFP_NOFS);
			if (ret < 0)
				goto out;
		}
		if (ref->count && ref->parent) {
			if (extent_item_pos && !ref->inode_list &&
			    ref->level == 0) {
				struct extent_buffer *eb;
				struct btrfs_tree_parent_check check = {
					.level = ref->level,
				};

				eb = read_tree_block(fs_info, ref->parent, &check);
				if (!extent_buffer_uptodate(eb)) {
					free_extent_buffer(eb);
					ret = -EIO;
					goto out;
				}
				ret = find_extent_in_eb(eb, bytenr,
							*extent_item_pos, &eie);
				free_extent_buffer(eb);
				if (ret < 0)
					goto out;
				ref->inode_list = eie;
			}
			ret = ulist_add_merge_ptr(refs, ref->parent,
						  ref->inode_list,
						  (void **)&eie, GFP_NOFS);
			if (ret < 0)
				goto out;
			if (!ret && extent_item_pos) {
				/*
				 * we've recorded that parent, so we must extend
				 * its inode list here
				 */
				BUG_ON(!eie);
				while (eie->next)
					eie = eie->next;
				eie->next = ref->inode_list;
			}
			eie = NULL;
		}
		list_del(&ref->list);
		kfree(ref);
	}

out:
	btrfs_free_path(path);
	while (!list_empty(&prefstate.pending)) {
		ref = list_first_pref(&prefstate.pending);
		list_del(&ref->list);
		kfree(ref);
	}
	if (ret < 0)
		free_inode_elem_list(eie);
	return ret;
}

static void free_leaf_list(struct ulist *blocks)
{
	struct ulist_node *node = NULL;
	struct extent_inode_elem *eie;
	struct ulist_iterator uiter;

	ULIST_ITER_INIT(&uiter);
	while ((node = ulist_next(blocks, &uiter))) {
		if (!node->aux)
			continue;
		eie = (struct extent_inode_elem *)(uintptr_t)node->aux;
		free_inode_elem_list(eie);
		node->aux = 0;
	}

	ulist_free(blocks);
}

/*
 * Finds all leafs with a reference to the specified combination of bytenr and
 * offset. key_list_head will point to a list of corresponding keys (caller must
 * free each list element). The leafs will be stored in the leafs ulist, which
 * must be freed with ulist_free.
 *
 * returns 0 on success, <0 on error
 */
static int btrfs_find_all_leafs(struct btrfs_trans_handle *trans,
				struct btrfs_fs_info *fs_info, u64 bytenr,
				u64 time_seq, struct ulist **leafs,
				const u64 *extent_item_pos)
{
	int ret;

	*leafs = ulist_alloc(GFP_NOFS);
	if (!*leafs)
		return -ENOMEM;

	ret = find_parent_nodes(trans, fs_info, bytenr,
				time_seq, *leafs, NULL, extent_item_pos);
	if (ret < 0 && ret != -ENOENT) {
		free_leaf_list(*leafs);
		return ret;
	}

	return 0;
}

/*
 * walk all backrefs for a given extent to find all roots that reference this
 * extent. Walking a backref means finding all extents that reference this
 * extent and in turn walk the backrefs of those, too. Naturally this is a
 * recursive process, but here it is implemented in an iterative fashion: We
 * find all referencing extents for the extent in question and put them on a
 * list. In turn, we find all referencing extents for those, further appending
 * to the list. The way we iterate the list allows adding more elements after
 * the current while iterating. The process stops when we reach the end of the
 * list. Found roots are added to the roots list.
 *
 * returns 0 on success, < 0 on error.
 */
static int __btrfs_find_all_roots(struct btrfs_trans_handle *trans,
				  struct btrfs_fs_info *fs_info, u64 bytenr,
				  u64 time_seq, struct ulist **roots)
{
	struct ulist *tmp;
	struct ulist_node *node = NULL;
	struct ulist_iterator uiter;
	int ret;

	tmp = ulist_alloc(GFP_NOFS);
	if (!tmp)
		return -ENOMEM;
	*roots = ulist_alloc(GFP_NOFS);
	if (!*roots) {
		ulist_free(tmp);
		return -ENOMEM;
	}

	ULIST_ITER_INIT(&uiter);
	while (1) {
		ret = find_parent_nodes(trans, fs_info, bytenr,
					time_seq, tmp, *roots, NULL);
		if (ret < 0 && ret != -ENOENT) {
			ulist_free(tmp);
			ulist_free(*roots);
			return ret;
		}
		node = ulist_next(tmp, &uiter);
		if (!node)
			break;
		bytenr = node->val;
		cond_resched();
	}

	ulist_free(tmp);
	return 0;
}

int btrfs_find_all_roots(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *fs_info, u64 bytenr,
			 u64 time_seq, struct ulist **roots)
{
	return __btrfs_find_all_roots(trans, fs_info, bytenr, time_seq, roots);
}

/*
 * this makes the path point to (inum INODE_ITEM ioff)
 */
int inode_item_info(u64 inum, u64 ioff, struct btrfs_root *fs_root,
			struct btrfs_path *path)
{
	struct btrfs_key key;
	return btrfs_find_item(fs_root, path, inum, ioff,
			BTRFS_INODE_ITEM_KEY, &key);
}

static int inode_ref_info(u64 inum, u64 ioff, struct btrfs_root *fs_root,
				struct btrfs_path *path,
				struct btrfs_key *found_key)
{
	return btrfs_find_item(fs_root, path, inum, ioff,
			BTRFS_INODE_REF_KEY, found_key);
}

int btrfs_find_one_extref(struct btrfs_root *root, u64 inode_objectid,
			  u64 start_off, struct btrfs_path *path,
			  struct btrfs_inode_extref **ret_extref,
			  u64 *found_off)
{
	int ret, slot;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_inode_extref *extref;
	struct extent_buffer *leaf;
	unsigned long ptr;

	key.objectid = inode_objectid;
	key.type = BTRFS_INODE_EXTREF_KEY;
	key.offset = start_off;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		return ret;

	while (1) {
		leaf = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			/*
			 * If the item at offset is not found,
			 * btrfs_search_slot will point us to the slot
			 * where it should be inserted. In our case
			 * that will be the slot directly before the
			 * next INODE_REF_KEY_V2 item. In the case
			 * that we're pointing to the last slot in a
			 * leaf, we must move one leaf over.
			 */
			ret = btrfs_next_leaf(root, path);
			if (ret) {
				if (ret >= 1)
					ret = -ENOENT;
				break;
			}
			continue;
		}

		btrfs_item_key_to_cpu(leaf, &found_key, slot);

		/*
		 * Check that we're still looking at an extended ref key for
		 * this particular objectid. If we have different
		 * objectid or type then there are no more to be found
		 * in the tree and we can exit.
		 */
		ret = -ENOENT;
		if (found_key.objectid != inode_objectid)
			break;
		if (found_key.type != BTRFS_INODE_EXTREF_KEY)
			break;

		ret = 0;
		ptr = btrfs_item_ptr_offset(leaf, path->slots[0]);
		extref = (struct btrfs_inode_extref *)ptr;
		*ret_extref = extref;
		if (found_off)
			*found_off = found_key.offset;
		break;
	}

	return ret;
}

/*
 * this iterates to turn a name (from iref/extref) into a full filesystem path.
 * Elements of the path are separated by '/' and the path is guaranteed to be
 * 0-terminated. the path is only given within the current file system.
 * Therefore, it never starts with a '/'. the caller is responsible to provide
 * "size" bytes in "dest". the dest buffer will be filled backwards. finally,
 * the start point of the resulting string is returned. this pointer is within
 * dest, normally.
 * in case the path buffer would overflow, the pointer is decremented further
 * as if output was written to the buffer, though no more output is actually
 * generated. that way, the caller can determine how much space would be
 * required for the path to fit into the buffer. in that case, the returned
 * value will be smaller than dest. callers must check this!
 */
char *btrfs_ref_to_path(struct btrfs_root *fs_root, struct btrfs_path *path,
			u32 name_len, unsigned long name_off,
			struct extent_buffer *eb_in, u64 parent,
			char *dest, u32 size)
{
	int slot;
	u64 next_inum;
	int ret;
	s64 bytes_left = ((s64)size) - 1;
	struct extent_buffer *eb = eb_in;
	struct btrfs_key found_key;
	struct btrfs_inode_ref *iref;

	if (bytes_left >= 0)
		dest[bytes_left] = '\0';

	while (1) {
		bytes_left -= name_len;
		if (bytes_left >= 0)
			read_extent_buffer(eb, dest + bytes_left,
					   name_off, name_len);
		if (eb != eb_in)
			free_extent_buffer(eb);
		ret = inode_ref_info(parent, 0, fs_root, path, &found_key);
		if (ret > 0)
			ret = -ENOENT;
		if (ret)
			break;

		next_inum = found_key.offset;

		/* regular exit ahead */
		if (parent == next_inum)
			break;

		slot = path->slots[0];
		eb = path->nodes[0];
		/* make sure we can use eb after releasing the path */
		if (eb != eb_in)
			eb->refs++;
		btrfs_release_path(path);
		iref = btrfs_item_ptr(eb, slot, struct btrfs_inode_ref);

		name_len = btrfs_inode_ref_name_len(eb, iref);
		name_off = (unsigned long)(iref + 1);

		parent = next_inum;
		--bytes_left;
		if (bytes_left >= 0)
			dest[bytes_left] = '/';
	}

	btrfs_release_path(path);

	if (ret)
		return ERR_PTR(ret);

	return dest + bytes_left;
}

/*
 * this makes the path point to (logical EXTENT_ITEM *)
 * returns BTRFS_EXTENT_FLAG_DATA for data, BTRFS_EXTENT_FLAG_TREE_BLOCK for
 * tree blocks and <0 on error.
 */
int extent_from_logical(struct btrfs_fs_info *fs_info, u64 logical,
			struct btrfs_path *path, struct btrfs_key *found_key,
			u64 *flags_ret)
{
	struct btrfs_root *extent_root = btrfs_extent_root(fs_info, logical);
	int ret;
	u64 flags;
	u64 size = 0;
	u32 item_size;
	struct extent_buffer *eb;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;

	key.objectid = logical;
	if (btrfs_fs_incompat(fs_info, SKINNY_METADATA))
		key.type = BTRFS_METADATA_ITEM_KEY;
	else
		key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0)
		return ret;

	ret = btrfs_previous_extent_item(extent_root, path, 0);
	if (ret) {
		if (ret > 0)
			ret = -ENOENT;
		return ret;
	}
	btrfs_item_key_to_cpu(path->nodes[0], found_key, path->slots[0]);
	if (found_key->type == BTRFS_METADATA_ITEM_KEY)
		size = fs_info->nodesize;
	else if (found_key->type == BTRFS_EXTENT_ITEM_KEY)
		size = found_key->offset;

	if (found_key->objectid > logical ||
	    found_key->objectid + size <= logical) {
		pr_debug("logical %llu is not within any extent\n", logical);
		return -ENOENT;
	}

	eb = path->nodes[0];
	item_size = btrfs_item_size(eb, path->slots[0]);
	BUG_ON(item_size < sizeof(*ei));

	ei = btrfs_item_ptr(eb, path->slots[0], struct btrfs_extent_item);
	flags = btrfs_extent_flags(eb, ei);

	pr_debug("logical %llu is at position %llu within the extent (%llu "
		 "EXTENT_ITEM %llu) flags %#llx size %u\n",
		 logical, logical - found_key->objectid, found_key->objectid,
		 found_key->offset, flags, item_size);

	if (flags_ret) {
		if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
			*flags_ret = BTRFS_EXTENT_FLAG_TREE_BLOCK;
		else if (flags & BTRFS_EXTENT_FLAG_DATA)
			*flags_ret = BTRFS_EXTENT_FLAG_DATA;
		else
			BUG_ON(1);
		return 0;
	} else {
		WARN_ON(1);
		return -EIO;
	}
}

/*
 * helper function to iterate extent inline refs. ptr must point to a 0 value
 * for the first call and may be modified. it is used to track state.
 * if more refs exist, 0 is returned and the next call to
 * __get_extent_inline_ref must pass the modified ptr parameter to get the
 * next ref. after the last ref was processed, 1 is returned.
 * returns <0 on error
 */
static int __get_extent_inline_ref(unsigned long *ptr, struct extent_buffer *eb,
				   struct btrfs_key *key,
				   struct btrfs_extent_item *ei, u32 item_size,
				   struct btrfs_extent_inline_ref **out_eiref,
				   int *out_type)
{
	unsigned long end;
	u64 flags;
	struct btrfs_tree_block_info *info;

	if (!*ptr) {
		/* first call */
		flags = btrfs_extent_flags(eb, ei);
		if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
			if (key->type == BTRFS_METADATA_ITEM_KEY) {
				/* a skinny metadata extent */
				*out_eiref =
				     (struct btrfs_extent_inline_ref *)(ei + 1);
			} else {
				WARN_ON(key->type != BTRFS_EXTENT_ITEM_KEY);
				info = (struct btrfs_tree_block_info *)(ei + 1);
				*out_eiref =
				   (struct btrfs_extent_inline_ref *)(info + 1);
			}
		} else {
			*out_eiref = (struct btrfs_extent_inline_ref *)(ei + 1);
		}
		*ptr = (unsigned long)*out_eiref;
		if ((unsigned long)(*ptr) >= (unsigned long)ei + item_size)
			return -ENOENT;
	}

	end = (unsigned long)ei + item_size;
	*out_eiref = (struct btrfs_extent_inline_ref *)(*ptr);
	*out_type = btrfs_extent_inline_ref_type(eb, *out_eiref);

	*ptr += btrfs_extent_inline_ref_size(*out_type);
	WARN_ON(*ptr > end);
	if (*ptr == end)
		return 1; /* last */

	return 0;
}

/*
 * reads the tree block backref for an extent. tree level and root are returned
 * through out_level and out_root. ptr must point to a 0 value for the first
 * call and may be modified (see __get_extent_inline_ref comment).
 * returns 0 if data was provided, 1 if there was no more data to provide or
 * <0 on error.
 */
int tree_backref_for_extent(unsigned long *ptr, struct extent_buffer *eb,
			    struct btrfs_key *key, struct btrfs_extent_item *ei,
			    u32 item_size, u64 *out_root, u8 *out_level)
{
	int ret;
	int type;
	struct btrfs_tree_block_info *info;
	struct btrfs_extent_inline_ref *eiref;

	if (*ptr == (unsigned long)-1)
		return 1;

	while (1) {
		ret = __get_extent_inline_ref(ptr, eb, key, ei, item_size,
					      &eiref, &type);
		if (ret < 0)
			return ret;

		if (type == BTRFS_TREE_BLOCK_REF_KEY ||
		    type == BTRFS_SHARED_BLOCK_REF_KEY)
			break;

		if (ret == 1)
			return 1;
	}

	/* we can treat both ref types equally here */
	info = (struct btrfs_tree_block_info *)(ei + 1);
	*out_root = btrfs_extent_inline_ref_offset(eb, eiref);
	*out_level = btrfs_tree_block_level(eb, info);

	if (ret == 1)
		*ptr = (unsigned long)-1;

	return 0;
}

static int iterate_leaf_refs(struct extent_inode_elem *inode_list,
				u64 root, u64 extent_item_objectid,
				iterate_extent_inodes_t *iterate, void *ctx)
{
	struct extent_inode_elem *eie;
	int ret = 0;

	for (eie = inode_list; eie; eie = eie->next) {
		pr_debug("ref for %llu resolved, key (%llu EXTEND_DATA %llu), "
			 "root %llu\n", extent_item_objectid,
			 eie->inum, eie->offset, root);
		ret = iterate(eie->inum, eie->offset, root, ctx);
		if (ret) {
			pr_debug("stopping iteration for %llu due to ret=%d\n",
				 extent_item_objectid, ret);
			break;
		}
	}

	return ret;
}

/*
 * calls iterate() for every inode that references the extent identified by
 * the given parameters.
 * when the iterator function returns a non-zero value, iteration stops.
 */
int iterate_extent_inodes(struct btrfs_fs_info *fs_info,
				u64 extent_item_objectid, u64 extent_item_pos,
				int search_commit_root,
				iterate_extent_inodes_t *iterate, void *ctx)
{
	int ret;
	struct btrfs_trans_handle *trans = NULL;
	struct ulist *refs = NULL;
	struct ulist *roots = NULL;
	struct ulist_node *ref_node = NULL;
	struct ulist_node *root_node = NULL;
	struct ulist_iterator ref_uiter;
	struct ulist_iterator root_uiter;

	pr_debug("resolving all inodes for extent %llu\n",
			extent_item_objectid);

	ret = btrfs_find_all_leafs(trans, fs_info, extent_item_objectid,
				   0, &refs, &extent_item_pos);
	if (ret)
		goto out;

	ULIST_ITER_INIT(&ref_uiter);
	while (!ret && (ref_node = ulist_next(refs, &ref_uiter))) {
		ret = __btrfs_find_all_roots(trans, fs_info, ref_node->val,
					     0, &roots);
		if (ret)
			break;
		ULIST_ITER_INIT(&root_uiter);
		while (!ret && (root_node = ulist_next(roots, &root_uiter))) {
			pr_debug("root %llu references leaf %llu, data list "
				 "%#llx\n", root_node->val, ref_node->val,
				 ref_node->aux);
			ret = iterate_leaf_refs((struct extent_inode_elem *)
						(uintptr_t)ref_node->aux,
						root_node->val,
						extent_item_objectid,
						iterate, ctx);
		}
		ulist_free(roots);
	}

	free_leaf_list(refs);
out:
	return ret;
}

int iterate_inodes_from_logical(u64 logical, struct btrfs_fs_info *fs_info,
				struct btrfs_path *path,
				iterate_extent_inodes_t *iterate, void *ctx)
{
	int ret;
	u64 extent_item_pos;
	u64 flags = 0;
	struct btrfs_key found_key;
	int search_commit_root = 0;

	ret = extent_from_logical(fs_info, logical, path, &found_key, &flags);
	btrfs_release_path(path);
	if (ret < 0)
		return ret;
	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
		return -EINVAL;

	extent_item_pos = logical - found_key.objectid;
	ret = iterate_extent_inodes(fs_info, found_key.objectid,
					extent_item_pos, search_commit_root,
					iterate, ctx);

	return ret;
}

typedef int (iterate_irefs_t)(u64 parent, u32 name_len, unsigned long name_off,
			      struct extent_buffer *eb, void *ctx);

static int iterate_inode_refs(u64 inum, struct btrfs_root *fs_root,
			      struct btrfs_path *path,
			      iterate_irefs_t *iterate, void *ctx)
{
	int ret = 0;
	int slot;
	u32 cur;
	u32 len;
	u32 name_len;
	u64 parent = 0;
	int found = 0;
	struct extent_buffer *eb;
	struct btrfs_inode_ref *iref;
	struct btrfs_key found_key;

	while (!ret) {
		ret = inode_ref_info(inum, parent ? parent+1 : 0, fs_root, path,
				     &found_key);
		if (ret < 0)
			break;
		if (ret) {
			ret = found ? 0 : -ENOENT;
			break;
		}
		++found;

		parent = found_key.offset;
		slot = path->slots[0];
		eb = btrfs_clone_extent_buffer(path->nodes[0]);
		if (!eb) {
			ret = -ENOMEM;
			break;
		}
		extent_buffer_get(eb);
		btrfs_release_path(path);

		iref = btrfs_item_ptr(eb, slot, struct btrfs_inode_ref);

		for (cur = 0; cur < btrfs_item_size(eb, slot); cur += len) {
			name_len = btrfs_inode_ref_name_len(eb, iref);
			/* path must be released before calling iterate()! */
			pr_debug("following ref at offset %u for inode %llu in "
				 "tree %llu\n", cur, found_key.objectid,
				 fs_root->objectid);
			ret = iterate(parent, name_len,
				      (unsigned long)(iref + 1), eb, ctx);
			if (ret)
				break;
			len = sizeof(*iref) + name_len;
			iref = (struct btrfs_inode_ref *)((char *)iref + len);
		}
		free_extent_buffer(eb);
	}

	btrfs_release_path(path);

	return ret;
}

static int iterate_inode_extrefs(u64 inum, struct btrfs_root *fs_root,
				 struct btrfs_path *path,
				 iterate_irefs_t *iterate, void *ctx)
{
	int ret;
	int slot;
	u64 offset = 0;
	u64 parent;
	int found = 0;
	struct extent_buffer *eb;
	struct btrfs_inode_extref *extref;
	struct extent_buffer *leaf;
	u32 item_size;
	u32 cur_offset;
	unsigned long ptr;

	while (1) {
		ret = btrfs_find_one_extref(fs_root, inum, offset, path, &extref,
					    &offset);
		if (ret < 0)
			break;
		if (ret) {
			ret = found ? 0 : -ENOENT;
			break;
		}
		++found;

		slot = path->slots[0];
		eb = btrfs_clone_extent_buffer(path->nodes[0]);
		if (!eb) {
			ret = -ENOMEM;
			break;
		}
		extent_buffer_get(eb);

		btrfs_release_path(path);

		leaf = path->nodes[0];
		item_size = btrfs_item_size(leaf, slot);
		ptr = btrfs_item_ptr_offset(leaf, slot);
		cur_offset = 0;

		while (cur_offset < item_size) {
			u32 name_len;

			extref = (struct btrfs_inode_extref *)(ptr + cur_offset);
			parent = btrfs_inode_extref_parent(eb, extref);
			name_len = btrfs_inode_extref_name_len(eb, extref);
			ret = iterate(parent, name_len,
				      (unsigned long)&extref->name, eb, ctx);
			if (ret)
				break;

			cur_offset += btrfs_inode_extref_name_len(leaf, extref);
			cur_offset += sizeof(*extref);
		}
		free_extent_buffer(eb);

		offset++;
	}

	btrfs_release_path(path);

	return ret;
}

static int iterate_irefs(u64 inum, struct btrfs_root *fs_root,
			 struct btrfs_path *path, iterate_irefs_t *iterate,
			 void *ctx)
{
	int ret;
	int found_refs = 0;

	ret = iterate_inode_refs(inum, fs_root, path, iterate, ctx);
	if (!ret)
		++found_refs;
	else if (ret != -ENOENT)
		return ret;

	ret = iterate_inode_extrefs(inum, fs_root, path, iterate, ctx);
	if (ret == -ENOENT && found_refs)
		return 0;

	return ret;
}

/*
 * returns 0 if the path could be dumped (probably truncated)
 * returns <0 in case of an error
 */
static int inode_to_path(u64 inum, u32 name_len, unsigned long name_off,
			 struct extent_buffer *eb, void *ctx)
{
	struct inode_fs_paths *ipath = ctx;
	char *fspath;
	char *fspath_min;
	int i = ipath->fspath->elem_cnt;
	const int s_ptr = sizeof(char *);
	u32 bytes_left;

	bytes_left = ipath->fspath->bytes_left > s_ptr ?
					ipath->fspath->bytes_left - s_ptr : 0;

	fspath_min = (char *)ipath->fspath->val + (i + 1) * s_ptr;
	fspath = btrfs_ref_to_path(ipath->fs_root, ipath->btrfs_path, name_len,
				   name_off, eb, inum, fspath_min, bytes_left);
	if (IS_ERR(fspath))
		return PTR_ERR(fspath);

	if (fspath > fspath_min) {
		ipath->fspath->val[i] = (u64)(unsigned long)fspath;
		++ipath->fspath->elem_cnt;
		ipath->fspath->bytes_left = fspath - fspath_min;
	} else {
		++ipath->fspath->elem_missed;
		ipath->fspath->bytes_missing += fspath_min - fspath;
		ipath->fspath->bytes_left = 0;
	}

	return 0;
}

/*
 * this dumps all file system paths to the inode into the ipath struct, provided
 * is has been created large enough. each path is zero-terminated and accessed
 * from ipath->fspath->val[i].
 * when it returns, there are ipath->fspath->elem_cnt number of paths available
 * in ipath->fspath->val[]. When the allocated space wasn't sufficient, the
 * number of missed paths is recorded in ipath->fspath->elem_missed, otherwise,
 * it's zero. ipath->fspath->bytes_missing holds the number of bytes that would
 * have been needed to return all paths.
 */
int paths_from_inode(u64 inum, struct inode_fs_paths *ipath)
{
	return iterate_irefs(inum, ipath->fs_root, ipath->btrfs_path,
			     inode_to_path, ipath);
}

struct btrfs_data_container *init_data_container(u32 total_bytes)
{
	struct btrfs_data_container *data;
	size_t alloc_bytes;

	alloc_bytes = max_t(size_t, total_bytes, sizeof(*data));
	data = vmalloc(alloc_bytes);
	if (!data)
		return ERR_PTR(-ENOMEM);

	if (total_bytes >= sizeof(*data)) {
		data->bytes_left = total_bytes - sizeof(*data);
		data->bytes_missing = 0;
	} else {
		data->bytes_missing = sizeof(*data) - total_bytes;
		data->bytes_left = 0;
	}

	data->elem_cnt = 0;
	data->elem_missed = 0;

	return data;
}

/*
 * allocates space to return multiple file system paths for an inode.
 * total_bytes to allocate are passed, note that space usable for actual path
 * information will be total_bytes - sizeof(struct inode_fs_paths).
 * the returned pointer must be freed with free_ipath() in the end.
 */
struct inode_fs_paths *init_ipath(s32 total_bytes, struct btrfs_root *fs_root,
					struct btrfs_path *path)
{
	struct inode_fs_paths *ifp;
	struct btrfs_data_container *fspath;

	fspath = init_data_container(total_bytes);
	if (IS_ERR(fspath))
		return (void *)fspath;

	ifp = kmalloc(sizeof(*ifp), GFP_NOFS);
	if (!ifp) {
		kfree(fspath);
		return ERR_PTR(-ENOMEM);
	}

	ifp->btrfs_path = path;
	ifp->fspath = fspath;
	ifp->fs_root = fs_root;

	return ifp;
}

void free_ipath(struct inode_fs_paths *ipath)
{
	if (!ipath)
		return;
	vfree(ipath->fspath);
	kfree(ipath);
}
