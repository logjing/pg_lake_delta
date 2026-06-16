# PG Lake Delta — 需求与技术方案

## 1. 需求概述

在 openGauss 中引入"Delta 表"机制，使 Iceberg 外表的 INSERT 操作不直接写入外表，而是先写入对应的本地 Delta 内表，后续批量同步到 Iceberg 外表的逻辑由后续团队实现。

核心目标：
- **INSERT 截流**：对外表的 INSERT 改路至 Delta 内表
- **映射持久化**：在 catalog 中记录 外表OID ↔ Delta表OID ↔ 外表路径 的三元关系

## 2. 关键代码路径分析

### 2.1 外表 INSERT 的执行路径

用户执行 `INSERT INTO foreign_table ...` 时，执行流程为：

```
解析 → 重写 → 规划 → 执行
```

在执行层，核心拦截点在 `nodeModifyTable.cpp:ExecInsertT()`：

```cpp
// nodeModifyTable.cpp, 约1192行
} else if (result_rel_info->ri_FdwRoutine) {
    // 外表插入：直接调用 FDW 的 ExecForeignInsert
    slot = result_rel_info->ri_FdwRoutine->ExecForeignInsert(estate, result_rel_info, slot, planSlot);
}
```

当 `ResultRelInfo.ri_FdwRoutine` 非空时（目标关系为外表），整个 INSERT 直接交给 FDW 回调处理，不走 heap_insert 路径。

### 2.2 外表定义的存储

外表信息存储在 `pg_foreign_table` catalog（OID 3118）：

```cpp
// pg_foreign_table.h
CATALOG(pg_foreign_table,3118) BKI_WITHOUT_OIDS BKI_SCHEMA_MACRO
{
    Oid  ftrelid;       // 外表 OID
    Oid  ftserver;      // 外部服务器 OID
    bool ftwriteonly;   // 是否只写
    text ftoptions[1];  // FDW 选项（含 location/path 等）
};
```

`ForeignTable` 运行时结构（`foreign.h`）：

```cpp
typedef struct ForeignTable {
    Oid  relid;       // 外表 OID
    Oid  serverid;    // 服务器 OID
    List* options;    // ftoptions（含 location/foldername 等）
    bool write_only;
} ForeignTable;
```

### 2.3 ResultRelInfo 结构

```cpp
// execnodes.h, 543行
typedef struct ResultRelInfo {
    Relation ri_RelationDesc;       // 目标关系的 Relation 描述符
    struct FdwRoutine* ri_FdwRoutine; // FDW 回调（外表才有）
    void*   ri_FdwState;            // FDW 私有状态
    ...
};
```

### 2.4 外表类型判定

```cpp
// rel_gs.h
#define RelationIsForeignTable(relation) \
    (RELKIND_FOREIGN_TABLE == (relation)->rd_rel->relkind)

// pg_class.h
#define RELKIND_FOREIGN_TABLE 'f'
#define RELKIND_RELATION 'r'
```

Iceberg 外表的 server type 需要新增一个标识（如 `T_ICEBERG_SERVER`），用于在执行路径中区分 Iceberg 外表与其他外表（postgres_fdw、mysql_fdw 等）。

## 3. 技术方案

### 3.1 新增 catalog：`pg_delta_table`

新增系统 catalog 表存储外表与 Delta 表的映射关系。

**定义**（`src/include/catalog/pg_delta_table.h`）：

```cpp
CATALOG(pg_delta_table,<新OID>) BKI_WITHOUT_OIDS BKI_SCHEMA_MACRO
{
    Oid  delta_relid;     // Delta 内表 OID
    Oid  foreign_relid;   // Iceberg 外表 OID
    text foreign_path[1]; // 外表的实际路径（如 s3://bucket/table/）
};
```

字段说明：
| 字段 | 类型 | 说明 |
|------|------|------|
| `delta_relid` | Oid | Delta 内表的 OID（对应 `pg_class` 中的行） |
| `foreign_relid` | Oid | Iceberg 外表的 OID（对应 `pg_class` + `pg_foreign_table` 中的行） |
| `foreign_path` | text | 外表数据路径，从外表的 `location`/`foldername` option 中提取 |

**索引**：
- 在 `foreign_relid` 上建唯一索引，用于快速查找"给定外表 OID → 对应 Delta 表"
- 在 `delta_relid` 上建唯一索引，用于反向查找

**配套工作**：
- 在 `src/include/catalog/indexing.h` 中注册索引声明
- 在 `src/gausskernel/catalog/` 中添加 catalog 的 CRUD 函数
- 在 `src/bin/initdb/initdb.cpp` 中确保 initdb 时创建该 catalog
- 在 `pg_class` 的 `relkind` 列中，Delta 表使用 `RELKIND_RELATION ('r')`，即普通内部表

### 3.2 Delta 表的创建机制

**方案选择：手动注册 + 辅助函数**

不自动在 CREATE FOREIGN TABLE 时创建 Delta 表（避免侵入DDL路径过深），而是提供辅助函数：

```sql
-- 创建 Delta 表并注册映射
SELECT create_delta_table('iceberg_foreign_table_name');

-- 或分步操作：
-- 1. 手动创建与外表同 schema 的普通表
CREATE TABLE delta_xxx ( ... ) ;  -- schema 与外表一致

-- 2. 注册映射关系
SELECT register_delta_mapping(
    delta_table_oid,
    foreign_table_oid,
    's3://my-bucket/iceberg/table/'
);
```

**最小实现策略**：先实现 `register_delta_mapping()` 函数（直接插入 `pg_delta_table` 行），`create_delta_table()` 可后续补齐自动建表逻辑。

**`create_delta_table()` 的实现思路**：
1. 获取外表的 schema（从 `pg_attribute` 读取列定义）
2. 在同一 schema 下创建同结构的普通内部表，表名约定为 `delta_<foreign_table_name>`
3. 将三元组写入 `pg_delta_table`
4. 建立依赖关系（`pg_depend`），确保删外表时联动删 Delta 表

### 3.3 INSERT 截流：改路至 Delta 表

这是核心修改点。在 `ExecInsertT()` 中，当目标关系是 Iceberg 外表时，将 INSERT 重定向到对应的 Delta 内表。

**修改位置**：`src/gausskernel/runtime/executor/nodeModifyTable.cpp` 的 `ExecInsertT()` 函数。

**修改策略**：

```cpp
// 在 ExecInsertT 中，原代码约1192行的分支：
} else if (result_rel_info->ri_FdwRoutine) {
    // === 新增：Iceberg 外表截流逻辑 ===
    if (IsIcebergForeignTable(result_relation_desc)) {
        Oid delta_oid = LookupDeltaTableByForeignOid(result_relation_desc->rd_id);
        if (OidIsValid(delta_oid)) {
            // 将 ResultRelInfo 切换为 Delta 内表
            Relation delta_rel = heap_open(delta_oid, RowExclusiveLock);
            // 走普通内部表的 heap_insert 路径（即跳过此 else-if 分支，
            // 让后续的普通表插入逻辑处理）
            // ... 切换 result_rel_info 的关系描述符 ...
        }
        // 如果找不到 Delta 表映射，仍走原 FDW 路径（或报错）
    }
    // === 原逻辑：非 Iceberg 外表，走 FDW ExecForeignInsert ===
    slot = result_rel_info->ri_FdwRoutine->ExecForeignInsert(...);
}
```

**具体实现要点**：

1. **判断 Iceberg 外表**：新增 `IsIcebergForeignTable()` 函数，通过 `ServerTypeOption == T_ICEBERG_SERVER` 或检查外表 server 名称/类型来判定。

2. **查找 Delta 表 OID**：新增 `LookupDeltaTableByForeignOid()` 函数，查询 `pg_delta_table` catalog，以 `foreign_relid` 为 key 获取 `delta_relid`。

3. **切换插入目标**：将 `result_rel_info->ri_RelationDesc` 替换为 Delta 表的 Relation，清除 `ri_FdwRoutine`（使其变为 NULL），让代码走到后续的普通表 heap_insert 分支。

4. **事务与锁**：Delta 表的打开/关闭需在当前事务上下文中完成，锁模式为 `RowExclusiveLock`。

**更简洁的替代方案**：不替换 `ResultRelInfo` 的字段，而是在 FDW 分支内部直接调用 heap_insert 到 Delta 表：

```cpp
} else if (result_rel_info->ri_FdwRoutine) {
    if (IsIcebergForeignTable(result_relation_desc)) {
        Oid delta_oid = LookupDeltaTableByForeignOid(RelationGetRelid(result_relation_desc));
        if (OidIsValid(delta_oid)) {
            Relation delta_rel = heap_open(delta_oid, RowExclusiveLock);
            // 直接走 heap_insert 到 Delta 表
            heap_insert(delta_rel, tuple, GetCurrentCommandId(true), 0, NULL);
            heap_close(delta_rel, RowExclusiveLock);
            return slot;  // 返回原始 slot
        }
    }
    // 原逻辑
    slot = result_rel_info->ri_FdwRoutine->ExecForeignInsert(...);
}
```

**推荐采用替代方案**，原因：
- 侵入性更小，不修改 `ResultRelInfo` 的内部状态
- 不影响后续的 trigger、constraint 检查逻辑（这些仍基于原外表的 `ResultRelInfo`）
- Delta 表的索引、约束由建表时独立管理，不与外表混淆

### 3.4 Server Type 扩展

在 `foreign.h` 的 `ServerTypeOption` 枚举中新增：

```cpp
typedef enum ServerTypeOption {
    T_INVALID = 0,
    T_OBS_SERVER,
    T_HDFS_SERVER,
    T_MOT_SERVER,
    T_DUMMY_SERVER,
    T_TXT_CSV_OBS_SERVER,
    T_PGFDW_SERVER,
    T_ICEBERG_SERVER,    // 新增
} ServerTypeOption;
```

配套新增判断宏：

```cpp
#define isIcebergFDWFromTblOid(relId) \
    (OidIsValid(relId) && IsSpecifiedFDWFromRelid(relId, ICEBERG_FDW))

#define isIcebergFDWFromSrvName(srvName) \
    (IsSpecifiedFDW(srvName, ICEBERG_FDW))
```

以及对应的 `ICEBERG_FDW` 常量字符串定义。

## 4. 需修改的文件清单

| 文件 | 修改内容 |
|------|----------|
| `src/include/catalog/pg_delta_table.h` | **新增** — Delta 映射 catalog 头文件 |
| `src/include/catalog/indexing.h` | 注册 `pg_delta_table` 的索引声明 |
| `src/include/foreign/foreign.h` | 新增 `T_ICEBERG_SERVER`、`ICEBERG_FDW` 宏/常量、`isIcebergFDW*` 宏 |
| `src/include/catalog/pg_class.h` | 可能需确认 OID 分配空间 |
| `src/gausskernel/runtime/executor/nodeModifyTable.cpp` | **核心** — `ExecInsertT()` 中新增 Iceberg 截流逻辑 |
| `src/gausskernel/catalog/` | 新增 `pg_delta_table` 的 CRUD 函数 |
| `src/bin/initdb/initdb.cpp` | 确保 initdb 创建 `pg_delta_table` |
| `src/gausskernel/optimizer/commands/foreigncmds.cpp` | 可能需在创建外表时标记 Iceberg 类型 |

## 5. 不涉及的部分（后续团队实现）

- Iceberg FDW 的完整实现（scan/insert 元数据操作）
- Parquet 文件读写
- Iceberg catalog 操作（Hive/Glue/REST catalog 交互）
- Delta → Iceberg 数据同步逻辑
- Delta 表的自动清理/过期策略
- 并发写入冲突处理

## 6. 最小可用实现的步骤顺序

1. **新增 `pg_delta_table` catalog**（头文件 + initdb + CRUD 函数）
2. **新增 `T_ICEBERG_SERVER` 类型标识**（`foreign.h` + 相关宏）
3. **实现 `register_delta_mapping()` 函数**（SQL callable，写入映射）
4. **修改 `ExecInsertT()` 截流逻辑**（最核心的一步）
5. **集成测试**：手动创建 Iceberg 外表 + Delta 表 + 映射 → INSERT 验证数据落入 Delta 表

## 7. 风险与约束

- **OID 冲突**：`pg_delta_table` 需分配未使用的 OID，需查阅 openGauss 的 OID 分配规则
- **schema 同步**：如果外表 ALTER 增删列，Delta 表 schema 需同步更新，否则 INSERT 会失败。最小实现中暂不处理，需人工保证 schema 一致
- **事务一致性**：INSERT 截流在同一个事务中完成 Delta 表写入，若事务回滚则 Delta 表数据也回滚，一致性无问题
- **权限**：Delta 表是普通内部表，需确保用户对 Delta 表有 INSERT 权限。最小实现中由建表者保证
- **性能**：每次 INSERT 需额外查询 `pg_delta_table` catalog。可通过 Relation 缓存（`rd_delta_info`）优化，最小实现中暂不做

## 8. TODO（后续优化）

- **syscache 注册**：为 `pg_delta_table` 注册 `DELTATABLEREL` 和 `DELTATABLEDELTAID` 两个 syscache 缓存 ID，将查询从 `systable_beginscan` 手动扫描升级为 `SearchSysCache1` 直接缓存查找，提升高频 INSERT 场景下的查询性能
