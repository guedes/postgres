/*------------------------------------------------------------------------
 * colstore_dummy.c
 * 		Simple column store implementation for POSTGRES
 *
 * Copyright (c) 2015, PostgreSQL Global Development Group
 *
 * src/backend/colstore/colstore_dummy.c
 *
 *------------------------------------------------------------------------
 */
#include "postgres.h"

#include "colstore/colstoreapi.h"
#include "colstore/colstore_dummy.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(cstore_dummy_handler);

static void cstore_dummy_insert(Relation rel,
				Relation colstorerel, ColumnStoreInfo *info,
				int natts, Datum *values, bool *nulls,
				ItemPointer tupleid);
static void cstore_dummy_batch_insert(Relation rel,
				Relation colstorerel, ColumnStoreInfo *info,
				int nrows, int natts, Datum **values, bool **nulls,
				ItemPointer *tupleids);
static Buffer get_colstore_buffer(Relation rel, Relation colstore);
static int	ColumnarPageGetFreeItems(ColumnarPage page);


Datum
cstore_dummy_handler(PG_FUNCTION_ARGS)
{
        ColumnStoreRoutine *routine = makeNode(ColumnStoreRoutine);

		routine->ExecColumnStoreInsert = cstore_dummy_insert;
		routine->ExecColumnStoreBatchInsert = cstore_dummy_batch_insert;

        PG_RETURN_POINTER(routine);
}

static void
cstore_dummy_insert(Relation rel,
					Relation colstorerel, ColumnStoreInfo *info,
					int natts, Datum *values, bool *nulls,
					ItemPointer tupleid)
{
	int i;
	Buffer 				buffer = get_colstore_buffer(rel, colstorerel);
	ColumnarPage 		page = BufferGetColumnarPage(buffer);
	ColumnarPageHeader	header = (ColumnarPageHeader)page;

	/* how many free item slots are on the current page? */
	int				nitems = ColumnarPageGetFreeItems(page);

	Assert(nitems > 0);

	for (i = 0; i < header->pd_ncolumns; i++)
	{
		int byteIdx = (header->pd_nitems) / 8;
		int bitIdx  = (header->pd_nitems) % 8;

		/* copy the data in place */
		memcpy(PageGetColumnDataNext(page, i),
			   &values[i], PageGetColumnAttlen(page, i));

		PageGetColumnDataAddBytes(page, i, PageGetColumnAttlen(page,i));

		/* set the NULL bitmap */
		*(PageGetColumnNulls(page, i) + byteIdx) &= (0x01 << bitIdx);
		PageGetColumnNullsSetBytes(page, i, (byteIdx+1));
	}

	/* now set tuple ID */
	memcpy(PageGetNextTupleId(page), tupleid, sizeof(ItemPointerData));

	/* FIXME update min/max TID */

	/* update number of items on the page */
	header->pd_nitems += 1;

	Assert(header->pd_nitems <= header->pd_maxitems);

	PageSetChecksumInplace((Page)page, BufferGetBlockNumber(buffer));

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
}

static void
cstore_dummy_batch_insert(Relation rel,
				Relation colstorerel, ColumnStoreInfo *info,
				int nrows, int natts, Datum **values, bool **nulls,
				ItemPointer *tupleids)
{
	int		i,
			j;
	int		first = 0;

	while (first < nrows)
	{
		Buffer 			buffer = get_colstore_buffer(rel, colstorerel);
		ColumnarPage 	page = BufferGetColumnarPage(buffer);
		ColumnarPageHeader header = (ColumnarPageHeader)page;

		/* how many free item slots are on the current page? */
		int				nitems = ColumnarPageGetFreeItems(page);

		Assert(nitems > 0);

		nitems = (nitems < (nrows - first)) ? nitems : (nrows - first);

		for (i = 0; i < header->pd_ncolumns; i++)
		{
			for (j = 0; j < nitems; j++)
			{
				int byteIdx = (header->pd_nitems + j) / 8;
				int bitIdx  = (header->pd_nitems + j) % 8;

				/* copy the data in place */
				memcpy(PageGetColumnDataNext(page, i),
					   &values[i][first+j], PageGetColumnAttlen(page, i));

				PageGetColumnDataAddBytes(page, i, PageGetColumnAttlen(page,i));

				/* set the NULL bitmap */
				*(PageGetColumnNulls(page, i) + byteIdx) &= (0x01 << bitIdx);
				PageGetColumnNullsSetBytes(page, i, (byteIdx+1));
			}
		}

		/* now set tuple IDs */
		for (i = 0; i < nitems; i++)
			memcpy(PageGetNextTupleId(page) + i * sizeof(ItemPointerData),
				   &tupleids[i], sizeof(ItemPointerData));

		/* FIXME update min/max TID */

		/* update number of items on the page */
		header->pd_nitems += nitems;
		first += nitems;

		Assert(header->pd_nitems <= header->pd_maxitems);

		PageSetChecksumInplace((Page)page, BufferGetBlockNumber(buffer));

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}
}

void
ColumnarPageInit(ColumnarPage page, Size pageSize, Relation rel)
{
	int 				i;
	ColumnarPageHeader	header;
	TupleDesc			tupdesc;
	Size				itemsize;
	Size				freespace;
	int					maxtuples = 0;
	int					natts;
	int					nnulls = 0;

	/* zero the page first */
	memset(page, 0, pageSize);

	tupdesc = RelationGetDescr(rel);
	natts   = tupdesc->natts;

	header = (ColumnarPageHeader)page;

	header->pd_ncolumns = natts;
	header->pd_flags = 0;

	/*
	 * Set the pd_lower/upper/special in a sensible way - we don't use special
	 * space, so we'll set pd_special to pageSize. And we'll set both pd_lower
	 * and pd_upper right after the column info array, So the page seems to be
	 * entirely full (pd_upper-pd_lower==0).
	 *
	 * XXX An alternative might be to store the column info structs in the
	 *     special section, not sure if that's better.
	 */
	header->pd_lower = offsetof(ColumnarPageHeaderData, pd_columns)
						 - natts * sizeof(ColumnInfoData);
	header->pd_upper = header->pd_lower;
	header->pd_special = pageSize;	/* no special */

	PageSetPageSizeAndVersion(page, pageSize, PG_PAGE_LAYOUT_VERSION);

	/* we need an item pointer for each 'row' */
	itemsize = sizeof(ItemPointerData);

	/* compute size of a single 'row' added to the page */
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attlen < 0)
			elog(ERROR, "variable-length data types not supported yet");

		itemsize += tupdesc->attrs[i]->attlen;

		header->pd_columns[i].attnum     = tupdesc->attrs[i]->attnum;
		header->pd_columns[i].attlen     = tupdesc->attrs[i]->attlen;
		header->pd_columns[i].atttypid   = tupdesc->attrs[i]->atttypid;
		header->pd_columns[i].attnotnull = tupdesc->attrs[i]->attnotnull;

		nnulls += (header->pd_columns[i].attnotnull) ? 0 : 1;
	}

	freespace = pageSize - offsetof(ColumnarPageHeaderData, pd_columns)
						 - natts * sizeof(ColumnInfoData);

	/*
	 * We'll do a bit arithmetics magic, because we need to include NULLs,
	 * because 8 rows needs 1 byte in NULL bitmap
	 */
	maxtuples = 8 * freespace / (itemsize * 8 + nnulls);

	/*
	 * We haven't considered alignment yet, so let's see if we fit on the page
	 * (and if not, decrement the number of items until we do).
	 */
	while (true)
	{
		Size	offset = offsetof(ColumnarPageHeaderData, pd_columns)
						 + natts * sizeof(ColumnInfoData);

		for (i = 0; i < natts; i++)
		{
			offset = MAXALIGN(offset);
			header->pd_columns[i].data_start = offset;

			/* space for data */
			offset += maxtuples * tupdesc->attrs[i]->attlen;

			offset = MAXALIGN(offset);
			header->pd_columns[i].null_start = offset;

			/* NULL bitmap size */
			offset += (maxtuples + 7) / 8;
		}

		/* and finally one item pointer for each row */
		offset = MAXALIGN(offset);

		header->pd_tupleids = offset;
		offset += maxtuples * sizeof(ItemPointerData);

		/* if we fit onto a page, terminate, otherwise decrement maxtuples */
		if (offset <= pageSize)
			break;

		maxtuples--;
	}

	/* remember the max number of tuples */
	header->pd_maxitems = maxtuples;

	return;
}

static Buffer
get_colstore_buffer(Relation rel, Relation colstore)
{
	Buffer			buffer;
	ColumnarPage	page;
	BlockNumber		targetBlock = InvalidBlockNumber;
	bool			needLock = !RELATION_IS_LOCAL(rel);	/* check the parent */
	BlockNumber		nblocks = RelationGetNumberOfBlocks(colstore);

	/* we'll always try the last block first, and then possibly extend */
	if (nblocks > 0)
		targetBlock = nblocks - 1;

	/* get the last block (if the relation is empty, just do the extension) */
	if (targetBlock != InvalidBlockNumber)
	{
		buffer = ReadBuffer(colstore, targetBlock);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

		page = BufferGetColumnarPage(buffer);

		/* if there's enough space for another item, we're done */
		if (ColumnarPageGetFreeItems(page) > 0)
			return buffer;

		/* otherwise, let's allocate a new page at the end */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}

	if (needLock)
		LockRelationForExtension(colstore, ExclusiveLock);

	buffer = ReadBuffer(colstore, P_NEW);

	/*
	 * Now acquire lock on the new page.
	 */
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	if (needLock)
		UnlockRelationForExtension(colstore, ExclusiveLock);

	page = BufferGetColumnarPage(buffer);

	ColumnarPageInit(page, BufferGetPageSize(buffer), colstore);

	RelationSetTargetBlock(colstore, BufferGetBlockNumber(buffer));

	MarkBufferDirty(buffer);

	return buffer;
}

static int
ColumnarPageGetFreeItems(ColumnarPage page)
{
	ColumnarPageHeader header = (ColumnarPageHeader) page;

	return (header->pd_maxitems - header->pd_nitems);
}
