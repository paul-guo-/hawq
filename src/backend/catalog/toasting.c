/*-------------------------------------------------------------------------
 *
 * toasting.c
 *	  This file contains routines to support creation of toast tables
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/toasting.c,v 1.3 2006/10/04 00:29:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/tuptoaster.h"
#include "access/xact.h"
#include "catalog/catquery.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "catalog/toasting.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/guc.h"

#include "cdb/cdbvars.h"

static bool create_toast_table(Relation rel, Oid toastOid, Oid toastIndexOid,
							   Oid *comptypeOid, bool is_part_child);


/*
 * AlterTableCreateToastTable
 *		If the table needs a toast table, and doesn't already have one,
 *		then create a toast table for it.
 *
 * We expect the caller to have verified that the relation is a table and have
 * already done any necessary permission checks.  Callers expect this function
 * to end with CommandCounterIncrement if it makes any changes.
 */
void
AlterTableCreateToastTable(Oid relOid)
{
	Relation	rel;
	bool is_part_child = !rel_needs_long_lock(relOid);

	/*
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.  (This is probably redundant in all present
	 * uses...)
	 */
	rel = heap_open(relOid, AccessExclusiveLock);

	/* create_toast_table does all the work */
	(void) create_toast_table(rel, InvalidOid, InvalidOid, NULL, is_part_child);

	heap_close(rel, NoLock);
}

void
AlterTableCreateToastTableWithOid(Oid relOid, Oid newOid, Oid newIndexOid,
								  Oid * comptypeOid, bool is_part_child)
{
	Relation	rel;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.  (This is probably redundant in all present
	 * uses...)
	 */
	if (is_part_child)
		rel = heap_open(relOid, NoLock);
	else
		rel = heap_open(relOid, AccessExclusiveLock);



	/* create_toast_table does all the work */
	(void) create_toast_table(rel, newOid, newIndexOid,
							  comptypeOid, is_part_child);

	heap_close(rel, NoLock);
}


/*
 * Create a toast table during bootstrap
 *
 * Here we need to prespecify the OIDs of the toast table and its index
 */
void
BootstrapToastTable(char *relName, Oid toastOid, Oid toastIndexOid)
{
	Relation	rel;
	Oid         typid;

	rel = heap_openrv(makeRangeVar(NULL /*catalogname*/, NULL, relName, -1), AccessExclusiveLock);

	/* Note: during bootstrap may see uncataloged relation */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_UNCATALOGED)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						relName)));

	/* 
	 * In order to be able to support upgrade we need to be able to
	 * support toast tables with fixed oids.  Toast tables created
	 * pre 4.0 can continue to be dynamic, but anything new must
	 * be declared in pg_type.h and again here. 
	 */
	switch (toastOid)
	{
		/* New Toast Tables in 4.0 */

/* TIDYCAT_BEGIN_CODEGEN 
*/
/*
   WARNING: DO NOT MODIFY THE FOLLOWING SECTION: 
   Generated by ./tidycat.pl version 31
   on Thu Sep  1 16:43:17 2011
 */
/* relation id: 5033 - pg_filespace_entry 20101122 */
		case PgFileSpaceEntryToastTable:
			typid = PG_FILESPACE_ENTRY_TOAST_RELTYPE_OID;
			break;
/* relation id: 5036 - gp_segment_configuration 20101122 */
		case GpSegmentConfigToastTable:
			typid = GP_SEGMENT_CONFIGURATION_TOAST_RELTYPE_OID;
			break;
/* relation id: 3231 - pg_attribute_encoding 20110727 */
		case PgAttributeEncodingToastTable:
			typid = PG_ATTRIBUTE_ENCODING_TOAST_RELTYPE_OID;
			break;
/* relation id: 3220 - pg_type_encoding 20110727 */
		case PgTypeEncodingToastTable:
			typid = PG_TYPE_ENCODING_TOAST_RELTYPE_OID;
			break;
/* relation id: 9903 - pg_partition_encoding 20110814 */
		case PgPartitionEncodingToastTable:
			typid = PG_PARTITION_ENCODING_TOAST_RELTYPE_OID;
			break;
/* relation id: 5080 - pg_filesystem 20120903 */
		case PgFileSystemToastTable:
			typid = PG_FILESYSTEM_TOAST_RELTYPE_OID;
			break;

/* relation id: 7076 - pg_remote_credentials 20140205 */
		case PgRemoteCredentialsToastTable:
			typid = PG_REMOTE_CREDENTIALS_TOAST_RELTYPE_OID;
			break;

/* relation id: 6026 - pg_resqueue 20140917 */
		case PgResQueueToastTable:
			typid = PG_RESQUEUE_TOAST_RELTYPE_OID;
			break;

/* TIDYCAT_END_CODEGEN */
			
		default:
			typid = InvalidOid;
			break;
	}

	/* create_toast_table does all the work */
	if (!create_toast_table(rel, toastOid, toastIndexOid, &typid, false))
		elog(ERROR, "\"%s\" does not require a toast table",
			 relName);

	heap_close(rel, NoLock);
}


/*
 * create_toast_table --- internal workhorse
 *
 * rel is already opened and exclusive-locked
 * toastOid and toastIndexOid are normally InvalidOid, but during
 * bootstrap they can be nonzero to specify hand-assigned OIDs
 */
static bool
create_toast_table(Relation rel, Oid toastOid, Oid toastIndexOid,
				   Oid *comptypeOid, bool is_part_child)
{
	Oid			relOid = RelationGetRelid(rel);
	HeapTuple	reltup;
	TupleDesc	tupdesc;
	bool		shared_relation;
	Relation	class_rel;
	Oid			toast_relid;
	Oid			toast_idxid;
	char		toast_relname[NAMEDATALEN];
	char		toast_idxname[NAMEDATALEN];
	IndexInfo  *indexInfo;
	Oid			classObjectId[2];
	ObjectAddress baseobject,
				toastobject;
	cqContext	cqc;
	cqContext  *pcqCtx;
	Oid			tablespaceOid = ChooseTablespaceForLimitedObject(rel->rd_rel->reltablespace);

	/*
	 * Is it already toasted?
	 */
	if (!gp_upgrade_mode && rel->rd_rel->reltoastrelid != InvalidOid)
		return false;

	/*
	 * Check to see whether the table actually needs a TOAST table.
	 */
	if (!RelationNeedsToastTable(rel))
		return false;

	/*
	 * If we're in upgrade mode, and we say we don't want a toast table
	 * (by having an InvalidOid for toast Oid).
	 */
	if (gp_upgrade_mode && toastOid==InvalidOid)
		return false;

	/*
	 * Toast table is shared if and only if its parent is.
	 *
	 * We cannot allow toasting a shared relation after initdb (because
	 * there's no way to mark it toasted in other databases' pg_class).
	 */
	shared_relation = rel->rd_rel->relisshared;
	if (shared_relation && !IsBootstrapProcessingMode()&&!gp_upgrade_mode)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("shared tables cannot be toasted after initdb")));

	/*
	 * Create the toast table and its index
	 */
	snprintf(toast_relname, sizeof(toast_relname),
			 "pg_toast_%u", relOid);
	snprintf(toast_idxname, sizeof(toast_idxname),
			 "pg_toast_%u_index", relOid);

	/* this is pretty painful...  need a tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(3, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1,
					   "chunk_id",
					   OIDOID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2,
					   "chunk_seq",
					   INT4OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3,
					   "chunk_data",
					   BYTEAOID,
					   -1, 0);

	/*
	 * Ensure that the toast table doesn't itself get toasted, or we'll be
	 * toast :-(.  This is essential for chunk_data because type bytea is
	 * toastable; hit the other two just to be sure.
	 */
	tupdesc->attrs[0]->attstorage = 'p';
	tupdesc->attrs[1]->attstorage = 'p';
	tupdesc->attrs[2]->attstorage = 'p';

	/*
	 * Note: the toast relation is placed in the regular pg_toast namespace
	 * even if its master relation is a temp table.  There cannot be any
	 * naming collision, and the toast rel will be destroyed when its master
	 * is, so there's no need to handle the toast rel as temp.
	 *
	 * XXX would it make sense to apply the master's reloptions to the toast
	 * table?
	 */
	toast_relid = heap_create_with_catalog(toast_relname,
										   PG_TOAST_NAMESPACE,
										   tablespaceOid,
										   toastOid,
										   rel->rd_rel->relowner,
										   tupdesc,
										   /* relam */ InvalidOid,
										   RELKIND_TOASTVALUE,
										   RELSTORAGE_HEAP,
										   shared_relation,
										   true,
										   /* bufferPoolBulkLoad */ false,
										   0,
										   ONCOMMIT_NOOP,
										   NULL, /* CDB POLICY */
										   (Datum) 0,
										   true,
										   comptypeOid,
										   /* persistentTid */ NULL,
										   /* persistentSerialNum */ NULL,
										   /* formattername */ NULL);

	/* make the toast relation visible, else index creation will fail */
	CommandCounterIncrement();

	/*
	 * Create unique index on chunk_id, chunk_seq.
	 *
	 * NOTE: the normal TOAST access routines could actually function with a
	 * single-column index on chunk_id only. However, the slice access
	 * routines use both columns for faster access to an individual chunk. In
	 * addition, we want it to be unique as a check against the possibility of
	 * duplicate TOAST chunk OIDs. The index might also be a little more
	 * efficient this way, since btree isn't all that happy with large numbers
	 * of equal keys.
	 */

	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_NumIndexAttrs = 2;
	indexInfo->ii_KeyAttrNumbers[0] = 1;
	indexInfo->ii_KeyAttrNumbers[1] = 2;
	indexInfo->ii_Expressions = NIL;
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_Predicate = NIL;
	indexInfo->ii_PredicateState = NIL;
	indexInfo->ii_Unique = true;
	indexInfo->ii_Concurrent = false;

	classObjectId[0] = OID_BTREE_OPS_OID;
	classObjectId[1] = INT4_BTREE_OPS_OID;

	toast_idxid = index_create(toast_relid, toast_idxname, toastIndexOid,
							   indexInfo,
							   BTREE_AM_OID,
							   tablespaceOid,
							   classObjectId, (Datum) 0,
							   true, false, (Oid *) NULL, true, false, false, NULL);

	/*
	 * If this is a partitioned child, we can unlock since the master is
	 * already locked.
	 */
	if (is_part_child)
	{
		UnlockRelationOid(toast_relid, ShareLock);
		UnlockRelationOid(toast_idxid, AccessExclusiveLock);
	}

	/*
	 * Store the toast table's OID in the parent relation's pg_class row
	 */
	class_rel = heap_open(RelationRelationId, RowExclusiveLock);

	pcqCtx = caql_addrel(cqclr(&cqc), class_rel);

	reltup = caql_getfirst(
			pcqCtx,
			cql("SELECT * FROM pg_class "
				" WHERE oid = :1 "
				" FOR UPDATE ",
				ObjectIdGetDatum(relOid)));

	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);

	((Form_pg_class) GETSTRUCT(reltup))->reltoastrelid = toast_relid;

	if (!IsBootstrapProcessingMode())
	{
		/* normal case, use a transactional update */
		caql_update_current(pcqCtx, reltup);
		/* and Update indexes (implicit) */
	}
	else
	{
		/* While bootstrapping, we cannot UPDATE, so overwrite in-place */
		heap_inplace_update(class_rel, reltup);
	}

	heap_freetuple(reltup);

	heap_close(class_rel, RowExclusiveLock);

	/*
	 * Register dependency from the toast table to the master, so that the
	 * toast table will be deleted if the master is.  Skip this in bootstrap
	 * mode.
	 */
	if (!IsBootstrapProcessingMode())
	{
		baseobject.classId = RelationRelationId;
		baseobject.objectId = relOid;
		baseobject.objectSubId = 0;
		toastobject.classId = RelationRelationId;
		toastobject.objectId = toast_relid;
		toastobject.objectSubId = 0;

		recordDependencyOn(&toastobject, &baseobject, DEPENDENCY_INTERNAL);
	}

	/*
	 * Make changes visible
	 */
	CommandCounterIncrement();

	return true;
}

/*
 * Check to see whether the table needs a TOAST table.	It does only if
 * (1) there are any toastable attributes, and (2) the maximum length
 * of a tuple could exceed TOAST_TUPLE_THRESHOLD.  (We don't want to
 * create a toast table for something like "f1 varchar(20)".)
 */
bool
RelationNeedsToastTable(Relation rel)
{
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc;
	Form_pg_attribute *att;
	int32		tuple_length;
	int			i;

	if(RelationIsExternal(rel))
		return false;
	
	/*
	 * in hawq, we cannot use toast table for dispatched table.
	 */
	if (RelationIsAo(rel))
		return false ;

	tupdesc = rel->rd_att;
	att = tupdesc->attrs;

	for (i = 0; i < tupdesc->natts; i++)
	{
		if (att[i]->attisdropped)
			continue;
		data_length = att_align(data_length, att[i]->attalign);
		if (att[i]->attlen > 0)
		{
			/* Fixed-length types are never toastable */
			data_length += att[i]->attlen;
		}
		else
		{
			int32		maxlen = type_maximum_size(att[i]->atttypid,
												   att[i]->atttypmod);

			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att[i]->attstorage != 'p')
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
		return false;			/* nothing to toast? */
	if (maxlength_unknown)
		return true;			/* any unlimited-length attrs? */
	tuple_length = MAXALIGN(offsetof(HeapTupleHeaderData, t_bits) +
							BITMAPLEN(tupdesc->natts)) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
}
