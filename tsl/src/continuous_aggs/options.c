/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>
#include <access/xact.h>
#include <catalog/namespace.h>
#include <commands/view.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <optimizer/optimizer.h>
#include <rewrite/rewriteManip.h>
#include <utils/int8.h>
#include <utils/builtins.h>

#include "options.h"
#include "cache.h"
#include "compression_with_clause.h"
#include "continuous_agg.h"
#include "continuous_aggs/create.h"
#include "compression/create.h"
#include "hypertable_cache.h"
#include "scan_iterator.h"

static void
update_materialized_only(ContinuousAgg *agg, bool materialized_only)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, RowExclusiveLock, CurrentMemoryContext);
	iterator.ctx.index = catalog_get_index(ts_catalog_get(), CONTINUOUS_AGG, CONTINUOUS_AGG_PKEY);

	ts_scan_iterator_scan_key_init(&iterator,
								   Anum_continuous_agg_pkey_mat_hypertable_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(agg->data.mat_hypertable_id));

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		bool nulls[Natts_continuous_agg];
		Datum values[Natts_continuous_agg];
		bool repl[Natts_continuous_agg] = { false };
		bool should_free;
		HeapTuple tuple = ts_scan_iterator_fetch_heap_tuple(&iterator, false, &should_free);
		HeapTuple new_tuple;
		TupleDesc tupdesc = ts_scan_iterator_tupledesc(&iterator);

		heap_deform_tuple(tuple, tupdesc, values, nulls);

		repl[AttrNumberGetAttrOffset(Anum_continuous_agg_materialize_only)] = true;
		values[AttrNumberGetAttrOffset(Anum_continuous_agg_materialize_only)] =
			BoolGetDatum(materialized_only);

		new_tuple = heap_modify_tuple(tuple, tupdesc, values, nulls, repl);

		ts_catalog_update(ti->scanrel, new_tuple);
		heap_freetuple(new_tuple);

		if (should_free)
			heap_freetuple(tuple);

		break;
	}
	ts_scan_iterator_close(&iterator);
}

/*
 * Retrieve the cagg view query and find the groupby clause and
 * time_bucket clause.Map them to the column names(of mat.hypertable)
 * Returns: list of column names used in group by clause of the cagg query.
 */
static List *
cagg_find_groupingcols(ContinuousAgg *agg, Hypertable *mat_ht)
{
	List *retlist = NIL;
	ListCell *lc;
	Oid cagg_view_oid =
		get_relname_relid(NameStr(agg->data.user_view_name),
						  get_namespace_oid(NameStr(agg->data.user_view_schema), false));
	Relation cagg_view_rel = table_open(cagg_view_oid, AccessShareLock);
	RuleLock *cagg_view_rules = cagg_view_rel->rd_rules;
	Assert(cagg_view_rules && cagg_view_rules->numLocks == 1);
	RewriteRule *rule = cagg_view_rules->rules[0];
	if (rule->event != CMD_SELECT)
		elog(ERROR, "rule missing for view ");

	Query *cagg_view_query = copyObject(linitial(rule->actions));
	table_close(cagg_view_rel, NoLock); // lock with be released at end of txn
	List *tlist = cagg_view_query->targetList;
	Oid mat_relid = mat_ht->main_table_relid;
	foreach (lc, cagg_view_query->groupClause)
	{
		SortGroupClause *cagg_gc = (SortGroupClause *) lfirst(lc);
		TargetEntry *cagg_tle = get_sortgroupclause_tle(cagg_gc, tlist);
		/* groupby clauses are columns from the mat hypertable */
		Assert(IsA(cagg_tle->expr, Var));
		Var *mat_var = castNode(Var, cagg_tle->expr);
		char *mat_colname = get_attname(mat_relid, mat_var->varattno, false);
		retlist = lappend(retlist, mat_colname);
	}
	return retlist;
}

static List *
cagg_get_compression_params(ContinuousAgg *agg, Hypertable *mat_ht)
{
	List *defelems = NIL;
	const Dimension *mat_ht_dim = hyperspace_get_open_dimension(mat_ht->space, 0);
	const char *mat_ht_timecolname = NameStr(mat_ht_dim->fd.column_name);
	DefElem *ordby = makeDefElemExtended("timescaledb",
										 "compress_orderby",
										 (Node *) makeString((char *) mat_ht_timecolname),
										 DEFELEM_UNSPEC,
										 -1);
	defelems = lappend(defelems, ordby);
	List *grp_colnames = cagg_find_groupingcols(agg, mat_ht);
	if (grp_colnames)
	{
		ListCell *lc;
		/* we have column names. they are guaranteed to be atmost
		 * NAMEDATALEN
		 */
		int seglen = ((NAMEDATALEN + 1) * list_length(grp_colnames)) + 1;
		char *segmentby = (char *) palloc(seglen);
		int segidx = 0;
		foreach (lc, grp_colnames)
		{
			char *grpcol = (char *) lfirst(lc);
			/* skip time dimension col if it appears in group-by list */
			if (namestrcmp((Name) & (mat_ht_dim->fd.column_name), grpcol) == 0)
				continue;
			int collen = strlen(grpcol);
			if (segidx > 0)
			{
				segmentby = strncat(segmentby + segidx, ",", seglen - collen);
				collen = collen + 1;
			}
			segmentby = strncat(segmentby + segidx, grpcol, seglen - collen);
			segidx = segidx + collen;
		}
		if (segidx != 0)
		{
			DefElem *segby;
			segmentby[segidx] = '\0';
			segby = makeDefElemExtended("timescaledb",
										"compress_segmentby",
										(Node *) makeString(segmentby),
										DEFELEM_UNSPEC,
										-1);
			defelems = lappend(defelems, segby);
		}
	}

	return defelems;
}

/* enable/disable compression on continuous aggregate */
static void
cagg_alter_compression(ContinuousAgg *agg, Hypertable *mat_ht, bool compress_enable)
{
	List *defelems = NIL;
	Assert(mat_ht != NULL);
	if (compress_enable)
		defelems = cagg_get_compression_params(agg, mat_ht);

	DefElem *enable = makeDefElemExtended("timescaledb",
										  "compress",
										  compress_enable ? (Node *) makeString("true") :
															(Node *) makeString("false"),
										  DEFELEM_UNSPEC,
										  -1);
	defelems = lappend(defelems, enable);

	WithClauseResult *with_clause_options = ts_compress_hypertable_set_clause_parse(defelems);
	AlterTableCmd alter_cmd = {
		.type = T_AlterTableCmd,
		.subtype = AT_SetRelOptions,
		.def = (Node *) defelems,
	};

	tsl_process_compress_table(&alter_cmd, mat_ht, with_clause_options);
}

void
continuous_agg_update_options(ContinuousAgg *agg, WithClauseResult *with_clause_options)
{
	if (!with_clause_options[ContinuousEnabled].is_default)
		elog(ERROR, "cannot disable continuous aggregates");

	if (!with_clause_options[ContinuousViewOptionMaterializedOnly].is_default)
	{
		bool materialized_only =
			DatumGetBool(with_clause_options[ContinuousViewOptionMaterializedOnly].parsed);
		if (agg->data.materialized_only != materialized_only)
		{
			Cache *hcache = ts_hypertable_cache_pin();
			Hypertable *mat_ht =
				ts_hypertable_cache_get_entry_by_id(hcache, agg->data.mat_hypertable_id);
			agg->data.materialized_only = materialized_only;
			Assert(mat_ht != NULL);

			cagg_update_view_definition(agg, mat_ht);
			update_materialized_only(agg, agg->data.materialized_only);
			ts_cache_release(hcache);
		}
	}
	if (!with_clause_options[ContinuousViewOptionCompress].is_default)
	{
		bool compress_enable =
			DatumGetBool(with_clause_options[ContinuousViewOptionCompress].parsed);
		Cache *hcache = ts_hypertable_cache_pin();
		Hypertable *mat_ht =
			ts_hypertable_cache_get_entry_by_id(hcache, agg->data.mat_hypertable_id);
		Assert(mat_ht != NULL);

		cagg_alter_compression(agg, mat_ht, compress_enable);
		ts_cache_release(hcache);
	}
	if (!with_clause_options[ContinuousViewOptionCreateGroupIndex].is_default)
	{
		elog(ERROR, "cannot alter create_group_indexes option for continuous aggregates");
	}
}
