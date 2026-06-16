/* -------------------------------------------------------------------------
 *
 * pg_delta_table.h
 *      definition of the system "delta table mapping" relation (pg_delta_table)
 *
 * pg_delta_table stores the mapping between Iceberg foreign tables and their
 * corresponding local Delta internal tables: foreign_relid <-> delta_relid
 * <-> foreign_path.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 2021, openGauss Contributors
 *
 * src/include/catalog/pg_delta_table.h
 *
 * NOTES
 *      the genbki.pl script reads this file and generates .bki
 *      information from the DATA() statements.
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_DELTA_TABLE_H
#define PG_DELTA_TABLE_H

#include "catalog/genbki.h"

/* ----------------
 *        pg_delta_table definition.  cpp turns this into
 *        typedef struct FormData_pg_delta_table
 * ----------------
 */
#define DeltaTableRelationId 9994
#define DeltaTableRelation_Rowtype_Id 11663

CATALOG(pg_delta_table,9994) BKI_WITHOUT_OIDS BKI_SCHEMA_MACRO
{
    Oid         delta_relid;        /* OID of the Delta internal table */
    Oid         foreign_relid;      /* OID of the Iceberg foreign table */

#ifdef CATALOG_VARLEN            /* variable-length fields start here */
    text        foreign_path[1];    /* Foreign table data path (e.g. s3://bucket/table/) */
#endif
} FormData_pg_delta_table;

/* ----------------
 *        Form_pg_delta_table corresponds to a pointer to a tuple with
 *        the format of pg_delta_table relation.
 * ----------------
 */
typedef FormData_pg_delta_table *Form_pg_delta_table;

/* ----------------
 *        compiler constants for pg_delta_table
 * ----------------
 */
#define Natts_pg_delta_table                    3
#define Anum_pg_delta_table_delta_relid         1
#define Anum_pg_delta_table_foreign_relid       2
#define Anum_pg_delta_table_foreign_path        3

/* ----------------
 *        pg_delta_table foreign_relid index
 * ----------------
 */
#define DeltaTableForeignRelidIndexId   10007

/* ----------------
 *        pg_delta_table delta_relid index
 * ----------------
 */
#define DeltaTableDeltaRelidIndexId     10008

/* 创建 Delta 表映射记录：将 Delta 内表OID、Iceberg 外表OID 和外表数据路径写入 pg_delta_table，
 * 成功时返回 Delta 内表 OID */
extern Oid CreateDeltaTableMapping(Oid deltaRelid, Oid foreignRelid, const char *foreignPath);
/* 根据外表 OID 查找对应的 Delta 内表 OID，未找到时返回 InvalidOid */
extern Oid LookupDeltaTableByForeignOid(Oid foreignRelid);
/* 根据 Delta 内表 OID 查找对应的 Iceberg 外表 OID，未找到时返回 InvalidOid */
extern Oid LookupDeltaTableByDeltaOid(Oid deltaRelid);
/* 根据外表 OID 删除 pg_delta_table 中的映射记录 */
extern void RemoveDeltaTableMappingByForeignOid(Oid foreignRelid);
/* 根据 Delta 内表 OID 删除 pg_delta_table 中的映射记录 */
extern void RemoveDeltaTableMappingByDeltaOid(Oid deltaRelid);

#endif   /* PG_DELTA_TABLE_H */
