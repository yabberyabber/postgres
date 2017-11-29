/*-------------------------------------------------------------------------
 *
 * nbtree.h
 *	  header file for postgres btree access method implementation.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nbtree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTREE_H
#define NBTREE_H

#include "access/amapi.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "access/xlogreader.h"
#include "catalog/pg_index.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"

/* There's room for a 16-bit vacuum cycle ID in BTPageOpaqueData */
typedef uint16 SK_BTCycleId;

/*
 *	BTPageOpaqueData -- At the end of every page, we store a pointer
 *	to both siblings in the tree.  This is used to do forward/backward
 *	index scans.  The next-page link is also critical for recovery when
 *	a search has navigated to the wrong page due to concurrent page splits
 *	or deletions; see src/backend/access/nbtree/README for more info.
 *
 *	In addition, we store the page's btree level (counting upwards from
 *	zero at a leaf page) as well as some flag bits indicating the page type
 *	and status.  If the page is deleted, we replace the level with the
 *	next-transaction-ID value indicating when it is safe to reclaim the page.
 *
 *	We also store a "vacuum cycle ID".  When a page is split while VACUUM is
 *	processing the index, a nonzero value associated with the VACUUM run is
 *	stored into both halves of the split page.  (If VACUUM is not running,
 *	both pages receive zero cycleids.)	This allows VACUUM to detect whether
 *	a page was split since it started, with a small probability of false match
 *	if the page was last split some exact multiple of MAX_BT_CYCLE_ID VACUUMs
 *	ago.  Also, during a split, the BTP_SPLIT_END flag is cleared in the left
 *	(original) page, and set in the right page, but only if the next page
 *	to its right has a different cycleid.
 *
 *	NOTE: the BTP_LEAF flag bit is redundant since level==0 could be tested
 *	instead.
 */

typedef struct SK_BTPageOpaqueData
{
	BlockNumber btpo_prev;		/* left sibling, or P_NONE if leftmost */
	BlockNumber btpo_next;		/* right sibling, or P_NONE if rightmost */
	union
	{
		uint32		level;		/* tree level --- zero for leaf pages */
		TransactionId xact;		/* next transaction ID, if deleted */
	}			btpo;
	uint16		btpo_flags;		/* flag bits, see below */
	SK_BTCycleId	btpo_cycleid;	/* vacuum cycle ID of latest split */
} SK_BTPageOpaqueData;

typedef SK_BTPageOpaqueData *SK_BTPageOpaque;

/* Bits defined in btpo_flags */
#define BTP_LEAF		(1 << 0)	/* leaf page, i.e. not internal page */
#define BTP_ROOT		(1 << 1)	/* root page (has no parent) */
#define BTP_DELETED		(1 << 2)	/* page has been deleted from tree */
#define BTP_META		(1 << 3)	/* meta-page */
#define BTP_HALF_DEAD	(1 << 4)	/* empty, but still in tree */
#define BTP_SPLIT_END	(1 << 5)	/* rightmost page of split group */
#define BTP_HAS_GARBAGE (1 << 6)	/* page has LP_DEAD tuples */
#define BTP_INCOMPLETE_SPLIT (1 << 7)	/* right sibling's downlink is missing */

/*
 * The max allowed value of a cycle ID is a bit less than 64K.  This is
 * for convenience of pg_filedump and similar utilities: we want to use
 * the last 2 bytes of special space as an index type indicator, and
 * restricting cycle ID lets btree use that space for vacuum cycle IDs
 * while still allowing index type to be identified.
 */
#define MAX_BT_CYCLE_ID		0xFF7F
#define SKIPLIST_HEIGHT 12


typedef struct SkiplistNode
{
    ItemPointerData next[SKIPLIST_HEIGHT];
    ItemPointerData thisLocation;
    IndexTupleData data;
} SkiplistNode;

SkiplistNode *getSkipNodeFromBlock(Page *page, ItemPointerData ptr);

/*
 * The Meta page is always the first page in the btree index.
 * Its primary purpose is to point to the location of the btree root page.
 * We also point to the "fast" root, which is the current effective root;
 * see README for discussion.
 */

typedef struct SK_BTMetaPageData
{
	uint32		btm_magic;		/* should contain BTREE_MAGIC */
	uint32		btm_version;	/* should contain BTREE_VERSION */
    SkiplistNode head;
    SkiplistNode tail;
	BlockNumber btm_next_free;
	OffsetNumber nextOffset;
} SK_BTMetaPageData;

#define BTPageGetMeta(p) \
	((SK_BTMetaPageData *) PageGetContents(p))

#define BTREE_METAPAGE	0		/* first page is meta */
#define BTREE_MAGIC		0x053162	/* magic number of btree pages */
#define BTREE_VERSION	3		/* current version number */

/*
 * Maximum size of a btree index entry, including its tuple header.
 *
 * We actually need to be able to fit three items on every page,
 * so restrict any one item to 1/3 the per-page available space.
 */
#define BTMaxItemSize(page) \
	MAXALIGN_DOWN((PageGetPageSize(page) - \
				   MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
				   MAXALIGN(sizeof(SK_BTPageOpaqueData))) / 3)

/*
 * The leaf-page fillfactor defaults to 90% but is user-adjustable.
 * For pages above the leaf level, we use a fixed 70% fillfactor.
 * The fillfactor is applied during index build and when splitting
 * a rightmost page; when splitting non-rightmost pages we try to
 * divide the data equally.
 */
#define BTREE_MIN_FILLFACTOR		10
#define BTREE_DEFAULT_FILLFACTOR	90
#define BTREE_NONLEAF_FILLFACTOR	70

/*
 *	Test whether two btree entries are "the same".
 *
 *	Old comments:
 *	In addition, we must guarantee that all tuples in the index are unique,
 *	in order to satisfy some assumptions in Lehman and Yao.  The way that we
 *	do this is by generating a new OID for every insertion that we do in the
 *	tree.  This adds eight bytes to the size of btree index tuples.  Note
 *	that we do not use the OID as part of a composite key; the OID only
 *	serves as a unique identifier for a given index tuple (logical position
 *	within a page).
 *
 *	New comments:
 *	actually, we must guarantee that all tuples in A LEVEL
 *	are unique, not in ALL INDEX. So, we can use the t_tid
 *	as unique identifier for a given index tuple (logical position
 *	within a level). - vadim 04/09/97
 */
#define BTTidSame(i1, i2)	\
	((ItemPointerGetBlockNumber(&(i1)) == ItemPointerGetBlockNumber(&(i2))) && \
	 (ItemPointerGetOffsetNumber(&(i1)) == ItemPointerGetOffsetNumber(&(i2))))
#define BTEntrySame(i1, i2) \
	BTTidSame((i1)->t_tid, (i2)->t_tid)


/*
 *	In general, the btree code tries to localize its knowledge about
 *	page layout to a couple of routines.  However, we need a special
 *	value to indicate "no page number" in those places where we expect
 *	page numbers.  We can use zero for this because we never need to
 *	make a pointer to the metadata page.
 */

#define P_NONE			0

/*
 * Macros to test whether a page is leftmost or rightmost on its tree level,
 * as well as other state info kept in the opaque data.
 */
#define P_LEFTMOST(opaque)		((opaque)->btpo_prev == P_NONE)
#define P_RIGHTMOST(opaque)		((opaque)->btpo_next == P_NONE)
#define P_ISLEAF(opaque)		(((opaque)->btpo_flags & BTP_LEAF) != 0)
#define P_ISROOT(opaque)		(((opaque)->btpo_flags & BTP_ROOT) != 0)
#define P_ISDELETED(opaque)		(((opaque)->btpo_flags & BTP_DELETED) != 0)
#define P_ISMETA(opaque)		(((opaque)->btpo_flags & BTP_META) != 0)
#define P_ISHALFDEAD(opaque)	(((opaque)->btpo_flags & BTP_HALF_DEAD) != 0)
#define P_IGNORE(opaque)		(((opaque)->btpo_flags & (BTP_DELETED|BTP_HALF_DEAD)) != 0)
#define P_HAS_GARBAGE(opaque)	(((opaque)->btpo_flags & BTP_HAS_GARBAGE) != 0)
#define P_INCOMPLETE_SPLIT(opaque)	(((opaque)->btpo_flags & BTP_INCOMPLETE_SPLIT) != 0)

/*
 *	Lehman and Yao's algorithm requires a ``high key'' on every non-rightmost
 *	page.  The high key is not a data key, but gives info about what range of
 *	keys is supposed to be on this page.  The high key on a page is required
 *	to be greater than or equal to any data key that appears on the page.
 *	If we find ourselves trying to insert a key > high key, we know we need
 *	to move right (this should only happen if the page was split since we
 *	examined the parent page).
 *
 *	Our insertion algorithm guarantees that we can use the initial least key
 *	on our right sibling as the high key.  Once a page is created, its high
 *	key changes only if the page is split.
 *
 *	On a non-rightmost page, the high key lives in item 1 and data items
 *	start in item 2.  Rightmost pages have no high key, so we store data
 *	items beginning in item 1.
 */

#define P_HIKEY				((OffsetNumber) 1)
#define P_FIRSTKEY			((OffsetNumber) 2)
#define P_FIRSTDATAKEY(opaque)	(P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY)


/*
 *	Operator strategy numbers for B-tree have been moved to access/stratnum.h,
 *	because many places need to use them in ScanKeyInit() calls.
 *
 *	The strategy numbers are chosen so that we can commute them by
 *	subtraction, thus:
 */
#define BTCommuteStrategyNumber(strat)	(BTMaxStrategyNumber + 1 - (strat))

/*
 *	When a new operator class is declared, we require that the user
 *	supply us with an amproc procedure (BTORDER_PROC) for determining
 *	whether, for two keys a and b, a < b, a = b, or a > b.  This routine
 *	must return < 0, 0, > 0, respectively, in these three cases.  (It must
 *	not return INT_MIN, since we may negate the result before using it.)
 *
 *	To facilitate accelerated sorting, an operator class may choose to
 *	offer a second procedure (BTSORTSUPPORT_PROC).  For full details, see
 *	src/include/utils/sortsupport.h.
 */

#define BTORDER_PROC		1
#define BTSORTSUPPORT_PROC	2
#define BTNProcs			2

/*
 *	We need to be able to tell the difference between read and write
 *	requests for pages, in order to do locking correctly.
 */

#define BT_READ			BUFFER_LOCK_SHARE
#define BT_WRITE		BUFFER_LOCK_EXCLUSIVE

/*
 *	BTStackData -- As we descend a tree, we push the (location, downlink)
 *	pairs from internal pages onto a private stack.  If we split a
 *	leaf, we use this stack to walk back up the tree and insert data
 *	into parent pages (and possibly to split them, too).  Lehman and
 *	Yao's update algorithm guarantees that under no circumstances can
 *	our private stack give us an irredeemably bad picture up the tree.
 *	Again, see the paper for details.
 */

typedef struct SK_BTStackData
{
	BlockNumber bts_blkno;
	OffsetNumber bts_offset;
	IndexTupleData bts_btentry;
	struct SK_BTStackData *bts_parent;
} SK_BTStackData;

typedef SK_BTStackData *BTStack;

typedef struct SkiplistContextData
{
    ItemPointerData preds[SKIPLIST_HEIGHT];
    ItemPointerData succs[SKIPLIST_HEIGHT];
    int lfound;
    int marked;
    int fullyLinked;
} SkiplistContextData;

typedef SkiplistContextData *SkiplistContext;

/*
 * BTScanOpaqueData is the btree-private state needed for an indexscan.
 * This consists of preprocessed scan keys (see sk_bt_preprocess_keys() for
 * details of the preprocessing), information about the current location
 * of the scan, and information about the marked location, if any.  (We use
 * BTScanPosData to represent the data needed for each of current and marked
 * locations.)	In addition we can remember some known-killed index entries
 * that must be marked before we can move off the current page.
 *
 * Index scans work a page at a time: we pin and read-lock the page, identify
 * all the matching items on the page and save them in BTScanPosData, then
 * release the read-lock while returning the items to the caller for
 * processing.  This approach minimizes lock/unlock traffic.  Note that we
 * keep the pin on the index page until the caller is done with all the items
 * (this is needed for VACUUM synchronization, see nbtree/README).  When we
 * are ready to step to the next page, if the caller has told us any of the
 * items were killed, we re-lock the page to mark them killed, then unlock.
 * Finally we drop the pin and step to the next page in the appropriate
 * direction.
 *
 * If we are doing an index-only scan, we save the entire IndexTuple for each
 * matched item, otherwise only its heap TID and offset.  The IndexTuples go
 * into a separate workspace array; each BTScanPosItem stores its tuple's
 * offset within that array.
 */

typedef struct SK_BTScanPosItem	/* what we remember about each match */
{
	ItemPointerData heapTid;	/* TID of referenced heap item */
	OffsetNumber indexOffset;	/* index item's location within page */
	LocationIndex tupleOffset;	/* IndexTuple's offset in workspace, if any */
} SK_BTScanPosItem;

typedef struct SK_BTScanPosData
{
	Buffer		buf;			/* if valid, the buffer is pinned */

	XLogRecPtr	lsn;			/* pos in the WAL stream when page was read */
	BlockNumber currPage;		/* page referenced by items array */
	BlockNumber nextPage;		/* page's right link when we scanned it */

	/*
	 * moreLeft and moreRight track whether we think there may be matching
	 * index entries to the left and right of the current page, respectively.
	 * We can clear the appropriate one of these flags when sk_bt_checkkeys()
	 * returns continuescan = false.
	 */
	bool		moreLeft;
	bool		moreRight;

	/*
	 * If we are doing an index-only scan, nextTupleOffset is the first free
	 * location in the associated tuple storage workspace.
	 */
	int			nextTupleOffset;

	/*
	 * The items array is always ordered in index order (ie, increasing
	 * indexoffset).  When scanning backwards it is convenient to fill the
	 * array back-to-front, so we start at the last slot and fill downwards.
	 * Hence we need both a first-valid-entry and a last-valid-entry counter.
	 * itemIndex is a cursor showing which entry was last returned to caller.
	 */
	int			firstItem;		/* first valid index in items[] */
	int			lastItem;		/* last valid index in items[] */
	int			itemIndex;		/* current index in items[] */

	SK_BTScanPosItem items[MaxIndexTuplesPerPage]; /* MUST BE LAST */
} SK_BTScanPosData;

typedef SK_BTScanPosData *BTScanPos;

#define BTScanPosIsPinned(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BufferIsValid((scanpos).buf) \
)
#define BTScanPosUnpin(scanpos) \
	do { \
		ReleaseBuffer((scanpos).buf); \
		(scanpos).buf = InvalidBuffer; \
	} while (0)
#define BTScanPosUnpinIfPinned(scanpos) \
	do { \
		if (BTScanPosIsPinned(scanpos)) \
			BTScanPosUnpin(scanpos); \
	} while (0)

#define BTScanPosIsValid(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BlockNumberIsValid((scanpos).currPage) \
)
#define BTScanPosInvalidate(scanpos) \
	do { \
		(scanpos).currPage = InvalidBlockNumber; \
		(scanpos).nextPage = InvalidBlockNumber; \
		(scanpos).buf = InvalidBuffer; \
		(scanpos).lsn = InvalidXLogRecPtr; \
		(scanpos).nextTupleOffset = 0; \
	} while (0);

/* We need one of these for each equality-type SK_SEARCHARRAY scan key */
typedef struct SK_BTArrayKeyInfo
{
	int			scan_key;		/* index of associated key in arrayKeyData */
	int			cur_elem;		/* index of current element in elem_values */
	int			mark_elem;		/* index of marked element in elem_values */
	int			num_elems;		/* number of elems in current array value */
	Datum	   *elem_values;	/* array of num_elems Datums */
} SK_BTArrayKeyInfo;

typedef struct SK_BTScanOpaqueData
{
	/* these fields are set by sk_bt_preprocess_keys(): */
	bool		qual_ok;		/* false if qual can never be satisfied */
	int			numberOfKeys;	/* number of preprocessed scan keys */
	ScanKey		keyData;		/* array of preprocessed scan keys */

	/* workspace for SK_SEARCHARRAY support */
	ScanKey		arrayKeyData;	/* modified copy of scan->keyData */
	int			numArrayKeys;	/* number of equality-type array keys (-1 if
								 * there are any unsatisfiable array keys) */
	int			arrayKeyCount;	/* count indicating number of array scan keys
								 * processed */
	SK_BTArrayKeyInfo *arrayKeys;	/* info about each equality-type array key */
	MemoryContext arrayContext; /* scan-lifespan context for array data */

	/* info about killed items if any (killedItems is NULL if never used) */
	int		   *killedItems;	/* currPos.items indexes of killed items */
	int			numKilled;		/* number of currently stored items */

	/*
	 * If we are doing an index-only scan, these are the tuple storage
	 * workspaces for the currPos and markPos respectively.  Each is of size
	 * BLCKSZ, so it can hold as much as a full page's worth of tuples.
	 */
	char	   *currTuples;		/* tuple storage for currPos */
	char	   *markTuples;		/* tuple storage for markPos */

	/*
	 * If the marked position is on the same page as current position, we
	 * don't use markPos, but just keep the marked itemIndex in markItemIndex
	 * (all the rest of currPos is valid for the mark position). Hence, to
	 * determine if there is a mark, first look at markItemIndex, then at
	 * markPos.
	 */
	int			markItemIndex;	/* itemIndex, or -1 if not valid */

	/* keep these last in struct for efficiency */
	SK_BTScanPosData currPos;		/* current position data */
	SK_BTScanPosData markPos;		/* marked position, if any */
	ItemPointerData thisNode;
} SK_BTScanOpaqueData;

typedef SK_BTScanOpaqueData *SK_BTScanOpaque;

/*
 * We use some private sk_flags bits in preprocessed scan keys.  We're allowed
 * to use bits 16-31 (see skey.h).  The uppermost bits are copied from the
 * index's indoption[] array entry for the index attribute.
 */
#define SK_BT_REQFWD	0x00010000	/* required to continue forward scan */
#define SK_BT_REQBKWD	0x00020000	/* required to continue backward scan */
#define SK_BT_INDOPTION_SHIFT  24	/* must clear the above bits */
#define SK_BT_DESC			(INDOPTION_DESC << SK_BT_INDOPTION_SHIFT)
#define SK_BT_NULLS_FIRST	(INDOPTION_NULLS_FIRST << SK_BT_INDOPTION_SHIFT)

/*
 * external entry points for btree, in nbtree.c
 */
extern IndexBuildResult *sk_btbuild(Relation heap, Relation index,
		struct IndexInfo *indexInfo);
extern void sk_btbuildempty(Relation index);
extern bool sk_btinsert(Relation rel, Datum *values, bool *isnull,
		 ItemPointer ht_ctid, Relation heapRel,
		 IndexUniqueCheck checkUnique,
		 struct IndexInfo *indexInfo);
extern IndexScanDesc sk_btbeginscan(Relation rel, int nkeys, int norderbys);
extern Size sk_btestimateparallelscan(void);
extern void sk_btinitparallelscan(void *target);
extern bool sk_btgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 sk_btgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void sk_btrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys);
extern void sk_btparallelrescan(IndexScanDesc scan);
extern void sk_btendscan(IndexScanDesc scan);
extern void sk_btmarkpos(IndexScanDesc scan);
extern void sk_btrestrpos(IndexScanDesc scan);
extern IndexBulkDeleteResult *sk_btbulkdelete(IndexVacuumInfo *info,
			 IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback,
			 void *callback_state);
extern IndexBulkDeleteResult *sk_btvacuumcleanup(IndexVacuumInfo *info,
				IndexBulkDeleteResult *stats);
extern bool sk_btcanreturn(Relation index, int attno);

/*
 * prototypes for internal functions in nbtree.c
 */
extern bool sk_bt_parallel_seize(IndexScanDesc scan, BlockNumber *pageno);
extern void sk_bt_parallel_release(IndexScanDesc scan, BlockNumber scan_page);
extern void sk_bt_parallel_done(IndexScanDesc scan);
extern void sk_bt_parallel_advance_array_keys(IndexScanDesc scan);

/*
 * prototypes for functions in nbtinsert.c
 */
extern bool sk_bt_doinsert(Relation rel, IndexTuple itup,
			 IndexUniqueCheck checkUnique, Relation heapRel);
extern Buffer sk_bt_getstackbuf(Relation rel, BTStack stack, int access);
extern void sk_bt_finish_split(Relation rel, Buffer bbuf, BTStack stack);

/*
 * prototypes for functions in nbtpage.c
 */
extern void sk_bt_initmetapage(Page page, BlockNumber rootbknum, uint32 level);
extern Buffer sk_bt_getroot(Relation rel, int access);
extern Buffer sk_bt_gettrueroot(Relation rel);
extern int	sk_bt_getrootheight(Relation rel);
extern void sk_bt_checkpage(Relation rel, Buffer buf);
extern Buffer sk_bt_getbuf(Relation rel, BlockNumber blkno, int access);
extern Buffer sk_bt_relandgetbuf(Relation rel, Buffer obuf,
				 BlockNumber blkno, int access);
extern void sk_bt_relbuf(Relation rel, Buffer buf);
extern void sk_bt_pageinit(Page page, Size size);
extern bool sk_bt_page_recyclable(Page page);
extern void sk_bt_delitems_delete(Relation rel, Buffer buf,
					OffsetNumber *itemnos, int nitems, Relation heapRel);
extern void sk_bt_delitems_vacuum(Relation rel, Buffer buf,
					OffsetNumber *itemnos, int nitems,
					BlockNumber lastBlockVacuumed);
extern int	sk_bt_pagedel(Relation rel, Buffer buf);

/*
 * prototypes for functions in nbtsearch.c
 */
extern SkiplistContext sk_bt_search(Relation rel,
		   int keysz, ScanKey scankey, bool nextkey,
		   Buffer *bufP, int access, Snapshot snapshot);
extern Buffer sk_bt_moveright(Relation rel, Buffer buf, int keysz,
			  ScanKey scankey, bool nextkey, bool forupdate, BTStack stack,
			  int access, Snapshot snapshot);
extern OffsetNumber sk_bt_binsrch(Relation rel, Buffer buf, int keysz,
			ScanKey scankey, bool nextkey);
extern int32 sk_bt_compare(Relation rel, int keysz, ScanKey scankey,
			Page page, OffsetNumber offnum);
extern bool sk_bt_first(IndexScanDesc scan, ScanDirection dir);
extern bool sk_bt_next(IndexScanDesc scan, ScanDirection dir);
extern Buffer sk_bt_get_endpoint(Relation rel, uint32 level, bool rightmost,
				 Snapshot snapshot);

/*
 * prototypes for functions in nbtutils.c
 */
extern ScanKey sk_bt_mkscankey(Relation rel, IndexTuple itup);
extern ScanKey sk_bt_mkscankey_nodata(Relation rel);
extern void sk_bt_freeskey(ScanKey skey);
extern void sk_bt_freestack(SkiplistContext stack);
extern void sk_bt_preprocess_array_keys(IndexScanDesc scan);
extern void sk_bt_start_array_keys(IndexScanDesc scan, ScanDirection dir);
extern bool sk_bt_advance_array_keys(IndexScanDesc scan, ScanDirection dir);
extern void sk_bt_mark_array_keys(IndexScanDesc scan);
extern void sk_bt_restore_array_keys(IndexScanDesc scan);
extern void sk_bt_preprocess_keys(IndexScanDesc scan);
extern IndexTuple sk_bt_checkkeys(IndexScanDesc scan,
			  Page page, OffsetNumber offnum,
			  ScanDirection dir, bool *continuescan);
extern void sk_bt_killitems(IndexScanDesc scan);
extern SK_BTCycleId sk_bt_vacuum_cycleid(Relation rel);
extern SK_BTCycleId sk_bt_start_vacuum(Relation rel);
extern void sk_bt_end_vacuum(Relation rel);
extern void sk_bt_end_vacuum_callback(int code, Datum arg);
extern Size sk_BTreeShmemSize(void);
extern void sk_BTreeShmemInit(void);
extern bytea *sk_btoptions(Datum reloptions, bool validate);
extern bool sk_btproperty(Oid index_oid, int attno,
		   IndexAMProperty prop, const char *propname,
		   bool *res, bool *isnull);

/*
 * prototypes for functions in skiplistnode.c
 */

/*
 * prototypes for functions in nbtvalidate.c
 */
extern bool sk_btvalidate(Oid opclassoid);

/*
 * prototypes for functions in nbtsort.c
 */
typedef struct BTSpool BTSpool; /* opaque type known only within nbtsort.c */

extern BTSpool *sk_bt_spoolinit(Relation heap, Relation index,
			  bool isunique, bool isdead);
extern void sk_bt_spooldestroy(BTSpool *btspool);
extern void sk_bt_spool(BTSpool *btspool, ItemPointer self,
		  Datum *values, bool *isnull);
extern void sk_bt_leafbuild(BTSpool *btspool, BTSpool *spool2);

#endif							/* NBTREE_H */
