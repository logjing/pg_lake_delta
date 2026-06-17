# PG Lake Delta 详细设计文档

## 1. 概述

PG Lake Delta 在 openGauss 中引入"Delta 表"机制，使 Iceberg 外表的 INSERT 操作不直接写入外表，而是先写入对应的本地 Delta 内表，后续批量同步到 Iceberg 外表。

### 1.1 整体架构

```
INSERT INTO t_iceberg (id, name, score) VALUES (1, 'alice', 95.5);
                          │
                          ▼
┌─────────────────────────────────────────────────────┐
│  analyze.cpp: CheckSupportedFDWType()               │
│  ICEBERG_FDW 在白名单中 → 允许 INSERT 通过解析阶段   │
└─────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────┐
│  execMain.cpp: FDW ExecForeignInsert NULL 检查      │
│  检测到 iceberg_fdw + Delta 映射 → 跳过 FDW 校验     │
└─────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────┐
│  nodeModifyTable.cpp: ExecInsertT()                 │
│  isIcebergFDWFromTblOid() → true                    │
│  LookupDeltaTableByForeignOid() → deltaOid           │
│  tableam_tuple_insert(deltaRel, tuple) → 写入 Delta 内表(USTORE) │
└─────────────────────────────────────────────────────┘
                          │
                          ▼
              ┌───────────────────┐
              │  Delta 内表 (t_delta)│
              │  id=1, alice, 95.5   │
              └───────────────────┘
```

### 1.2 数据同步流程（后续团队实现）

```
Delta 内表 ──→ Parquet 文件 ──→ S3 ──→ Iceberg 元数据更新
              (批量同步)         (foreign_path 指向的位置)
```

## 2. 数据模型

### 2.1 pg_delta_table catalog

新增系统 catalog 表，存储外表 ↔ Delta 表的映射关系。

| 字段 | 类型 | 说明 |
|------|------|------|
| `delta_relid` | Oid | Delta 内表 OID |
| `foreign_relid` | Oid | Iceberg 外表 OID |
| `foreign_path` | text[] | 外表数据路径（如 s3://bucket/table/） |

**Catalog OID**: 9994

**索引**：
- `pg_delta_table_foreign_relid_index` (OID 10007)：foreign_relid 唯一索引
- `pg_delta_table_delta_relid_index` (OID 10008)：delta_relid 唯一索引

### 2.2 相关 OID 分配

| 对象 | OID |
|------|-----|
| pg_delta_table catalog | 9994 |
| pg_delta_table rowtype | 11663 |
| foreign_relid 唯一索引 | 10007 |
| delta_relid 唯一索引 | 10008 |
| register_delta_mapping 函数 | 8554 |

## 3. API 设计

### 3.1 C 层函数

定义在 `pg_delta_table.h` / `pg_delta_table.cpp`：

```cpp
// 创建映射记录
Oid CreateDeltaTableMapping(Oid deltaRelid, Oid foreignRelid, const char *foreignPath);

// 根据外表 OID 查找 Delta 内表 OID
Oid LookupDeltaTableByForeignOid(Oid foreignRelid);

// 根据 Delta 内表 OID 查找外表 OID
Oid LookupDeltaTableByDeltaOid(Oid deltaRelid);

// 根据外表 OID 删除映射
void RemoveDeltaTableMappingByForeignOid(Oid foreignRelid);

// 根据 Delta 内表 OID 删除映射
void RemoveDeltaTableMappingByDeltaOid(Oid deltaRelid);
```

查询方式：通过 `systable_beginscan` 手动扫描 catalog（暂不注册 syscache）。

### 3.2 SQL 可调用函数

```sql
-- 注册映射，返回 Delta 内表 OID
SELECT register_delta_mapping(delta_oid, foreign_oid, 's3://path/');

-- 第三个参数可以为 NULL
SELECT register_delta_mapping(delta_oid, foreign_oid, NULL);
```

函数属性：OID 8554，3 参数 (OID, OID, TEXT)，NOT strict，volatile。

## 4. 类型标识系统

### 4.1 新增常量与宏

```cpp
// postgres.h
#define ICEBERG_FDW "iceberg_fdw"

// foreign.h - ServerTypeOption 枚举
T_ICEBERG_SERVER  /* 值 = 7 */

// foreign.h - 判断宏
#define isIcebergFDWFromTblOid(relId) \
    (OidIsValid(relId) && IsSpecifiedFDWFromRelid(relId, ICEBERG_FDW))

#define isIcebergFDWFromSrvName(srvName) \
    (IsSpecifiedFDW(srvName, ICEBERG_FDW))
```

### 4.2 FDW 名称判定机制

`IsSpecifiedFDWFromRelid(relId, ICEBERG_FDW)` 内部遍历链：
```
pg_class.oid → pg_foreign_table.ftserver → pg_foreign_server.srvfdw → pg_foreign_data_wrapper.fdwname
```
比对 `fdwname == "iceberg_fdw"`。

### 4.3 getServerType() 扩展

```cpp
ServerTypeOption getServerType(Oid foreignTableId) {
    // ...
    } else if (IsSpecifiedFDWFromRelid(foreignTableId, ICEBERG_FDW)) {
        srvType = T_ICEBERG_SERVER;
    }
    // ...
}
```

## 5. INSERT 截流机制

### 5.1 入口条件

INSERT INTO iceberg_table 要成功地被截流，需要同时满足：

1. 目标表是外表（`relkind = 'f'`）
2. 外表的 FDW 名称是 `"iceberg_fdw"`（`isIcebergFDWFromTblOid → true`）
3. `pg_delta_table` 中存在该外表的映射记录（`LookupDeltaTableByForeignOid → ValidOid`）

### 5.2 执行流程

```
ExecInsertT() [nodeModifyTable.cpp]
  │
  ├─ 1129: tuple = tableam_tslot_get_tuple_from_slot(...)
  ├─ 1158: BEFORE ROW INSERT Triggers
  ├─ 1179: INSTEAD OF ROW INSERT Triggers
  │
  ├─ 1192: } else if (ri_FdwRoutine) {
  │   ├─ 1196: Generated columns 计算
  │   ├─ 1202: MOT 特殊处理
  │   │
  │   ├─ NEW: isIcebergFDWFromTblOid(relid)?
  │   │   ├─ YES → LookupDeltaTableByForeignOid(relid)
  │   │   │   ├─ ValidOid → heap_open(deltaOid)
  │   │   │   │             tableam_tslot_get_tuple_from_slot + tableam_tuple_insert
  │   │   │   │             icebergRouted = true
  │   │   │   └─ InvalidOid → 回退到 FDW 路径
  │   │   └─ NO → 走原有 FDW ExecForeignInsert 路径
  │   │
  │   └─ !icebergRouted → ExecForeignInsert(...)
  │
  ├─ 1616: canSetTag (es_processed++)
  ├─ 1642: AFTER ROW INSERT Triggers
  ├─ 1665: WITH CHECK OPTIONS
  └─ 1669: RETURNING processing
```

### 5.3 FDW 校验绕过

在 `execMain.cpp` 的 `InitPlan()` 阶段，Iceberg 表需要绕过两个 FDW 层面的校验：

```cpp
case CMD_INSERT:
    // 如果是有 Delta 映射的 Iceberg 外表，跳过 FDW ExecForeignInsert NULL 检查
    if (isIcebergFDWFromTblOid(relid) && OidIsValid(LookupDeltaTableByForeignOid(relid))) {
        /* 跳过：INSERT 将在 ExecInsertT 中被重定向 */
    } else if (fdwroutine->ExecForeignInsert == NULL) {
        ERROR("cannot insert into foreign table");
    }

    // IsForeignRelUpdatable 检查同样需要绕过
    if (fdwroutine->IsForeignRelUpdatable != NULL &&
        !(isIcebergFDWFromTblOid(relid) && OidIsValid(LookupDeltaTableByForeignOid(relid))) &&
        ...) {
        ERROR("foreign table does not allow inserts");
    }
```

### 5.4 解析阶段白名单

`CheckSupportedFDWType()` 在 `foreign.cpp` 中维护 FDW 白名单，决定哪些 FDW 类型的 INSERT 在解析阶段不被拒绝：

```cpp
static const char* supportFDWType[] = {
    MOT_FDW, MYSQL_FDW, ORACLE_FDW, POSTGRES_FDW, ICEBERG_FDW
};
```

## 6. 文件清单

| 文件 | 作用 | 新增/修改 |
|------|------|----------|
| `src/include/catalog/pg_delta_table.h` | catalog 头文件 | 新增 |
| `src/common/backend/catalog/pg_delta_table.cpp` | CRUD 函数 + register_delta_mapping() | 新增 |
| `src/common/backend/catalog/builtin_funcs.ini` | register_delta_mapping 内置函数注册 | 修改 |
| `src/common/backend/utils/pg_builtin_proc.h` | 内置函数 extern 声明 | 自动生成 |
| `src/common/backend/utils/fmgroids.h` | 函数 OID 常量 | 自动生成 |
| `src/common/backend/catalog/Makefile` | 构建：添加 pg_delta_table.h/o | 修改 |
| `src/common/backend/catalog/CMakeLists.txt` | 构建：添加 pg_delta_table.h | 修改 |
| `src/include/catalog/indexing.h` | 索引声明 | 修改 |
| `src/include/postgres.h` | ICEBERG_FDW 常量 | 修改 |
| `src/include/foreign/foreign.h` | T_ICEBERG_SERVER + 判断宏 | 修改 |
| `src/gausskernel/cbb/extension/foreign/foreign.cpp` | getServerType + CheckSupportedFDWType | 修改 |
| `src/gausskernel/runtime/executor/nodeModifyTable.cpp` | INSERT 截流核心 | 修改 |
| `src/gausskernel/runtime/executor/execMain.cpp` | FDW 校验绕过 | 修改 |

## 7. 已知限制与后续工作

| 限制 | 说明 | 后续方案 |
|------|------|---------|
| Delta 表约束不检查 | heap_insert 跳过 ExecConstraints、索引等 | 可在 ExecInsertT 中补全约束检查 |
| Schema 必须一致 | 外表和 Delta 表列定义必须相同 | SQL 辅助函数 create_delta_table() |
| 仅支持 INSERT | UPDATE/DELETE 截流未实现 | 后续实现 |
| 每次 INSERT 查询 catalog | LookupDeltaTableByForeignOid 每次都扫描 pg_delta_table | 注册 syscache 缓存 |
| 无事务批量同步 | Delta → Iceberg 同步逻辑未实现 | 后续团队实现 |
| 仅支持 Iceberg 外表 | 其他 FDW 类型不支持 | 可扩展映射机制 |

## 8. 提交历史

| Commit | 内容 |
|--------|------|
| `40f11f8` | pg_delta_table catalog（表结构、索引、CRUD 函数） |
| `02f8eb4` | catalog 测试报告 |
| `e362a19` | ICEBERG_FDW + T_ICEBERG_SERVER 类型标识 |
| `df5865e` | FDW 标识测试报告 |
| `1e18d56` | register_delta_mapping() SQL 函数 |
| `ae95497` | INSERT 截流实现 + 测试报告 |
