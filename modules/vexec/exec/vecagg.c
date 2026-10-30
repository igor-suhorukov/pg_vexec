/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecagg.c
 *	  VecAgg: aggregation, plain or hashed, of any split, its input a batch
 *	  at a time (pg_vector_executor.md §3.8).
 *
 * The node reads its child's batches -- or its rows, a batch at a time,
 * where the child hands up none, as an Agg reads every row of its input
 * anyway -- and the aggregates' arguments and FILTERs are compiled over them
 * as a VecResult's targets are (expr/): the eager ones a batch at a time,
 * the rest, and the rows a kernel could not compute, by PostgreSQL's
 * evaluator, a row at a time.
 *
 * Each row then finds its group and advances the aggregates' states there
 * (aggtrans.c).  What a row of nodeAgg.c runs, it runs in nodeAgg.c's
 * order (execExpr.c:3700-3960): the group looked up, then each transition
 * in turn, its FILTER, its arguments, its function.  A vectorized transition
 * takes the batch's rows at once after that, where the order cannot show:
 * where it cannot raise and its sums do not depend on the order (counts,
 * integer and numeric sums, min, max, bool_and), or, for a float's sums,
 * where no row of the batch goes to PostgreSQL's evaluator and nothing else
 * in the node can raise.  Anywhere else it is advanced in the row loop, in
 * its turn.  So an error is the one nodeAgg.c raises, at the row it raises
 * it at, and a float's sum is the same bits.
 *
 * The table begins at the planner's estimate of the groups, or at the most
 * groups the same node's table held before where that is more (C6, §3.19;
 * aggsizes.c), within the memory it may hold.
 *
 * Grouping is a hash table over the full keys (lib/simplehash.h), hashed
 * and compared as the grouping operators compare: integers and the like by
 * their bits, text of a deterministic collation and bytea by their bytes,
 * any other type through its hash and equality functions.  The keys are
 * plain columns of the input, as an Agg's are (grpColIdx).
 *
 * The table is held to get_hash_memory_limit(), measured with
 * MemoryContextMemAllocated() each time a group is made.  Past it, a row
 * whose group is not in the table goes to the partition on disk that bits
 * of its hash choose -- the input row itself, its arguments evaluated again
 * when it is read back, as nodeAgg.c's spill does (nodeAgg.c:3029-3077) --
 * and the partitions are aggregated in turn, through the same batches, once
 * the table's groups are out.  Each level of partitions takes the next bits
 * of the hash; past the last level the table grows, as nodeAgg.c's does
 * when its hash bits run out.
 *
 * A first stage that stops taking new keys (C2, §3.19).  A partial
 * aggregation under a VecRepartition -- whose final stage, in every
 * participant, combines what it is dealt -- freezes its table once it holds
 * vexec.partial_agg_limit groups, or its groups and their states take
 * VEXEC_AGG_FREEZE_BYTES -- a size that stays in the cache; its buckets,
 * made for the planner's estimate, are not counted -- as ClickHouse's
 * adaptive aggregator does
 * (enable_adaptive_aggregator, CH:src/Interpreters/AdaptiveAggregation.h):
 * a row whose key is in it updates it, and a batch's other rows aggregate
 * into a table of the batch's own, whose groups go on to the repartition as
 * partial states once the batch is done, and which then starts empty.  The
 * final stage merges a key's states, however many, as it merges those of
 * several participants (C1's kernels).  So a grouping that barely reduces
 * -- a key a row or two -- spends nothing on a table of millions, and one
 * that does keeps the reduction its frozen table holds.  A frozen table
 * thaws -- takes new keys again, and does not freeze again until it is
 * reset -- where the groups it hands on repeat their keys, as ClickHouse's
 * thaws a stream that keeps staging the same keys
 * (CH:src/Interpreters/AdaptiveAggregationImpl.h): each repeat is a row the
 * table would have merged where it came, which goes on through the
 * repartition's files as a group of its own.  A HyperLogLog counts the keys
 * handed on since the table froze; past VEXEC_AGG_THAW_EVIDENCE groups, at
 * VEXEC_AGG_THAW_REPEAT groups a key or more, the table thaws.  It freezes
 * again, for good, at VEXEC_AGG_THAW_GROWTH times the bounds it froze at
 * (F16, pg_vector_executor.md §3.37): a table that grows past the caches
 * misses them at every row, which costs more than the final stage's merge
 * of the groups it would have held.  Nor does a first stage that may
 * freeze spill: past its memory it freezes, and thaws no more.
 *
 * The top n kept inside (C3, §3.19).  A bounded VecSort above lends the
 * node its keys and its bound (vexec_agg_set_topn()), as a bounded sort
 * lends its scan a running bound (H6).  The node then ranks every group by
 * the sort's keys, computed from only the columns they read -- a count is
 * its state -- after HAVING, keeps a heap of the n best, and finishes and
 * projects only those, which it hands up for the sort to order: so the
 * final functions, the projection and the sort run for n groups, not for
 * every one.  Under H5's Gather Merge each participant's groups are its own,
 * so each one's n best hold the n best of all.  Only where the output and
 * HAVING are free of volatile functions and subplans, which would otherwise
 * run for fewer groups than they do.
 *
 * Groups in their keys' order (VC4).  Where the planner found the input a
 * serial scan whose source hands its rows in an order the keys are a prefix
 * of (vexec_source.h's ordering(), minor 4) -- the rows of equal keys one
 * after another -- the node takes it so, if the scan's source says, as it
 * begins, that its rows come so: a row whose keys are the open group's
 * goes to it, any other closes it and opens the next, nothing hashed or
 * looked up but each group's hash once, which a stage above may read (C4).
 * The groups a batch closes are handed up once it is done, the open one
 * kept: so no table and no spill, the groups in two generations of memory,
 * the older one emptied once its groups are out and the open group is the
 * newer one's.  A scan whose rows do not come so -- its storage's units
 * changed since the plan was made, or a top n, a LIMIT's groups, a
 * dealing or a freeze asked of the node -- is hashed as any other.
 * ClickHouse's MergeTree in one part, TPC-H's lineitem grouped by its key:
 * 29-47% faster serially than hashing, 45-65% with 4 MB of work_mem
 * (ch_table_engines.md §3, "VC4, VC6 and VC16, researched").
 *
 * Groups handed on as they lie (D2, §3.20).  A first stage under a
 * VecRepartition whose every state is one of vexec's own without a pointer
 * -- counts, integer sums and averages, int128 sums, min and max of a
 * by-value type, bool_and and bool_or -- and whose final stage can merge
 * them packs its groups into the partitions (vecrepart.c): a packed group
 * is a group as the table lays it out -- its hash, keys, extras and states
 * -- its length beside the hash, and each by-reference value's bytes after
 * the states, at the offset from the group's start its Datum then holds.
 * A frozen table's batch table (C2) makes its groups packed, in their
 * partitions' chunks, as the batch finds them, so that handing them on
 * copies nothing; the table's own groups are packed once the input is read.
 * The final stage merges each packed group into its table as it lies: its
 * keys looked up where they lie, its states added to the group's as the
 * combine functions would add the partial states PostgreSQL's forms of
 * them would be (aggtrans.c), with no slot, array or bytea between.
 *
 * The first stage bypassed (§3.20, as DuckDB's sink skips its lookups,
 * DD:src/execution/radix_partitioned_hashtable.cpp).  A frozen table whose
 * keys a batch's rows seldom meet, in the table or among the batch's own
 * groups -- a key a row or two, Q32's (WatchID, ClientIP) -- reduces
 * nothing, and still looks each row up twice.  Once the rows of
 * VEXEC_AGG_BYPASS_EVIDENCE make VEXEC_AGG_BYPASS_SHARE groups a row or
 * more, each row of the next VEXEC_AGG_BYPASS_BATCHES batches is a packed
 * group of its own, made in its partition with no lookup; the batches
 * after them are looked up again, to measure again.  The final stage
 * merges a key's groups however many there are.
 *
 * Its output is a row a call: a group's keys, the columns taken from its
 * first row, and its aggregates' results, in the scan tuple that
 * custom_scan_tlist describes, through the node's HAVING qual and target
 * list, which read them as INDEX_VAR.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodeAgg.h"
#include "lib/hyperloglog.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "utils/sortsupport.h"
#include "port/pg_bitutils.h"
#include "storage/buffile.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "utils/ruleutils.h"
#include "utils/typcache.h"
#include "varatt.h"

#include "cb_explain.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/aggtrans.h"
#include "exec/device.h"
#include "exec/exec.h"
#include "exec/keyhash.h"
#include "expr/expr.h"
#include "expr/kernel.h"
#include "plan/plan.h"

/* How a key column is hashed and compared. */
typedef enum VexecAggKeyKind
{
	KEY_BITS,					/* a by-value type equal where its bits are */
	KEY_BYTES,					/* text of a deterministic collation, bytea:
								 * equal where their bytes are */
	KEY_FMGR					/* the type's hash and equality functions */
} VexecAggKeyKind;

typedef struct VexecAggKeyCol
{
	int			col;			/* the input column, from 0 */
	VexecAggKeyKind kind;
	int16		typlen;
	bool		typbyval;
	Oid			eqop;			/* the grouping's equality operator */
	Oid			collation;
	bool		numeric;		/* F10: numeric's, by a batch's scaled integers where it has them */
	FmgrInfo	hash;
	FmgrInfo	eq;
} VexecAggKeyCol;

/* A row's key, as it is looked up. */
typedef struct VexecAggProbe
{
	Datum		value;
	const char *bytes;			/* KEY_BYTES */
	int32		len;
	bool		isnull;
	bool		scaled;			/* F10: a numeric key's value the int64 at `scale`, its numeric not made */
	int16		scale;
} VexecAggProbe;

/* simplehash's entry: a group, found by its hash and its keys */
typedef struct VexecAggEntry
{
	char	   *group;
	uint32		hash;
	char		status;
} VexecAggEntry;

typedef struct VexecAggState VexecAggState;

static bool group_equal(VexecAggState *s, const char *group);

#define SH_PREFIX vagg
#define SH_ELEMENT_TYPE VexecAggEntry
#define SH_KEY_TYPE char *
#define SH_KEY group
#define SH_HASH_KEY(tb, key) (*(const uint32 *) (key))
#define SH_EQUAL(tb, a, b) group_equal((VexecAggState *) (tb)->private_data, (a))
#define SH_SCOPE static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) ((a)->hash)
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/* A partition of spilled input rows. */
typedef struct VexecAggPartition
{
	BufFile    *file;
	int64		rows;
	int			depth;			/* the level its rows were spilled at */
	BufFile    *pfile;			/* the device strategy's groups the table
								 * could not take, packed (V8b): read back
								 * before the partition's rows, into the
								 * same table */
	int			sub_begin;		/* F23: a run of a final stage's sub-buffers,
								 * [sub_begin, sub_end) in memory; sub_end 0
								 * none */
	int			sub_end;
} VexecAggPartition;

struct AggDevice;

/* X5: the most slots an array of groups has, Velox's 2M (velox/exec/HashTable.h) */
#define VEXEC_AGG_ARRAY_SLOTS		(1 << 21)

/* C2: the bytes a first stage's table freezes at, ClickHouse's 4 MiB */
#define VEXEC_AGG_FREEZE_BYTES		((Size) 4 * 1024 * 1024)

/*
 * C2: a frozen table thaws once this many groups have gone on, or more, at
 * this many a key or more: a table taking them would have merged a third
 */
#define VEXEC_AGG_THAW_EVIDENCE		16384
#define VEXEC_AGG_THAW_REPEAT		1.5

/*
 * F16: a thawed table freezes again at this many times its first bounds --
 * 131,072 groups or 32 MiB -- where C2's thaw case, 100,000 keys of avg()'s
 * states, fits; ClickBench's URL groupings at 100M rows grew to 1.3M groups,
 * 270 to 385 MB a participant, and ran 13% slower than never thawed
 */
#define VEXEC_AGG_THAW_GROWTH		8

/*
 * The first stage bypassed (above): rows a frozen table measures over, the
 * share of them that must be groups of their own, and the batches then
 * bypassed before it measures again
 */
#define VEXEC_AGG_BYPASS_EVIDENCE	8192
#define VEXEC_AGG_BYPASS_SHARE		0.98
#define VEXEC_AGG_BYPASS_BATCHES	64

/*
 * F33 (§3.38): the share where the rows measured are those a run's first row
 * alone looks up (key_runs()): a lookup costs little against a packed group's
 * way through the exchange, but ClickBench's Q4, Q8, Q15, Q16 and Q35 at 100M
 * rows find 90 to 97% of theirs make groups, at a lookup in each of two tables
 * for the few they find.  0.70 and 0.95 measured within 0.6% of this.
 */
#define VEXEC_AGG_BYPASS_RUN_SHARE	0.85

#define VEXEC_AGG_PARTITION_BITS	5
#define VEXEC_AGG_PARTITIONS		(1 << VEXEC_AGG_PARTITION_BITS)
#define VEXEC_AGG_MAX_DEPTH			(32 / VEXEC_AGG_PARTITION_BITS)

/*
 * F23 (§3.38): a final stage's packed groups scattered by their hash's top
 * VEXEC_AGG_SUB_BITS bits into as many sub-buffers held in memory, then
 * merged a run of sub-buffers at a time -- VEXEC_AGG_SUB_RECORDS records at
 * most, where there are more -- into a table sized for the run, its groups
 * handed on and the table emptied before the next: the run's table and
 * groups within the caches, where one table of every group was a random
 * DRAM access a record.  The runs are read as spilled partitions of level
 * VEXEC_AGG_SUB_DEPTH, so that one spilling again deals its records by the
 * bits below the sub-buffers'.  A sub-buffer's chunks double from
 * VEXEC_AGG_SUB_CHUNK_FIRST bytes to VEXEC_AGG_SUB_CHUNK_MAX.  Past the
 * node's memory, the records held are merged into one table of every group
 * a window at a time, in the sub-buffers' order, as before but for the
 * order.
 */
#define VEXEC_AGG_SUB_BITS			8
#define VEXEC_AGG_SUBS				(1 << VEXEC_AGG_SUB_BITS)
#define VEXEC_AGG_SUB_RECORDS		8192
#define VEXEC_AGG_SUB_DEPTH			2
#define VEXEC_AGG_SUB_CHUNK_FIRST	((Size) 2048)
#define VEXEC_AGG_SUB_CHUNK_MAX		((Size) 256 * 1024)

/* F23: a sub-buffer's records, in chunks */
typedef struct VexecAggSubChunk
{
	struct VexecAggSubChunk *next;
	Size		used;
	Size		size;			/* of data */
	char	   *data;			/* MAXALIGNed, the records back to back */
} VexecAggSubChunk;

typedef struct VexecAggSub
{
	VexecAggSubChunk *first;
	VexecAggSubChunk *last;
	int64		rows;
} VexecAggSub;

/*
 * F29 (§3.38): a sub-buffer's entry where the exchange keeps the records
 * where they lie (vexec_repart_keep_chunks()): the record's hash, and where
 * it is -- in the exchange's chunk, or a file's record's copy
 */
typedef struct VexecAggSubRef
{
	uint32		hash;
	uint32		len;
	const char *rec;
} VexecAggSubRef;

/* F29: the records of files copied for the references, in chunks of this many bytes */
#define VEXEC_AGG_COPY_CHUNK		((Size) 256 * 1024)

struct VexecAggState
{
	VexecNode	node;			/* first: the input side, the aggregates'
								 * arguments and FILTERs compiled over the
								 * input's columns */
	PlanState  *child;
	bool		child_batches;	/* the child hands up batches */
	bool		child_done;		/* the child has returned its last row: a
								 * node is not called past its end, where an
								 * index scan would begin again */
	VexecBatch *rowbatch;		/* rows into a batch: the child's, or a
								 * partition's */
	TupleDesc	indesc;

	AggStrategy strategy;
	AggSplit	split;
	double		numgroups;
	double		group_bytes;	/* W45: the planner's bytes of a group on the
								 * device; 0: none */

	/* the keys, and the row being looked up */
	int			nkeys;
	VexecAggKeyCol *keys;
	VexecAggProbe *probe;		/* the row being looked up: its keys of
								 * rowkeys, or probe_slot's */
	VexecAggProbe *probe_slot;	/* a row PostgreSQL's evaluator has */
	VexecAggProbe *rowkeys;		/* a batch's keys, nkeys a row */
	uint32	   *rowhash;		/* and their hashes */
	bool		hashes_from_child;	/* C4: a VecRepartition child deals out
									 * rows by these keys and hands up their
									 * hashes, which are this node's */
	bool		hashes_read;	/* C4: a VecRepartition parent reads this
								 * node's hashes as its own */
	const uint32 *batch_hashes; /* and those of the batch being read, or
								 * NULL */
	uint32		last_hash;		/* the hash of the group last returned */
	int		   *rows;			/* the batch's rows to take, in order */

	/*
	 * VC5: per key, the hashes of a dictionary key's values, for the
	 * dictionary of that identity (D6): a value's hash computed once for
	 * every batch that comes with it
	 */
	uint64	   *dict_hash_ids;
	uint32	  **dict_hashes;
	char	   *probe_group;	/* stands for the probe in the table */

	/* the columns a group keeps from its first row */
	int			nextra;
	int		   *extracol;

	/* what each column of the scan tuple is */
	int			noutcols;
	int		   *outkind;		/* VEXEC_AGGCOL_* */
	int		   *outsrc;

	/* the aggregates */
	AggState   *aggstate;
	int			naggs;
	int			ntrans;
	VexecAggTrans *trans;
	int			ninputs;		/* the input targets: each transition's
								 * arguments, then its FILTER */
	bool		any_lazy;		/* an input PostgreSQL's evaluator computes */
	bool		floats_ok;		/* a float's transition may take a batch at
								 * once: nothing else here can raise */
	bool	   *batched;		/* per transition, for the current batch */
	Datum	   *argvals;
	bool	   *argnulls;

	/* a group: a header with its hash, keys, extras, states */
	Size		keyoff;
	Size		keyformoff;		/* F14: a byte a key, the form its Datum is in, where a key is
								 * numeric; 0 where none is */
	Size		extraoff;
	Size		stateoff;
	Size		groupsize;

	/* the table */
	ExprContext *aggcontext;	/* the states' memory, as AggCheckCallContext
								 * gives it */
	ExprContext *tmpcontext;	/* a row's */
	MemoryContext groupcxt;		/* groups, their keys and extras */
	MemoryContext hashcxt;
	struct vagg_hash *table;
	char	   *plaingroup;
	Size		memlimit;
	char	  **groups;			/* per row of a batch: its group */

	/*
	 * X9: the table's groups in the order they were made, in the table's
	 * memory, which they are handed up in: a group's memory follows the
	 * one made before it, where the buckets' order is the hashes', a cache
	 * miss a group
	 */
	char	  **made;
	int64		nmade;
	int64		made_size;
	int64		next_made;

	/*
	 * X5: the array mode (above): the table an array of groups, a slot for
	 * each combination of the keys' values in their ranges, NULL's first
	 */
	bool		array_ok;		/* every key an integer's bits */
	bool		array_decided;	/* for the current table */
	bool		array_mode;
	char	  **slots;
	int64	   *alo;			/* per key: its range */
	int64	   *ahi;
	int64	   *awidth;			/* per key: its slots, NULL's and the range's */
	int64		nslots;
	int64		array_tables;	/* EXPLAIN ANALYZE: tables made arrays, */
	int64		array_left;		/* and those hashed after all */
	int64		array_slots;	/* the most slots one had */

	/* spill */
	bool		spilling;
	VexecAggPartition *parts;	/* the level being written */
	int			depth;			/* the level of the rows being read */
	List	   *pending;		/* partitions written, to read */
	VexecAggPartition *reading;
	bool	   *spillneeded;	/* per input column: kept when spilled */
	TupleTableSlot *spillslot;
	TupleTableSlot *readslot;

	/* F23: the final stage's sub-buffers (above), and what they did */
	bool		sub_on;			/* the fill scatters its packed groups */
	VexecAggSub *subs;
	MemoryContext subcxt;
	Size		sub_bytes;
	bool		sub_windowed;	/* the records merged a window at a time */
	int64		sub_runs;
	int64		sub_windows;
	int64		sub_records;

	/* F29 (§3.38): the records referenced where the exchange keeps them, and what they did */
	bool		sub_refs;		/* this fill's sub-buffers hold references */
	bool		sub_kept;		/* the exchange keeps the chunks read: let go once the groups are out */
	MemoryContext subcopycxt;	/* a file's records copied, as long as the references */
	char	   *copy_at;
	Size		copy_left;
	bool		rec_groups;		/* a record of the producer's is a group of this node's as it lies */
	bool		rec_groups_live;	/* the records taken now stay until their groups are out */
	bool		pack_taken;		/* the record taken became its group: nothing of it to merge */
	uint32	   *pk_hashes;		/* beside pk_recs, the references' hashes */
	bool		pk_hashes_on;
	int64		sub_referenced;
	int64		sub_copied;
	int64		rec_groups_made;

	/* F26 (§3.38): rows that took the previous row's group, its keys theirs */
	int64		consecutive_rows;
	/* F31 (§3.38): batches of one key, taken as a plain aggregation's into its group */
	int64		one_key_batches;
	/* F30 (§3.38): a batch's integer keys read from their columns, no probe made a row */
	bool		keys_vectored;	/* this batch's: rowhash set, rowkeys not */
	VexecAggProbe *vprobe;		/* a row's probe, made as it is looked up */
	int64		vector_key_batches;
	/* F37 (§3.38): the inputs' expressions, for the scan below (agg_tell_lengths()) */
	List	   *input_exprs;
	bool		lengths_told;
	/* F32 (§3.38): a batch's rows cut into runs of one key (key_runs()) */
	bool		keys_runs;		/* this batch's: rowhash and rowkeys of the runs' first rows alone */
	int			nruns;
	int		   *run_begin;		/* each run's first place in rows; its end the next's */
	uint8	   *run_diff;		/* per place in rows: its keys are not the place before's */
	int64		run_batches;
	int64		runs_total;

	/*
	 * C2: a first stage under a VecRepartition freezes its table (above);
	 * the batch's table, its groups and their states' memory, handed on as
	 * each batch ends
	 */
	bool		may_freeze;		/* the VecRepartition above allows it */
	bool		frozen;
	bool		thawed;			/* frozen no more until the table is reset */
	bool		full;			/* F16: frozen for good, past its memory or its thawed bounds */
	hyperLogLogState passed_keys;	/* the keys handed on since it froze */
	bool		passed_keys_made;
	int64		passed_since;	/* the groups handed on since it froze */
	int64		thaw_next;		/* passed_since at the next look */
	struct vagg_hash *btable;
	MemoryContext bgroupcxt;
	MemoryContext bhashcxt;
	ExprContext *baggcontext;
	uint64	   *bmask;			/* the batch's rows whose group is the batch
								 * table's */
	bool		boutput;		/* its groups are being handed on */
	bool		biterating;
	vagg_iterator biter;

	/*
	 * C3: the top n a bounded VecSort above lends (vexec_agg_set_topn()):
	 * the sort's keys, columns of the output, their programs over the scan
	 * tuple and orders, the scan tuple's columns they and HAVING read, and a
	 * heap of the kept rows, the worst at its root
	 */
	int64		topn;
	int			ntopkeys;
	ExprState **topkey;
	SortSupport topssup;
	int16	   *topkeylen;
	bool	   *topkeybyval;
	bool	   *topneeded;
	MemoryContext topcxt;
	struct TopEntry *top;
	int			ntop;
	bool		top_filled;
	int			topnext;
	TupleTableSlot *topslot;
	int64		top_ranked;		/* groups ranked, for EXPLAIN ANALYZE */
	Datum	   *topkv;			/* a group's keys, as it is ranked */
	bool	   *topkn;
	int		   *topkeycol;		/* a key that is a column of the scan tuple:
								 * its index, read as it is; else -1 */
	int		   *topcols;		/* the scan tuple's columns ranking reads */
	int			ntopcols;
	Size		topdirect;		/* a lone key that is a count: its state in
								 * a group, read as it lies; else 0 */

	/*
	 * C5 (§3.19): under a LIMIT without ORDER BY, the groups of the topk
	 * smallest hashes kept: a max-heap of the smallest hashes of the groups
	 * made, its root the boundary once it is full, the boundary the
	 * participants share where a VecRepartition gives one, and the stage
	 * whose boundary a final stage takes, its child's
	 */
	int64		topk;
	bool		topk_skip;		/* rows past the boundary passed over */
	uint32	   *theap;
	int			ntheap;
	uint32		tbound;			/* this participant's; PG_UINT32_MAX: none */
	pg_atomic_uint32 *tshared;	/* the participants', or NULL */
	uint32		tfinal;			/* the boundary the table's output is held */
	bool		tfinal_set;		/* to, once the table is filled */
	int64		rows_skipped;	/* EXPLAIN ANALYZE's */

	/* output */
	bool		filled;
	bool		done;
	VexecBatch *outb;			/* X9: the batch of groups a vector parent
								 * takes, the scan tuple's columns */
	uint32	   *outhash;		/* and its rows' groups' hashes */

	/*
	 * X9: the groups dealt into a VecRepartition's partitions as they go
	 * out (vexec_agg_deal()): a batch a partition, the scan tuple's columns
	 * and an int4 of each row's hash, handed up as it fills, and each one's
	 * rest at the end
	 */
	int			ndeal;			/* the partitions; 0: none */
	int			(*deal) (void *arg, uint32 hash);
	void	   *deal_arg;
	VexecType **dealtypes;
	VexecBatch **dealt;
	int			dealt_size;
	int			dealt_out;		/* the partition of the batch handed up last,
								 * or -1 */
	int			deal_rest;		/* at the end: the next partition to hand up */
	Datum	   *dealvals;
	bool	   *dealnulls;

	/*
	 * D2 (above): a first stage's groups packed into a VecRepartition's
	 * partitions -- whether every key, extra and state can be, and where
	 * they go -- and a final stage's taken from them: the stage below, and
	 * where each of this one's keys, extras and states lie in its groups
	 */
	bool		pack_ok;
	bool		pack_values;	/* F5: a text's minimum or maximum, or a
								 * numeric sum, among the states: each group
								 * packed by a copy */
	VexecPackSink *sink;		/* NULL: none packed */
	Datum	   *pk_keyvals;		/* a group's by-reference keys and extras, */
	Datum	   *pk_extravals;	/* as they are packed */
	bool	   *pk_extranulls;
	int64		packed_groups;
	bool		unpack_ok;
	bool		unpacking;		/* this run's input is packed groups */
	struct VexecAggState *producer;
	int		   *pk_key;			/* per key: the producer's */
	int		   *pk_extra;		/* per extra: the producer's key, or -1 less
								 * its extra */
	Size	   *pk_stateoff;	/* per transition: the producer's state */
	VexecAggTrans **pk_trans;	/* and the producer's transition */
	VexecAggProbe *pk_probe;	/* a packed group's keys, looked up */
	const char *pack_rec;		/* the packed group a new group takes its
								 * extras from */
	char	  **pk_recs;		/* packed groups, a batch's worth */
	MemoryContext pk_readcxt;	/* those read back from a spill file */
	int64		unpacked_groups;

	/*
	 * The first stage bypassed (above): the frozen table's rows measured,
	 * those that made a group, whether the rows bypass the tables now, and
	 * for how many batches more
	 */
	int64		by_rows;
	int64		by_groups;
	bool		bypass;
	int			bypass_left;
	int64		bypassed_rows;	/* EXPLAIN ANALYZE's */

	/* figures */
	int64		ngroups;
	int64		total_groups;
	Size		peak_memory;
	int64		spilled_rows;
	int64		spilled_partitions;
	int			max_depth;
	int64		rows_by_postgres;
	int64		rows_batched;
	uint64		size_key;		/* C6: the node's slot of sizes */
	uint64		buckets_initial;	/* the first table's buckets, and the */
	uint64		buckets_final;	/* most any table had, for EXPLAIN */
	bool		table_lazy;		/* W23: a hashed table made at its first group */
	uint64		table_hint;		/* W23: the groups an empty table is made for, or 0 */
	int64		frozen_groups;	/* C2: the groups the table froze at */
	int64		passed_groups;	/* the batch tables' groups handed on */
	int64		thawed_after;	/* those handed on before the table thawed */
	int64		refrozen_groups;	/* F16: the groups a thawed table froze again at */

	/*
	 * VC4: the input in the keys' order (above): planned, decided at the
	 * first group asked for, and taken; the open group, its generation,
	 * the generation new groups go to, and the two generations' memory --
	 * their groups, and their states' as AggCheckCallContext gives it.
	 * The groups a batch closes wait in `made` to be handed up.
	 */
	bool		sorted_planned;
	bool		sorted_decided;
	bool		sorted;
	bool		sorted_eof;		/* the input is read: the open group is the last */
	char	   *cur;
	int			cur_gen;
	int			sgen;
	MemoryContext sgroupcxt[2];
	ExprContext *saggcontext[2];

	/*
	 * H2: the requests the child's source answers from its statistics, a
	 * unit of rows at a time; nstats -1 where it is asked none
	 */
	int			nstats;
	VexecSourceAgg *stats_reqs;
	VexecSourceAggAnswer *stats_answers;
	int		   *stats_first;	/* per transition: its first request */

	/*
	 * The device strategy (§3.11, V8b): planned, or forced; the node's
	 * device side where it may take it as it begins, else why not
	 */
	bool		dev_planned;
	struct AggDevice *dev;
	const char *dev_why;
	List	   *whole_cols;		/* W1: H3's upper stage the device's whole (plan) */
};

static void agg_batch(VexecAggState *s);
static void agg_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *agg_exec(CustomScanState *css);
static void agg_end(CustomScanState *css);
static void agg_rescan(CustomScanState *css);
static void agg_explain(CustomScanState *css, List *ancestors, ExplainState *es);
static void table_reset(VexecAggState *s);
static void topk_note(VexecAggState *s, uint32 hash);

static const CustomExecMethods agg_exec_methods = {
	.CustomName = VEXEC_AGG_NAME,
	.BeginCustomScan = agg_begin,
	.ExecCustomScan = agg_exec,
	.EndCustomScan = agg_end,
	.ReScanCustomScan = agg_rescan,
	.ExplainCustomScan = agg_explain,
};

/* Its methods, for pg_otel_probe's reporter of vector nodes (node.c). */
const CustomExecMethods *
vexec_agg_exec_methods(void)
{
	return &agg_exec_methods;
}

Node *
vexec_create_agg_state(CustomScan *cscan)
{
	VexecAggState *s = palloc0(sizeof(VexecAggState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &agg_exec_methods;
	s->node.kind = VEXEC_NODE_AGG;
	return (Node *) s;
}

/* ---------------------------------------------------------------------
 * The plan's private list (plan/build.c writes it)
 * ---------------------------------------------------------------------
 */

/*
 * A plan travels as text, to parallel workers and to the segments; a Float
 * is written with its point, or it reads back as an Integer.
 */
List *
vexec_agg_plan_encode(const VexecAggPlan *plan)
{
	return list_make5(lappend(lappend(lappend(list_make5(makeInteger(plan->strategy), makeInteger(plan->split),
														 makeFloat(psprintf("%.1f", plan->numgroups)),
														 makeInteger((int) plan->topk),
														 makeInteger(plan->sorted_input ? 1 : 0)),
											  makeInteger(plan->device ? 1 : 0)),
									  plan->whole_cols),
							  makeFloat(psprintf("%.1f", plan->group_bytes))),
					  list_make3(plan->keycols, plan->eqops, plan->collations),
					  list_make2(plan->outkind, plan->outsrc),
					  plan->aggrefs,
					  makeInteger(plan->ntrans));
}

void
vexec_agg_plan_decode(CustomScan *cscan, VexecAggPlan *plan)
{
	List	   *priv = cscan->custom_private;
	List	   *head;
	List	   *keys;
	List	   *outs;

	if (list_length(priv) != 5)
		elog(ERROR, "vexec: a VecAgg plan of %d private items", list_length(priv));
	head = linitial(priv);
	keys = lsecond(priv);
	outs = lthird(priv);
	plan->strategy = intVal(linitial(head));
	plan->split = intVal(lsecond(head));
	plan->numgroups = floatVal(lthird(head));
	plan->topk = list_length(head) > 3 ? intVal(lfourth(head)) : 0;
	plan->sorted_input = list_length(head) > 4 && intVal(list_nth(head, 4)) != 0;
	plan->device = list_length(head) > 5 && intVal(list_nth(head, 5)) != 0;
	plan->whole_cols = list_length(head) > 6 ? (List *) list_nth(head, 6) : NIL;
	plan->group_bytes = list_length(head) > 7 ? floatVal(list_nth(head, 7)) : 0;
	plan->keycols = linitial(keys);
	plan->eqops = lsecond(keys);
	plan->collations = lthird(keys);
	plan->outkind = linitial(outs);
	plan->outsrc = lsecond(outs);
	plan->aggrefs = lfourth(priv);
	plan->ntrans = intVal(list_nth(priv, 4));
}

/* ---------------------------------------------------------------------
 * Groups and their keys
 * ---------------------------------------------------------------------
 */

static inline Datum *
group_keys(VexecAggState *s, char *g)
{
	return (Datum *) (g + s->keyoff);
}

static inline bool *
group_keynulls(VexecAggState *s, char *g)
{
	return (bool *) (g + s->keyoff + sizeof(Datum) * s->nkeys);
}

/*
 * F14 (§3.32): the form a group's numeric key is in -- 0 its numeric, its
 * type's Datum as any key's; scale + 1 the int64 of its value scaled at that
 * scale, as F10 takes a batch's (batch_keys()), its numeric made where the
 * group is projected (group_column()).  Q18 made a numeric for each of its
 * 9.6M groups, nearly every row its first stage passed over, 15% of its
 * samples, and projects ten.
 */
static inline uint8
group_keyform(VexecAggState *s, const char *g, int i)
{
	return s->keyformoff != 0 ? ((const uint8 *) (g + s->keyformoff))[i] : 0;
}

static inline void
set_group_keyform(VexecAggState *s, char *g, int i, uint8 form)
{
	if (s->keyformoff != 0)
		((uint8 *) (g + s->keyformoff))[i] = form;
}

/* F14: whether two scaled integers' values are equal, whatever their scales */
static inline bool
scaled_eq(int64 a, int sa, int64 b, int sb)
{
	static const int64 pow10[] = {
		INT64CONST(1), INT64CONST(10), INT64CONST(100), INT64CONST(1000), INT64CONST(10000),
		INT64CONST(100000), INT64CONST(1000000), INT64CONST(10000000), INT64CONST(100000000),
		INT64CONST(1000000000), INT64CONST(10000000000), INT64CONST(100000000000),
		INT64CONST(1000000000000), INT64CONST(10000000000000), INT64CONST(100000000000000),
		INT64CONST(1000000000000000), INT64CONST(10000000000000000), INT64CONST(100000000000000000),
		INT64CONST(1000000000000000000)
	};

	if (sa == sb)
		return a == b;
	/* the one of the smaller scale at the other's: within 128 bits, the scales 0 to 18 */
	if (sa < sb)
		return (int128) a * pow10[sb - sa] == (int128) b;
	return (int128) b * pow10[sa - sb] == (int128) a;
}

static inline Datum *
group_extras(VexecAggState *s, char *g)
{
	return (Datum *) (g + s->extraoff);
}

static inline bool *
group_extranulls(VexecAggState *s, char *g)
{
	return (bool *) (g + s->extraoff + sizeof(Datum) * s->nextra);
}

static inline char *
group_state(VexecAggState *s, char *g, VexecAggTrans *t)
{
	return g + s->stateoff + t->stateoff;
}

/* D2: whether a group is packed: its length beside its hash, 0 in the table's */
static inline bool
group_packed(const char *g)
{
	return *(const uint32 *) (g + sizeof(uint32)) != 0;
}

/* D2: a packed group's by-reference value, whose Datum holds its offset */
static inline Datum
packed_ref(const char *g, Datum d)
{
	return PointerGetDatum(g + DatumGetUInt32(d));
}

/*
 * D2: a by-reference value as a packed group holds it: a varlena flat and
 * uncompressed -- a toast pointer, compressed bytes or an expanded object
 * made plain, in the current context -- and the bytes it takes there
 */
static inline Datum
packed_value(Datum d, int16 typlen, Size *size)
{
	if (typlen == -1)
	{
		varlena    *vl = (varlena *) DatumGetPointer(d);

		if (VARATT_IS_EXTENDED(vl) && !VARATT_IS_SHORT(vl))
			vl = pg_detoast_datum_packed(vl);
		*size = VARSIZE_ANY(vl);
		return PointerGetDatum(vl);
	}
	*size = (Size) typlen;
	return d;
}

/* A text or bytea Datum's bytes, detoasted into the current context. */
static void
varlena_bytes(Datum d, const char **p, int32 *len)
{
	varlena    *vl = (varlena *) DatumGetPointer(d);

	if (VARATT_IS_EXTENDED(vl))
		vl = pg_detoast_datum_packed(vl);
	*p = VARDATA_ANY(vl);
	*len = VARSIZE_ANY_EXHDR(vl);
}

static inline Datum key_bits(Datum d, int16 typlen);
static inline uint32 key_hash(VexecAggKeyCol *k, VexecAggProbe *p);

/*
 * F10: a scaled probe's numeric, made in the batch's registers, as a new
 * group keeps it: the one vexec_vec_datum() would have made of the row.
 */
static inline void
probe_numeric(VexecAggState *s, VexecAggProbe *p)
{
	if (p->scaled)
	{
		p->value = vexec_scaled_to_numeric(s->node.work, (int64) p->value, p->scale);
		p->scaled = false;
	}
}

/* Whether a group's keys are the probe's: NULLs group together. */
static bool
group_equal(VexecAggState *s, const char *group)
{
	Datum	   *gk = group_keys(s, (char *) group);
	bool	   *gn = group_keynulls(s, (char *) group);
	bool		packed = group_packed(group);	/* D2: a batch table's */
	int			i;

	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecAggProbe *p = &s->probe[i];

		if (gn[i] != p->isnull)
			return false;
		if (p->isnull)
			continue;
		switch (k->kind)
		{
			case KEY_BITS:
				if (key_bits(gk[i], k->typlen) != p->value)
					return false;
				break;
			case KEY_BYTES:
				{
					varlena    *vl = (varlena *) DatumGetPointer(packed ? packed_ref(group, gk[i]) :
																gk[i]);

					if (VARSIZE(vl) - VARHDRSZ != (Size) p->len ||
						memcmp(VARDATA(vl), p->bytes, p->len) != 0)
						return false;
				}
				break;
			case KEY_FMGR:
				if (group_keyform(s, group, i) != 0)
				{
					/* F14: the group's key its scaled integer */
					int			gs = group_keyform(s, group, i) - 1;

					if (p->scaled ? !scaled_eq((int64) p->value, p->scale, DatumGetInt64(gk[i]), gs) :
						!vexec_scaled_numeric_eq(DatumGetInt64(gk[i]), gs, p->value))
						return false;
				}
				else if (p->scaled)
				{
					if (!vexec_scaled_numeric_eq((int64) p->value, p->scale,
												 packed ? packed_ref(group, gk[i]) : gk[i]))
						return false;
				}
				else if (!DatumGetBool(FunctionCall2Coll(&k->eq, k->collation,
														 packed && !k->typbyval ?
														 packed_ref(group, gk[i]) : gk[i],
														 p->value)))
					return false;
				break;
		}
	}
	return true;
}

/* Probes' hash: each key's, combined as nodeAgg.c combines them. */
static uint32
probes_hash(VexecAggKeyCol *keys, VexecAggProbe *probe, int nkeys)
{
	uint32		h = 0;
	int			i;

	for (i = 0; i < nkeys; i++)
	{
		h = pg_rotate_left32(h, 1);
		if (probe[i].isnull)
			continue;
		h ^= key_hash(&keys[i], &probe[i]);
	}
	return murmurhash32(h);
}

/* The probe's hash. */
static uint32
probe_hash(VexecAggState *s)
{
	return probes_hash(s->keys, s->probe, s->nkeys);
}

/*
 * A by-value key's bits: its type's bytes only, as a Datum built from a
 * tuple (fetch_att) and one built by a function (ObjectIdGetDatum) may
 * extend a narrower value's sign otherwise.
 */
static inline Datum
key_bits(Datum d, int16 typlen)
{
	switch (typlen)
	{
		case 1:
			return d & 0xFF;
		case 2:
			return d & 0xFFFF;
		case 4:
			return d & UINT64CONST(0xFFFFFFFF);
		default:
			return d;
	}
}

/* A key's hash, as probe_hash() takes it for one column. */
static inline uint32
key_hash(VexecAggKeyCol *k, VexecAggProbe *p)
{
	switch (k->kind)
	{
		case KEY_BITS:
			return (uint32) murmurhash64((uint64) p->value);
		case KEY_BYTES:
			return vexec_hash_bytes(p->bytes, p->len);	/* C4 */
		default:
			if (p->scaled)
				return vexec_scaled_hash_numeric((int64) p->value, p->scale);	/* F10 */
			return DatumGetUInt32(FunctionCall1Coll(&k->hash, k->collation, p->value));
	}
}

/* A key value into a probe, from its Datum. */
static void
probe_set(VexecAggKeyCol *k, VexecAggProbe *p, Datum d, bool isnull)
{
	p->isnull = isnull;
	p->scaled = false;
	p->value = isnull ? (Datum) 0 : k->kind == KEY_BITS ? key_bits(d, k->typlen) : d;
	if (!isnull && k->kind == KEY_BYTES)
		varlena_bytes(d, &p->bytes, &p->len);
}

/*
 * VC5: the hashes of a dictionary key column's values, as key_hash() gives
 * them, kept for the dictionary's identity; NULL where it has none and more
 * values than the rows they answer, whose hashes are then a row's each.
 */
static const uint32 *
dict_key_hashes(VexecAggState *s, int key, const VexecVec *v, int nrows)
{
	const VexecVec *d = v->dictionary;
	uint32	   *h;
	int			i;

	if (s->dict_hashes == NULL)
	{
		s->dict_hash_ids = MemoryContextAllocZero(s->node.mcxt, sizeof(uint64) * s->nkeys);
		s->dict_hashes = MemoryContextAllocZero(s->node.mcxt, sizeof(uint32 *) * s->nkeys);
	}
	if (v->dict_id != 0 && s->dict_hash_ids[key] == v->dict_id)
		return s->dict_hashes[key];
	if (v->dict_id == 0 && d->nvalues > nrows)
		return NULL;
	if (s->dict_hashes[key])
		pfree(s->dict_hashes[key]);
	h = MemoryContextAlloc(s->node.mcxt, sizeof(uint32) * Max(d->nvalues, 1));
	for (i = 0; i < d->nvalues; i++)
	{
		const char *bytes;
		Size		len;

		vexec_vec_value_bytes(d, i, &bytes, &len);
		h[i] = vexec_hash_bytes(bytes, (int) len);
	}
	s->dict_hashes[key] = h;
	s->dict_hash_ids[key] = v->dict_id;
	return h;
}

/*
 * The keys and hashes of a batch's rows, a key column at a time: integers
 * read from their array, Arrow's bytes where they are -- a dictionary's at
 * the rows' codes, their hashes its values' (VC5) -- any other value as
 * its Datum.  The rows are those of `rows`, in the batch.
 */
static void
batch_keys(VexecAggState *s, const int *rows, int nlist)
{
	VexecBatch *in = s->node.in;
	int			nk = s->nkeys;
	const uint32 *hashes = s->batch_hashes;
	int			i,
				j;

	for (j = 0; j < nlist; j++)
		s->rowhash[rows[j]] = 0;
	for (i = 0; i < nk; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecVec   *v = &in->cols[k->col];
		bool		ints = k->kind == KEY_BITS && v->encoding == VEXEC_FLAT &&
			v->shape.layout == VEXEC_FIXED && !v->shape.arrow_values &&
			(v->shape.width == 2 || v->shape.width == 4 || v->shape.width == 8) &&
			v->shape.stride == v->shape.width && v->shape.width == k->typlen;
		bool		views = k->kind == KEY_BYTES && v->encoding == VEXEC_FLAT &&
			(v->shape.layout == VEXEC_VIEW || v->shape.layout == VEXEC_OFFSETS);
		bool		dict = k->kind == KEY_BYTES && v->encoding == VEXEC_DICT &&
			(v->dictionary->shape.layout == VEXEC_VIEW ||
			 v->dictionary->shape.layout == VEXEC_OFFSETS);
		const uint32 *dhash = dict && hashes == NULL && !s->sorted ?
			dict_key_hashes(s, i, v, nlist) : NULL;

		/*
		 * F10 (§3.31): a numeric key of a batch's scaled int64s -- extract()'s
		 * fields, Q18's minute -- hashed and compared by them, as
		 * hash_numeric() and numeric_eq() would their numerics; a group's
		 * numeric made as it is
		 */
		bool		scaled = k->numeric && v->encoding == VEXEC_FLAT &&
			v->shape.layout == VEXEC_SCALED && v->shape.width == 8 &&
			v->shape.scale >= 0 && v->shape.scale <= 18;

		for (j = 0; j < nlist; j++)
		{
			int			row = rows[j];
			VexecAggProbe *p = &s->rowkeys[row * nk + i];
			uint32		h = pg_rotate_left32(s->rowhash[row], 1);

			if (ints)
			{
				p->isnull = v->validity != NULL && !vexec_bit(v->validity, row);
				if (!p->isnull)
				{
					if (v->shape.width == 4)
						p->value = (Datum) (uint32) ((const int32 *) v->values)[row];
					else if (v->shape.width == 8)
						p->value = (Datum) ((const int64 *) v->values)[row];
					else
						p->value = (Datum) (uint16) ((const int16 *) v->values)[row];
				}
			}
			else if (views)
			{
				p->isnull = vexec_vec_isnull(v, row);
				if (!p->isnull)
				{
					const char *bytes;
					Size		len;

					vexec_vec_value_bytes(v, row, &bytes, &len);
					p->bytes = bytes;
					p->len = (int32) len;
				}
			}
			else if (scaled)
			{
				p->isnull = vexec_vec_isnull(v, row);
				p->scaled = !p->isnull;
				p->scale = v->shape.scale;
				if (!p->isnull)
				{
					int64		v64;

					memcpy(&v64, (const char *) v->values + (Size) row * 8, sizeof(int64));
					p->value = (Datum) v64;
				}
			}
			else if (dict)
			{
				p->isnull = vexec_vec_isnull(v, row);
				if (!p->isnull)
				{
					const char *bytes;
					Size		len;

					vexec_vec_value_bytes(v->dictionary, v->codes[row], &bytes, &len);
					p->bytes = bytes;
					p->len = (int32) len;
					if (dhash != NULL)
					{
						s->rowhash[row] = h ^ dhash[v->codes[row]];
						continue;
					}
				}
			}
			else
			{
				bool		isnull;
				Datum		d = vexec_vec_datum(s->node.work, v, row, &isnull);

				probe_set(k, p, d, isnull);
			}
			/* VC4: in the keys' order, a row is compared, not hashed */
			if (!p->isnull && hashes == NULL && !s->sorted)
				h ^= key_hash(k, p);
			s->rowhash[row] = h;
		}
	}

	/* the child's hashes are this node's (C4): the keys are only compared */
	if (!s->sorted)
		for (j = 0; j < nlist; j++)
			s->rowhash[rows[j]] = hashes != NULL ? hashes[rows[j]] :
				murmurhash32(s->rowhash[rows[j]]);
}

static void
probe_from_slot(VexecAggState *s, TupleTableSlot *slot)
{
	int			i;

	s->probe = s->probe_slot;
	for (i = 0; i < s->nkeys; i++)
	{
		int			col = s->keys[i].col;

		probe_set(&s->keys[i], &s->probe[i], slot->tts_values[col], slot->tts_isnull[col]);
	}
}

static Size
table_memory(VexecAggState *s)
{
	Size		m = MemoryContextMemAllocated(s->aggcontext->ecxt_per_tuple_memory, true) +
		MemoryContextMemAllocated(s->groupcxt, true) +
		MemoryContextMemAllocated(s->hashcxt, true);

	s->peak_memory = Max(s->peak_memory, m);
	return m;
}

/*
 * A new group's extra i: the input row's, where PostgreSQL's evaluator has
 * it in slot, else the input batch's `row`; a packed group's that the
 * group is made for (D2); or none, for a plain aggregation's group made
 * before any row.
 */
static void
row_extra(VexecAggState *s, int i, TupleTableSlot *slot, int row, Datum *d, bool *isnull)
{
	int			col = s->extracol[i];

	if (slot != NULL)
	{
		*d = slot->tts_values[col];
		*isnull = slot->tts_isnull[col];
	}
	else if (row >= 0)
		*d = vexec_vec_datum(s->node.work, &s->node.in->cols[col], row, isnull);
	else if (s->pack_rec != NULL)
	{
		VexecAggState *p = s->producer;
		const char *rec = s->pack_rec;
		int			src = s->pk_extra[i];
		bool		byval;

		if (src >= 0)
		{
			*isnull = group_keynulls(p, (char *) rec)[src];
			*d = group_keys(p, (char *) rec)[src];
			byval = p->keys[src].typbyval;
			/* F14: a key the producer keeps scaled, its numeric made */
			if (!*isnull && group_keyform(p, rec, src) != 0)
			{
				*d = vexec_scaled_to_numeric(s->node.work, DatumGetInt64(*d),
											 group_keyform(p, rec, src) - 1);
				return;
			}
		}
		else
		{
			src = -1 - src;
			*isnull = group_extranulls(p, (char *) rec)[src];
			*d = group_extras(p, (char *) rec)[src];
			byval = TupleDescAttr(p->indesc, p->extracol[src])->attbyval;
		}
		if (!*isnull && !byval)
			*d = packed_ref(rec, *d);
	}
	else
	{
		*d = (Datum) 0;
		*isnull = true;
	}
}

/* A KEY_BITS key's Datum, as a tuple's value would be: fetch_att's. */
static inline Datum
key_datum(VexecAggKeyCol *k, Datum bits)
{
	switch (k->typlen)
	{
		case 1:
			return CharGetDatum((char) bits);
		case 2:
			return Int16GetDatum((int16) bits);
		case 4:
			return Int32GetDatum((int32) bits);
		default:
			return bits;
	}
}

/*
 * D2: a batch table's new group (C2), packed in the partition its hash
 * goes to: the probe's keys and the row's extras, their by-reference bytes
 * after the states, its states begun.  It lies there from now on: the
 * batch's rows advance its states in place, and the batch's end hands it
 * on, copying nothing.
 */
static char *
group_create_packed(VexecAggState *s, uint32 hash, TupleTableSlot *slot, int row)
{
	Size		len = s->groupsize;
	Size		off;
	Size		n;
	char	   *g;
	int			i;

	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecAggProbe *p = &s->probe[i];

		if (p->isnull || k->typbyval)
			continue;
		if (k->kind == KEY_BYTES)
			len += MAXALIGN(VARHDRSZ + p->len);
		else if (!p->scaled)	/* F14: a scaled key its integer, no bytes */
		{
			s->pk_keyvals[i] = packed_value(p->value, k->typlen, &n);
			len += MAXALIGN(n);
		}
	}
	for (i = 0; i < s->nextra; i++)
	{
		Form_pg_attribute att = TupleDescAttr(s->indesc, s->extracol[i]);

		row_extra(s, i, slot, row, &s->pk_extravals[i], &s->pk_extranulls[i]);
		if (!s->pk_extranulls[i] && !att->attbyval)
		{
			s->pk_extravals[i] = packed_value(s->pk_extravals[i], att->attlen, &n);
			len += MAXALIGN(n);
		}
	}
	g = vexec_pack_alloc(s->sink, s->deal(s->deal_arg, hash), len);
	memset(g, 0, s->groupsize);
	*(uint32 *) g = hash;
	*(uint32 *) (g + sizeof(uint32)) = (uint32) len;
	off = s->groupsize;
	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecAggProbe *p = &s->probe[i];

		group_keynulls(s, g)[i] = p->isnull;
		if (p->isnull)
			continue;
		if (k->kind == KEY_BYTES)
		{
			varlena    *vl = (varlena *) (g + off);

			SET_VARSIZE(vl, VARHDRSZ + p->len);
			memcpy(VARDATA(vl), p->bytes, p->len);
			group_keys(s, g)[i] = UInt32GetDatum((uint32) off);
			off += MAXALIGN(VARHDRSZ + p->len);
		}
		else if (k->kind == KEY_BITS)
			group_keys(s, g)[i] = key_datum(k, p->value);
		else if (k->typbyval)
			group_keys(s, g)[i] = p->value;
		else if (p->scaled)
		{
			/* F14: its scaled integer, its numeric made where the group is projected */
			group_keys(s, g)[i] = p->value;
			set_group_keyform(s, g, i, (uint8) (p->scale + 1));
		}
		else
		{
			n = k->typlen == -1 ? VARSIZE_ANY(DatumGetPointer(s->pk_keyvals[i])) : (Size) k->typlen;
			memcpy(g + off, DatumGetPointer(s->pk_keyvals[i]), n);
			group_keys(s, g)[i] = UInt32GetDatum((uint32) off);
			off += MAXALIGN(n);
		}
	}
	for (i = 0; i < s->nextra; i++)
	{
		Form_pg_attribute att = TupleDescAttr(s->indesc, s->extracol[i]);

		group_extranulls(s, g)[i] = s->pk_extranulls[i];
		if (s->pk_extranulls[i])
			continue;
		if (att->attbyval)
			group_extras(s, g)[i] = s->pk_extravals[i];
		else
		{
			n = att->attlen == -1 ? VARSIZE_ANY(DatumGetPointer(s->pk_extravals[i])) :
				(Size) att->attlen;
			memcpy(g + off, DatumGetPointer(s->pk_extravals[i]), n);
			group_extras(s, g)[i] = UInt32GetDatum((uint32) off);
			off += MAXALIGN(n);
		}
	}
	Assert(off == len);
	for (i = 0; i < s->ntrans; i++)
		vexec_aggtrans_init(&s->trans[i], s->aggstate, group_state(s, g, &s->trans[i]));
	s->passed_groups++;
	s->passed_since++;
	s->packed_groups++;
	addHyperLogLog(&s->passed_keys, murmurhash32(hash));
	return g;
}

/*
 * D2: a group of the table, packed into the partition its hash goes to:
 * its fixed part as it is, its by-reference values' bytes after it -- and
 * after them each text's minimum's or maximum's value (F5), its state's
 * Datum then the offset from the state.
 */
static void
pack_group(VexecAggState *s, char *g)
{
	Size		len = s->groupsize;
	Size		off;
	Size		n;
	char	   *r;
	int			i;

	for (i = 0; i < s->nkeys; i++)
		if (!s->keys[i].typbyval && !group_keynulls(s, g)[i] && group_keyform(s, g, i) == 0)
		{
			s->pk_keyvals[i] = packed_value(group_keys(s, g)[i], s->keys[i].typlen, &n);
			len += MAXALIGN(n);
		}
	for (i = 0; i < s->nextra; i++)
	{
		Form_pg_attribute att = TupleDescAttr(s->indesc, s->extracol[i]);

		if (!att->attbyval && !group_extranulls(s, g)[i])
		{
			s->pk_extravals[i] = packed_value(group_extras(s, g)[i], att->attlen, &n);
			len += MAXALIGN(n);
		}
	}
	for (i = 0; s->pack_values && i < s->ntrans; i++)
		if (vexec_aggtrans_packs_value(&s->trans[i]))
		{
			const varlena *vl = vexec_aggtrans_state_value(&s->trans[i], group_state(s, g, &s->trans[i]));

			if (vl != NULL)
				len += MAXALIGN(VARSIZE_ANY(vl));
		}
	r = vexec_pack_alloc(s->sink, s->deal(s->deal_arg, *(uint32 *) g), len);
	memcpy(r, g, s->groupsize);
	*(uint32 *) (r + sizeof(uint32)) = (uint32) len;
	off = s->groupsize;
	for (i = 0; i < s->nkeys; i++)
		if (!s->keys[i].typbyval && !group_keynulls(s, g)[i] && group_keyform(s, g, i) == 0)
		{
			n = s->keys[i].typlen == -1 ? VARSIZE_ANY(DatumGetPointer(s->pk_keyvals[i])) :
				(Size) s->keys[i].typlen;
			memcpy(r + off, DatumGetPointer(s->pk_keyvals[i]), n);
			group_keys(s, r)[i] = UInt32GetDatum((uint32) off);
			off += MAXALIGN(n);
		}
	for (i = 0; i < s->nextra; i++)
	{
		Form_pg_attribute att = TupleDescAttr(s->indesc, s->extracol[i]);

		if (!att->attbyval && !group_extranulls(s, g)[i])
		{
			n = att->attlen == -1 ? VARSIZE_ANY(DatumGetPointer(s->pk_extravals[i])) :
				(Size) att->attlen;
			memcpy(r + off, DatumGetPointer(s->pk_extravals[i]), n);
			group_extras(s, r)[i] = UInt32GetDatum((uint32) off);
			off += MAXALIGN(n);
		}
	}
	for (i = 0; s->pack_values && i < s->ntrans; i++)
		if (vexec_aggtrans_packs_value(&s->trans[i]))
		{
			VexecAggTrans *t = &s->trans[i];
			const varlena *vl = vexec_aggtrans_state_value(t, group_state(s, g, t));
			char	   *st = group_state(s, r, t);

			if (vl == NULL)
				continue;
			n = VARSIZE_ANY(vl);
			memcpy(r + off, vl, n);
			vexec_aggtrans_state_value_at(t, st, (uint32) (r + off - st));
			off += MAXALIGN(n);
		}
	Assert(off == len);
	s->packed_groups++;
}

/* F5: a frozen table's batch table's groups, each packed by a copy as the batch ends */
static void
pack_batch_table(VexecAggState *s)
{
	MemoryContext old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);
	vagg_iterator it;
	VexecAggEntry *e;

	vagg_start_iterate(s->btable, &it);
	while ((e = vagg_iterate(s->btable, &it)) != NULL)
		pack_group(s, e->group);
	MemoryContextSwitchTo(old);
	ResetExprContext(s->tmpcontext);
}

/*
 * A new group: the probe's keys and the row's extras copied, its states
 * begun.  slot is the input row where PostgreSQL's evaluator has it, else
 * the row is the input batch's `row`.
 */
static void group_note_made(VexecAggState *s, char *g, uint32 hash);

static char *
group_create_in(VexecAggState *s, uint32 hash, TupleTableSlot *slot, int row, bool batch)
{
	MemoryContext old;
	char	   *g;
	int			i;

	/*
	 * D2: a batch table's group made where it is handed on -- but where a
	 * state holds a text, whose value changes in place: packed by a copy as
	 * the batch ends (F5)
	 */
	if (batch && s->sink != NULL && !s->pack_values)
		return group_create_packed(s, hash, slot, row);

	/*
	 * F29 (§3.38): a final stage's packed group, its keys its bits or bytes
	 * and its states by value as this node lays them out, the group itself
	 * where it lies -- in the exchange's chunk, kept until the groups are
	 * out -- its states its own, nothing made, copied or merged
	 */
	if (!batch && s->rec_groups_live && s->pack_rec != NULL)
	{
		g = (char *) s->pack_rec;
		s->pack_taken = true;
		s->rec_groups_made++;
		group_note_made(s, g, hash);
		return g;
	}
	old = MemoryContextSwitchTo(batch ? s->bgroupcxt :
								s->sorted ? s->sgroupcxt[s->sgen] : s->groupcxt);
	g = palloc0(s->groupsize);

	*(uint32 *) g = hash;
	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecAggProbe *p = &s->probe[i];

		group_keynulls(s, g)[i] = p->isnull;
		if (p->isnull)
			continue;
		if (k->kind == KEY_BYTES)
		{
			varlena    *vl = palloc(VARHDRSZ + p->len);

			SET_VARSIZE(vl, VARHDRSZ + p->len);
			memcpy(VARDATA(vl), p->bytes, p->len);
			group_keys(s, g)[i] = PointerGetDatum(vl);
		}
		else if (k->kind == KEY_BITS)
			group_keys(s, g)[i] = key_datum(k, p->value);
		else if (p->scaled)
		{
			/* F14: its scaled integer, its numeric made where the group is projected */
			group_keys(s, g)[i] = p->value;
			set_group_keyform(s, g, i, (uint8) (p->scale + 1));
		}
		else
			group_keys(s, g)[i] = datumCopy(p->value, k->typbyval, k->typlen);
	}
	for (i = 0; i < s->nextra; i++)
	{
		Form_pg_attribute att = TupleDescAttr(s->indesc, s->extracol[i]);
		bool		isnull;
		Datum		d;

		row_extra(s, i, slot, row, &d, &isnull);
		group_extranulls(s, g)[i] = isnull;
		group_extras(s, g)[i] = isnull ? (Datum) 0 : datumCopy(d, att->attbyval, att->attlen);
	}
	MemoryContextSwitchTo(old);
	if (batch)
	{
		/* its states in the batch table's memory, freed as it ends */
		ExprContext *saved = s->aggstate->curaggcontext;

		s->aggstate->curaggcontext = s->baggcontext;
		for (i = 0; i < s->ntrans; i++)
			vexec_aggtrans_init(&s->trans[i], s->aggstate, group_state(s, g, &s->trans[i]));
		s->aggstate->curaggcontext = saved;
		s->passed_groups++;
		s->passed_since++;
		addHyperLogLog(&s->passed_keys, murmurhash32(hash));
		return g;
	}
	for (i = 0; i < s->ntrans; i++)
		vexec_aggtrans_init(&s->trans[i], s->aggstate, group_state(s, g, &s->trans[i]));
	group_note_made(s, g, hash);
	return g;
}

/* A table's group made: counted, ranked, and kept among the groups made, in order */
static void
group_note_made(VexecAggState *s, char *g, uint32 hash)
{
	s->ngroups++;
	if (s->topk > 0)
		topk_note(s, hash);
	if (s->strategy == AGG_HASHED && s->sorted)
		s->total_groups++;		/* VC4: in `made` once it closes */
	else if (s->strategy == AGG_HASHED)
	{
		s->total_groups++;
		if (s->nmade == s->made_size)
		{
			s->made_size = Max(s->made_size * 2, 1024);
			s->made = s->made == NULL ?
				MemoryContextAllocHuge(s->hashcxt, sizeof(char *) * s->made_size) :
				repalloc_huge(s->made, sizeof(char *) * s->made_size);
		}
		s->made[s->nmade++] = g;
	}
}

static char *
group_create(VexecAggState *s, uint32 hash, TupleTableSlot *slot, int row)
{
	return group_create_in(s, hash, slot, row, false);
}

/*
 * C2: the table frozen; the batch table, its memory made the first time a
 * table freezes.
 */
static void
freeze(VexecAggState *s)
{
	s->frozen = true;
	if (!s->thawed)
		s->frozen_groups = Max(s->frozen_groups, s->ngroups);
	if (s->btable == NULL)
	{
		EState	   *estate = s->node.css.ss.ps.state;

		s->bgroupcxt = BumpContextCreate(s->node.mcxt, "VecAgg batch groups",
										 ALLOCSET_DEFAULT_SIZES);
		s->bhashcxt = AllocSetContextCreate(s->node.mcxt, "VecAgg batch table",
											ALLOCSET_DEFAULT_SIZES);
		s->baggcontext = CreateWorkExprContext(estate);
		s->btable = vagg_create(s->bhashcxt, 2 * vexec_batch_rows, s);
	}
	/* the keys handed on from now, counted for maybe_thaw() */
	if (!s->passed_keys_made)
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		initHyperLogLog(&s->passed_keys, 10);
		MemoryContextSwitchTo(old);
		s->passed_keys_made = true;
	}
	else
		memset(s->passed_keys.hashesArr, 0, s->passed_keys.arrSize);
	s->passed_since = 0;
	s->thaw_next = VEXEC_AGG_THAW_EVIDENCE;
}

/*
 * C2: the table thawed, where the groups handed on since it froze -- past
 * VEXEC_AGG_THAW_EVIDENCE of them -- repeat their keys
 * VEXEC_AGG_THAW_REPEAT times or more; else looked at again as many groups
 * on.
 */
static void
maybe_thaw(VexecAggState *s)
{
	s->thaw_next = s->passed_since + VEXEC_AGG_THAW_EVIDENCE;
	if (s->full)
		return;					/* F16: frozen for good */
	if ((double) s->passed_since <
		VEXEC_AGG_THAW_REPEAT * estimateHyperLogLog(&s->passed_keys))
		return;
	s->frozen = false;
	s->thawed = true;
	s->thawed_after = Max(s->thawed_after, s->passed_since);
}

/* F16: the table frozen, or frozen again where it thawed, to thaw no more */
static void
freeze_for_good(VexecAggState *s)
{
	if (s->thawed)
		s->refrozen_groups = Max(s->refrozen_groups, s->ngroups);
	freeze(s);
	s->full = true;
}

/* The batch table emptied, once its groups are handed on. */
static void
batch_table_reset(VexecAggState *s)
{
	if (s->btable == NULL)
		return;
	ReScanExprContext(s->baggcontext);
	MemoryContextReset(s->bgroupcxt);
	vagg_reset(s->btable);
	s->boutput = false;
	s->biterating = false;
}

/*
 * C2: a partial aggregation under a VecRepartition may freeze its table:
 * the stage above it combines whatever states it is dealt.
 */
void
vexec_agg_allow_freeze(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;

	if (node->kind == VEXEC_NODE_AGG && s->strategy == AGG_HASHED &&
		DO_AGGSPLIT_SKIPFINAL(s->split) && !DO_AGGSPLIT_COMBINE(s->split) &&
		vexec_partial_agg_limit > 0 && s->topk == 0 && s->dev == NULL)
	{
		s->may_freeze = true;
		/* the table, still empty, made again no larger than it may grow */
		if (s->table != NULL && s->ngroups == 0)
		{
			s->buckets_initial = 0;
			table_reset(s);
		}
	}
}

/*
 * An empty table, for the input and for each partition read back; a hashed
 * one made at its first group (W23, table_make()).
 */
static void
table_reset(VexecAggState *s)
{
	ReScanExprContext(s->aggcontext);	/* AggRegisterCallback()'s run */
	MemoryContextReset(s->groupcxt);
	MemoryContextReset(s->hashcxt);
	s->table = NULL;
	s->plaingroup = NULL;
	s->made = NULL;
	s->nmade = 0;
	s->made_size = 0;
	s->next_made = 0;
	s->array_decided = false;
	s->array_mode = false;
	s->slots = NULL;
	s->tfinal_set = false;
	s->spilling = false;
	s->ngroups = 0;
	s->frozen = false;
	s->thawed = false;
	s->full = false;
	s->bypass = false;
	s->by_rows = 0;
	s->by_groups = 0;
	batch_table_reset(s);
	s->table_lazy = s->strategy == AGG_HASHED;
	if (s->strategy != AGG_HASHED)
		s->plaingroup = group_create(s, 0, NULL, -1);
}

/*
 * W23: the hashed table, made as its first group is looked for -- a row's,
 * a device's answer's or an array's groups' -- not as the node begins: a
 * node whose device hands its answer up from its pages, and a stage the
 * device takes whole with the stage above it, which never runs, make none,
 * where a table of the planner's estimate, up to two million buckets zeroed
 * as each began, cost ClickBench's device groupings 2 to 8 ms of page
 * faults (pg_vector_executor.md §3.28).  An empty table made for a device's
 * answer is sized by its groups (table_hint).
 */
static void
table_make(VexecAggState *s)
{
	double		fit = (double) s->memlimit / (double) (s->groupsize + sizeof(VexecAggEntry));
	double		size = Min(s->numgroups, 1048576);

	Assert(s->strategy == AGG_HASHED && s->table == NULL);

	/*
	 * The planner's estimate, up to a million groups; the most a table of
	 * the node held before (C6), as far as the memory holds them, filling
	 * no more than 80% of the buckets: fuller, simplehash grows a large
	 * table early, where a key lands far from its bucket (SH_GROW_MAX_DIB).
	 * A partition read back holds a part of the groups, no more than its
	 * rows; a device's answer, its groups.
	 */
	if (s->reading != NULL)
		size = Min(size, (double) s->reading->rows);
	else
		size = Max(size, (double) vexec_aggsize_get(s->size_key) * 0.9 / 0.8);
	if (s->table_hint > 0)
		size = Min(size, (double) s->table_hint * 0.9 / 0.8);
	size = Min(size, fit);
	/* a table that may freeze never holds more than it freezes at (C2) */
	if (s->may_freeze)
		size = Min(size, vexec_partial_agg_limit);

	/*
	 * F38 (§3.38): under C5's hash bound a table holds a few times the groups
	 * the LIMIT keeps, whatever the planner's estimate of all of them: it
	 * grows if it must.  ClickBench's Q17 at 100M rows made 2M buckets, 33 MB
	 * of faults a participant, for 481 groups.
	 */
	if (s->topk_skip && vexec_enable_topk_table_bound)
		size = Min(size, 64.0 * (double) s->topk);

	s->table = vagg_create(s->hashcxt, (uint32) Min(Max(size, 64), 1 << 30), s);
	s->table_lazy = false;
	if (s->buckets_initial == 0)
		s->buckets_initial = s->table->size;
}

/* ---------------------------------------------------------------------
 * Spill
 * ---------------------------------------------------------------------
 */

/*
 * The partitions, their files and the queue outlive the row context a
 * batch is aggregated in: they are the node's.
 */
static void
spill_begin(VexecAggState *s)
{
	int			i;

	s->spilling = true;
	s->parts = MemoryContextAllocZero(s->node.mcxt,
									  sizeof(VexecAggPartition) * VEXEC_AGG_PARTITIONS);
	for (i = 0; i < VEXEC_AGG_PARTITIONS; i++)
		s->parts[i].depth = s->depth + 1;
	s->max_depth = Max(s->max_depth, s->depth + 1);
}

/* The input row to the partition the hash's next bits choose. */
static void
spill_row(VexecAggState *s, uint32 hash, TupleTableSlot *slot, int row)
{
	int			shift = 32 - VEXEC_AGG_PARTITION_BITS * (s->depth + 1);
	VexecAggPartition *part = &s->parts[(hash >> shift) & (VEXEC_AGG_PARTITIONS - 1)];
	TupleTableSlot *out = s->spillslot;
	MinimalTuple tup;
	bool		shouldFree;
	int			i;

	ExecClearTuple(out);
	for (i = 0; i < s->indesc->natts; i++)
	{
		if (!s->spillneeded[i])
		{
			out->tts_values[i] = (Datum) 0;
			out->tts_isnull[i] = true;
		}
		else if (slot != NULL)
		{
			out->tts_values[i] = slot->tts_values[i];
			out->tts_isnull[i] = slot->tts_isnull[i];
		}
		else
			out->tts_values[i] = vexec_vec_datum(s->node.work, &s->node.in->cols[i], row,
												 &out->tts_isnull[i]);
	}
	ExecStoreVirtualTuple(out);
	tup = ExecFetchSlotMinimalTuple(out, &shouldFree);
	if (part->file == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		part->file = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
	}
	BufFileWrite(part->file, tup, tup->t_len);
	part->rows++;
	s->spilled_rows++;
	if (shouldFree)
		pfree(tup);
}

/* The level's partitions, queued to be read. */
static void
spill_finish(VexecAggState *s)
{
	MemoryContext old;
	int			i;

	if (s->parts == NULL)
		return;
	old = MemoryContextSwitchTo(s->node.mcxt);
	for (i = 0; i < VEXEC_AGG_PARTITIONS; i++)
	{
		VexecAggPartition *part = &s->parts[i];
		VexecAggPartition *queued;

		if (part->file == NULL && part->pfile == NULL)
			continue;
		if ((part->file != NULL && BufFileSeek(part->file, 0, 0, SEEK_SET) != 0) ||
			(part->pfile != NULL && BufFileSeek(part->pfile, 0, 0, SEEK_SET) != 0))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind a VecAgg spill file")));
		queued = palloc_object(VexecAggPartition);
		*queued = *part;
		s->pending = lappend(s->pending, queued);
		s->spilled_partitions++;
	}
	pfree(s->parts);
	s->parts = NULL;
	MemoryContextSwitchTo(old);
}

static void
spill_close_all(VexecAggState *s)
{
	ListCell   *lc;

	foreach(lc, s->pending)
	{
		VexecAggPartition *p = lfirst(lc);

		if (p->file)
			BufFileClose(p->file);
		if (p->pfile)
			BufFileClose(p->pfile);
	}
	s->pending = NIL;
	if (s->parts != NULL)
	{
		for (int i = 0; i < VEXEC_AGG_PARTITIONS; i++)
		{
			if (s->parts[i].file)
				BufFileClose(s->parts[i].file);
			if (s->parts[i].pfile)
				BufFileClose(s->parts[i].pfile);
		}
		pfree(s->parts);
		s->parts = NULL;
	}
	if (s->reading != NULL && s->reading->file != NULL)
		BufFileClose(s->reading->file);
	if (s->reading != NULL && s->reading->pfile != NULL)
		BufFileClose(s->reading->pfile);
	s->reading = NULL;
}

/* ---------------------------------------------------------------------
 * Input
 * ---------------------------------------------------------------------
 */

/* Rows into the row batch, from the child or from a partition. */
static bool
fill_rows(VexecAggState *s)
{
	VexecBatch *in = s->rowbatch;
	TupleDesc	desc = s->indesc;

	vexec_batch_reset(in);
	vexec_batch_begin_rows(in);
	while (in->nrows < in->room)
	{
		TupleTableSlot *slot;

		if (s->reading != NULL)
		{
			BufFile    *f = s->reading->file;
			uint32		len;
			MinimalTuple tup;

			if (f == NULL || BufFileReadMaybeEOF(f, &len, sizeof(uint32), true) == 0)
				break;
			tup = (MinimalTuple) MemoryContextAlloc(in->mcxt, len);
			tup->t_len = len;
			BufFileReadExact(f, (char *) tup + sizeof(uint32), len - sizeof(uint32));
			slot = ExecStoreMinimalTuple(tup, s->readslot, false);
		}
		else
		{
			if (s->child_done)
				break;
			slot = ExecProcNode(s->child);
			if (TupIsNull(slot))
			{
				s->child_done = true;
				break;
			}
		}
		slot_getsomeattrs(slot, desc->natts);
		vexec_batch_add_values(in, slot->tts_values, slot->tts_isnull);
	}
	if (in->nrows == 0)
		return false;
	vexec_batch_apply_config(in, &s->node.layout);
	s->node.in = in;
	return true;
}

/*
 * H2: the units of rows the child's source answers from its statistics,
 * before its next batch, into the plain group's states.
 */
static void
stats_take(VexecAggState *s)
{
	int64		nrows;

	while (vexec_scan_aggregate(s->node.vec_child, s->nstats, s->stats_reqs,
								s->stats_answers, &nrows))
	{
		for (int i = 0; i < s->ntrans; i++)
		{
			VexecAggTrans *t = &s->trans[i];

			vexec_aggtrans_stats_apply(t, s->aggstate, group_state(s, s->plaingroup, t),
									   &s->stats_answers[s->stats_first[i]], nrows);
		}
	}
}

/*
 * F37 (§3.38): the scan below told which of its output columns this node reads
 * only by length(), before its first batch: where no row of its input comes to
 * PostgreSQL's evaluator or a partition's file, which read a value's bytes -- no
 * lazy input or extra, an input that may raise only length(), and a table that
 * cannot spill, a plain aggregation's or a first stage's that freezes instead
 * (F16) -- and no key is the column.
 */
static void
agg_tell_lengths(VexecAggState *s)
{
	VexecNode  *child = s->node.vec_child;
	VexecLengthUses w;
	bool	   *outputs;
	bool		any = false;
	ListCell   *lc;

	s->lengths_told = true;
	if (!vexec_enable_length_columns || child == NULL || child->kind != VEXEC_NODE_SCAN || s->any_lazy ||
		s->node.needs_input_datums || s->nextra > 0 || s->sorted || s->dev != NULL ||
		(s->strategy != AGG_PLAIN && !s->may_freeze))
		return;
	foreach(lc, s->input_exprs)
	{
		Expr	   *e = lfirst_node(TargetEntry, lc)->expr;

		if (!vexec_is_length_call((Node *) e) && vexec_expr_may_raise((Node *) e))
			return;
	}
	w.varno = OUTER_VAR;
	w.n = list_length(child->css.ss.ps.plan->targetlist);
	w.uses = palloc0(Max(w.n, 1));
	(void) vexec_length_uses((Node *) s->input_exprs, &w);
	for (int i = 0; i < s->nkeys; i++)
		if (s->keys[i].col < w.n)
			w.uses[s->keys[i].col] = 2;
	outputs = MemoryContextAllocZero(s->node.mcxt, sizeof(bool) * Max(w.n, 1));
	for (int i = 0; i < w.n; i++)
	{
		outputs[i] = w.uses[i] == 1;
		any |= outputs[i];
	}
	if (any)
		vexec_scan_parent_lengths(child, outputs, w.n);
	pfree(w.uses);
}

/* The next input batch: the child's, its rows, or a partition's. */
static bool
agg_fetch(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;

	if (s->reading == NULL && s->child_batches)
	{
		VexecNode  *child = node->vec_child;
		VexecBatch *b;

		if (s->nstats >= 0)
			stats_take(s);
		if (!s->lengths_told)
			agg_tell_lengths(s);
		b = vexec_next_batch(s->child);

		if (b == NULL)
			return false;
		node->in = b;
		s->batch_hashes = s->hashes_from_child ? vexec_repart_batch_hashes(child) : NULL;
		if (vexec_bits_any(child->redo, b->nrows) ||
			(child->child_redo != NULL && vexec_bits_any(child->child_redo, b->nrows)))
		{
			node->child_redo = vexec_bits_copy(node->work, child->redo, b->nrows);
			if (child->child_redo != NULL)
				vexec_bits_or_into(node->child_redo, child->child_redo, b->nrows);
		}
		return true;
	}
	s->batch_hashes = NULL;
	return fill_rows(s);
}

/* ---------------------------------------------------------------------
 * C5: the groups of the K smallest hashes (above)
 * ---------------------------------------------------------------------
 */

/*
 * The boundary now: a row or a group whose hash is past it is no one's of
 * the K groups of the smallest hashes -- K are held below it, by this
 * participant or by another, or, a final stage's, by the first stage below.
 */
static uint32
topk_bound(VexecAggState *s)
{
	uint32		b = s->tbound;

	if (s->tshared != NULL)
		b = Min(b, pg_atomic_read_u32(s->tshared));
	if (DO_AGGSPLIT_COMBINE(s->split) && s->child_batches &&
		s->node.vec_child->kind == VEXEC_NODE_REPART)
		b = Min(b, vexec_repart_topk_bound(s->node.vec_child));
	return b;
}

/* A new group's hash, into the heap of the K smallest; the boundary falls. */
static void
topk_note(VexecAggState *s, uint32 hash)
{
	int			i;

	if (s->ntheap < s->topk)
	{
		/* sift up */
		i = s->ntheap++;
		while (i > 0 && s->theap[(i - 1) / 2] < hash)
		{
			s->theap[i] = s->theap[(i - 1) / 2];
			i = (i - 1) / 2;
		}
		s->theap[i] = hash;
	}
	else if (hash < s->theap[0])
	{
		/* the largest out: sift down */
		i = 0;
		for (;;)
		{
			int			c = 2 * i + 1;

			if (c >= s->ntheap)
				break;
			if (c + 1 < s->ntheap && s->theap[c + 1] > s->theap[c])
				c++;
			if (s->theap[c] <= hash)
				break;
			s->theap[i] = s->theap[c];
			i = c;
		}
		s->theap[i] = hash;
	}
	else
		return;
	if (s->ntheap == s->topk && s->theap[0] < s->tbound)
	{
		s->tbound = s->theap[0];
		/* the participants' boundary: the least of theirs */
		if (s->tshared != NULL)
		{
			uint32		cur = pg_atomic_read_u32(s->tshared);

			while (s->tbound < cur &&
				   !pg_atomic_compare_exchange_u32(s->tshared, &cur, s->tbound))
				;
		}
	}
}

/*
 * C5: the participants' shared boundary, in the DSM of the VecRepartition
 * above this first stage, which it lends once the DSM is made.
 */
void
vexec_agg_set_topk_shared(VexecNode *node, pg_atomic_uint32 *shared)
{
	VexecAggState *s = (VexecAggState *) node;

	if (node->kind == VEXEC_NODE_AGG && s->topk > 0)
		s->tshared = shared;
}

/* C5: this stage's boundary, for the VecRepartition above it to pass on. */
uint32
vexec_agg_topk_bound(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;

	if (node->kind != VEXEC_NODE_AGG || s->topk == 0)
		return PG_UINT32_MAX;
	return topk_bound(s);
}

/* ---------------------------------------------------------------------
 * X5: the array mode (above)
 * ---------------------------------------------------------------------
 */

static void group_made(VexecAggState *s);

/* A key's bits as the signed integer of its width. */
static inline int64
key_int(Datum bits, int16 typlen)
{
	switch (typlen)
	{
		case 1:
			return (int8) (uint8) bits;
		case 2:
			return (int16) (uint16) bits;
		case 4:
			return (int32) (uint32) bits;
		default:
			return (int64) bits;
	}
}

/* The probe's slot in the array; false where a key is out of its range. */
static inline bool
array_slot(VexecAggState *s, const VexecAggProbe *p, int64 *slot)
{
	int64		r = 0;

	for (int k = 0; k < s->nkeys; k++)
	{
		int64		i = 0;

		if (!p[k].isnull)
		{
			int64		v = key_int(p[k].value, s->keys[k].typlen);

			if (v < s->alo[k] || v > s->ahi[k])
				return false;
			i = v - s->alo[k] + 1;
		}
		r = r * s->awidth[k] + i;
	}
	*slot = r;
	return true;
}

/*
 * Whether the empty table becomes an array, by the keys of its first batch,
 * as Velox's VectorHasher chooses (velox/exec/HashTable.cpp, decideHashMode):
 * each key's range, half its width again on either side, and a slot for
 * NULL, multiplied, no more than VEXEC_AGG_ARRAY_SLOTS -- else the range
 * alone.  The array is in the table's memory.
 */
static void
array_decide(VexecAggState *s, int nlist)
{
	int128		total = 1;
	int			k;

	s->array_decided = true;
	if (nlist == 0)
	{
		s->array_decided = false;	/* the next batch decides */
		return;
	}
	for (int pass = 0; pass < 2; pass++)
	{
		total = 1;
		for (k = 0; k < s->nkeys; k++)
		{
			int64		lo = PG_INT64_MAX;
			int64		hi = PG_INT64_MIN;
			int128		w;

			for (int j = 0; j < nlist; j++)
			{
				const VexecAggProbe *p = &s->rowkeys[s->rows[j] * s->nkeys + k];
				int64		v;

				if (p->isnull)
					continue;
				v = key_int(p->value, s->keys[k].typlen);
				lo = Min(lo, v);
				hi = Max(hi, v);
			}
			if (lo > hi)
				lo = hi = 0;	/* every value NULL */
			if (pass == 0)
			{
				/* half the width again on either side, within the type */
				int128		half = ((int128) hi - lo + 1) / 2;
				int128		min = s->keys[k].typlen == 1 ? PG_INT8_MIN :
					s->keys[k].typlen == 2 ? PG_INT16_MIN :
					s->keys[k].typlen == 4 ? PG_INT32_MIN : PG_INT64_MIN;
				int128		max = s->keys[k].typlen == 1 ? PG_INT8_MAX :
					s->keys[k].typlen == 2 ? PG_INT16_MAX :
					s->keys[k].typlen == 4 ? PG_INT32_MAX : PG_INT64_MAX;

				lo = (int64) Max((int128) lo - half, min);
				hi = (int64) Min((int128) hi + half, max);
			}
			w = (int128) hi - lo + 2;
			s->alo[k] = lo;
			s->ahi[k] = hi;
			s->awidth[k] = (int64) Min(w, (int128) PG_INT64_MAX);
			total *= w;
			if (total > VEXEC_AGG_ARRAY_SLOTS)
				break;
		}
		if (total <= VEXEC_AGG_ARRAY_SLOTS)
			break;
	}
	if (total > VEXEC_AGG_ARRAY_SLOTS)
		return;
	s->nslots = (int64) total;
	s->slots = MemoryContextAllocExtended(s->hashcxt, sizeof(char *) * s->nslots,
										  MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	s->array_mode = true;
	s->array_tables++;
	s->array_slots = Max(s->array_slots, s->nslots);
}

/*
 * The array given up for the hash table: a key out of its range, or the
 * table past its memory, about to spill.  Its groups go into the hash
 * table by the hashes they were made with, in the order they were made.
 */
static void
array_leave(VexecAggState *s)
{
	VexecAggProbe *row_probe = s->probe;
	uint32		row_hash = *(uint32 *) s->probe_group;

	if (s->table == NULL)
		table_make(s);
	/* each group's keys its probe, which the table compares an entry with */
	s->probe = s->probe_slot;
	for (int64 i = 0; i < s->nmade; i++)
	{
		char	   *g = s->made[i];
		uint32		hash = *(uint32 *) g;
		bool		found;
		VexecAggEntry *e;

		for (int k = 0; k < s->nkeys; k++)
		{
			s->probe[k].isnull = group_keynulls(s, g)[k];
			s->probe[k].value = s->probe[k].isnull ? (Datum) 0 :
				key_bits(group_keys(s, g)[k], s->keys[k].typlen);
			/* F14: a key the group keeps scaled */
			s->probe[k].scaled = !s->probe[k].isnull && group_keyform(s, g, k) != 0;
			s->probe[k].scale = s->probe[k].scaled ? group_keyform(s, g, k) - 1 : 0;
		}
		*(uint32 *) s->probe_group = hash;
		e = vagg_insert_hash(s->table, s->probe_group, hash, &found);
		Assert(!found);
		e->group = g;
	}
	s->probe = row_probe;
	/* F14: the slot's probes not scaled, as PostgreSQL's rows and the device's set them */
	for (int k = 0; k < s->nkeys; k++)
		s->probe_slot[k].scaled = false;
	*(uint32 *) s->probe_group = row_hash;
	pfree(s->slots);
	s->slots = NULL;
	s->array_mode = false;
	s->array_left++;
}

/* ---------------------------------------------------------------------
 * VC4: groups in their keys' order (above)
 * ---------------------------------------------------------------------
 */

/* A closed group, to hand up */
static void
sorted_close(VexecAggState *s, char *g)
{
	if (s->nmade == s->made_size)
	{
		s->made_size = Max(s->made_size * 2, vexec_batch_rows + 1);
		s->made = s->made == NULL ?
			MemoryContextAllocHuge(s->node.mcxt, sizeof(char *) * s->made_size) :
			repalloc_huge(s->made, sizeof(char *) * s->made_size);
	}
	s->made[s->nmade++] = g;
}

/*
 * A row's group in the keys' order: the open one where the row's keys are
 * its, else a new one, the open one closed.  A row of the open group made
 * in the older generation is marked in bmask: its states are in that
 * generation's memory (agg_batch()).
 */
static char *
sorted_row_group(VexecAggState *s, TupleTableSlot *slot, int row)
{
	if (s->cur != NULL && group_equal(s, s->cur))
	{
		if (s->cur_gen != s->sgen && row >= 0)
			vexec_bit_set(s->bmask, row);
		return s->cur;
	}
	if (s->cur != NULL)
		sorted_close(s, s->cur);
	/* its hash once, for a stage above that reads it (C4) */
	s->cur = group_create(s, probe_hash(s), slot, row);
	s->cur_gen = s->sgen;
	return s->cur;
}

/*
 * The groups closed so far are out: every generation but the open group's
 * emptied, and new groups made in a freshly emptied one.
 */
static void
sorted_round_end(VexecAggState *s)
{
	int			keep = s->cur != NULL ? s->cur_gen : -1;
	Size		mem = 0;

	for (int g = 0; g < 2; g++)
		mem += MemoryContextMemAllocated(s->sgroupcxt[g], true) +
			MemoryContextMemAllocated(s->saggcontext[g]->ecxt_per_tuple_memory, true);
	s->peak_memory = Max(s->peak_memory, mem);
	for (int g = 0; g < 2; g++)
		if (g != keep)
		{
			ReScanExprContext(s->saggcontext[g]);	/* AggRegisterCallback()'s run */
			MemoryContextReset(s->sgroupcxt[g]);
		}
	s->sgen = keep >= 0 ? 1 - keep : 0;
	s->nmade = 0;
	s->next_made = 0;
}

/* Both generations emptied, no group open: the node's first, or a rescan's */
static void
sorted_reset(VexecAggState *s)
{
	s->cur = NULL;
	(void) sorted_round_end(s);
	s->sorted_eof = false;
	s->ngroups = 0;
}

/*
 * Whether the input comes in the keys' order: the child a serial VecScan
 * whose source says, as it begins, that its rows come in an order the
 * keys -- Vars of its relation, compared by their types' default equality,
 * a deterministic collation's for text -- are a prefix of, in any order.
 */
static bool
sorted_input_holds(VexecAggState *s)
{
	VexecNode  *child = s->node.vec_child;
	Plan	   *cplan = s->child->plan;
	AttrNumber	order[INDEX_MAX_KEYS];
	bool		used[INDEX_MAX_KEYS] = {false};
	int			k;

	if (!s->child_batches || child == NULL || child->kind != VEXEC_NODE_SCAN ||
		s->nkeys == 0 || s->nkeys > INDEX_MAX_KEYS)
		return false;
	k = vexec_scan_ordering(child, INDEX_MAX_KEYS, order);
	if (k < s->nkeys)
		return false;
	for (int i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *key = &s->keys[i];
		TargetEntry *tle;
		Var		   *v;
		int			j;

		if (key->col >= list_length(cplan->targetlist))
			return false;
		tle = list_nth_node(TargetEntry, cplan->targetlist, key->col);
		v = (Var *) tle->expr;
		if (!IsA(v, Var) || v->varno != ((Scan *) cplan)->scanrelid || v->varlevelsup != 0 ||
			v->varattno < 1)
			return false;
		if (key->eqop != lookup_type_cache(v->vartype, TYPECACHE_EQ_OPR)->eq_opr ||
			(OidIsValid(key->collation) && !get_collation_isdeterministic(key->collation)))
			return false;
		for (j = 0; j < s->nkeys; j++)
			if (order[j] == v->varattno && !used[j])
				break;
		if (j == s->nkeys)
			return false;
		used[j] = true;
	}
	return true;
}

/*
 * At the first group asked for: the input in the keys' order, where the
 * plan said it may be and nothing else asked of the node hashes; else the
 * table, as any hashed aggregation's.
 */
static void
sorted_decide(VexecAggState *s)
{
	EState	   *estate = s->node.css.ss.ps.state;

	s->sorted_decided = true;
	s->sorted = s->sorted_planned && vexec_enable_sorted_agg && s->topn == 0 && s->topk == 0 &&
		s->ndeal == 0 && !s->may_freeze && !s->hashes_from_child && sorted_input_holds(s);
	if (!s->sorted)
	{
		table_reset(s);
		return;
	}
	s->array_ok = false;
	for (int g = 0; g < 2; g++)
	{
		s->sgroupcxt[g] = BumpContextCreate(s->node.mcxt, "VecAgg groups in order",
											ALLOCSET_DEFAULT_SIZES);
		s->saggcontext[g] = CreateWorkExprContext(estate);
	}
	sorted_reset(s);
}

/*
 * Input batches until one closes a group, or the input ends: the open
 * group's generation the states' memory of its rows, the newer one the
 * others' (agg_batch()).
 */
static void
sorted_fill(VexecAggState *s)
{
	for (;;)
	{
		MemoryContext old;

		if (s->nmade > 0)
			return;
		if (!vexec_node_next_input(&s->node))
		{
			s->sorted_eof = true;
			return;
		}
		old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);
		s->aggstate->curaggcontext = s->saggcontext[s->sgen];
		agg_batch(s);
		s->aggstate->curaggcontext = s->aggcontext;
		MemoryContextSwitchTo(old);
	}
}

/*
 * The next group in the keys' order, or NULL once every one is out: those
 * the last batch closed, the memory of those handed up before emptied first
 * -- their rows are their parent's until this call -- and the open one
 * last.
 */
static char *
sorted_next_group(VexecAggState *s)
{
	for (;;)
	{
		if (s->next_made < s->nmade)
			return s->made[s->next_made++];
		if (s->nmade > 0)
			sorted_round_end(s);
		if (s->sorted_eof)
		{
			if (s->cur != NULL)
			{
				sorted_close(s, s->cur);
				s->cur = NULL;
				continue;
			}
			s->done = true;
			return NULL;
		}
		sorted_fill(s);
	}
}

/*
 * The states' memory of the rows bmask marks: the batch table's (C2), made
 * as the table freezes, which it may do within the batch; the open group's
 * older generation (VC4)
 */
static inline ExprContext *
bmask_context(VexecAggState *s)
{
	return s->sorted ? s->saggcontext[1 - s->sgen] : s->baggcontext;
}

/*
 * A row's group: found, made, or NULL when the table is past its memory
 * and the row's group is not in it.
 */
static char *
row_group(VexecAggState *s, TupleTableSlot *slot, int row, uint32 hash)
{
	VexecAggEntry *e;
	bool		found;
	int64		si;

	if (s->strategy == AGG_PLAIN)
		return s->plaingroup;
	if (s->sorted)
		return sorted_row_group(s, slot, row);
	if (unlikely(s->table == NULL))
		table_make(s);
	*(uint32 *) s->probe_group = hash;
	if (s->spilling)
	{
		e = vagg_lookup_hash(s->table, s->probe_group, hash);
		return e ? e->group : NULL;
	}
	if (s->frozen)
	{
		/* bypassed: a group of the row's own, in its partition (above) */
		if (s->bypass)
		{
			vexec_bit_set(s->bmask, row);
			s->bypassed_rows++;
			return group_create_packed(s, hash, slot, row);
		}
		s->by_rows++;
		/* C2: a key the table holds updates it; the batch table takes the others */
		if (s->array_mode)
		{
			if (array_slot(s, s->probe, &si) && s->slots[si] != NULL)
				return s->slots[si];
		}
		else
		{
			e = vagg_lookup_hash(s->table, s->probe_group, hash);
			if (e != NULL)
				return e->group;
		}
		e = vagg_insert_hash(s->btable, s->probe_group, hash, &found);
		if (!found)
		{
			e->group = group_create_in(s, hash, slot, row, true);
			s->by_groups++;
		}
		vexec_bit_set(s->bmask, row);
		return e->group;
	}
	if (s->array_mode)
	{
		/* X5: the key's slot, where it is in range; else the hash table */
		if (array_slot(s, s->probe, &si))
		{
			char	   *g = s->slots[si];

			if (g == NULL)
			{
				g = s->slots[si] = group_create(s, hash, slot, row);
				group_made(s);
			}
			return g;
		}
		array_leave(s);
	}
	e = vagg_insert_hash(s->table, s->probe_group, hash, &found);
	if (!found)
	{
		e->group = group_create(s, hash, slot, row);
		group_made(s);
	}
	return e->group;
}

/*
 * A group made: past the memory the table may hold, the table spills --
 * from the hash table -- and no group is made; or a first stage under a
 * VecRepartition freezes it (C2), and freezes it for good where it thawed
 * or fills its memory (F16).
 */
static void
group_made(VexecAggState *s)
{
	Size		mem = table_memory(s);
	int			growth = s->thawed ? VEXEC_AGG_THAW_GROWTH : 1;

	if (s->depth < VEXEC_AGG_MAX_DEPTH - 1 && mem > s->memlimit)
	{
		/*
		 * F16 (pg_vector_executor.md §3.37): a first stage that may freeze
		 * -- the stage above combines whatever states it is dealt -- never
		 * spills: past its memory it freezes for good, the keys it does not
		 * hold handed on.  ClickBench's Q33 at 100M rows thawed, grew to
		 * its memory at 64 MB and spilled 2.1M rows a participant, read
		 * back to be handed on.
		 */
		if (s->may_freeze && s->depth == 0)
		{
			freeze_for_good(s);
			return;
		}
		if (s->array_mode)
			array_leave(s);
		spill_begin(s);
	}
	else if (s->may_freeze && s->depth == 0 &&
			 (s->ngroups >= (int64) vexec_partial_agg_limit * growth ||
			  MemoryContextMemAllocated(s->groupcxt, true) +
			  MemoryContextMemAllocated(s->aggcontext->ecxt_per_tuple_memory, true) >=
			  VEXEC_AGG_FREEZE_BYTES * growth))
	{
		/* F16: a thawed table, at VEXEC_AGG_THAW_GROWTH times the bounds it froze at, for good */
		if (s->thawed)
			freeze_for_good(s);
		else
			freeze(s);
	}
}

/*
 * A transition's FILTER and arguments for one row, evaluated in nodeAgg.c's
 * order: the FILTER, and the arguments only where it passes.  `pg` asks for
 * every one of them from PostgreSQL's evaluator, over `slot`; otherwise the
 * eager ones are read from their columns.  False where the FILTER rejects
 * the row.
 */
static bool
row_inputs(VexecAggState *s, VexecAggTrans *t, int row, TupleTableSlot *slot, bool pg)
{
	VexecNode  *node = &s->node;
	ExprContext *econtext = s->tmpcontext;
	int			k;

	if (t->filter >= 0)
	{
		Datum		d;
		bool		isnull;

		if (!pg && node->targets[t->filter].eager != NULL)
			d = vexec_vec_datum(node->work, node->outputs[t->filter], row, &isnull);
		else
		{
			econtext->ecxt_outertuple = slot;
			d = ExecEvalExpr(node->targets[t->filter].state, econtext, &isnull);
		}
		if (isnull || !DatumGetBool(d))
			return false;
	}
	for (k = 0; k < t->nargs; k++)
	{
		int			target = t->firstarg + k;

		if (!pg && node->targets[target].eager != NULL)
			s->argvals[k] = vexec_vec_datum(node->work, node->outputs[target], row,
											&s->argnulls[k]);
		else
		{
			econtext->ecxt_outertuple = slot;
			s->argvals[k] = ExecEvalExpr(node->targets[target].state, econtext,
										 &s->argnulls[k]);
		}
	}
	return true;
}

/* The vectorized transitions marked batched, over the rows of `rows`. */
static void
batch_transitions(VexecAggState *s, const uint64 *rows, int n)
{
	VexecNode  *node = &s->node;

	if (!vexec_bits_any(rows, n))
		return;
	for (int i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];

		if (!s->batched[i])
			continue;
		vexec_aggtrans_batch(t, s->aggstate, node->work,
							 s->strategy == AGG_PLAIN ? NULL : s->groups,
							 s->stateoff + t->stateoff,
							 s->strategy == AGG_PLAIN ? group_state(s, s->plaingroup, t) : NULL,
							 rows, n,
							 t->nargs > 0 ? node->outputs[t->firstarg] : NULL,
							 t->filter >= 0 ? node->outputs[t->filter] : NULL);
	}
}

/*
 * One batch, in row order.  A row a kernel computed reads its inputs from
 * the batch's columns; a row a kernel could not compute, or the child's
 * still to be resolved, is PostgreSQL's own, evaluated whole as it is
 * reached.  The vectorized transitions marked batched take their rows after
 * the loop.
 */
/*
 * F26 (§3.38): whether two rows' keys, as batch_keys() made them, are equal
 * by their bits or bytes alone; false where a key compares by its function
 */
static inline bool
probes_equal(const VexecAggState *s, const VexecAggProbe *a, const VexecAggProbe *b)
{
	for (int i = 0; i < s->nkeys; i++)
	{
		if (a[i].isnull != b[i].isnull)
			return false;
		if (a[i].isnull)
			continue;
		switch (s->keys[i].kind)
		{
			case KEY_BITS:
				if (a[i].value != b[i].value)
					return false;
				break;
			case KEY_BYTES:
				if (a[i].len != b[i].len || memcmp(a[i].bytes, b[i].bytes, a[i].len) != 0)
					return false;
				break;
			default:
				return false;
		}
	}
	return true;
}

/*
 * F30 (§3.38): a key column's value at a row as batch_keys() makes a probe's,
 * its type's bits zero-extended
 */
static inline Datum
vkey_value(const VexecVec *v, int row)
{
	switch (v->shape.width)
	{
		case 8:
			return (Datum) ((const int64 *) v->values)[row];
		case 4:
			return (Datum) (uint32) ((const int32 *) v->values)[row];
		default:
			return (Datum) (uint16) ((const int16 *) v->values)[row];
	}
}

/*
 * F30: the batch's keys read from their columns -- each an integer of its
 * type's width, no NULL among the batch's -- their hashes made over them as
 * batch_keys() makes them, and no probe made a row: agg_batch()'s loop
 * compares a row's values with the row before's, and makes the probe of a
 * row it looks up alone.  False, nothing done, where a key is of another
 * kind or layout, or a NULL is among them.
 */
static bool
vector_keys(VexecAggState *s, const int *rows, int nlist)
{
	VexecBatch *in = s->node.in;
	const uint32 *hashes = s->batch_hashes;
	int			nk = s->nkeys;

	for (int i = 0; i < nk; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecVec   *v = &in->cols[k->col];

		if (k->kind != KEY_BITS || v->encoding != VEXEC_FLAT || v->shape.layout != VEXEC_FIXED ||
			v->shape.arrow_values || v->shape.stride != v->shape.width || v->shape.width != k->typlen ||
			(v->shape.width != 2 && v->shape.width != 4 && v->shape.width != 8) || v->validity != NULL)
			return false;
	}
	if (hashes != NULL)
	{
		/* the child's hashes are this node's (C4) */
		for (int j = 0; j < nlist; j++)
			s->rowhash[rows[j]] = hashes[rows[j]];
	}
	else if (nk == 1 && in->cols[s->keys[0].col].shape.width == 8)
	{
		const int64 *vals = (const int64 *) in->cols[s->keys[0].col].values;

		for (int j = 0; j < nlist; j++)
			s->rowhash[rows[j]] = murmurhash32((uint32) murmurhash64((uint64) vals[rows[j]]));
	}
	else
	{
		for (int j = 0; j < nlist; j++)
		{
			int			row = rows[j];
			uint32		h = 0;

			for (int i = 0; i < nk; i++)
			{
				h = pg_rotate_left32(h, 1);
				h ^= (uint32) murmurhash64((uint64) vkey_value(&in->cols[s->keys[i].col], row));
			}
			s->rowhash[row] = murmurhash32(h);
		}
	}
	s->keys_vectored = true;
	s->vector_key_batches++;
	return true;
}

/* F30: whether two rows' keys are equal, by their columns' values */
static inline bool
vkeys_equal(VexecAggState *s, int a, int b)
{
	VexecBatch *in = s->node.in;

	for (int i = 0; i < s->nkeys; i++)
	{
		const VexecVec *v = &in->cols[s->keys[i].col];

		if (vkey_value(v, a) != vkey_value(v, b))
			return false;
	}
	return true;
}

/* F30: a row's probe, from its columns, for its lookup */
static inline VexecAggProbe *
vkeys_probe(VexecAggState *s, int row)
{
	VexecBatch *in = s->node.in;

	for (int i = 0; i < s->nkeys; i++)
	{
		VexecAggProbe *p = &s->vprobe[i];

		p->isnull = false;
		p->scaled = false;
		p->value = vkey_value(&in->cols[s->keys[i].col], row);
	}
	return s->vprobe;
}

/*
 * F31 (§3.38): whether the rows of `rows` hold one key -- each key column's
 * values the first row's, by their bits, their bytes or a dictionary's codes,
 * and their NULLs alike -- compared over the columns before any probe or
 * hash is made; false where a key is of another kind or layout
 */
static bool
batch_one_key(VexecAggState *s, const int *rows, int nlist)
{
	VexecBatch *in = s->node.in;
	int			first = rows[0];

	for (int i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecVec   *v = &in->cols[k->col];
		bool		fnull = vexec_vec_isnull(v, first);

		if (v->validity != NULL || fnull)
			for (int j = 1; j < nlist; j++)
				if (vexec_vec_isnull(v, rows[j]) != fnull)
					return false;
		if (fnull)
			continue;
		if (k->kind == KEY_BITS && v->encoding == VEXEC_FLAT && v->shape.layout == VEXEC_FIXED &&
			!v->shape.arrow_values && v->shape.stride == v->shape.width && v->shape.width == k->typlen &&
			(v->shape.width == 2 || v->shape.width == 4 || v->shape.width == 8))
		{
			const char *vals = (const char *) v->values;
			int			w = v->shape.width;

			if (w == 8)
			{
				int64		f = ((const int64 *) vals)[first];

				for (int j = 1; j < nlist; j++)
					if (((const int64 *) vals)[rows[j]] != f)
						return false;
			}
			else if (w == 4)
			{
				int32		f = ((const int32 *) vals)[first];

				for (int j = 1; j < nlist; j++)
					if (((const int32 *) vals)[rows[j]] != f)
						return false;
			}
			else
			{
				int16		f = ((const int16 *) vals)[first];

				for (int j = 1; j < nlist; j++)
					if (((const int16 *) vals)[rows[j]] != f)
						return false;
			}
		}
		else if (k->kind == KEY_BYTES && v->encoding == VEXEC_DICT)
		{
			for (int j = 1; j < nlist; j++)
				if (v->codes[rows[j]] != v->codes[first])
					return false;
		}
		else if (k->kind == KEY_BYTES && v->encoding == VEXEC_FLAT &&
				 (v->shape.layout == VEXEC_VIEW || v->shape.layout == VEXEC_OFFSETS))
		{
			const char *fb;
			Size		flen;

			vexec_vec_value_bytes(v, first, &fb, &flen);
			for (int j = 1; j < nlist; j++)
			{
				const char *b;
				Size		len;

				vexec_vec_value_bytes(v, rows[j], &b, &len);
				if (len != flen || (b != fb && memcmp(b, fb, len) != 0))
					return false;
			}
		}
		else
			return false;
	}
	return true;
}

/*
 * F31: a batch of one key -- every transition taking the batch at once, no
 * row PostgreSQL's evaluator's -- takes the key's group once and its
 * transitions as a plain aggregation's, over the batch into that group's
 * states: ClickBench's Q27 groups by CounterID, which nearly every batch of
 * the table's order holds alone.  True where it took the batch; false, with
 * nothing done, where the group is not to be had (the table spilling).
 */
static bool
agg_one_key(VexecAggState *s, int nlist, int n)
{
	VexecNode  *node = &s->node;
	int			first = s->rows[0];
	uint64	   *taken;
	uint32		hash;
	char	   *g;
	bool		in_batch_table;
	ExprContext *mainctx = s->aggstate->curaggcontext;

	batch_keys(s, s->rows, 1);
	hash = s->rowhash[first];
	if (s->topk_skip)
	{
		uint32		b = topk_bound(s);

		if (b != PG_UINT32_MAX && hash > b)
		{
			s->rows_skipped += nlist;
			return true;
		}
	}
	s->probe = &s->rowkeys[first * s->nkeys];
	g = row_group(s, NULL, first, hash);
	if (g == NULL)
		return false;
	in_batch_table = s->bmask != NULL && vexec_bit(s->bmask, first);
	if (s->frozen)
		s->by_rows += nlist - 1;	/* as row_group() counts each row it finds */
	taken = vexec_bitmap_alloc(node->work, n, false);
	for (int j = 0; j < nlist; j++)
	{
		vexec_bit_set(taken, s->rows[j]);
		if (in_batch_table)
			vexec_bit_set(s->bmask, s->rows[j]);
	}
	if (in_batch_table)
		s->aggstate->curaggcontext = bmask_context(s);
	for (int i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];

		vexec_aggtrans_batch(t, s->aggstate, node->work, NULL, 0, group_state(s, g, t), taken, n,
							 t->nargs > 0 ? node->outputs[t->firstarg] : NULL,
							 t->filter >= 0 ? node->outputs[t->filter] : NULL);
	}
	s->aggstate->curaggcontext = mainctx;
	s->rows_batched += nlist;
	s->one_key_batches++;
	ResetExprContext(s->tmpcontext);
	return true;
}

/*
 * F32 (§3.38): a key column as key_runs() compares it over the column, the
 * layouts batch_keys() reads without a Datum
 */
typedef enum VexecRunKey
{
	RUN_KEY_NONE,
	RUN_KEY_INT,				/* an integer of its type's width */
	RUN_KEY_SCALED,				/* F10's scaled int64 */
	RUN_KEY_DICT,				/* a dictionary's codes */
	RUN_KEY_BYTES				/* a text's bytes: offsets or views */
} VexecRunKey;

static inline VexecRunKey
run_key_kind(const VexecAggKeyCol *k, const VexecVec *v)
{
	if (k->kind == KEY_BITS && v->encoding == VEXEC_FLAT && v->shape.layout == VEXEC_FIXED &&
		!v->shape.arrow_values && v->shape.stride == v->shape.width && v->shape.width == k->typlen &&
		(v->shape.width == 2 || v->shape.width == 4 || v->shape.width == 8))
		return RUN_KEY_INT;
	if (k->kind == KEY_BYTES && v->encoding == VEXEC_FLAT &&
		(v->shape.layout == VEXEC_VIEW || v->shape.layout == VEXEC_OFFSETS))
		return RUN_KEY_BYTES;
	if (k->kind == KEY_BYTES && v->encoding == VEXEC_DICT &&
		(v->dictionary->shape.layout == VEXEC_VIEW || v->dictionary->shape.layout == VEXEC_OFFSETS))
		return RUN_KEY_DICT;
	if (k->numeric && v->encoding == VEXEC_FLAT && v->shape.layout == VEXEC_SCALED &&
		v->shape.width == 8 && v->shape.scale >= 0 && v->shape.scale <= 18)
		return RUN_KEY_SCALED;
	return RUN_KEY_NONE;
}

/* Each place of rows whose values, at their rows, differ from the place before's, marked in diff. */
#define RUN_MARK(type, values) \
	do { \
		const type *vv_ = (const type *) (values); \
		if (dense) \
			for (int j_ = 1; j_ < nlist; j_++) \
				diff[j_] |= vv_[j_] != vv_[j_ - 1]; \
		else \
			for (int j_ = 1; j_ < nlist; j_++) \
				diff[j_] |= vv_[rows[j_]] != vv_[rows[j_ - 1]]; \
	} while (0)

/* The places of rows not yet marked whose text differs from the place before's. */
static void
run_mark_bytes(const VexecVec *v, const int *rows, int nlist, uint8 *diff)
{
	if (v->shape.layout == VEXEC_OFFSETS)
	{
		const int32 *off = (const int32 *) v->values;
		const char *data = v->buffers[0];

		for (int j = 1; j < nlist; j++)
		{
			int			a = rows[j - 1];
			int			b = rows[j];
			int32		len = off[b + 1] - off[b];

			if (diff[j])
				continue;
			if (len != off[a + 1] - off[a] ||
				(len > 0 && off[a] != off[b] && memcmp(data + off[a], data + off[b], len) != 0))
				diff[j] = 1;
		}
	}
	else
	{
		const VexecView *views = (const VexecView *) v->values;

		for (int j = 1; j < nlist; j++)
		{
			const VexecView *a = &views[rows[j - 1]];
			const VexecView *b = &views[rows[j]];
			int32		len = b->inlined.size;

			if (diff[j])
				continue;
			if (len != a->inlined.size)
				diff[j] = 1;
			else if (len <= VEXEC_VIEW_INLINE)
				diff[j] = len > 0 && memcmp(a->inlined.data, b->inlined.data, len) != 0;
			else if (memcmp(a->ref.prefix, b->ref.prefix, sizeof(a->ref.prefix)) != 0 ||
					 memcmp(v->buffers[a->ref.buffer_index] + a->ref.offset,
							v->buffers[b->ref.buffer_index] + b->ref.offset, len) != 0)
				diff[j] = 1;
		}
	}
}

/*
 * F32 (§3.38): the batch's rows of `rows` cut into runs of one key, over the
 * key columns before any row is hashed: integers and F10's scaled by their
 * values, a dictionary's by its codes, a text's by its length and then its
 * bytes, NULLs by their flags.  Then each run's first row alone hashed and
 * given its probe, as batch_keys() does -- so that a stage reading the hashes
 * reads the same -- and agg_runs() takes the runs.  False, nothing done, where
 * a key is of another kind or layout.
 */
static bool
key_runs(VexecAggState *s, const int *rows, int nlist)
{
	VexecBatch *in = s->node.in;
	int			nk = s->nkeys;
	const uint32 *hashes = s->batch_hashes;
	uint8	   *diff = s->run_diff;
	bool		dense = nlist == in->nrows;
	bool		any_bytes = false;
	int			nruns = 0;
	VexecRunKey kinds[INDEX_MAX_KEYS];
	const uint32 *dhash[INDEX_MAX_KEYS];

	if (nk > INDEX_MAX_KEYS)
		return false;
	for (int i = 0; i < nk; i++)
	{
		const VexecVec *v = &in->cols[s->keys[i].col];

		kinds[i] = run_key_kind(&s->keys[i], v);
		if (kinds[i] == RUN_KEY_NONE)
			return false;
	}
	for (int i = 0; i < nk; i++)
		dhash[i] = kinds[i] == RUN_KEY_DICT && hashes == NULL ?
			dict_key_hashes(s, i, &in->cols[s->keys[i].col], nlist) : NULL;

	/* the columns of cheap comparisons first, then what they leave of the bytes */
	memset(diff, 0, nlist);
	diff[0] = 1;
	for (int i = 0; i < nk; i++)
	{
		const VexecVec *v = &in->cols[s->keys[i].col];

		if (v->validity != NULL)
			for (int j = 1; j < nlist; j++)
				diff[j] |= vexec_bit(v->validity, rows[j]) != vexec_bit(v->validity, rows[j - 1]);
		switch (kinds[i])
		{
			case RUN_KEY_INT:
				if (v->shape.width == 8)
					RUN_MARK(int64, v->values);
				else if (v->shape.width == 4)
					RUN_MARK(int32, v->values);
				else
					RUN_MARK(int16, v->values);
				break;
			case RUN_KEY_SCALED:
				RUN_MARK(int64, v->values);
				break;
			case RUN_KEY_DICT:
				RUN_MARK(int32, v->codes);
				break;
			default:
				any_bytes = true;
				break;
		}
	}
	if (any_bytes)
		for (int i = 0; i < nk; i++)
			if (kinds[i] == RUN_KEY_BYTES)
				run_mark_bytes(&in->cols[s->keys[i].col], rows, nlist, diff);

	for (int j = 0; j < nlist; j++)
	{
		s->run_begin[nruns] = j;
		nruns += diff[j] != 0;
	}
	s->run_begin[nruns] = nlist;
	s->nruns = nruns;

	for (int r = 0; r < nruns; r++)
	{
		int			row = rows[s->run_begin[r]];
		uint32		h = 0;

		for (int i = 0; i < nk; i++)
		{
			VexecAggKeyCol *k = &s->keys[i];
			const VexecVec *v = &in->cols[k->col];
			VexecAggProbe *p = &s->rowkeys[row * nk + i];

			h = pg_rotate_left32(h, 1);
			p->isnull = v->validity != NULL && !vexec_bit(v->validity, row);
			p->scaled = false;
			if (p->isnull)
				continue;
			switch (kinds[i])
			{
				case RUN_KEY_INT:
					p->value = vkey_value(v, row);
					break;
				case RUN_KEY_SCALED:
					p->value = (Datum) ((const int64 *) v->values)[row];
					p->scaled = true;
					p->scale = v->shape.scale;
					break;
				case RUN_KEY_DICT:
					{
						const char *bytes;
						Size		len;

						vexec_vec_value_bytes(v->dictionary, v->codes[row], &bytes, &len);
						p->bytes = bytes;
						p->len = (int32) len;
						if (dhash[i] != NULL)
						{
							h ^= dhash[i][v->codes[row]];
							continue;
						}
					}
					break;
				default:
					{
						const char *bytes;
						Size		len;

						vexec_vec_value_bytes(v, row, &bytes, &len);
						p->bytes = bytes;
						p->len = (int32) len;
					}
					break;
			}
			if (hashes == NULL)
				h ^= key_hash(k, p);
		}
		s->rowhash[row] = hashes != NULL ? hashes[row] : murmurhash32(h);
	}
	s->keys_runs = true;
	s->run_batches++;
	s->runs_total += nruns;
	return true;
}

/*
 * F32: the runs key_runs() cut, each run's group looked up once and taken by
 * its rows at once: where every transition is count(*), the run's length added
 * to each count, no group stored a row and true returned, the batch done; else
 * the rows' groups and bits, a run's in a stroke where the batch's rows are all
 * selected, for agg_batch()'s batched transitions.  A run past C5's boundary is
 * passed over whole; a table spilling sends a run's rows to their partition.
 * F33: a frozen table's measure of its groups over the rows looked up
 * (bypass_decide()), each run one, a bypassed run a packed group of its own.
 */
static bool
agg_runs(VexecAggState *s, int nlist, int n, uint64 *taken)
{
	const int  *rows = s->rows;
	bool		dense = nlist == n;
	bool		all_counts = true;
	uint32		bound = s->topk_skip ? topk_bound(s) : PG_UINT32_MAX;
	uint64	   *untaken = NULL;
	int64		done = 0;

	for (int i = 0; i < s->ntrans; i++)
		all_counts &= s->trans[i].kind == VEXEC_AGG_COUNT_STAR && s->trans[i].filter < 0;

	for (int ri = 0; ri < s->nruns; ri++)
	{
		int			j0 = s->run_begin[ri];
		int			j1 = s->run_begin[ri + 1];
		int			row = rows[j0];
		uint32		hash = s->rowhash[row];
		char	   *g;
		int			j;

		if (ri + 8 < s->nruns && s->table != NULL && !s->bypass)
			__builtin_prefetch(&s->table->data[s->rowhash[rows[s->run_begin[ri + 8]]] & s->table->sizemask]);
		if (hash > bound)
		{
			s->rows_skipped += j1 - j0;
			if (dense && !all_counts)
			{
				if (untaken == NULL)
					untaken = vexec_bitmap_alloc(s->node.work, n, false);
				for (j = j0; j < j1; j++)
					vexec_bit_set(untaken, j);
			}
			continue;
		}
		s->probe = &s->rowkeys[row * s->nkeys];
		g = row_group(s, NULL, row, hash);
		if (g == NULL)
		{
			if (dense && !all_counts && untaken == NULL)
				untaken = vexec_bitmap_alloc(s->node.work, n, false);
			for (j = j0; j < j1; j++)
			{
				spill_row(s, hash, NULL, rows[j]);
				if (dense && !all_counts)
					vexec_bit_set(untaken, j);
			}
			continue;
		}
		s->consecutive_rows += j1 - j0 - 1;
		if (s->bypass)
			s->bypassed_rows += j1 - j0 - 1;
		else if (s->frozen && !vexec_enable_bypass_over_runs)
			s->by_rows += j1 - j0 - 1;
		done += j1 - j0;
		if (all_counts)
		{
			for (int i = 0; i < s->ntrans; i++)
				vexec_aggtrans_count_run(group_state(s, g, &s->trans[i]), j1 - j0);
			continue;
		}
		if (dense)
		{
			/* past the run's end at times, which the next run writes over */
			j = j0;
			do
			{
				s->groups[j] = g;
				s->groups[j + 1] = g;
				s->groups[j + 2] = g;
				s->groups[j + 3] = g;
				j += 4;
			} while (j < j1);
			if (s->bmask != NULL && vexec_bit(s->bmask, row))
				for (j = j0 + 1; j < j1; j++)
					vexec_bit_set(s->bmask, j);
		}
		else
		{
			bool		in_mask = s->bmask != NULL && vexec_bit(s->bmask, row);

			for (j = j0; j < j1; j++)
			{
				s->groups[rows[j]] = g;
				vexec_bit_set(taken, rows[j]);
				if (in_mask)
					vexec_bit_set(s->bmask, rows[j]);
			}
		}
	}
	CHECK_FOR_INTERRUPTS();
	if (all_counts)
	{
		s->rows_batched += done;
		ResetExprContext(s->tmpcontext);
		return true;
	}
	if (dense)
	{
		for (int w = 0; w < (n + 63) >> 6; w++)
			taken[w] = untaken == NULL ? ~UINT64CONST(0) : ~untaken[w];
		if (n & 63)
			taken[n >> 6] &= (UINT64CONST(1) << (n & 63)) - 1;
	}
	return false;
}

static void
agg_batch(VexecAggState *s)
{
	VexecNode  *node = &s->node;
	VexecBatch *in = node->in;
	int			n = in->nrows;
	bool		any_redo = vexec_bits_any(node->redo, n) ||
		(node->child_redo != NULL && vexec_bits_any(node->child_redo, n));
	uint64	   *taken;
	bool		any_batched = false;
	bool		all_batched = true;
	ExprContext *mainctx = s->aggstate->curaggcontext;
	int			nlist;
	int			row;
	int			i,
				j;

	/*
	 * C2: the rows whose group is the batch table's, once the table froze;
	 * VC4: those of the open group of the older generation
	 */
	s->bmask = s->may_freeze || s->sorted ? vexec_bitmap_alloc(node->work, n, false) : NULL;

	/* which vectorized transitions take this batch at once */
	for (i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];
		bool		ok = t->kind != VEXEC_AGG_FMGR;
		int			k;

		for (k = 0; k < t->nargs && ok; k++)
			ok = node->targets[t->firstarg + k].eager != NULL;
		if (ok && t->filter >= 0)
			ok = node->targets[t->filter].eager != NULL;
		if (ok && (t->may_raise || t->order_dependent))
			ok = !any_redo && s->floats_ok;
		s->batched[i] = ok;
		any_batched |= ok;
		all_batched &= ok;
	}

	/*
	 * A plain aggregation that takes the whole batch at once has nothing to
	 * do a row at a time: its one group takes every row the batch selects.
	 */
	if (s->strategy == AGG_PLAIN && all_batched && !any_redo)
	{
		taken = node->candidates;
		goto batched;
	}

	taken = vexec_bitmap_alloc(node->work, n, false);

	/* the rows to take, in order: the kernels', PostgreSQL's, the child's */
	{
		uint64	   *all = vexec_bits_copy(node->work, node->candidates, n);

		vexec_bits_or_into(all, node->redo, n);
		if (node->child_redo != NULL)
			vexec_bits_or_into(all, node->child_redo, n);
		nlist = 0;
		VEXEC_FOREACH_ROW(all, n, r)
			s->rows[nlist++] = r;
	}
	/*
	 * F31: a batch of one key, as a plain aggregation's into its group --
	 * not before X5 has decided its table by a first batch's keys, nor in
	 * the keys' order (VC4), nor bypassed, nor where a key compares by its
	 * function
	 */
	if (s->strategy == AGG_HASHED && all_batched && !any_redo && !s->any_lazy && nlist > 1 &&
		vexec_enable_one_key_batches && !s->sorted && !s->bypass &&
		(!s->array_ok || s->array_decided) && batch_one_key(s, s->rows, nlist) &&
		agg_one_key(s, nlist, n))
		return;
	s->keys_vectored = false;
	s->keys_runs = false;

	/*
	 * F32: where every row only finds its group, the batch's rows cut into
	 * runs of one key before any is hashed -- not before X5 has decided its
	 * table by a first batch's probes, nor in the keys' order (VC4)
	 */
	if (s->strategy == AGG_HASHED && vexec_enable_key_runs && vexec_enable_consecutive_keys &&
		all_batched && !any_redo && !s->any_lazy && !s->sorted && (!s->array_ok || s->array_decided) &&
		key_runs(s, s->rows, nlist))
	{
		if (agg_runs(s, nlist, n, taken))
			return;
		goto batched;
	}
	if (s->strategy == AGG_HASHED)
	{
		/*
		 * F30: integer keys read from their columns where every row only finds
		 * its group (below) -- not before X5 has decided its table by a first
		 * batch's probes, nor in the keys' order (VC4)
		 */
		if (!(vexec_enable_vector_keys && all_batched && !any_redo && !s->any_lazy && !s->sorted &&
			  (!s->array_ok || s->array_decided) && vector_keys(s, s->rows, nlist)))
			batch_keys(s, s->rows, nlist);
		/*
		 * C5: a row whose key's hash is past the boundary is passed over:
		 * K groups of smaller hashes are held, so its group is no one's of
		 * those the LIMIT takes.  Not a row PostgreSQL's evaluator decides.
		 */
		if (s->topk_skip)
		{
			uint32		b = topk_bound(s);

			if (b != PG_UINT32_MAX)
			{
				int			kept = 0;

				for (j = 0; j < nlist; j++)
				{
					int			r = s->rows[j];

					if (s->rowhash[r] > b && !vexec_bit(node->redo, r) &&
						!(node->child_redo != NULL && vexec_bit(node->child_redo, r)))
						continue;
					s->rows[kept++] = r;
				}
				s->rows_skipped += nlist - kept;
				nlist = kept;
			}
		}
		/* X5: the table an array, by its first batch's keys */
		if (s->array_ok && !s->array_decided && s->ngroups == 0 && !s->frozen &&
			!s->spilling && s->reading == NULL && (s->table != NULL || s->table_lazy))
			array_decide(s, nlist);
	}

	/*
	 * Where every transition takes the batch at once and no row needs
	 * PostgreSQL's evaluator, a row only finds its group.
	 */
	if (s->strategy == AGG_HASHED && all_batched && !any_redo && !s->any_lazy)
	{
		/*
		 * F26: a row whose keys are the row before's takes its group,
		 * unlooked-up -- a table in its keys' order repeats them -- as the
		 * frozen table's lookup would count it; not where the rows bypass
		 * the table, each a group of its own, nor where a key compares by
		 * its function
		 */
		bool		consecutive = vexec_enable_consecutive_keys && !s->sorted && !s->bypass;
		int			prev = -1;
		char	   *prev_g = NULL;

		for (j = 0; j < nlist; j++)
		{
			char	   *g;

			row = s->rows[j];
			if (j + 8 < nlist && s->table != NULL)
				__builtin_prefetch(&s->table->data[s->rowhash[s->rows[j + 8]] & s->table->sizemask]);
			if (s->keys_vectored ?
				consecutive && prev_g != NULL && s->rowhash[row] == s->rowhash[prev] && vkeys_equal(s, row, prev) :
				(s->probe = &s->rowkeys[row * s->nkeys],
				 consecutive && prev_g != NULL && s->rowhash[row] == s->rowhash[prev] &&
				 probes_equal(s, s->probe, &s->rowkeys[prev * s->nkeys])))
			{
				g = prev_g;
				if (s->frozen)
				{
					s->by_rows++;
					if (s->bmask != NULL && vexec_bit(s->bmask, prev))
						vexec_bit_set(s->bmask, row);
				}
				s->consecutive_rows++;
			}
			else
			{
				if (s->keys_vectored)
					s->probe = vkeys_probe(s, row);	/* F30: the row's alone, as it is looked up */
				g = row_group(s, NULL, row, s->rowhash[row]);
			}
			prev = row;
			prev_g = g;
			if (g == NULL)
			{
				spill_row(s, s->rowhash[row], NULL, row);
				continue;
			}
			s->groups[row] = g;
			vexec_bit_set(taken, row);
		}
		CHECK_FOR_INTERRUPTS();
		goto batched;
	}

	for (j = 0; j < nlist; j++)
	{
		TupleTableSlot *slot = NULL;
		bool		pg = false;
		char	   *g;
		uint32		hash = 0;

		row = s->rows[j];
		ResetExprContext(s->tmpcontext);
		CHECK_FOR_INTERRUPTS();

		/* the bucket of a row ahead, for the table's misses to overlap */
		if (s->strategy == AGG_HASHED && j + 8 < nlist && s->table != NULL)
			__builtin_prefetch(&s->table->data[s->rowhash[s->rows[j + 8]] & s->table->sizemask]);

		if (node->child_redo != NULL && vexec_bit(node->child_redo, row))
		{
			/* the child's row, resolved by PostgreSQL's evaluator */
			slot = vexec_resolve_row(node->vec_child, row);
			if (slot == NULL)
				continue;
			slot_getallattrs(slot);
			node->loaded_row = -1;
			pg = true;
		}
		else if (vexec_bit(node->redo, row))
		{
			vexec_node_load_input(node, row);
			slot = node->input_slot;
			pg = true;
		}

		if (s->strategy == AGG_HASHED)
		{
			if (node->child_redo != NULL && vexec_bit(node->child_redo, row))
			{
				probe_from_slot(s, slot);
				hash = probe_hash(s);
			}
			else
			{
				s->probe = &s->rowkeys[row * s->nkeys];
				hash = s->rowhash[row];
			}
		}
		g = row_group(s, slot, row, hash);

		if (pg)
			s->rows_by_postgres++;
		else if (s->any_lazy)
		{
			/* the lazy inputs read the input row */
			vexec_node_load_input(node, row);
			slot = node->input_slot;
		}

		if (g == NULL)
		{
			/*
			 * Its group is not in the table: the row goes to a partition.
			 * nodeAgg.c still evaluates its FILTERs and arguments now, and
			 * again when it reads the row back (execExpr.c:3700-3960, the
			 * group's NULL check coming last); so do the lazy ones here.
			 */
			for (i = 0; i < s->ntrans && (pg || s->any_lazy); i++)
				(void) row_inputs(s, &s->trans[i], row, slot, pg);
			spill_row(s, hash, pg ? slot : NULL, row);
			continue;
		}

		if (!pg)
		{
			s->groups[row] = g;
			vexec_bit_set(taken, row);
		}
		/* a batch table's group keeps its states in that table's memory (C2) */
		if (s->bmask != NULL && vexec_bit(s->bmask, row))
			s->aggstate->curaggcontext = bmask_context(s);
		for (i = 0; i < s->ntrans; i++)
		{
			VexecAggTrans *t = &s->trans[i];

			if (!pg && s->batched[i])
				continue;
			if (row_inputs(s, t, row, slot, pg))
				vexec_aggtrans_advance(t, s->aggstate, group_state(s, g, t),
									   s->argvals, s->argnulls);
		}
		s->aggstate->curaggcontext = mainctx;
	}

batched:
	if (!any_batched || !vexec_bits_any(taken, n))
		return;
	s->rows_batched += vexec_bits_count(taken, n);
	if (s->bmask != NULL && vexec_bits_any(s->bmask, n))
	{
		/* the table's groups, then the batch table's in its memory (C2) */
		batch_transitions(s, vexec_bits_andnot(node->work, taken, s->bmask, n), n);
		s->aggstate->curaggcontext = bmask_context(s);
		batch_transitions(s, vexec_bits_and(node->work, taken, s->bmask, n), n);
		s->aggstate->curaggcontext = mainctx;
	}
	else
		batch_transitions(s, taken, n);
	ResetExprContext(s->tmpcontext);
}

/* ---------------------------------------------------------------------
 * The device strategy (§3.11, "The operators on the device", V8b)
 * ---------------------------------------------------------------------
 *
 * Where the node may take it as it begins -- a hashed first stage, or a
 * whole aggregation, its keys integers' bits or text's bytes, every
 * transition one whose state the device keeps (aggtrans.c), every input
 * eager -- its batches go to the device in chunks of vexec.gpu_chunk_rows
 * rows: each key's column, each aggregate's argument and each FILTER as the
 * CPU computed them over the batch, its selected rows alone.  The device
 * groups each chunk into its table; an answer is that table's groups, after
 * which the device starts again empty, and they are merged into the node's
 * table as a partition's packed groups are (D2), each looked up by its keys
 * and its states merged as the transition would have advanced them.
 * HAVING, the final functions, C3's top n, the projection and the output
 * are the node's, as they are.  The chunk being filled overlaps the device's
 * work on the one before it.
 *
 * A fallback costs time, never an answer (§3.11, "Falling back"):
 *   - a batch with rows for PostgreSQL's evaluator, or a column in a layout
 *     with no copy here: the CPU strategy, into the same table;
 *   - a chunk the device refused or had no memory for: its rows redone on
 *     the CPU from the chunk, and the device's groups so far answered;
 *   - the executor lost, or any other answer than OK: every chunk the
 *     device's answers do not yet cover -- each is kept until one does --
 *     redone on the CPU, and the rest of the input there.
 * An answer is merged once it is whole, its pages staged in the node's
 * memory meanwhile, so that an executor lost in the middle of one leaves
 * nothing merged twice.  A group the table cannot take goes to its
 * partition packed, beside the partition's rows, and is merged back before
 * them.
 *
 * The whole grouping's answer handed up (W1, §3.26).  Where the device
 * grouped every row -- no answer before the input's last, no batch on the
 * CPU, no group in the table -- that last answer holds each key once, and
 * is handed up straight from its staged pages: each of its rows made a
 * group of its own in `sgroup`, its states begun and merged from the row as
 * a table's group's would be, then finished and projected as any group is,
 * and gone as the next is made.  So no group is looked up, made in the
 * table or spilled.  To keep it so, the device is answered no sooner than
 * the input's end: the chunks kept for a redo let go once they would fill
 * half the task's blocks, where the input may be read again from its
 * start -- a serial one, no parallel-aware node below -- which is then the
 * redo should the device's answer be lost (`unkept`, `restart`: the table
 * emptied, the input read again on the CPU).  An answer that would take
 * more than the table's memory to stage is still asked for sooner, unless
 * it is ranked.
 *
 * The top n on the device (W2).  A bounded sort above that lends the node
 * its keys and bound (C3, vexec_agg_set_topn()) lends the device the prefix
 * of them it orders as PostgreSQL's operators do: the last answer, asked
 * ranked, is then the bound's first groups and every group tying with the
 * last of them on that prefix -- or just the bound's, where the prefix is
 * every key, its ties the sort's own -- which C3's heap then ranks.
 */

static void spill_packed(VexecAggState *s, uint32 hash, const char *rec);
static void take_packed(VexecAggState *s, int n);

/* An answer page, its columns copied out of the arena. */
typedef struct AggDevPage
{
	uint32		nrows;
	const char **values;
	const uint64 **validity;
	const int32 **offsets;
} AggDevPage;

typedef struct AggDevice
{
	char	   *program;		/* GROUPBY's, as the task begins */
	Size		proglen;
	int			ncols;
	VexecDevColType *types;
	int		   *col_src;		/* per chunk column: its key, or -1 less its
								 * input target */
	bool	   *col_true;		/* per chunk column: a FILTER's true rows */
	int		   *target_col;		/* per input target: its chunk column, or -1 */
	int			ndev;			/* the device's aggregates */
	int		   *trans_first;	/* per transition: its first, and */
	int		   *trans_n;		/* how many; 0: a state another's holds */
	int		   *dev_type;		/* per device aggregate: its result's type, */
	int16	   *dev_scale;		/* and a decimal's scale */
	VexecDevAgg *aggs;			/* the device's aggregates, for the program */
	uint32		cap;			/* a chunk's rows */
	int64		stage_groups;	/* the device's groups an answer is asked at */
	int64		stream_groups;	/* W1: and the groups a last answer handed up
								 * from its pages is staged with, its
								 * columns' bytes a group */

	/*
	 * W1: the last answer handed up from its pages (above): whether the
	 * input may be read again from its start, the chunks for a redo let go,
	 * the input to be read again on the CPU, the answer being handed up --
	 * its next page and row -- and the group a row is made into, its keys'
	 * bytes and states' memory in scxt's, reset as the next is made
	 */
	bool		rescannable;
	bool		unkept;
	bool		restart;
	bool		stream;
	ListCell   *spage;
	uint32		srow;

	/*
	 * W14 (§3.27): the grouping under a LIMIT without ORDER BY -- C5's on
	 * the CPU -- its last answer read a page at a time as it is handed up,
	 * the pages past the limit's groups never asked for: whether it is, and
	 * whether the answer has pages yet to read
	 */
	bool		limited;
	bool		more;
	char	   *sgroup;
	ExprContext *scxt;

	/* W2: the last answer's rank, a bounded sort's prefix of keys and bound */
	int			nrank;
	VexecDevRankKey *rank;
	bool		rank_ties;
	uint64		rank_k;
	bool		ranked;			/* the last answer was asked ranked */
	int64		groups_before_rank;
	bool		rank_sorted;	/* W37: the ranked answer's groups by sort */

	/*
	 * W17 (§3.28): the HAVING the device applies to its answer's groups before
	 * its rank and its pages -- a filter of the units' machine over the answer's
	 * columns -- where every aggregate it reads has its result in one device
	 * aggregate's value; else why not, for EXPLAIN.  W46 (§3.35): asked of the
	 * input's only answer alone, an earlier one's groups partial (having_left:
	 * left to the node, the device answering before the input's end).
	 */
	char	   *having;
	Size		having_len;
	const char *having_why;
	bool		having_left;

	/*
	 * W47 (§3.35): the groups held on the device to the input's end for its
	 * HAVING, past what an answer is staged whole with -- the most it held
	 * -- and whether its answer was then read a page at a time
	 */
	bool		held;
	int64		held_groups;
	bool		paged;

	VexecDevTask *t;
	bool		started;
	bool		lost;
	const char *why;			/* why the rest ran on the CPU */
	const char *device;
	VexecDevFill *fill;			/* being filled */
	VexecDevFill *flight;		/* submitted, its answer not yet read */
	List	   *kept;			/* taken, not yet covered by an answer */
	int			kept_blocks;
	int64		dev_groups;		/* the device's, after its last chunk */
	bool		last_answer;	/* the answer asked for is the input's last */
	uint32		pages[2];
	int			npages;
	MemoryContext stagecxt;
	List	   *staged;			/* AggDevPage * */
	VexecAggDevValue *vals;		/* a group's, a device aggregate each */
	uint64	   *strings;		/* per chunk column: a batch's string bytes */

	/*
	 * V8e: the scan's units, read and decoded by the device itself (below):
	 * per chunk column, its units' column -- the scan's spec's -- and its
	 * checks; the units gathered for the next request, the request in
	 * flight, those answered and not yet covered by an answer, and those to
	 * be read here, the device having lost or refused them
	 */
	bool		units;
	const char *units_why;		/* why not, for EXPLAIN */
	VexecNode  *scan;			/* the VecScan whose units they are */
	int		   *unit_col;
	uint8	   *unit_flags;		/* the program's columns', then the filter's */
	int16	   *unit_attnums;	/* W7: and their attributes of the relation */
	int		   *read_cols;		/* each column read's spec column: unit_col's, then qual_unit_col's */
	bool	   *key_numeric;	/* W10: per key, extract()'s numeric the device
								 * computes as INT64 */
	char	   *exprs;			/* W10: the computed columns' expressions, */
	Size		exprs_len;		/* one after another 8-aligned */

	/*
	 * W1: H3's two stages the device's whole, this node their upper one: the
	 * distinct count of its lower stage's scan's units, read by this node,
	 * its answer the groups' final counts; any unit it does not take, and the
	 * input is read again from its start through the lower stage, on the CPU
	 */
	bool		whole;
	bool		mixed;			/* W19: the other aggregates in the whole grouping too */
	int		   *whole_incol;	/* W1, W19: per program column, the lower stage's scan's output column, 1-based */

	/*
	 * W12: a device aggregate of a text's minimum or maximum, whose state
	 * holds a pointer, which a group packed to a partition cannot keep as
	 * it lies: the device answers once, at the input's end, and where its
	 * groups would go packed -- the table full, a chunk refused, the device
	 * lost -- the input is read again from its start on the CPU, as W1's
	 * whole stages' is
	 */
	bool		value_states;	/* states of a value packed after a group:
								 * W12's text, a numeric sum's overflow */

	/*
	 * W4: the scan's quals as the units' filter (§3.26): its bytes, and its
	 * own columns, read past the program's -- their units' columns and types
	 */
	char	   *filter;
	Size		filter_len;
	int			nqcols;
	int		   *qual_unit_col;
	VexecDevColType *qtypes;
	List	   *ufill;			/* VexecDevUnitDesc * */
	uint64		ufill_rows;
	uint64		ufill_bytes;	/* W38: their columns' bytes, about, */
	uint64		request_bytes;	/* and what a request's are gathered within */
	double		unit_row_bytes; /* a row's read columns' widths */
	List	   *uflight;
	List	   *ukept;
	List	   *uredo;
	const char *units_refused;	/* the executor's first refusal of units */

	/* EXPLAIN ANALYZE's */
	int64		units_read;		/* by the device */
	int64		unit_rows;
	int64		cols_cached;	/* W5: the units' columns the executor kept */
	int64		cols_asked;		/* from before, of those asked for */
	int64		units_redone;	/* read here for it */
	int64		batches_cpu;
	int64		rows_cpu;
	int64		groups_merged;
	int64		groups_passed;
	int64		groups_spilled;
	int64		groups_streamed;	/* W1 */
	int64		flushes;
	int64		redone_chunks;
	int64		redone_rows;
	int64		unkept_chunks;	/* W1: let go before an answer covered them */
	int64		restarts;		/* W1: the input read again on the CPU */
	int64		nomem;
	int64		units_split;	/* W39: units requests halved, the device's
								 * memory short of them */
	VexecDevFigures fig;		/* the ended tasks' */
} AggDevice;

/* A Datum of a device type's value at p, as the argument's type holds it. */
static Datum
dev_datum(int type, const char *p)
{
	switch (type)
	{
		case VEXEC_DEV_BOOL8:
			return BoolGetDatum(*p != 0);
		case VEXEC_DEV_INT8:
			return CharGetDatum(*p);
		case VEXEC_DEV_INT16:
			{
				int16		v;

				memcpy(&v, p, sizeof(v));
				return Int16GetDatum(v);
			}
		case VEXEC_DEV_INT32:
			{
				int32		v;

				memcpy(&v, p, sizeof(v));
				return Int32GetDatum(v);
			}
		case VEXEC_DEV_UINT32:
			{
				uint32		v;

				memcpy(&v, p, sizeof(v));
				return ObjectIdGetDatum(v);
			}
		case VEXEC_DEV_INT64:
			{
				int64		v;

				memcpy(&v, p, sizeof(v));
				return Int64GetDatum(v);
			}
		case VEXEC_DEV_FLOAT32:
			{
				float4		v;

				memcpy(&v, p, sizeof(v));
				return Float4GetDatum(v);
			}
		case VEXEC_DEV_FLOAT64:
			{
				float8		v;

				memcpy(&v, p, sizeof(v));
				return Float8GetDatum(v);
			}
	}
	return (Datum) 0;
}

/* A KEY_BITS key's bits from a device value of its width, as batch_keys() reads them. */
static inline Datum
dev_key_bits(const char *p, int width)
{
	switch (width)
	{
		case 1:
			return (Datum) (uint8) *p;
		case 2:
			{
				uint16		v;

				memcpy(&v, p, 2);
				return (Datum) v;
			}
		case 4:
			{
				uint32		v;

				memcpy(&v, p, 4);
				return (Datum) v;
			}
		default:
			{
				uint64		v;

				memcpy(&v, p, 8);
				return (Datum) v;
			}
	}
}

/*
 * A device answer's key value of a fixed width at p, as the probe holds it:
 * a KEY_BITS key's bits; W10, extract()'s numeric of the device's INT64,
 * made in the current memory context.
 */
static inline Datum
dev_key_value(VexecAggState *s, int key, const char *p, int type)
{
	if (s->dev->key_numeric[key])
	{
		int64		v;

		memcpy(&v, p, sizeof(v));
		return NumericGetDatum(int64_to_numeric(v));
	}
	return dev_key_bits(p, vexec_dev_type_width(type));
}

/*
 * A group of the probe's keys, its states begun, laid out as a packed group
 * (D2) -- its keys' bytes after its states -- for a group the table cannot
 * take: in the current memory context.  A by-reference key of the type's
 * functions (W10's numeric) is packed by its value's bytes, as
 * group_create_packed() packs one.
 */
static char *
dev_record(VexecAggState *s, uint32 hash)
{
	Size		len = s->groupsize;
	Size		off = s->groupsize;
	char	   *rec;
	int			i;

	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];

		if (s->probe[i].isnull)
			continue;
		if (k->kind == KEY_BYTES)
			len += MAXALIGN(VARHDRSZ + s->probe[i].len);
		else if (k->kind == KEY_FMGR && !k->typbyval)
		{
			Size		n;

			probe_numeric(s, &s->probe[i]);
			s->probe[i].value = packed_value(s->probe[i].value, k->typlen, &n);
			len += MAXALIGN(n);
		}
	}
	rec = palloc0(len);
	*(uint32 *) rec = hash;
	*(uint32 *) (rec + sizeof(uint32)) = (uint32) len;
	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecAggProbe *p = &s->probe[i];

		group_keynulls(s, rec)[i] = p->isnull;
		if (p->isnull)
			continue;
		if (k->kind == KEY_BYTES)
		{
			varlena    *vl = (varlena *) (rec + off);

			SET_VARSIZE(vl, VARHDRSZ + p->len);
			memcpy(VARDATA(vl), p->bytes, p->len);
			group_keys(s, rec)[i] = UInt32GetDatum((uint32) off);
			off += MAXALIGN(VARHDRSZ + p->len);
		}
		else if (k->kind == KEY_FMGR && !k->typbyval)
		{
			Size		n = k->typlen == -1 ? VARSIZE_ANY(DatumGetPointer(p->value)) : (Size) k->typlen;

			memcpy(rec + off, DatumGetPointer(p->value), n);
			group_keys(s, rec)[i] = UInt32GetDatum((uint32) off);
			off += MAXALIGN(n);
		}
		else
			group_keys(s, rec)[i] = key_datum(k, p->value);
	}
	for (i = 0; i < s->ntrans; i++)
		vexec_aggtrans_init(&s->trans[i], s->aggstate, group_state(s, rec, &s->trans[i]));
	return rec;
}

/*
 * W40 (pg_vector_executor.md §3.35): a device group's record, its states
 * merged, with each text's minimum or maximum -- and each numeric sum's
 * overflow numeric (§3.36, "Universal?") -- packed after it as F5's
 * pack_group() packs a group's: the value's bytes, its state's Datum then
 * their offset from the state, so that it spills as it lies, and a
 * partition's groups take it back (take_partition_groups()); the value's copy
 * the merge made freed.  The record as it is where no state holds a value.
 */
static char *
dev_record_values(VexecAggState *s, char *rec)
{
	Size		len = *(uint32 *) (rec + sizeof(uint32));
	Size		off = len;
	char	   *r;

	for (int i = 0; i < s->ntrans; i++)
		if (vexec_aggtrans_packs_value(&s->trans[i]))
		{
			const varlena *vl = vexec_aggtrans_state_value(&s->trans[i], group_state(s, rec, &s->trans[i]));

			if (vl != NULL)
				len += MAXALIGN(VARSIZE_ANY(vl));
		}
	if (len == off)
		return rec;
	r = palloc0(len);
	memcpy(r, rec, off);
	*(uint32 *) (r + sizeof(uint32)) = (uint32) len;
	for (int i = 0; i < s->ntrans; i++)
		if (vexec_aggtrans_packs_value(&s->trans[i]))
		{
			VexecAggTrans *t = &s->trans[i];
			const varlena *vl = vexec_aggtrans_state_value(t, group_state(s, rec, t));
			char	   *st = group_state(s, r, t);
			Size		n;

			if (vl == NULL)
				continue;
			n = VARSIZE_ANY(vl);
			memcpy(r + off, vl, n);
			vexec_aggtrans_state_value_at(t, st, (uint32) (r + off - st));
			pfree(unconstify(varlena *, vl));
			off += MAXALIGN(n);
		}
	pfree(rec);
	return r;
}

static void device_setup_units(VexecAggState *s);

/*
 * W1: whether a parallel-aware node lies in the input -- a scan whose
 * participants share its rows, which the node cannot begin again alone --
 * short of a Gather's, which begins its workers again as it rescans.
 */
static bool
parallel_aware_walker(PlanState *ps, void *context)
{
	if (ps == NULL)
		return false;
	if (ps->plan->parallel_aware)
		return true;
	if (IsA(ps, GatherState) || IsA(ps, GatherMergeState))
		return false;
	return planstate_tree_walker(ps, parallel_aware_walker, context);
}

/*
 * W17: the HAVING's column of the scan tuple's attribute (its Vars INDEX_VAR's,
 * as set_customscan_references() made them) as an answer column of the
 * device's: a grouping key's its program column, an aggregate's the device
 * aggregate whose value is its result (vexec_aggtrans_device_rank()); -1 for
 * any other, which keeps the HAVING the node's.
 */
typedef struct HavingColumns
{
	VexecAggState *s;
	AggDevice  *d;
} HavingColumns;

static int
having_column(void *arg, AttrNumber attnum, Oid typid, Oid collid)
{
	HavingColumns *h = arg;
	VexecAggState *s = h->s;
	AggDevice  *d = h->d;
	int			col = attnum - 1;

	(void) typid;
	(void) collid;
	if (col < 0 || col >= s->noutcols)
		return -1;
	if (s->outkind[col] == VEXEC_AGGCOL_KEY)
		return s->outsrc[col];
	if (s->outkind[col] == VEXEC_AGGCOL_AGG)
	{
		int			aggno = s->outsrc[col];
		int			ti = s->aggstate->peragg[aggno].transno;
		int			j = vexec_aggtrans_device_rank(&s->trans[ti], s->aggstate, aggno);

		if (j < 0 || j >= d->trans_n[ti])
			return -1;
		return s->nkeys + d->trans_first[ti] + j;
	}
	return -1;
}

/*
 * Whether the node may take its device strategy as it begins, and its
 * program's parts: the keys' columns, then each input target an aggregate
 * reads, and the device's aggregates of each transition.  Else s->dev_why
 * says why not, for EXPLAIN.
 */
static void
device_setup(VexecAggState *s)
{
	VexecNode  *node = &s->node;
	bool		fast = vexec_precision == VEXEC_PRECISION_FAST;
	const char *why = NULL;
	AggDevice  *d;
	VexecDevAgg tmp[VEXEC_AGGTRANS_DEVICE_MAX];
	VexecDevAgg *aggs;
	int			i;

	/*
	 * W1: H3's upper stage the device's whole, its one transition the
	 * distinct count's -- a count over the lower stage's distinct values,
	 * which the device counts itself from the scan's rows -- of a plain
	 * aggregation too, its child's hashes and states never read
	 */
	bool		whole = s->whole_cols != NIL;
	/* W19 (§3.28): the X1 stages' upper one, the other aggregates beside the distinct count, one grouping */
	bool		mixed = whole && list_length(s->whole_cols) == s->nkeys + 1 + 3 * s->aggstate->numaggs;
	VexecNode  *wscan = NULL;	/* the whole stages' lower stage's scan */

	if (!vexec_device_wanted(s->dev_planned, vexec_enable_gpu_agg, &why))
	{
		s->dev_why = why;
		return;
	}
	if (whole && !mixed && (s->ntrans != 1 || s->trans[0].kind != VEXEC_AGG_COUNT ||
							list_length(s->whole_cols) != s->nkeys + 1))
		why = "a distinct count beside other aggregates, not as one grouping";
	else if (whole && (outerPlanState(s->child) == NULL || !vexec_is_vector_state(outerPlanState(s->child)) ||
					   !vexec_node_batchable((VexecNode *) outerPlanState(s->child)) ||
					   ((VexecNode *) outerPlanState(s->child))->kind != VEXEC_NODE_SCAN))
		why = "its lower stage reads no VecScan";
	else if (!whole && s->strategy != AGG_HASHED && s->strategy != AGG_PLAIN)
		why = "a sorted aggregation";
	else if (!whole && DO_AGGSPLIT_COMBINE(s->split))
		why = "a stage that combines partial states";
	else if (parallel_aware_walker(s->child, NULL))
		why = "a participant's share of the rows: the device takes a grouping one task reads whole";
	else if (s->nextra > 0)
		why = "columns kept from a group's first row";
	else if (s->sorted_planned)
		why = "groups in the keys' order";
	else if (s->topk > 0 && !(s->dev_planned && s->strategy == AGG_HASHED && s->split == AGGSPLIT_SIMPLE))
		why = "the groups of the smallest hashes kept";
	else if (s->nkeys == 0 && s->ntrans == 0)
		why = "no key and no aggregate: nothing for it to compute";
	else if (s->nstats >= 0 && !s->dev_planned)
		why = "aggregates its source's statistics answer";
	else if (s->any_lazy)
		why = "an input PostgreSQL's evaluator computes";
	else if (s->hashes_from_child && !whole)
		why = "its child's hashes";
	if (why != NULL)
	{
		s->dev_why = why;
		return;
	}

	d = palloc0(sizeof(AggDevice));
	if (whole)
		wscan = (VexecNode *) outerPlanState(s->child);
	d->mixed = mixed;
	/* W14: C5's groups the device's whole grouping's, the limit's from its first pages */
	d->limited = s->topk > 0;
	d->types = palloc0_array(VexecDevColType, s->nkeys + s->ninputs);
	d->col_src = palloc0_array(int, s->nkeys + s->ninputs);
	d->col_true = palloc0_array(bool, s->nkeys + s->ninputs);
	d->target_col = palloc_array(int, Max(s->ninputs, 1));
	for (i = 0; i < s->ninputs; i++)
		d->target_col[i] = -1;
	d->whole_incol = palloc0_array(int, s->nkeys + s->ninputs + 1);
	d->trans_first = palloc0_array(int, Max(s->ntrans, 1));
	d->trans_n = palloc0_array(int, Max(s->ntrans, 1));
	aggs = palloc0_array(VexecDevAgg, Max(s->ntrans, 1) * VEXEC_AGGTRANS_DEVICE_MAX);

	/* the keys: integers' bits, or text's bytes */
	d->key_numeric = palloc0_array(bool, Max(s->nkeys, 1));
	for (i = 0; i < s->nkeys && why == NULL; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		const VexecType *type = s->rowbatch->types[k->col];
		int16		scale;
		int			t = vexec_dev_type_of(type, &scale);

		/*
		 * W10: extract()'s numeric of an integer field, of scale 0, its
		 * units' computed column the device's INT64, the group's key the
		 * numeric of it -- ClickBench's Q18 groups by the minute
		 */
		if (!whole && k->kind == KEY_FMGR && type->typid == NUMERICOID && s->child_batches &&
			((VexecNode *) s->child)->kind == VEXEC_NODE_SCAN &&
			vexec_devexpr_int_extract(vexec_scan_output_expr((VexecNode *) s->child, k->col)))
		{
			t = VEXEC_DEV_INT64;
			d->key_numeric[i] = true;
		}
		else if (k->kind == KEY_BYTES ? t != VEXEC_DEV_STRING :
				 (k->kind != KEY_BITS || t == VEXEC_DEV_NONE || t == VEXEC_DEV_STRING ||
				  vexec_dev_type_width(t) != k->typlen))
			why = psprintf("a key of type %s", format_type_be(type->typid));
		d->types[d->ncols].type = (uint8) t;
		if (whole)
			d->whole_incol[d->ncols] = list_nth_int(s->whole_cols, i);
		d->col_src[d->ncols++] = i;
	}

	/* the transitions: their arguments and FILTERs, a chunk column each */
	for (i = 0; i < s->ntrans && why == NULL; i++)
	{
		VexecAggTrans *t = &s->trans[i];
		int			argtype = VEXEC_DEV_NONE;
		int16		scale = 0;
		int			n;
		Oid			argtypid = InvalidOid;

		/*
		 * W1, W19: the whole stages' transition: the distinct count, over the
		 * scan's column of the distinct argument; or, mixed, another
		 * aggregate, its combining transition here over the scan's column of
		 * its argument -- the lower stage's -- not the partial state it reads
		 */
		bool		distinct = whole &&
			(!mixed || list_nth_int(s->whole_cols, s->nkeys + 1 + 3 * t->aggref->aggno + 2) != 0);
		int			argcol = !whole ? 0 : !mixed ? list_nth_int(s->whole_cols, s->nkeys) :
			list_nth_int(s->whole_cols, s->nkeys + 1 + 3 * t->aggref->aggno);

		if (t->nargs > 1)
		{
			why = psprintf("%s of %d arguments", t->kindname, t->nargs);
			break;
		}
		if (mixed && !distinct)
		{
			Expr	   *e = argcol > 0 ? vexec_scan_output_expr(wscan, argcol - 1) : NULL;

			if (argcol > 0 && e == NULL)
			{
				why = "an aggregate's argument its lower stage's scan does not give";
				break;
			}
			if (e != NULL)
			{
				argtypid = exprType((Node *) e);
				argtype = vexec_dev_type_of(vexec_type_make(argtypid, exprTypmod((Node *) e), exprCollation((Node *) e)),
											&scale);
			}
		}
		else if (t->nargs == 1)
		{
			argtypid = node->targets[t->firstarg].type->typid;
			argtype = vexec_dev_type_of(node->targets[t->firstarg].type, &scale);
		}
		n = vexec_aggtrans_device_aggs(t, argtype, fast, tmp);
		if (n < 0)
		{
			why = psprintf("a %s over %s", t->kindname, OidIsValid(argtypid) ? format_type_be(argtypid) : "no argument");
			break;
		}
		/* W1: the count the distinct count, of values equal by their bits or bytes */
		if (distinct)
		{
			if (argtype == VEXEC_DEV_FLOAT32 || argtype == VEXEC_DEV_FLOAT64 || n != 1 ||
				tmp[0].kind != VEXEC_DEV_AGG_COUNT)
			{
				why = psprintf("a distinct count over %s", OidIsValid(argtypid) ? format_type_be(argtypid) : "no argument");
				break;
			}
			tmp[0].kind = VEXEC_DEV_AGG_COUNT_DISTINCT;
		}
		d->trans_first[i] = d->ndev;
		d->trans_n[i] = n;
		for (int j = 0; j < n; j++)
		{
			VexecDevAgg *a = &aggs[d->ndev + j];

			*a = tmp[j];
			/* a decimal's result at its argument's scale: an int8's sum's 0 */
			if (a->result == VEXEC_DEV_DECIMAL64 || a->result == VEXEC_DEV_DECIMAL128)
				a->scale = scale;
			if (a->kind != VEXEC_DEV_AGG_COUNT_ALL)
			{
				int			target = t->firstarg;
				int			col = -1;

				if (whole)
				{
					/* W1, W19: the scan's column, one program column however many transitions read it */
					for (int c = s->nkeys; c < d->ncols; c++)
						if (d->whole_incol[c] == argcol && d->types[c].type == argtype && !d->col_true[c])
							col = c;
					if (col < 0)
					{
						col = d->ncols;
						d->types[d->ncols].type = (uint8) argtype;
						d->types[d->ncols].scale = scale;
						d->whole_incol[d->ncols] = argcol;
						d->col_src[d->ncols++] = -1 - target;
					}
				}
				else
				{
					if (d->target_col[target] < 0)
					{
						d->target_col[target] = d->ncols;
						d->types[d->ncols].type = (uint8) argtype;
						d->types[d->ncols].scale = scale;
						d->col_src[d->ncols++] = -1 - target;
					}
					col = d->target_col[target];
				}
				a->arg = (int16) col;
			}
			if (t->filter >= 0)
			{
				if (d->target_col[t->filter] < 0)
				{
					d->target_col[t->filter] = d->ncols;
					d->types[d->ncols].type = VEXEC_DEV_BOOL8;
					d->col_true[d->ncols] = true;
					d->col_src[d->ncols++] = -1 - t->filter;
				}
				a->filter = (int16) d->target_col[t->filter];
			}
		}
		d->ndev += n;
	}
	if (why != NULL)
	{
		s->dev_why = why;
		return;
	}

	/* the program's parts: it is written as the task begins, with a rank lent since (W2) */
	d->aggs = aggs;
	d->dev_type = palloc0_array(int, Max(d->ndev, 1));
	d->dev_scale = palloc0_array(int16, Max(d->ndev, 1));
	for (i = 0; i < d->ndev; i++)
	{
		d->dev_type[i] = aggs[i].result;
		d->dev_scale[i] = aggs[i].scale;
	}

	/* W1: an input with no parallel-aware node below may be read again from its start */
	d->rescannable = !parallel_aware_walker(s->child, NULL);
	/*
	 * W12: a text's minimum or maximum, its states packed after a group that
	 * spills since W40 (§3.35); and a numeric sum's overflow numeric (§3.36,
	 * "Universal?")
	 */
	for (i = 0; i < s->ntrans; i++)
		d->value_states |= vexec_aggtrans_packs_value(&s->trans[i]);

	/*
	 * W17 (§3.28): the HAVING as the device applies it to its answer's
	 * groups, where its form allows -- and, W46 (§3.35), where its provider
	 * applies it to the answers that ask for it alone
	 */
	if (node->css.ss.ps.plan->qual != NIL && !vexec_device_having_asked())
		d->having_why = "its provider applies a HAVING to each answer";
	else if (node->css.ss.ps.plan->qual != NIL)
	{
		HavingColumns hc = {s, d};
		const char *hwhy = NULL;

		d->having = vexec_devfilter_build(node->css.ss.ps.plan->qual, INDEX_VAR, having_column, &hc,
										  &d->having_len, &hwhy);
		if (d->having == NULL)
			d->having_why = hwhy;
	}

	d->cap = (uint32) Max(vexec_gpu_chunk_rows / VEXEC_BATCH_ROWS, 1) * VEXEC_BATCH_ROWS;
	/* an answer staged whole: the table's memory's worth of groups, or 64 MB */
	d->stage_groups = (int64) (Max(s->memlimit, (Size) 64 << 20) /
							   (Size) (s->nkeys * 24 + d->ndev * 16 + 16));
	/* W1: as many by the answer's own bytes a group, a string's offset and 32 */
	{
		Size		row = (Size) (s->nkeys + d->ndev + 7) / 8 + 1;

		for (i = 0; i < s->nkeys; i++)
			row += d->types[i].type == VEXEC_DEV_STRING ? 4 + 32 : vexec_dev_type_width(d->types[i].type);
		for (i = 0; i < d->ndev; i++)
			row += vexec_dev_type_width(d->dev_type[i]);
		d->stream_groups = (int64) (Max(s->memlimit, (Size) 64 << 20) / row);
	}
	d->vals = palloc0_array(VexecAggDevValue, Max(d->ndev, 1));
	d->strings = palloc0_array(uint64, Max(d->ncols, 1));
	d->stagecxt = AllocSetContextCreate(node->mcxt, "VecAgg device answer", ALLOCSET_DEFAULT_SIZES);
	/* W1: the group a streamed row is made into, and its memory */
	d->sgroup = MemoryContextAllocZero(node->mcxt, s->groupsize);
	d->scxt = CreateExprContext(node->css.ss.ps.state);
	d->whole = whole;
	s->dev = d;
	s->may_freeze = false;
	/* W14: the device's groups final, any of them the limit's: C5's keeping on the CPU off */
	if (d->limited)
		s->topk = 0;
	/*
	 * W9: a plain aggregation the plan gave the device is the device's, its
	 * source's statistics left out: the planner priced their answers for
	 * count, min and max alone (VC13), and ClickBench's Q2 and Q3, sums the
	 * statistics did not answer, read every batch on one CPU -- nor do the
	 * units the device reads leave the scan's own between them
	 */
	s->nstats = -1;
	device_setup_units(s);
	/* W1: the whole stages read the scan's units, or the lower stage takes the device itself */
	if (whole && !d->units)
	{
		s->dev_why = psprintf("its lower stage's scan's units: %s", d->units_why ? d->units_why : "none");
		s->dev = NULL;
	}
	/* W10: a numeric key the device computes from its units alone */
	for (i = 0; i < s->nkeys && s->dev != NULL; i++)
		if (d->key_numeric[i] && !d->units)
		{
			s->dev_why = psprintf("a numeric key the device computes from its units alone: %s",
								  d->units_why ? d->units_why : "none");
			s->dev = NULL;
		}
}

/*
 * W10: an expression over the node's input -- the scan's output, OUTER_VAR's
 * columns -- made one over the scan's relation, each OUTER_VAR the scan's
 * target list computes as it is there
 */
static Node *
outer_to_scan_mutator(Node *node, VexecNode *scan)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == OUTER_VAR && ((Var *) node)->varattno > 0)
	{
		Expr	   *e = vexec_scan_output_expr(scan, ((Var *) node)->varattno - 1);

		return e != NULL ? (Node *) copyObject(e) : node;
	}
	return expression_tree_mutator(node, outer_to_scan_mutator, scan);
}

/*
 * W4: the filter's column of an attribute of the scan's relation: a program
 * column of the same units' column and type, read once, else one of the
 * filter's own past them; -1 where the units do not hold it.
 */
static int
agg_filter_column(void *arg, AttrNumber attnum, Oid typid, Oid collid)
{
	VexecAggState *s = arg;
	AggDevice  *d = s->dev;
	int			uc = vexec_scan_attr_unit_column(d->scan, attnum);
	int			type = vexec_devfilter_type(typid);

	(void) collid;
	if (uc < 0 || type == VEXEC_DEV_NONE)
		return -1;
	for (int c = 0; c < d->ncols; c++)
		if (d->unit_col[c] == uc && d->types[c].type == type)
			return c;
	for (int q = 0; q < d->nqcols; q++)
		if (d->qual_unit_col[q] == uc)
			return d->ncols + q;
	d->qual_unit_col[d->nqcols] = uc;
	d->qtypes[d->nqcols].type = (uint8) type;
	return d->ncols + d->nqcols++;
}

/*
 * V8e: whether the device reads the scan's units itself: the child a VecScan
 * that may hand them (vexec_scan_can_units()), every chunk column a column
 * of the scan as it is -- a key, or an aggregate's argument or FILTER that
 * is the input's column -- and its units' column found; and the scan's
 * quals, where it has any, a filter the device applies to the units' rows
 * (W4, vexec_devfilter_build()), the scan then reading its columns in one
 * step.  W1: H3's whole stages', the lower stage's child, the program's
 * columns those of its output the plan named.
 */
static void
device_setup_units(VexecAggState *s)
{
	VexecNode  *node = &s->node;
	AggDevice  *d = s->dev;
	VexecNode  *scan;
	PlanState  *sps = s->child;
	const char *why = NULL;
	List	   *quals;
	Index		varno;
	bool	   *expr_col;
	StringInfoData exprs;

	if (!vexec_enable_gpu_scan)
		return;
	if (d->whole)
	{
		sps = outerPlanState(s->child);
		if (sps == NULL || !vexec_is_vector_state(sps) || !vexec_node_batchable((VexecNode *) sps) ||
			((VexecNode *) sps)->kind != VEXEC_NODE_SCAN)
		{
			d->units_why = "its lower stage reads no VecScan";
			return;
		}
	}
	else if (!s->child_batches || ((VexecNode *) s->child)->kind != VEXEC_NODE_SCAN)
		return;
	scan = (VexecNode *) sps;
	if (!vexec_scan_can_units(scan, true, &why))
	{
		d->units_why = why;
		return;
	}
	d->scan = scan;
	d->unit_col = palloc_array(int, Max(d->ncols, 1));
	expr_col = palloc0_array(bool, Max(d->ncols, 1));
	for (int c = 0; c < d->ncols; c++)
	{
		int			src = d->col_src[c];
		int			incol = -1;

		if (d->whole)
			incol = d->whole_incol[c] - 1;
		else if (src >= 0)
			incol = s->keys[src].col;
		else
		{
			Expr	   *e = node->targets[-1 - src].expr;

			if (IsA(e, Var) && ((Var *) e)->varno == OUTER_VAR && ((Var *) e)->varattno > 0)
				incol = ((Var *) e)->varattno - 1;
		}
		d->unit_col[c] = incol >= 0 ? vexec_scan_unit_column(scan, incol) : -1;
		expr_col[c] = d->unit_col[c] < 0;
	}

	/*
	 * W10 (§3.27): a key the scan computes, an argument or FILTER the node
	 * does, the device's computed column where its machine makes it -- over
	 * the relation's columns its units read, a program column of the same
	 * attribute or one of the filter's own past them
	 */
	quals = vexec_scan_quals(scan, &varno);
	d->qual_unit_col = palloc_array(int, Max(vexec_scan_unit_ncols(scan), 1));
	d->qtypes = palloc0_array(VexecDevColType, Max(vexec_scan_unit_ncols(scan), 1));
	initStringInfo(&exprs);
	for (int c = 0; c < d->ncols; c++)
	{
		int			src = d->col_src[c];
		Expr	   *e;
		char	   *blob;
		Size		len;
		int			type;

		if (!expr_col[c])
			continue;
		if (d->whole)
			e = vexec_scan_output_expr(scan, d->whole_incol[c] - 1);
		else if (src >= 0)
			e = vexec_scan_output_expr(scan, s->keys[src].col);
		else
			e = (Expr *) outer_to_scan_mutator((Node *) node->targets[-1 - src].expr, scan);
		blob = e != NULL ? vexec_devexpr_build(e, varno, agg_filter_column, s, &type, &len, &why) : NULL;
		if (blob == NULL || type != d->types[c].type)
		{
			d->units_why = psprintf("%s the device's machine does not compute: %s",
									src >= 0 ? "a key" : "an aggregate's argument or FILTER",
									blob == NULL ? (why ? why : "none") : "of another type");
			d->nqcols = 0;
			return;
		}
		while (exprs.len % 8 != 0)
			appendStringInfoChar(&exprs, '\0');
		appendBinaryStringInfo(&exprs, blob, len);
	}
	while (exprs.len % 8 != 0)
		appendStringInfoChar(&exprs, '\0');
	d->exprs = exprs.data;
	d->exprs_len = exprs.len;

	/* W4: the scan's quals, the units' filter */
	if (quals != NIL)
	{
		d->filter = vexec_devfilter_build(quals, varno, agg_filter_column, s, &d->filter_len, &why);
		if (d->filter == NULL)
		{
			d->units_why = psprintf("its scan's quals: %s", why);
			d->nqcols = 0;
			return;
		}
		vexec_scan_units_filtered(scan);
	}
	d->unit_flags = palloc0_array(uint8, Max(d->ncols + d->nqcols, 1));
	for (int c = 0; c < d->ncols; c++)
		if (expr_col[c])
			d->unit_flags[c] |= VEXEC_DEV_UCOL_EXPR;
	/* W7: each column read's attribute, by which the device's cache publishes what it keeps */
	d->unit_attnums = palloc0_array(int16, Max(d->ncols + d->nqcols, 1));
	d->read_cols = palloc_array(int, Max(d->ncols + d->nqcols, 1));
	for (int c = 0; c < d->ncols + d->nqcols; c++)
	{
		d->read_cols[c] = c < d->ncols ? d->unit_col[c] : d->qual_unit_col[c - d->ncols];
		d->unit_attnums[c] = (int16) vexec_scan_unit_attnum(scan, d->read_cols[c]);
	}
	/* W38: a row's bytes as the device decodes it, by its read columns' widths */
	d->unit_row_bytes = vexec_devunit_row_bytes(vexec_scan_relid(scan), d->ncols + d->nqcols, d->unit_attnums);
	d->units = true;
}

/*
 * W45 (§3.35): a group's bytes on the device as the planner held them
 * against its free memory (W42) -- 16 a group and a state, its keys by their
 * widths -- or, of a plan that gives none, ORCA's, 48 a key and a state,
 * three times over: what admission asks for a group, and what W47 holds the
 * device's groups so far against.
 */
static double
device_group_bytes(VexecAggState *s)
{
	if (s->group_bytes > 0)
		return s->group_bytes;
	return (double) (s->nkeys * 16 + s->dev->ndev * 16) * 3;
}

/*
 * GROUPBY's program, as the task begins: the chunks' columns, the keys --
 * the first of them -- the device's aggregates, and the rank a bounded sort
 * above lent (W2).
 */
static void
device_program(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	VexecDevProgram *prog;
	VexecDevGroupBy *gb;
	Size		len;

	if (d->program != NULL)
		pfree(d->program);
	len = sizeof(VexecDevProgram) + VEXEC_DEV_ALIGN(sizeof(VexecDevColType) * d->ncols) +
		vexec_dev_groupby_size(s->nkeys, d->ndev, d->nrank, d->having_len);
	d->program = MemoryContextAllocZero(s->node.mcxt, len);
	d->proglen = len;
	prog = (VexecDevProgram *) d->program;
	prog->ncols = d->ncols;
	prog->flags = vexec_precision == VEXEC_PRECISION_FAST ? VEXEC_DEV_PROG_FAST : 0;
	/* W37: a grouping ranked by its count may be grouped by sort; tests, its hashes cut */
	if (vexec_gpu_sort_rank)
		prog->flags |= VEXEC_DEV_PROG_SORT_RANK |
			((uint32) vexec_debug_gpu_rank_hash_bits << VEXEC_DEV_PROG_HASH_BITS_SHIFT);
	/*
	 * W45 (pg_vector_executor.md §3.35): admission asked for the groups'
	 * bytes as the planner held them against the device's free memory (W42)
	 * -- 16 a group and a state, its keys by their widths -- so that a
	 * grouping the planner put on the device is one the device takes:
	 * ClickBench's Q32 at 100M rows, 288 bytes a group by the rule before
	 * it, 28.8 GB of a 15 GB pool, refused, where W42's 76 planned it
	 * (device_group_bytes()).
	 */
	prog->memory = (uint64) Min(Max(s->numgroups, 1024.0) * device_group_bytes(s), (double) (UINT64CONST(1) << 40));
	prog->admission_wait_ms = VEXEC_DEV_WAIT_DEFAULT;
	memcpy(vexec_dev_program_cols(prog), d->types, sizeof(VexecDevColType) * d->ncols);
	gb = (VexecDevGroupBy *) vexec_dev_program_op(prog);
	gb->nkeys = (uint16) s->nkeys;
	gb->naggs = (uint16) d->ndev;
	for (int i = 0; i < s->nkeys; i++)
		vexec_dev_groupby_keys(gb)[i] = (int16) i;
	memcpy(vexec_dev_groupby_aggs(gb), d->aggs, sizeof(VexecDevAgg) * d->ndev);
	if (d->nrank > 0)
	{
		VexecDevRank *r = vexec_dev_groupby_rank(gb);

		gb->nrank = (uint16) d->nrank;
		gb->rank_flags = d->rank_ties ? VEXEC_DEV_RANK_TIES : 0;
		r->k = d->rank_k;
		memcpy(vexec_dev_rank_keys(r), d->rank, sizeof(VexecDevRankKey) * d->nrank);
	}
	/* W17: the HAVING the device applies before its rank */
	if (d->having_len > 0)
	{
		gb->having_length = (uint32) d->having_len;
		memcpy(vexec_dev_groupby_having(gb), d->having, d->having_len);
	}
}

/* The task begun, with its answer pages; false, the node then on the CPU, where it is not. */
static bool
device_start(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	const char *why;
	int			got;
	MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

	d->started = true;
	device_program(s);
	d->t = vexec_devtask_begin(VEXEC_DEV_OP_GROUPBY, d->program, d->proglen, s->node.mcxt, &why);
	if (d->t == NULL)
	{
		d->lost = true;
		d->why = why;
		MemoryContextSwitchTo(old);
		return false;
	}
	d->device = vexec_device_ready() ? vexec_device_ready()->device_name() : NULL;
	d->device = d->device ? pstrdup(d->device) : "the device";
	MemoryContextSwitchTo(old);
	got = vexec_devtask_claim_answers(d->t, lengthof(d->pages), d->pages);
	d->t->held += got;
	d->npages = got;
	if (got == 0)
	{
		vexec_devtask_end(d->t);
		d->t = NULL;
		d->lost = true;
		d->why = "no block of the arena for its answers";
		return false;
	}

	/* the groups the table cannot take go packed to their partitions, read back as a final stage's are */
	if (s->producer == NULL)
	{
		s->producer = s;
		s->pk_key = palloc_array(int, Max(s->nkeys, 1));
		s->pk_extra = palloc0_array(int, 1);
		s->pk_stateoff = palloc_array(Size, Max(s->ntrans, 1));
		s->pk_trans = palloc_array(VexecAggTrans *, Max(s->ntrans, 1));
		for (int i = 0; i < s->nkeys; i++)
			s->pk_key[i] = i;
		for (int i = 0; i < s->ntrans; i++)
		{
			s->pk_stateoff[i] = s->stateoff + s->trans[i].stateoff;
			s->pk_trans[i] = &s->trans[i];
		}
		s->pk_probe = palloc0_array(VexecAggProbe, Max(s->nkeys, 1));
		s->pk_recs = palloc0_array(char *, VEXEC_BATCH_ROWS);
		s->pk_readcxt = AllocSetContextCreate(s->node.mcxt, "VecAgg packed groups read back",
											  ALLOCSET_DEFAULT_SIZES);
	}
	return true;
}

/*
 * The task's figures so far kept, each counted once: a task whose last
 * answer is read a page at a time outlives the input's end (device_finish()).
 */
static void
device_keep_figures(AggDevice *d)
{
	d->fig.chunks += d->t->fig.chunks;
	d->fig.rows += d->t->fig.rows;
	d->fig.bytes += d->t->fig.bytes;
	d->fig.answers += d->t->fig.answers;
	d->fig.answer_rows += d->t->fig.answer_rows;
	d->fig.answer_bytes += d->t->fig.answer_bytes;
	d->fig.requests += d->t->fig.requests;
	d->fig.wait_ms += d->t->fig.wait_ms;
	d->fig.device_ms += d->t->fig.device_ms;
	d->fig.gpu_ms += d->t->fig.gpu_ms;
	memset(&d->t->fig, 0, sizeof(d->t->fig));
}

/* The task's figures kept, and the task ended: its slot and blocks freed. */
static void
device_end_task(AggDevice *d)
{
	if (d->t == NULL)
		return;
	device_keep_figures(d);
	vexec_devtask_end(d->t);
	d->t = NULL;
}

/* The chunk column c's vector of the batch being read. */
static inline VexecVec *
dev_col_vec(VexecAggState *s, int c)
{
	int			src = s->dev->col_src[c];

	return src >= 0 ? &s->node.in->cols[s->keys[src].col] : s->node.outputs[-1 - src];
}

/*
 * A chunk's rows redone on the CPU, in their order, as the batches they were
 * gathered from would have been: each row's group found -- or, where the
 * table can take no more, a group of the row alone, packed to its partition
 * -- and each transition advanced with the row's argument where its FILTER
 * took the row.
 */
static void
redo_fill(VexecAggState *s, VexecDevFill *f)
{
	AggDevice  *d = s->dev;
	/* the node's memory: a caller's may be the row context, reset below */
	MemoryContext cxt = s->node.mcxt;
	VexecDevCursor *vc = MemoryContextAlloc(cxt, sizeof(VexecDevCursor) * d->ncols);
	VexecDevCursor *oc = MemoryContextAlloc(cxt, sizeof(VexecDevCursor) * d->ncols);
	int32	   *prev = MemoryContextAllocZero(cxt, sizeof(int32) * d->ncols);
	char	  **strbuf = MemoryContextAllocZero(cxt, sizeof(char *) * d->ncols);
	uint32	   *strcap = MemoryContextAllocZero(cxt, sizeof(uint32) * d->ncols);
	Datum	   *colval = MemoryContextAllocZero(cxt, sizeof(Datum) * d->ncols);
	bool	   *colnull = MemoryContextAllocZero(cxt, sizeof(bool) * d->ncols);
	MemoryContext old;

	for (int c = 0; c < d->ncols; c++)
	{
		vexec_devcursor_init(&vc[c], f, &f->cols[c].values);
		vexec_devcursor_init(&oc[c], f, &f->cols[c].offsets);
		if (f->cols[c].type == VEXEC_DEV_STRING)
		{
			char		b[4];

			memcpy(&prev[c], vexec_devcursor_next(&oc[c], sizeof(int32), b), sizeof(int32));
		}
	}
	s->probe = s->probe_slot;
	for (uint32 r = 0; r < f->nrows; r++)
	{
		uint32		hash;
		char	   *g;
		char	   *rec = NULL;

		if ((r & 1023) == 0)
		{
			ResetExprContext(s->tmpcontext);
			CHECK_FOR_INTERRUPTS();
		}
		old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);
		for (int c = 0; c < d->ncols; c++)
		{
			VexecDevFillCol *fc = &f->cols[c];
			char		b[16];
			bool		isnull = vexec_devfill_isnull(f, c, r);
			int			src = d->col_src[c];

			if (fc->type == VEXEC_DEV_STRING)
			{
				int32		next;
				uint32		len;
				const char *p;

				memcpy(&next, vexec_devcursor_next(&oc[c], sizeof(int32), b), sizeof(int32));
				len = (uint32) (next - prev[c]);
				prev[c] = next;
				if (len > strcap[c])
				{
					strcap[c] = Max(len, 64);
					strbuf[c] = strbuf[c] ? repalloc(strbuf[c], strcap[c]) :
						MemoryContextAlloc(s->node.mcxt, strcap[c]);
				}
				p = len > 0 ? vexec_devcursor_next(&vc[c], len, strbuf[c]) : "";
				if (src >= 0)
				{
					s->probe[src].isnull = isnull;
					s->probe[src].bytes = p;
					s->probe[src].len = (int32) len;
				}
				colnull[c] = isnull;
				/* count(x) reads only its NULLs; W12, a text's minimum or maximum its value */
				colval[c] = isnull ? (Datum) 0 : PointerGetDatum(cstring_to_text_with_len(p, (int) len));
				continue;
			}
			{
				const char *p = vexec_devcursor_next(&vc[c], fc->width, b);

				colnull[c] = isnull;
				colval[c] = d->col_true[c] ? BoolGetDatum(*p != 0) : dev_datum(fc->type, p);
				if (src >= 0)
				{
					s->probe[src].isnull = isnull;
					s->probe[src].value = isnull ? (Datum) 0 : dev_key_bits(p, fc->width);
				}
			}
		}
		hash = probes_hash(s->keys, s->probe, s->nkeys);
		g = row_group(s, NULL, -1, hash);
		if (g == NULL)
			g = rec = dev_record(s, hash);
		for (int i = 0; i < s->ntrans; i++)
		{
			VexecAggTrans *t = &s->trans[i];
			Datum		arg = (Datum) 0;
			bool		argnull = false;

			if (d->trans_n[i] == 0)
				continue;
			if (t->filter >= 0)
			{
				int			fc = d->target_col[t->filter];

				if (colnull[fc] || !DatumGetBool(colval[fc]))
					continue;
			}
			if (t->nargs == 1)
			{
				int			ac = d->target_col[t->firstarg];

				arg = colval[ac];
				argnull = colnull[ac];
			}
			vexec_aggtrans_advance(t, s->aggstate, group_state(s, g, t), &arg, &argnull);
		}
		if (rec != NULL)
		{
			/* W40: a text's state, or a numeric sum's overflow, packed after the record, as F5 packs one */
			if (d->value_states)
				rec = dev_record_values(s, rec);
			spill_packed(s, hash, rec);
			d->groups_spilled++;
		}
		MemoryContextSwitchTo(old);
	}
	ResetExprContext(s->tmpcontext);
	for (int c = 0; c < d->ncols; c++)
		if (strbuf[c])
			pfree(strbuf[c]);
	pfree(vc);
	pfree(oc);
	pfree(prev);
	pfree(strbuf);
	pfree(strcap);
	pfree(colval);
	pfree(colnull);
	d->redone_chunks++;
	d->redone_rows += f->nrows;
}

/*
 * The device's answer given up: every chunk it does not cover redone on the
 * CPU, the one being filled too, and the rest of the input there.
 */
static void
device_lose(VexecAggState *s, VexecDevFill *f, int status)
{
	AggDevice  *d = s->dev;
	ListCell   *lc;
	MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

	/* all of it in the node's memory: the lists outlive the row context a caller may be in */
	d->why = psprintf("the device's answer was \"%s\"%s%s", vexec_dev_status_name(status),
					  d->t && d->t->message[0] ? ": " : "", d->t ? d->t->message : "");
	d->lost = true;
	if (d->unkept || d->whole)
	{
		/*
		 * W1: chunks let go before an answer covered them, or H3's whole
		 * stages' counts, which no redo completes: the input read again, on
		 * the CPU -- a text's states merged as any others since W40
		 */
		d->restart = true;
		d->ufill_rows = 0;
		d->ufill_bytes = 0;
	}
	else
	{
		/* V8e: every unit not yet covered by an answer read here, between units */
		d->uredo = list_concat(d->uredo, d->ukept);
		d->uredo = list_concat(d->uredo, d->uflight);
		d->uredo = list_concat(d->uredo, d->ufill);
		d->ukept = d->uflight = d->ufill = NIL;
		d->ufill_rows = 0;
		d->ufill_bytes = 0;
		foreach(lc, d->kept)
			redo_fill(s, lfirst(lc));
		if (f != NULL)
			redo_fill(s, f);
		if (d->fill != NULL)
			redo_fill(s, d->fill);
	}
	foreach(lc, d->kept)
		vexec_devfill_free(lfirst(lc));
	list_free(d->kept);
	d->kept = NIL;
	d->kept_blocks = 0;
	vexec_devfill_free(f);
	vexec_devfill_free(d->fill);
	d->fill = NULL;
	MemoryContextReset(d->stagecxt);
	d->staged = NIL;
	device_end_task(d);
	MemoryContextSwitchTo(old);
}

/* The answer page just read, copied into the node's memory. */
static void
stage_page(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	const VexecDevAnswer *a = vexec_devtask_answer(d->t);
	VexecDevAnswer *aa = (VexecDevAnswer *) a;
	MemoryContext old = MemoryContextSwitchTo(d->stagecxt);
	AggDevPage *pg = palloc0(sizeof(AggDevPage));
	int			ncols = s->nkeys + d->ndev;

	if (a->ncols != ncols)
		elog(ERROR, "vexec: a device's answer of %d columns, of %d", a->ncols, ncols);
	pg->nrows = a->nrows;
	pg->values = palloc0_array(const char *, ncols);
	pg->validity = palloc0_array(const uint64 *, ncols);
	pg->offsets = palloc0_array(const int32 *, ncols);
	for (int i = 0; i < ncols; i++)
	{
		const VexecDevAnsCol *c = &vexec_dev_answer_cols(aa)[i];
		const char *v = vexec_devans_values(d->t, c);
		const uint64 *nv = vexec_devans_validity(d->t, c);
		const int32 *ov = vexec_devans_offsets(d->t, c);

		if (v != NULL)
			pg->values[i] = memcpy(palloc(c->values.length), v, c->values.length);
		if (nv != NULL)
			pg->validity[i] = memcpy(palloc(c->validity.length), nv, c->validity.length);
		if (ov != NULL)
			pg->offsets[i] = memcpy(palloc(c->offsets.length), ov, c->offsets.length);
	}
	d->staged = lappend(d->staged, pg);
	MemoryContextSwitchTo(old);
}

/* Row r's results of a staged page, a device aggregate each, into d->vals. */
static void
dev_values(VexecAggState *s, const AggDevPage *pg, uint32 r)
{
	AggDevice  *d = s->dev;

	for (int j = 0; j < d->ndev; j++)
	{
		int			col = s->nkeys + j;
		VexecAggDevValue *v = &d->vals[j];
		const char *p;

		v->isnull = pg->validity[col] != NULL && !vexec_bit(pg->validity[col], r);
		/*
		 * W12: a text's minimum or maximum, its bytes where the page holds
		 * them -- none where its every value is '', the column's bytes then
		 * none, which read as NULL until W54's check of '' (§3.36)
		 */
		if (!v->isnull && d->dev_type[j] == VEXEC_DEV_STRING)
		{
			v->bytes = pg->values[col] ? pg->values[col] + pg->offsets[col][r] : "";
			v->len = pg->offsets[col][r + 1] - pg->offsets[col][r];
			continue;
		}
		if (v->isnull || pg->values[col] == NULL)
		{
			v->isnull = true;
			v->i64 = 0;
			continue;
		}
		p = pg->values[col] + (Size) r * vexec_dev_type_width(d->dev_type[j]);
		if (d->dev_type[j] == VEXEC_DEV_DECIMAL128)
		{
			memcpy(&v->i128, p, sizeof(int128));
			v->scale = d->dev_scale[j];
		}
		else
		{
			/* a count or a sum as an int64; a minimum as its argument's Datum */
			if (d->dev_type[j] == VEXEC_DEV_INT64)
				memcpy(&v->i64, p, sizeof(int64));
			v->d = dev_datum(d->dev_type[j], p);
		}
	}
}

static void whole_restart(VexecAggState *s, const char *why);

/* A staged page's groups merged into the table, or packed to their partitions. */
static void
merge_page(VexecAggState *s, const AggDevPage *pg)
{
	AggDevice  *d = s->dev;
	MemoryContext old;

	s->probe = s->probe_slot;
	for (uint32 r = 0; r < pg->nrows; r++)
	{
		uint32		hash;
		char	   *g;
		char	   *rec = NULL;

		if ((r & 1023) == 0)
		{
			ResetExprContext(s->tmpcontext);
			CHECK_FOR_INTERRUPTS();
		}
		old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);
		for (int i = 0; i < s->nkeys; i++)
		{
			VexecAggProbe *p = &s->probe[i];
			int			type = d->types[i].type;

			p->isnull = pg->validity[i] != NULL && !vexec_bit(pg->validity[i], r);
			if (p->isnull)
				continue;
			p->scaled = false;	/* F14: the device's numeric made, not scaled */
			if (type == VEXEC_DEV_STRING)
			{
				p->bytes = pg->values[i] ? pg->values[i] + pg->offsets[i][r] : "";
				p->len = pg->offsets[i][r + 1] - pg->offsets[i][r];
			}
			else
				p->value = dev_key_value(s, i, pg->values[i] + (Size) r * vexec_dev_type_width(type), type);
		}
		dev_values(s, pg, r);
		hash = probes_hash(s->keys, s->probe, s->nkeys);
		g = row_group(s, NULL, -1, hash);
		if (g == NULL)
			g = rec = dev_record(s, hash);
		for (int i = 0; i < s->ntrans; i++)
			if (d->trans_n[i] > 0)
				vexec_aggtrans_device_merge(&s->trans[i], s->aggstate,
											group_state(s, g, &s->trans[i]),
											&d->vals[d->trans_first[i]]);
		if (rec != NULL)
		{
			/* W40: a text's state, or a numeric sum's overflow, packed after the record, as F5 packs one, not the input again */
			if (d->value_states)
				rec = dev_record_values(s, rec);
			spill_packed(s, hash, rec);
			d->groups_spilled++;
		}
		else
			d->groups_merged++;
		MemoryContextSwitchTo(old);
	}
	ResetExprContext(s->tmpcontext);
}

/*
 * A staged page's groups made as they are, after the table's groups, none
 * looked up (pass_through()).
 */
static void
pass_page(VexecAggState *s, const AggDevPage *pg)
{
	AggDevice  *d = s->dev;
	MemoryContext old;

	s->probe = s->probe_slot;
	for (uint32 r = 0; r < pg->nrows; r++)
	{
		uint32		hash;
		char	   *g;

		if ((r & 1023) == 0)
		{
			ResetExprContext(s->tmpcontext);
			CHECK_FOR_INTERRUPTS();
		}
		old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);
		for (int i = 0; i < s->nkeys; i++)
		{
			VexecAggProbe *p = &s->probe[i];
			int			type = d->types[i].type;

			p->isnull = pg->validity[i] != NULL && !vexec_bit(pg->validity[i], r);
			if (p->isnull)
				continue;
			p->scaled = false;	/* F14: the device's numeric made, not scaled */
			if (type == VEXEC_DEV_STRING)
			{
				p->bytes = pg->values[i] ? pg->values[i] + pg->offsets[i][r] : "";
				p->len = pg->offsets[i][r + 1] - pg->offsets[i][r];
			}
			else
				p->value = dev_key_value(s, i, pg->values[i] + (Size) r * vexec_dev_type_width(type), type);
		}
		dev_values(s, pg, r);
		hash = probes_hash(s->keys, s->probe, s->nkeys);
		MemoryContextSwitchTo(old);
		g = group_create(s, hash, NULL, -1);
		for (int i = 0; i < s->ntrans; i++)
			if (d->trans_n[i] > 0)
				vexec_aggtrans_device_merge(&s->trans[i], s->aggstate,
											group_state(s, g, &s->trans[i]),
											&d->vals[d->trans_first[i]]);
		d->groups_passed++;
	}
	ResetExprContext(s->tmpcontext);
}

/*
 * W1: the last answer's next group, straight from its staged pages, or NULL
 * once every one is out: the page's row made a group of its own in sgroup
 * -- its keys, its states begun and merged from the row's values as a
 * table's group's would be -- its keys' bytes and its states' memory in
 * scxt's, which the group before it, handed up and done with, gives back.
 * Its hash is computed only for a VecRepartition above, which reads it.
 */
static char *
dev_stream_next(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	ExprContext *saved = s->aggstate->curaggcontext;
	char	   *g = d->sgroup;
	MemoryContext old;
	AggDevPage *pg;
	uint32		r;

	ReScanExprContext(d->scxt);
	for (;;)
	{
		/*
		 * W14: the answer's next page, its rows handed up one by one -- a
		 * device lost once some are up leaves no redo that hands up none of
		 * them twice
		 */
		if (d->spage == NULL && d->more)
		{
			int			status = vexec_devtask_answer_page(d->t, d->npages, d->pages, 0);

			if (status != VEXEC_DEV_OK && status != VEXEC_DEV_MORE)
				ereport(ERROR,
						(errcode(ERRCODE_SYSTEM_ERROR),
						 errmsg("vexec: the device's answer was \"%s\" once some of its groups were handed up",
								vexec_dev_status_name(status)),
						 errdetail("%s", d->t->message)));
			MemoryContextReset(d->stagecxt);
			d->staged = NIL;
			stage_page(s);
			d->more = status == VEXEC_DEV_MORE;
			/* its last page staged: the task over */
			if (!d->more)
				device_end_task(d);
			d->spage = list_head(d->staged);
			d->srow = 0;
		}
		if (d->spage == NULL)
			return NULL;
		pg = lfirst(d->spage);
		if (d->srow < pg->nrows)
			break;
		d->spage = lnext(d->staged, d->spage);
		d->srow = 0;
	}
	r = d->srow++;
	if ((d->groups_streamed & 1023) == 0)
		CHECK_FOR_INTERRUPTS();
	d->groups_streamed++;
	s->total_groups++;

	old = MemoryContextSwitchTo(d->scxt->ecxt_per_tuple_memory);
	memset(g, 0, s->groupsize);
	s->probe = s->probe_slot;
	for (int i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecAggProbe *p = &s->probe[i];
		int			type = d->types[i].type;

		p->isnull = pg->validity[i] != NULL && !vexec_bit(pg->validity[i], r);
		group_keynulls(s, g)[i] = p->isnull;
		if (p->isnull)
			continue;
		if (type == VEXEC_DEV_STRING)
		{
			varlena    *vl;

			p->bytes = pg->values[i] ? pg->values[i] + pg->offsets[i][r] : "";
			p->len = pg->offsets[i][r + 1] - pg->offsets[i][r];
			vl = palloc(VARHDRSZ + p->len);
			SET_VARSIZE(vl, VARHDRSZ + p->len);
			memcpy(VARDATA(vl), p->bytes, p->len);
			group_keys(s, g)[i] = PointerGetDatum(vl);
		}
		else
		{
			p->scaled = false;	/* F14: the device's numeric made, not scaled */
			p->value = dev_key_value(s, i, pg->values[i] + (Size) r * vexec_dev_type_width(type), type);
			group_keys(s, g)[i] = k->kind == KEY_BITS ? key_datum(k, p->value) : p->value;
		}
	}
	if (s->hashes_read || s->ndeal > 0)
		*(uint32 *) g = probes_hash(s->keys, s->probe, s->nkeys);
	dev_values(s, pg, r);
	s->aggstate->curaggcontext = d->scxt;
	for (int i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];

		vexec_aggtrans_init(t, s->aggstate, group_state(s, g, t));
		if (d->trans_n[i] > 0)
			vexec_aggtrans_device_merge(t, s->aggstate, group_state(s, g, t),
										&d->vals[d->trans_first[i]]);
	}
	s->aggstate->curaggcontext = saved;
	MemoryContextSwitchTo(old);
	return g;
}

/*
 * Whether an answer's groups are made as they are, none looked up in the
 * table, where no key can then be made twice where it may not: a partial
 * stage, whose final stage combines every group of a key however many it
 * is handed, while they take no more than the table's memory; or a whole
 * aggregation's one answer, the input's last, with no group in the table
 * before it.  Its device grouped every row, so its answer holds each key
 * once.  A grouping with many groups so costs no lookup a group.
 */
static bool
pass_through(VexecAggState *s)
{
	AggDevice  *d = s->dev;

	if (s->spilling)
		return false;
	if (DO_AGGSPLIT_SKIPFINAL(s->split))
		return (Size) (s->ngroups + d->dev_groups) * s->groupsize <= s->memlimit;
	return d->last_answer && d->flushes == 0 && s->ngroups == 0 && d->batches_cpu == 0;
}

/*
 * W1: whether the input's last answer may yet be handed up straight from
 * its pages: the table holds no group and nothing is spilled -- no answer
 * of the device's before it, no batch of the CPU's and no chunk redone made
 * one -- and the device holds every row since the run began.
 */
static bool
stream_possible(VexecAggState *s)
{
	/* W9: a plain aggregation's one group is merged, its chunks answered as they fill the blocks */
	if (s->strategy == AGG_PLAIN && !s->dev->whole)
		return false;
	return !s->dev->lost && s->ngroups == 0 && !s->spilling && s->parts == NULL &&
		s->pending == NIL;
}

/* W1: the chunks kept for a redo let go, the input's start then their redo. */
static void
device_unkeep(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	ListCell   *lc;

	foreach(lc, d->kept)
		vexec_devfill_free(lfirst(lc));
	d->unkept_chunks += list_length(d->kept);
	list_free(d->kept);
	d->kept = NIL;
	d->kept_blocks = 0;
	d->unkept = true;
}

/*
 * W47 (pg_vector_executor.md §3.35): whether the device's groups so far,
 * past what an answer is staged whole with, are held to the input's end for
 * a HAVING the device applies (W17): asked of the input's only answer
 * (W46), it cuts its groups there to those it keeps, where an earlier
 * answer leaves it to the node over every group merged here -- ClickBench's
 * Q28 at 100M rows answered twice, its 3.2 million groups merged on the CPU,
 * 2.9 million of them spilled, 2.7 s, where its one answer, cut on the
 * device to 77 groups, took 1.0.  So while no answer has been asked and no
 * batch read here, and while W42's bytes of them fit the memory the task's
 * admission reserved and the memory no task has; past it, answered as
 * before.
 */
static bool
having_holds(VexecAggState *s)
{
	AggDevice  *d = s->dev;

	if (d->having == NULL || d->flushes > 0 || d->batches_cpu > 0)
		return false;
	return (double) d->dev_groups * device_group_bytes(s) <=
		(double) ((VexecDevProgram *) d->program)->memory + (double) vexec_device_plan_bytes();
}

/*
 * Whether the device's groups so far are to be answered now, before the
 * input's end: where the chunks they cover would fill half the task's
 * blocks, or they are more than an answer is staged whole with.  W1: where
 * the last answer may yet be handed up from its pages, the chunks of a
 * serial input are let go instead, and a ranked answer is staged at the
 * bound's size, whatever the groups.  W47: and groups a HAVING of the
 * device's cuts held to the input's end, while they fit its memory.
 */
static bool
device_flush_due(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	bool		stream = stream_possible(s);

	/* W1: a distinct count answered once, at the input's end -- a text's minimum or maximum as any since W40 */
	if (d->whole)
		return false;

	if (d->kept_blocks * 2 >= d->t->share)
	{
		if (!stream || !d->rescannable || d->units)
			return true;
		device_unkeep(s);
	}
	if (!stream)
		return d->dev_groups >= d->stage_groups;
	/* W14: an answer read a page at a time is staged no more than a page */
	if (d->nrank > 0 || d->limited || d->dev_groups < d->stream_groups)
		return false;
	if (having_holds(s))
	{
		d->held = true;
		d->held_groups = Max(d->held_groups, d->dev_groups);
		return false;
	}
	return true;
}

static void device_flush(VexecAggState *s);
static void device_take(VexecAggState *s);
static bool device_batch(VexecAggState *s);
static void device_send_units(VexecAggState *s, List *units);

/* ---------------------------------------------------------------------
 * V8e: the scan's units, read by the device itself (§3.25, N1)
 *
 * Where the node's child is a VecScan whose source hands units -- row
 * groups of Parquet files, record batches of Arrow IPC files (vexec_source.h,
 * next_unit()) -- and every chunk column is a column of the scan as it is,
 * the node asks the scan for its units one at a time, in place of its
 * batches.  A device's unit the scan passes over, and the node gathers it
 * into a units request (VEXEC_DEV_REQ_UNITS): the executor reads and
 * decodes its columns from the file and takes them as it takes a chunk the
 * node wrote, into the same table.  A unit the source cannot hand so comes
 * as the CPU's, its batches the scan's next, which the node takes as ever:
 * as chunks, or into its own table.  A request is kept as a chunk is, until
 * an answer covers it; one the executor refuses -- a file changed, a value
 * a check rejects, a type it does not make the program's -- or loses has
 * its units read by the scan again here, between units, whose reader then
 * raises what it raises.
 * ---------------------------------------------------------------------
 */

/* A unit's copy, and its strings, in the node's memory. */
static VexecDevUnitDesc *
unit_copy(VexecAggState *s, const VexecSourceUnit *u)
{
	AggDevice  *d = s->dev;

	VexecDevUnitDesc *x = vexec_devunit_copy(s->node.mcxt, u, d->ncols + d->nqcols, d->read_cols, d->unit_flags);

	vexec_scan_unit_granules(d->scan, x);	/* W34 */
	return x;
}

/* W6: a MERGETREE unit's stream's bytes, from the scan's source, into the device's memory */
static void
agg_unit_bytes(void *arg, const VexecDevUnitDesc *x, int stream, uint64 offset, uint64 length, void *dest)
{
	VexecAggState *s = arg;

	vexec_scan_read_unit_bytes(s->dev->scan, &x->unit, x->streams[stream].source, offset, length, dest);
}

static void
unit_free(VexecDevUnitDesc *x)
{
	vexec_devunit_free(x);
}

/*
 * W1: H3's whole stages given up -- a unit the device did not take, its
 * refusal or its loss: the counts so far are no count of every row -- the
 * input to be read again from its start, through the lower stage, on the CPU
 */
static void
whole_restart(VexecAggState *s, const char *why)
{
	AggDevice  *d = s->dev;

	if (!d->lost)
	{
		d->lost = true;
		d->why = why;
	}
	d->restart = true;
}

/* The units request in flight answered: kept until an answer covers it, or its units read here. */
static void
device_take_units(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	int			status;
	const VexecDevAnswer *a;

	if (d->uflight == NIL)
		return;
	status = vexec_devtask_wait(d->t);
	vexec_devtask_units_done(d->t);
	if (status == VEXEC_DEV_OK)
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		a = vexec_devtask_answer(d->t);
		/* W21: the granules none of whose rows the filter kept, into the scan's condition cache */
		if ((a->flags & VEXEC_DEV_ANSF_GRANULES) && d->filter != NULL)
		{
			const uint64 *words = vexec_dev_answer_granules(unconstify(VexecDevAnswer *, a));
			ListCell   *lc;

			foreach(lc, d->uflight)
			{
				VexecDevUnitDesc *x = lfirst(lc);

				if (x->granule_rows == NULL)
					continue;
				vexec_scan_units_none(d->scan, x, words);
				words += (x->nparts + 63) / 64;
			}
		}
		d->units_read += list_length(d->uflight);
		d->unit_rows += a->nrows;
		d->cols_cached += a->cols_cached;
		d->cols_asked += a->cols_asked;
		d->ukept = list_concat(d->ukept, d->uflight);
		list_free(d->uflight);
		d->uflight = NIL;
		MemoryContextSwitchTo(old);
		d->dev_groups = (int64) a->value;
		if (device_flush_due(s))
			device_flush(s);
		return;
	}
	if (status == VEXEC_DEV_NOMEM && list_length(d->uflight) > 1)
	{
		/*
		 * W39 (pg_vector_executor.md §3.35): the device had no memory for the
		 * request, and holds the task as it was before it, its kept columns
		 * given back: its halves asked for, one after the other, down to a
		 * unit -- only a unit it cannot hold alone the CPU's, or, a whole
		 * grouping's or a text's states', the input's again
		 */
		List	   *flight = d->uflight;
		int			n = list_length(flight);
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);
		List	   *first = list_copy_head(flight, n / 2);
		List	   *rest = list_copy_tail(flight, n / 2);

		MemoryContextSwitchTo(old);
		list_free(flight);
		d->uflight = NIL;
		d->units_split++;
		device_send_units(s, first);
		device_send_units(s, rest);
		device_take_units(s);
		return;
	}
	if ((status == VEXEC_DEV_NOMEM || status == VEXEC_DEV_REFUSED) && d->whole)
	{
		d->nomem++;
		if (d->units_refused == NULL)
			d->units_refused = MemoryContextStrdup(s->node.mcxt, d->t->message);
		d->ukept = list_concat(d->ukept, d->uflight);
		d->uflight = NIL;
		whole_restart(s, "a unit the device refused");
		return;
	}
	if (status == VEXEC_DEV_NOMEM || status == VEXEC_DEV_REFUSED)
	{
		/* these units here; the groups so far answered, as for a chunk refused */
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		d->nomem++;
		d->uredo = list_concat(d->uredo, d->uflight);
		list_free(d->uflight);
		d->uflight = NIL;
		MemoryContextSwitchTo(old);
		if (d->units_refused == NULL)
		{
			old = MemoryContextSwitchTo(s->node.mcxt);
			d->units_refused = pstrdup(d->t->message);
			MemoryContextSwitchTo(old);
		}
		device_flush(s);
		return;
	}
	{
		List	   *flight = d->uflight;

		d->uflight = NIL;
		d->uredo = list_concat(d->uredo, flight);
		device_lose(s, NULL, status);
	}
}

/* W5, W6: the columns the device keeps of MERGETREE units probed, before their request */
static void
device_probe_units(VexecAggState *s, VexecDevUnitDesc **arr, int n)
{
	AggDevice  *d = s->dev;

	vexec_devunits_probe(d->t, d->ncols, d->nqcols, d->unit_flags, d->qtypes, vexec_scan_relid(d->scan),
						 d->unit_attnums, arr, n);
}

/*
 * Units as one request, once the request before it is answered: in halves
 * where the request's area does not hold them, a unit it cannot hold read
 * here.  The list is the request's, or the CPU's.
 */
static void
device_send_units(VexecAggState *s, List *units)
{
	AggDevice  *d = s->dev;
	VexecDevUnitDesc **arr;
	int			n = list_length(units);
	int			i = 0;
	ListCell   *lc;

	if (n == 0)
		return;
	device_take(s);
	if (d->lost || d->t == NULL)
	{
		/* the units here, between units */
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		d->uredo = list_concat(d->uredo, units);
		MemoryContextSwitchTo(old);
		list_free(units);
		return;
	}
	arr = palloc(sizeof(VexecDevUnitDesc *) * n);
	foreach(lc, units)
		arr[i++] = lfirst(lc);
	device_probe_units(s, arr, n);
	if (!vexec_devtask_units(d->t, d->ncols, d->nqcols, d->unit_flags, d->qtypes, vexec_scan_relid(d->scan),
							 d->unit_attnums, d->filter, d->filter_len, d->exprs, d->exprs_len, n, arr, 0,
							 agg_unit_bytes, s, 0))
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		pfree(arr);
		/* more than a request holds: the first half, then the rest */
		if (n > 1)
		{
			List	   *rest = list_copy_tail(units, n / 2);

			units = list_truncate(units, n / 2);
			MemoryContextSwitchTo(old);
			device_send_units(s, units);
			device_send_units(s, rest);
		}
		else
		{
			d->uredo = list_concat(d->uredo, units);
			MemoryContextSwitchTo(old);
			list_free(units);
		}
		return;
	}
	pfree(arr);
	d->uflight = units;
}

/* The units gathered, as one request (device_send_units()). */
static void
device_submit_units(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	List	   *units = d->ufill;

	d->ufill = NIL;
	d->ufill_rows = 0;
	d->ufill_bytes = 0;
	device_send_units(s, units);
}

/* A unit for the device, gathered into the next request. */
static void
device_add_unit(VexecAggState *s, const VexecSourceUnit *u)
{
	AggDevice  *d = s->dev;
	VexecDevUnitDesc *x = unit_copy(s, u);
	uint64		bytes = vexec_devunit_bytes(u, d->unit_row_bytes);
	MemoryContext old;

	/*
	 * W38 (pg_vector_executor.md §3.35): a request's units' columns within a
	 * share of the device's free memory as the request begins -- those
	 * gathered sent before a unit that would take them past it, which begins
	 * the next; a unit past it alone
	 */
	if (d->ufill != NIL && d->ufill_bytes + bytes > d->request_bytes)
		device_submit_units(s);
	if (d->ufill == NIL)
		d->request_bytes = vexec_device_request_bytes();
	old = MemoryContextSwitchTo(s->node.mcxt);
	d->ufill = lappend(d->ufill, x);
	MemoryContextSwitchTo(old);
	d->ufill_rows += u->rows;
	d->ufill_bytes += bytes;
	/*
	 * Units of up to 32 chunks' rows a request: the executor reads its files
	 * at once where their schemas are one, a call's own cost each file's
	 * otherwise -- a table of its own files of 13 parts read in 38 ms so,
	 * where a request a part took 173 (CH8)
	 */
	if (d->ufill_rows >= (uint64) d->cap * 32 || list_length(d->ufill) >= 256)
		device_submit_units(s);
}

/* The input's batches until they end: a unit's, as the CPU unit's or one read here. */
static void
agg_take_batches(VexecAggState *s)
{
	AggDevice  *d = s->dev;

	while (vexec_node_next_input(&s->node))
	{
		MemoryContext old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);

		if (d->lost || !device_batch(s))
		{
			/* W1: the input to be read again from its start, this batch with it */
			if (d->restart)
			{
				MemoryContextSwitchTo(old);
				ResetExprContext(s->tmpcontext);
				break;
			}
			d->batches_cpu++;
			d->rows_cpu += vexec_bits_count(s->node.candidates, s->node.in->nrows);
			agg_batch(s);
		}
		MemoryContextSwitchTo(old);
		ResetExprContext(s->tmpcontext);
	}
}

/* The units the device lost or refused, read here by the scan, between units. */
static void
device_redo_units(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	VexecNode  *scan = d->scan;

	if (d->whole && d->uredo != NIL)
	{
		whole_restart(s, "units the device lost");
		return;
	}
	while (d->uredo != NIL)
	{
		VexecDevUnitDesc *x = linitial(d->uredo);

		d->uredo = list_delete_first(d->uredo);
		vexec_scan_read_unit(scan, &x->unit);
		d->units_redone++;
		agg_take_batches(s);
		unit_free(x);
	}
}

/* The first pass's input as the scan's units, the device reading its own. */
static void
agg_fill_units(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	VexecNode  *scan = d->scan;
	VexecSourceUnit unit;

	for (;;)
	{
		device_redo_units(s);
		if (d->restart)
			return;
		if (!vexec_scan_next_unit(scan, &unit))
			break;
		/* W1: the whole stages take every unit on the device, or none */
		if (d->whole && (unit.format == VEXEC_UNIT_CPU || d->lost || d->t == NULL))
		{
			whole_restart(s, unit.format == VEXEC_UNIT_CPU ? "a unit its source handed the CPU" :
						  "the device lost");
			return;
		}
		if (unit.format == VEXEC_UNIT_CPU)
		{
			agg_take_batches(s);
			continue;
		}
		if (d->lost || d->t == NULL)
		{
			/* a unit the device does not take now: read here */
			vexec_scan_read_unit(scan, &unit);
			d->units_redone++;
			agg_take_batches(s);
			continue;
		}
		device_add_unit(s, &unit);
	}
	device_submit_units(s);
	device_take(s);
	device_redo_units(s);
}

/*
 * The device's groups so far, asked for a page at a time, staged until the
 * answer is whole, then merged; the chunks they cover released.  W1: the
 * input's last answer, where the device grouped every row, kept staged, to
 * be handed up from its pages (dev_stream_next()); ranked, where a bounded
 * sort above lent its rank (W2).
 */
static void
device_flush(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	ListCell   *lc;
	bool		stream;
	bool		only;
	uint32		flags = 0;

	/* nothing the device holds: no chunk or unit since the last answer, kept or let go */
	if (d->lost || (d->kept == NIL && d->ukept == NIL && !d->unkept))
		return;
	/* a plain aggregation's one group merged into its own (W1's whole stages) */
	stream = d->last_answer && stream_possible(s) && s->strategy == AGG_HASHED;

	/*
	 * W46 (§3.35): the device's HAVING asked of the input's only answer --
	 * the last, none before it, no batch read here, so that its groups hold
	 * every row of theirs -- and its rank, where it has a HAVING, then alone:
	 * an earlier answer's groups are a part of theirs, and so are the last's
	 * after one, a HAVING of theirs the node's, once merged.  W17 asked it of
	 * each: ClickBench's Q28 at 100M rows lost a group's rows wherever an
	 * answer held 100,000 of them or fewer.
	 */
	only = stream && d->flushes == 0 && d->batches_cpu == 0;
	if (d->having != NULL && only)
		flags |= VEXEC_DEV_PAGES_HAVING;
	else if (d->having != NULL)
		d->having_left = true;
	if (stream && d->nrank > 0 && (d->having == NULL || only))
		flags |= VEXEC_DEV_PAGES_RANK;
	MemoryContextReset(d->stagecxt);
	d->staged = NIL;
	for (;;)
	{
		int			status = vexec_devtask_answer_page(d->t, d->npages, d->pages, flags);

		if (status != VEXEC_DEV_OK && status != VEXEC_DEV_MORE)
		{
			device_lose(s, NULL, status);
			return;
		}
		if (d->staged == NIL)
		{
			d->groups_before_rank = (int64) vexec_devtask_answer(d->t)->value;
			d->rank_sorted = (vexec_devtask_answer(d->t)->flags & VEXEC_DEV_ANSF_SORTED) != 0;
		}
		stage_page(s);
		/*
		 * W14: the limit's groups handed up from the first pages, read as they
		 * go; W47, so too the groups held for the device's HAVING where those
		 * it kept are still more than an answer is staged whole with
		 */
		d->more = status == VEXEC_DEV_MORE && stream &&
			(d->limited || (d->held && d->groups_before_rank > d->stream_groups));
		if (d->more && !d->limited)
			d->paged = true;
		if (status == VEXEC_DEV_OK || d->more)
			break;
	}
	if (stream)
	{
		d->stream = true;
		d->ranked = (flags & VEXEC_DEV_PAGES_RANK) != 0;
		d->spage = list_head(d->staged);
		d->srow = 0;
	}
	else
	{
		bool		pass = s->strategy == AGG_HASHED && pass_through(s);

		/* W23: an empty table made for the answer's groups, not the planner's estimate's */
		if (s->strategy == AGG_HASHED && !pass && s->table == NULL && !s->sorted)
		{
			s->table_hint = (uint64) s->ngroups;
			foreach(lc, d->staged)
				s->table_hint += ((const AggDevPage *) lfirst(lc))->nrows;
			if (s->table_hint > 0)
				table_make(s);
			s->table_hint = 0;
		}
		foreach(lc, d->staged)
		{
			if (pass)
				pass_page(s, lfirst(lc));
			else
				merge_page(s, lfirst(lc));
			/* W12: a text's state the table could not take, the input to be read again */
			if (d->restart)
				break;
		}
		MemoryContextReset(d->stagecxt);
		d->staged = NIL;
	}
	foreach(lc, d->kept)
		vexec_devfill_free(lfirst(lc));
	list_free(d->kept);
	d->kept = NIL;
	d->kept_blocks = 0;
	foreach(lc, d->ukept)
		unit_free(lfirst(lc));
	list_free(d->ukept);
	d->ukept = NIL;
	d->unkept = false;			/* the chunks let go covered too */
	d->dev_groups = 0;
	d->flushes++;
}

/*
 * The chunk in flight answered: kept until an answer covers it; one the
 * device had no room or memory for, redone on the CPU and the device's
 * groups so far answered; anything else, the device given up.
 */
static void
device_take(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	VexecDevFill *f;
	int			status;

	device_take_units(s);
	f = d->flight;
	if (f == NULL)
		return;
	d->flight = NULL;
	status = vexec_devtask_wait(d->t);
	if (status == VEXEC_DEV_OK)
	{
		/* the node's memory: a batch's is reset as the batch ends */
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		d->kept = lappend(d->kept, f);
		MemoryContextSwitchTo(old);
		d->kept_blocks += f->nblocks;
		d->dev_groups = (int64) vexec_devtask_answer(d->t)->value;
		if (device_flush_due(s))
			device_flush(s);
		return;
	}
	if (status == VEXEC_DEV_NOMEM || status == VEXEC_DEV_REFUSED)
	{
		/* a text's states too since W40: the groups so far answered, a spilled one's packed */
		d->nomem++;
		redo_fill(s, f);
		vexec_devfill_free(f);
		device_flush(s);
		return;
	}
	device_lose(s, f, status);
}

/* The chunk being filled to the device, once the one before it is answered. */
static void
device_submit(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	VexecDevFill *f = d->fill;

	d->fill = NULL;
	device_take(s);
	if (d->lost)
	{
		if (f != NULL)
		{
			/* W12: an input to be read again from its start has none of its chunks redone */
			if (!d->restart)
				redo_fill(s, f);
			vexec_devfill_free(f);
		}
		return;
	}
	if (f == NULL)
		return;
	if (f->nrows == 0)
	{
		vexec_devfill_free(f);
		return;
	}
	vexec_devfill_submit(f);
	d->flight = f;
}

/* A new chunk to fill: its blocks claimed, an answer asked for first where they are out. */
static VexecDevFill *
device_new_fill(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	VexecDevFill *f;

	f = vexec_devfill_begin(d->t, d->ncols, d->types, d->cap, 0);
	if (f == NULL && (d->kept != NIL || d->flight != NULL))
	{
		device_take(s);
		/* W1: the kept chunks let go rather than answered, where the last answer may stream */
		if (!d->lost && stream_possible(s) && d->rescannable && !d->units)
			device_unkeep(s);
		else
			device_flush(s);
		if (!d->lost)
			f = vexec_devfill_begin(d->t, d->ncols, d->types, d->cap, 0);
	}
	return f;
}

/*
 * The batch's selected rows into the chunk being filled: false where the
 * batch is the CPU strategy's.
 */
static bool
device_batch(VexecAggState *s)
{
	VexecNode  *node = &s->node;
	AggDevice  *d = s->dev;
	VexecBatch *in = node->in;
	int			n = in->nrows;
	const uint64 *sel = node->candidates;
	int			k;

	if (vexec_bits_any(node->redo, n) ||
		(node->child_redo != NULL && vexec_bits_any(node->child_redo, n)))
		return false;
	k = vexec_bits_count(sel, n);
	if (k == 0)
		return true;
	if (d->fill == NULL && (d->fill = device_new_fill(s)) == NULL)
		return false;
	for (int c = 0; c < d->ncols; c++)
	{
		VexecVec   *v = dev_col_vec(s, c);

		if (d->col_true[c] ? (v->shape.layout != VEXEC_BIT_BOOL && v->shape.layout != VEXEC_BYTE_BOOL) ||
			v->encoding == VEXEC_DICT : !vexec_devfill_can_append(d->fill, c, v))
			return false;
		d->strings[c] = d->types[c].type == VEXEC_DEV_STRING ? vexec_devfill_string_bytes(v, sel, n) : 0;
	}
	if (!vexec_devfill_room(d->fill, k, d->strings))
	{
		device_submit(s);
		if (d->lost)
			return false;
		d->fill = device_new_fill(s);
		if (d->fill == NULL || !vexec_devfill_room(d->fill, k, d->strings))
			return false;
	}
	for (int c = 0; c < d->ncols; c++)
	{
		VexecVec   *v = dev_col_vec(s, c);
		bool		ok = d->col_true[c] ?
			vexec_devfill_append_true(d->fill, c, v, sel, n, k) :
			vexec_devfill_append(d->fill, c, node->work, v, sel, n, k);

		if (!ok)
			elog(ERROR, "vexec: a device chunk's column %d refused a layout it was checked for", c);
	}
	vexec_devfill_rows(d->fill, k);
	if (d->fill->nrows + VEXEC_BATCH_ROWS > d->fill->cap)
		device_submit(s);
	return true;
}

/*
 * W1: the device's answer lost after the chunks for its redo were let go:
 * the table emptied, and the input read again from its start, every batch
 * on the CPU.
 */
static void
device_restart(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	ListCell   *lc;

	d->restart = false;
	d->restarts++;
	foreach(lc, d->kept)
		vexec_devfill_free(lfirst(lc));
	list_free(d->kept);
	d->kept = NIL;
	d->kept_blocks = 0;
	foreach(lc, d->ufill)
		unit_free(lfirst(lc));
	foreach(lc, d->uflight)
		unit_free(lfirst(lc));
	foreach(lc, d->ukept)
		unit_free(lfirst(lc));
	foreach(lc, d->uredo)
		unit_free(lfirst(lc));
	d->ufill = d->uflight = d->ukept = d->uredo = NIL;
	d->ufill_rows = 0;
	d->ufill_bytes = 0;
	MemoryContextReset(d->stagecxt);
	d->staged = NIL;
	d->stream = false;
	spill_close_all(s);
	table_reset(s);
	s->child_done = false;
	ExecReScan(s->child);
	vexec_node_rescan(&s->node);
	agg_take_batches(s);
}

/* The input's end: the last chunk submitted, every chunk answered and merged, the task ended. */
static void
device_finish(VexecAggState *s)
{
	AggDevice  *d = s->dev;

	if (!d->lost && d->t != NULL)
	{
		device_submit(s);
		d->last_answer = true;
		device_take(s);
		device_flush(s);
		d->last_answer = false;
	}

	/*
	 * W14, W47 (§3.35): an answer read a page at a time keeps its task until
	 * its last page is read (dev_stream_next()) or the node is reset, its
	 * figures so far kept: ended here, its next page was asked of no task --
	 * a LIMIT's 100,000 groups of 200-byte texts past their first page ended
	 * the process
	 */
	if (d->stream && d->more)
		device_keep_figures(d);
	else
		device_end_task(d);
}

/* The device's state for a run again, or the node's end: the task over. */
static void
device_reset(VexecAggState *s)
{
	AggDevice  *d = s->dev;
	ListCell   *lc;

	if (d == NULL)
		return;
	foreach(lc, d->kept)
		vexec_devfill_free(lfirst(lc));
	list_free(d->kept);
	d->kept = NIL;
	d->kept_blocks = 0;
	vexec_devfill_free(d->fill);
	d->fill = NULL;
	/* a request in flight is the provider's to cancel as the task ends */
	if (d->flight != NULL)
	{
		d->flight->nblocks = 0;
		vexec_devfill_free(d->flight);
		d->flight = NULL;
	}
	device_end_task(d);
	MemoryContextReset(d->stagecxt);
	d->staged = NIL;
	d->started = false;
	d->lost = false;
	d->dev_groups = 0;
	d->stream = false;
	d->spage = NULL;
	d->unkept = false;
	d->restart = false;
	d->ranked = false;
	d->more = false;
	d->held = false;
	foreach(lc, d->ufill)
		unit_free(lfirst(lc));
	foreach(lc, d->uflight)
		unit_free(lfirst(lc));
	foreach(lc, d->ukept)
		unit_free(lfirst(lc));
	foreach(lc, d->uredo)
		unit_free(lfirst(lc));
	d->ufill = d->uflight = d->ukept = d->uredo = NIL;
	d->ufill_rows = 0;
	d->ufill_bytes = 0;
}

/* The first pass's input, its batches to the device. */
static void
agg_fill_device(VexecAggState *s)
{
	AggDevice  *d = s->dev;

	if (!d->started)
		(void) device_start(s);
	/* the node runs, whether or not a batch comes through it */
	s->node.ran = true;
	if (d->units && !d->lost)
		agg_fill_units(s);
	else
		agg_take_batches(s);
	device_finish(s);
	/* W1: the device's answer lost with no chunk kept for its redo */
	if (d->restart)
		device_restart(s);
	/* V8e: what the last answer's loss gave to be read here */
	if (d->uredo != NIL)
		device_redo_units(s);
	spill_finish(s);
	s->filled = true;
	vexec_aggsize_put(s->size_key, s->ngroups);
	/* a plain aggregation's, W1's whole stages', has no table */
	if (s->table != NULL)
		s->buckets_final = Max(s->buckets_final, s->table->size);
}

/* A partition's packed groups, the device's the table could not take, into the table. */
static void
take_partition_groups(VexecAggState *s)
{
	BufFile    *f = s->reading->pfile;

	for (;;)
	{
		int			n;

		MemoryContextReset(s->pk_readcxt);
		for (n = 0; n < vexec_batch_rows; n++)
		{
			uint32		head[2];
			char	   *rec;

			if (BufFileReadMaybeEOF(f, head, sizeof(head), true) == 0)
				break;
			rec = MemoryContextAllocHuge(s->pk_readcxt, head[1]);
			memcpy(rec, head, sizeof(head));
			BufFileReadExact(f, rec + sizeof(head), head[1] - sizeof(head));
			s->pk_recs[n] = rec;
		}
		if (n == 0)
			break;
		take_packed(s, n);
	}
	MemoryContextReset(s->pk_readcxt);
	BufFileClose(f);
	s->reading->pfile = NULL;
}

/*
 * Every input row -- the child's, or a partition's -- into the table.  A
 * batch is aggregated in the row context, which each row resets, as
 * nodeAgg.c's tmpcontext: what a key's or an argument's detoasting, or a
 * hash, equality or transition function, allocates is freed as it goes.
 */
static void agg_fill_packed(VexecAggState *s);
static char *next_group(VexecAggState *s);

/*
 * The first stage bypassed (above), decided at a batch's end: for the next
 * VEXEC_AGG_BYPASS_BATCHES batches where its frozen table's rows made
 * groups of their own; looked up again after them.  Packed groups only,
 * which go where they are made; never once the table thaws.
 */
static void
bypass_decide(VexecAggState *s)
{
	if (s->bypass)
	{
		if (--s->bypass_left <= 0 || !s->frozen)
			s->bypass = false;
		return;
	}
	/* F5: a group a row is made where it is handed on, which a text's state cannot be */
	if (!s->frozen || s->by_rows < VEXEC_AGG_BYPASS_EVIDENCE || !vexec_partial_agg_bypass || s->pack_values)
		return;
	if (s->by_groups >= (s->keys_runs && vexec_enable_bypass_over_runs ?
						 VEXEC_AGG_BYPASS_RUN_SHARE : VEXEC_AGG_BYPASS_SHARE) * s->by_rows)
	{
		s->bypass = true;
		s->bypass_left = VEXEC_AGG_BYPASS_BATCHES;
	}
	s->by_rows = 0;
	s->by_groups = 0;
}

static void
agg_fill(VexecAggState *s)
{
	/* D2: the child's packed groups, where this run's partitions hold them */
	if (s->unpack_ok && s->reading == NULL && s->depth == 0)
		s->unpacking = vexec_repart_packed(s->node.vec_child);
	if (s->unpacking)
	{
		agg_fill_packed(s);
		return;
	}
	/* V8b: a partition's groups the device's answers could not put in the table, first */
	if (s->reading != NULL && s->reading->pfile != NULL)
		take_partition_groups(s);
	/* V8b: the first pass's batches to the device */
	if (s->dev != NULL && s->reading == NULL && s->depth == 0)
	{
		agg_fill_device(s);
		return;
	}
	while (vexec_node_next_input(&s->node))
	{
		MemoryContext old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);

		agg_batch(s);
		MemoryContextSwitchTo(old);

		/*
		 * D2: a frozen table's batch, its groups packed in their partitions
		 * already -- the batch table's, or the bypassed rows' -- handed on
		 * as the batch ends; the table thaws where they repeat their keys
		 */
		if (s->frozen && s->sink != NULL)
		{
			if (s->passed_since >= s->thaw_next)
				maybe_thaw(s);
			if (s->btable->members > 0)
			{
				/* F5: a text's minimum or maximum among the states: each group packed by a copy */
				if (s->pack_values)
					pack_batch_table(s);
				batch_table_reset(s);
			}
			vexec_pack_batch_end(s->sink);
			bypass_decide(s);
			continue;
		}

		/*
		 * C2: the batch table's groups go on before the next batch; the
		 * table thaws where they repeat their keys
		 */
		if (s->frozen && s->btable->members > 0)
		{
			if (s->passed_since >= s->thaw_next)
				maybe_thaw(s);
			s->boutput = true;
			s->biterating = false;
			return;
		}
	}
	if (s->reading != NULL)
	{
		if (s->reading->file != NULL)
			BufFileClose(s->reading->file);
		s->reading->file = NULL;
		s->reading = NULL;
	}
	spill_finish(s);
	s->filled = true;

	/* C6: the groups the table held, for the next run to begin with */
	if (s->strategy == AGG_HASHED)
	{
		vexec_aggsize_put(s->size_key, s->ngroups);
		if (s->table != NULL)
			s->buckets_final = Max(s->buckets_final, s->table->size);
	}
}

/* ---------------------------------------------------------------------
 * D2: packed groups taken (above)
 * ---------------------------------------------------------------------
 */

/* A packed group of the producer's: its keys, as this stage looks them up. */
static void
unpack_probe(VexecAggState *s, const char *rec)
{
	VexecAggState *p = s->producer;
	int			i;

	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecAggProbe *pr = &s->pk_probe[i];
		int			j = s->pk_key[i];
		Datum		d = group_keys(p, (char *) rec)[j];

		pr->isnull = group_keynulls(p, (char *) rec)[j];
		pr->scaled = false;
		if (pr->isnull)
			continue;
		if (k->kind == KEY_BYTES)
		{
			const varlena *vl = (const varlena *) DatumGetPointer(packed_ref(rec, d));

			pr->bytes = VARDATA_ANY(vl);
			pr->len = VARSIZE_ANY_EXHDR(vl);
		}
		else if (k->kind == KEY_BITS)
			pr->value = key_bits(d, k->typlen);
		else if (group_keyform(p, rec, j) != 0)
		{
			/* F14: the producer's group keeps it as its scaled integer */
			pr->value = d;
			pr->scaled = true;
			pr->scale = group_keyform(p, rec, j) - 1;
		}
		else
			pr->value = k->typbyval ? d : packed_ref(rec, d);
	}
	s->probe = s->pk_probe;
}

/* A packed group to the partition the hash's next bits choose, as it lies. */
static void
spill_packed(VexecAggState *s, uint32 hash, const char *rec)
{
	int			shift = 32 - VEXEC_AGG_PARTITION_BITS * (s->depth + 1);
	VexecAggPartition *part;
	BufFile   **fp;

	if (s->parts == NULL)
		spill_begin(s);
	part = &s->parts[(hash >> shift) & (VEXEC_AGG_PARTITIONS - 1)];

	/*
	 * A final stage's input is packed groups: its partitions hold them. A
	 * first stage's device strategy keeps its rows' partitions and, beside
	 * each, one of the groups its answers could not put in the table (V8b).
	 */
	fp = s->unpacking ? &part->file : &part->pfile;
	if (*fp == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		*fp = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
	}
	BufFileWrite(*fp, rec, *(const uint32 *) (rec + sizeof(uint32)));
	part->rows++;
	s->spilled_rows++;
}

/*
 * The packed groups of pk_recs[0 .. n) into the table: each one's group
 * found or made, its keys and extras taken where they lie, its states
 * merged into the group's (aggtrans.c); past the table's memory, a group
 * not in it goes to a partition as it is.  C5's groups past the boundary
 * are passed over, as their rows would be.
 */
static void
take_packed(VexecAggState *s, int n)
{
	MemoryContext old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);
	uint32		bound = s->topk_skip ? topk_bound(s) : PG_UINT32_MAX;
	int			i,
				j;

	for (j = 0; j < n; j++)
	{
		const char *rec = s->pk_recs[j];
		uint32		hash;
		char	   *g;

		if (s->pk_hashes_on)
		{
			/*
			 * F29: references into the exchange's chunks, the records spread
			 * over them -- each fetched ahead, its bucket after it -- where a
			 * sub-buffer's copies lay one after another
			 */
			hash = s->pk_hashes[j];
			if (j + 16 < n)
			{
				__builtin_prefetch(s->pk_recs[j + 16]);
				__builtin_prefetch(s->pk_recs[j + 16] + 64);
			}
			if (j + 8 < n && s->table != NULL)
				__builtin_prefetch(&s->table->data[s->pk_hashes[j + 8] & s->table->sizemask]);
		}
		else
		{
			hash = *(const uint32 *) rec;
			if (j + 8 < n && s->table != NULL)
				__builtin_prefetch(&s->table->data[*(const uint32 *) s->pk_recs[j + 8] &
												   s->table->sizemask]);
		}
		if (hash > bound)
		{
			s->rows_skipped++;
			continue;
		}
		unpack_probe(s, rec);
		s->pack_rec = rec;
		g = row_group(s, NULL, -1, hash);
		s->pack_rec = NULL;
		if (g == NULL)
		{
			spill_packed(s, hash, rec);
			continue;
		}
		if (s->pack_taken)
		{
			/* F29: the record is the group, its states its own */
			s->pack_taken = false;
			continue;
		}
		for (i = 0; i < s->ntrans; i++)
			vexec_aggtrans_merge(&s->trans[i], s->aggstate, group_state(s, g, &s->trans[i]),
								 s->pk_trans[i], rec + s->pk_stateoff[i]);
	}
	s->unpacked_groups += n;
	MemoryContextSwitchTo(old);
	ResetExprContext(s->tmpcontext);
	CHECK_FOR_INTERRUPTS();
}

/*
 * D2: the table filled from packed groups: the chunks of the partitions
 * the VecRepartition below claims, or a partition of this node's own read
 * back; then as agg_fill() ends.
 */
/* ---------------------------------------------------------------------
 * F23: a final stage's sub-buffers (above)
 * ---------------------------------------------------------------------
 */

/* Whether this fill scatters its packed groups into sub-buffers. */
static bool
sub_eligible(VexecAggState *s)
{
	return vexec_enable_final_subpartitions && s->strategy == AGG_HASHED && s->depth == 0 &&
		s->reading == NULL && s->dev == NULL && s->sink == NULL && !s->topk_skip && !s->sorted;
}

static void
sub_begin(VexecAggState *s)
{
	if (s->subcxt == NULL)
		s->subcxt = AllocSetContextCreate(s->node.mcxt, "VecAgg final sub-buffers", ALLOCSET_DEFAULT_SIZES);
	else
		MemoryContextReset(s->subcxt);
	s->subs = MemoryContextAllocZero(s->subcxt, sizeof(VexecAggSub) * VEXEC_AGG_SUBS);
	s->sub_bytes = 0;
}

/* A packed group into its sub-buffer, copied: the chunk it lies in goes at the next */
static void
sub_put(VexecAggState *s, const char *rec)
{
	uint32		hash = *(const uint32 *) rec;
	Size		len = *(const uint32 *) (rec + sizeof(uint32));
	VexecAggSub *sb = &s->subs[hash >> (32 - VEXEC_AGG_SUB_BITS)];
	VexecAggSubChunk *c = sb->last;

	if (c == NULL || c->used + len > c->size)
	{
		Size		size = c == NULL ? VEXEC_AGG_SUB_CHUNK_FIRST : Min(c->size * 2, VEXEC_AGG_SUB_CHUNK_MAX);
		VexecAggSubChunk *n;

		size = Max(size, MAXALIGN(len));
		n = MemoryContextAlloc(s->subcxt, MAXALIGN(sizeof(VexecAggSubChunk)) + size);
		n->next = NULL;
		n->used = 0;
		n->size = size;
		n->data = (char *) n + MAXALIGN(sizeof(VexecAggSubChunk));
		if (c == NULL)
			sb->first = n;
		else
			c->next = n;
		sb->last = n;
		c = n;
		s->sub_bytes += MAXALIGN(sizeof(VexecAggSubChunk)) + size;
	}
	memcpy(c->data + c->used, rec, len);
	c->used += len;
	sb->rows++;
	s->sub_records++;
}

/* F29: a record of a file's, whose memory goes at the next one, copied for its reference */
static const char *
sub_copy(VexecAggState *s, const char *rec, Size len)
{
	char	   *dst;

	if (s->copy_at == NULL || MAXALIGN(len) > s->copy_left)
	{
		Size		size = Max(VEXEC_AGG_COPY_CHUNK, MAXALIGN(len));

		if (s->subcopycxt == NULL)
			s->subcopycxt = AllocSetContextCreate(s->node.mcxt, "VecAgg final records copied",
												  ALLOCSET_DEFAULT_SIZES);
		s->copy_at = MemoryContextAlloc(s->subcopycxt, size);
		s->copy_left = size;
		s->sub_bytes += size;
	}
	dst = s->copy_at;
	memcpy(dst, rec, len);
	s->copy_at += MAXALIGN(len);
	s->copy_left -= MAXALIGN(len);
	s->sub_copied++;
	return dst;
}

/*
 * F29: a packed group's reference into its sub-buffer -- where it lies, in a
 * chunk the exchange keeps, or its copy where it is a file's record
 */
static void
sub_put_ref(VexecAggState *s, const char *rec, bool kept)
{
	uint32		hash = *(const uint32 *) rec;
	uint32		len = *(const uint32 *) (rec + sizeof(uint32));
	VexecAggSub *sb = &s->subs[hash >> (32 - VEXEC_AGG_SUB_BITS)];
	VexecAggSubChunk *c = sb->last;
	VexecAggSubRef *ref;

	if (c == NULL || c->used + sizeof(VexecAggSubRef) > c->size)
	{
		Size		size = c == NULL ? VEXEC_AGG_SUB_CHUNK_FIRST : Min(c->size * 2, VEXEC_AGG_SUB_CHUNK_MAX);
		VexecAggSubChunk *n;

		n = MemoryContextAlloc(s->subcxt, MAXALIGN(sizeof(VexecAggSubChunk)) + size);
		n->next = NULL;
		n->used = 0;
		n->size = size;
		n->data = (char *) n + MAXALIGN(sizeof(VexecAggSubChunk));
		if (c == NULL)
			sb->first = n;
		else
			c->next = n;
		sb->last = n;
		c = n;
		s->sub_bytes += MAXALIGN(sizeof(VexecAggSubChunk)) + size;
	}
	ref = (VexecAggSubRef *) (c->data + c->used);
	ref->hash = hash;
	ref->len = len;
	ref->rec = kept ? rec : sub_copy(s, rec, len);
	c->used += sizeof(VexecAggSubRef);
	sb->rows++;
	s->sub_records++;
	if (kept)
		s->sub_referenced++;
}

/*
 * F29: the exchange's chunks the references pointed into, and the copies,
 * let go once every group of them is out -- the last run's handed on, the
 * node rescanned or ended
 */
static void
sub_let_go(VexecAggState *s)
{
	if (s->sub_kept)
	{
		vexec_repart_release_kept(s->node.vec_child);
		s->sub_kept = false;
	}
	if (s->subcopycxt != NULL)
		MemoryContextReset(s->subcopycxt);
	s->copy_at = NULL;
	s->copy_left = 0;
	s->rec_groups_live = false;
}

/* Sub-buffers [begin, end)'s records into the table, a batch's worth of them at a time */
static void
sub_take(VexecAggState *s, int begin, int end)
{
	int			n = 0;

	if (s->sub_refs)
	{
		/* F29: the references, the records where they lie -- groups of their own, where they may be */
		s->pk_hashes_on = true;
		s->rec_groups_live = s->rec_groups && vexec_enable_final_record_groups;
		for (int p = begin; p < end; p++)
			for (VexecAggSubChunk *c = s->subs[p].first; c != NULL; c = c->next)
			{
				const VexecAggSubRef *ref = (const VexecAggSubRef *) c->data;
				const VexecAggSubRef *stop = (const VexecAggSubRef *) (c->data + c->used);

				for (; ref < stop; ref++)
				{
					s->pk_recs[n] = (char *) ref->rec;
					s->pk_hashes[n++] = ref->hash;
					if (n == vexec_batch_rows)
					{
						take_packed(s, n);
						n = 0;
					}
				}
			}
		if (n > 0)
			take_packed(s, n);
		s->pk_hashes_on = false;
		s->rec_groups_live = false;
		return;
	}
	for (int p = begin; p < end; p++)
		for (VexecAggSubChunk *c = s->subs[p].first; c != NULL; c = c->next)
		{
			const char *rec = c->data;
			const char *stop = c->data + c->used;

			while (rec < stop)
			{
				s->pk_recs[n++] = (char *) rec;
				rec += *(const uint32 *) (rec + sizeof(uint32));
				if (n == vexec_batch_rows)
				{
					take_packed(s, n);
					n = 0;
				}
			}
		}
	if (n > 0)
		take_packed(s, n);
}

/*
 * Past the node's memory: the records held merged into the one table, in the
 * sub-buffers' order, and the sub-buffers emptied for the next window
 */
static void
sub_window(VexecAggState *s)
{
	sub_take(s, 0, VEXEC_AGG_SUBS);
	s->sub_windowed = true;
	s->sub_windows++;
	sub_begin(s);
}

/*
 * The input's end: the sub-buffers' runs queued as partitions to read, in
 * order, each of VEXEC_AGG_SUB_RECORDS records at most where a sub-buffer
 * has no more; the table, never made, hands up nothing first
 */
static void
sub_runs(VexecAggState *s)
{
	MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);
	int			begin = 0;
	int64		rows = 0;

	for (int p = 0; p <= VEXEC_AGG_SUBS; p++)
	{
		if (p < VEXEC_AGG_SUBS && (rows == 0 || rows + s->subs[p].rows <= VEXEC_AGG_SUB_RECORDS))
		{
			rows += s->subs[p].rows;
			continue;
		}
		if (rows > 0)
		{
			VexecAggPartition *run = palloc0_object(VexecAggPartition);

			run->depth = VEXEC_AGG_SUB_DEPTH;
			run->rows = rows;
			run->sub_begin = begin;
			run->sub_end = p;
			s->pending = lappend(s->pending, run);
			s->sub_runs++;
		}
		begin = p;
		rows = p < VEXEC_AGG_SUBS ? s->subs[p].rows : 0;
	}
	MemoryContextSwitchTo(old);
}

/* Sub-buffers [begin, end)'s chunks freed, their records merged */
static void
sub_free(VexecAggState *s, int begin, int end)
{
	for (int p = begin; p < end; p++)
	{
		VexecAggSubChunk *c = s->subs[p].first;

		while (c != NULL)
		{
			VexecAggSubChunk *next = c->next;

			pfree(c);
			c = next;
		}
		s->subs[p].first = s->subs[p].last = NULL;
	}
}

static void
agg_fill_packed(VexecAggState *s)
{
	VexecNode  *child = s->node.vec_child;

	if (s->reading != NULL && s->reading->sub_end > 0)
	{
		/* F23: a run of sub-buffers, from memory, freed once merged */
		sub_take(s, s->reading->sub_begin, s->reading->sub_end);
		sub_free(s, s->reading->sub_begin, s->reading->sub_end);
		s->reading = NULL;
		spill_finish(s);
		s->filled = true;
		if (s->table != NULL)
			s->buckets_final = Max(s->buckets_final, s->table->size);
		return;
	}
	if (s->reading != NULL)
	{
		BufFile    *f = s->reading->file;

		for (;;)
		{
			int			n;

			MemoryContextReset(s->pk_readcxt);
			for (n = 0; n < vexec_batch_rows; n++)
			{
				uint32		head[2];
				char	   *rec;

				if (BufFileReadMaybeEOF(f, head, sizeof(head), true) == 0)
					break;
				rec = MemoryContextAllocHuge(s->pk_readcxt, head[1]);
				memcpy(rec, head, sizeof(head));
				BufFileReadExact(f, rec + sizeof(head), head[1] - sizeof(head));
				s->pk_recs[n] = rec;
			}
			if (n == 0)
				break;
			take_packed(s, n);
		}
		MemoryContextReset(s->pk_readcxt);
		BufFileClose(s->reading->file);
		s->reading->file = NULL;
		s->reading = NULL;
	}
	else
	{
		const char *groups;
		Size		len;
		bool		kept;

		/* F23: into sub-buffers first, where it may */
		s->sub_on = sub_eligible(s);
		s->sub_windowed = false;
		if (s->sub_on)
			sub_begin(s);
		/* F29: their references, the exchange keeping the chunks they lie in until the groups are out */
		s->sub_refs = s->sub_on && vexec_enable_final_references;
		if (s->sub_refs)
		{
			vexec_repart_keep_chunks(child, true);
			s->sub_kept = true;
		}
		while (vexec_repart_next_chunk(child, &groups, &len, &kept))
		{
			const char *end = groups + len;

			if (s->sub_on)
			{
				int64		n = 0;

				while (groups < end)
				{
					if (s->sub_refs)
						sub_put_ref(s, groups, kept);
					else
						sub_put(s, groups);
					groups += *(const uint32 *) (groups + sizeof(uint32));
					n++;
				}
				if (child->css.ss.ps.instrument)
					InstrUpdateTupleCount(child->css.ss.ps.instrument, n);
				/*
				 * the records held past the node's memory: merged into one
				 * table a window at a time -- a run's table, small, needs
				 * none, where they fit
				 */
				if (s->sub_bytes > s->memlimit)
					sub_window(s);
				continue;
			}
			while (groups < end)
			{
				int			n = 0;

				while (groups < end && n < vexec_batch_rows)
				{
					s->pk_recs[n++] = (char *) groups;
					groups += *(const uint32 *) (groups + sizeof(uint32));
				}
				take_packed(s, n);
				if (child->css.ss.ps.instrument)
					InstrUpdateTupleCount(child->css.ss.ps.instrument, n);
			}
		}
		if (s->sub_refs)
			vexec_repart_keep_chunks(child, false);
		if (s->sub_on)
		{
			if (s->sub_windowed)
			{
				sub_take(s, 0, VEXEC_AGG_SUBS);
				MemoryContextReset(s->subcxt);
				s->subs = NULL;
			}
			else
				sub_runs(s);
			s->sub_on = false;
		}
	}
	spill_finish(s);
	s->filled = true;
	vexec_aggsize_put(s->size_key, s->ngroups);
	if (s->table != NULL)
		s->buckets_final = Max(s->buckets_final, s->table->size);
}

/*
 * D2: whether the node can pack its groups: a first stage, hashed, whose
 * every key and extra has a length and every state is one without a
 * pointer (aggtrans.c), its output its scan tuple as it is
 */
bool
vexec_agg_pack_ok(VexecNode *node)
{
	return node->kind == VEXEC_NODE_AGG && ((VexecAggState *) node)->pack_ok;
}

/*
 * D2: the groups to be packed into sink's npartitions partitions, by
 * deal(arg, hash): before the node's first row, and in place of handing
 * any up.
 */
void
vexec_agg_pack(VexecNode *node, VexecPackSink *sink, int npartitions,
			   int (*deal) (void *arg, uint32 hash), void *arg)
{
	VexecAggState *s = (VexecAggState *) node;

	Assert(s->pack_ok && s->ndeal == 0);
	(void) npartitions;
	/* a Gather run again rescans the node here, as ExecProcNode would */
	if (node->css.ss.ps.chgParam != NULL)
		ExecReScan(&node->css.ss.ps);
	s->sink = sink;
	s->deal = deal;
	s->deal_arg = arg;
	if (s->pk_keyvals == NULL)
	{
		s->pk_keyvals = MemoryContextAllocZero(node->mcxt, sizeof(Datum) * Max(s->nkeys, 1));
		s->pk_extravals = MemoryContextAllocZero(node->mcxt, sizeof(Datum) * Max(s->nextra, 1));
		s->pk_extranulls = MemoryContextAllocZero(node->mcxt, sizeof(bool) * Max(s->nextra, 1));
	}
}

/*
 * D2: every group of the input packed: a frozen table's batch tables' as
 * each batch ends (agg_fill()), then the table's, then each partition's of
 * a spill as it is aggregated in turn.  The groups packed, the node's rows
 * for EXPLAIN ANALYZE.
 */
int64
vexec_agg_pack_all(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;
	PlanState  *ps = &node->css.ss.ps;
	int64		before = s->packed_groups;
	char	   *g;

	Assert(s->sink != NULL && ps->chgParam == NULL);
	if (ps->instrument)
		InstrStartNode(ps->instrument);
	for (;;)
	{
		if (!s->filled)
			agg_fill(s);
		while ((g = next_group(s)) != NULL)
		{
			MemoryContext old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);

			pack_group(s, g);
			MemoryContextSwitchTo(old);
			ResetExprContext(s->tmpcontext);
		}
		vexec_pack_batch_end(s->sink);
		if (s->pending == NIL)
			break;
		s->reading = linitial(s->pending);
		s->pending = list_delete_first(s->pending);
		s->depth = s->reading->depth;
		table_reset(s);
		vexec_node_rescan(&s->node);
		s->filled = false;
	}
	s->done = true;
	s->sink = NULL;
	if (ps->instrument)
		InstrStopNode(ps->instrument, (double) (s->packed_groups - before));
	return s->packed_groups - before;
}

/*
 * D2: column col of the VecRepartition rep's output, from 0, as the column
 * of the scan tuple of the stage p below it -- through rep's target list
 * and p's, each a Var of the scan tuple's column -- or -1.
 */
static int
producer_col(VexecNode *rep, VexecAggState *p, int col)
{
	List	   *rtl = rep->css.ss.ps.plan->targetlist;
	List	   *ptl = p->node.css.ss.ps.plan->targetlist;
	TargetEntry *tle;
	Var		   *v;

	if (col < 0 || col >= list_length(rtl))
		return -1;
	tle = list_nth_node(TargetEntry, rtl, col);
	if (!IsA(tle->expr, Var) || ((Var *) tle->expr)->varno != INDEX_VAR)
		return -1;
	col = ((Var *) tle->expr)->varattno - 1;
	if (col < 0 || col >= list_length(ptl))
		return -1;
	tle = list_nth_node(TargetEntry, ptl, col);
	if (!IsA(tle->expr, Var))
		return -1;
	v = (Var *) tle->expr;
	if (v->varno != INDEX_VAR || v->varattno < 1 || v->varattno > p->noutcols)
		return -1;
	return v->varattno - 1;
}

/*
 * D2: a final stage over a VecRepartition of a stage that can pack its
 * groups takes them as they lie where each of its keys is one of the
 * producer's, of its kind; each extra a key or extra of the producer's;
 * and each transition a combining one over a column of the producer's
 * states that it merges (aggtrans.c).  The VecRepartition is told, before
 * any DSM is made.
 */
static void
unpack_setup(VexecAggState *s)
{
	VexecNode  *rep = s->node.vec_child;
	VexecAggState *p;
	int			i;

	if (rep == NULL || rep->kind != VEXEC_NODE_REPART || !s->hashes_from_child ||
		s->strategy != AGG_HASHED || s->sorted_planned || s->nstats >= 0 ||
		!DO_AGGSPLIT_COMBINE(s->split))
		return;
	p = (VexecAggState *) vexec_repart_child_agg(rep);
	if (p == NULL || !vexec_agg_pack_ok(&p->node))
		return;

	s->pk_key = palloc(sizeof(int) * Max(s->nkeys, 1));
	s->pk_extra = palloc(sizeof(int) * Max(s->nextra, 1));
	s->pk_stateoff = palloc(sizeof(Size) * Max(s->ntrans, 1));
	s->pk_trans = palloc(sizeof(VexecAggTrans *) * Max(s->ntrans, 1));
	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		int			pc = producer_col(rep, p, k->col);
		VexecAggKeyCol *pk;

		if (pc < 0 || p->outkind[pc] != VEXEC_AGGCOL_KEY)
			return;
		pk = &p->keys[p->outsrc[pc]];
		if (pk->kind != k->kind || pk->typlen != k->typlen || pk->typbyval != k->typbyval ||
			pk->eqop != k->eqop || pk->collation != k->collation)
			return;
		s->pk_key[i] = p->outsrc[pc];
	}
	for (i = 0; i < s->nextra; i++)
	{
		int			pc = producer_col(rep, p, s->extracol[i]);

		if (pc < 0)
			return;
		if (p->outkind[pc] == VEXEC_AGGCOL_KEY)
			s->pk_extra[i] = p->outsrc[pc];
		else if (p->outkind[pc] == VEXEC_AGGCOL_EXTRA)
			s->pk_extra[i] = -1 - p->outsrc[pc];
		else
			return;
	}
	for (i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];
		Expr	   *arg;
		int			pc;
		VexecAggTrans *pt;
		VexecAggTrans *st;

		if (t->nargs != 1 || t->filter >= 0 || t->share >= 0 || t->aggref->args == NIL)
			return;
		arg = linitial_node(TargetEntry, t->aggref->args)->expr;
		if (!IsA(arg, Var) || ((Var *) arg)->varno != OUTER_VAR)
			return;
		pc = producer_col(rep, p, ((Var *) arg)->varattno - 1);
		if (pc < 0 || p->outkind[pc] != VEXEC_AGGCOL_AGG)
			return;
		pt = &p->trans[p->aggstate->peragg[p->outsrc[pc]].transno];
		st = pt->share >= 0 ? &p->trans[pt->share] : pt;
		if (!vexec_aggtrans_merges(t, pt))
			return;
		s->pk_trans[i] = pt;
		s->pk_stateoff[i] = p->stateoff + st->stateoff;
	}
	s->producer = p;
	s->pk_probe = palloc0_array(VexecAggProbe, Max(s->nkeys, 1));
	s->pk_recs = palloc0_array(char *, VEXEC_BATCH_ROWS);
	s->pk_hashes = palloc0_array(uint32, VEXEC_BATCH_ROWS);

	/*
	 * F29: a packed group is a group of this node's as it lies where every
	 * key is its bits or its bytes, at its place in the producer's group --
	 * a text's in the group, at the offset its Datum holds, which
	 * group_equal() and group_column() read so -- and every state by value,
	 * as the merge would make it: no extra, no numeric key kept scaled
	 * (F14), every state's offset the producer's
	 */
	s->rec_groups = s->nextra == 0 && s->keyformoff == 0 && p->keyformoff == 0 && s->nkeys == p->nkeys &&
		s->keyoff == p->keyoff && s->extraoff == p->extraoff && s->groupsize <= p->groupsize;
	for (i = 0; i < s->nkeys && s->rec_groups; i++)
		s->rec_groups = (s->keys[i].kind == KEY_BITS || s->keys[i].kind == KEY_BYTES) && s->pk_key[i] == i;
	for (i = 0; i < s->ntrans && s->rec_groups; i++)
		s->rec_groups = vexec_aggtrans_state_as_is(&s->trans[i], s->pk_trans[i]) &&
			s->pk_stateoff[i] == s->stateoff + s->trans[i].stateoff;
	s->pk_readcxt = AllocSetContextCreate(s->node.mcxt, "VecAgg packed groups read back",
										  ALLOCSET_DEFAULT_SIZES);
	s->unpack_ok = vexec_repart_offer_unpack(rep);
}

/* ---------------------------------------------------------------------
 * Output
 * ---------------------------------------------------------------------
 */

/* A group's column i of the scan tuple into the slot. */
static inline void
group_column(VexecAggState *s, char *g, int i, TupleTableSlot *slot)
{
	int			src = s->outsrc[i];

	switch (s->outkind[i])
	{
		case VEXEC_AGGCOL_KEY:
			slot->tts_values[i] = group_keys(s, g)[src];
			slot->tts_isnull[i] = group_keynulls(s, g)[src];
			/* F29: a packed group, a final stage's, its text in it at the offset its Datum holds */
			if (!slot->tts_isnull[i] && !s->keys[src].typbyval && group_packed(g))
				slot->tts_values[i] = packed_ref(g, slot->tts_values[i]);
			if (!slot->tts_isnull[i] && group_keyform(s, g, src) != 0)
			{
				/* F14: its numeric, made in the row's memory, which the next row's output resets */
				MemoryContext old = MemoryContextSwitchTo(s->node.css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

				slot->tts_values[i] = vexec_scaled_to_numeric(NULL, DatumGetInt64(slot->tts_values[i]),
															   group_keyform(s, g, src) - 1);
				MemoryContextSwitchTo(old);
			}
			break;
		case VEXEC_AGGCOL_EXTRA:
			slot->tts_values[i] = group_extras(s, g)[src];
			slot->tts_isnull[i] = group_extranulls(s, g)[src];
			break;
		case VEXEC_AGGCOL_AGG:
			{
				VexecAggTrans *t = &s->trans[s->aggstate->peragg[src].transno];
				VexecAggTrans *st = t->share >= 0 ? &s->trans[t->share] : t;

				vexec_aggtrans_result(t, s->aggstate, src, group_state(s, g, st),
									  &slot->tts_values[i], &slot->tts_isnull[i]);
			}
			break;
	}
}

/*
 * A group's columns into the scan tuple: those C3's ranking reads, the
 * others NULL, where `needed` is given; else every one.
 */
static TupleTableSlot *
group_output_cols(VexecAggState *s, char *g, const bool *needed)
{
	TupleTableSlot *slot = s->node.css.ss.ss_ScanTupleSlot;
	int			i;

	ExecClearTuple(slot);
	if (needed != NULL)
	{
		for (i = 0; i < s->noutcols; i++)
			slot->tts_isnull[i] = true;
		for (int j = 0; j < s->ntopcols; j++)
			group_column(s, g, s->topcols[j], slot);
		return ExecStoreVirtualTuple(slot);
	}
	for (i = 0; i < s->noutcols; i++)
		group_column(s, g, i, slot);
	return ExecStoreVirtualTuple(slot);
}

static TupleTableSlot *
group_output(VexecAggState *s, char *g)
{
	return group_output_cols(s, g, NULL);
}

/* ---------------------------------------------------------------------
 * C3: the top n kept inside (above)
 * ---------------------------------------------------------------------
 */

static char *next_group(VexecAggState *s);

typedef struct TopEntry
{
	MinimalTuple tup;			/* the projected row */
	Datum	   *keys;
	bool	   *nulls;
} TopEntry;

/* The order of two rows' keys, the sort's: < 0 where a comes first. */
static int
top_compare(VexecAggState *s, const Datum *ak, const bool *an, const Datum *bk, const bool *bn)
{
	for (int k = 0; k < s->ntopkeys; k++)
	{
		int			c = ApplySortComparator(ak[k], an[k], bk[k], bn[k], &s->topssup[k]);

		if (c != 0)
			return c;
	}
	return 0;
}

/* The heap's order: the row that comes last at the root. */
static void
top_sift_down(VexecAggState *s, int i)
{
	TopEntry   *h = s->top;

	for (;;)
	{
		int			l = 2 * i + 1;
		int			r = l + 1;
		int			m = i;

		if (l < s->ntop && top_compare(s, h[l].keys, h[l].nulls, h[m].keys, h[m].nulls) > 0)
			m = l;
		if (r < s->ntop && top_compare(s, h[r].keys, h[r].nulls, h[m].keys, h[m].nulls) > 0)
			m = r;
		if (m == i)
			return;
		{
			TopEntry	t = h[i];

			h[i] = h[m];
			h[m] = t;
		}
		i = m;
	}
}

static void
top_sift_up(VexecAggState *s, int i)
{
	TopEntry   *h = s->top;

	while (i > 0)
	{
		int			p = (i - 1) / 2;

		if (top_compare(s, h[i].keys, h[i].nulls, h[p].keys, h[p].nulls) <= 0)
			return;
		{
			TopEntry	t = h[i];

			h[i] = h[p];
			h[p] = t;
		}
		i = p;
	}
}

/*
 * A group ranked: HAVING first, over the columns it and the keys read; then
 * the keys; kept where the heap has room or it comes before the heap's
 * last, its whole row projected then.
 */
static void
top_consider(VexecAggState *s, char *g)
{
	CustomScanState *css = &s->node.css;
	ExprContext *econtext = css->ss.ps.ps_ExprContext;
	Datum	   *kv = s->topkv;
	bool	   *kn = s->topkn;
	TupleTableSlot *out;
	MemoryContext old;
	TopEntry   *e;

	ResetExprContext(econtext);
	s->top_ranked++;
	if (s->topdirect > 0)
	{
		/* a count: its state, as it lies, with no slot made (above) */
		kv[0] = Int64GetDatum(*(const int64 *) (g + s->topdirect));
		kn[0] = false;
	}
	else
	{
		econtext->ecxt_scantuple = group_output_cols(s, g, s->topneeded);
		if (css->ss.ps.qual != NULL && !ExecQual(css->ss.ps.qual, econtext))
		{
			InstrCountFiltered1(css, 1);
			return;
		}
		for (int k = 0; k < s->ntopkeys; k++)
		{
			int			col = s->topkeycol[k];

			if (col >= 0)
			{
				kv[k] = econtext->ecxt_scantuple->tts_values[col];
				kn[k] = econtext->ecxt_scantuple->tts_isnull[col];
			}
			else
				kv[k] = ExecEvalExpr(s->topkey[k], econtext, &kn[k]);
		}
	}
	if (s->ntop == s->topn &&
		top_compare(s, kv, kn, s->top[0].keys, s->top[0].nulls) >= 0)
		return;

	/* kept: the whole row, finished and projected, in the heap's memory */
	econtext->ecxt_scantuple = group_output(s, g);
	out = css->ss.ps.ps_ProjInfo != NULL ? ExecProject(css->ss.ps.ps_ProjInfo) :
		ExecCopySlot(css->ss.ps.ps_ResultTupleSlot, econtext->ecxt_scantuple);
	if (s->ntop == s->topn)
	{
		e = &s->top[0];
		pfree(e->tup);
		for (int k = 0; k < s->ntopkeys; k++)
			if (!s->topkeybyval[k] && !e->nulls[k])
				pfree(DatumGetPointer(e->keys[k]));
	}
	else
		e = &s->top[s->ntop++];
	old = MemoryContextSwitchTo(s->topcxt);
	e->tup = ExecCopySlotMinimalTuple(out);
	for (int k = 0; k < s->ntopkeys; k++)
	{
		e->nulls[k] = kn[k];
		e->keys[k] = kn[k] ? (Datum) 0 :
			datumCopy(kv[k], s->topkeybyval[k], s->topkeylen[k]);
	}
	MemoryContextSwitchTo(old);
	if (e == &s->top[0] && s->ntop == s->topn)
		top_sift_down(s, 0);
	else
		top_sift_up(s, s->ntop - 1);
}

/* The Var attnos of the scan tuple an expression reads. */
static bool
scan_cols_walker(Node *node, bool *needed)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR && ((Var *) node)->varattno > 0)
	{
		needed[((Var *) node)->varattno - 1] = true;
		return false;
	}
	return expression_tree_walker(node, scan_cols_walker, needed);
}

/*
 * W2: the prefix of a bounded sort's keys that the device orders as
 * PostgreSQL's operators do, lent to its last answer (above): each key a
 * column of the scan tuple -- a grouping key of an integer's bits but a
 * "char"'s, or text or bytea by their bytes, under C's collation; or an
 * aggregate whose result one of the device's aggregates holds
 * (vexec_aggtrans_device_rank()), never a float -- ordered by its type's <
 * or >.  None under a HAVING but one the device applies itself before its
 * cut (W17).
 */
static void
device_rank_setup(VexecAggState *s, List *sortops, List *collations, List *nullsfirst)
{
	AggDevice  *d = s->dev;
	TupleDesc	desc = s->node.css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	int			n = 0;

	/* W17: under a HAVING only where the device applies it itself, before its cut */
	if (d == NULL || (s->node.css.ss.ps.plan->qual != NIL && d->having == NULL) || s->topn <= 0)
		return;
	d->rank = MemoryContextAllocZero(s->node.mcxt, sizeof(VexecDevRankKey) * Max(s->ntopkeys, 1));
	for (int k = 0; k < s->ntopkeys; k++)
	{
		int			col = s->topkeycol[k];
		Oid			coll = list_nth_oid(collations, k);
		Oid			sortop = list_nth_oid(sortops, k);
		int			devcol;
		int			devtype;
		Oid			type;
		TypeCacheEntry *tc;

		if (col < 0)
			break;
		type = TupleDescAttr(desc, col)->atttypid;
		if (s->outkind[col] == VEXEC_AGGCOL_KEY)
		{
			devcol = s->outsrc[col];
			devtype = d->types[devcol].type;
			if (devtype == VEXEC_DEV_STRING && type != BYTEAOID &&
				(!OidIsValid(coll) || !pg_newlocale_from_collation(coll)->collate_is_c))
				break;
		}
		else if (s->outkind[col] == VEXEC_AGGCOL_AGG)
		{
			int			aggno = s->outsrc[col];
			int			ti = s->aggstate->peragg[aggno].transno;
			int			j = vexec_aggtrans_device_rank(&s->trans[ti], s->aggstate, aggno);

			if (j < 0 || j >= d->trans_n[ti])
				break;
			devcol = s->nkeys + d->trans_first[ti] + j;
			devtype = d->dev_type[d->trans_first[ti] + j];
			/* W17: a text's minimum or maximum, by its bytes under C's collation alone */
			if (devtype == VEXEC_DEV_STRING && type != BYTEAOID &&
				(!OidIsValid(coll) || !pg_newlocale_from_collation(coll)->collate_is_c))
				break;
		}
		else
			break;
		if (devtype == VEXEC_DEV_NONE || devtype == VEXEC_DEV_INT8 ||
			devtype == VEXEC_DEV_FLOAT32 || devtype == VEXEC_DEV_FLOAT64)
			break;
		tc = lookup_type_cache(type, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
		if (sortop == tc->lt_opr)
			d->rank[n].descending = 0;
		else if (sortop == tc->gt_opr)
			d->rank[n].descending = 1;
		else
			break;
		d->rank[n].col = (int16) devcol;
		d->rank[n].nulls_first = list_nth_int(nullsfirst, k) != 0;
		n++;
	}
	d->nrank = n;
	d->rank_ties = n < s->ntopkeys;
	d->rank_k = (uint64) s->topn;
}

/*
 * C3: a bounded VecSort above lends its keys -- columns of the node's
 * output, with their ordering operators, collations and NULLS FIRST flags
 * -- and its bound: the node keeps its n best groups (above).  False where
 * it does not take them: no hashed final aggregation, or an output or
 * HAVING that a volatile function or a subplan would run for fewer groups.
 * W2: the device, where the node takes it, ranks its last answer by them.
 */
bool
vexec_agg_set_topn(VexecNode *node, int64 bound, List *keycols, List *sortops,
				   List *collations, List *nullsfirst)
{
	VexecAggState *s = (VexecAggState *) node;
	Plan	   *plan = node->css.ss.ps.plan;
	ListCell   *lc;

	if (node->kind != VEXEC_NODE_AGG || s->strategy != AGG_HASHED ||
		DO_AGGSPLIT_SKIPFINAL(s->split) || bound <= 0 || bound > 64 * 1024 ||
		contain_volatile_functions((Node *) plan->targetlist) ||
		contain_volatile_functions((Node *) plan->qual) ||
		contain_subplans((Node *) plan->targetlist) || contain_subplans((Node *) plan->qual))
		return false;
	s->topn = bound;
	s->ntopkeys = list_length(keycols);
	s->topkey = palloc(sizeof(ExprState *) * s->ntopkeys);
	s->topssup = palloc0(sizeof(SortSupportData) * s->ntopkeys);
	s->topkeylen = palloc(sizeof(int16) * s->ntopkeys);
	s->topkeybyval = palloc(sizeof(bool) * s->ntopkeys);
	s->topneeded = palloc0(sizeof(bool) * Max(s->noutcols, 1));
	s->topkeycol = palloc(sizeof(int) * s->ntopkeys);
	foreach(lc, keycols)
	{
		int			k = foreach_current_index(lc);
		TargetEntry *tle = list_nth_node(TargetEntry, plan->targetlist, lfirst_int(lc) - 1);
		SortSupport ssup = &s->topssup[k];

		s->topkey[k] = ExecInitExpr(tle->expr, &node->css.ss.ps);
		s->topkeycol[k] = IsA(tle->expr, Var) && ((Var *) tle->expr)->varno == INDEX_VAR &&
			((Var *) tle->expr)->varattno > 0 ? ((Var *) tle->expr)->varattno - 1 : -1;
		get_typlenbyval(exprType((Node *) tle->expr), &s->topkeylen[k], &s->topkeybyval[k]);
		scan_cols_walker((Node *) tle->expr, s->topneeded);
		ssup->ssup_cxt = node->mcxt;
		ssup->ssup_collation = list_nth_oid(collations, k);
		ssup->ssup_nulls_first = list_nth_int(nullsfirst, k) != 0;
		ssup->ssup_attno = k + 1;
		ssup->abbreviate = false;
		PrepareSortSupportFromOrderingOp(list_nth_oid(sortops, k), ssup);
	}
	scan_cols_walker((Node *) plan->qual, s->topneeded);
	/* a lone key that is a count, under no HAVING: read from its state */
	if (s->ntopkeys == 1 && plan->qual == NIL && s->topkeycol[0] >= 0 &&
		s->outkind[s->topkeycol[0]] == VEXEC_AGGCOL_AGG)
	{
		int			aggno = s->outsrc[s->topkeycol[0]];
		VexecAggTrans *t = &s->trans[s->aggstate->peragg[aggno].transno];

		if (t->share < 0 && vexec_aggtrans_int64_result(t, s->aggstate, aggno))
			s->topdirect = s->stateoff + t->stateoff;
	}
	s->topcols = palloc(sizeof(int) * Max(s->noutcols, 1));
	for (int i = 0; i < s->noutcols; i++)
		if (s->topneeded[i])
			s->topcols[s->ntopcols++] = i;
	s->topcxt = AllocSetContextCreate(node->mcxt, "VecAgg top n", ALLOCSET_DEFAULT_SIZES);
	s->top = MemoryContextAllocZero(node->mcxt, sizeof(TopEntry) * bound);
	for (int i = 0; i < bound; i++)
	{
		s->top[i].keys = MemoryContextAlloc(node->mcxt, sizeof(Datum) * s->ntopkeys);
		s->top[i].nulls = MemoryContextAlloc(node->mcxt, sizeof(bool) * s->ntopkeys);
	}
	s->topkv = palloc(sizeof(Datum) * s->ntopkeys);
	s->topkn = palloc(sizeof(bool) * s->ntopkeys);
	s->topslot = ExecInitExtraTupleSlot(node->css.ss.ps.state,
										node->css.ss.ps.ps_ResultTupleDesc,
										&TTSOpsMinimalTuple);
	device_rank_setup(s, sortops, collations, nullsfirst);
	return true;
}

/*
 * C3's output: every group ranked -- the table's, then each spilled
 * partition's -- and the kept rows handed up, in the heap's order, for the
 * sort above to order.
 */
static TupleTableSlot *
top_exec(VexecAggState *s)
{
	CustomScanState *css = &s->node.css;

	if (!s->top_filled)
	{
		for (;;)
		{
			char	   *g;

			if (!s->filled)
				agg_fill(s);
			while ((g = next_group(s)) != NULL)
				top_consider(s, g);
			if (s->pending == NIL)
				break;
			/* the table is out: the next partition */
			s->reading = linitial(s->pending);
			s->pending = list_delete_first(s->pending);
			s->depth = s->reading->depth;
			table_reset(s);
			vexec_node_rescan(&s->node);
			s->filled = false;
		}
		s->top_filled = true;
		s->topnext = 0;
		sub_let_go(s);			/* F29: the rows kept are copies */
	}
	if (s->topnext >= s->ntop)
		return ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
	return ExecStoreMinimalTuple(s->top[s->topnext++].tup, s->topslot, false);
}

/*
 * The next group of the table, or NULL once every one is out: in the order
 * they were made (X9, above), the ones a few places on fetched ahead.
 */
static char *
next_group(VexecAggState *s)
{
	char	   *g;

	if (s->strategy == AGG_PLAIN)
	{
		g = s->plaingroup;
		s->plaingroup = NULL;
		return g;
	}
	/* W1: the device's last answer, from its pages, the table holding no group */
	if (s->dev != NULL && s->dev->stream)
		return dev_stream_next(s);
	if (s->next_made >= s->nmade)
		return NULL;
	if (s->next_made + 8 < s->nmade)
	{
		char	   *ahead = s->made[s->next_made + 8];

		__builtin_prefetch(ahead);
		__builtin_prefetch(ahead + s->stateoff);
	}
	return s->made[s->next_made++];
}

/* C2: the batch table's next group, or NULL once every one is out. */
static char *
next_bgroup(VexecAggState *s)
{
	VexecAggEntry *e;

	if (!s->biterating)
	{
		vagg_start_iterate(s->btable, &s->biter);
		s->biterating = true;
	}
	e = vagg_iterate(s->btable, &s->biter);
	return e != NULL ? e->group : NULL;
}

/*
 * The next group to hand up, or NULL once every one is out: a batch
 * table's groups as each batch ends (C2), the table's once the input is
 * read, then each partition's.
 */
static char *
next_out_group(VexecAggState *s)
{
	if (!s->sorted_decided)
		sorted_decide(s);
	for (;;)
	{
		char	   *g;

		if (s->done)
			return NULL;
		if (s->sorted)
			return sorted_next_group(s);
		if (s->boutput)
		{
			/*
			 * C2: the groups of the batch just aggregated, handed on; the
			 * table emptied once the last is out, the row handed up before
			 * it consumed
			 */
			g = next_bgroup(s);
			if (g == NULL)
			{
				batch_table_reset(s);
				continue;
			}
			return g;
		}
		if (!s->filled)
		{
			agg_fill(s);
			if (s->boutput)
				continue;
		}

		g = next_group(s);
		/*
		 * C5: a group past the boundary may lack rows passed over; one
		 * within it lacks none.  A first stage hands every group on, for
		 * the final stage to judge.
		 */
		if (g != NULL && s->topk > 0 && !DO_AGGSPLIT_SKIPFINAL(s->split))
		{
			/*
			 * the boundary once the table is filled: a later partition's
			 * rows may lower it, past groups of this table that lack none
			 */
			if (!s->tfinal_set)
			{
				s->tfinal = topk_bound(s);
				s->tfinal_set = true;
			}
			if (*(uint32 *) g > s->tfinal)
				continue;
		}
		if (g == NULL)
		{
			/* the table is out: the next partition, if any */
			if (s->pending == NIL)
			{
				s->done = true;
				sub_let_go(s);	/* F29 */
				continue;
			}
			s->reading = linitial(s->pending);
			s->pending = list_delete_first(s->pending);
			s->depth = s->reading->depth;
			table_reset(s);
			vexec_node_rescan(&s->node);
			s->filled = false;
			continue;
		}
		return g;
	}
}

/*
 * X9: the next batch of the groups dealt into the partitions: the one a
 * group fills, or, once every group is out, each partition's rest.  The
 * batch handed up before is emptied first: its parent is done with it.
 */
static VexecBatch *
next_dealt_batch(VexecAggState *s)
{
	ExprContext *econtext = s->node.css.ss.ps.ps_ExprContext;
	int			ncols = s->noutcols;

	if (s->dealt_out >= 0)
		vexec_batch_reset(s->dealt[s->dealt_out]);
	s->dealt_out = -1;
	for (;;)
	{
		char	   *g;
		uint32		hash;
		int			p;
		VexecBatch *b;
		TupleTableSlot *slot;

		ResetExprContext(econtext);
		g = next_out_group(s);
		if (g == NULL)
		{
			while (s->deal_rest < s->ndeal)
			{
				p = s->deal_rest++;
				if (s->dealt[p] != NULL && s->dealt[p]->nrows > 0)
				{
					s->dealt_out = p;
					return s->dealt[p];
				}
			}
			return NULL;
		}
		hash = *(uint32 *) g;
		p = s->deal(s->deal_arg, hash);
		b = s->dealt[p];
		if (b == NULL)
			b = s->dealt[p] = vexec_batch_create(s->node.mcxt, ncols + 1, s->dealtypes);
		if (b->nrows == 0 && !b->filling)
			vexec_batch_begin_rows(b);
		slot = group_output(s, g);
		memcpy(s->dealvals, slot->tts_values, sizeof(Datum) * ncols);
		memcpy(s->dealnulls, slot->tts_isnull, sizeof(bool) * ncols);
		s->dealvals[ncols] = Int32GetDatum((int32) hash);
		s->dealnulls[ncols] = false;
		vexec_batch_add_values(b, s->dealvals, s->dealnulls);
		if (b->nrows == b->room)
		{
			s->dealt_out = p;
			return b;
		}
	}
}

/*
 * X9: the groups to be dealt into a VecRepartition's npartitions
 * partitions by deal(arg, hash) as they go out: each batch the node hands
 * up from now on holds one partition's groups -- the scan tuple's columns,
 * in their build shapes, and an int4 of each group's hash -- whose number
 * vexec_agg_dealt_partition() then gives.  Before the node's first batch.
 */
void
vexec_agg_deal(VexecNode *node, int npartitions, int (*deal) (void *arg, uint32 hash),
			   void *arg)
{
	VexecAggState *s = (VexecAggState *) node;
	MemoryContext old = MemoryContextSwitchTo(node->mcxt);

	Assert(node->next_out != NULL && s->topn == 0);
	if (s->dealtypes == NULL)
	{
		s->dealtypes = palloc(sizeof(VexecType *) * (s->noutcols + 1));
		memcpy(s->dealtypes, s->outb->types, sizeof(VexecType *) * s->noutcols);
		s->dealtypes[s->noutcols] = vexec_type_make(INT4OID, -1, InvalidOid);
		s->dealvals = palloc(sizeof(Datum) * (s->noutcols + 1));
		s->dealnulls = palloc(sizeof(bool) * (s->noutcols + 1));
	}
	if (s->dealt_size != npartitions)
	{
		s->dealt = palloc0(sizeof(VexecBatch *) * npartitions);
		s->dealt_size = npartitions;
	}
	for (int p = 0; p < npartitions; p++)
		if (s->dealt[p] != NULL)
			vexec_batch_reset(s->dealt[p]);
	s->ndeal = npartitions;
	s->deal = deal;
	s->deal_arg = arg;
	s->dealt_out = -1;
	s->deal_rest = 0;
	MemoryContextSwitchTo(old);
}

/* X9: the partition whose groups the batch handed up last holds. */
int
vexec_agg_dealt_partition(VexecNode *node)
{
	return ((VexecAggState *) node)->dealt_out;
}

/*
 * X9: the next batch of groups, for a vector parent -- the node's output
 * where it is the scan tuple as it is, with no HAVING (agg_begin()) -- and
 * each row's group's hash (vexec_agg_batch_hashes()), a batch of a
 * group a row, as Velox's aggregation extracts its output's columns from
 * its groups' rows (RowContainer::extractColumn(), velox/exec/RowContainer.h).
 * The values are copied into the batch, so a group's memory may go before
 * the batch does: a batch table's goes as its last group is out (C2).  The
 * columns keep their build shapes, which every kernel reads, and a frame
 * takes as they are.
 */
static VexecBatch *
agg_next_batch(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;
	VexecBatch *out = s->outb;
	TupleTableSlot *slot;

	if (s->ndeal > 0)
		return next_dealt_batch(s);
	vexec_batch_reset(out);
	vexec_batch_begin_rows(out);
	/* the results' memory, of the batch before: copied into it */
	ResetExprContext(node->css.ss.ps.ps_ExprContext);
	while (out->nrows < out->room)
	{
		if (s->topn > 0)
		{
			/* C3's n best, as rows */
			slot = top_exec(s);
			if (TupIsNull(slot))
				break;
			slot_getallattrs(slot);
			s->outhash[out->nrows] = 0;
		}
		else
		{
			char	   *g = next_out_group(s);

			if (g == NULL)
				break;
			s->outhash[out->nrows] = *(uint32 *) g;
			slot = group_output(s, g);
		}
		vexec_batch_add_values(out, slot->tts_values, slot->tts_isnull);
	}
	if (out->nrows == 0)
		return NULL;
	return out;
}

static TupleTableSlot *
agg_exec(CustomScanState *css)
{
	VexecAggState *s = (VexecAggState *) css;
	ExprContext *econtext = css->ss.ps.ps_ExprContext;

	if (s->topn > 0)
		return top_exec(s);
	for (;;)
	{
		char	   *g;

		ResetExprContext(econtext);
		g = next_out_group(s);
		if (g == NULL)
			return ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
		s->last_hash = *(uint32 *) g;
		econtext->ecxt_scantuple = group_output(s, g);
		if (css->ss.ps.qual != NULL && !ExecQual(css->ss.ps.qual, econtext))
		{
			InstrCountFiltered1(css, 1);
			continue;
		}
		if (css->ss.ps.ps_ProjInfo != NULL)
			return ExecProject(css->ss.ps.ps_ProjInfo);
		return ExecCopySlot(css->ss.ps.ps_ResultTupleSlot, econtext->ecxt_scantuple);
	}
}

/* ---------------------------------------------------------------------
 * Begin, rescan, end
 * ---------------------------------------------------------------------
 */

/* How a grouping column's operator lets its keys be hashed and compared. */
static VexecAggKeyKind
key_kind(Oid eqop, Oid collation, Form_pg_attribute att)
{
	RegProcedure eqfn = get_opcode(eqop);

	switch (eqfn)
	{
		case F_INT2EQ:
		case F_INT4EQ:
		case F_INT8EQ:
		case F_OIDEQ:
		case F_BOOLEQ:
		case F_CHAREQ:
		case F_DATE_EQ:
		case F_TIME_EQ:
		case F_TIMESTAMP_EQ:
		case F_TIMESTAMPTZ_EQ:
		case F_CASH_EQ:
			if (att->attbyval)
				return KEY_BITS;
			break;
		case F_BYTEAEQ:
			return KEY_BYTES;
		case F_TEXTEQ:
			if (!OidIsValid(collation) || get_collation_isdeterministic(collation))
				return KEY_BYTES;
			break;
		default:
			break;
	}
	return KEY_FMGR;
}

/* A grouping column, set up: how it is hashed and compared. */
static void
key_setup(VexecAggKeyCol *k, TupleDesc desc, int col, Oid eqop, Oid collation)
{
	Oid			lhash;
	Oid			rhash;
	Form_pg_attribute att;

	k->col = col;
	if (k->col < 0 || k->col >= desc->natts)
		elog(ERROR, "vexec: grouping column %d of %d", k->col + 1, desc->natts);
	att = TupleDescAttr(desc, k->col);
	k->eqop = eqop;
	k->collation = collation;
	k->typlen = att->attlen;
	k->typbyval = att->attbyval;
	k->kind = key_kind(eqop, k->collation, att);
	k->numeric = k->kind == KEY_FMGR && get_opcode(eqop) == F_NUMERIC_EQ;
	if (!get_op_hash_functions(eqop, &lhash, &rhash))
		elog(ERROR, "vexec: no hash function for the grouping operator %u", eqop);
	fmgr_info(lhash, &k->hash);
	fmgr_info(get_opcode(eqop), &k->eq);
}

/*
 * C4 (§3.19): VecAgg's hash of a row's keys, for VecRepartition, which deals
 * rows out by it: so that the VecAgg above takes each row's hash from it,
 * and the partial VecAgg below hands its groups' on, and no node hashes a
 * key twice.  The hash is the one a VecAgg grouping by the same keys --
 * the same columns, equality operators and collations, in the same order --
 * computes for them.
 */
struct VexecKeyHash
{
	int			nkeys;
	VexecAggKeyCol *keys;
	VexecAggProbe *probe;
};

VexecKeyHash *
vexec_key_hash_create(TupleDesc desc, List *keycols, List *eqops, List *collations)
{
	VexecKeyHash *kh = palloc0(sizeof(VexecKeyHash));

	kh->nkeys = list_length(keycols);
	kh->keys = palloc0_array(VexecAggKeyCol, Max(kh->nkeys, 1));
	kh->probe = palloc0_array(VexecAggProbe, Max(kh->nkeys, 1));
	for (int i = 0; i < kh->nkeys; i++)
		key_setup(&kh->keys[i], desc, list_nth_int(keycols, i) - 1,
				  list_nth_oid(eqops, i), list_nth_oid(collations, i));
	return kh;
}

/*
 * The hash of a row's keys; the slot's columns deformed.  A key's
 * detoasting allocates in the current memory context.
 */
uint32
vexec_key_hash_slot(VexecKeyHash *kh, TupleTableSlot *slot)
{
	for (int i = 0; i < kh->nkeys; i++)
	{
		int			col = kh->keys[i].col;

		probe_set(&kh->keys[i], &kh->probe[i], slot->tts_values[col], slot->tts_isnull[col]);
	}
	return probes_hash(kh->keys, kh->probe, kh->nkeys);
}

/*
 * Whether a VecAgg's output rows carry their groups' hashes for these keys
 * -- columns of its output from 1, with these equality operators and
 * collations: its own keys, in their order, each a column of its scan tuple
 * as it is.  vexec_agg_last_hash() then gives the hash of the row it last
 * returned, which is vexec_key_hash_slot()'s for that row.
 */
bool
vexec_agg_hashes_keys(VexecNode *node, List *keycols, List *eqops, List *collations)
{
	VexecAggState *s = (VexecAggState *) node;
	List	   *tlist = node->css.ss.ps.plan->targetlist;

	if (node->kind != VEXEC_NODE_AGG || s->strategy != AGG_HASHED ||
		list_length(keycols) != s->nkeys)
		return false;
	for (int i = 0; i < s->nkeys; i++)
	{
		int			col = list_nth_int(keycols, i);
		TargetEntry *tle;
		Var		   *v;

		if (col < 1 || col > list_length(tlist))
			return false;
		tle = list_nth_node(TargetEntry, tlist, col - 1);
		if (!IsA(tle->expr, Var))
			return false;
		v = (Var *) tle->expr;
		if (v->varno != INDEX_VAR || v->varattno < 1 || v->varattno > s->noutcols ||
			s->outkind[v->varattno - 1] != VEXEC_AGGCOL_KEY || s->outsrc[v->varattno - 1] != i ||
			s->keys[i].eqop != list_nth_oid(eqops, i) ||
			s->keys[i].collation != list_nth_oid(collations, i))
			return false;
	}
	s->hashes_read = true;		/* W1: a streamed group's hash computed for it */
	return true;
}

uint32
vexec_agg_last_hash(VexecNode *node)
{
	return ((VexecAggState *) node)->last_hash;
}

/*
 * Whether a VecAgg's groups may be read a batch ahead of its parent's
 * asking (pg_vector_executor.md §3.22's T4; vechashjoin.c's
 * vexec_state_read_ahead_safe()): it reads its input whole before its first
 * group -- plain or hashed, not grouped in its input's order -- each of its
 * transitions is one of vexec's own that raises nothing, so that a spilled
 * partition's rows advanced past the first group raise nothing either --
 * not a float's sum or accumulation, which may overflow, nor PostgreSQL's
 * transition function -- and what its output runs is none or one of the
 * built-in functions below, which raise nothing over a state these
 * transitions made: a partial stage's serialization functions, as ORCA
 * pushes one below a join (TPC-H q03), and a final stage's or a whole
 * aggregate's final functions.  Its HAVING and target list are the caller's
 * to check.
 */
bool
vexec_agg_read_ahead_safe(PlanState *ps)
{
	VexecAggState *s = (VexecAggState *) ps;

	if (s->strategy != AGG_PLAIN && s->strategy != AGG_HASHED)
		return false;
	for (int i = 0; i < s->ntrans; i++)
	{
		switch (s->trans[i].kind)
		{
			case VEXEC_AGG_FMGR:
			case VEXEC_AGG_SUM_FLOAT:
			case VEXEC_AGG_ACCUM_FLOAT:
				return false;
			default:
				break;
		}
		if (DO_AGGSPLIT_SERIALIZE(s->trans[i].aggref->aggsplit))
			switch (s->trans[i].pertrans->serialfn_oid)
			{
				case InvalidOid:
				case F_NUMERIC_AVG_SERIALIZE:
				case F_INT8_AVG_SERIALIZE:
				case F_NUMERIC_POLY_SERIALIZE:
					break;
				default:
					return false;
			}
	}
	for (int i = 0; i < s->naggs; i++)
	{
		AggStatePerAgg peragg = &s->aggstate->peragg[i];

		if (peragg->aggref == NULL)
			return false;		/* not set up: nothing known of it */
		switch (peragg->finalfn_oid)
		{
			case InvalidOid:	/* none, as a partial stage's (SKIPFINAL) */
			case F_NUMERIC_SUM:
			case F_NUMERIC_AVG:
			case F_NUMERIC_POLY_SUM:
			case F_NUMERIC_POLY_AVG:
			case F_INT8_AVG:
				break;
			default:
				return false;
		}
	}
	return true;
}

/*
 * X9: the hashes of the groups of the batch the node last handed up, a
 * row's each; NULL where it keeps none, its n best (C3).
 */
const uint32 *
vexec_agg_batch_hashes(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;

	return s->topn > 0 ? NULL : s->outhash;
}

/*
 * pg_vector_executor.md §3.22's T5, option A: agg(CASE WHEN c THEN x [ELSE
 * NULL] END) taken as agg(x) FILTER (WHERE c), where the transition skips
 * a NULL input -- strict, or one of vexec's sums and averages, which skip
 * it as PostgreSQL's do -- and x can raise nothing, so that it may be
 * computed for the rows c turns down (target_inexact, below).  The CASE's
 * kernels would make each row's value a register's and sum it; the FILTER
 * sums x where c holds, as a pivot's sums of one column by a key's values
 * want (TPC-DS 02, 43, 59): the measurement after the kernels found them
 * still a third slower than this.
 */
static bool
case_filter(VexecAggTrans *t, Expr **xp, Expr **cp)
{
	Aggref	   *aggref = t->aggref;
	CaseExpr   *c;
	CaseWhen   *w;
	Oid			fn = t->pertrans->transfn_oid;

	if (DO_AGGSPLIT_COMBINE(aggref->aggsplit) || aggref->aggfilter != NULL ||
		aggref->aggdistinct != NIL || aggref->aggorder != NIL || list_length(aggref->args) != 1)
		return false;
	c = (CaseExpr *) linitial_node(TargetEntry, aggref->args)->expr;
	if (!IsA(c, CaseExpr) || c->arg != NULL || list_length(c->args) != 1)
		return false;
	if (c->defresult != NULL &&
		!(IsA(c->defresult, Const) && ((Const *) c->defresult)->constisnull))
		return false;
	if (!t->pertrans->transfn.fn_strict &&
		fn != F_NUMERIC_AVG_ACCUM && fn != F_INT8_AVG_ACCUM && fn != F_INT4_SUM &&
		fn != F_INT2_SUM && fn != F_NUMERIC_ACCUM && fn != F_INT8_ACCUM)
		return false;
	w = linitial_node(CaseWhen, c->args);
	if (vexec_expr_may_raise((Node *) w->result))
		return false;
	*xp = w->result;
	*cp = w->expr;
	return true;
}

/*
 * H8 (§3.14): SUM(x + k) -- int4_sum over int2 x plus, or minus, an int4
 * constant with which no int2 overflows int4 -- as SUM(x) + k * COUNT(x).
 * x + k can raise nothing, so leaving it unevaluated changes no error.
 * False for any other aggregate.
 */
static bool
sum_offset(VexecAggTrans *t, Expr **xp, int64 *kp)
{
	Aggref	   *aggref = t->aggref;
	Expr	   *arg;
	OpExpr	   *op;
	Expr	   *x;
	Node	   *c;
	int64		k;

	if (DO_AGGSPLIT_COMBINE(aggref->aggsplit) || aggref->aggfilter != NULL ||
		list_length(aggref->args) != 1 || t->pertrans->transfn_oid != F_INT4_SUM)
		return false;
	arg = linitial_node(TargetEntry, aggref->args)->expr;
	if (!IsA(arg, OpExpr) || list_length(((OpExpr *) arg)->args) != 2)
		return false;
	op = (OpExpr *) arg;
	set_opfuncid(op);
	switch (op->opfuncid)
	{
		case F_INT24PL:
		case F_INT24MI:
			x = linitial(op->args);
			c = lsecond(op->args);
			break;
		case F_INT42PL:
			x = lsecond(op->args);
			c = linitial(op->args);
			break;
		default:
			return false;
	}
	if (!IsA(c, Const) || ((Const *) c)->constisnull || exprType((Node *) x) != INT2OID)
		return false;
	k = DatumGetInt32(((Const *) c)->constvalue);
	if (op->opfuncid == F_INT24MI)
		k = -k;
	if (k > (int64) PG_INT32_MAX - PG_INT16_MAX || k < (int64) PG_INT32_MIN - PG_INT16_MIN)
		return false;
	*xp = x;
	*kp = k;
	return true;
}

/* The input columns an expression reads, as OUTER_VAR Vars. */
static bool
mark_outer_vars(Node *node, bool *needed)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == OUTER_VAR && var->varattno > 0)
			needed[var->varattno - 1] = true;
		return false;
	}
	return expression_tree_walker(node, mark_outer_vars, needed);
}

/*
 * H2: whether the child's source is asked to answer the aggregates from its
 * statistics.  A plain aggregate over a VecScan with no qual whose source
 * can; and every transition, without a FILTER, one whose state statistics
 * give exactly (vexec_aggtrans_stats_requests()), over nothing or over a
 * column of the scan's table.
 */
static void
stats_setup(VexecAggState *s, List *inputs)
{
	VexecNode  *child = s->node.vec_child;
	Plan	   *cplan;
	Index		scanrelid;
	VexecSourceAgg *reqs;
	int			n = 0;

	s->nstats = -1;
	if (!vexec_aggregate_statistics || s->strategy != AGG_PLAIN ||
		DO_AGGSPLIT_COMBINE(s->split) || child == NULL ||
		child->kind != VEXEC_NODE_SCAN || !vexec_scan_can_aggregate(child))
		return;
	cplan = child->css.ss.ps.plan;
	scanrelid = ((Scan *) cplan)->scanrelid;
	reqs = palloc0_array(VexecSourceAgg, 3 * Max(s->ntrans, 1));
	s->stats_first = palloc0_array(int, Max(s->ntrans, 1));
	for (int i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];
		AttrNumber	attnum = 0;
		Oid			argtype = InvalidOid;
		int			k;

		if (t->filter >= 0 || t->kind == VEXEC_AGG_SUM_OFFSET || t->nargs > 1)
			return;
		/* F36: under the scan's qual, count(*) alone */
		if (cplan->qual != NIL && t->kind != VEXEC_AGG_COUNT_STAR)
			return;
		if (t->nargs == 1)
		{
			Expr	   *arg = list_nth_node(TargetEntry, inputs, t->firstarg)->expr;
			Var		   *v;
			TargetEntry *ctle;

			if (!IsA(arg, Var) || ((Var *) arg)->varno != OUTER_VAR ||
				((Var *) arg)->varattno < 1 ||
				((Var *) arg)->varattno > list_length(cplan->targetlist))
				return;
			ctle = list_nth_node(TargetEntry, cplan->targetlist, ((Var *) arg)->varattno - 1);
			if (!IsA(ctle->expr, Var))
				return;
			v = (Var *) ctle->expr;
			if (v->varno != scanrelid || v->varattno < 1 || v->varlevelsup != 0)
				return;
			attnum = v->varattno;
			argtype = v->vartype;
		}
		k = vexec_aggtrans_stats_requests(t, argtype, attnum, &reqs[n]);
		if (k < 0)
			return;
		s->stats_first[i] = n;
		n += k;
	}
	s->nstats = n;
	s->stats_reqs = reqs;
	s->stats_answers = palloc0_array(VexecSourceAggAnswer, Max(n, 1));
	if (cplan->qual != NIL)
		vexec_scan_count_under_quals(child);
}

static void
agg_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecAggState *s = (VexecAggState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	VexecAggPlan plan;
	List	   *inputs = NIL;
	bool	   *inexact;
	Expr	  **offset_x;
	ListCell   *lc;
	Size		off;
	int			i;

	vexec_node_begin(node, estate);
	node->fetch = agg_fetch;
	vexec_agg_plan_decode(cscan, &plan);
	s->strategy = plan.strategy;
	s->split = plan.split;
	s->numgroups = plan.numgroups;
	s->group_bytes = plan.group_bytes;
	s->dev_planned = plan.device;
	s->whole_cols = plan.whole_cols;
	/* VC4: in the keys' order, or hashed, decided at the first group asked for */
	s->sorted_planned = plan.sorted_input && plan.strategy == AGG_HASHED && plan.keycols != NIL;
	s->sorted_decided = !s->sorted_planned;

	/* the child: its batches where it hands them up, else its rows */
	s->child = ExecInitNode(outerPlan(cscan), estate,
							eflags & ~(EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK));
	outerPlanState(css) = s->child;
	s->indesc = ExecGetResultType(s->child);
	s->child_batches = vexec_is_vector_state(s->child) &&
		vexec_node_batchable((VexecNode *) s->child);
	if (s->child_batches)
		node->vec_child = (VexecNode *) s->child;

	node->input_varno = OUTER_VAR;
	node->ninput = s->indesc->natts;
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(s->indesc->natts, 1));
	for (i = 0; i < s->indesc->natts; i++)
		node->input_attnos[i] = i + 1;
	node->input_slot = ExecInitExtraTupleSlot(estate, s->indesc, &TTSOpsVirtual);
	{
		VexecType **types = palloc(sizeof(VexecType *) * Max(s->indesc->natts, 1));

		for (i = 0; i < s->indesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(s->indesc, i);

			types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		}
		s->rowbatch = vexec_batch_create(node->mcxt, s->indesc->natts, types);
	}

	/* the keys */
	s->nkeys = list_length(plan.keycols);
	s->keys = palloc0_array(VexecAggKeyCol, Max(s->nkeys, 1));
	s->probe_slot = palloc0_array(VexecAggProbe, Max(s->nkeys, 1));
	s->probe = s->probe_slot;
	s->rowkeys = palloc0_array(VexecAggProbe, (Size) Max(s->nkeys, 1) * VEXEC_BATCH_ROWS);
	s->vprobe = palloc0_array(VexecAggProbe, Max(s->nkeys, 1));
	s->run_begin = palloc0_array(int, VEXEC_BATCH_ROWS + 1);
	s->run_diff = palloc0_array(uint8, VEXEC_BATCH_ROWS);
	s->rowhash = palloc0_array(uint32, VEXEC_BATCH_ROWS);
	s->rows = palloc0_array(int, VEXEC_BATCH_ROWS);
	for (i = 0; i < s->nkeys; i++)
		key_setup(&s->keys[i], s->indesc, list_nth_int(plan.keycols, i) - 1,
				  list_nth_oid(plan.eqops, i), list_nth_oid(plan.collations, i));
	/* X5: an array of groups where every key is an integer's bits */
	s->array_ok = s->strategy == AGG_HASHED && s->nkeys > 0;
	for (i = 0; i < s->nkeys; i++)
		s->array_ok &= s->keys[i].kind == KEY_BITS &&
			(s->keys[i].typlen == 1 || s->keys[i].typlen == 2 ||
			 s->keys[i].typlen == 4 || s->keys[i].typlen == 8);
	s->alo = palloc0_array(int64, Max(s->nkeys, 1));
	s->ahi = palloc0_array(int64, Max(s->nkeys, 1));
	s->awidth = palloc0_array(int64, Max(s->nkeys, 1));
	s->topk = s->strategy == AGG_HASHED && s->nkeys > 0 ? plan.topk : 0;
	s->tbound = PG_UINT32_MAX;
	s->hashes_from_child = s->child_batches && s->strategy == AGG_HASHED &&
		node->vec_child->kind == VEXEC_NODE_REPART &&
		vexec_repart_hashes_keys(node->vec_child, plan.keycols, plan.eqops, plan.collations);

	/* the scan tuple's columns, and the extras */
	s->noutcols = list_length(plan.outkind);
	s->outkind = palloc(sizeof(int) * Max(s->noutcols, 1));
	s->outsrc = palloc(sizeof(int) * Max(s->noutcols, 1));
	s->extracol = palloc(sizeof(int) * Max(s->noutcols, 1));
	for (i = 0; i < s->noutcols; i++)
	{
		s->outkind[i] = list_nth_int(plan.outkind, i);
		s->outsrc[i] = list_nth_int(plan.outsrc, i);
		if (s->outkind[i] == VEXEC_AGGCOL_EXTRA)
		{
			s->extracol[s->nextra] = s->outsrc[i] - 1;
			s->outsrc[i] = s->nextra++;
		}
	}

	/* the aggregates, set up as nodeAgg.c sets them up */
	s->aggcontext = CreateWorkExprContext(estate);
	s->tmpcontext = CreateExprContext(estate);
	s->naggs = list_length(plan.aggrefs);
	s->aggstate = vexec_agg_context_create(&css->ss.ps, estate, s->split, s->naggs,
										   plan.ntrans, s->aggcontext, s->tmpcontext);
	s->trans = vexec_aggtrans_setup(s->aggstate, plan.aggrefs, estate, &s->ntrans);

	/* each transition's arguments, then its FILTER, as the input's targets */
	offset_x = palloc0_array(Expr *, Max(s->ntrans, 1));
	for (i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];
		Aggref	   *aggref = t->aggref;
		int			k = 0;
		Expr	   *x;
		Expr	   *cond;
		int64		offset;

		if (aggref == NULL)
			elog(ERROR, "vexec: transition %d has no aggregate", i);
		if (sum_offset(t, &x, &offset))
		{
			int			share = -1;

			for (int j = 0; j < i && share < 0; j++)
				if (offset_x[j] != NULL && s->trans[j].share < 0 && equal(offset_x[j], x))
					share = j;
			vexec_aggtrans_set_offset(t, offset, share);
			t->filter = -1;
			if (share >= 0)
			{
				t->firstarg = s->trans[share].firstarg;
				continue;
			}
			offset_x[i] = x;
			t->firstarg = list_length(inputs);
			inputs = lappend(inputs, makeTargetEntry(x, list_length(inputs) + 1, NULL, false));
			continue;
		}
		t->firstarg = list_length(inputs);
		if (case_filter(t, &x, &cond))
		{
			inputs = lappend(inputs, makeTargetEntry(x, list_length(inputs) + 1, NULL, false));
			t->filter = list_length(inputs);
			inputs = lappend(inputs, makeTargetEntry(cond, list_length(inputs) + 1, NULL, false));
			continue;
		}
		foreach(lc, aggref->args)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			if (k++ >= t->nargs)
				break;
			inputs = lappend(inputs, makeTargetEntry(tle->expr, list_length(inputs) + 1,
													 NULL, false));
		}
		t->filter = -1;
		if (aggref->aggfilter != NULL && !DO_AGGSPLIT_COMBINE(aggref->aggsplit))
		{
			t->filter = list_length(inputs);
			inputs = lappend(inputs, makeTargetEntry(aggref->aggfilter, list_length(inputs) + 1,
													 NULL, false));
		}
	}
	s->ninputs = list_length(inputs);
	s->input_exprs = inputs;

	/*
	 * An argument under a FILTER is evaluated for rows nodeAgg.c would not
	 * evaluate it for: not exactly its rows, for an InitPlan behind it
	 * (expr/eval.c).
	 */
	inexact = palloc0_array(bool, Max(s->ninputs, 1));
	for (i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];

		if (t->filter >= 0)
			for (int k = 0; k < t->nargs; k++)
				inexact[t->firstarg + k] = true;
	}
	node->target_inexact = inexact;
	vexec_node_compile(node, NIL, inputs);
	/*
	 * PQ21 (pg_vector_executor.md §3.40): a scan child's nested columns as it
	 * hands them, where the inputs' kernels alone read them -- not a key's,
	 * which the keys read by their layouts
	 */
	if (s->child_batches && node->vec_child->kind == VEXEC_NODE_SCAN)
	{
		bool	   *excluded = palloc0_array(bool, Max(s->indesc->natts, 1));

		for (i = 0; i < s->nkeys; i++)
			if (s->keys[i].col >= 0 && s->keys[i].col < s->indesc->natts)
				excluded[s->keys[i].col] = true;
		/* and the extras, a group's input values */
		for (i = 0; i < s->nextra; i++)
			if (s->extracol[i] >= 0 && s->extracol[i] < s->indesc->natts)
				excluded[s->extracol[i]] = true;
		vexec_node_parent_nested(node, node->vec_child, excluded);
	}

	/* the kinds: vectorized where the argument's type has one */
	s->floats_ok = true;
	for (i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];
		Oid			argtype = InvalidOid;
		int32		argtypmod = -1;

		if (t->kind == VEXEC_AGG_SUM_OFFSET)
			continue;			/* H8's, set above */
		if (t->nargs > 0)
		{
			argtype = node->targets[t->firstarg].type->typid;
			argtypmod = node->targets[t->firstarg].type->typmod;
		}
		vexec_aggtrans_choose(t, t->aggref->aggsplit, argtype, argtypmod);
		if (t->kind == VEXEC_AGG_FMGR)
			s->floats_ok = false;
	}

	/*
	 * C5: rows passed over only where no transition may raise -- each one
	 * vexec's own, not through fmgr -- for such a row to be one PostgreSQL's
	 * would not have raised on.  The groups of the smallest hashes are kept,
	 * and a final or a single stage returns only those, either way: a stage
	 * below or above may pass rows over
	 */
	s->topk_skip = s->topk > 0;
	for (i = 0; i < s->ntrans && s->topk_skip; i++)
		if (s->trans[i].kind == VEXEC_AGG_FMGR || s->trans[i].may_raise)
			s->topk_skip = false;
	if (s->topk > 0)
		s->theap = palloc(sizeof(uint32) * s->topk);
	for (i = 0; i < s->ninputs; i++)
		if (node->targets[i].eager == NULL)
		{
			s->any_lazy = true;
			s->floats_ok = false;
		}
	s->batched = palloc0_array(bool, Max(s->ntrans, 1));
	stats_setup(s, inputs);
	s->argvals = palloc0_array(Datum, FUNC_MAX_ARGS);
	s->argnulls = palloc0_array(bool, FUNC_MAX_ARGS);

	/* a group: header, keys and their NULLs, extras and theirs, states */
	off = 0;
	for (i = 0; i < s->ntrans; i++)
	{
		s->trans[i].stateoff = off;
		off += MAXALIGN(s->trans[i].statesize);
	}
	s->keyoff = MAXALIGN(2 * sizeof(uint32));	/* the hash; D2's length */
	/* F14: a byte a key after the keys' NULLs, where one is numeric */
	s->keyformoff = 0;
	for (i = 0; i < s->nkeys; i++)
		if (s->keys[i].numeric)
			s->keyformoff = s->keyoff + (sizeof(Datum) + sizeof(bool)) * s->nkeys;
	s->extraoff = s->keyoff + MAXALIGN((sizeof(Datum) + sizeof(bool) + (s->keyformoff != 0 ? 1 : 0)) * s->nkeys);
	s->stateoff = s->extraoff + MAXALIGN((sizeof(Datum) + sizeof(bool)) * s->nextra);
	s->groupsize = s->stateoff + off;
	s->probe_group = palloc0(MAXALIGN(sizeof(uint32)));
	s->groups = palloc0_array(char *, VEXEC_BATCH_ROWS + 4);	/* agg_runs() stores past a run's end */

	/* spill: the input columns the keys, extras and inputs read */
	s->spillneeded = palloc0_array(bool, Max(s->indesc->natts, 1));
	for (i = 0; i < s->nkeys; i++)
		s->spillneeded[s->keys[i].col] = true;
	for (i = 0; i < s->nextra; i++)
		s->spillneeded[s->extracol[i]] = true;
	mark_outer_vars((Node *) inputs, s->spillneeded);
	s->spillslot = MakeSingleTupleTableSlot(s->indesc, &TTSOpsVirtual);
	s->readslot = MakeSingleTupleTableSlot(s->indesc, &TTSOpsMinimalTuple);

	s->memlimit = get_hash_memory_limit();
	if (s->strategy == AGG_HASHED)
		s->size_key = vexec_aggsize_key(estate, cscan->scan.plan.plan_node_id);

	/*
	 * X9: batches for a vector parent where the output is the scan tuple as
	 * it is -- no HAVING, no projection -- and no column of it is a state
	 * kept in memory, an internal one
	 */
	if (css->ss.ps.qual == NULL && css->ss.ps.ps_ProjInfo == NULL)
	{
		TupleDesc	scandesc = css->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
		VexecType **types = palloc(sizeof(VexecType *) * Max(scandesc->natts, 1));
		bool		ok = scandesc->natts > 0;

		for (i = 0; i < scandesc->natts && ok; i++)
		{
			Form_pg_attribute att = TupleDescAttr(scandesc, i);

			if (att->atttypid == INTERNALOID || get_typtype(att->atttypid) == TYPTYPE_PSEUDO)
				ok = false;
			else
				types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		}
		if (ok)
		{
			s->outb = vexec_batch_create(node->mcxt, scandesc->natts, types);
			s->outhash = palloc0_array(uint32, VEXEC_BATCH_ROWS);
			node->next_out = agg_next_batch;
		}
	}
	s->groupcxt = BumpContextCreate(node->mcxt, "VecAgg groups", ALLOCSET_DEFAULT_SIZES);
	s->hashcxt = AllocSetContextCreate(node->mcxt, "VecAgg hash table", ALLOCSET_DEFAULT_SIZES);

	/*
	 * D2: a first stage may pack its groups where every key, extra and
	 * state can lie in them as it is (above); a final stage over a
	 * VecRepartition of such a stage may take them so
	 */
	s->pack_ok = s->strategy == AGG_HASHED && DO_AGGSPLIT_SKIPFINAL(s->split) &&
		s->nkeys > 0 && !s->sorted_planned && s->nstats < 0 &&
		css->ss.ps.qual == NULL && css->ss.ps.ps_ProjInfo == NULL;
	for (i = 0; i < s->nkeys && s->pack_ok; i++)
		s->pack_ok = s->keys[i].typlen != -2;
	for (i = 0; i < s->nextra && s->pack_ok; i++)
		s->pack_ok = TupleDescAttr(s->indesc, s->extracol[i])->attlen != -2;
	for (i = 0; i < s->ntrans && s->pack_ok; i++)
		s->pack_ok = vexec_aggtrans_packable(&s->trans[i]);
	s->pack_values = false;
	for (i = 0; i < s->ntrans && s->pack_ok; i++)
		s->pack_values |= vexec_aggtrans_packs_value(&s->trans[i]);
	unpack_setup(s);

	/* V8b: the device strategy, where the node may take it */
	device_setup(s);

	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY) && s->sorted_decided)
		table_reset(s);
}

static void
agg_rescan(CustomScanState *css)
{
	VexecAggState *s = (VexecAggState *) css;

	device_reset(s);
	spill_close_all(s);
	s->depth = 0;
	s->filled = false;
	s->done = false;
	s->child_done = false;
	s->sink = NULL;
	s->unpacking = false;
	/* F23: the sub-buffers' records, every run's read or not; F29's chunks and copies */
	sub_let_go(s);
	if (s->subcxt != NULL)
		MemoryContextReset(s->subcxt);
	s->subs = NULL;
	s->sub_on = false;
	s->sub_refs = false;
	if (s->topn > 0)
	{
		MemoryContextReset(s->topcxt);
		s->ntop = 0;
		s->top_filled = false;
		s->topnext = 0;
	}
	/* C5: the heap of hashes again, from none */
	s->ntheap = 0;
	s->tbound = PG_UINT32_MAX;
	if (s->sorted)
		sorted_reset(s);		/* VC4: the child's order is the same scan's */
	else if (s->sorted_decided)
		table_reset(s);
	/*
	 * X9: the groups dealt as before, the parent's choice, each partition's
	 * batch empty -- a child rescans lazily, at the first batch its parent
	 * asks of it, after the parent has asked for the dealing again
	 */
	for (int p = 0; p < s->ndeal; p++)
		if (s->dealt[p] != NULL)
			vexec_batch_reset(s->dealt[p]);
	s->dealt_out = -1;
	s->deal_rest = 0;
	/* the child rescans on its first call when its parameters changed */
	if (s->child->chgParam == NULL)
		ExecReScan(s->child);
	vexec_node_rescan(&s->node);
}

static void
agg_end(CustomScanState *css)
{
	VexecAggState *s = (VexecAggState *) css;

	device_reset(s);
	spill_close_all(s);
	if (s->aggcontext)
		ReScanExprContext(s->aggcontext);
	for (int g = 0; g < 2; g++)
		if (s->saggcontext[g])
			ReScanExprContext(s->saggcontext[g]);
	if (s->spillslot)
		ExecDropSingleTupleTableSlot(s->spillslot);
	if (s->readslot)
		ExecDropSingleTupleTableSlot(s->readslot);
	sub_let_go(s);				/* F29: before the exchange lets go of its maps */
	ExecEndNode(s->child);
	vexec_node_end(&s->node);
}

/* ---------------------------------------------------------------------
 * EXPLAIN
 * ---------------------------------------------------------------------
 */

static const char *
agg_label(VexecAggState *s)
{
	const char *split = "";

	if (DO_AGGSPLIT_SKIPFINAL(s->split) && DO_AGGSPLIT_COMBINE(s->split))
		split = "Combine ";
	else if (DO_AGGSPLIT_SKIPFINAL(s->split))
		split = "Partial ";
	else if (DO_AGGSPLIT_COMBINE(s->split))
		split = "Finalize ";
	return psprintf("Vec %s%s", split, s->sorted_planned ? "GroupAggregate" :
					s->strategy == AGG_HASHED ? "HashAggregate" : "Aggregate");
}

/*
 * V8b: the device strategy, where the plan has it or vexec.gpu forces it --
 * the device, or why the node runs on the CPU -- and with ANALYZE what it
 * did: its chunks, the rows and bytes they took to the device, the answer
 * pages and groups that came back, and what the CPU strategy took.
 */
static void
agg_explain_device(VexecAggState *s, ExplainState *es)
{
	AggDevice  *d = s->dev;

	if (d == NULL)
	{
		if (s->dev_why != NULL)
			ExplainPropertyText("Device", psprintf("none: %s", s->dev_why), es);
		return;
	}
	/* W2: the bounded sort's keys the device ranks its last answer by */
	if (d->nrank > 0)
		ExplainPropertyText("Device Rank",
							psprintf("%d of the sort's %d keys, its first " INT64_FORMAT " groups%s",
									 d->nrank, s->ntopkeys, (int64) d->rank_k,
									 d->rank_ties ? " and those tying with the last" : ""), es);
	/* W17: the HAVING applied on the device to its answer's groups; W46, the node's where it answered early */
	if (d->having != NULL && es->analyze && d->having_left)
		ExplainPropertyText("Device HAVING", "left to the node: the device answered before its input's end", es);
	else if (d->having != NULL)
		ExplainPropertyText("Device HAVING", "applied on the device to its answer's groups", es);
	else if (d->having_why != NULL && es->verbose)
		ExplainPropertyText("Device HAVING", psprintf("none: %s", d->having_why), es);
	if (!es->analyze || !s->node.ran)
	{
		ExplainPropertyText("Device", "the GPU's group-by, as the node begins", es);
		if (d->whole)
			ExplainPropertyText("Device Stages", d->mixed ? "both, the distinct count of its lower stage's scan's rows, the other aggregates with it" :
							"both, the distinct count of its lower stage's scan's rows", es);
		if (vexec_precision == VEXEC_PRECISION_FAST)
			ExplainPropertyText("Precision", "fast", es);
		if (d->units)
			ExplainPropertyText("Device Scan", d->filter != NULL ?
								"its scan's units, read and decoded by the device, filtered by the scan's quals there" :
								"its scan's units, read and decoded by the device", es);
		else if (d->units_why != NULL)
			ExplainPropertyText("Device Scan", psprintf("none: %s", d->units_why), es);
		return;
	}
	if (d->device != NULL && !d->lost)
		ExplainPropertyText("Device", d->device, es);
	else if (d->device != NULL)
		ExplainPropertyText("Device", psprintf("%s, then the CPU: %s", d->device, d->why), es);
	else
		ExplainPropertyText("Device", psprintf("none: %s", d->why ? d->why : "it did not begin"), es);
	if (d->whole)
		ExplainPropertyText("Device Stages", d->mixed ? "both, the distinct count of its lower stage's scan's rows, the other aggregates with it" :
							"both, the distinct count of its lower stage's scan's rows", es);
	if (vexec_precision == VEXEC_PRECISION_FAST)
		ExplainPropertyText("Precision", "fast", es);
	ExplainPropertyInteger("Device Chunks", NULL, d->fig.chunks, es);
	ExplainPropertyInteger("Device Rows", NULL, d->fig.rows, es);
	ExplainPropertyInteger("Device Groups", NULL, d->fig.answer_rows, es);
	/* W1, W2: the last answer handed up from its pages, ranked */
	if (d->groups_streamed > 0 || d->ranked)
		ExplainPropertyText("Device Answer", d->ranked ?
							psprintf("ranked, its first " INT64_FORMAT " of " INT64_FORMAT " groups handed up from its pages",
									 d->fig.answer_rows, d->groups_before_rank) :
							"handed up from its pages", es);
	/* W47: the groups held to the input's end for the device's HAVING */
	if (d->held_groups > 0)
		ExplainPropertyText("Device Groups Held",
							psprintf(INT64_FORMAT " to the input's end for its HAVING, past the " INT64_FORMAT
									 " an answer is staged whole with%s", d->held_groups, d->stream_groups,
									 d->paged ? ", its answer read a page at a time" : ""), es);
	/* W37: the ranked answer's groups by sort */
	if (d->ranked && d->rank_sorted)
		ExplainPropertyText("Device Grouping", "by sort: its rows' keys sorted, the ranked keys' rows alone grouped", es);
	if (d->restarts > 0)
		ExplainPropertyInteger("Input Read Again on the CPU", NULL, d->restarts, es);
	if (es->verbose && d->unkept_chunks > 0)
		ExplainPropertyInteger("Device Chunks Let Go", NULL, d->unkept_chunks, es);
	if (d->units)
	{
		ExplainPropertyInteger("Device Units", NULL, d->units_read, es);
		ExplainPropertyInteger("Device Unit Rows", NULL, d->unit_rows, es);
		/* W5: of the units' columns, those the device kept from an earlier read */
		if (d->cols_asked > 0)
			ExplainPropertyText("Device Columns Kept", psprintf(INT64_FORMAT " of " INT64_FORMAT,
																 d->cols_cached, d->cols_asked), es);
		if (d->units_redone > 0)
			ExplainPropertyInteger("Units Read Here", NULL, d->units_redone, es);
		if (d->units_refused != NULL)
			ExplainPropertyText("Units Refused", d->units_refused, es);
		/* W39: requests the device had no memory for, halved and asked for again */
		if (d->units_split > 0)
			ExplainPropertyInteger("Device Requests Split", NULL, d->units_split, es);
		/* W38: what the last request's units' columns were gathered within */
		if (es->verbose && d->request_bytes > 0 && d->request_bytes < PG_UINT64_MAX)
			ExplainPropertyInteger("Device Request Limit", "kB", d->request_bytes / 1024, es);
	}
	else if (d->units_why != NULL && es->verbose)
		ExplainPropertyText("Device Scan", psprintf("none: %s", d->units_why), es);
	if (es->verbose)
	{
		ExplainPropertyInteger("Device Bytes In", "kB", (d->fig.bytes + 1023) / 1024, es);
		ExplainPropertyInteger("Device Bytes Out", "kB", (d->fig.answer_bytes + 1023) / 1024, es);
		ExplainPropertyInteger("Device Answers", NULL, d->flushes, es);
		ExplainPropertyInteger("Device Answer Pages", NULL, d->fig.answers, es);
		if (es->timing)
		{
			ExplainPropertyFloat("Device Wait Time", "ms", d->fig.wait_ms, 3, es);
			ExplainPropertyFloat("Device Time", "ms", d->fig.device_ms, 3, es);
		}
	}
	if (d->batches_cpu > 0)
	{
		ExplainPropertyInteger("CPU Batches", NULL, d->batches_cpu, es);
		ExplainPropertyInteger("CPU Rows", NULL, d->rows_cpu, es);
	}
	if (d->redone_rows > 0)
		ExplainPropertyInteger("Rows Redone on the CPU", NULL, d->redone_rows, es);
	if (es->verbose && d->groups_passed > 0)
		ExplainPropertyInteger("Device Groups Not Looked Up", NULL, d->groups_passed, es);
	if (d->groups_spilled > 0)
		ExplainPropertyInteger("Device Groups Spilled", NULL, d->groups_spilled, es);
	if (d->nomem > 0)
		ExplainPropertyInteger("Device Chunks Refused", NULL, d->nomem, es);
}

/*
 * pg_otel_probe's span of a VecAgg that ran (node.c; pg_vector_executor.md
 * §3.41): its device strategy's device, or why none, and what its tasks
 * did, as agg_explain_device() prints them (OT1); a hashed table's memory
 * and buckets, and the spills (OT2).  Its fields alone.
 */
void
vexec_agg_figures(VexecNode *node, VexecNodeFigures *f, bool on_error)
{
	VexecAggState *s = (VexecAggState *) node;
	AggDevice  *d = s->dev;

	(void) on_error;
	if (d == NULL && s->dev_why != NULL)
	{
		f->device = "none";
		f->device_why = s->dev_why;
	}
	else if (d != NULL)
	{
		f->device = d->device != NULL ? d->device : "none";
		if (d->device == NULL)
			f->device_why = d->why != NULL ? d->why : "it did not begin";
		else if (d->lost)
			f->device_why = d->why;
		f->device_requests = d->fig.requests;
		f->device_bytes_in = d->fig.bytes;
		f->device_bytes_out = d->fig.answer_bytes;
		f->device_ms = d->fig.device_ms;
		f->device_gpu_ms = d->fig.gpu_ms;
		f->device_wait_ms = d->fig.wait_ms;
		if (d->redone_rows > 0)
			f->device_rows_redone = d->redone_rows;
	}
	if (s->strategy == AGG_HASHED)
	{
		f->memory_kb = (int64) ((s->peak_memory + 1023) / 1024);
		if (s->buckets_initial > 0)
			f->buckets = (int64) Max(s->buckets_final, s->buckets_initial);
	}
	if (s->spilled_partitions > 0)
	{
		f->spilled_partitions = s->spilled_partitions;
		f->spilled_rows = s->spilled_rows;
		f->spill_depth = s->max_depth;
	}
}

static void
agg_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecAggState *s = (VexecAggState *) css;
	VexecNode  *node = &s->node;
	int			i;

	CbExplainRelabel(css, es, agg_label(s), NULL);
	if (s->nkeys > 0)
	{
		List	   *context = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan, ancestors);
		List	   *keys = NIL;

		for (i = 0; i < s->nkeys; i++)
		{
			Form_pg_attribute att = TupleDescAttr(s->indesc, s->keys[i].col);
			Var		   *v = makeVar(OUTER_VAR, s->keys[i].col + 1, att->atttypid,
									att->atttypmod, att->attcollation, 0);

			keys = lappend(keys, deparse_expression((Node *) v, context, es->verbose, false));
		}
		ExplainPropertyList("Group Key", keys, es);
	}
	/* VC4: the input in the keys' order, its source's, where the scan says so as it begins */
	if (s->sorted_planned)
		ExplainPropertyText("Group Order", "the scan's", es);
	if (s->sorted_planned && es->analyze && node->ran && s->sorted_decided)
		ExplainPropertyText("Groups In Order", s->sorted ? "yes" : "no: hashed", es);
	if (es->verbose)
	{
		StringInfoData buf;
		int			fast = 0;

		initStringInfo(&buf);
		for (i = 0; i < s->ntrans; i++)
		{
			if (s->trans[i].kind != VEXEC_AGG_FMGR)
				fast++;
			if (i > 0)
				appendStringInfoString(&buf, ", ");
			vexec_aggtrans_name(&s->trans[i], &buf);
		}
		ExplainPropertyText("Batch Format",
							node->layout.format == VEXEC_FORMAT_ARROW ? "arrow" : "postgres", es);
		ExplainPropertyText("Vector Transitions", psprintf("%d of %d", fast, s->ntrans), es);
		if (s->ntrans > 0)
			ExplainPropertyText("Transitions", buf.data, es);
		ExplainPropertyInteger("Kernel Steps", NULL, node->nkernels, es);
		ExplainPropertyInteger("Fallback Steps", NULL, node->nfallbacks, es);
		if (node->nshared > 0)
			ExplainPropertyInteger("Shared Steps", NULL, node->nshared, es);
		vexec_node_explain_declared(node, es);
		ExplainPropertyText("Input", s->unpack_ok ? "packed groups" :
							s->child_batches ? "batches" : "rows", es);
		if (es->analyze && s->packed_groups > 0)
			ExplainPropertyText("Output", "packed groups", es);
		else if (es->analyze && node->stats.batches_out > 0)
			ExplainPropertyText("Output", "batches", es);
		if (s->hashes_from_child)
			ExplainPropertyText("Key Hashes", "the child's", es);
		if (s->nstats >= 0)
			ExplainPropertyText("From Statistics",
								vexec_scan_source_name(node->vec_child), es);
	}
	agg_explain_device(s, es);
	if (es->analyze && node->ran)
	{
		if (s->strategy == AGG_HASHED)
		{
			ExplainPropertyInteger("Groups", NULL, s->total_groups, es);
			ExplainPropertyInteger("Memory Usage", "kB",
								   (int64) ((s->peak_memory + 1023) / 1024), es);
			/* C6: whether a table grew past the size it began with */
			if (es->verbose && s->buckets_initial > 0)
			{
				if (es->format != EXPLAIN_FORMAT_TEXT)
				{
					ExplainPropertyUInteger("Buckets", NULL, s->buckets_final, es);
					ExplainPropertyUInteger("Original Buckets", NULL, s->buckets_initial, es);
				}
				else if (s->buckets_final > s->buckets_initial)
					ExplainPropertyText("Buckets", psprintf(UINT64_FORMAT " (originally " UINT64_FORMAT ")",
															s->buckets_final, s->buckets_initial), es);
				else
					ExplainPropertyUInteger("Buckets", NULL, s->buckets_initial, es);
			}
		}
		/* C5: the groups kept by their hashes, and the rows passed over */
		if (s->topk > 0)
		{
			ExplainPropertyInteger("Groups Kept by Hash", NULL, s->topk, es);
			ExplainPropertyInteger("Rows Passed Over", NULL, s->rows_skipped, es);
		}
		/* X5: tables made arrays of groups, and those hashed after all */
		if (es->verbose && s->array_tables > 0)
			ExplainPropertyText("Group Table",
								psprintf("array of " INT64_FORMAT " slots%s", s->array_slots,
										 s->array_left > 0 ? ", then hashed" : ""), es);
		if (s->frozen_groups > 0)
		{
			ExplainPropertyInteger("Table Frozen At", "groups", s->frozen_groups, es);
			ExplainPropertyInteger("Groups Handed On", NULL, s->passed_groups, es);
			if (s->thawed_after > 0)
				ExplainPropertyInteger("Table Thawed After", "groups handed on",
									   s->thawed_after, es);
			if (s->refrozen_groups > 0)
				ExplainPropertyInteger("Table Frozen Again At", "groups", s->refrozen_groups, es);
		}
		/* D2: the groups packed into the partitions, or merged from them */
		if (s->packed_groups > 0)
			ExplainPropertyInteger("Groups Packed", NULL, s->packed_groups, es);
		if (s->bypassed_rows > 0)
			ExplainPropertyInteger("Rows Bypassing the Table", NULL, s->bypassed_rows, es);
		if (s->unpacked_groups > 0)
			ExplainPropertyInteger("Packed Groups Merged", NULL, s->unpacked_groups, es);
		if (s->topn > 0)
		{
			ExplainPropertyInteger("Top N Kept", NULL, s->ntop, es);
			ExplainPropertyInteger("Groups Ranked", NULL, s->top_ranked, es);
		}
		if (s->spilled_partitions > 0)
		{
			ExplainPropertyInteger("Spilled Partitions", NULL, s->spilled_partitions, es);
			ExplainPropertyInteger("Spilled Rows", NULL, s->spilled_rows, es);
			ExplainPropertyInteger("Spill Depth", NULL, s->max_depth, es);
		}
		if (es->verbose)
		{
			ExplainPropertyInteger("Batches", NULL, node->stats.batches, es);
			if (s->rows_by_postgres > 0)
				ExplainPropertyInteger("Rows Evaluated Row by Row", NULL, s->rows_by_postgres, es);
			/* F26 */
			if (s->consecutive_rows > 0)
				ExplainPropertyInteger("Rows Taking the Row Before's Group", NULL, s->consecutive_rows, es);
			/* F31 */
			if (s->one_key_batches > 0)
				ExplainPropertyInteger("Batches of One Key", NULL, s->one_key_batches, es);
			/* F30 */
			if (s->vector_key_batches > 0)
				ExplainPropertyInteger("Batches Keyed by Columns", NULL, s->vector_key_batches, es);
			/* F32 */
			if (s->run_batches > 0)
			{
				ExplainPropertyInteger("Batches Keyed by Runs", NULL, s->run_batches, es);
				ExplainPropertyInteger("Key Runs", NULL, s->runs_total, es);
			}
		}
	}

	/*
	 * F23: a final stage's sub-buffers, this participant's -- read in runs,
	 * or merged in windows past the memory -- VERBOSE's: a final stage of
	 * packed groups is not one the node's counts above run for
	 */
	if (es->analyze && es->verbose && (s->sub_runs > 0 || s->sub_windows > 0))
	{
		ExplainPropertyInteger("Sub-buffer Runs", NULL, s->sub_runs, es);
		ExplainPropertyInteger("Sub-buffer Windows", NULL, s->sub_windows, es);
		ExplainPropertyInteger("Sub-buffer Records", NULL, s->sub_records, es);
		/* F29: of them, those merged where the exchange held them, those copied, those made groups */
		if (s->sub_referenced > 0 || s->sub_copied > 0)
		{
			ExplainPropertyInteger("Records Referenced", NULL, s->sub_referenced, es);
			ExplainPropertyInteger("Records Copied", NULL, s->sub_copied, es);
			ExplainPropertyInteger("Records Made Groups", NULL, s->rec_groups_made, es);
		}
	}
}
