/*
 * Copyright (c) 2000-2003 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Further, this software is distributed without any warranty that it is
 * free of the rightful claim of any third person regarding infringement
 * or the like.  Any license provided herein, whether implied or
 * otherwise, applies only to this software file.  Patent licenses, if
 * any, provided herein do not apply to combinations of this program with
 * other software, or any other product whatsoever.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Contact information: Silicon Graphics, Inc., 1600 Amphitheatre Pkwy,
 * Mountain View, CA  94043, or:
 *
 * http://www.sgi.com
 *
 * For further information regarding this notice, see:
 *
 * http://oss.sgi.com/projects/GenInfo/SGIGPLNoticeExplan/
 */

#include <xfs.h>

xfs_zone_t		*xfs_bmap_free_item_zone;

/*
 * Called by xfs_bmapi to update extent list structure and the btree
 * after allocating space (or doing a delayed allocation).
 */
STATIC int				/* error */
xfs_bmap_add_extent(
	xfs_inode_t		*ip,	/* incore inode pointer */
	xfs_extnum_t		idx,	/* extent number to update/insert */
	xfs_btree_cur_t		**curp,	/* if *curp is null, not a btree */
	xfs_bmbt_irec_t		*new,	/* new data to put in extent list */
	xfs_fsblock_t		*first,	/* pointer to firstblock variable */
	xfs_bmap_free_t		*flist,	/* list of extents to be freed */
	int			*logflagsp, /* inode logging flags */
	int			whichfork, /* data or attr fork */
	int			rsvd)	/* OK to use reserved data blocks */
{
	xfs_btree_cur_t		*cur;	/* btree cursor or null */
	xfs_filblks_t		da_new; /* new count del alloc blocks used */
	xfs_filblks_t		da_old; /* old count del alloc blocks used */
	int			error;	/* error return value */
#ifdef XFS_BMAP_TRACE
	static char		fname[] = "xfs_bmap_add_extent";
#endif
	xfs_ifork_t		*ifp;	/* inode fork ptr */
	int			logflags; /* returned value */
	xfs_extnum_t		nextents; /* number of extents in file now */

	XFS_STATS_INC(xfsstats.xs_add_exlist);
	cur = *curp;
	ifp = XFS_IFORK_PTR(ip, whichfork);
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	ASSERT(idx <= nextents);
	da_old = da_new = 0;
	error = 0;
	/*
	 * This is the first extent added to a new/empty file.
	 * Special case this one, so other routines get to assume there are
	 * already extents in the list.
	 */
	if (nextents == 0) {
		xfs_bmap_trace_insert(fname, "insert empty", ip, 0, 1, new,
			NULL, whichfork);
		xfs_bmap_insert_exlist(ip, 0, 1, new, whichfork);
		ASSERT(cur == NULL);
		ifp->if_lastex = 0;
		if (!ISNULLSTARTBLOCK(new->br_startblock)) {
			XFS_IFORK_NEXT_SET(ip, whichfork, 1);
			logflags = XFS_ILOG_CORE | XFS_ILOG_FEXT(whichfork);
		} else
			logflags = 0;
	}
	/*
	 * Any kind of new delayed allocation goes here.
	 */
	else if (ISNULLSTARTBLOCK(new->br_startblock)) {
		if (cur)
			ASSERT((cur->bc_private.b.flags &
				XFS_BTCUR_BPRV_WASDEL) == 0);
		if ((error = xfs_bmap_add_extent_hole_delay(ip, idx, cur, new,
				&logflags, rsvd)))
			goto done;
	}
	/*
	 * Real allocation off the end of the file.
	 */
	else if (idx == nextents) {
		if (cur)
			ASSERT((cur->bc_private.b.flags &
				XFS_BTCUR_BPRV_WASDEL) == 0);
		if ((error = xfs_bmap_add_extent_hole_real(ip, idx, cur, new,
				&logflags, whichfork)))
			goto done;
	} else {
		xfs_bmbt_irec_t	prev;	/* old extent at offset idx */

		/*
		 * Get the record referred to by idx.
		 */
		xfs_bmbt_get_all(&ifp->if_u1.if_extents[idx], &prev);
		/*
		 * If it's a real allocation record, and the new allocation ends
		 * after the start of the referred to record, then we're filling
		 * in a delayed or unwritten allocation with a real one, or
		 * converting real back to unwritten.
		 */
		if (!ISNULLSTARTBLOCK(new->br_startblock) &&
		    new->br_startoff + new->br_blockcount > prev.br_startoff) {
			if (prev.br_state != XFS_EXT_UNWRITTEN &&
			    ISNULLSTARTBLOCK(prev.br_startblock)) {
				da_old = STARTBLOCKVAL(prev.br_startblock);
				if (cur)
					ASSERT(cur->bc_private.b.flags &
						XFS_BTCUR_BPRV_WASDEL);
				if ((error = xfs_bmap_add_extent_delay_real(ip,
					idx, &cur, new, &da_new, first, flist,
					&logflags, rsvd)))
					goto done;
			} else if (new->br_state == XFS_EXT_NORM) {
				ASSERT(new->br_state == XFS_EXT_NORM);
				if ((error = xfs_bmap_add_extent_unwritten_real(
					ip, idx, &cur, new, &logflags)))
					goto done;
			} else {
				ASSERT(new->br_state == XFS_EXT_UNWRITTEN);
				if ((error = xfs_bmap_add_extent_unwritten_real(
					ip, idx, &cur, new, &logflags)))
					goto done;
			}
			ASSERT(*curp == cur || *curp == NULL);
		}
		/*
		 * Otherwise we're filling in a hole with an allocation.
		 */
		else {
			if (cur)
				ASSERT((cur->bc_private.b.flags &
					XFS_BTCUR_BPRV_WASDEL) == 0);
			if ((error = xfs_bmap_add_extent_hole_real(ip, idx, cur,
					new, &logflags, whichfork)))
				goto done;
		}
	}

	ASSERT(*curp == cur || *curp == NULL);
	/*
	 * Convert to a btree if necessary.
	 */
	if (XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_EXTENTS &&
	    XFS_IFORK_NEXTENTS(ip, whichfork) > ifp->if_ext_max) {
		int	tmp_logflags;	/* partial log flag return val */

		ASSERT(cur == NULL);
		error = xfs_bmap_extents_to_btree(ip->i_transp, ip, first,
			flist, &cur, da_old > 0, &tmp_logflags, whichfork);
		logflags |= tmp_logflags;
		if (error)
			goto done;
	}
	/*
	 * Adjust for changes in reserved delayed indirect blocks.
	 * Nothing to do for disk quotas here.
	 */
	if (da_old || da_new) {
		xfs_filblks_t	nblks;

		nblks = da_new;
		if (cur)
			nblks += cur->bc_private.b.allocated;
		ASSERT(nblks <= da_old);
		if (nblks < da_old)
			xfs_mod_incore_sb(ip->i_mount, XFS_SBS_FDBLOCKS,
				(int)(da_old - nblks), rsvd);
	}
	/*
	 * Clear out the allocated field, done with it now in any case.
	 */
	if (cur) {
		cur->bc_private.b.allocated = 0;
		*curp = cur;
	}
done:
#ifdef XFSDEBUG
	if (!error)
		xfs_bmap_check_leaf_extents(*curp, ip, whichfork);
#endif
	*logflagsp = logflags;
	return error;
}

/*
 * Called by xfs_bmap_add_extent to handle cases converting a delayed
 * allocation to a real allocation.
 */
STATIC int				/* error */
xfs_bmap_add_extent_delay_real(
	xfs_inode_t		*ip,	/* incore inode pointer */
	xfs_extnum_t		idx,	/* extent number to update/insert */
	xfs_btree_cur_t		**curp,	/* if *curp is null, not a btree */
	xfs_bmbt_irec_t		*new,	/* new data to put in extent list */
	xfs_filblks_t		*dnew,	/* new delayed-alloc indirect blocks */
	xfs_fsblock_t		*first,	/* pointer to firstblock variable */
	xfs_bmap_free_t		*flist,	/* list of extents to be freed */
	int			*logflagsp, /* inode logging flags */
	int			rsvd)	/* OK to use reserved data block allocation */
{
	xfs_bmbt_rec_t		*base;	/* base of extent entry list */
	xfs_btree_cur_t		*cur;	/* btree cursor */
	int			diff;	/* temp value */
	xfs_bmbt_rec_t		*ep;	/* extent entry for idx */
	int			error;	/* error return value */
#ifdef XFS_BMAP_TRACE
	static char		fname[] = "xfs_bmap_add_extent_delay_real";
#endif
	int			i;	/* temp state */
	xfs_fileoff_t		new_endoff;	/* end offset of new entry */
	xfs_bmbt_irec_t		r[3];	/* neighbor extent entries */
					/* left is 0, right is 1, prev is 2 */
	int			rval=0;	/* return value (logging flags) */
	int			state = 0;/* state bits, accessed thru macros */
	xfs_filblks_t		temp;	/* value for dnew calculations */
	xfs_filblks_t		temp2;	/* value for dnew calculations */
	int			tmp_rval;	/* partial logging flags */
	enum {				/* bit number definitions for state */
		LEFT_CONTIG,	RIGHT_CONTIG,
		LEFT_FILLING,	RIGHT_FILLING,
		LEFT_DELAY,	RIGHT_DELAY,
		LEFT_VALID,	RIGHT_VALID
	};

#define	LEFT		r[0]
#define	RIGHT		r[1]
#define	PREV		r[2]
#define	MASK(b)		(1 << (b))
#define	MASK2(a,b)	(MASK(a) | MASK(b))
#define	MASK3(a,b,c)	(MASK2(a,b) | MASK(c))
#define	MASK4(a,b,c,d)	(MASK3(a,b,c) | MASK(d))
#define	STATE_SET(b,v)	((v) ? (state |= MASK(b)) : (state &= ~MASK(b)))
#define	STATE_TEST(b)	(state & MASK(b))
#define	STATE_SET_TEST(b,v)	((v) ? ((state |= MASK(b)), 1) : \
				       ((state &= ~MASK(b)), 0))
#define	SWITCH_STATE		\
	(state & MASK4(LEFT_FILLING, RIGHT_FILLING, LEFT_CONTIG, RIGHT_CONTIG))

	/*
	 * Set up a bunch of variables to make the tests simpler.
	 */
	cur = *curp;
	base = ip->i_df.if_u1.if_extents;
	ep = &base[idx];
	xfs_bmbt_get_all(ep, &PREV);
	new_endoff = new->br_startoff + new->br_blockcount;
	ASSERT(PREV.br_startoff <= new->br_startoff);
	ASSERT(PREV.br_startoff + PREV.br_blockcount >= new_endoff);
	/*
	 * Set flags determining what part of the previous delayed allocation
	 * extent is being replaced by a real allocation.
	 */
	STATE_SET(LEFT_FILLING, PREV.br_startoff == new->br_startoff);
	STATE_SET(RIGHT_FILLING,
		PREV.br_startoff + PREV.br_blockcount == new_endoff);
	/*
	 * Check and set flags if this segment has a left neighbor.
	 * Don't set contiguous if the combined extent would be too large.
	 */
	if (STATE_SET_TEST(LEFT_VALID, idx > 0)) {
		xfs_bmbt_get_all(ep - 1, &LEFT);
		STATE_SET(LEFT_DELAY, ISNULLSTARTBLOCK(LEFT.br_startblock));
	}
	STATE_SET(LEFT_CONTIG,
		STATE_TEST(LEFT_VALID) && !STATE_TEST(LEFT_DELAY) &&
		LEFT.br_startoff + LEFT.br_blockcount == new->br_startoff &&
		LEFT.br_startblock + LEFT.br_blockcount == new->br_startblock &&
		LEFT.br_state == new->br_state &&
		LEFT.br_blockcount + new->br_blockcount <= MAXEXTLEN);
	/*
	 * Check and set flags if this segment has a right neighbor.
	 * Don't set contiguous if the combined extent would be too large.
	 * Also check for all-three-contiguous being too large.
	 */
	if (STATE_SET_TEST(RIGHT_VALID,
			idx <
			ip->i_df.if_bytes / (uint)sizeof(xfs_bmbt_rec_t) - 1)) {
		xfs_bmbt_get_all(ep + 1, &RIGHT);
		STATE_SET(RIGHT_DELAY, ISNULLSTARTBLOCK(RIGHT.br_startblock));
	}
	STATE_SET(RIGHT_CONTIG,
		STATE_TEST(RIGHT_VALID) && !STATE_TEST(RIGHT_DELAY) &&
		new_endoff == RIGHT.br_startoff &&
		new->br_startblock + new->br_blockcount ==
		    RIGHT.br_startblock &&
		new->br_state == RIGHT.br_state &&
		new->br_blockcount + RIGHT.br_blockcount <= MAXEXTLEN &&
		((state & MASK3(LEFT_CONTIG, LEFT_FILLING, RIGHT_FILLING)) !=
		  MASK3(LEFT_CONTIG, LEFT_FILLING, RIGHT_FILLING) ||
		 LEFT.br_blockcount + new->br_blockcount + RIGHT.br_blockcount
		     <= MAXEXTLEN));
	error = 0;
	/*
	 * Switch out based on the FILLING and CONTIG state bits.
	 */
	switch (SWITCH_STATE) {

	case MASK4(LEFT_FILLING, RIGHT_FILLING, LEFT_CONTIG, RIGHT_CONTIG):
		/*
		 * Filling in all of a previously delayed allocation extent.
		 * The left and right neighbors are both contiguous with new.
		 */
		xfs_bmap_trace_pre_update(fname, "LF|RF|LC|RC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep - 1,
			LEFT.br_blockcount + PREV.br_blockcount +
			RIGHT.br_blockcount);
		xfs_bmap_trace_post_update(fname, "LF|RF|LC|RC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmap_trace_delete(fname, "LF|RF|LC|RC", ip, idx, 2,
			XFS_DATA_FORK);
		xfs_bmap_delete_exlist(ip, idx, 2, XFS_DATA_FORK);
		ip->i_df.if_lastex = idx - 1;
		ip->i_d.di_nextents--;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, RIGHT.br_startoff,
					RIGHT.br_startblock,
					RIGHT.br_blockcount, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_delete(cur, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_decrement(cur, 0, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, LEFT.br_startoff,
					LEFT.br_startblock,
					LEFT.br_blockcount +
					PREV.br_blockcount +
					RIGHT.br_blockcount, LEFT.br_state)))
				goto done;
		}
		*dnew = 0;
		break;

	case MASK3(LEFT_FILLING, RIGHT_FILLING, LEFT_CONTIG):
		/*
		 * Filling in all of a previously delayed allocation extent.
		 * The left neighbor is contiguous, the right is not.
		 */
		xfs_bmap_trace_pre_update(fname, "LF|RF|LC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep - 1,
			LEFT.br_blockcount + PREV.br_blockcount);
		xfs_bmap_trace_post_update(fname, "LF|RF|LC", ip, idx - 1,
			XFS_DATA_FORK);
		ip->i_df.if_lastex = idx - 1;
		xfs_bmap_trace_delete(fname, "LF|RF|LC", ip, idx, 1,
			XFS_DATA_FORK);
		xfs_bmap_delete_exlist(ip, idx, 1, XFS_DATA_FORK);
		if (cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			if ((error = xfs_bmbt_lookup_eq(cur, LEFT.br_startoff,
					LEFT.br_startblock, LEFT.br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, LEFT.br_startoff,
					LEFT.br_startblock,
					LEFT.br_blockcount +
					PREV.br_blockcount, LEFT.br_state)))
				goto done;
		}
		*dnew = 0;
		break;

	case MASK3(LEFT_FILLING, RIGHT_FILLING, RIGHT_CONTIG):
		/*
		 * Filling in all of a previously delayed allocation extent.
		 * The right neighbor is contiguous, the left is not.
		 */
		xfs_bmap_trace_pre_update(fname, "LF|RF|RC", ip, idx,
			XFS_DATA_FORK);
		xfs_bmbt_set_startblock(ep, new->br_startblock);
		xfs_bmbt_set_blockcount(ep,
			PREV.br_blockcount + RIGHT.br_blockcount);
		xfs_bmap_trace_post_update(fname, "LF|RF|RC", ip, idx,
			XFS_DATA_FORK);
		ip->i_df.if_lastex = idx;
		xfs_bmap_trace_delete(fname, "LF|RF|RC", ip, idx + 1, 1,
			XFS_DATA_FORK);
		xfs_bmap_delete_exlist(ip, idx + 1, 1, XFS_DATA_FORK);
		if (cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			if ((error = xfs_bmbt_lookup_eq(cur, RIGHT.br_startoff,
					RIGHT.br_startblock,
					RIGHT.br_blockcount, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, PREV.br_startoff,
					new->br_startblock,
					PREV.br_blockcount +
					RIGHT.br_blockcount, PREV.br_state)))
				goto done;
		}
		*dnew = 0;
		break;

	case MASK2(LEFT_FILLING, RIGHT_FILLING):
		/*
		 * Filling in all of a previously delayed allocation extent.
		 * Neither the left nor right neighbors are contiguous with
		 * the new one.
		 */
		xfs_bmap_trace_pre_update(fname, "LF|RF", ip, idx,
			XFS_DATA_FORK);
		xfs_bmbt_set_startblock(ep, new->br_startblock);
		xfs_bmap_trace_post_update(fname, "LF|RF", ip, idx,
			XFS_DATA_FORK);
		ip->i_df.if_lastex = idx;
		ip->i_d.di_nextents++;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, new->br_startoff,
					new->br_startblock, new->br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 0);
			cur->bc_rec.b.br_state = XFS_EXT_NORM;
			if ((error = xfs_bmbt_insert(cur, &i)))
				goto done;
			ASSERT(i == 1);
		}
		*dnew = 0;
		break;

	case MASK2(LEFT_FILLING, LEFT_CONTIG):
		/*
		 * Filling in the first part of a previous delayed allocation.
		 * The left neighbor is contiguous.
		 */
		xfs_bmap_trace_pre_update(fname, "LF|LC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep - 1,
			LEFT.br_blockcount + new->br_blockcount);
		xfs_bmbt_set_startoff(ep,
			PREV.br_startoff + new->br_blockcount);
		xfs_bmap_trace_post_update(fname, "LF|LC", ip, idx - 1,
			XFS_DATA_FORK);
		temp = PREV.br_blockcount - new->br_blockcount;
		xfs_bmap_trace_pre_update(fname, "LF|LC", ip, idx,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep, temp);
		ip->i_df.if_lastex = idx - 1;
		if (cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			if ((error = xfs_bmbt_lookup_eq(cur, LEFT.br_startoff,
					LEFT.br_startblock, LEFT.br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, LEFT.br_startoff,
					LEFT.br_startblock,
					LEFT.br_blockcount +
					new->br_blockcount,
					LEFT.br_state)))
				goto done;
		}
		temp = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(ip, temp),
			STARTBLOCKVAL(PREV.br_startblock));
		xfs_bmbt_set_startblock(ep, NULLSTARTBLOCK((int)temp));
		xfs_bmap_trace_post_update(fname, "LF|LC", ip, idx,
			XFS_DATA_FORK);
		*dnew = temp;
		break;

	case MASK(LEFT_FILLING):
		/*
		 * Filling in the first part of a previous delayed allocation.
		 * The left neighbor is not contiguous.
		 */
		xfs_bmap_trace_pre_update(fname, "LF", ip, idx, XFS_DATA_FORK);
		xfs_bmbt_set_startoff(ep, new_endoff);
		temp = PREV.br_blockcount - new->br_blockcount;
		xfs_bmbt_set_blockcount(ep, temp);
		xfs_bmap_trace_insert(fname, "LF", ip, idx, 1, new, NULL,
			XFS_DATA_FORK);
		xfs_bmap_insert_exlist(ip, idx, 1, new, XFS_DATA_FORK);
		ip->i_df.if_lastex = idx;
		ip->i_d.di_nextents++;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, new->br_startoff,
					new->br_startblock, new->br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 0);
			cur->bc_rec.b.br_state = XFS_EXT_NORM;
			if ((error = xfs_bmbt_insert(cur, &i)))
				goto done;
			ASSERT(i == 1);
		}
		if (ip->i_d.di_format == XFS_DINODE_FMT_EXTENTS &&
		    ip->i_d.di_nextents > ip->i_df.if_ext_max) {
			error = xfs_bmap_extents_to_btree(ip->i_transp, ip,
					first, flist, &cur, 1, &tmp_rval,
					XFS_DATA_FORK);
			rval |= tmp_rval;
			if (error)
				goto done;
		}
		temp = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(ip, temp),
			STARTBLOCKVAL(PREV.br_startblock) -
			(cur ? cur->bc_private.b.allocated : 0));
		base = ip->i_df.if_u1.if_extents;
		ep = &base[idx + 1];
		xfs_bmbt_set_startblock(ep, NULLSTARTBLOCK((int)temp));
		xfs_bmap_trace_post_update(fname, "LF", ip, idx + 1,
			XFS_DATA_FORK);
		*dnew = temp;
		break;

	case MASK2(RIGHT_FILLING, RIGHT_CONTIG):
		/*
		 * Filling in the last part of a previous delayed allocation.
		 * The right neighbor is contiguous with the new allocation.
		 */
		temp = PREV.br_blockcount - new->br_blockcount;
		xfs_bmap_trace_pre_update(fname, "RF|RC", ip, idx,
			XFS_DATA_FORK);
		xfs_bmap_trace_pre_update(fname, "RF|RC", ip, idx + 1,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep, temp);
		xfs_bmbt_set_allf(ep + 1, new->br_startoff, new->br_startblock,
			new->br_blockcount + RIGHT.br_blockcount,
			RIGHT.br_state);
		xfs_bmap_trace_post_update(fname, "RF|RC", ip, idx + 1,
			XFS_DATA_FORK);
		ip->i_df.if_lastex = idx + 1;
		if (cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			if ((error = xfs_bmbt_lookup_eq(cur, RIGHT.br_startoff,
					RIGHT.br_startblock,
					RIGHT.br_blockcount, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, new->br_startoff,
					new->br_startblock,
					new->br_blockcount +
					RIGHT.br_blockcount,
					RIGHT.br_state)))
				goto done;
		}
		temp = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(ip, temp),
			STARTBLOCKVAL(PREV.br_startblock));
		xfs_bmbt_set_startblock(ep, NULLSTARTBLOCK((int)temp));
		xfs_bmap_trace_post_update(fname, "RF|RC", ip, idx,
			XFS_DATA_FORK);
		*dnew = temp;
		break;

	case MASK(RIGHT_FILLING):
		/*
		 * Filling in the last part of a previous delayed allocation.
		 * The right neighbor is not contiguous.
		 */
		temp = PREV.br_blockcount - new->br_blockcount;
		xfs_bmap_trace_pre_update(fname, "RF", ip, idx, XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep, temp);
		xfs_bmap_trace_insert(fname, "RF", ip, idx + 1, 1,
			new, NULL, XFS_DATA_FORK);
		xfs_bmap_insert_exlist(ip, idx + 1, 1, new, XFS_DATA_FORK);
		ip->i_df.if_lastex = idx + 1;
		ip->i_d.di_nextents++;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, new->br_startoff,
					new->br_startblock, new->br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 0);
			cur->bc_rec.b.br_state = XFS_EXT_NORM;
			if ((error = xfs_bmbt_insert(cur, &i)))
				goto done;
			ASSERT(i == 1);
		}
		if (ip->i_d.di_format == XFS_DINODE_FMT_EXTENTS &&
		    ip->i_d.di_nextents > ip->i_df.if_ext_max) {
			error = xfs_bmap_extents_to_btree(ip->i_transp, ip,
				first, flist, &cur, 1, &tmp_rval,
				XFS_DATA_FORK);
			rval |= tmp_rval;
			if (error)
				goto done;
		}
		temp = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(ip, temp),
			STARTBLOCKVAL(PREV.br_startblock) -
			(cur ? cur->bc_private.b.allocated : 0));
		base = ip->i_df.if_u1.if_extents;
		ep = &base[idx];
		xfs_bmbt_set_startblock(ep, NULLSTARTBLOCK((int)temp));
		xfs_bmap_trace_post_update(fname, "RF", ip, idx, XFS_DATA_FORK);
		*dnew = temp;
		break;

	case 0:
		/*
		 * Filling in the middle part of a previous delayed allocation.
		 * Contiguity is impossible here.
		 * This case is avoided almost all the time.
		 */
		temp = new->br_startoff - PREV.br_startoff;
		xfs_bmap_trace_pre_update(fname, "0", ip, idx, XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep, temp);
		r[0] = *new;
		r[1].br_startoff = new_endoff;
		temp2 = PREV.br_startoff + PREV.br_blockcount - new_endoff;
		r[1].br_blockcount = temp2;
		xfs_bmap_trace_insert(fname, "0", ip, idx + 1, 2, &r[0], &r[1],
			XFS_DATA_FORK);
		xfs_bmap_insert_exlist(ip, idx + 1, 2, &r[0], XFS_DATA_FORK);
		ip->i_df.if_lastex = idx + 1;
		ip->i_d.di_nextents++;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, new->br_startoff,
					new->br_startblock, new->br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 0);
			cur->bc_rec.b.br_state = XFS_EXT_NORM;
			if ((error = xfs_bmbt_insert(cur, &i)))
				goto done;
			ASSERT(i == 1);
		}
		if (ip->i_d.di_format == XFS_DINODE_FMT_EXTENTS &&
		    ip->i_d.di_nextents > ip->i_df.if_ext_max) {
			error = xfs_bmap_extents_to_btree(ip->i_transp, ip,
					first, flist, &cur, 1, &tmp_rval,
					XFS_DATA_FORK);
			rval |= tmp_rval;
			if (error)
				goto done;
		}
		temp = xfs_bmap_worst_indlen(ip, temp);
		temp2 = xfs_bmap_worst_indlen(ip, temp2);
		diff = (int)(temp + temp2 - STARTBLOCKVAL(PREV.br_startblock) -
			(cur ? cur->bc_private.b.allocated : 0));
		if (diff > 0 &&
		    xfs_mod_incore_sb(ip->i_mount, XFS_SBS_FDBLOCKS, -diff, rsvd)) {
			/*
			 * Ick gross gag me with a spoon.
			 */
			ASSERT(0);	/* want to see if this ever happens! */
			while (diff > 0) {
				if (temp) {
					temp--;
					diff--;
					if (!diff ||
					    !xfs_mod_incore_sb(ip->i_mount,
						    XFS_SBS_FDBLOCKS, -diff, rsvd))
						break;
				}
				if (temp2) {
					temp2--;
					diff--;
					if (!diff ||
					    !xfs_mod_incore_sb(ip->i_mount,
						    XFS_SBS_FDBLOCKS, -diff, rsvd))
						break;
				}
			}
		}
		base = ip->i_df.if_u1.if_extents;
		ep = &base[idx];
		xfs_bmbt_set_startblock(ep, NULLSTARTBLOCK((int)temp));
		xfs_bmap_trace_post_update(fname, "0", ip, idx, XFS_DATA_FORK);
		xfs_bmap_trace_pre_update(fname, "0", ip, idx + 2,
			XFS_DATA_FORK);
		xfs_bmbt_set_startblock(ep + 2, NULLSTARTBLOCK((int)temp2));
		xfs_bmap_trace_post_update(fname, "0", ip, idx + 2,
			XFS_DATA_FORK);
		*dnew = temp + temp2;
		break;

	case MASK3(LEFT_FILLING, LEFT_CONTIG, RIGHT_CONTIG):
	case MASK3(RIGHT_FILLING, LEFT_CONTIG, RIGHT_CONTIG):
	case MASK2(LEFT_FILLING, RIGHT_CONTIG):
	case MASK2(RIGHT_FILLING, LEFT_CONTIG):
	case MASK2(LEFT_CONTIG, RIGHT_CONTIG):
	case MASK(LEFT_CONTIG):
	case MASK(RIGHT_CONTIG):
		/*
		 * These cases are all impossible.
		 */
		ASSERT(0);
	}
	*curp = cur;
done:
	*logflagsp = rval;
	return error;
#undef	LEFT
#undef	RIGHT
#undef	PREV
#undef	MASK
#undef	MASK2
#undef	MASK3
#undef	MASK4
#undef	STATE_SET
#undef	STATE_TEST
#undef	STATE_SET_TEST
#undef	SWITCH_STATE
}

/*
 * Called by xfs_bmap_add_extent to handle cases converting an unwritten
 * allocation to a real allocation or vice versa.
 */
STATIC int				/* error */
xfs_bmap_add_extent_unwritten_real(
	xfs_inode_t		*ip,	/* incore inode pointer */
	xfs_extnum_t		idx,	/* extent number to update/insert */
	xfs_btree_cur_t		**curp,	/* if *curp is null, not a btree */
	xfs_bmbt_irec_t		*new,	/* new data to put in extent list */
	int			*logflagsp) /* inode logging flags */
{
	xfs_bmbt_rec_t		*base;	/* base of extent entry list */
	xfs_btree_cur_t		*cur;	/* btree cursor */
	xfs_bmbt_rec_t		*ep;	/* extent entry for idx */
	int			error;	/* error return value */
#ifdef XFS_BMAP_TRACE
	static char		fname[] = "xfs_bmap_add_extent_unwritten_real";
#endif
	int			i;	/* temp state */
	xfs_fileoff_t		new_endoff;	/* end offset of new entry */
	xfs_exntst_t		newext;	/* new extent state */
	xfs_exntst_t		oldext;	/* old extent state */
	xfs_bmbt_irec_t		r[3];	/* neighbor extent entries */
					/* left is 0, right is 1, prev is 2 */
	int			rval=0;	/* return value (logging flags) */
	int			state = 0;/* state bits, accessed thru macros */
	enum {				/* bit number definitions for state */
		LEFT_CONTIG,	RIGHT_CONTIG,
		LEFT_FILLING,	RIGHT_FILLING,
		LEFT_DELAY,	RIGHT_DELAY,
		LEFT_VALID,	RIGHT_VALID
	};

#define	LEFT		r[0]
#define	RIGHT		r[1]
#define	PREV		r[2]
#define	MASK(b)		(1 << (b))
#define	MASK2(a,b)	(MASK(a) | MASK(b))
#define	MASK3(a,b,c)	(MASK2(a,b) | MASK(c))
#define	MASK4(a,b,c,d)	(MASK3(a,b,c) | MASK(d))
#define	STATE_SET(b,v)	((v) ? (state |= MASK(b)) : (state &= ~MASK(b)))
#define	STATE_TEST(b)	(state & MASK(b))
#define	STATE_SET_TEST(b,v)	((v) ? ((state |= MASK(b)), 1) : \
				       ((state &= ~MASK(b)), 0))
#define	SWITCH_STATE		\
	(state & MASK4(LEFT_FILLING, RIGHT_FILLING, LEFT_CONTIG, RIGHT_CONTIG))

	/*
	 * Set up a bunch of variables to make the tests simpler.
	 */
	error = 0;
	cur = *curp;
	base = ip->i_df.if_u1.if_extents;
	ep = &base[idx];
	xfs_bmbt_get_all(ep, &PREV);
	newext = new->br_state;
	oldext = (newext == XFS_EXT_UNWRITTEN) ?
		XFS_EXT_NORM : XFS_EXT_UNWRITTEN;
	ASSERT(PREV.br_state == oldext);
	new_endoff = new->br_startoff + new->br_blockcount;
	ASSERT(PREV.br_startoff <= new->br_startoff);
	ASSERT(PREV.br_startoff + PREV.br_blockcount >= new_endoff);
	/*
	 * Set flags determining what part of the previous oldext allocation
	 * extent is being replaced by a newext allocation.
	 */
	STATE_SET(LEFT_FILLING, PREV.br_startoff == new->br_startoff);
	STATE_SET(RIGHT_FILLING,
		PREV.br_startoff + PREV.br_blockcount == new_endoff);
	/*
	 * Check and set flags if this segment has a left neighbor.
	 * Don't set contiguous if the combined extent would be too large.
	 */
	if (STATE_SET_TEST(LEFT_VALID, idx > 0)) {
		xfs_bmbt_get_all(ep - 1, &LEFT);
		STATE_SET(LEFT_DELAY, ISNULLSTARTBLOCK(LEFT.br_startblock));
	}
	STATE_SET(LEFT_CONTIG,
		STATE_TEST(LEFT_VALID) && !STATE_TEST(LEFT_DELAY) &&
		LEFT.br_startoff + LEFT.br_blockcount == new->br_startoff &&
		LEFT.br_startblock + LEFT.br_blockcount == new->br_startblock &&
		LEFT.br_state == newext &&
		LEFT.br_blockcount + new->br_blockcount <= MAXEXTLEN);
	/*
	 * Check and set flags if this segment has a right neighbor.
	 * Don't set contiguous if the combined extent would be too large.
	 * Also check for all-three-contiguous being too large.
	 */
	if (STATE_SET_TEST(RIGHT_VALID,
			idx <
			ip->i_df.if_bytes / (uint)sizeof(xfs_bmbt_rec_t) - 1)) {
		xfs_bmbt_get_all(ep + 1, &RIGHT);
		STATE_SET(RIGHT_DELAY, ISNULLSTARTBLOCK(RIGHT.br_startblock));
	}
	STATE_SET(RIGHT_CONTIG,
		STATE_TEST(RIGHT_VALID) && !STATE_TEST(RIGHT_DELAY) &&
		new_endoff == RIGHT.br_startoff &&
		new->br_startblock + new->br_blockcount ==
		    RIGHT.br_startblock &&
		newext == RIGHT.br_state &&
		new->br_blockcount + RIGHT.br_blockcount <= MAXEXTLEN &&
		((state & MASK3(LEFT_CONTIG, LEFT_FILLING, RIGHT_FILLING)) !=
		  MASK3(LEFT_CONTIG, LEFT_FILLING, RIGHT_FILLING) ||
		 LEFT.br_blockcount + new->br_blockcount + RIGHT.br_blockcount
		     <= MAXEXTLEN));
	/*
	 * Switch out based on the FILLING and CONTIG state bits.
	 */
	switch (SWITCH_STATE) {

	case MASK4(LEFT_FILLING, RIGHT_FILLING, LEFT_CONTIG, RIGHT_CONTIG):
		/*
		 * Setting all of a previous oldext extent to newext.
		 * The left and right neighbors are both contiguous with new.
		 */
		xfs_bmap_trace_pre_update(fname, "LF|RF|LC|RC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep - 1,
			LEFT.br_blockcount + PREV.br_blockcount +
			RIGHT.br_blockcount);
		xfs_bmap_trace_post_update(fname, "LF|RF|LC|RC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmap_trace_delete(fname, "LF|RF|LC|RC", ip, idx, 2,
			XFS_DATA_FORK);
		xfs_bmap_delete_exlist(ip, idx, 2, XFS_DATA_FORK);
		ip->i_df.if_lastex = idx - 1;
		ip->i_d.di_nextents -= 2;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, RIGHT.br_startoff,
					RIGHT.br_startblock,
					RIGHT.br_blockcount, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_delete(cur, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_decrement(cur, 0, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_delete(cur, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_decrement(cur, 0, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, LEFT.br_startoff,
				LEFT.br_startblock,
				LEFT.br_blockcount + PREV.br_blockcount +
				RIGHT.br_blockcount, LEFT.br_state)))
				goto done;
		}
		break;

	case MASK3(LEFT_FILLING, RIGHT_FILLING, LEFT_CONTIG):
		/*
		 * Setting all of a previous oldext extent to newext.
		 * The left neighbor is contiguous, the right is not.
		 */
		xfs_bmap_trace_pre_update(fname, "LF|RF|LC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep - 1,
			LEFT.br_blockcount + PREV.br_blockcount);
		xfs_bmap_trace_post_update(fname, "LF|RF|LC", ip, idx - 1,
			XFS_DATA_FORK);
		ip->i_df.if_lastex = idx - 1;
		xfs_bmap_trace_delete(fname, "LF|RF|LC", ip, idx, 1,
			XFS_DATA_FORK);
		xfs_bmap_delete_exlist(ip, idx, 1, XFS_DATA_FORK);
		ip->i_d.di_nextents--;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, PREV.br_startoff,
					PREV.br_startblock, PREV.br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_delete(cur, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_decrement(cur, 0, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, LEFT.br_startoff,
				LEFT.br_startblock,
				LEFT.br_blockcount + PREV.br_blockcount,
				LEFT.br_state)))
				goto done;
		}
		break;

	case MASK3(LEFT_FILLING, RIGHT_FILLING, RIGHT_CONTIG):
		/*
		 * Setting all of a previous oldext extent to newext.
		 * The right neighbor is contiguous, the left is not.
		 */
		xfs_bmap_trace_pre_update(fname, "LF|RF|RC", ip, idx,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep,
			PREV.br_blockcount + RIGHT.br_blockcount);
		xfs_bmbt_set_state(ep, newext);
		xfs_bmap_trace_post_update(fname, "LF|RF|RC", ip, idx,
			XFS_DATA_FORK);
		ip->i_df.if_lastex = idx;
		xfs_bmap_trace_delete(fname, "LF|RF|RC", ip, idx + 1, 1,
			XFS_DATA_FORK);
		xfs_bmap_delete_exlist(ip, idx + 1, 1, XFS_DATA_FORK);
		ip->i_d.di_nextents--;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, RIGHT.br_startoff,
					RIGHT.br_startblock,
					RIGHT.br_blockcount, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_delete(cur, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_decrement(cur, 0, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, new->br_startoff,
				new->br_startblock,
				new->br_blockcount + RIGHT.br_blockcount,
				newext)))
				goto done;
		}
		break;

	case MASK2(LEFT_FILLING, RIGHT_FILLING):
		/*
		 * Setting all of a previous oldext extent to newext.
		 * Neither the left nor right neighbors are contiguous with
		 * the new one.
		 */
		xfs_bmap_trace_pre_update(fname, "LF|RF", ip, idx,
			XFS_DATA_FORK);
		xfs_bmbt_set_state(ep, newext);
		xfs_bmap_trace_post_update(fname, "LF|RF", ip, idx,
			XFS_DATA_FORK);
		ip->i_df.if_lastex = idx;
		if (cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			if ((error = xfs_bmbt_lookup_eq(cur, new->br_startoff,
					new->br_startblock, new->br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, new->br_startoff,
				new->br_startblock, new->br_blockcount,
				newext)))
				goto done;
		}
		break;

	case MASK2(LEFT_FILLING, LEFT_CONTIG):
		/*
		 * Setting the first part of a previous oldext extent to newext.
		 * The left neighbor is contiguous.
		 */
		xfs_bmap_trace_pre_update(fname, "LF|LC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep - 1,
			LEFT.br_blockcount + new->br_blockcount);
		xfs_bmbt_set_startoff(ep,
			PREV.br_startoff + new->br_blockcount);
		xfs_bmap_trace_post_update(fname, "LF|LC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmap_trace_pre_update(fname, "LF|LC", ip, idx,
			XFS_DATA_FORK);
		xfs_bmbt_set_startblock(ep,
			new->br_startblock + new->br_blockcount);
		xfs_bmbt_set_blockcount(ep,
			PREV.br_blockcount - new->br_blockcount);
		xfs_bmap_trace_post_update(fname, "LF|LC", ip, idx,
			XFS_DATA_FORK);
		ip->i_df.if_lastex = idx - 1;
		if (cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			if ((error = xfs_bmbt_lookup_eq(cur, PREV.br_startoff,
					PREV.br_startblock, PREV.br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur,
				PREV.br_startoff + new->br_blockcount,
				PREV.br_startblock + new->br_blockcount,
				PREV.br_blockcount - new->br_blockcount,
				oldext)))
				goto done;
			if ((error = xfs_bmbt_decrement(cur, 0, &i)))
				goto done;
			if (xfs_bmbt_update(cur, LEFT.br_startoff,
				LEFT.br_startblock,
				LEFT.br_blockcount + new->br_blockcount,
				LEFT.br_state))
				goto done;
		}
		break;

	case MASK(LEFT_FILLING):
		/*
		 * Setting the first part of a previous oldext extent to newext.
		 * The left neighbor is not contiguous.
		 */
		xfs_bmap_trace_pre_update(fname, "LF", ip, idx, XFS_DATA_FORK);
		ASSERT(ep && xfs_bmbt_get_state(ep) == oldext);
		xfs_bmbt_set_startoff(ep, new_endoff);
		xfs_bmbt_set_blockcount(ep,
			PREV.br_blockcount - new->br_blockcount);
		xfs_bmbt_set_startblock(ep,
			new->br_startblock + new->br_blockcount);
		xfs_bmap_trace_post_update(fname, "LF", ip, idx, XFS_DATA_FORK);
		xfs_bmap_trace_insert(fname, "LF", ip, idx, 1, new, NULL,
			XFS_DATA_FORK);
		xfs_bmap_insert_exlist(ip, idx, 1, new, XFS_DATA_FORK);
		ip->i_df.if_lastex = idx;
		ip->i_d.di_nextents++;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, PREV.br_startoff,
					PREV.br_startblock, PREV.br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur,
				PREV.br_startoff + new->br_blockcount,
				PREV.br_startblock + new->br_blockcount,
				PREV.br_blockcount - new->br_blockcount,
				oldext)))
				goto done;
			cur->bc_rec.b = *new;
			if ((error = xfs_bmbt_insert(cur, &i)))
				goto done;
			ASSERT(i == 1);
		}
		break;

	case MASK2(RIGHT_FILLING, RIGHT_CONTIG):
		/*
		 * Setting the last part of a previous oldext extent to newext.
		 * The right neighbor is contiguous with the new allocation.
		 */
		xfs_bmap_trace_pre_update(fname, "RF|RC", ip, idx,
			XFS_DATA_FORK);
		xfs_bmap_trace_pre_update(fname, "RF|RC", ip, idx + 1,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep,
			PREV.br_blockcount - new->br_blockcount);
		xfs_bmap_trace_post_update(fname, "RF|RC", ip, idx,
			XFS_DATA_FORK);
		xfs_bmbt_set_allf(ep + 1, new->br_startoff, new->br_startblock,
			new->br_blockcount + RIGHT.br_blockcount, newext);
		xfs_bmap_trace_post_update(fname, "RF|RC", ip, idx + 1,
			XFS_DATA_FORK);
		ip->i_df.if_lastex = idx + 1;
		if (cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			if ((error = xfs_bmbt_lookup_eq(cur, PREV.br_startoff,
					PREV.br_startblock,
					PREV.br_blockcount, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, PREV.br_startoff,
				PREV.br_startblock,
				PREV.br_blockcount - new->br_blockcount,
				oldext)))
				goto done;
			if ((error = xfs_bmbt_increment(cur, 0, &i)))
				goto done;
			if ((error = xfs_bmbt_update(cur, new->br_startoff,
				new->br_startblock,
				new->br_blockcount + RIGHT.br_blockcount,
				newext)))
				goto done;
		}
		break;

	case MASK(RIGHT_FILLING):
		/*
		 * Setting the last part of a previous oldext extent to newext.
		 * The right neighbor is not contiguous.
		 */
		xfs_bmap_trace_pre_update(fname, "RF", ip, idx, XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep,
			PREV.br_blockcount - new->br_blockcount);
		xfs_bmap_trace_post_update(fname, "RF", ip, idx, XFS_DATA_FORK);
		xfs_bmap_trace_insert(fname, "RF", ip, idx + 1, 1,
			new, NULL, XFS_DATA_FORK);
		xfs_bmap_insert_exlist(ip, idx + 1, 1, new, XFS_DATA_FORK);
		ip->i_df.if_lastex = idx + 1;
		ip->i_d.di_nextents++;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, PREV.br_startoff,
					PREV.br_startblock, PREV.br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_update(cur, PREV.br_startoff,
				PREV.br_startblock,
				PREV.br_blockcount - new->br_blockcount,
				oldext)))
				goto done;
			if ((error = xfs_bmbt_lookup_eq(cur, new->br_startoff,
					new->br_startblock, new->br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 0);
			cur->bc_rec.b.br_state = XFS_EXT_NORM;
			if ((error = xfs_bmbt_insert(cur, &i)))
				goto done;
			ASSERT(i == 1);
		}
		break;

	case 0:
		/*
		 * Setting the middle part of a previous oldext extent to
		 * newext.  Contiguity is impossible here.
		 * One extent becomes three extents.
		 */
		xfs_bmap_trace_pre_update(fname, "0", ip, idx, XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep,
			new->br_startoff - PREV.br_startoff);
		xfs_bmap_trace_post_update(fname, "0", ip, idx, XFS_DATA_FORK);
		r[0] = *new;
		r[1].br_startoff = new_endoff;
		r[1].br_blockcount =
			PREV.br_startoff + PREV.br_blockcount - new_endoff;
		r[1].br_startblock = new->br_startblock + new->br_blockcount;
		r[1].br_state = oldext;
		xfs_bmap_trace_insert(fname, "0", ip, idx + 1, 2, &r[0], &r[1],
			XFS_DATA_FORK);
		xfs_bmap_insert_exlist(ip, idx + 1, 2, &r[0], XFS_DATA_FORK);
		ip->i_df.if_lastex = idx + 1;
		ip->i_d.di_nextents += 2;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			if ((error = xfs_bmbt_lookup_eq(cur, PREV.br_startoff,
					PREV.br_startblock, PREV.br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 1);
			/* new right extent - oldext */
			if ((error = xfs_bmbt_update(cur, r[1].br_startoff,
				r[1].br_startblock, r[1].br_blockcount,
				r[1].br_state)))
				goto done;
			/* new left extent - oldext */
			PREV.br_blockcount =
				new->br_startoff - PREV.br_startoff;
			cur->bc_rec.b = PREV;
			if ((error = xfs_bmbt_insert(cur, &i)))
				goto done;
			ASSERT(i == 1);
			if ((error = xfs_bmbt_increment(cur, 0, &i)))
				goto done;
			ASSERT(i == 1);
			/* new middle extent - newext */
			cur->bc_rec.b = *new;
			if ((error = xfs_bmbt_insert(cur, &i)))
				goto done;
			ASSERT(i == 1);
		}
		break;

	case MASK3(LEFT_FILLING, LEFT_CONTIG, RIGHT_CONTIG):
	case MASK3(RIGHT_FILLING, LEFT_CONTIG, RIGHT_CONTIG):
	case MASK2(LEFT_FILLING, RIGHT_CONTIG):
	case MASK2(RIGHT_FILLING, LEFT_CONTIG):
	case MASK2(LEFT_CONTIG, RIGHT_CONTIG):
	case MASK(LEFT_CONTIG):
	case MASK(RIGHT_CONTIG):
		/*
		 * These cases are all impossible.
		 */
		ASSERT(0);
	}
	*curp = cur;
done:
	*logflagsp = rval;
	return error;
#undef	LEFT
#undef	RIGHT
#undef	PREV
#undef	MASK
#undef	MASK2
#undef	MASK3
#undef	MASK4
#undef	STATE_SET
#undef	STATE_TEST
#undef	STATE_SET_TEST
#undef	SWITCH_STATE
}

/*
 * Called by xfs_bmap_add_extent to handle cases converting a hole
 * to a delayed allocation.
 */
/*ARGSUSED*/
STATIC int				/* error */
xfs_bmap_add_extent_hole_delay(
	xfs_inode_t		*ip,	/* incore inode pointer */
	xfs_extnum_t		idx,	/* extent number to update/insert */
	xfs_btree_cur_t		*cur,	/* if null, not a btree */
	xfs_bmbt_irec_t		*new,	/* new data to put in extent list */
	int			*logflagsp, /* inode logging flags */
	int			rsvd)		/* OK to allocate reserved blocks */
{
	xfs_bmbt_rec_t		*base;	/* base of extent entry list */
	xfs_bmbt_rec_t		*ep;	/* extent list entry for idx */
#ifdef XFS_BMAP_TRACE
	static char		fname[] = "xfs_bmap_add_extent_hole_delay";
#endif
	xfs_bmbt_irec_t		left;	/* left neighbor extent entry */
	xfs_filblks_t		newlen=0;	/* new indirect size */
	xfs_filblks_t		oldlen=0;	/* old indirect size */
	xfs_bmbt_irec_t		right;	/* right neighbor extent entry */
	int			state;  /* state bits, accessed thru macros */
	xfs_filblks_t		temp;	/* temp for indirect calculations */
	enum {				/* bit number definitions for state */
		LEFT_CONTIG,	RIGHT_CONTIG,
		LEFT_DELAY,	RIGHT_DELAY,
		LEFT_VALID,	RIGHT_VALID
	};

#define	MASK(b)			(1 << (b))
#define	MASK2(a,b)		(MASK(a) | MASK(b))
#define	STATE_SET(b,v)		((v) ? (state |= MASK(b)) : (state &= ~MASK(b)))
#define	STATE_TEST(b)		(state & MASK(b))
#define	STATE_SET_TEST(b,v)	((v) ? ((state |= MASK(b)), 1) : \
				       ((state &= ~MASK(b)), 0))
#define	SWITCH_STATE		(state & MASK2(LEFT_CONTIG, RIGHT_CONTIG))

	base = ip->i_df.if_u1.if_extents;
	ep = &base[idx];
	state = 0;
	ASSERT(ISNULLSTARTBLOCK(new->br_startblock));
	/*
	 * Check and set flags if this segment has a left neighbor
	 */
	if (STATE_SET_TEST(LEFT_VALID, idx > 0)) {
		xfs_bmbt_get_all(ep - 1, &left);
		STATE_SET(LEFT_DELAY, ISNULLSTARTBLOCK(left.br_startblock));
	}
	/*
	 * Check and set flags if the current (right) segment exists.
	 * If it doesn't exist, we're converting the hole at end-of-file.
	 */
	if (STATE_SET_TEST(RIGHT_VALID,
			   idx <
			   ip->i_df.if_bytes / (uint)sizeof(xfs_bmbt_rec_t))) {
		xfs_bmbt_get_all(ep, &right);
		STATE_SET(RIGHT_DELAY, ISNULLSTARTBLOCK(right.br_startblock));
	}
	/*
	 * Set contiguity flags on the left and right neighbors.
	 * Don't let extents get too large, even if the pieces are contiguous.
	 */
	STATE_SET(LEFT_CONTIG,
		STATE_TEST(LEFT_VALID) && STATE_TEST(LEFT_DELAY) &&
		left.br_startoff + left.br_blockcount == new->br_startoff &&
		left.br_blockcount + new->br_blockcount <= MAXEXTLEN);
	STATE_SET(RIGHT_CONTIG,
		STATE_TEST(RIGHT_VALID) && STATE_TEST(RIGHT_DELAY) &&
		new->br_startoff + new->br_blockcount == right.br_startoff &&
		new->br_blockcount + right.br_blockcount <= MAXEXTLEN &&
		(!STATE_TEST(LEFT_CONTIG) ||
		 (left.br_blockcount + new->br_blockcount +
		     right.br_blockcount <= MAXEXTLEN)));
	/*
	 * Switch out based on the contiguity flags.
	 */
	switch (SWITCH_STATE) {

	case MASK2(LEFT_CONTIG, RIGHT_CONTIG):
		/*
		 * New allocation is contiguous with delayed allocations
		 * on the left and on the right.
		 * Merge all three into a single extent list entry.
		 */
		temp = left.br_blockcount + new->br_blockcount +
			right.br_blockcount;
		xfs_bmap_trace_pre_update(fname, "LC|RC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep - 1, temp);
		oldlen = STARTBLOCKVAL(left.br_startblock) +
			STARTBLOCKVAL(new->br_startblock) +
			STARTBLOCKVAL(right.br_startblock);
		newlen = xfs_bmap_worst_indlen(ip, temp);
		xfs_bmbt_set_startblock(ep - 1, NULLSTARTBLOCK((int)newlen));
		xfs_bmap_trace_post_update(fname, "LC|RC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmap_trace_delete(fname, "LC|RC", ip, idx, 1,
			XFS_DATA_FORK);
		xfs_bmap_delete_exlist(ip, idx, 1, XFS_DATA_FORK);
		ip->i_df.if_lastex = idx - 1;
		break;

	case MASK(LEFT_CONTIG):
		/*
		 * New allocation is contiguous with a delayed allocation
		 * on the left.
		 * Merge the new allocation with the left neighbor.
		 */
		temp = left.br_blockcount + new->br_blockcount;
		xfs_bmap_trace_pre_update(fname, "LC", ip, idx - 1,
			XFS_DATA_FORK);
		xfs_bmbt_set_blockcount(ep - 1, temp);
		oldlen = STARTBLOCKVAL(left.br_startblock) +
			STARTBLOCKVAL(new->br_startblock);
		newlen = xfs_bmap_worst_indlen(ip, temp);
		xfs_bmbt_set_startblock(ep - 1, NULLSTARTBLOCK((int)newlen));
		xfs_bmap_trace_post_update(fname, "LC", ip, idx - 1,
			XFS_DATA_FORK);
		ip->i_df.if_lastex = idx - 1;
		break;

	case MASK(RIGHT_CONTIG):
		/*
		 * New allocation is contiguous with a delayed allocation
		 * on the right.
		 * Merge the new allocation with the right neighbor.
		 */
		xfs_bmap_trace_pre_update(fname, "RC", ip, idx, XFS_DATA_FORK);
		temp = new->br_blockcount + right.br_blockcount;
		oldlen = STARTBLOCKVAL(new->br_startblock) +
			STARTBLOCKVAL(right.br_startblock);
		newlen = xfs_bmap_worst_indlen(ip, temp);
		xfs_bmbt_set_allf(ep, new->br_startoff,
			NULLSTARTBLOCK((int)newlen), temp, right.br_state);
		xfs_bmap_trace_post_update(fname, "RC", ip, idx, XFS_DATA_FORK);
		ip->i_df.if_lastex = idx;
		break;

	case 0:
		/*
		 * New allocation is not contiguous with another
		 * delayed allocation.
		 * Insert a new entry.
		 */
		oldlen = newlen = 0;
		xfs_bmap_trace_insert(fname, "0", ip, idx, 1, new, NULL,
			XFS_DATA_FORK);
		xfs_bmap_insert_exlist(ip, idx, 1, new, XFS_DATA_FORK);
		ip->i_df.if_lastex = idx;
		break;
	}
	if (oldlen != newlen) {
		ASSERT(oldlen > newlen);
		xfs_mod_incore_sb(ip->i_mount, XFS_SBS_FDBLOCKS,
			(int)(oldlen - newlen), rsvd);
		/*
		 * Nothing to do for disk quota accounting here.
		 */
	}
	*logflagsp = 0;
	return 0;
#undef	MASK
#undef	MASK2
#undef	STATE_SET
#undef	STATE_TEST
#undef	STATE_SET_TEST
#undef	SWITCH_STATE
}

/*
 * Called by xfs_bmap_add_extent to handle cases converting a hole
 * to a real allocation.
 */
STATIC int				/* error */
xfs_bmap_add_extent_hole_real(
	xfs_inode_t		*ip,	/* incore inode pointer */
	xfs_extnum_t		idx,	/* extent number to update/insert */
	xfs_btree_cur_t		*cur,	/* if null, not a btree */
	xfs_bmbt_irec_t		*new,	/* new data to put in extent list */
	int			*logflagsp, /* inode logging flags */
	int			whichfork) /* data or attr fork */
{
	xfs_bmbt_rec_t		*ep;	/* pointer to extent entry ins. point */
	int			error;	/* error return value */
#ifdef XFS_BMAP_TRACE
	static char		fname[] = "xfs_bmap_add_extent_hole_real";
#endif
	int			i;	/* temp state */
	xfs_ifork_t		*ifp;	/* inode fork pointer */
	xfs_bmbt_irec_t		left;	/* left neighbor extent entry */
	xfs_bmbt_irec_t		right;	/* right neighbor extent entry */
	int			state;	/* state bits, accessed thru macros */
	enum {				/* bit number definitions for state */
		LEFT_CONTIG,	RIGHT_CONTIG,
		LEFT_DELAY,	RIGHT_DELAY,
		LEFT_VALID,	RIGHT_VALID
	};

#define	MASK(b)			(1 << (b))
#define	MASK2(a,b)		(MASK(a) | MASK(b))
#define	STATE_SET(b,v)		((v) ? (state |= MASK(b)) : (state &= ~MASK(b)))
#define	STATE_TEST(b)		(state & MASK(b))
#define	STATE_SET_TEST(b,v)	((v) ? ((state |= MASK(b)), 1) : \
				       ((state &= ~MASK(b)), 0))
#define	SWITCH_STATE		(state & MASK2(LEFT_CONTIG, RIGHT_CONTIG))

	ifp = XFS_IFORK_PTR(ip, whichfork);
	ASSERT(idx <= ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t));
	ep = &ifp->if_u1.if_extents[idx];
	state = 0;
	/*
	 * Check and set flags if this segment has a left neighbor.
	 */
	if (STATE_SET_TEST(LEFT_VALID, idx > 0)) {
		xfs_bmbt_get_all(ep - 1, &left);
		STATE_SET(LEFT_DELAY, ISNULLSTARTBLOCK(left.br_startblock));
	}
	/*
	 * Check and set flags if this segment has a current value.
	 * Not true if we're inserting into the "hole" at eof.
	 */
	if (STATE_SET_TEST(RIGHT_VALID,
			   idx <
			   ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t))) {
		xfs_bmbt_get_all(ep, &right);
		STATE_SET(RIGHT_DELAY, ISNULLSTARTBLOCK(right.br_startblock));
	}
	/*
	 * We're inserting a real allocation between "left" and "right".
	 * Set the contiguity flags.  Don't let extents get too large.
	 */
	STATE_SET(LEFT_CONTIG,
		STATE_TEST(LEFT_VALID) && !STATE_TEST(LEFT_DELAY) &&
		left.br_startoff + left.br_blockcount == new->br_startoff &&
		left.br_startblock + left.br_blockcount == new->br_startblock &&
		left.br_state == new->br_state &&
		left.br_blockcount + new->br_blockcount <= MAXEXTLEN);
	STATE_SET(RIGHT_CONTIG,
		STATE_TEST(RIGHT_VALID) && !STATE_TEST(RIGHT_DELAY) &&
		new->br_startoff + new->br_blockcount == right.br_startoff &&
		new->br_startblock + new->br_blockcount ==
		    right.br_startblock &&
		new->br_state == right.br_state &&
		new->br_blockcount + right.br_blockcount <= MAXEXTLEN &&
		(!STATE_TEST(LEFT_CONTIG) ||
		 left.br_blockcount + new->br_blockcount +
		     right.br_blockcount <= MAXEXTLEN));

	/*
	 * Select which case we're in here, and implement it.
	 */
	switch (SWITCH_STATE) {

	case MASK2(LEFT_CONTIG, RIGHT_CONTIG):
		/*
		 * New allocation is contiguous with real allocations on the
		 * left and on the right.
		 * Merge all three into a single extent list entry.
		 */
		xfs_bmap_trace_pre_update(fname, "LC|RC", ip, idx - 1,
			whichfork);
		xfs_bmbt_set_blockcount(ep - 1,
			left.br_blockcount + new->br_blockcount +
			right.br_blockcount);
		xfs_bmap_trace_post_update(fname, "LC|RC", ip, idx - 1,
			whichfork);
		xfs_bmap_trace_delete(fname, "LC|RC", ip,
			idx, 1, whichfork);
		xfs_bmap_delete_exlist(ip, idx, 1, whichfork);
		ifp->if_lastex = idx - 1;
		XFS_IFORK_NEXT_SET(ip, whichfork,
			XFS_IFORK_NEXTENTS(ip, whichfork) - 1);
		if (cur == NULL) {
			*logflagsp = XFS_ILOG_CORE | XFS_ILOG_FEXT(whichfork);
			return 0;
		}
		*logflagsp = XFS_ILOG_CORE;
		if ((error = xfs_bmbt_lookup_eq(cur, right.br_startoff,
				right.br_startblock, right.br_blockcount, &i)))
			return error;
		ASSERT(i == 1);
		if ((error = xfs_bmbt_delete(cur, &i)))
			return error;
		ASSERT(i == 1);
		if ((error = xfs_bmbt_decrement(cur, 0, &i)))
			return error;
		ASSERT(i == 1);
		error = xfs_bmbt_update(cur, left.br_startoff,
				left.br_startblock,
				left.br_blockcount + new->br_blockcount +
				right.br_blockcount, left.br_state);
		return error;

	case MASK(LEFT_CONTIG):
		/*
		 * New allocation is contiguous with a real allocation
		 * on the left.
		 * Merge the new allocation with the left neighbor.
		 */
		xfs_bmap_trace_pre_update(fname, "LC", ip, idx - 1, whichfork);
		xfs_bmbt_set_blockcount(ep - 1,
			left.br_blockcount + new->br_blockcount);
		xfs_bmap_trace_post_update(fname, "LC", ip, idx - 1, whichfork);
		ifp->if_lastex = idx - 1;
		if (cur == NULL) {
			*logflagsp = XFS_ILOG_FEXT(whichfork);
			return 0;
		}
		*logflagsp = 0;
		if ((error = xfs_bmbt_lookup_eq(cur, left.br_startoff,
				left.br_startblock, left.br_blockcount, &i)))
			return error;
		ASSERT(i == 1);
		error = xfs_bmbt_update(cur, left.br_startoff,
				left.br_startblock,
				left.br_blockcount + new->br_blockcount,
				left.br_state);
		return error;

	case MASK(RIGHT_CONTIG):
		/*
		 * New allocation is contiguous with a real allocation
		 * on the right.
		 * Merge the new allocation with the right neighbor.
		 */
		xfs_bmap_trace_pre_update(fname, "RC", ip, idx, whichfork);
		xfs_bmbt_set_allf(ep, new->br_startoff, new->br_startblock,
			new->br_blockcount + right.br_blockcount,
			right.br_state);
		xfs_bmap_trace_post_update(fname, "RC", ip, idx, whichfork);
		ifp->if_lastex = idx;
		if (cur == NULL) {
			*logflagsp = XFS_ILOG_FEXT(whichfork);
			return 0;
		}
		*logflagsp = 0;
		if ((error = xfs_bmbt_lookup_eq(cur, right.br_startoff,
				right.br_startblock, right.br_blockcount, &i)))
			return error;
		ASSERT(i == 1);
		error = xfs_bmbt_update(cur, new->br_startoff,
				new->br_startblock,
				new->br_blockcount + right.br_blockcount,
				right.br_state);
		return error;

	case 0:
		/*
		 * New allocation is not contiguous with another
		 * real allocation.
		 * Insert a new entry.
		 */
		xfs_bmap_trace_insert(fname, "0", ip, idx, 1, new, NULL,
			whichfork);
		xfs_bmap_insert_exlist(ip, idx, 1, new, whichfork);
		ifp->if_lastex = idx;
		XFS_IFORK_NEXT_SET(ip, whichfork,
			XFS_IFORK_NEXTENTS(ip, whichfork) + 1);
		if (cur == NULL) {
			*logflagsp = XFS_ILOG_CORE | XFS_ILOG_FEXT(whichfork);
			return 0;
		}
		*logflagsp = XFS_ILOG_CORE;
		if ((error = xfs_bmbt_lookup_eq(cur, new->br_startoff,
				new->br_startblock, new->br_blockcount, &i)))
			return error;
		ASSERT(i == 0);
		cur->bc_rec.b.br_state = new->br_state;
		if ((error = xfs_bmbt_insert(cur, &i)))
			return error;
		ASSERT(i == 1);
		return 0;
	}
#undef	MASK
#undef	MASK2
#undef	STATE_SET
#undef	STATE_TEST
#undef	STATE_SET_TEST
#undef	SWITCH_STATE
	/* NOTREACHED */
	ASSERT(0);
	return 0; /* keep gcc quite */
}

#define XFS_ALLOC_GAP_UNITS	4

/*
 * xfs_bmap_alloc is called by xfs_bmapi to allocate an extent for a file.
 * It figures out where to ask the underlying allocator to put the new extent.
 */
STATIC int				/* error */
xfs_bmap_alloc(
	xfs_bmalloca_t	*ap)		/* bmap alloc argument struct */
{
	xfs_fsblock_t	adjust;		/* adjustment to block numbers */
	xfs_alloctype_t	atype=0;	/* type for allocation routines */
	int		error;		/* error return value */
	xfs_agnumber_t	fb_agno;	/* ag number of ap->firstblock */
	xfs_mount_t	*mp;		/* mount point structure */
	int		nullfb;		/* true if ap->firstblock isn't set */
	int		rt;		/* true if inode is realtime */
#ifdef __KERNEL__
	xfs_extlen_t	prod=0;		/* product factor for allocators */
	xfs_extlen_t	ralen=0;	/* realtime allocation length */
#endif

#define	ISLEGAL(x,y)	\
	(rt ? \
		(x) < mp->m_sb.sb_rblocks : \
		XFS_FSB_TO_AGNO(mp, x) == XFS_FSB_TO_AGNO(mp, y) && \
		XFS_FSB_TO_AGNO(mp, x) < mp->m_sb.sb_agcount && \
		XFS_FSB_TO_AGBNO(mp, x) < mp->m_sb.sb_agblocks)

	/*
	 * Set up variables.
	 */
	mp = ap->ip->i_mount;
	nullfb = ap->firstblock == NULLFSBLOCK;
	rt = XFS_IS_REALTIME_INODE(ap->ip) && ap->userdata;
	fb_agno = nullfb ? NULLAGNUMBER : XFS_FSB_TO_AGNO(mp, ap->firstblock);
#ifdef __KERNEL__
	if (rt) {
		xfs_extlen_t	extsz;		/* file extent size for rt */
		xfs_fileoff_t	nexto;		/* next file offset */
		xfs_extlen_t	orig_alen;	/* original ap->alen */
		xfs_fileoff_t	orig_end;	/* original off+len */
		xfs_fileoff_t	orig_off;	/* original ap->off */
		xfs_extlen_t	mod_off;	/* modulus calculations */
		xfs_fileoff_t	prevo;		/* previous file offset */
		xfs_rtblock_t	rtx;		/* realtime extent number */
		xfs_extlen_t	temp;		/* temp for rt calculations */

		/*
		 * Set prod to match the realtime extent size.
		 */
		if (!(extsz = ap->ip->i_d.di_extsize))
			extsz = mp->m_sb.sb_rextsize;
		prod = extsz / mp->m_sb.sb_rextsize;
		orig_off = ap->off;
		orig_alen = ap->alen;
		orig_end = orig_off + orig_alen;
		/*
		 * If the file offset is unaligned vs. the extent size
		 * we need to align it.  This will be possible unless
		 * the file was previously written with a kernel that didn't
		 * perform this alignment.
		 */
		mod_off = do_mod(orig_off, extsz);
		if (mod_off) {
			ap->alen += mod_off;
			ap->off -= mod_off;
		}
		/*
		 * Same adjustment for the end of the requested area.
		 */
		if ((temp = (ap->alen % extsz)))
			ap->alen += extsz - temp;
		/*
		 * If the previous block overlaps with this proposed allocation
		 * then move the start forward without adjusting the length.
		 */
		prevo =
			ap->prevp->br_startoff == NULLFILEOFF ?
				0 :
				(ap->prevp->br_startoff +
				 ap->prevp->br_blockcount);
		if (ap->off != orig_off && ap->off < prevo)
			ap->off = prevo;
		/*
		 * If the next block overlaps with this proposed allocation
		 * then move the start back without adjusting the length,
		 * but not before offset 0.
		 * This may of course make the start overlap previous block,
		 * and if we hit the offset 0 limit then the next block
		 * can still overlap too.
		 */
		nexto = (ap->eof || ap->gotp->br_startoff == NULLFILEOFF) ?
			NULLFILEOFF : ap->gotp->br_startoff;
		if (!ap->eof &&
		    ap->off + ap->alen != orig_end &&
		    ap->off + ap->alen > nexto)
			ap->off = nexto > ap->alen ? nexto - ap->alen : 0;
		/*
		 * If we're now overlapping the next or previous extent that
		 * means we can't fit an extsz piece in this hole.  Just move
		 * the start forward to the first legal spot and set
		 * the length so we hit the end.
		 */
		if ((ap->off != orig_off && ap->off < prevo) ||
		    (ap->off + ap->alen != orig_end &&
		     ap->off + ap->alen > nexto)) {
			ap->off = prevo;
			ap->alen = nexto - prevo;
		}
		/*
		 * If the result isn't a multiple of rtextents we need to
		 * remove blocks until it is.
		 */
		if ((temp = (ap->alen % mp->m_sb.sb_rextsize))) {
			/*
			 * We're not covering the original request, or
			 * we won't be able to once we fix the length.
			 */
			if (orig_off < ap->off ||
			    orig_end > ap->off + ap->alen ||
			    ap->alen - temp < orig_alen)
				return XFS_ERROR(EINVAL);
			/*
			 * Try to fix it by moving the start up.
			 */
			if (ap->off + temp <= orig_off) {
				ap->alen -= temp;
				ap->off += temp;
			}
			/*
			 * Try to fix it by moving the end in.
			 */
			else if (ap->off + ap->alen - temp >= orig_end)
				ap->alen -= temp;
			/*
			 * Set the start to the minimum then trim the length.
			 */
			else {
				ap->alen -= orig_off - ap->off;
				ap->off = orig_off;
				ap->alen -= ap->alen % mp->m_sb.sb_rextsize;
			}
			/*
			 * Result doesn't cover the request, fail it.
			 */
			if (orig_off < ap->off || orig_end > ap->off + ap->alen)
				return XFS_ERROR(EINVAL);
		}
		ASSERT(ap->alen % mp->m_sb.sb_rextsize == 0);
		/*
		 * If the offset & length are not perfectly aligned
		 * then kill prod, it will just get us in trouble.
		 */
		if (do_mod(ap->off, extsz) || ap->alen % extsz)
			prod = 1;
		/*
		 * Set ralen to be the actual requested length in rtextents.
		 */
		ralen = ap->alen / mp->m_sb.sb_rextsize;
		/*
		 * If the old value was close enough to MAXEXTLEN that
		 * we rounded up to it, cut it back so it's legal again.
		 * Note that if it's a really large request (bigger than
		 * MAXEXTLEN), we don't hear about that number, and can't
		 * adjust the starting point to match it.
		 */
		if (ralen * mp->m_sb.sb_rextsize >= MAXEXTLEN)
			ralen = MAXEXTLEN / mp->m_sb.sb_rextsize;
		/*
		 * If it's an allocation to an empty file at offset 0,
		 * pick an extent that will space things out in the rt area.
		 */
		if (ap->eof && ap->off == 0) {
			error = xfs_rtpick_extent(mp, ap->tp, ralen, &rtx);
			if (error)
				return error;
			ap->rval = rtx * mp->m_sb.sb_rextsize;
		} else
			ap->rval = 0;
	}
#else
	if (rt)
		ap->rval = 0;
#endif	/* __KERNEL__ */
	else if (nullfb)
		ap->rval = XFS_INO_TO_FSB(mp, ap->ip->i_ino);
	else
		ap->rval = ap->firstblock;
	/*
	 * If allocating at eof, and there's a previous real block,
	 * try to use it's last block as our starting point.
	 */
	if (ap->eof && ap->prevp->br_startoff != NULLFILEOFF &&
	    !ISNULLSTARTBLOCK(ap->prevp->br_startblock) &&
	    ISLEGAL(ap->prevp->br_startblock + ap->prevp->br_blockcount,
		    ap->prevp->br_startblock)) {
		ap->rval = ap->prevp->br_startblock + ap->prevp->br_blockcount;
		/*
		 * Adjust for the gap between prevp and us.
		 */
		adjust = ap->off -
			(ap->prevp->br_startoff + ap->prevp->br_blockcount);
		if (adjust &&
		    ISLEGAL(ap->rval + adjust, ap->prevp->br_startblock))
			ap->rval += adjust;
	}
	/*
	 * If not at eof, then compare the two neighbor blocks.
	 * Figure out whether either one gives us a good starting point,
	 * and pick the better one.
	 */
	else if (!ap->eof) {
		xfs_fsblock_t	gotbno;		/* right side block number */
		xfs_fsblock_t	gotdiff=0;	/* right side difference */
		xfs_fsblock_t	prevbno;	/* left side block number */
		xfs_fsblock_t	prevdiff=0;	/* left side difference */

		/*
		 * If there's a previous (left) block, select a requested
		 * start block based on it.
		 */
		if (ap->prevp->br_startoff != NULLFILEOFF &&
		    !ISNULLSTARTBLOCK(ap->prevp->br_startblock) &&
		    (prevbno = ap->prevp->br_startblock +
			       ap->prevp->br_blockcount) &&
		    ISLEGAL(prevbno, ap->prevp->br_startblock)) {
			/*
			 * Calculate gap to end of previous block.
			 */
			adjust = prevdiff = ap->off -
				(ap->prevp->br_startoff +
				 ap->prevp->br_blockcount);
			/*
			 * Figure the startblock based on the previous block's
			 * end and the gap size.
			 * Heuristic!
			 * If the gap is large relative to the piece we're
			 * allocating, or using it gives us an illegal block
			 * number, then just use the end of the previous block.
			 */
			if (prevdiff <= XFS_ALLOC_GAP_UNITS * ap->alen &&
			    ISLEGAL(prevbno + prevdiff,
				    ap->prevp->br_startblock))
				prevbno += adjust;
			else
				prevdiff += adjust;
			/*
			 * If the firstblock forbids it, can't use it,
			 * must use default.
			 */
			if (!rt && !nullfb &&
			    XFS_FSB_TO_AGNO(mp, prevbno) != fb_agno)
				prevbno = NULLFSBLOCK;
		}
		/*
		 * No previous block or can't follow it, just default.
		 */
		else
			prevbno = NULLFSBLOCK;
		/*
		 * If there's a following (right) block, select a requested
		 * start block based on it.
		 */
		if (!ISNULLSTARTBLOCK(ap->gotp->br_startblock)) {
			/*
			 * Calculate gap to start of next block.
			 */
			adjust = gotdiff = ap->gotp->br_startoff - ap->off;
			/*
			 * Figure the startblock based on the next block's
			 * start and the gap size.
			 */
			gotbno = ap->gotp->br_startblock;
			/*
			 * Heuristic!
			 * If the gap is large relative to the piece we're
			 * allocating, or using it gives us an illegal block
			 * number, then just use the start of the next block
			 * offset by our length.
			 */
			if (gotdiff <= XFS_ALLOC_GAP_UNITS * ap->alen &&
			    ISLEGAL(gotbno - gotdiff, gotbno))
				gotbno -= adjust;
			else if (ISLEGAL(gotbno - ap->alen, gotbno)) {
				gotbno -= ap->alen;
				gotdiff += adjust - ap->alen;
			} else
				gotdiff += adjust;
			/*
			 * If the firstblock forbids it, can't use it,
			 * must use default.
			 */
			if (!rt && !nullfb &&
			    XFS_FSB_TO_AGNO(mp, gotbno) != fb_agno)
				gotbno = NULLFSBLOCK;
		}
		/*
		 * No next block, just default.
		 */
		else
			gotbno = NULLFSBLOCK;
		/*
		 * If both valid, pick the better one, else the only good
		 * one, else ap->rval is already set (to 0 or the inode block).
		 */
		if (prevbno != NULLFSBLOCK && gotbno != NULLFSBLOCK)
			ap->rval = prevdiff <= gotdiff ? prevbno : gotbno;
		else if (prevbno != NULLFSBLOCK)
			ap->rval = prevbno;
		else if (gotbno != NULLFSBLOCK)
			ap->rval = gotbno;
	}
	/*
	 * If allowed, use ap->rval; otherwise must use firstblock since
	 * it's in the right allocation group.
	 */
	if (nullfb || rt || XFS_FSB_TO_AGNO(mp, ap->rval) == fb_agno)
		;
	else
		ap->rval = ap->firstblock;
	/*
	 * Realtime allocation, done through xfs_rtallocate_extent.
	 */
	if (rt) {
#ifndef __KERNEL__
		ASSERT(0);
#else
		xfs_rtblock_t	rtb;

		atype = ap->rval == 0 ?
			XFS_ALLOCTYPE_ANY_AG : XFS_ALLOCTYPE_NEAR_BNO;
		do_div(ap->rval, mp->m_sb.sb_rextsize);
		rtb = ap->rval;
		ap->alen = ralen;
		if ((error = xfs_rtallocate_extent(ap->tp, ap->rval, 1, ap->alen,
				&ralen, atype, ap->wasdel, prod, &rtb)))
			return error;
		if (rtb == NULLFSBLOCK && prod > 1 &&
		    (error = xfs_rtallocate_extent(ap->tp, ap->rval, 1,
						   ap->alen, &ralen, atype,
						   ap->wasdel, 1, &rtb)))
			return error;
		ap->rval = rtb;
		if (ap->rval != NULLFSBLOCK) {
			ap->rval *= mp->m_sb.sb_rextsize;
			ralen *= mp->m_sb.sb_rextsize;
			ap->alen = ralen;
			ap->ip->i_d.di_nblocks += ralen;
			xfs_trans_log_inode(ap->tp, ap->ip, XFS_ILOG_CORE);
			if (ap->wasdel)
				ap->ip->i_delayed_blks -= ralen;
			/*
			 * Adjust the disk quota also. This was reserved
			 * earlier.
			 */
			XFS_TRANS_MOD_DQUOT_BYINO(mp, ap->tp, ap->ip,
				ap->wasdel ? XFS_TRANS_DQ_DELRTBCOUNT :
						XFS_TRANS_DQ_RTBCOUNT,
				(long) ralen);
		} else
			ap->alen = 0;
#endif	/* __KERNEL__ */
	}
	/*
	 * Normal allocation, done through xfs_alloc_vextent.
	 */
	else {
		xfs_agnumber_t	ag;
		xfs_alloc_arg_t	args;
		xfs_extlen_t	blen;
		xfs_extlen_t	delta;
		int		isaligned;
		xfs_extlen_t	longest;
		xfs_extlen_t	need;
		xfs_extlen_t	nextminlen=0;
		int		notinit;
		xfs_perag_t	*pag;
		xfs_agnumber_t	startag;
		int		tryagain;

		tryagain = isaligned = 0;
		args.tp = ap->tp;
		args.mp = mp;
		args.fsbno = ap->rval;
		args.maxlen = MIN(ap->alen, mp->m_sb.sb_agblocks);
		blen = 0;
		if (nullfb) {
			args.type = XFS_ALLOCTYPE_START_BNO;
			args.total = ap->total;
			/*
			 * Find the longest available space.
			 * We're going to try for the whole allocation at once.
			 */
			startag = ag = XFS_FSB_TO_AGNO(mp, args.fsbno);
			notinit = 0;
			down_read(&mp->m_peraglock);
			while (blen < ap->alen) {
				pag = &mp->m_perag[ag];
				if (!pag->pagf_init &&
				    (error = xfs_alloc_pagf_init(mp, args.tp,
					    ag, XFS_ALLOC_FLAG_TRYLOCK))) {
					up_read(&mp->m_peraglock);
					return error;
				}
				/*
				 * See xfs_alloc_fix_freelist...
				 */
				if (pag->pagf_init) {
					need = XFS_MIN_FREELIST_PAG(pag, mp);
					delta = need > pag->pagf_flcount ?
						need - pag->pagf_flcount : 0;
					longest = (pag->pagf_longest > delta) ?
						(pag->pagf_longest - delta) :
						(pag->pagf_flcount > 0 ||
						 pag->pagf_longest > 0);
					if (blen < longest)
						blen = longest;
				} else
					notinit = 1;
				if (++ag == mp->m_sb.sb_agcount)
					ag = 0;
				if (ag == startag)
					break;
			}
			up_read(&mp->m_peraglock);
			/*
			 * Since the above loop did a BUF_TRYLOCK, it is
			 * possible that there is space for this request.
			 */
			if (notinit || blen < ap->minlen)
				args.minlen = ap->minlen;
			/*
			 * If the best seen length is less than the request
			 * length, use the best as the minimum.
			 */
			else if (blen < ap->alen)
				args.minlen = blen;
			/*
			 * Otherwise we've seen an extent as big as alen,
			 * use that as the minimum.
			 */
			else
				args.minlen = ap->alen;
		} else if (ap->low) {
			args.type = XFS_ALLOCTYPE_FIRST_AG;
			args.total = args.minlen = ap->minlen;
		} else {
			args.type = XFS_ALLOCTYPE_NEAR_BNO;
			args.total = ap->total;
			args.minlen = ap->minlen;
		}
		if (ap->ip->i_d.di_extsize) {
			args.prod = ap->ip->i_d.di_extsize;
			if ((args.mod = (xfs_extlen_t)do_mod(ap->off, args.prod)))
				args.mod = (xfs_extlen_t)(args.prod - args.mod);
		} else if (mp->m_sb.sb_blocksize >= NBPP) {
			args.prod = 1;
			args.mod = 0;
		} else {
			args.prod = NBPP >> mp->m_sb.sb_blocklog;
			if ((args.mod = (xfs_extlen_t)(do_mod(ap->off, args.prod))))
				args.mod = (xfs_extlen_t)(args.prod - args.mod);
		}
		/*
		 * If we are not low on available data blocks, and the
		 * underlying logical volume manager is a stripe, and
		 * the file offset is zero then try to allocate data
		 * blocks on stripe unit boundary.
		 * NOTE: ap->aeof is only set if the allocation length
		 * is >= the stripe unit and the allocation offset is
		 * at the end of file.
		 */
		if (!ap->low && ap->aeof) {
			if (!ap->off) {
				args.alignment = mp->m_dalign;
				atype = args.type;
				isaligned = 1;
				/*
				 * Adjust for alignment
				 */
				if (blen > args.alignment && blen <= ap->alen)
					args.minlen = blen - args.alignment;
				args.minalignslop = 0;
			} else {
				/*
				 * First try an exact bno allocation.
				 * If it fails then do a near or start bno
				 * allocation with alignment turned on.
				 */
				atype = args.type;
				tryagain = 1;
				args.type = XFS_ALLOCTYPE_THIS_BNO;
				args.alignment = 1;
				/*
				 * Compute the minlen+alignment for the
				 * next case.  Set slop so that the value
				 * of minlen+alignment+slop doesn't go up
				 * between the calls.
				 */
				if (blen > mp->m_dalign && blen <= ap->alen)
					nextminlen = blen - mp->m_dalign;
				else
					nextminlen = args.minlen;
				if (nextminlen + mp->m_dalign > args.minlen + 1)
					args.minalignslop =
						nextminlen + mp->m_dalign -
						args.minlen - 1;
				else
					args.minalignslop = 0;
			}
		} else {
			args.alignment = 1;
			args.minalignslop = 0;
		}
		args.minleft = ap->minleft;
		args.wasdel = ap->wasdel;
		args.isfl = 0;
		args.userdata = ap->userdata;
		if ((error = xfs_alloc_vextent(&args)))
			return error;
		if (tryagain && args.fsbno == NULLFSBLOCK) {
			/*
			 * Exact allocation failed. Now try with alignment
			 * turned on.
			 */
			args.type = atype;
			args.fsbno = ap->rval;
			args.alignment = mp->m_dalign;
			args.minlen = nextminlen;
			args.minalignslop = 0;
			isaligned = 1;
			if ((error = xfs_alloc_vextent(&args)))
				return error;
		}
		if (isaligned && args.fsbno == NULLFSBLOCK) {
			/*
			 * allocation failed, so turn off alignment and
			 * try again.
			 */
			args.type = atype;
			args.fsbno = ap->rval;
			args.alignment = 0;
			if ((error = xfs_alloc_vextent(&args)))
				return error;
		}
		if (args.fsbno == NULLFSBLOCK && nullfb &&
		    args.minlen > ap->minlen) {
			args.minlen = ap->minlen;
			args.type = XFS_ALLOCTYPE_START_BNO;
			args.fsbno = ap->rval;
			if ((error = xfs_alloc_vextent(&args)))
				return error;
		}
		if (args.fsbno == NULLFSBLOCK && nullfb) {
			args.fsbno = 0;
			args.type = XFS_ALLOCTYPE_FIRST_AG;
			args.total = ap->minlen;
			args.minleft = 0;
			if ((error = xfs_alloc_vextent(&args)))
				return error;
			ap->low = 1;
		}
		if (args.fsbno != NULLFSBLOCK) {
			ap->firstblock = ap->rval = args.fsbno;
			ASSERT(nullfb || fb_agno == args.agno ||
			       (ap->low && fb_agno < args.agno));
			ap->alen = args.len;
			ap->ip->i_d.di_nblocks += args.len;
			xfs_trans_log_inode(ap->tp, ap->ip, XFS_ILOG_CORE);
			if (ap->wasdel)
				ap->ip->i_delayed_blks -= args.len;
			/*
			 * Adjust the disk quota also. This was reserved
			 * earlier.
			 */
			XFS_TRANS_MOD_DQUOT_BYINO(mp, ap->tp, ap->ip,
				ap->wasdel ? XFS_TRANS_DQ_DELBCOUNT :
						XFS_TRANS_DQ_BCOUNT,
				(long) args.len);
		} else {
			ap->rval = NULLFSBLOCK;
			ap->alen = 0;
		}
	}
	return 0;
#undef	ISLEGAL
}

/*
 * Transform a btree format file with only one leaf node, where the
 * extents list will fit in the inode, into an extents format file.
 * Since the extent list is already in-core, all we have to do is
 * give up the space for the btree root and pitch the leaf block.
 */
STATIC int				/* error */
xfs_bmap_btree_to_extents(
	xfs_trans_t		*tp,	/* transaction pointer */
	xfs_inode_t		*ip,	/* incore inode pointer */
	xfs_btree_cur_t		*cur,	/* btree cursor */
	int			*logflagsp, /* inode logging flags */
	int			whichfork)  /* data or attr fork */
{
	/* REFERENCED */
	xfs_bmbt_block_t	*cblock;/* child btree block */
	xfs_fsblock_t		cbno;	/* child block number */
	xfs_buf_t		*cbp;	/* child block's buffer */
	int			error;	/* error return value */
	xfs_ifork_t		*ifp;	/* inode fork data */
	xfs_mount_t		*mp;	/* mount point structure */
	xfs_bmbt_ptr_t		*pp;	/* ptr to block address */
	xfs_bmbt_block_t	*rblock;/* root btree block */

	ifp = XFS_IFORK_PTR(ip, whichfork);
	ASSERT(ifp->if_flags & XFS_IFEXTENTS);
	ASSERT(XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_BTREE);
	rblock = ifp->if_broot;
	ASSERT(INT_GET(rblock->bb_level, ARCH_CONVERT) == 1);
	ASSERT(INT_GET(rblock->bb_numrecs, ARCH_CONVERT) == 1);
	ASSERT(XFS_BMAP_BROOT_MAXRECS(ifp->if_broot_bytes) == 1);
	mp = ip->i_mount;
	pp = XFS_BMAP_BROOT_PTR_ADDR(rblock, 1, ifp->if_broot_bytes);
	*logflagsp = 0;
#ifdef DEBUG
	if ((error = xfs_btree_check_lptr(cur, INT_GET(*pp, ARCH_CONVERT), 1)))
		return error;
#endif
	cbno = INT_GET(*pp, ARCH_CONVERT);
	if ((error = xfs_btree_read_bufl(mp, tp, cbno, 0, &cbp,
			XFS_BMAP_BTREE_REF)))
		return error;
	cblock = XFS_BUF_TO_BMBT_BLOCK(cbp);
	if ((error = xfs_btree_check_lblock(cur, cblock, 0, cbp)))
		return error;
	xfs_bmap_add_free(cbno, 1, cur->bc_private.b.flist, mp);
	ip->i_d.di_nblocks--;
	XFS_TRANS_MOD_DQUOT_BYINO(mp, tp, ip, XFS_TRANS_DQ_BCOUNT, -1L);
	xfs_trans_binval(tp, cbp);
	if (cur->bc_bufs[0] == cbp)
		cur->bc_bufs[0] = NULL;
	xfs_iroot_realloc(ip, -1, whichfork);
	ASSERT(ifp->if_broot == NULL);
	ASSERT((ifp->if_flags & XFS_IFBROOT) == 0);
	XFS_IFORK_FMT_SET(ip, whichfork, XFS_DINODE_FMT_EXTENTS);
	*logflagsp = XFS_ILOG_CORE | XFS_ILOG_FEXT(whichfork);
	return 0;
}

/*
 * Called by xfs_bmapi to update extent list structure and the btree
 * after removing space (or undoing a delayed allocation).
 */
STATIC int				/* error */
xfs_bmap_del_extent(
	xfs_inode_t		*ip,	/* incore inode pointer */
	xfs_trans_t		*tp,	/* current transaction pointer */
	xfs_extnum_t		idx,	/* extent number to update/delete */
	xfs_bmap_free_t		*flist,	/* list of extents to be freed */
	xfs_btree_cur_t		*cur,	/* if null, not a btree */
	xfs_bmbt_irec_t		*del,	/* data to remove from extent list */
	int			*logflagsp, /* inode logging flags */
	int			whichfork, /* data or attr fork */
	int			rsvd)	/* OK to allocate reserved blocks */
{
	xfs_filblks_t		da_new;	/* new delay-alloc indirect blocks */
	xfs_filblks_t		da_old;	/* old delay-alloc indirect blocks */
	xfs_fsblock_t		del_endblock=0;	/* first block past del */
	xfs_fileoff_t		del_endoff;	/* first offset past del */
	int			delay;	/* current block is delayed allocated */
	int			do_fx;	/* free extent at end of routine */
	xfs_bmbt_rec_t		*ep;	/* current extent entry pointer */
	int			error;	/* error return value */
	int			flags;	/* inode logging flags */
#ifdef XFS_BMAP_TRACE
	static char		fname[] = "xfs_bmap_del_extent";
#endif
	xfs_bmbt_irec_t		got;	/* current extent entry */
	xfs_fileoff_t		got_endoff;	/* first offset past got */
	int			i;	/* temp state */
	xfs_ifork_t		*ifp;	/* inode fork pointer */
	xfs_mount_t		*mp;	/* mount structure */
	xfs_filblks_t		nblks;	/* quota/sb block count */
	xfs_bmbt_irec_t		new;	/* new record to be inserted */
	/* REFERENCED */
	xfs_extnum_t		nextents;	/* number of extents in list */
	uint			qfield;	/* quota field to update */
	xfs_filblks_t		temp;	/* for indirect length calculations */
	xfs_filblks_t		temp2;	/* for indirect length calculations */

	XFS_STATS_INC(xfsstats.xs_del_exlist);
	mp = ip->i_mount;
	ifp = XFS_IFORK_PTR(ip, whichfork);
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	ASSERT(idx >= 0 && idx < nextents);
	ASSERT(del->br_blockcount > 0);
	ep = &ifp->if_u1.if_extents[idx];
	xfs_bmbt_get_all(ep, &got);
	ASSERT(got.br_startoff <= del->br_startoff);
	del_endoff = del->br_startoff + del->br_blockcount;
	got_endoff = got.br_startoff + got.br_blockcount;
	ASSERT(got_endoff >= del_endoff);
	delay = ISNULLSTARTBLOCK(got.br_startblock);
	ASSERT(ISNULLSTARTBLOCK(del->br_startblock) == delay);
	flags = 0;
	qfield = 0;
	error = 0;
	/*
	 * If deleting a real allocation, must free up the disk space.
	 */
	if (!delay) {
		flags = XFS_ILOG_CORE;
		/*
		 * Realtime allocation.  Free it and record di_nblocks update.
		 */
		if (whichfork == XFS_DATA_FORK &&
		    (ip->i_d.di_flags & XFS_DIFLAG_REALTIME)) {
			xfs_fsblock_t	bno;
			xfs_filblks_t	len;

			ASSERT(do_mod(del->br_blockcount,
				      mp->m_sb.sb_rextsize) == 0);
			ASSERT(do_mod(del->br_startblock,
				      mp->m_sb.sb_rextsize) == 0);
			bno = del->br_startblock;
			len = del->br_blockcount;
			do_div(bno, mp->m_sb.sb_rextsize);
			do_div(len, mp->m_sb.sb_rextsize);
			if ((error = xfs_rtfree_extent(ip->i_transp, bno,
					(xfs_extlen_t)len)))
				goto done;
			do_fx = 0;
			nblks = len * mp->m_sb.sb_rextsize;
			qfield = XFS_TRANS_DQ_RTBCOUNT;
		}
		/*
		 * Ordinary allocation.
		 */
		else {
			do_fx = 1;
			nblks = del->br_blockcount;
			qfield = XFS_TRANS_DQ_BCOUNT;
		}
		/*
		 * Set up del_endblock and cur for later.
		 */
		del_endblock = del->br_startblock + del->br_blockcount;
		if (cur) {
			if ((error = xfs_bmbt_lookup_eq(cur, got.br_startoff,
					got.br_startblock, got.br_blockcount,
					&i)))
				goto done;
			ASSERT(i == 1);
		}
		da_old = da_new = 0;
	} else {
		da_old = STARTBLOCKVAL(got.br_startblock);
		da_new = 0;
		nblks = 0;
		do_fx = 0;
	}
	/*
	 * Set flag value to use in switch statement.
	 * Left-contig is 2, right-contig is 1.
	 */
	switch (((got.br_startoff == del->br_startoff) << 1) |
		(got_endoff == del_endoff)) {
	case 3:
		/*
		 * Matches the whole extent.  Delete the entry.
		 */
		xfs_bmap_trace_delete(fname, "3", ip, idx, 1, whichfork);
		xfs_bmap_delete_exlist(ip, idx, 1, whichfork);
		ifp->if_lastex = idx;
		if (delay)
			break;
		XFS_IFORK_NEXT_SET(ip, whichfork,
			XFS_IFORK_NEXTENTS(ip, whichfork) - 1);
		flags |= XFS_ILOG_CORE;
		if (!cur) {
			flags |= XFS_ILOG_FEXT(whichfork);
			break;
		}
		if ((error = xfs_bmbt_delete(cur, &i)))
			goto done;
		ASSERT(i == 1);
		break;

	case 2:
		/*
		 * Deleting the first part of the extent.
		 */
		xfs_bmap_trace_pre_update(fname, "2", ip, idx, whichfork);
		xfs_bmbt_set_startoff(ep, del_endoff);
		temp = got.br_blockcount - del->br_blockcount;
		xfs_bmbt_set_blockcount(ep, temp);
		ifp->if_lastex = idx;
		if (delay) {
			temp = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(ip, temp),
				da_old);
			xfs_bmbt_set_startblock(ep, NULLSTARTBLOCK((int)temp));
			xfs_bmap_trace_post_update(fname, "2", ip, idx,
				whichfork);
			da_new = temp;
			break;
		}
		xfs_bmbt_set_startblock(ep, del_endblock);
		xfs_bmap_trace_post_update(fname, "2", ip, idx, whichfork);
		if (!cur) {
			flags |= XFS_ILOG_FEXT(whichfork);
			break;
		}
		if ((error = xfs_bmbt_update(cur, del_endoff, del_endblock,
				got.br_blockcount - del->br_blockcount,
				got.br_state)))
			goto done;
		break;

	case 1:
		/*
		 * Deleting the last part of the extent.
		 */
		temp = got.br_blockcount - del->br_blockcount;
		xfs_bmap_trace_pre_update(fname, "1", ip, idx, whichfork);
		xfs_bmbt_set_blockcount(ep, temp);
		ifp->if_lastex = idx;
		if (delay) {
			temp = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(ip, temp),
				da_old);
			xfs_bmbt_set_startblock(ep, NULLSTARTBLOCK((int)temp));
			xfs_bmap_trace_post_update(fname, "1", ip, idx,
				whichfork);
			da_new = temp;
			break;
		}
		xfs_bmap_trace_post_update(fname, "1", ip, idx, whichfork);
		if (!cur) {
			flags |= XFS_ILOG_FEXT(whichfork);
			break;
		}
		if ((error = xfs_bmbt_update(cur, got.br_startoff,
				got.br_startblock,
				got.br_blockcount - del->br_blockcount,
				got.br_state)))
			goto done;
		break;

	case 0:
		/*
		 * Deleting the middle of the extent.
		 */
		temp = del->br_startoff - got.br_startoff;
		xfs_bmap_trace_pre_update(fname, "0", ip, idx, whichfork);
		xfs_bmbt_set_blockcount(ep, temp);
		new.br_startoff = del_endoff;
		temp2 = got_endoff - del_endoff;
		new.br_blockcount = temp2;
		new.br_state = got.br_state;
		if (!delay) {
			new.br_startblock = del_endblock;
			flags |= XFS_ILOG_CORE;
			if (cur) {
				if ((error = xfs_bmbt_update(cur,
						got.br_startoff,
						got.br_startblock, temp,
						got.br_state)))
					goto done;
				if ((error = xfs_bmbt_increment(cur, 0, &i)))
					goto done;
				cur->bc_rec.b = new;
				error = xfs_bmbt_insert(cur, &i);
				if (error && error != ENOSPC)
					goto done;
				/*
				 * If get no-space back from btree insert,
				 * it tried a split, and we have a zero
				 * block reservation.
				 * Fix up our state and return the error.
				 */
				if (error == ENOSPC) {
					/*
					 * Reset the cursor, don't trust
					 * it after any insert operation.
					 */
					if ((error = xfs_bmbt_lookup_eq(cur,
							got.br_startoff,
							got.br_startblock,
							temp, &i)))
						goto done;
					ASSERT(i == 1);
					/*
					 * Update the btree record back
					 * to the original value.
					 */
					if ((error = xfs_bmbt_update(cur,
							got.br_startoff,
							got.br_startblock,
							got.br_blockcount,
							got.br_state)))
						goto done;
					/*
					 * Reset the extent record back
					 * to the original value.
					 */
					xfs_bmbt_set_blockcount(ep,
						got.br_blockcount);
					flags = 0;
					error = XFS_ERROR(ENOSPC);
					goto done;
				}
				ASSERT(i == 1);
			} else
				flags |= XFS_ILOG_FEXT(whichfork);
			XFS_IFORK_NEXT_SET(ip, whichfork,
				XFS_IFORK_NEXTENTS(ip, whichfork) + 1);
		} else {
			ASSERT(whichfork == XFS_DATA_FORK);
			temp = xfs_bmap_worst_indlen(ip, temp);
			xfs_bmbt_set_startblock(ep, NULLSTARTBLOCK((int)temp));
			temp2 = xfs_bmap_worst_indlen(ip, temp2);
			new.br_startblock = NULLSTARTBLOCK((int)temp2);
			da_new = temp + temp2;
			while (da_new > da_old) {
				if (temp) {
					temp--;
					da_new--;
					xfs_bmbt_set_startblock(ep,
						NULLSTARTBLOCK((int)temp));
				}
				if (da_new == da_old)
					break;
				if (temp2) {
					temp2--;
					da_new--;
					new.br_startblock =
						NULLSTARTBLOCK((int)temp2);
				}
			}
		}
		xfs_bmap_trace_post_update(fname, "0", ip, idx, whichfork);
		xfs_bmap_trace_insert(fname, "0", ip, idx + 1, 1, &new, NULL,
			whichfork);
		xfs_bmap_insert_exlist(ip, idx + 1, 1, &new, whichfork);
		ifp->if_lastex = idx + 1;
		break;
	}
	/*
	 * If we need to, add to list of extents to delete.
	 */
	if (do_fx)
		xfs_bmap_add_free(del->br_startblock, del->br_blockcount, flist,
			mp);
	/*
	 * Adjust inode # blocks in the file.
	 */
	if (nblks)
		ip->i_d.di_nblocks -= nblks;
	/*
	 * Adjust quota data.
	 */
	if (qfield)
		XFS_TRANS_MOD_DQUOT_BYINO(mp, tp, ip, qfield, (long)-nblks);

	/*
	 * Account for change in delayed indirect blocks.
	 * Nothing to do for disk quota accounting here.
	 */
	ASSERT(da_old >= da_new);
	if (da_old > da_new)
		xfs_mod_incore_sb(mp, XFS_SBS_FDBLOCKS, (int)(da_old - da_new),
			rsvd);
done:
	*logflagsp = flags;
	return error;
}

/*
 * Remove the entry "free" from the free item list.  Prev points to the
 * previous entry, unless "free" is the head of the list.
 */
STATIC void
xfs_bmap_del_free(
	xfs_bmap_free_t		*flist,	/* free item list header */
	xfs_bmap_free_item_t	*prev,	/* previous item on list, if any */
	xfs_bmap_free_item_t	*free)	/* list item to be freed */
{
	if (prev)
		prev->xbfi_next = free->xbfi_next;
	else
		flist->xbf_first = free->xbfi_next;
	flist->xbf_count--;
	kmem_zone_free(xfs_bmap_free_item_zone, free);
}

/*
 * Remove count entries from the extents array for inode "ip", starting
 * at index "idx".  Copies the remaining items down over the deleted ones,
 * and gives back the excess memory.
 */
STATIC void
xfs_bmap_delete_exlist(
	xfs_inode_t	*ip,		/* incore inode pointer */
	xfs_extnum_t	idx,		/* starting delete index */
	xfs_extnum_t	count,		/* count of items to delete */
	int		whichfork)	/* data or attr fork */
{
	xfs_bmbt_rec_t	*base;		/* base of extent list */
	xfs_ifork_t	*ifp;		/* inode fork pointer */
	xfs_extnum_t	nextents;	/* number of extents in list after */

	ifp = XFS_IFORK_PTR(ip, whichfork);
	ASSERT(ifp->if_flags & XFS_IFEXTENTS);
	base = ifp->if_u1.if_extents;
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t) - count;
	memmove(&base[idx], &base[idx + count],
		(nextents - idx) * sizeof(*base));
	xfs_iext_realloc(ip, -count, whichfork);
}

/*
 * Convert an extents-format file into a btree-format file.
 * The new file will have a root block (in the inode) and a single child block.
 */
STATIC int					/* error */
xfs_bmap_extents_to_btree(
	xfs_trans_t		*tp,		/* transaction pointer */
	xfs_inode_t		*ip,		/* incore inode pointer */
	xfs_fsblock_t		*firstblock,	/* first-block-allocated */
	xfs_bmap_free_t		*flist,		/* blocks freed in xaction */
	xfs_btree_cur_t		**curp,		/* cursor returned to caller */
	int			wasdel,		/* converting a delayed alloc */
	int			*logflagsp,	/* inode logging flags */
	int			whichfork)	/* data or attr fork */
{
	xfs_bmbt_block_t	*ablock;	/* allocated (child) bt block */
	xfs_buf_t		*abp;		/* buffer for ablock */
	xfs_alloc_arg_t		args;		/* allocation arguments */
	xfs_bmbt_rec_t		*arp;		/* child record pointer */
	xfs_bmbt_block_t	*block;		/* btree root block */
	xfs_btree_cur_t		*cur;		/* bmap btree cursor */
	xfs_bmbt_rec_t		*ep;		/* extent list pointer */
	int			error;		/* error return value */
	xfs_extnum_t		i, cnt;		/* extent list index */
	xfs_ifork_t		*ifp;		/* inode fork pointer */
	xfs_bmbt_key_t		*kp;		/* root block key pointer */
	xfs_mount_t		*mp;		/* mount structure */
	xfs_extnum_t		nextents;	/* extent list size */
	xfs_bmbt_ptr_t		*pp;		/* root block address pointer */

	ifp = XFS_IFORK_PTR(ip, whichfork);
	ASSERT(XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_EXTENTS);
	ASSERT(ifp->if_ext_max ==
	       XFS_IFORK_SIZE(ip, whichfork) / (uint)sizeof(xfs_bmbt_rec_t));
	/*
	 * Make space in the inode incore.
	 */
	xfs_iroot_realloc(ip, 1, whichfork);
	ifp->if_flags |= XFS_IFBROOT;
	/*
	 * Fill in the root.
	 */
	block = ifp->if_broot;
	INT_SET(block->bb_magic, ARCH_CONVERT, XFS_BMAP_MAGIC);
	INT_SET(block->bb_level, ARCH_CONVERT, 1);
	INT_SET(block->bb_numrecs, ARCH_CONVERT, 1);
	INT_SET(block->bb_leftsib, ARCH_CONVERT, NULLDFSBNO);
	INT_SET(block->bb_rightsib, ARCH_CONVERT, NULLDFSBNO);
	/*
	 * Need a cursor.  Can't allocate until bb_level is filled in.
	 */
	mp = ip->i_mount;
	cur = xfs_btree_init_cursor(mp, tp, NULL, 0, XFS_BTNUM_BMAP, ip,
		whichfork);
	cur->bc_private.b.firstblock = *firstblock;
	cur->bc_private.b.flist = flist;
	cur->bc_private.b.flags = wasdel ? XFS_BTCUR_BPRV_WASDEL : 0;
	/*
	 * Convert to a btree with two levels, one record in root.
	 */
	XFS_IFORK_FMT_SET(ip, whichfork, XFS_DINODE_FMT_BTREE);
	args.tp = tp;
	args.mp = mp;
	if (*firstblock == NULLFSBLOCK) {
		args.type = XFS_ALLOCTYPE_START_BNO;
		args.fsbno = XFS_INO_TO_FSB(mp, ip->i_ino);
	} else if (flist->xbf_low) {
		args.type = XFS_ALLOCTYPE_START_BNO;
		args.fsbno = *firstblock;
	} else {
		args.type = XFS_ALLOCTYPE_NEAR_BNO;
		args.fsbno = *firstblock;
	}
	args.minlen = args.maxlen = args.prod = 1;
	args.total = args.minleft = args.alignment = args.mod = args.isfl =
		args.minalignslop = 0;
	args.wasdel = wasdel;
	*logflagsp = 0;
	if ((error = xfs_alloc_vextent(&args))) {
		xfs_iroot_realloc(ip, -1, whichfork);
		xfs_btree_del_cursor(cur, XFS_BTREE_ERROR);
		return error;
	}
	/*
	 * Allocation can't fail, the space was reserved.
	 */
	ASSERT(args.fsbno != NULLFSBLOCK);
	ASSERT(*firstblock == NULLFSBLOCK ||
	       args.agno == XFS_FSB_TO_AGNO(mp, *firstblock) ||
	       (flist->xbf_low &&
		args.agno > XFS_FSB_TO_AGNO(mp, *firstblock)));
	*firstblock = cur->bc_private.b.firstblock = args.fsbno;
	cur->bc_private.b.allocated++;
	ip->i_d.di_nblocks++;
	XFS_TRANS_MOD_DQUOT_BYINO(mp, tp, ip, XFS_TRANS_DQ_BCOUNT, 1L);
	abp = xfs_btree_get_bufl(mp, tp, args.fsbno, 0);
	/*
	 * Fill in the child block.
	 */
	ablock = XFS_BUF_TO_BMBT_BLOCK(abp);
	INT_SET(ablock->bb_magic, ARCH_CONVERT, XFS_BMAP_MAGIC);
	INT_ZERO(ablock->bb_level, ARCH_CONVERT);
	INT_SET(ablock->bb_leftsib, ARCH_CONVERT, NULLDFSBNO);
	INT_SET(ablock->bb_rightsib, ARCH_CONVERT, NULLDFSBNO);
	arp = XFS_BMAP_REC_IADDR(ablock, 1, cur);
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	for (ep = ifp->if_u1.if_extents, cnt = i = 0; i < nextents; i++, ep++) {
		if (!ISNULLSTARTBLOCK(xfs_bmbt_get_startblock(ep))) {
			arp->l0 = INT_GET(ep->l0, ARCH_CONVERT);
			arp->l1 = INT_GET(ep->l1, ARCH_CONVERT);
			arp++; cnt++;
		}
	}
	INT_SET(ablock->bb_numrecs, ARCH_CONVERT, cnt);
	ASSERT(INT_GET(ablock->bb_numrecs, ARCH_CONVERT) == XFS_IFORK_NEXTENTS(ip, whichfork));
	/*
	 * Fill in the root key and pointer.
	 */
	kp = XFS_BMAP_KEY_IADDR(block, 1, cur);
	arp = XFS_BMAP_REC_IADDR(ablock, 1, cur);
	INT_SET(kp->br_startoff, ARCH_CONVERT, xfs_bmbt_disk_get_startoff(arp));
	pp = XFS_BMAP_PTR_IADDR(block, 1, cur);
	INT_SET(*pp, ARCH_CONVERT, args.fsbno);
	/*
	 * Do all this logging at the end so that
	 * the root is at the right level.
	 */
	xfs_bmbt_log_block(cur, abp, XFS_BB_ALL_BITS);
	xfs_bmbt_log_recs(cur, abp, 1, INT_GET(ablock->bb_numrecs, ARCH_CONVERT));
	ASSERT(*curp == NULL);
	*curp = cur;
	*logflagsp = XFS_ILOG_CORE | XFS_ILOG_FBROOT(whichfork);
	return 0;
}

/*
 * Insert new item(s) in the extent list for inode "ip".
 * Count new items are inserted at offset idx.
 */
STATIC void
xfs_bmap_insert_exlist(
	xfs_inode_t	*ip,		/* incore inode pointer */
	xfs_extnum_t	idx,		/* starting index of new items */
	xfs_extnum_t	count,		/* number of inserted items */
	xfs_bmbt_irec_t	*new,		/* items to insert */
	int		whichfork)	/* data or attr fork */
{
	xfs_bmbt_rec_t	*base;		/* extent list base */
	xfs_ifork_t	*ifp;		/* inode fork pointer */
	xfs_extnum_t	nextents;	/* extent list size */
	xfs_extnum_t	to;		/* extent list index */

	ifp = XFS_IFORK_PTR(ip, whichfork);
	ASSERT(ifp->if_flags & XFS_IFEXTENTS);
	xfs_iext_realloc(ip, count, whichfork);
	base = ifp->if_u1.if_extents;
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	memmove(&base[idx + count], &base[idx],
		(nextents - (idx + count)) * sizeof(*base));
	for (to = idx; to < idx + count; to++, new++)
		xfs_bmbt_set_all(&base[to], new);
}

/*
 * Convert a local file to an extents file.
 * This code is out of bounds for data forks of regular files,
 * since the file data needs to get logged so things will stay consistent.
 * (The bmap-level manipulations are ok, though).
 */
STATIC int				/* error */
xfs_bmap_local_to_extents(
	xfs_trans_t	*tp,		/* transaction pointer */
	xfs_inode_t	*ip,		/* incore inode pointer */
	xfs_fsblock_t	*firstblock,	/* first block allocated in xaction */
	xfs_extlen_t	total,		/* total blocks needed by transaction */
	int		*logflagsp,	/* inode logging flags */
	int		whichfork)	/* data or attr fork */
{
	int		error;		/* error return value */
	int		flags;		/* logging flags returned */
#ifdef XFS_BMAP_TRACE
	static char	fname[] = "xfs_bmap_local_to_extents";
#endif
	xfs_ifork_t	*ifp;		/* inode fork pointer */

	/*
	 * We don't want to deal with the case of keeping inode data inline yet.
	 * So sending the data fork of a regular inode is illegal.
	 */
	ASSERT(!((ip->i_d.di_mode & IFMT) == IFREG &&
		 whichfork == XFS_DATA_FORK));
	ifp = XFS_IFORK_PTR(ip, whichfork);
	ASSERT(XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_LOCAL);
	flags = 0;
	error = 0;
	if (ifp->if_bytes) {
		xfs_alloc_arg_t	args;	/* allocation arguments */
		xfs_buf_t	*bp;	/* buffer for extent list block */
		xfs_bmbt_rec_t	*ep;	/* extent list pointer */

		args.tp = tp;
		args.mp = ip->i_mount;
		ASSERT(ifp->if_flags & XFS_IFINLINE);
		/*
		 * Allocate a block.  We know we need only one, since the
		 * file currently fits in an inode.
		 */
		if (*firstblock == NULLFSBLOCK) {
			args.fsbno = XFS_INO_TO_FSB(args.mp, ip->i_ino);
			args.type = XFS_ALLOCTYPE_START_BNO;
		} else {
			args.fsbno = *firstblock;
			args.type = XFS_ALLOCTYPE_NEAR_BNO;
		}
		args.total = total;
		args.mod = args.minleft = args.alignment = args.wasdel =
			args.isfl = args.minalignslop = 0;
		args.minlen = args.maxlen = args.prod = 1;
		if ((error = xfs_alloc_vextent(&args)))
			goto done;
		/*
		 * Can't fail, the space was reserved.
		 */
		ASSERT(args.fsbno != NULLFSBLOCK);
		ASSERT(args.len == 1);
		*firstblock = args.fsbno;
		bp = xfs_btree_get_bufl(args.mp, tp, args.fsbno, 0);
		memcpy((char *)XFS_BUF_PTR(bp), ifp->if_u1.if_data,
			ifp->if_bytes);
		xfs_trans_log_buf(tp, bp, 0, ifp->if_bytes - 1);
		xfs_idata_realloc(ip, -ifp->if_bytes, whichfork);
		xfs_iext_realloc(ip, 1, whichfork);
		ep = ifp->if_u1.if_extents;
		xfs_bmbt_set_allf(ep, 0, args.fsbno, 1, XFS_EXT_NORM);
		xfs_bmap_trace_post_update(fname, "new", ip, 0, whichfork);
		XFS_IFORK_NEXT_SET(ip, whichfork, 1);
		ip->i_d.di_nblocks = 1;
		XFS_TRANS_MOD_DQUOT_BYINO(args.mp, tp, ip,
			XFS_TRANS_DQ_BCOUNT, 1L);
		flags |= XFS_ILOG_FEXT(whichfork);
	} else
		ASSERT(XFS_IFORK_NEXTENTS(ip, whichfork) == 0);
	ifp->if_flags &= ~XFS_IFINLINE;
	ifp->if_flags |= XFS_IFEXTENTS;
	XFS_IFORK_FMT_SET(ip, whichfork, XFS_DINODE_FMT_EXTENTS);
	flags |= XFS_ILOG_CORE;
done:
	*logflagsp = flags;
	return error;
}

xfs_bmbt_rec_t *			/* pointer to found extent entry */
xfs_bmap_do_search_extents(
	xfs_bmbt_rec_t	*base,		/* base of extent list */
	xfs_extnum_t	lastx,		/* last extent index used */
	xfs_extnum_t	nextents,	/* extent list size */
	xfs_fileoff_t	bno,		/* block number searched for */
	int		*eofp,		/* out: end of file found */
	xfs_extnum_t	*lastxp,	/* out: last extent index */
	xfs_bmbt_irec_t	*gotp,		/* out: extent entry found */
	xfs_bmbt_irec_t	*prevp)		/* out: previous extent entry found */
{
	xfs_bmbt_rec_t	*ep;		/* extent list entry pointer */
	xfs_bmbt_irec_t	got;		/* extent list entry, decoded */
	int		high;		/* high index of binary search */
	int		low;		/* low index of binary search */

	if (lastx != NULLEXTNUM && lastx < nextents)
		ep = base + lastx;
	else
		ep = NULL;
	prevp->br_startoff = NULLFILEOFF;
	if (ep && bno >= (got.br_startoff = xfs_bmbt_get_startoff(ep)) &&
	    bno < got.br_startoff +
		  (got.br_blockcount = xfs_bmbt_get_blockcount(ep)))
		*eofp = 0;
	else if (ep && lastx < nextents - 1 &&
		 bno >= (got.br_startoff = xfs_bmbt_get_startoff(ep + 1)) &&
		 bno < got.br_startoff +
		       (got.br_blockcount = xfs_bmbt_get_blockcount(ep + 1))) {
		lastx++;
		ep++;
		*eofp = 0;
	} else if (nextents == 0)
		*eofp = 1;
	else if (bno == 0 &&
		 (got.br_startoff = xfs_bmbt_get_startoff(base)) == 0) {
		ep = base;
		lastx = 0;
		got.br_blockcount = xfs_bmbt_get_blockcount(ep);
		*eofp = 0;
	} else {
		/* binary search the extents array */
		low = 0;
		high = nextents - 1;
		while (low <= high) {
			XFS_STATS_INC(xfsstats.xs_cmp_exlist);
			lastx = (low + high) >> 1;
			ep = base + lastx;
			got.br_startoff = xfs_bmbt_get_startoff(ep);
			got.br_blockcount = xfs_bmbt_get_blockcount(ep);
			if (bno < got.br_startoff)
				high = lastx - 1;
			else if (bno >= got.br_startoff + got.br_blockcount)
				low = lastx + 1;
			else {
				got.br_startblock = xfs_bmbt_get_startblock(ep);
				got.br_state = xfs_bmbt_get_state(ep);
				*eofp = 0;
				*lastxp = lastx;
				*gotp = got;
				return ep;
			}
		}
		if (bno >= got.br_startoff + got.br_blockcount) {
			lastx++;
			if (lastx == nextents) {
				*eofp = 1;
				got.br_startblock = xfs_bmbt_get_startblock(ep);
				got.br_state = xfs_bmbt_get_state(ep);
				*prevp = got;
				ep = NULL;
			} else {
				*eofp = 0;
				xfs_bmbt_get_all(ep, prevp);
				ep++;
				got.br_startoff = xfs_bmbt_get_startoff(ep);
				got.br_blockcount = xfs_bmbt_get_blockcount(ep);
			}
		} else {
			*eofp = 0;
			if (ep > base)
				xfs_bmbt_get_all(ep - 1, prevp);
		}
	}
	if (ep) {
		got.br_startblock = xfs_bmbt_get_startblock(ep);
		got.br_state = xfs_bmbt_get_state(ep);
	}
	*lastxp = lastx;
	*gotp = got;
	return ep;
}

/*
 * Search the extents list for the inode, for the extent containing bno.
 * If bno lies in a hole, point to the next entry.  If bno lies past eof,
 * *eofp will be set, and *prevp will contain the last entry (null if none).
 * Else, *lastxp will be set to the index of the found
 * entry; *gotp will contain the entry.
 */
STATIC xfs_bmbt_rec_t *                 /* pointer to found extent entry */
xfs_bmap_search_extents(
	xfs_inode_t     *ip,            /* incore inode pointer */
	xfs_fileoff_t   bno,            /* block number searched for */
	int             whichfork,      /* data or attr fork */
	int             *eofp,          /* out: end of file found */
	xfs_extnum_t    *lastxp,        /* out: last extent index */
	xfs_bmbt_irec_t *gotp,          /* out: extent entry found */
	xfs_bmbt_irec_t *prevp)         /* out: previous extent entry found */
{
	xfs_ifork_t	*ifp;		/* inode fork pointer */
	xfs_bmbt_rec_t  *base;          /* base of extent list */
	xfs_extnum_t    lastx;          /* last extent index used */
	xfs_extnum_t    nextents;       /* extent list size */

	XFS_STATS_INC(xfsstats.xs_look_exlist);
	ifp = XFS_IFORK_PTR(ip, whichfork);
	lastx = ifp->if_lastex;
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	base = &ifp->if_u1.if_extents[0];

	return xfs_bmap_do_search_extents(base, lastx, nextents, bno, eofp,
					  lastxp, gotp, prevp);
}

/*
 * Compute the worst-case number of indirect blocks that will be used
 * for ip's delayed extent of length "len".
 */
STATIC xfs_filblks_t
xfs_bmap_worst_indlen(
	xfs_inode_t	*ip,		/* incore inode pointer */
	xfs_filblks_t	len)		/* delayed extent length */
{
	int		level;		/* btree level number */
	int		maxrecs;	/* maximum record count at this level */
	xfs_mount_t	*mp;		/* mount structure */
	xfs_filblks_t	rval;		/* return value */

	mp = ip->i_mount;
	maxrecs = mp->m_bmap_dmxr[0];
	for (level = 0, rval = 0;
	     level < XFS_BM_MAXLEVELS(mp, XFS_DATA_FORK);
	     level++) {
		len += maxrecs - 1;
		do_div(len, maxrecs);
		rval += len;
		if (len == 1)
			return rval + XFS_BM_MAXLEVELS(mp, XFS_DATA_FORK) -
				level - 1;
		if (level == 0)
			maxrecs = mp->m_bmap_dmxr[1];
	}
	return rval;
}

/*
 * Add the extent to the list of extents to be free at transaction end.
 * The list is maintained sorted (by block number).
 */
/* ARGSUSED */
void
xfs_bmap_add_free(
	xfs_fsblock_t		bno,		/* fs block number of extent */
	xfs_filblks_t		len,		/* length of extent */
	xfs_bmap_free_t		*flist,		/* list of extents */
	xfs_mount_t		*mp)		/* mount point structure */
{
	xfs_bmap_free_item_t	*cur;		/* current (next) element */
	xfs_bmap_free_item_t	*new;		/* new element */
	xfs_bmap_free_item_t	*prev;		/* previous element */
#ifdef DEBUG
	xfs_agnumber_t		agno;
	xfs_agblock_t		agbno;

	ASSERT(bno != NULLFSBLOCK);
	ASSERT(len > 0);
	ASSERT(len <= MAXEXTLEN);
	ASSERT(!ISNULLSTARTBLOCK(bno));
	agno = XFS_FSB_TO_AGNO(mp, bno);
	agbno = XFS_FSB_TO_AGBNO(mp, bno);
	ASSERT(agno < mp->m_sb.sb_agcount);
	ASSERT(agbno < mp->m_sb.sb_agblocks);
	ASSERT(len < mp->m_sb.sb_agblocks);
	ASSERT(agbno + len <= mp->m_sb.sb_agblocks);
#endif
	ASSERT(xfs_bmap_free_item_zone != NULL);
	new = kmem_zone_alloc(xfs_bmap_free_item_zone, KM_SLEEP);
	new->xbfi_startblock = bno;
	new->xbfi_blockcount = (xfs_extlen_t)len;
	for (prev = NULL, cur = flist->xbf_first;
	     cur != NULL;
	     prev = cur, cur = cur->xbfi_next) {
		if (cur->xbfi_startblock >= bno)
			break;
	}
	if (prev)
		prev->xbfi_next = new;
	else
		flist->xbf_first = new;
	new->xbfi_next = cur;
	flist->xbf_count++;
}

/*
 * Compute and fill in the value of the maximum depth of a bmap btree
 * in this filesystem.  Done once, during mount.
 */
void
xfs_bmap_compute_maxlevels(
	xfs_mount_t	*mp,		/* file system mount structure */
	int		whichfork)	/* data or attr fork */
{
	int		level;		/* btree level */
	uint		maxblocks;	/* max blocks at this level */
	uint		maxleafents;	/* max leaf entries possible */
	int		maxrootrecs;	/* max records in root block */
	int		minleafrecs;	/* min records in leaf block */
	int		minnoderecs;	/* min records in node block */
	int		sz;		/* root block size */

	/*
	 * The maximum number of extents in a file, hence the maximum
	 * number of leaf entries, is controlled by the type of di_nextents
	 * (a signed 32-bit number, xfs_extnum_t), or by di_anextents
	 * (a signed 16-bit number, xfs_aextnum_t).
	 */
	maxleafents = (whichfork == XFS_DATA_FORK) ? MAXEXTNUM : MAXAEXTNUM;
	minleafrecs = mp->m_bmap_dmnr[0];
	minnoderecs = mp->m_bmap_dmnr[1];
	sz = (whichfork == XFS_DATA_FORK) ?
		mp->m_attroffset :
		mp->m_sb.sb_inodesize - mp->m_attroffset;
	maxrootrecs = (int)XFS_BTREE_BLOCK_MAXRECS(sz, xfs_bmdr, 0);
	maxblocks = (maxleafents + minleafrecs - 1) / minleafrecs;
	for (level = 1; maxblocks > 1; level++) {
		if (maxblocks <= maxrootrecs)
			maxblocks = 1;
		else
			maxblocks = (maxblocks + minnoderecs - 1) / minnoderecs;
	}
	mp->m_bm_maxlevels[whichfork] = level;
}

/*
 * Returns the file-relative block number of the first unused block(s)
 * in the file with at least "len" logically contiguous blocks free.
 * This is the lowest-address hole if the file has holes, else the first block
 * past the end of file.
 * Return 0 if the file is currently local (in-inode).
 */
int						/* error */
xfs_bmap_first_unused(
	xfs_trans_t	*tp,			/* transaction pointer */
	xfs_inode_t	*ip,			/* incore inode */
	xfs_extlen_t	len,			/* size of hole to find */
	xfs_fileoff_t	*first_unused,		/* unused block */
	int		whichfork)		/* data or attr fork */
{
	xfs_bmbt_rec_t	*base;			/* base of extent array */
	xfs_bmbt_rec_t	*ep;			/* pointer to an extent entry */
	int		error;			/* error return value */
	xfs_ifork_t	*ifp;			/* inode fork pointer */
	xfs_fileoff_t	lastaddr;		/* last block number seen */
	xfs_fileoff_t	lowest;			/* lowest useful block */
	xfs_fileoff_t	max;			/* starting useful block */
	xfs_fileoff_t	off;			/* offset for this block */
	xfs_extnum_t	nextents;		/* number of extent entries */

	ASSERT(XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_BTREE ||
	       XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_EXTENTS ||
	       XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_LOCAL);
	if (XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_LOCAL) {
		*first_unused = 0;
		return 0;
	}
	ifp = XFS_IFORK_PTR(ip, whichfork);
	if (!(ifp->if_flags & XFS_IFEXTENTS) &&
	    (error = xfs_iread_extents(tp, ip, whichfork)))
		return error;
	lowest = *first_unused;
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	base = &ifp->if_u1.if_extents[0];
	for (lastaddr = 0, max = lowest, ep = base;
	     ep < &base[nextents];
	     ep++) {
		off = xfs_bmbt_get_startoff(ep);
		/*
		 * See if the hole before this extent will work.
		 */
		if (off >= lowest + len && off - max >= len) {
			*first_unused = max;
			return 0;
		}
		lastaddr = off + xfs_bmbt_get_blockcount(ep);
		max = XFS_FILEOFF_MAX(lastaddr, lowest);
	}
	*first_unused = max;
	return 0;
}

/*
 * Returns the file-relative block number of the last block + 1 before
 * last_block (input value) in the file.
 * This is not based on i_size, it is based on the extent list.
 * Returns 0 for local files, as they do not have an extent list.
 */
int						/* error */
xfs_bmap_last_before(
	xfs_trans_t	*tp,			/* transaction pointer */
	xfs_inode_t	*ip,			/* incore inode */
	xfs_fileoff_t	*last_block,		/* last block */
	int		whichfork)		/* data or attr fork */
{
	xfs_fileoff_t	bno;			/* input file offset */
	int		eof;			/* hit end of file */
	xfs_bmbt_rec_t	*ep;			/* pointer to last extent */
	int		error;			/* error return value */
	xfs_bmbt_irec_t	got;			/* current extent value */
	xfs_ifork_t	*ifp;			/* inode fork pointer */
	xfs_extnum_t	lastx;			/* last extent used */
	xfs_bmbt_irec_t	prev;			/* previous extent value */

	if (XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_BTREE &&
	    XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_EXTENTS &&
	    XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_LOCAL)
	       return XFS_ERROR(EIO);
	if (XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_LOCAL) {
		*last_block = 0;
		return 0;
	}
	ifp = XFS_IFORK_PTR(ip, whichfork);
	if (!(ifp->if_flags & XFS_IFEXTENTS) &&
	    (error = xfs_iread_extents(tp, ip, whichfork)))
		return error;
	bno = *last_block - 1;
	ep = xfs_bmap_search_extents(ip, bno, whichfork, &eof, &lastx, &got,
		&prev);
	if (eof || xfs_bmbt_get_startoff(ep) > bno) {
		if (prev.br_startoff == NULLFILEOFF)
			*last_block = 0;
		else
			*last_block = prev.br_startoff + prev.br_blockcount;
	}
	/*
	 * Otherwise *last_block is already the right answer.
	 */
	return 0;
}

/*
 * Returns the file-relative block number of the first block past eof in
 * the file.  This is not based on i_size, it is based on the extent list.
 * Returns 0 for local files, as they do not have an extent list.
 */
int						/* error */
xfs_bmap_last_offset(
	xfs_trans_t	*tp,			/* transaction pointer */
	xfs_inode_t	*ip,			/* incore inode */
	xfs_fileoff_t	*last_block,		/* last block */
	int		whichfork)		/* data or attr fork */
{
	xfs_bmbt_rec_t	*base;			/* base of extent array */
	xfs_bmbt_rec_t	*ep;			/* pointer to last extent */
	int		error;			/* error return value */
	xfs_ifork_t	*ifp;			/* inode fork pointer */
	xfs_extnum_t	nextents;		/* number of extent entries */

	if (XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_BTREE &&
	    XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_EXTENTS &&
	    XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_LOCAL)
	       return XFS_ERROR(EIO);
	if (XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_LOCAL) {
		*last_block = 0;
		return 0;
	}
	ifp = XFS_IFORK_PTR(ip, whichfork);
	if (!(ifp->if_flags & XFS_IFEXTENTS) &&
	    (error = xfs_iread_extents(tp, ip, whichfork)))
		return error;
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	if (!nextents) {
		*last_block = 0;
		return 0;
	}
	base = &ifp->if_u1.if_extents[0];
	ASSERT(base != NULL);
	ep = &base[nextents - 1];
	*last_block = xfs_bmbt_get_startoff(ep) + xfs_bmbt_get_blockcount(ep);
	return 0;
}

/*
 * Returns whether the selected fork of the inode has exactly one
 * block or not.  For the data fork we check this matches di_size,
 * implying the file's range is 0..bsize-1.
 */
int					/* 1=>1 block, 0=>otherwise */
xfs_bmap_one_block(
	xfs_inode_t	*ip,		/* incore inode */
	int		whichfork)	/* data or attr fork */
{
	xfs_bmbt_rec_t	*ep;		/* ptr to fork's extent */
	xfs_ifork_t	*ifp;		/* inode fork pointer */
	int		rval;		/* return value */
	xfs_bmbt_irec_t	s;		/* internal version of extent */

#ifndef DEBUG
	if (whichfork == XFS_DATA_FORK)
		return ip->i_d.di_size == ip->i_mount->m_sb.sb_blocksize;
#endif	/* !DEBUG */
	if (XFS_IFORK_NEXTENTS(ip, whichfork) != 1)
		return 0;
	if (XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_EXTENTS)
		return 0;
	ifp = XFS_IFORK_PTR(ip, whichfork);
	ASSERT(ifp->if_flags & XFS_IFEXTENTS);
	ep = ifp->if_u1.if_extents;
	xfs_bmbt_get_all(ep, &s);
	rval = s.br_startoff == 0 && s.br_blockcount == 1;
	if (rval && whichfork == XFS_DATA_FORK)
		ASSERT(ip->i_d.di_size == ip->i_mount->m_sb.sb_blocksize);
	return rval;
}

/*
 * Read in the extents to if_extents.
 * All inode fields are set up by caller, we just traverse the btree
 * and copy the records in. If the file system cannot contain unwritten
 * extents, the records are checked for no "state" flags.
 */
int					/* error */
xfs_bmap_read_extents(
	xfs_trans_t		*tp,	/* transaction pointer */
	xfs_inode_t		*ip,	/* incore inode */
	int			whichfork) /* data or attr fork */
{
	xfs_bmbt_block_t	*block;	/* current btree block */
	xfs_fsblock_t		bno;	/* block # of "block" */
	xfs_buf_t		*bp;	/* buffer for "block" */
	int			error;	/* error return value */
	xfs_exntfmt_t		exntf;	/* XFS_EXTFMT_NOSTATE, if checking */
#ifdef XFS_BMAP_TRACE
	static char		fname[] = "xfs_bmap_read_extents";
#endif
	xfs_extnum_t		i, j;	/* index into the extents list */
	xfs_ifork_t		*ifp;	/* fork structure */
	int			level;	/* btree level, for checking */
	xfs_mount_t		*mp;	/* file system mount structure */
	xfs_bmbt_ptr_t		*pp;	/* pointer to block address */
	/* REFERENCED */
	xfs_extnum_t		room;	/* number of entries there's room for */
	xfs_bmbt_rec_t		*trp;	/* target record pointer */

	bno = NULLFSBLOCK;
	mp = ip->i_mount;
	ifp = XFS_IFORK_PTR(ip, whichfork);
	exntf = (whichfork != XFS_DATA_FORK) ? XFS_EXTFMT_NOSTATE :
					XFS_EXTFMT_INODE(ip);
	block = ifp->if_broot;
	/*
	 * Root level must use BMAP_BROOT_PTR_ADDR macro to get ptr out.
	 */
	ASSERT(INT_GET(block->bb_level, ARCH_CONVERT) > 0);
	level = INT_GET(block->bb_level, ARCH_CONVERT);
	pp = XFS_BMAP_BROOT_PTR_ADDR(block, 1, ifp->if_broot_bytes);
	ASSERT(INT_GET(*pp, ARCH_CONVERT) != NULLDFSBNO);
	ASSERT(XFS_FSB_TO_AGNO(mp, INT_GET(*pp, ARCH_CONVERT)) < mp->m_sb.sb_agcount);
	ASSERT(XFS_FSB_TO_AGBNO(mp, INT_GET(*pp, ARCH_CONVERT)) < mp->m_sb.sb_agblocks);
	bno = INT_GET(*pp, ARCH_CONVERT);
	/*
	 * Go down the tree until leaf level is reached, following the first
	 * pointer (leftmost) at each level.
	 */
	while (level-- > 0) {
		if ((error = xfs_btree_read_bufl(mp, tp, bno, 0, &bp,
				XFS_BMAP_BTREE_REF)))
			return error;
		block = XFS_BUF_TO_BMBT_BLOCK(bp);
		XFS_WANT_CORRUPTED_GOTO(
			XFS_BMAP_SANITY_CHECK(mp, block, level),
			error0);
		if (level == 0)
			break;
		pp = XFS_BTREE_PTR_ADDR(mp->m_sb.sb_blocksize, xfs_bmbt, block,
			1, mp->m_bmap_dmxr[1]);
		XFS_WANT_CORRUPTED_GOTO(
			XFS_FSB_SANITY_CHECK(mp, INT_GET(*pp, ARCH_CONVERT)),
			error0);
		bno = INT_GET(*pp, ARCH_CONVERT);
		xfs_trans_brelse(tp, bp);
	}
	/*
	 * Here with bp and block set to the leftmost leaf node in the tree.
	 */
	room = ifp->if_bytes / (uint)sizeof(*trp);
	trp = ifp->if_u1.if_extents;
	i = 0;
	/*
	 * Loop over all leaf nodes.  Copy information to the extent list.
	 */
	for (;;) {
		xfs_bmbt_rec_t	*frp, *temp;
		xfs_fsblock_t	nextbno;
		xfs_extnum_t	num_recs;


		num_recs = INT_GET(block->bb_numrecs, ARCH_CONVERT);
		if (unlikely(i + num_recs > room)) {
			ASSERT(i + num_recs <= room);
			xfs_fs_cmn_err(CE_WARN, ip->i_mount,
				"corrupt dinode %Lu, (btree extents).  Unmount and run xfs_repair.",
				(unsigned long long) ip->i_ino);
			XFS_ERROR_REPORT("xfs_bmap_read_extents(1)",
					 XFS_ERRLEVEL_LOW,
					ip->i_mount);
			goto error0;
		}
		XFS_WANT_CORRUPTED_GOTO(
			XFS_BMAP_SANITY_CHECK(mp, block, 0),
			error0);
		/*
		 * Read-ahead the next leaf block, if any.
		 */
		nextbno = INT_GET(block->bb_rightsib, ARCH_CONVERT);
		if (nextbno != NULLFSBLOCK)
			xfs_btree_reada_bufl(mp, nextbno, 1);
		/*
		 * Copy records into the extent list.
		 */
		frp = XFS_BTREE_REC_ADDR(mp->m_sb.sb_blocksize, xfs_bmbt,
			block, 1, mp->m_bmap_dmxr[0]);
		temp = trp;
		for (j = 0; j < num_recs; j++, frp++, trp++) {
			trp->l0 = INT_GET(frp->l0, ARCH_CONVERT);
			trp->l1 = INT_GET(frp->l1, ARCH_CONVERT);
		}
		if (exntf == XFS_EXTFMT_NOSTATE) {
			/*
			 * Check all attribute bmap btree records and
			 * any "older" data bmap btree records for a
			 * set bit in the "extent flag" position.
			 */
			if (unlikely(xfs_check_nostate_extents(temp, num_recs))) {
				XFS_ERROR_REPORT("xfs_bmap_read_extents(2)",
						 XFS_ERRLEVEL_LOW,
						 ip->i_mount);
				goto error0;
			}
		}
		i += num_recs;
		xfs_trans_brelse(tp, bp);
		bno = nextbno;
		/*
		 * If we've reached the end, stop.
		 */
		if (bno == NULLFSBLOCK)
			break;
		if ((error = xfs_btree_read_bufl(mp, tp, bno, 0, &bp,
				XFS_BMAP_BTREE_REF)))
			return error;
		block = XFS_BUF_TO_BMBT_BLOCK(bp);
	}
	ASSERT(i == ifp->if_bytes / (uint)sizeof(*trp));
	ASSERT(i == XFS_IFORK_NEXTENTS(ip, whichfork));
	xfs_bmap_trace_exlist(fname, ip, i, whichfork);
	return 0;
error0:
	xfs_trans_brelse(tp, bp);
	return XFS_ERROR(EFSCORRUPTED);
}

/*
 * Map file blocks to filesystem blocks.
 * File range is given by the bno/len pair.
 * Adds blocks to file if a write ("flags & XFS_BMAPI_WRITE" set)
 * into a hole or past eof.
 * Only allocates blocks from a single allocation group,
 * to avoid locking problems.
 * The returned value in "firstblock" from the first call in a transaction
 * must be remembered and presented to subsequent calls in "firstblock".
 * An upper bound for the number of blocks to be allocated is supplied to
 * the first call in "total"; if no allocation group has that many free
 * blocks then the call will fail (return NULLFSBLOCK in "firstblock").
 */
int					/* error */
xfs_bmapi(
	xfs_trans_t	*tp,		/* transaction pointer */
	xfs_inode_t	*ip,		/* incore inode */
	xfs_fileoff_t	bno,		/* starting file offs. mapped */
	xfs_filblks_t	len,		/* length to map in file */
	int		flags,		/* XFS_BMAPI_... */
	xfs_fsblock_t	*firstblock,	/* first allocated block
					   controls a.g. for allocs */
	xfs_extlen_t	total,		/* total blocks needed */
	xfs_bmbt_irec_t	*mval,		/* output: map values */
	int		*nmap,		/* i/o: mval size/count */
	xfs_bmap_free_t	*flist)		/* i/o: list extents to free */
{
	xfs_fsblock_t	abno;		/* allocated block number */
	xfs_extlen_t	alen;		/* allocated extent length */
	xfs_fileoff_t	aoff;		/* allocated file offset */
	xfs_bmalloca_t	bma;		/* args for xfs_bmap_alloc */
	char		contig;		/* allocation must be one extent */
	xfs_btree_cur_t	*cur;		/* bmap btree cursor */
	char		delay;		/* this request is for delayed alloc */
	xfs_fileoff_t	end;		/* end of mapped file region */
	int		eof;		/* we've hit the end of extent list */
	xfs_bmbt_rec_t	*ep;		/* extent list entry pointer */
	int		error;		/* error return */
	char		exact;		/* don't do all of wasdelayed extent */
	xfs_bmbt_irec_t	got;		/* current extent list record */
	xfs_ifork_t	*ifp;		/* inode fork pointer */
	xfs_extlen_t	indlen;		/* indirect blocks length */
	char		inhole;		/* current location is hole in file */
	xfs_extnum_t	lastx;		/* last useful extent number */
	int		logflags;	/* flags for transaction logging */
	xfs_extlen_t	minleft;	/* min blocks left after allocation */
	xfs_extlen_t	minlen;		/* min allocation size */
	xfs_mount_t	*mp;		/* xfs mount structure */
	int		n;		/* current extent index */
	int		nallocs;	/* number of extents alloc\'d */
	xfs_extnum_t	nextents;	/* number of extents in file */
	xfs_fileoff_t	obno;		/* old block number (offset) */
	xfs_bmbt_irec_t	prev;		/* previous extent list record */
	char		stateless;	/* ignore state flag set */
	int		tmp_logflags;	/* temp flags holder */
	char		trim;		/* output trimmed to match range */
	char		userdata;	/* allocating non-metadata */
	char		wasdelay;	/* old extent was delayed */
	int		whichfork;	/* data or attr fork */
	char		wr;		/* this is a write request */
	char		rsvd;		/* OK to allocate reserved blocks */
#ifdef DEBUG
	xfs_fileoff_t	orig_bno;	/* original block number value */
	int		orig_flags;	/* original flags arg value */
	xfs_filblks_t	orig_len;	/* original value of len arg */
	xfs_bmbt_irec_t	*orig_mval;	/* original value of mval */
	int		orig_nmap;	/* original value of *nmap */

	orig_bno = bno;
	orig_len = len;
	orig_flags = flags;
	orig_mval = mval;
	orig_nmap = *nmap;
#endif
	ASSERT(*nmap >= 1);
	ASSERT(*nmap <= XFS_BMAP_MAX_NMAP || !(flags & XFS_BMAPI_WRITE));
	whichfork = (flags & XFS_BMAPI_ATTRFORK) ?
		XFS_ATTR_FORK : XFS_DATA_FORK;
	mp = ip->i_mount;
	if (unlikely(XFS_TEST_ERROR(
	    (XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_EXTENTS &&
	     XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_BTREE &&
	     XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_LOCAL),
	     mp, XFS_ERRTAG_BMAPIFORMAT, XFS_RANDOM_BMAPIFORMAT))) {
		XFS_ERROR_REPORT("xfs_bmapi", XFS_ERRLEVEL_LOW, mp);
		return XFS_ERROR(EFSCORRUPTED);
	}
	if (XFS_FORCED_SHUTDOWN(mp))
		return XFS_ERROR(EIO);
	ifp = XFS_IFORK_PTR(ip, whichfork);
	ASSERT(ifp->if_ext_max ==
	       XFS_IFORK_SIZE(ip, whichfork) / (uint)sizeof(xfs_bmbt_rec_t));
	if ((wr = (flags & XFS_BMAPI_WRITE)) != 0)
		XFS_STATS_INC(xfsstats.xs_blk_mapw);
	else
		XFS_STATS_INC(xfsstats.xs_blk_mapr);
	delay = (flags & XFS_BMAPI_DELAY) != 0;
	trim = (flags & XFS_BMAPI_ENTIRE) == 0;
	userdata = (flags & XFS_BMAPI_METADATA) == 0;
	exact = (flags & XFS_BMAPI_EXACT) != 0;
	rsvd = (flags & XFS_BMAPI_RSVBLOCKS) != 0;
	contig = (flags & XFS_BMAPI_CONTIG) != 0;
	/*
	 * stateless is used to combine extents which
	 * differ only due to the state of the extents.
	 * This technique is used from xfs_getbmap()
	 * when the caller does not wish to see the
	 * separation (which is the default).
	 *
	 * This technique is also used when writing a
	 * buffer which has been partially written,
	 * (usually by being flushed during a chunkread),
	 * to ensure one write takes place. This also
	 * prevents a change in the xfs inode extents at
	 * this time, intentionally. This change occurs
	 * on completion of the write operation, in
	 * xfs_strat_comp(), where the xfs_bmapi() call
	 * is transactioned, and the extents combined.
	 */
	stateless = (flags & XFS_BMAPI_IGSTATE) != 0;
	if (stateless && wr)	/* if writing unwritten space, no */
		wr = 0;		/* allocations are allowed */
	ASSERT(wr || !delay);
	logflags = 0;
	nallocs = 0;
	cur = NULL;
	if (XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_LOCAL) {
		ASSERT(wr && tp);
		if ((error = xfs_bmap_local_to_extents(tp, ip,
				firstblock, total, &logflags, whichfork)))
			goto error0;
	}
	if (wr && *firstblock == NULLFSBLOCK) {
		if (XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_BTREE)
			minleft = INT_GET(ifp->if_broot->bb_level, ARCH_CONVERT) + 1;
		else
			minleft = 1;
	} else
		minleft = 0;
	if (!(ifp->if_flags & XFS_IFEXTENTS) &&
	    (error = xfs_iread_extents(tp, ip, whichfork)))
		goto error0;
	ep = xfs_bmap_search_extents(ip, bno, whichfork, &eof, &lastx, &got,
		&prev);
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	n = 0;
	end = bno + len;
	obno = bno;
	bma.ip = NULL;
	while (bno < end && n < *nmap) {
		/*
		 * Reading past eof, act as though there's a hole
		 * up to end.
		 */
		if (eof && !wr)
			got.br_startoff = end;
		inhole = eof || got.br_startoff > bno;
		wasdelay = wr && !inhole && !delay &&
			ISNULLSTARTBLOCK(got.br_startblock);
		/*
		 * First, deal with the hole before the allocated space
		 * that we found, if any.
		 */
		if (wr && (inhole || wasdelay)) {
			/*
			 * For the wasdelay case, we could also just
			 * allocate the stuff asked for in this bmap call
			 * but that wouldn't be as good.
			 */
			if (wasdelay && !exact) {
				alen = (xfs_extlen_t)got.br_blockcount;
				aoff = got.br_startoff;
				if (lastx != NULLEXTNUM && lastx) {
					ep = &ifp->if_u1.if_extents[lastx - 1];
					xfs_bmbt_get_all(ep, &prev);
				}
			} else if (wasdelay) {
				alen = (xfs_extlen_t)
					XFS_FILBLKS_MIN(len,
						(got.br_startoff +
						 got.br_blockcount) - bno);
				aoff = bno;
			} else {
				alen = (xfs_extlen_t)
					XFS_FILBLKS_MIN(len, MAXEXTLEN);
				if (!eof)
					alen = (xfs_extlen_t)
						XFS_FILBLKS_MIN(alen,
							got.br_startoff - bno);
				aoff = bno;
			}
			minlen = contig ? alen : 1;
			if (delay) {
				indlen = (xfs_extlen_t)
					xfs_bmap_worst_indlen(ip, alen);
				ASSERT(indlen > 0);
				/*
				 * Make a transaction-less quota reservation for
				 * delayed allocation blocks. This number gets
				 * adjusted later.
				 * We return EDQUOT if we haven't allocated
				 * blks already inside this loop;
				 */
				if (XFS_TRANS_RESERVE_BLKQUOTA(
						mp, NULL, ip, (long)alen)) {
					if (n == 0) {
						*nmap = 0;
						ASSERT(cur == NULL);
						return XFS_ERROR(EDQUOT);
					}
					break;
				}
				if (xfs_mod_incore_sb(mp, XFS_SBS_FDBLOCKS,
						-(alen + indlen), rsvd)) {
					XFS_TRANS_UNRESERVE_BLKQUOTA(
						mp, NULL, ip, (long)alen);
					break;
				}
				ip->i_delayed_blks += alen;
				abno = NULLSTARTBLOCK(indlen);
			} else {
				/*
				 * If first time, allocate and fill in
				 * once-only bma fields.
				 */
				if (bma.ip == NULL) {
					bma.tp = tp;
					bma.ip = ip;
					bma.prevp = &prev;
					bma.gotp = &got;
					bma.total = total;
					bma.userdata = 0;
				}
				/* Indicate if this is the first user data
				 * in the file, or just any user data.
				 */
				if (userdata) {
					bma.userdata = (aoff == 0) ?
						XFS_ALLOC_INITIAL_USER_DATA :
						XFS_ALLOC_USERDATA;
				}
				/*
				 * Fill in changeable bma fields.
				 */
				bma.eof = eof;
				bma.firstblock = *firstblock;
				bma.alen = alen;
				bma.off = aoff;
				bma.wasdel = wasdelay;
				bma.minlen = minlen;
				bma.low = flist->xbf_low;
				bma.minleft = minleft;
				/*
				 * Only want to do the alignment at the
				 * eof if it is userdata and allocation length
				 * is larger than a stripe unit.
				 */
				if (mp->m_dalign && alen >= mp->m_dalign &&
				    userdata && whichfork == XFS_DATA_FORK) {
					if ((error = xfs_bmap_isaeof(ip, aoff,
							whichfork, &bma.aeof)))
						goto error0;
				} else
					bma.aeof = 0;
				/*
				 * Call allocator.
				 */
				if ((error = xfs_bmap_alloc(&bma)))
					goto error0;
				/*
				 * Copy out result fields.
				 */
				abno = bma.rval;
				if ((flist->xbf_low = bma.low))
					minleft = 0;
				alen = bma.alen;
				aoff = bma.off;
				ASSERT(*firstblock == NULLFSBLOCK ||
				       XFS_FSB_TO_AGNO(mp, *firstblock) ==
				       XFS_FSB_TO_AGNO(mp, bma.firstblock) ||
				       (flist->xbf_low &&
					XFS_FSB_TO_AGNO(mp, *firstblock) <
					XFS_FSB_TO_AGNO(mp, bma.firstblock)));
				*firstblock = bma.firstblock;
				if (cur)
					cur->bc_private.b.firstblock =
						*firstblock;
				if (abno == NULLFSBLOCK)
					break;
				if ((ifp->if_flags & XFS_IFBROOT) && !cur) {
					cur = xfs_btree_init_cursor(mp,
						tp, NULL, 0, XFS_BTNUM_BMAP,
						ip, whichfork);
					cur->bc_private.b.firstblock =
						*firstblock;
					cur->bc_private.b.flist = flist;
				}
				/*
				 * Bump the number of extents we've allocated
				 * in this call.
				 */
				nallocs++;
			}
			if (cur)
				cur->bc_private.b.flags =
					wasdelay ? XFS_BTCUR_BPRV_WASDEL : 0;
			got.br_startoff = aoff;
			got.br_startblock = abno;
			got.br_blockcount = alen;
			got.br_state = XFS_EXT_NORM;	/* assume normal */
			/*
			 * Determine state of extent, and the filesystem.
			 * A wasdelay extent has been initialized, so
			 * shouldn't be flagged as unwritten.
			 */
			if (wr && XFS_SB_VERSION_HASEXTFLGBIT(&mp->m_sb)) {
				if (!wasdelay && (flags & XFS_BMAPI_PREALLOC))
					got.br_state = XFS_EXT_UNWRITTEN;
			}
			error = xfs_bmap_add_extent(ip, lastx, &cur, &got,
				firstblock, flist, &tmp_logflags, whichfork,
				rsvd);
			logflags |= tmp_logflags;
			if (error)
				goto error0;
			lastx = ifp->if_lastex;
			ep = &ifp->if_u1.if_extents[lastx];
			nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
			xfs_bmbt_get_all(ep, &got);
			ASSERT(got.br_startoff <= aoff);
			ASSERT(got.br_startoff + got.br_blockcount >=
				aoff + alen);
#ifdef DEBUG
			if (delay) {
				ASSERT(ISNULLSTARTBLOCK(got.br_startblock));
				ASSERT(STARTBLOCKVAL(got.br_startblock) > 0);
			}
			ASSERT(got.br_state == XFS_EXT_NORM ||
			       got.br_state == XFS_EXT_UNWRITTEN);
#endif
			/*
			 * Fall down into the found allocated space case.
			 */
		} else if (inhole) {
			/*
			 * Reading in a hole.
			 */
			mval->br_startoff = bno;
			mval->br_startblock = HOLESTARTBLOCK;
			mval->br_blockcount =
				XFS_FILBLKS_MIN(len, got.br_startoff - bno);
			mval->br_state = XFS_EXT_NORM;
			bno += mval->br_blockcount;
			len -= mval->br_blockcount;
			mval++;
			n++;
			continue;
		}
		/*
		 * Then deal with the allocated space we found.
		 */
		ASSERT(ep != NULL);
		if (trim && (got.br_startoff + got.br_blockcount > obno)) {
			if (obno > bno)
				bno = obno;
			ASSERT((bno >= obno) || (n == 0));
			ASSERT(bno < end);
			mval->br_startoff = bno;
			if (ISNULLSTARTBLOCK(got.br_startblock)) {
				ASSERT(!wr || delay);
				mval->br_startblock = DELAYSTARTBLOCK;
			} else
				mval->br_startblock =
					got.br_startblock +
					(bno - got.br_startoff);
			/*
			 * Return the minimum of what we got and what we
			 * asked for for the length.  We can use the len
			 * variable here because it is modified below
			 * and we could have been there before coming
			 * here if the first part of the allocation
			 * didn't overlap what was asked for.
			 */
			mval->br_blockcount =
				XFS_FILBLKS_MIN(end - bno, got.br_blockcount -
					(bno - got.br_startoff));
			mval->br_state = got.br_state;
			ASSERT(mval->br_blockcount <= len);
		} else {
			*mval = got;
			if (ISNULLSTARTBLOCK(mval->br_startblock)) {
				ASSERT(!wr || delay);
				mval->br_startblock = DELAYSTARTBLOCK;
			}
		}

		/*
		 * Check if writing previously allocated but
		 * unwritten extents.
		 */
		if (wr && mval->br_state == XFS_EXT_UNWRITTEN &&
		    ((flags & (XFS_BMAPI_PREALLOC|XFS_BMAPI_DELAY)) == 0)) {
			/*
			 * Modify (by adding) the state flag, if writing.
			 */
			ASSERT(mval->br_blockcount <= len);
			if ((ifp->if_flags & XFS_IFBROOT) && !cur) {
				cur = xfs_btree_init_cursor(mp,
					tp, NULL, 0, XFS_BTNUM_BMAP,
					ip, whichfork);
				cur->bc_private.b.firstblock =
					*firstblock;
				cur->bc_private.b.flist = flist;
			}
			mval->br_state = XFS_EXT_NORM;
			error = xfs_bmap_add_extent(ip, lastx, &cur, mval,
				firstblock, flist, &tmp_logflags, whichfork,
				rsvd);
			logflags |= tmp_logflags;
			if (error)
				goto error0;
			lastx = ifp->if_lastex;
			ep = &ifp->if_u1.if_extents[lastx];
			nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
			xfs_bmbt_get_all(ep, &got);
			/*
			 * We may have combined previously unwritten
			 * space with written space, so generate
			 * another request.
			 */
			if (mval->br_blockcount < len)
				continue;
		}

		ASSERT(!trim ||
		       ((mval->br_startoff + mval->br_blockcount) <= end));
		ASSERT(!trim || (mval->br_blockcount <= len) ||
		       (mval->br_startoff < obno));
		bno = mval->br_startoff + mval->br_blockcount;
		len = end - bno;
		if (n > 0 && mval->br_startoff == mval[-1].br_startoff) {
			ASSERT(mval->br_startblock == mval[-1].br_startblock);
			ASSERT(mval->br_blockcount > mval[-1].br_blockcount);
			ASSERT(mval->br_state == mval[-1].br_state);
			mval[-1].br_blockcount = mval->br_blockcount;
			mval[-1].br_state = mval->br_state;
		} else if (n > 0 && mval->br_startblock != DELAYSTARTBLOCK &&
			   mval[-1].br_startblock != DELAYSTARTBLOCK &&
			   mval[-1].br_startblock != HOLESTARTBLOCK &&
			   mval->br_startblock ==
			   mval[-1].br_startblock + mval[-1].br_blockcount &&
			   (stateless || mval[-1].br_state == mval->br_state)) {
			ASSERT(mval->br_startoff ==
			       mval[-1].br_startoff + mval[-1].br_blockcount);
			mval[-1].br_blockcount += mval->br_blockcount;
		} else if (n > 0 &&
			   mval->br_startblock == DELAYSTARTBLOCK &&
			   mval[-1].br_startblock == DELAYSTARTBLOCK &&
			   mval->br_startoff ==
			   mval[-1].br_startoff + mval[-1].br_blockcount) {
			mval[-1].br_blockcount += mval->br_blockcount;
			mval[-1].br_state = mval->br_state;
		} else if (!((n == 0) &&
			     ((mval->br_startoff + mval->br_blockcount) <=
			      obno))) {
			mval++;
			n++;
		}
		/*
		 * If we're done, stop now.  Stop when we've allocated
		 * XFS_BMAP_MAX_NMAP extents no matter what.  Otherwise
		 * the transaction may get too big.
		 */
		if (bno >= end || n >= *nmap || nallocs >= *nmap)
			break;
		/*
		 * Else go on to the next record.
		 */
		ep++;
		lastx++;
		if (lastx >= nextents) {
			eof = 1;
			prev = got;
		} else
			xfs_bmbt_get_all(ep, &got);
	}
	ifp->if_lastex = lastx;
	*nmap = n;
	/*
	 * Transform from btree to extents, give it cur.
	 */
	if (tp && XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_BTREE &&
	    XFS_IFORK_NEXTENTS(ip, whichfork) <= ifp->if_ext_max) {
		ASSERT(wr && cur);
		error = xfs_bmap_btree_to_extents(tp, ip, cur,
			&tmp_logflags, whichfork);
		logflags |= tmp_logflags;
		if (error)
			goto error0;
	}
	ASSERT(ifp->if_ext_max ==
	       XFS_IFORK_SIZE(ip, whichfork) / (uint)sizeof(xfs_bmbt_rec_t));
	ASSERT(XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_BTREE ||
	       XFS_IFORK_NEXTENTS(ip, whichfork) > ifp->if_ext_max);
	error = 0;

error0:
	/*
	 * Log everything.  Do this after conversion, there's no point in
	 * logging the extent list if we've converted to btree format.
	 */
	if ((logflags & XFS_ILOG_FEXT(whichfork)) &&
	    XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_EXTENTS)
		logflags &= ~XFS_ILOG_FEXT(whichfork);
	else if ((logflags & XFS_ILOG_FBROOT(whichfork)) &&
		 XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_BTREE)
		logflags &= ~XFS_ILOG_FBROOT(whichfork);
	/*
	 * Log whatever the flags say, even if error.  Otherwise we might miss
	 * detecting a case where the data is changed, there's an error,
	 * and it's not logged so we don't shutdown when we should.
	 */
	if (logflags) {
		ASSERT(tp && wr);
		xfs_trans_log_inode(tp, ip, logflags);
	}
	if (cur) {
		if (!error) {
			ASSERT(*firstblock == NULLFSBLOCK ||
			       XFS_FSB_TO_AGNO(mp, *firstblock) ==
			       XFS_FSB_TO_AGNO(mp,
				       cur->bc_private.b.firstblock) ||
			       (flist->xbf_low &&
				XFS_FSB_TO_AGNO(mp, *firstblock) <
				XFS_FSB_TO_AGNO(mp,
					cur->bc_private.b.firstblock)));
			*firstblock = cur->bc_private.b.firstblock;
		}
		xfs_btree_del_cursor(cur,
			error ? XFS_BTREE_ERROR : XFS_BTREE_NOERROR);
	}
	if (!error)
		xfs_bmap_validate_ret(orig_bno, orig_len, orig_flags, orig_mval,
			orig_nmap, *nmap);
	return error;
}

/*
 * Map file blocks to filesystem blocks, simple version.
 * One block (extent) only, read-only.
 * For flags, only the XFS_BMAPI_ATTRFORK flag is examined.
 * For the other flag values, the effect is as if XFS_BMAPI_METADATA
 * was set and all the others were clear.
 */
int						/* error */
xfs_bmapi_single(
	xfs_trans_t	*tp,		/* transaction pointer */
	xfs_inode_t	*ip,		/* incore inode */
	int		whichfork,	/* data or attr fork */
	xfs_fsblock_t	*fsb,		/* output: mapped block */
	xfs_fileoff_t	bno)		/* starting file offs. mapped */
{
	int		eof;		/* we've hit the end of extent list */
	int		error;		/* error return */
	xfs_bmbt_irec_t	got;		/* current extent list record */
	xfs_ifork_t	*ifp;		/* inode fork pointer */
	xfs_extnum_t	lastx;		/* last useful extent number */
	xfs_bmbt_irec_t	prev;		/* previous extent list record */

	ifp = XFS_IFORK_PTR(ip, whichfork);
	if (unlikely(
	    XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_BTREE &&
	    XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_EXTENTS)) {
	       XFS_ERROR_REPORT("xfs_bmapi_single", XFS_ERRLEVEL_LOW,
				ip->i_mount);
	       return XFS_ERROR(EFSCORRUPTED);
	}
	if (XFS_FORCED_SHUTDOWN(ip->i_mount))
		return XFS_ERROR(EIO);
	XFS_STATS_INC(xfsstats.xs_blk_mapr);
	if (!(ifp->if_flags & XFS_IFEXTENTS) &&
	    (error = xfs_iread_extents(tp, ip, whichfork)))
		return error;
	(void)xfs_bmap_search_extents(ip, bno, whichfork, &eof, &lastx, &got,
		&prev);
	/*
	 * Reading past eof, act as though there's a hole
	 * up to end.
	 */
	if (eof || got.br_startoff > bno) {
		*fsb = NULLFSBLOCK;
		return 0;
	}
	ASSERT(!ISNULLSTARTBLOCK(got.br_startblock));
	ASSERT(bno < got.br_startoff + got.br_blockcount);
	*fsb = got.br_startblock + (bno - got.br_startoff);
	ifp->if_lastex = lastx;
	return 0;
}

/*
 * Unmap (remove) blocks from a file.
 * If nexts is nonzero then the number of extents to remove is limited to
 * that value.  If not all extents in the block range can be removed then
 * *done is set.
 */
int						/* error */
xfs_bunmapi(
	xfs_trans_t		*tp,		/* transaction pointer */
	struct xfs_inode	*ip,		/* incore inode */
	xfs_fileoff_t		bno,		/* starting offset to unmap */
	xfs_filblks_t		len,		/* length to unmap in file */
	int			flags,		/* misc flags */
	xfs_extnum_t		nexts,		/* number of extents max */
	xfs_fsblock_t		*firstblock,	/* first allocated block
						   controls a.g. for allocs */
	xfs_bmap_free_t		*flist,		/* i/o: list extents to free */
	int			*done)		/* set if not done yet */
{
	xfs_btree_cur_t		*cur;		/* bmap btree cursor */
	xfs_bmbt_irec_t		del;		/* extent being deleted */
	int			eof;		/* is deleting at eof */
	xfs_bmbt_rec_t		*ep;		/* extent list entry pointer */
	int			error;		/* error return value */
	xfs_extnum_t		extno;		/* extent number in list */
	xfs_bmbt_irec_t		got;		/* current extent list entry */
	xfs_ifork_t		*ifp;		/* inode fork pointer */
	int			isrt;		/* freeing in rt area */
	xfs_extnum_t		lastx;		/* last extent index used */
	int			logflags;	/* transaction logging flags */
	xfs_extlen_t		mod;		/* rt extent offset */
	xfs_mount_t		*mp;		/* mount structure */
	xfs_extnum_t		nextents;	/* size of extent list */
	xfs_bmbt_irec_t		prev;		/* previous extent list entry */
	xfs_fileoff_t		start;		/* first file offset deleted */
	int			tmp_logflags;	/* partial logging flags */
	int			wasdel;		/* was a delayed alloc extent */
	int			whichfork;	/* data or attribute fork */
	int			rsvd;		/* OK to allocate reserved blocks */
	xfs_fsblock_t		sum;

	xfs_bunmap_trace(ip, bno, len, flags, (inst_t *)__return_address);
	whichfork = (flags & XFS_BMAPI_ATTRFORK) ?
		XFS_ATTR_FORK : XFS_DATA_FORK;
	ifp = XFS_IFORK_PTR(ip, whichfork);
	if (unlikely(
	    XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_EXTENTS &&
	    XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_BTREE)) {
		XFS_ERROR_REPORT("xfs_bunmapi", XFS_ERRLEVEL_LOW,
				 ip->i_mount);
		return XFS_ERROR(EFSCORRUPTED);
	}
	mp = ip->i_mount;
	if (XFS_FORCED_SHUTDOWN(mp))
		return XFS_ERROR(EIO);
	rsvd = (flags & XFS_BMAPI_RSVBLOCKS) != 0;
	ASSERT(len > 0);
	ASSERT(nexts >= 0);
	ASSERT(ifp->if_ext_max ==
	       XFS_IFORK_SIZE(ip, whichfork) / (uint)sizeof(xfs_bmbt_rec_t));
	if (!(ifp->if_flags & XFS_IFEXTENTS) &&
	    (error = xfs_iread_extents(tp, ip, whichfork)))
		return error;
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	if (nextents == 0) {
		*done = 1;
		return 0;
	}
	XFS_STATS_INC(xfsstats.xs_blk_unmap);
	isrt = (whichfork == XFS_DATA_FORK) &&
	       (ip->i_d.di_flags & XFS_DIFLAG_REALTIME);
	start = bno;
	bno = start + len - 1;
	ep = xfs_bmap_search_extents(ip, bno, whichfork, &eof, &lastx, &got,
		&prev);
	/*
	 * Check to see if the given block number is past the end of the
	 * file, back up to the last block if so...
	 */
	if (eof) {
		ep = &ifp->if_u1.if_extents[--lastx];
		xfs_bmbt_get_all(ep, &got);
		bno = got.br_startoff + got.br_blockcount - 1;
	}
	logflags = 0;
	if (ifp->if_flags & XFS_IFBROOT) {
		ASSERT(XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_BTREE);
		cur = xfs_btree_init_cursor(mp, tp, NULL, 0, XFS_BTNUM_BMAP, ip,
			whichfork);
		cur->bc_private.b.firstblock = *firstblock;
		cur->bc_private.b.flist = flist;
		cur->bc_private.b.flags = 0;
	} else
		cur = NULL;
	extno = 0;
	while (bno != (xfs_fileoff_t)-1 && bno >= start && lastx >= 0 &&
	       (nexts == 0 || extno < nexts)) {
		/*
		 * Is the found extent after a hole in which bno lives?
		 * Just back up to the previous extent, if so.
		 */
		if (got.br_startoff > bno) {
			if (--lastx < 0)
				break;
			ep--;
			xfs_bmbt_get_all(ep, &got);
		}
		/*
		 * Is the last block of this extent before the range
		 * we're supposed to delete?  If so, we're done.
		 */
		bno = XFS_FILEOFF_MIN(bno,
			got.br_startoff + got.br_blockcount - 1);
		if (bno < start)
			break;
		/*
		 * Then deal with the (possibly delayed) allocated space
		 * we found.
		 */
		ASSERT(ep != NULL);
		del = got;
		wasdel = ISNULLSTARTBLOCK(del.br_startblock);
		if (got.br_startoff < start) {
			del.br_startoff = start;
			del.br_blockcount -= start - got.br_startoff;
			if (!wasdel)
				del.br_startblock += start - got.br_startoff;
		}
		if (del.br_startoff + del.br_blockcount > bno + 1)
			del.br_blockcount = bno + 1 - del.br_startoff;
		sum = del.br_startblock + del.br_blockcount;
		if (isrt &&
		    (mod = do_mod(sum, mp->m_sb.sb_rextsize))) {
			/*
			 * Realtime extent not lined up at the end.
			 * The extent could have been split into written
			 * and unwritten pieces, or we could just be
			 * unmapping part of it.  But we can't really
			 * get rid of part of a realtime extent.
			 */
			if (del.br_state == XFS_EXT_UNWRITTEN ||
			    !XFS_SB_VERSION_HASEXTFLGBIT(&mp->m_sb)) {
				/*
				 * This piece is unwritten, or we're not
				 * using unwritten extents.  Skip over it.
				 */
				ASSERT(bno >= mod);
				bno -= mod > del.br_blockcount ?
					del.br_blockcount : mod;
				if (bno < got.br_startoff) {
					if (--lastx >= 0)
						xfs_bmbt_get_all(--ep, &got);
				}
				continue;
			}
			/*
			 * It's written, turn it unwritten.
			 * This is better than zeroing it.
			 */
			ASSERT(del.br_state == XFS_EXT_NORM);
			ASSERT(xfs_trans_get_block_res(tp) > 0);
			/*
			 * If this spans a realtime extent boundary,
			 * chop it back to the start of the one we end at.
			 */
			if (del.br_blockcount > mod) {
				del.br_startoff += del.br_blockcount - mod;
				del.br_startblock += del.br_blockcount - mod;
				del.br_blockcount = mod;
			}
			del.br_state = XFS_EXT_UNWRITTEN;
			error = xfs_bmap_add_extent(ip, lastx, &cur, &del,
				firstblock, flist, &logflags, XFS_DATA_FORK, 0);
			if (error)
				goto error0;
			goto nodelete;
		}
		if (isrt && (mod = do_mod(del.br_startblock, mp->m_sb.sb_rextsize))) {
			/*
			 * Realtime extent is lined up at the end but not
			 * at the front.  We'll get rid of full extents if
			 * we can.
			 */
			mod = mp->m_sb.sb_rextsize - mod;
			if (del.br_blockcount > mod) {
				del.br_blockcount -= mod;
				del.br_startoff += mod;
				del.br_startblock += mod;
			} else if ((del.br_startoff == start &&
				    (del.br_state == XFS_EXT_UNWRITTEN ||
				     xfs_trans_get_block_res(tp) == 0)) ||
				   !XFS_SB_VERSION_HASEXTFLGBIT(&mp->m_sb)) {
				/*
				 * Can't make it unwritten.  There isn't
				 * a full extent here so just skip it.
				 */
				ASSERT(bno >= del.br_blockcount);
				bno -= del.br_blockcount;
				if (bno < got.br_startoff) {
					if (--lastx >= 0)
						xfs_bmbt_get_all(--ep, &got);
				}
				continue;
			} else if (del.br_state == XFS_EXT_UNWRITTEN) {
				/*
				 * This one is already unwritten.
				 * It must have a written left neighbor.
				 * Unwrite the killed part of that one and
				 * try again.
				 */
				ASSERT(lastx > 0);
				xfs_bmbt_get_all(ep - 1, &prev);
				ASSERT(prev.br_state == XFS_EXT_NORM);
				ASSERT(!ISNULLSTARTBLOCK(prev.br_startblock));
				ASSERT(del.br_startblock ==
				       prev.br_startblock + prev.br_blockcount);
				if (prev.br_startoff < start) {
					mod = start - prev.br_startoff;
					prev.br_blockcount -= mod;
					prev.br_startblock += mod;
					prev.br_startoff = start;
				}
				prev.br_state = XFS_EXT_UNWRITTEN;
				error = xfs_bmap_add_extent(ip, lastx - 1, &cur,
					&prev, firstblock, flist, &logflags,
					XFS_DATA_FORK, 0);
				if (error)
					goto error0;
				goto nodelete;
			} else {
				ASSERT(del.br_state == XFS_EXT_NORM);
				del.br_state = XFS_EXT_UNWRITTEN;
				error = xfs_bmap_add_extent(ip, lastx, &cur,
					&del, firstblock, flist, &logflags,
					XFS_DATA_FORK, 0);
				if (error)
					goto error0;
				goto nodelete;
			}
		}
		if (wasdel) {
			ASSERT(STARTBLOCKVAL(del.br_startblock) > 0);
			xfs_mod_incore_sb(mp, XFS_SBS_FDBLOCKS,
				(int)del.br_blockcount, rsvd);
			/* Unreserve our quota space */
			XFS_TRANS_RESERVE_QUOTA_NBLKS(
				mp, NULL, ip, -((long)del.br_blockcount), 0,
				isrt ?	XFS_QMOPT_RES_RTBLKS :
					XFS_QMOPT_RES_REGBLKS);
			ip->i_delayed_blks -= del.br_blockcount;
			if (cur)
				cur->bc_private.b.flags |=
					XFS_BTCUR_BPRV_WASDEL;
		} else if (cur)
			cur->bc_private.b.flags &= ~XFS_BTCUR_BPRV_WASDEL;
		/*
		 * If it's the case where the directory code is running
		 * with no block reservation, and the deleted block is in
		 * the middle of its extent, and the resulting insert
		 * of an extent would cause transformation to btree format,
		 * then reject it.  The calling code will then swap
		 * blocks around instead.
		 * We have to do this now, rather than waiting for the
		 * conversion to btree format, since the transaction
		 * will be dirty.
		 */
		if (!wasdel && xfs_trans_get_block_res(tp) == 0 &&
		    XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_EXTENTS &&
		    XFS_IFORK_NEXTENTS(ip, whichfork) >= ifp->if_ext_max &&
		    del.br_startoff > got.br_startoff &&
		    del.br_startoff + del.br_blockcount <
		    got.br_startoff + got.br_blockcount) {
			error = XFS_ERROR(ENOSPC);
			goto error0;
		}
		error = xfs_bmap_del_extent(ip, tp, lastx, flist, cur, &del,
			&tmp_logflags, whichfork, rsvd);
		logflags |= tmp_logflags;
		if (error)
			goto error0;
		bno = del.br_startoff - 1;
nodelete:
		lastx = ifp->if_lastex;
		/*
		 * If not done go on to the next (previous) record.
		 * Reset ep in case the extents array was re-alloced.
		 */
		ep = &ifp->if_u1.if_extents[lastx];
		if (bno != (xfs_fileoff_t)-1 && bno >= start) {
			if (lastx >= XFS_IFORK_NEXTENTS(ip, whichfork) ||
			    xfs_bmbt_get_startoff(ep) > bno) {
				lastx--;
				ep--;
			}
			if (lastx >= 0)
				xfs_bmbt_get_all(ep, &got);
			extno++;
		}
	}
	ifp->if_lastex = lastx;
	*done = bno == (xfs_fileoff_t)-1 || bno < start || lastx < 0;
	ASSERT(ifp->if_ext_max ==
	       XFS_IFORK_SIZE(ip, whichfork) / (uint)sizeof(xfs_bmbt_rec_t));
	/*
	 * Convert to a btree if necessary.
	 */
	if (XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_EXTENTS &&
	    XFS_IFORK_NEXTENTS(ip, whichfork) > ifp->if_ext_max) {
		ASSERT(cur == NULL);
		error = xfs_bmap_extents_to_btree(tp, ip, firstblock, flist,
			&cur, 0, &tmp_logflags, whichfork);
		logflags |= tmp_logflags;
		if (error)
			goto error0;
	}
	/*
	 * transform from btree to extents, give it cur
	 */
	else if (XFS_IFORK_FORMAT(ip, whichfork) == XFS_DINODE_FMT_BTREE &&
		 XFS_IFORK_NEXTENTS(ip, whichfork) <= ifp->if_ext_max) {
		ASSERT(cur != NULL);
		error = xfs_bmap_btree_to_extents(tp, ip, cur, &tmp_logflags,
			whichfork);
		logflags |= tmp_logflags;
		if (error)
			goto error0;
	}
	/*
	 * transform from extents to local?
	 */
	ASSERT(ifp->if_ext_max ==
	       XFS_IFORK_SIZE(ip, whichfork) / (uint)sizeof(xfs_bmbt_rec_t));
	error = 0;
error0:
	/*
	 * Log everything.  Do this after conversion, there's no point in
	 * logging the extent list if we've converted to btree format.
	 */
	if ((logflags & XFS_ILOG_FEXT(whichfork)) &&
	    XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_EXTENTS)
		logflags &= ~XFS_ILOG_FEXT(whichfork);
	else if ((logflags & XFS_ILOG_FBROOT(whichfork)) &&
		 XFS_IFORK_FORMAT(ip, whichfork) != XFS_DINODE_FMT_BTREE)
		logflags &= ~XFS_ILOG_FBROOT(whichfork);
	/*
	 * Log inode even in the error case, if the transaction
	 * is dirty we'll need to shut down the filesystem.
	 */
	if (logflags)
		xfs_trans_log_inode(tp, ip, logflags);
	if (cur) {
		if (!error) {
			*firstblock = cur->bc_private.b.firstblock;
			cur->bc_private.b.allocated = 0;
		}
		xfs_btree_del_cursor(cur,
			error ? XFS_BTREE_ERROR : XFS_BTREE_NOERROR);
	}
	return error;
}

/*
 * Check the last inode extent to determine whether this allocation will result
 * in blocks being allocated at the end of the file. When we allocate new data
 * blocks at the end of the file which do not start at the previous data block,
 * we will try to align the new blocks at stripe unit boundaries.
 */
int					/* error */
xfs_bmap_isaeof(
	xfs_inode_t	*ip,		/* incore inode pointer */
	xfs_fileoff_t   off,		/* file offset in fsblocks */
	int             whichfork,	/* data or attribute fork */
	char		*aeof)		/* return value */
{
	int		error;		/* error return value */
	xfs_ifork_t	*ifp;		/* inode fork pointer */
	xfs_bmbt_rec_t	*lastrec;	/* extent list entry pointer */
	xfs_extnum_t	nextents;	/* size of extent list */
	xfs_bmbt_irec_t	s;		/* expanded extent list entry */

	ASSERT(whichfork == XFS_DATA_FORK);
	ifp = XFS_IFORK_PTR(ip, whichfork);
	if (!(ifp->if_flags & XFS_IFEXTENTS) &&
	    (error = xfs_iread_extents(NULL, ip, whichfork)))
		return error;
	nextents = ifp->if_bytes / (uint)sizeof(xfs_bmbt_rec_t);
	if (nextents == 0) {
		*aeof = 1;
		return 0;
	}
	/*
	 * Go to the last extent
	 */
	lastrec = &ifp->if_u1.if_extents[nextents - 1];
	xfs_bmbt_get_all(lastrec, &s);
	/*
	 * Check we are allocating in the last extent (for delayed allocations)
	 * or past the last extent for non-delayed allocations.
	 */
	*aeof = (off >= s.br_startoff &&
		 off < s.br_startoff + s.br_blockcount &&
		 ISNULLSTARTBLOCK(s.br_startblock)) ||
		off >= s.br_startoff + s.br_blockcount;
	return 0;
}
