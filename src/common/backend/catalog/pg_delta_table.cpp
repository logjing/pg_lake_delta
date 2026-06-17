/*
 * -------------------------------------------------------------------------
 *
 * pg_delta_table.cpp
 *      routines to support manipulation of the pg_delta_table relation
 *
 * This catalog stores the mapping between Iceberg foreign tables and their
 * corresponding local Delta internal tables.
 *
 * IDENTIFICATION
 *    src/common/backend/catalog/pg_delta_table.cpp
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_delta_table.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * CreateDeltaTableMapping - 向 pg_delta_table 中插入一条新的映射记录。
 * 将 Delta 内表OID、Iceberg 外表OID 和外表数据路径写入 catalog，
 * 成功时返回 Delta 内表 OID。
 */
Oid CreateDeltaTableMapping(Oid deltaRelid, Oid foreignRelid, const char *foreignPath)
{
    Relation deltaRel;
    Datum values[Natts_pg_delta_table];
    bool nulls[Natts_pg_delta_table];
    HeapTuple tuple;
    errno_t rc = 0;

    Assert(OidIsValid(deltaRelid));
    Assert(OidIsValid(foreignRelid));

    rc = memset_s(values, sizeof(values), 0, sizeof(values));
    securec_check(rc, "\0", "\0");
    rc = memset_s(nulls, sizeof(nulls), false, sizeof(nulls));
    securec_check(rc, "\0", "\0");

    values[Anum_pg_delta_table_delta_relid - 1] = ObjectIdGetDatum(deltaRelid);
    values[Anum_pg_delta_table_foreign_relid - 1] = ObjectIdGetDatum(foreignRelid);

    if (foreignPath != NULL) {
        Datum textDatum = CStringGetTextDatum(foreignPath);
        values[Anum_pg_delta_table_foreign_path - 1] = PointerGetDatum(
            construct_array(&textDatum, 1, TEXTOID, -1, false, 'i'));
    } else {
        nulls[Anum_pg_delta_table_foreign_path - 1] = true;
    }

    deltaRel = heap_open(DeltaTableRelationId, RowExclusiveLock);

    tuple = heap_form_tuple(deltaRel->rd_att, values, nulls);

    (void)simple_heap_insert(deltaRel, tuple);
    CatalogUpdateIndexes(deltaRel, tuple);

    heap_freetuple(tuple);
    heap_close(deltaRel, RowExclusiveLock);

    CommandCounterIncrement();

    return deltaRelid;
}

/*
 * register_delta_mapping - SQL 可调用包装函数，将外表 OID ↔ Delta 内表 OID
 * 的映射关系注册到 pg_delta_table catalog 中。
 * 用法: SELECT register_delta_mapping(delta_oid, foreign_oid, 's3://path/');
 * 成功时返回 Delta 内表 OID。
 */
PG_FUNCTION_INFO_V1(register_delta_mapping);
Datum register_delta_mapping(PG_FUNCTION_ARGS)
{
    Oid deltaRelid = PG_GETARG_OID(0);
    Oid foreignRelid = PG_GETARG_OID(1);
    text *foreignPathText = PG_ARGISNULL(2) ? NULL : PG_GETARG_TEXT_PP(2);
    char *foreignPath = foreignPathText ? text_to_cstring(foreignPathText) : NULL;

    Oid result = CreateDeltaTableMapping(deltaRelid, foreignRelid, foreignPath);
    PG_RETURN_OID(result);
}

/*
 * LookupDeltaTableByForeignOid - 根据外表 OID 查找对应的 Delta 内表 OID。
 * 未找到映射时返回 InvalidOid。
 */
Oid LookupDeltaTableByForeignOid(Oid foreignRelid)
{
    Relation deltaRel;
    ScanKeyData key[1];
    SysScanDesc scan;
    HeapTuple tuple;
    Oid deltaRelid = InvalidOid;

    Assert(OidIsValid(foreignRelid));

    deltaRel = heap_open(DeltaTableRelationId, AccessShareLock);

    ScanKeyInit(&key[0],
                Anum_pg_delta_table_foreign_relid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(foreignRelid));

    scan = systable_beginscan(deltaRel, DeltaTableForeignRelidIndexId, true, NULL, 1, key);

    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple)) {
        deltaRelid = ((Form_pg_delta_table)GETSTRUCT(tuple))->delta_relid;
    }

    systable_endscan(scan);
    heap_close(deltaRel, AccessShareLock);

    return deltaRelid;
}

/*
 * LookupDeltaTableByDeltaOid - 根据 Delta 内表 OID 查找对应的 Iceberg 外表 OID。
 * 未找到映射时返回 InvalidOid。
 */
Oid LookupDeltaTableByDeltaOid(Oid deltaRelid)
{
    Relation deltaRel;
    ScanKeyData key[1];
    SysScanDesc scan;
    HeapTuple tuple;
    Oid foreignRelid = InvalidOid;

    Assert(OidIsValid(deltaRelid));

    deltaRel = heap_open(DeltaTableRelationId, AccessShareLock);

    ScanKeyInit(&key[0],
                Anum_pg_delta_table_delta_relid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(deltaRelid));

    scan = systable_beginscan(deltaRel, DeltaTableDeltaRelidIndexId, true, NULL, 1, key);

    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple)) {
        foreignRelid = ((Form_pg_delta_table)GETSTRUCT(tuple))->foreign_relid;
    }

    systable_endscan(scan);
    heap_close(deltaRel, AccessShareLock);

    return foreignRelid;
}

/*
 * RemoveDeltaTableMappingByForeignOid - 根据外表 OID 删除 pg_delta_table 中的映射记录。
 */
void RemoveDeltaTableMappingByForeignOid(Oid foreignRelid)
{
    Relation deltaRel;
    ScanKeyData key[1];
    SysScanDesc scan;
    HeapTuple tuple;

    Assert(OidIsValid(foreignRelid));

    deltaRel = heap_open(DeltaTableRelationId, RowExclusiveLock);

    ScanKeyInit(&key[0],
                Anum_pg_delta_table_foreign_relid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(foreignRelid));

    scan = systable_beginscan(deltaRel, DeltaTableForeignRelidIndexId, true, NULL, 1, key);

    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple)) {
        simple_heap_delete(deltaRel, &tuple->t_self);
    }

    systable_endscan(scan);
    heap_close(deltaRel, RowExclusiveLock);

    CommandCounterIncrement();
}

/*
 * RemoveDeltaTableMappingByDeltaOid - 根据 Delta 内表 OID 删除 pg_delta_table 中的映射记录。
 */
void RemoveDeltaTableMappingByDeltaOid(Oid deltaRelid)
{
    Relation deltaRel;
    ScanKeyData key[1];
    SysScanDesc scan;
    HeapTuple tuple;

    Assert(OidIsValid(deltaRelid));

    deltaRel = heap_open(DeltaTableRelationId, RowExclusiveLock);

    ScanKeyInit(&key[0],
                Anum_pg_delta_table_delta_relid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(deltaRelid));

    scan = systable_beginscan(deltaRel, DeltaTableDeltaRelidIndexId, true, NULL, 1, key);

    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple)) {
        simple_heap_delete(deltaRel, &tuple->t_self);
    }

    systable_endscan(scan);
    heap_close(deltaRel, RowExclusiveLock);

    CommandCounterIncrement();
}
