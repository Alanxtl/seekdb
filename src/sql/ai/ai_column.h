/* Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/thread/thread_pool.h"
#include <atomic>
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase {
namespace sql {
class ObExecContext;
class ObSQLSessionInfo;
class ObSchemaChecker;
class ObTableModifyOp;
struct ObDMLBaseCtDef;
struct ObResolverParams;
class ObRawExpr;
class ObTableScanSpec;

struct AiColumnDefinition
{
  common::ObString expression_;
  common::ObSEArray<common::ObString, 4> dependencies_;
  common::ObString version_;
  common::ObString system_prompt_;
  common::ObString model_;
  common::ObString prompt_expression_;
  common::ObString config_expression_;
  int64_t dimension_ = 0;
  int64_t max_retries_ = 2;
  bool embedding_ = false;
  bool prompt_is_object_ = false;
  int read(const common::ObString &json, common::ObIAllocator &allocator);
  static int resolve(const ParseNode &node, share::schema::ObColumnSchemaV2 &column,
                     common::ObIAllocator &allocator);
  static int add_hidden_columns(share::schema::ObTableSchema &table, bool altering = false,
                                const share::schema::ObTableSchema *original = nullptr);
  static int validate(share::schema::ObColumnSchemaV2 &column,
      share::schema::ObTableSchema &table,
      common::ObIArray<share::schema::ObColumnSchemaV2 *> &columns,
      ObResolverParams &params);
};

// SQL owns transactional enqueueing and publication. HTTP work runs outside
// business transactions; durable tasks are the source of truth after restart.
class AiColumnService final : public lib::ThreadPool
{
public:
  static AiColumnService &instance();
  static bool is_internal();
  static bool allows_hidden_columns();
  static bool has_columns(const share::schema::ObTableSchema &table);
  int start_service();
  void stop_service();
  static int process_dml(const ObDMLBaseCtDef &definition, ObTableModifyOp &op, bool *touched = nullptr);
  static int check_strict(ObExecContext &context, uint64_t table_id,
                         const common::ObIArray<uint64_t> &columns);
  static int retry(ObExecContext &context, uint64_t table_id,
                   const common::ObString &column, const common::ObString &condition);
  static int backfill(uint64_t table_id);
private:
  std::atomic<bool> stopping_{false};
  std::atomic<bool> recovered_{false};
  AiColumnService() = default;
  void run1() override;
  int run_one();
  int recover_definitions();
};
} // namespace sql
} // namespace oceanbase
