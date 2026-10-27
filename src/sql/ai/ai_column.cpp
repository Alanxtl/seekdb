/* Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/ai/ai_column.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/ob_server_struct.h"
#include "lib/utility/ob_smart_call.h"
#include "lib/worker.h"
#include "sql/resolver/ddl/ob_ddl_resolver.h"
#include "sql/ob_sql_utils.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "share/object/ob_obj_cast.h"
#include "share/schema/ob_schema_service.h"
#include "query/vector/ob_vector_index_util.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include <atomic>
#include <cctype>

namespace oceanbase {
namespace sql {
using namespace common;
using namespace share::schema;
namespace {
thread_local bool ai_column_internal = false;
thread_local int ai_column_force = 0;
thread_local uint64_t ai_column_retry_id = OB_INVALID_ID;
constexpr const char *AI_TASK_TABLE = "oceanbase.__all_ai_column_task";
int ai_read_with_session(ObExecContext &, const ObSqlString &, bool &);
struct InternalAiColumnScope {
  bool saved_;
  InternalAiColumnScope() : saved_(ai_column_internal) { ai_column_internal = true; }
  ~InternalAiColumnScope() { ai_column_internal = saved_; }
};
struct AiSqlScope {
  ObExecContext &context_;
  bool saved_;
  explicit AiSqlScope(ObExecContext &context) : context_(context), saved_(context.is_ai_column_sql())
  { context_.set_ai_column_sql(true); }
  ~AiSqlScope() { context_.set_ai_column_sql(saved_); }
};
int ai_new_version(ObSqlString &version)
{
  char uuid[UuidCommon::LENGTH_UUID];
  int ret = ObExprUuid::gen_server_uuid(uuid, sizeof(uuid));
  OZ(version.append(uuid, sizeof(uuid)));
  return ret;
}
int ai_validate_result(uint64_t table_id, uint64_t column_id, const ObString &text, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObColumnSchemaV2 *column = nullptr;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schema(table_id, table));
  if (OB_SUCC(ret) && (table == nullptr || (column = table->get_column_schema(column_id)) == nullptr)) {
    ret = OB_SCHEMA_EAGAIN;
  } else if (OB_SUCC(ret) && column->get_data_type() == ObJsonType) {
    ObIJsonBase *json = nullptr;
    OZ(ObJsonBaseFactory::get_json_base(&allocator, text, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json));
  } else if (OB_SUCC(ret) && !column->is_collection()) {
    ObAccuracy accuracy = column->get_accuracy();
    ObCastCtx cast_context(&allocator, nullptr, 0, CM_STRICT_MODE | CM_COLUMN_CONVERT,
        column->get_collation_type(), nullptr, &accuracy);
    ObObj source;
    ObObj converted;
    source.set_varchar(text);
    source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    OZ(ObObjCaster::to_type(column->get_data_type(), cast_context, source, converted));
    OZ(ob_obj_accuracy_check_only(column->get_accuracy(), column->get_collation_type(), converted));
    if (OB_SUCC(ret) && cast_context.warning_ != OB_SUCCESS) { ret = OB_INVALID_DATA; }
  }
  return ret;
}
int ai_append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    OZ(sql.append(name.ptr() + i, 1));
    if (name.ptr()[i] == '`') { OZ(sql.append("`")); }
  }
  OZ(sql.append("`"));
  return ret;
}
int ai_append_hex(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("X'");
  const char digits[] = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    unsigned char byte = static_cast<unsigned char>(value.ptr()[i]);
    char encoded[] = {digits[byte >> 4], digits[byte & 15]};
    OZ(sql.append(encoded, 2));
  }
  OZ(sql.append("'"));
  return ret;
}
int ai_append_table(ObSqlString &sql, const ObTableSchema &table, ObSchemaGetterGuard &guard)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(guard.get_database_schema(table.get_database_id(), database))) {
  } else if (OB_ISNULL(database)) {
    ret = OB_ERR_BAD_DATABASE;
  } else {
    OZ(ai_append_identifier(sql, database->get_database_name_str()));
    OZ(sql.append("."));
    OZ(ai_append_identifier(sql, table.get_table_name_str()));
  }
  return ret;
}
int ai_read_string(ObJsonObject *json, const char *key, ObString &out)
{
  int ret = OB_SUCCESS;
  ObJsonNode *value = json->get_value(key);
  if (OB_ISNULL(value) || value->json_type() != ObJsonNodeType::J_STRING) {
    ret = OB_INVALID_ARGUMENT;
  } else { out = static_cast<ObJsonString *>(value)->get_str(); }
  return ret;
}
int ai_collect_dependencies(const ParseNode *node, ObJsonArray &dependencies,
                           ObIAllocator &allocator, int64_t depth = 0)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
  } else if (depth > 128) {
    ret = OB_SIZE_OVERFLOW;
  } else if (node->type_ == T_COLUMN_REF) {
    if (node->num_child_ != 3 || OB_ISNULL(node->children_[2])
        || OB_NOT_NULL(node->children_[0]) || OB_NOT_NULL(node->children_[1])) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObString name(node->children_[2]->str_len_, node->children_[2]->str_value_);
      bool exists = false;
      for (int64_t i = 0; i < dependencies.element_count(); ++i) {
        if (static_cast<ObJsonString *>(dependencies.get_value(i))->get_str().case_compare(name) == 0) {
          exists = true;
        }
      }
      if (!exists) {
        ObJsonString *text = nullptr;
        OZ(ObAIFuncJsonUtils::get_json_string(allocator, name, text));
        OZ(dependencies.append(text));
      }
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      OZ(SMART_CALL(ai_collect_dependencies(node->children_[i], dependencies, allocator, depth + 1)));
    }
  }
  return ret;
}
int ai_row_literal(const ObDMLBaseCtDef &ctdef, const ObExprPtrIArray &row,
                   uint64_t column_id, ObTableModifyOp &op, ObIAllocator &allocator,
                   ObString &out)
{
  int ret = OB_SUCCESS;
  int64_t index = -1;
  for (int64_t i = 0; i < ctdef.column_ids_.count(); ++i) {
    if (ctdef.column_ids_.at(i) == column_id) { index = i; break; }
  }
  if (index < 0 || index >= row.count() || OB_ISNULL(row.at(index))) {
    ret = OB_ERR_UNEXPECTED;
    FLOG_INFO("AI dependency absent from DML row", K(column_id), K(index), K(ctdef.column_ids_), K(row.count()));
  } else {
    ObExpr *expr = row.at(index);
    ObDatum *datum = nullptr;
    ObObj object;
    if (OB_FAIL(expr->eval(op.get_eval_ctx(), datum))) {
    } else if (OB_FAIL(datum->to_obj(object, expr->obj_meta_))) {
    } else {
      if (!datum->is_null() && ob_is_text_tc(expr->datum_meta_.type_)) {
        ObString text;
        if (OB_FAIL(ObTextStringHelper::read_real_string_data(op.get_exec_ctx(), allocator,
            *datum, expr->datum_meta_, expr->obj_meta_.has_lob_header(), text))) {
        } else {
          object.set_varchar(text);
          object.set_collation_type(expr->datum_meta_.cs_type_);
        }
      }
      char *buffer = nullptr;
      int64_t length = 0;
      int64_t pos = 0;
      ObObjPrintParams print_params(op.get_exec_ctx().get_my_session()->get_timezone_info());
      if (OB_SUCC(ret) && OB_FAIL(object.print_sql_literal(buffer, length, pos, allocator, print_params))) {
      } else if (OB_SUCC(ret)) {
        out.assign_ptr(buffer, pos);
      }
    }
  }
  return ret;
}
int ai_row_filter(const ObTableSchema &table, const ObDMLBaseCtDef &ctdef,
                  const ObExprPtrIArray &row, ObTableModifyOp &op, ObIAllocator &allocator,
                  ObSqlString &filter)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_rowkey_column_num(); ++i) {
    uint64_t cid = OB_INVALID_ID;
    const ObColumnSchemaV2 *column = nullptr;
    ObString literal;
    OZ(table.get_rowkey_info().get_column_id(i, cid));
    if (OB_SUCC(ret) && OB_ISNULL(column = table.get_column_schema(cid))) {
      ret = OB_ERR_UNEXPECTED;
    }
    OZ(ai_row_literal(ctdef, row, cid, op, allocator, literal));
    if (i > 0) { OZ(filter.append(" AND ")); }
    if (OB_SUCC(ret)) { OZ(ai_append_identifier(filter, column->get_column_name_str())); }
    OZ(filter.append(" = "));
    OZ(filter.append(literal));
  }
  return ret;
}
int ai_task_key(ObSqlString &sql, uint64_t table_id, const ObString &filter)
{
  int ret = sql.append_fmt("table_id=%lu AND row_key=UNHEX(SHA2(", table_id);
  OZ(ai_append_hex(sql, filter));
  OZ(sql.append(",256))"));
  return ret;
}
int ai_execute_nested(ObTableModifyOp &op, const ObSqlString &sql)
{
  AiSqlScope sql_scope(op.get_exec_ctx());
  int ret = op.open_inner_conn();
  if (OB_SUCC(ret) && OB_SUCC(ret = op.begin_nested_session(true))) {
    int write_ret = op.execute_write(sql.ptr());
    if (write_ret != OB_SUCCESS) { FLOG_INFO("AI task write failed", K(write_ret)); }
    int end_ret = op.end_nested_session();
    ret = write_ret == OB_SUCCESS ? end_ret : write_ret;
  }
  return ret;
}
} // namespace

int AiColumnDefinition::read(const ObString &json, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  ObJsonObject *object = nullptr;
  ObString source = json;
  dependencies_.reset();
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(allocator, source, object))) {
  } else if (OB_FAIL(ai_read_string(object, "expression", expression_))) {
  } else {
    version_ = json;
    if (object->get_value("prompt_is_object")) { prompt_is_object_ = object->get_value("prompt_is_object")->get_boolean(); }
    if (object->get_value("version")) { OZ(ai_read_string(object, "version", version_)); }
    if (object->get_value("system_prompt")) { OZ(ai_read_string(object, "system_prompt", system_prompt_)); }
    if (object->get_value("model")) { OZ(ai_read_string(object, "model", model_)); }
    if (object->get_value("prompt_expression")) { OZ(ai_read_string(object, "prompt_expression", prompt_expression_)); }
    if (object->get_value("config_expression")) { OZ(ai_read_string(object, "config_expression", config_expression_)); }
    ObJsonNode *dim = object->get_value("dimension");
    if (OB_NOT_NULL(dim)) { dimension_ = dim->get_int(); }
    ObJsonNode *dependencies = object->get_value("dependencies");
    ObJsonNode *retries = object->get_value("max_retries");
    ObJsonNode *embedding = object->get_value("embedding");
    if (OB_ISNULL(dependencies) || dependencies->json_type() != ObJsonNodeType::J_ARRAY
        || OB_ISNULL(retries) || retries->json_type() != ObJsonNodeType::J_INT
        || OB_ISNULL(embedding) || embedding->json_type() != ObJsonNodeType::J_BOOLEAN) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      max_retries_ = retries->get_int();
      embedding_ = embedding->get_boolean();
      auto *array = static_cast<ObJsonArray *>(dependencies);
      for (int64_t i = 0; OB_SUCC(ret) && i < array->element_count(); ++i) {
        ObJsonNode *name = array->get_value(i);
        if (OB_ISNULL(name) || name->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
        } else { OZ(dependencies_.push_back(static_cast<ObJsonString *>(name)->get_str())); }
      }
    }
  }
  return ret;
}

int AiColumnDefinition::resolve(const ParseNode &node, ObColumnSchemaV2 &column,
                                ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (node.num_child_ != 2 || OB_ISNULL(node.children_[0])) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *definition = nullptr;
    ObJsonArray *dependencies = nullptr;
    ObString expression(node.children_[0]->str_len_, node.children_[0]->str_value_);
    ObJsonString *text = nullptr;
    ObJsonInt *retries = nullptr;
    ObJsonBoolean *embedding = nullptr;
    int64_t max_retries = 2;
    bool seen_mode = false;
    bool seen_retries = false;
    ParseNode *options = node.children_[1];
    for (int64_t i = 0; OB_SUCC(ret) && OB_NOT_NULL(options) && i < options->num_child_; ++i) {
      ParseNode *option = options->children_[i];

      if (OB_ISNULL(option) || option->type_ != T_OP_EQ || option->num_child_ != 2
          || OB_ISNULL(option->children_[0]) || option->children_[0]->type_ != T_COLUMN_REF
          || OB_ISNULL(option->children_[0]->children_[2]) || OB_ISNULL(option->children_[1])) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        ParseNode *key_node = option->children_[0]->children_[2];
        ParseNode *value = option->children_[1];
        ObString key(key_node->str_len_, key_node->str_value_);
        if (key.case_compare("sync_mode") == 0 && !seen_mode) {
          seen_mode = true;
          ObString mode(value->str_len_, value->str_value_);
          if (value->type_ != T_VARCHAR || mode.case_compare("async") != 0) { ret = OB_INVALID_ARGUMENT; }
        } else if (key.case_compare("max_retries") == 0 && !seen_retries) {
          seen_retries = true;
          if (value->type_ != T_INT || value->value_ < 0 || value->value_ > 100) { ret = OB_INVALID_ARGUMENT; }
          else { max_retries = value->value_; }
        } else { ret = OB_INVALID_ARGUMENT; }
      }
    }
    OZ(ObAIFuncJsonUtils::get_json_object(allocator, definition));
    OZ(ObAIFuncJsonUtils::get_json_string(allocator, expression, text));
    OZ(ObAIFuncJsonUtils::get_json_array(allocator, dependencies));
    OZ(ai_collect_dependencies(node.children_[0], *dependencies, allocator));
    OZ(ObAIFuncJsonUtils::get_json_int(allocator, max_retries, retries));
    // The resolved expression type is checked below; this value is updated there.
    ObString prefix(expression.length() < 8 ? expression.length() : 8, expression.ptr());
    OZ(ObAIFuncJsonUtils::get_json_boolean(allocator, prefix.case_compare("AI_EMBED") == 0, embedding));
    OZ(definition->add("expression", text));
    OZ(definition->add("dependencies", dependencies));
    OZ(definition->add("max_retries", retries));
    OZ(definition->add("embedding", embedding));
    ObSqlString version;
    ObJsonString *version_json = nullptr;
    OZ(ai_new_version(version));
    ObString version_text = version.string();
    OZ(ObAIFuncJsonUtils::get_json_string(allocator, version_text, version_json));
    OZ(definition->add("version", version_json));
    ObString serialized;
    OZ(ObAIFuncJsonUtils::print_json_to_str(allocator, definition, serialized));
    OZ(column.set_ai_column_definition(serialized));
  }
  if (ret == OB_INVALID_ARGUMENT) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "invalid AI COLUMN definition or options");
  }
  return ret;
}

int AiColumnDefinition::validate(ObColumnSchemaV2 &column, ObTableSchema &table,
    ObIArray<ObColumnSchemaV2 *> &columns, ObResolverParams &params)
{
  int ret = OB_SUCCESS;
  AiColumnDefinition definition;
  ObRawExpr *expression = nullptr;
  ObColumnSchemaV2 temporary;
  if (OB_FAIL(definition.read(column.get_ai_column_definition(), *params.allocator_))) {
  } else if (!column.is_nullable() || column.is_rowkey_column() || column.is_autoincrement()
      || column.is_generated_column() || !column.get_cur_default_value().is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(temporary.assign(column))) {
  } else if (OB_FAIL(ObResolverUtils::resolve_generated_column_expr(params, definition.expression_,
      table, columns, temporary, expression, ObResolverUtils::DISABLE_CHECK))) {
  } else if (OB_ISNULL(expression) || (expression->get_expr_type() != T_FUN_SYS_AI_COMPLETE
      && expression->get_expr_type() != T_FUN_SYS_AI_EMBED)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    if ((expression->get_expr_type() == T_FUN_SYS_AI_EMBED) != column.is_collection()) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < expression->get_param_count(); ++i) {
      OZ(ObRawExprUtils::check_is_valid_generated_col(expression->get_param_expr(i), *params.allocator_));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < definition.dependencies_.count(); ++i) {
      const ObColumnSchemaV2 *dependency = table.get_column_schema(definition.dependencies_.at(i));
      for (int64_t j = 0; OB_ISNULL(dependency) && j < columns.count(); ++j) {
        if (columns.at(j)->get_column_name_str().case_compare(definition.dependencies_.at(i)) == 0) {
          dependency = columns.at(j);
        }
      }
      if (OB_ISNULL(dependency) || dependency->is_ai_column() || dependency->is_generated_column()
          || dependency->get_column_id() == column.get_column_id()) {
        ret = OB_NOT_SUPPORTED;
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObJsonObject *json = nullptr;
    ObString source = column.get_ai_column_definition();
    OZ(ObAIFuncJsonUtils::get_json_object_form_str(*params.allocator_, source, json));
    const bool embedding = expression->get_expr_type() == T_FUN_SYS_AI_EMBED;
    ObString system_prompt = embedding ? ObString("") :
        column.get_data_type() == ObJsonType ? ObString("Return exactly one valid JSON value.") :
        ob_is_integer_type(column.get_data_type()) ? ObString("Return exactly one base-10 integer.") :
        ObString("Return only the requested text.");
    ObString model;
    ParamStore constant_params;
    if (expression->get_param_count() < 2 || !expression->get_param_expr(0)->is_const_expr()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObObj model_value;
      OZ(ObSQLUtils::calc_simple_expr_without_row(params.session_info_,
          expression->get_param_expr(0), model_value, &constant_params, *params.allocator_));
      if (OB_SUCC(ret) && (!model_value.is_string_type() || model_value.get_string().empty())) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_SUCC(ret)) { model = model_value.get_string(); }
      ObAIFuncExprInfo *info = nullptr;
      OZ(ObAIFuncUtils::get_ai_func_info(*params.allocator_, model, info));
      if (OB_SUCC(ret) && (info == nullptr ||
          info->type_ != (embedding ? share::EndpointType::DENSE_EMBEDDING : share::EndpointType::COMPLETION))) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    int64_t dimension = 0;
    if (OB_SUCC(ret) && embedding) {
      OZ(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), dimension));
      if (OB_SUCC(ret) && expression->get_param_count() == 3) {
        ObRawExpr *dim = expression->get_param_expr(2);
        ObObj dimension_value;
        if (!dim->is_const_expr()) { ret = OB_INVALID_ARGUMENT; }
        else {
          OZ(ObSQLUtils::calc_simple_expr_without_row(params.session_info_,
              dim, dimension_value, &constant_params, *params.allocator_));
          if (OB_SUCC(ret) && !dimension_value.is_integer_type()) { ret = OB_INVALID_ARGUMENT; }
          else if (OB_SUCC(ret) && dimension_value.get_int() != dimension) {
            ret = OB_ERR_INVALID_VECTOR_DIM;
            LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<unsigned>(dimension),
                static_cast<unsigned>(dimension_value.get_int()));
          }
        }
      }
    }
    ObColumnSchemaV2 printed;
    ObString prompt_expression;
    ObString config_expression("NULL");
    OZ(ObDDLResolver::print_expr_to_default_value(*expression->get_param_expr(1), printed,
        params.schema_checker_, params.session_info_->get_timezone_info()));
    if (OB_SUCC(ret)) { prompt_expression = printed.get_cur_default_value().get_string(); }
    ObColumnSchemaV2 printed_config;
    if (OB_SUCC(ret) && !embedding && expression->get_param_count() == 3) {
      OZ(ObDDLResolver::print_expr_to_default_value(*expression->get_param_expr(2), printed_config,
          params.schema_checker_, params.session_info_->get_timezone_info()));
      if (OB_SUCC(ret)) { config_expression = printed_config.get_cur_default_value().get_string(); }
    }
    const char *keys[] = {"system_prompt", "model", "prompt_expression", "config_expression"};
    ObString values[] = {system_prompt, model, prompt_expression, config_expression};
    for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
      ObJsonString *value = nullptr;
      OZ(ObAIFuncJsonUtils::get_json_string(*params.allocator_, values[i], value));
      OZ(json->add(keys[i], value));
    }
    ObJsonBoolean *prompt_is_object = nullptr;
    OZ(ObAIFuncJsonUtils::get_json_boolean(*params.allocator_, !embedding &&
        expression->get_param_expr(1)->get_result_type().is_json(), prompt_is_object));
    OZ(json->add("prompt_is_object", prompt_is_object));
    ObJsonInt *dim = nullptr;
    OZ(ObAIFuncJsonUtils::get_json_int(*params.allocator_, dimension, dim));
    OZ(json->add("dimension", dim));
    ObString serialized;
    OZ(ObAIFuncJsonUtils::print_json_to_str(*params.allocator_, json, serialized));
    OZ(column.set_ai_column_definition(serialized));
  }
  if (ret == OB_INVALID_ARGUMENT) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN requires an AI function, independent ordinary columns and a compatible nullable type");
  }
  return ret;
}

int AiColumnDefinition::add_hidden_columns(ObTableSchema &table, bool altering, const ObTableSchema *original)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> names;
  ObArray<bool> dropping;
  for (auto it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
    const ObColumnSchemaV2 *old = original == nullptr ? nullptr : original->get_column_schema((*it)->get_column_name_str());
    const bool drop = altering && static_cast<const AlterColumnSchema *>(*it)->alter_type_ == OB_DDL_DROP_COLUMN;
    if ((*it)->is_ai_column() || (drop && old != nullptr && old->is_ai_column())) {
      OZ(names.push_back((*it)->get_column_name_str()));
      OZ(dropping.push_back(drop));
    }
  }
  uint64_t cid = table.get_max_used_column_id();
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    const char *prefixes[] = {"__ai_state_", "__ai_request_", "__ai_spec_"};
    for (int j = 0; OB_SUCC(ret) && j < 3; ++j) {
      ObSqlString name;
      OZ(name.append_fmt("%s%.*s", prefixes[j], names.at(i).length(), names.at(i).ptr()));
      if (table.get_column_schema(name.string()) != nullptr) { continue; }
      const ObColumnSchemaV2 *old_hidden = original == nullptr ? nullptr : original->get_column_schema(name.string());
      if (!dropping.at(i) && old_hidden != nullptr) { continue; }
      if (dropping.at(i) && old_hidden == nullptr) { continue; }
      AlterColumnSchema hidden;
      hidden.alter_type_ = dropping.at(i) ? OB_DDL_DROP_COLUMN : OB_DDL_ADD_COLUMN;
      hidden.set_table_id(table.get_table_id());
      hidden.set_column_id(altering ? 0 : ++cid);
      hidden.set_data_type(j == 0 ? ObIntType : ObVarcharType);
      hidden.set_nullable(true);
      hidden.set_is_hidden(true);
      hidden.set_data_length(64);
      hidden.set_charset_type(j == 0 ? CHARSET_BINARY : CHARSET_UTF8MB4);
      hidden.set_collation_type(j == 0 ? CS_TYPE_BINARY : CS_TYPE_UTF8MB4_BIN);
      OZ(hidden.set_column_name(name.string()));
      OZ(hidden.set_origin_column_name(name.string()));
      if (altering) { OZ(static_cast<AlterTableSchema &>(table).add_alter_column(hidden, true)); }
      else { OZ(table.add_column(hidden)); }
    }
  }
  if (!altering) { table.set_max_used_column_id(cid); }
  return ret;
}

AiColumnService &AiColumnService::instance()
{
  static AiColumnService service;
  return service;
}
bool AiColumnService::is_internal() { return ai_column_internal; }
bool AiColumnService::allows_hidden_columns() { return ai_column_internal || ai_column_force != 0; }
bool AiColumnService::has_columns(const ObTableSchema &table)
{
  bool result = false;
  for (auto it = table.column_begin(); !result && it != table.column_end(); ++it) {
    result = OB_NOT_NULL(*it) && !(*it)->is_unused() && (*it)->is_ai_column();
  }
  return result;
}
int AiColumnService::start_service()
{
  stopping_.store(false);
  int ret = set_thread_count(4);
  if (OB_SUCC(ret)) { ret = start(); }
  return ret;
}
void AiColumnService::stop_service() { stopping_.store(true); stop(); wait(); }

int AiColumnService::process_dml(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op, bool *touched)
{
  int ret = OB_SUCCESS;
  if (touched != nullptr) { *touched = false; }
  if (!ctdef.is_primary_index_ || is_internal()) { return ret; }
  ObArenaAllocator allocator("AiColumnDml");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const uint64_t tid = ctdef.das_base_ctdef_.index_tid_;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(tid, table))) {
  } else if (OB_ISNULL(table)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (!has_columns(*table)) {
  } else {
    const bool inserting = ctdef.old_row_.empty();
    const bool deleting = ctdef.new_row_.empty();
    ObSqlString old_filter;
    ObSqlString new_filter;
    if (!inserting) { OZ(ai_row_filter(*table, ctdef, ctdef.old_row_, op, allocator, old_filter)); }
    if (!deleting) { OZ(ai_row_filter(*table, ctdef, ctdef.new_row_, op, allocator, new_filter)); }
    if (OB_SUCC(ret) && inserting && static_cast<const ObInsCtDef &>(ctdef).das_ctdef_.is_ignore_) {
      // Match native INSERT IGNORE's current-row conflict check before creating
      // any durable work. The nested read shares the business transaction.
      ObSqlString existing;
      OZ(existing.append("SELECT 1 FROM "));
      OZ(ai_append_table(existing, *table, guard));
      OZ(existing.append_fmt(" WHERE %.*s FOR UPDATE", new_filter.length(), new_filter.ptr()));
      bool duplicate = false;
      InternalAiColumnScope internal;
      OZ(ai_read_with_session(op.get_exec_ctx(), existing, duplicate));
      if (OB_SUCC(ret) && duplicate) { return ret; }
    }
    if (!inserting && (deleting || old_filter.string() != new_filter.string())) {
      ObSqlString remove;
      OZ(remove.append_fmt("DELETE FROM %s WHERE ", AI_TASK_TABLE));
      OZ(ai_task_key(remove, tid, old_filter.string()));
      OZ(ai_execute_nested(op, remove));
    }
    if (OB_SUCC(ret) && !deleting) {
      ObSqlString group;
      OZ(ai_new_version(group));
      for (auto it = table->column_begin(); OB_SUCC(ret) && it != table->column_end(); ++it) {
        const ObColumnSchemaV2 &column = **it;
        if (column.is_unused() || !column.is_ai_column()) { continue; }
        AiColumnDefinition definition;
        OZ(definition.read(column.get_ai_column_definition(), allocator));
        if (ai_column_force == 2 && column.get_column_id() != ai_column_retry_id) { continue; }
        bool changed = inserting || old_filter.string() != new_filter.string() || ai_column_force == 2;
        for (int64_t i = 0; OB_SUCC(ret) && !changed && i < definition.dependencies_.count(); ++i) {
          const ObColumnSchemaV2 *dependency = table->get_column_schema(definition.dependencies_.at(i));
          ObString old_value;
          ObString new_value;
          if (OB_ISNULL(dependency)) { ret = OB_SCHEMA_EAGAIN; }
          else {
            OZ(ai_row_literal(ctdef, ctdef.old_row_, dependency->get_column_id(), op, allocator, old_value));
            OZ(ai_row_literal(ctdef, ctdef.new_row_, dependency->get_column_id(), op, allocator, new_value));
            changed = old_value != new_value;
          }
        }
        if (OB_SUCC(ret) && !changed && ai_column_force == 1) {
          ObSqlString name;
          OZ(name.append_fmt("__ai_spec_%.*s", column.get_column_name_str().length(), column.get_column_name_str().ptr()));
          const ObColumnSchemaV2 *hidden = table->get_column_schema(name.string());
          ObString old_spec;
          if (hidden == nullptr) { ret = OB_SCHEMA_EAGAIN; }
          else {
            OZ(ai_row_literal(ctdef, ctdef.old_row_, hidden->get_column_id(), op, allocator, old_spec));
            for (int64_t k = 0; OB_SUCC(ret) && k < ctdef.column_ids_.count(); ++k) {
              if (ctdef.column_ids_.at(k) == hidden->get_column_id()) {
                ObDatum *datum = nullptr;
                OZ(ctdef.old_row_.at(k)->eval(op.get_eval_ctx(), datum));
                if (OB_SUCC(ret)) {
                  changed = datum->is_null() || datum->get_string() != definition.version_;
                }
                break;
              }
            }
          }
        }
        if (!changed || OB_FAIL(ret)) { continue; }
        if (touched != nullptr) { *touched = true; }
        // All native data and index writers observe NULL in the new row.
        bool found_result = false;
        for (int64_t i = 0; OB_SUCC(ret) && i < ctdef.column_ids_.count(); ++i) {
          if (ctdef.column_ids_.at(i) == column.get_column_id()) {
            ObDatum *datum = nullptr;
            if (i >= ctdef.new_row_.count()) { ret = OB_ERR_UNEXPECTED; }
            else if (OB_FAIL(ctdef.new_row_.at(i)->eval(op.get_eval_ctx(), datum))) {
            } else {
              datum->set_null(); found_result = true;
              ObExpr *result_expr = ctdef.new_row_.at(i);
              op.clear_dml_evaluated_flag(result_expr->parent_cnt_, result_expr->parents_);
            }
          }
        }
        if (OB_SUCC(ret) && !found_result) {
          ret = OB_ERR_UNEXPECTED;
          FLOG_INFO("AI result absent from DML row", K(column.get_column_id()), K(ctdef.column_ids_), K(ctdef.new_row_.count()));
        }
        const char *prefixes[] = {"__ai_state_", "__ai_request_", "__ai_spec_"};
        for (int j = 0; OB_SUCC(ret) && j < 3; ++j) {
          ObSqlString name;
          OZ(name.append_fmt("%s%.*s", prefixes[j], column.get_column_name_str().length(), column.get_column_name_str().ptr()));
          const ObColumnSchemaV2 *hidden = table->get_column_schema(name.string());
          bool found = false;
          for (int64_t k = 0; OB_SUCC(ret) && hidden != nullptr && k < ctdef.column_ids_.count(); ++k) {
            if (ctdef.column_ids_.at(k) == hidden->get_column_id()) {
              ObDatum *datum = nullptr;
              OZ(ctdef.new_row_.at(k)->eval(op.get_eval_ctx(), datum));
              if (OB_SUCC(ret)) {
                if (j == 0) {
                  // The new row reuses the old row's datum buffer for untouched
                  // columns, so an in-place write would corrupt the old row that
                  // storage compares against. Point at fresh memory instead.
                  int64_t *state = static_cast<int64_t *>(
                      op.get_exec_ctx().get_allocator().alloc(sizeof(int64_t)));
                  if (OB_ISNULL(state)) {
                    ret = OB_ALLOCATE_MEMORY_FAILED;
                  } else {
                    *state = 0;
                    datum->ptr_ = reinterpret_cast<const char *>(state);
                    datum->pack_ = sizeof(int64_t);
                  }
                } else {
                  ObString value;
                  OZ(ob_write_string(op.get_exec_ctx().get_allocator(),
                      j == 1 ? group.string() : definition.version_, value));
                  if (OB_SUCC(ret)) { datum->set_string(value); }
                }
                found = true;
              }
            }
          }
          if (OB_SUCC(ret) && !found) {
            ret = OB_ERR_UNEXPECTED;
            FLOG_INFO("AI state absent from DML row", K(name), K(ctdef.column_ids_), K(ctdef.new_row_.count()));
          }
        }
        ObSqlString input;
        OZ(input.append_fmt("SELECT CAST((%.*s) AS CHAR) AS prompt, CAST((%.*s) AS CHAR) AS config FROM (SELECT ",
            definition.prompt_expression_.length(), definition.prompt_expression_.ptr(),
            definition.config_expression_.length(), definition.config_expression_.ptr()));
        // Snapshot only ordinary dependencies, with SQL literals produced by the engine.
        if (definition.dependencies_.empty()) { OZ(input.append("1 AS __ai_unused")); }
        for (int64_t i = 0; OB_SUCC(ret) && i < definition.dependencies_.count(); ++i) {
          const ObColumnSchemaV2 *dependency = table->get_column_schema(definition.dependencies_.at(i));
          ObString value;
          OZ(ai_row_literal(ctdef, ctdef.new_row_, dependency->get_column_id(), op, allocator, value));
          if (i > 0) { OZ(input.append(",")); }
          OZ(input.append(value));
          OZ(input.append(" AS "));
          OZ(ai_append_identifier(input, dependency->get_column_name_str()));
        }
        OZ(input.append(") AS __ai_input"));
        ObSqlString enqueue;
        OZ(enqueue.append_fmt("REPLACE INTO %s (table_id,row_key,column_id,request_id,group_id,"
            "definition,state,attempt,max_retries,lease_until,input_sql,row_filter,rowkey_predicate) VALUES (%lu,UNHEX(SHA2(",
            AI_TASK_TABLE, tid));
        OZ(ai_append_hex(enqueue, new_filter.string()));
        OZ(enqueue.append_fmt(",256)),%lu,", column.get_column_id()));
        OZ(ai_append_hex(enqueue, group.string()));
        OZ(enqueue.append(","));
        OZ(ai_append_hex(enqueue, group.string()));
        OZ(enqueue.append(","));
        OZ(ai_append_hex(enqueue, column.get_ai_column_definition()));
        OZ(enqueue.append_fmt(",0,0,%ld,0,", definition.max_retries_));
        OZ(ai_append_hex(enqueue, input.string()));
        OZ(enqueue.append(","));
        OZ(ai_append_hex(enqueue, new_filter.string()));
        OZ(enqueue.append(","));
        OZ(ai_append_hex(enqueue, new_filter.string()));
        OZ(enqueue.append(")"));
        OZ(ai_execute_nested(op, enqueue));
      }
    }
  }
  if (OB_FAIL(ret)) { FLOG_INFO("AI column DML failed", K(ret), K(tid), K(ctdef.column_ids_)); }
  return ret;
}

namespace {
int ai_read_with_session(ObExecContext &context, const ObSqlString &sql,
                        bool &has_row)
{
  int ret = OB_SUCCESS;
  has_row = false;
  AiSqlScope sql_scope(context);
  common::sqlclient::ObISQLConnectionGuard connection;
  ObSQLSessionInfo *session = context.get_my_session();
  if (OB_ISNULL(session)) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, connection))) {
  } else {
    ObIInnerSQLConnection *inner = as_inner_sql_connection(connection.get_ptr());
    ObSQLSessionInfo::StmtSavedValue saved_session;
    ObIInnerSQLConnection::SavedValue saved_connection;
    if (OB_ISNULL(inner)) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(inner->begin_nested_session(saved_session, saved_connection, true))) {
    } else {
      {
        ObMySQLProxy::MySQLResult result;
        if (OB_FAIL(inner->execute_read(sql.string(), result))) {
        } else if (OB_ISNULL(result.get_result())) { ret = OB_ERR_UNEXPECTED; }
        else if (OB_FAIL(result.get_result()->next())) {
          if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        } else { has_row = true; }
      }
      int end_ret = inner->end_nested_session(saved_session, saved_connection);
      if (OB_SUCC(ret)) { ret = end_ret; }
    }
  }
  return ret;
}
}

int AiColumnService::check_strict(ObExecContext &context, uint64_t table_id,
                                 const ObIArray<uint64_t> &columns)
{
  int ret = OB_SUCCESS;
  bool strict = false;
  if (is_internal() || columns.empty()) { return ret; }
  if (OB_FAIL(context.get_my_session()->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, strict))) {
  } else if (strict) {
    ObSchemaGetterGuard guard;
    const ObTableSchema *table = nullptr;
    OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
    OZ(guard.get_table_schema(table_id, table));
    for (int64_t i = 0; OB_SUCC(ret) && OB_NOT_NULL(table) && i < columns.count(); ++i) {
      const ObColumnSchemaV2 *column = table->get_column_schema(columns.at(i));
      if (OB_NOT_NULL(column) && column->is_ai_column()) {
        ObSqlString sql;
        ObSqlString state_name;
        OZ(state_name.append_fmt("__ai_state_%.*s", column->get_column_name_str().length(), column->get_column_name_str().ptr()));
        OZ(sql.append("SELECT 1 FROM "));
        OZ(ai_append_table(sql, *table, guard));
        OZ(sql.append(" WHERE COALESCE("));
        OZ(ai_append_identifier(sql, state_name.string()));
        OZ(sql.append(",0)<>1 LIMIT 1"));
        bool pending = false;
        InternalAiColumnScope internal;
        if (OB_FAIL(ai_read_with_session(context, sql, pending))) {
        } else if (pending) {
          ret = OB_AI_COLUMN_NOT_READY;
          LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, table_id, columns.at(i));
        }
      }
    }
  }
  return ret;
}

namespace {
struct AiClaimedTask
{
  uint64_t table_id_ = OB_INVALID_ID;
  uint64_t column_id_ = OB_INVALID_ID;
  ObString request_id_;
  ObString group_id_;
  ObString definition_;
  ObString input_sql_;
  ObString row_filter_;
  ObString result_sql_;
  int64_t attempt_ = 0;
  int64_t max_retries_ = 0;
  int error_ = OB_SUCCESS;
};
int ai_copy_result_string(common::sqlclient::ObMySQLResult &result, const char *column,
                          ObIAllocator &allocator, ObString &value, bool nullable = false)
{
  int ret = OB_SUCCESS;
  ObString temporary;
  if (OB_FAIL(result.get_varchar(column, temporary))) {
    if (nullable && ret == OB_ERR_NULL_VALUE) { ret = OB_SUCCESS; }
  } else { ret = ob_write_string(allocator, temporary, value, true); }
  return ret;
}
int ai_claim_predicate(ObSqlString &sql, const ObString &claim, const ObString &request, uint64_t column_id)
{
  int ret = sql.append("claim_id=");
  OZ(ai_append_hex(sql, claim));
  OZ(sql.append(" AND request_id="));
  OZ(ai_append_hex(sql, request));
  OZ(sql.append_fmt(" AND column_id=%lu", column_id));
  return ret;
}
int ai_write(ObSqlString &sql, int64_t &affected)
{
  return GCTX.sql_proxy_->write(sql.ptr(), affected);
}
}

int AiColumnService::run_one()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnTask");
  ObString group_id;
  const int64_t now = ObTimeUtility::current_time();
  {
    ObMySQLProxy::MySQLResult result;
    ObSqlString select;
    OZ(select.append_fmt("SELECT group_id FROM %s WHERE state IN (0,3) AND lease_until<%ld "
                         "ORDER BY gmt_create LIMIT 1", AI_TASK_TABLE, now));
    OZ(GCTX.sql_proxy_->read(result, select.ptr()));
    if (OB_SUCC(ret)) {
      if (OB_FAIL(result.get_result()->next())) {
      } else { OZ(ai_copy_result_string(*result.get_result(), "group_id", allocator, group_id)); }
    }
  }
  if (OB_FAIL(ret)) { return ret; }
  ObSqlString claim;
  OZ(ai_new_version(claim));
  {
    ObSqlString update;
    int64_t affected = 0;
    OZ(update.append_fmt("UPDATE %s SET claim_id=", AI_TASK_TABLE));
    OZ(ai_append_hex(update, claim.string()));
    OZ(update.append_fmt(",lease_until=%ld WHERE state IN (0,3) AND lease_until<%ld AND group_id=", now + 60000000, now));
    OZ(ai_append_hex(update, group_id));
    OZ(ai_write(update, affected));
    if (OB_SUCC(ret) && affected == 0) { ret = OB_EAGAIN; }
  }
  ObArray<AiClaimedTask> tasks;
  if (OB_SUCC(ret)) {
    ObMySQLProxy::MySQLResult result;
    ObSqlString select;
    OZ(select.append_fmt("SELECT * FROM %s WHERE claim_id=", AI_TASK_TABLE));
    OZ(ai_append_hex(select, claim.string()));
    OZ(GCTX.sql_proxy_->read(result, select.ptr()));
    while (OB_SUCC(ret) && OB_SUCC(ret = result.get_result()->next())) {
      AiClaimedTask task;
      int64_t tid = 0;
      int64_t cid = 0;
      OZ(result.get_result()->get_int("table_id", tid));
      OZ(result.get_result()->get_int("column_id", cid));
      task.table_id_ = tid;
      task.column_id_ = cid;
      OZ(ai_copy_result_string(*result.get_result(), "request_id", allocator, task.request_id_));
      OZ(ai_copy_result_string(*result.get_result(), "group_id", allocator, task.group_id_));
      OZ(ai_copy_result_string(*result.get_result(), "definition", allocator, task.definition_));
      OZ(ai_copy_result_string(*result.get_result(), "input_sql", allocator, task.input_sql_));
      OZ(ai_copy_result_string(*result.get_result(), "row_filter", allocator, task.row_filter_));
      OZ(ai_copy_result_string(*result.get_result(), "result_sql", allocator, task.result_sql_, true));
      OZ(result.get_result()->get_int("attempt", task.attempt_));
      OZ(result.get_result()->get_int("max_retries", task.max_retries_));
      OZ(tasks.push_back(task));
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count() && !has_set_stop(); ++i) {
    AiClaimedTask &task = tasks.at(i);
    if (!task.result_sql_.empty()) { continue; }
    task.error_ = OB_ERROR;
    while (task.result_sql_.empty() && task.attempt_ <= task.max_retries_ && !has_set_stop()) {
      ObSqlString attempt;
      int64_t affected = 0;
      OZ(attempt.append_fmt("UPDATE %s SET attempt=attempt+1,lease_until=%ld WHERE ",
                           AI_TASK_TABLE, ObTimeUtility::current_time() + 60000000));
      OZ(ai_claim_predicate(attempt, claim.string(), task.request_id_, task.column_id_));
      OZ(ai_write(attempt, affected));
      if (OB_FAIL(ret) || affected == 0) { break; }
      ++task.attempt_;
      ObSqlString heartbeat;
      OZ(heartbeat.append_fmt("UPDATE %s SET lease_until=%ld WHERE claim_id=", AI_TASK_TABLE,
          ObTimeUtility::current_time() + 60000000));
      OZ(ai_append_hex(heartbeat, claim.string()));
      OZ(ai_write(heartbeat, affected));
      {
        ObMySQLProxy::MySQLResult result;
        // Evaluate the pure arguments on the committed dependency snapshot.
        THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + 30000000);
        task.error_ = GCTX.sql_proxy_->read(result, task.input_sql_.ptr());
        if (task.error_ == OB_SUCCESS) {
          task.error_ = result.get_result()->next();
          if (task.error_ == OB_SUCCESS) {
            ObString prompt;
            ObString config_text;
            AiColumnDefinition definition;
            task.error_ = definition.read(task.definition_, allocator);
            if (task.error_ == OB_SUCCESS) { task.error_ = ai_copy_result_string(*result.get_result(), "prompt", allocator, prompt, true); }
            if (task.error_ == OB_SUCCESS && definition.prompt_is_object_ && prompt.ptr() != nullptr) {
              ObJsonObject *object = nullptr;
              task.error_ = ObAIFuncJsonUtils::get_json_object_form_str(allocator, prompt, object);
              if (task.error_ == OB_SUCCESS) {
                task.error_ = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, object, prompt);
              }
            }
            if (task.error_ == OB_SUCCESS) { task.error_ = ai_copy_result_string(*result.get_result(), "config", allocator, config_text, true); }
            ObJsonObject *config = nullptr;
            if (task.error_ == OB_SUCCESS && !config_text.empty()) {
              task.error_ = ObAIFuncJsonUtils::get_json_object_form_str(allocator, config_text, config);
            }
            const bool input_is_null = prompt.ptr() == nullptr;
            ObString inference;
            ObAIFuncExprInfo *info = nullptr;
            share::ObAiModelEndpointInfo endpoint;
            auto *resolver = share::server_service<query::ObIAiEndpointResolver>();
            if (task.error_ == OB_SUCCESS && !input_is_null) { task.error_ = ObAIFuncUtils::get_ai_func_info(allocator, definition.model_, info); }
            if (task.error_ == OB_SUCCESS && !input_is_null) { task.error_ = resolver == nullptr ? OB_NOT_INIT : resolver->resolve_by_model_name(definition.model_, allocator, endpoint); }
            if (task.error_ == OB_SUCCESS && !input_is_null) {
              // Internal SQL restores its own worker deadline. Give the HTTP
              // operation a fresh, explicit budget after metadata resolution.
              THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + 30000000);
              ObAIFuncModel model(allocator, *info, endpoint);
              if (definition.embedding_) {
                ObJsonInt *dim = nullptr;
                task.error_ = ObAIFuncJsonUtils::get_json_object(allocator, config);
                if (task.error_ == OB_SUCCESS) { task.error_ = ObAIFuncJsonUtils::get_json_int(allocator, definition.dimension_, dim); }
                if (task.error_ == OB_SUCCESS) { task.error_ = config->add("dimensions", dim); }
                ObArray<ObString> inputs;
                ObArray<ObString> outputs;
                if (task.error_ == OB_SUCCESS) { task.error_ = inputs.push_back(prompt); }
                if (task.error_ == OB_SUCCESS) { task.error_ = model.call_dense_embedding_vector_v2(inputs, config, outputs, 0, &stopping_); }
                if (task.error_ == OB_SUCCESS) { inference = outputs.at(0); }
              } else {
                task.error_ = model.call_completion(prompt, config, inference, definition.system_prompt_, 0, &stopping_);
              }
            }
            if (task.error_ == OB_SUCCESS && !input_is_null) {
              task.error_ = ai_validate_result(task.table_id_, task.column_id_, inference, allocator);
            }
            if (task.error_ == OB_SUCCESS && input_is_null) {
              task.result_sql_ = ObString("NULL");
            } else if (task.error_ == OB_SUCCESS) {
              ObSqlString literal;
              task.error_ = literal.append("CAST(");
              if (task.error_ == OB_SUCCESS) { task.error_ = ai_append_hex(literal, inference); }
              if (task.error_ == OB_SUCCESS) { task.error_ = literal.append(" AS CHAR CHARACTER SET utf8mb4)"); }
              if (task.error_ == OB_SUCCESS) { task.error_ = ob_write_string(allocator, literal.string(), task.result_sql_, true); }
            }
          }
        }
      }
      // HTTP timeout must not prevent persisting the failure and publishing
      // the terminal state in a separate database transaction.
      THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + 30000000);
      if (task.error_ != OB_SUCCESS) {
        FLOG_INFO("AI column inference attempt failed", K(task.table_id_), K(task.column_id_), K(task.attempt_), K(task.error_));
      }
      if (has_set_stop() && task.error_ == OB_CANCELED) {
        ObSqlString undo;
        OZ(undo.append_fmt("UPDATE %s SET attempt=GREATEST(attempt-1,0) WHERE ", AI_TASK_TABLE));
        OZ(ai_claim_predicate(undo, claim.string(), task.request_id_, task.column_id_));
        OZ(ai_write(undo, affected));
        break;
      }
      // Retain successful inference across process failure before grouped publication.
      ObSqlString save;
      OZ(save.append_fmt("UPDATE %s SET state=%d,retry_count=%ld,error_code=%d,result_sql=",
          AI_TASK_TABLE, task.error_ == OB_SUCCESS ? 0 : 3,
          std::min(task.attempt_, task.max_retries_), task.error_));
      if (task.result_sql_.empty()) { OZ(save.append("NULL")); }
      else { OZ(ai_append_hex(save, task.result_sql_)); }
      OZ(save.append(" WHERE "));
      OZ(ai_claim_predicate(save, claim.string(), task.request_id_, task.column_id_));
      OZ(ai_write(save, affected));
      if (affected == 0) { break; }
      if (task.error_ != OB_SUCCESS) {
        // Bounded backoff; shutdown never waits through a long sleep.
        for (int pause = 0; pause < 100 && !has_set_stop(); ++pause) {
          ob_usleep(10000);
        }
      }
    }
  }
  if (has_set_stop()) {
    ObSqlString release;
    int64_t affected = 0;
    OZ(release.append_fmt("UPDATE %s SET claim_id=NULL,lease_until=0 WHERE claim_id=", AI_TASK_TABLE));
    OZ(ai_append_hex(release, claim.string()));
    OZ(ai_write(release, affected));
    return ret;
  }
  if (OB_FAIL(ret) || tasks.empty()) { return ret; }
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schema(tasks.at(0).table_id_, table));
  if (OB_SUCC(ret) && OB_ISNULL(table)) {
    ObSqlString cleanup;
    int64_t affected = 0;
    OZ(cleanup.append_fmt("DELETE FROM %s WHERE claim_id=", AI_TASK_TABLE));
    OZ(ai_append_hex(cleanup, claim.string()));
    OZ(ai_write(cleanup, affected));
    return ret;
  }
  ObMySQLTransaction transaction;
  if (OB_SUCC(ret)) { OZ(transaction.start(GCTX.sql_proxy_)); }
  bool row_exists = false;
  if (OB_SUCC(ret)) {
    ObSqlString lock;
    OZ(lock.append("SELECT 1 FROM "));
    OZ(ai_append_table(lock, *table, guard));
    OZ(lock.append_fmt(" WHERE %.*s FOR UPDATE", tasks.at(0).row_filter_.length(), tasks.at(0).row_filter_.ptr()));
    {
      ObMySQLProxy::MySQLResult result;
      OZ(transaction.read(result, lock.ptr()));
      if (OB_SUCC(ret)) {
        ret = result.get_result()->next();
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        else if (OB_SUCC(ret)) { row_exists = true; }
      }
    }
  }
  // Lock and recheck each current request after taking the business-row lock.
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    AiClaimedTask &task = tasks.at(i);
    bool current = false;
    ObSqlString check;
    OZ(check.append_fmt("SELECT request_id FROM %s WHERE ", AI_TASK_TABLE));
    OZ(ai_claim_predicate(check, claim.string(), task.request_id_, task.column_id_));
    OZ(check.append(" FOR UPDATE"));
    {
      ObMySQLProxy::MySQLResult result;
      OZ(transaction.read(result, check.ptr()));
      if (OB_SUCC(ret)) {
        ret = result.get_result()->next();
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        else if (OB_SUCC(ret)) { current = true; }
      }
    }
    if (!current || OB_FAIL(ret)) { continue; }
    const ObColumnSchemaV2 *column = table->get_column_schema(task.column_id_);
    const bool valid = row_exists && OB_NOT_NULL(column) && column->is_ai_column()
        && column->get_ai_column_definition() == task.definition_;
    int64_t affected = 0;
    if (valid) {
      ObSqlString publish;
      OZ(publish.append("UPDATE "));
      OZ(ai_append_table(publish, *table, guard));
      OZ(publish.append(" SET "));
      OZ(ai_append_identifier(publish, column->get_column_name_str()));
      OZ(publish.append("="));
      OZ(publish.append(task.result_sql_.empty() ? ObString("NULL") : task.result_sql_));
      OZ(publish.append(","));
      ObSqlString state_name;
      OZ(state_name.append_fmt("__ai_state_%.*s", column->get_column_name_str().length(), column->get_column_name_str().ptr()));
      OZ(ai_append_identifier(publish, state_name.string()));
      OZ(publish.append_fmt("=%d WHERE %.*s AND ", task.result_sql_.empty() ? 2 : 1,
          task.row_filter_.length(), task.row_filter_.ptr()));
      ObSqlString request_name;
      OZ(request_name.append_fmt("__ai_request_%.*s", column->get_column_name_str().length(), column->get_column_name_str().ptr()));
      OZ(ai_append_identifier(publish, request_name.string()));
      OZ(publish.append("="));
      OZ(ai_append_hex(publish, task.request_id_));
      if (OB_SUCC(ret)) { ret = transaction.write(publish.ptr(), affected); }
    }
    ObSqlString finish;
    OZ(finish.append_fmt("DELETE FROM %s WHERE ", AI_TASK_TABLE));
    OZ(ai_claim_predicate(finish, claim.string(), task.request_id_, task.column_id_));
    OZ(transaction.write(finish.ptr(), affected));
  }
  if (transaction.is_started()) {
    int end_ret = transaction.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}

void AiColumnService::run1()
{
  lib::set_thread_name("AiColumn");
  InternalAiColumnScope internal;
  int64_t last_recovery = 0;
  while (!has_set_stop()) {
    THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + 30000000);
    int ret = OB_NOT_INIT;
    if (GCTX.status_ == share::SS_SERVING && OB_NOT_NULL(GCTX.sql_proxy_) && OB_NOT_NULL(GCTX.schema_service_)) {
      if (!recovered_.load()) {
        if (get_thread_idx() == 0) {
          ObSqlString recover;
          int64_t affected = 0;
          // A process restart abandons every previous lease. An interrupted
          // attempt without a saved response can safely be executed again.
          if (OB_SUCC(recover.append_fmt("UPDATE %s SET attempt=IF(state=0 AND result_sql IS NULL AND claim_id IS NOT NULL,GREATEST(attempt-1,0),attempt),claim_id=NULL,lease_until=0", AI_TASK_TABLE))
              && OB_SUCC(ai_write(recover, affected))) { recovered_.store(true); }
        }
        if (!recovered_.load()) { ob_usleep(10000); continue; }
      }
      if (get_thread_idx() == 0 && ObTimeUtility::current_time() - last_recovery > 1000000) {
        (void)recover_definitions();
        last_recovery = ObTimeUtility::current_time();
      }
      ret = run_one();
    }
    if (ret != OB_SUCCESS) { ob_usleep(10000); }
  }
}

int AiColumnService::backfill(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schema(table_id, table));
  if (OB_SUCC(ret) && OB_NOT_NULL(table) && has_columns(*table)) {
    uint64_t cid = OB_INVALID_ID;
    OZ(table->get_rowkey_info().get_column_id(0, cid));
    const ObColumnSchemaV2 *column = table->get_column_schema(cid);
    if (OB_ISNULL(column)) { ret = OB_ERR_UNEXPECTED; }
    ObSqlString sql;
    OZ(sql.append("UPDATE "));
    OZ(ai_append_table(sql, *table, guard));
    OZ(sql.append(" SET "));
    OZ(ai_append_identifier(sql, column->get_column_name_str()));
    OZ(sql.append("="));
    OZ(ai_append_identifier(sql, column->get_column_name_str()));
    const bool saved_internal = ai_column_internal;
    const int saved_force = ai_column_force;
    ai_column_internal = false;
    ai_column_force = 1;
    int64_t affected = 0;
    if (OB_SUCC(ret)) { ret = ai_write(sql, affected); }
    ai_column_internal = saved_internal;
    ai_column_force = saved_force;
  }
  return ret;
}

int AiColumnService::recover_definitions()
{
  // Only worker zero calls this cache. Durable task records decide whether an
  // existing row requires backfill, so restarting does not repeat READY work.
  static ObArray<uint64_t> observed_tables;
  static ObArray<uint64_t> observed_versions;
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  ObArray<const ObTableSchema *> tables;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schemas_in_runtime(tables));
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count() && !has_set_stop(); ++i) {
    const ObTableSchema &table = *tables.at(i);
    if (table.is_in_recyclebin() || !has_columns(table)) { continue; }
    uint64_t hash = 0;
    for (auto it = table.column_begin(); it != table.column_end(); ++it) {
      if (!(*it)->is_unused() && (*it)->is_ai_column()) { hash = (*it)->get_ai_column_definition().hash(hash); }
    }
    int64_t previous = -1;
    for (int64_t j = 0; j < observed_tables.count(); ++j) {
      if (observed_tables.at(j) == table.get_table_id()) { previous = j; break; }
    }
    if (previous < 0 || observed_versions.at(previous) != hash) {
      OZ(backfill(table.get_table_id()));
      if (OB_SUCC(ret) && previous < 0) {
        OZ(observed_tables.push_back(table.get_table_id()));
        OZ(observed_versions.push_back(hash));
        if (OB_FAIL(ret) && observed_tables.count() > observed_versions.count()) { observed_tables.pop_back(); }
      } else if (OB_SUCC(ret)) { observed_versions.at(previous) = hash; }
    }
  }
  return ret;
}

namespace {
int ai_retry_condition(const ObString &condition, const ObString &state, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  int64_t start = 0;
  while (start < condition.length() && std::isspace(static_cast<unsigned char>(condition.ptr()[start]))) { ++start; }
  if (condition.length() - start >= 5
      && ObString(5, condition.ptr() + start).case_compare("WHERE") == 0) { start += 5; }
  for (int64_t i = start; OB_SUCC(ret) && i < condition.length();) {
    const char c = condition.ptr()[i];
    if (c == '\'' || c == '"' || c == '`') {
      const int64_t begin = i++;
      while (i < condition.length()) {
        if (condition.ptr()[i] == '\\' && c != '`' && i + 1 < condition.length()) { i += 2; }
        else if (condition.ptr()[i] == c) {
          ++i;
          if (i < condition.length() && condition.ptr()[i] == c) { ++i; }
          else { break; }
        } else { ++i; }
      }
      OZ(out.append(condition.ptr() + begin, i - begin));
    } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const int64_t begin = i++;
      while (i < condition.length() && (std::isalnum(static_cast<unsigned char>(condition.ptr()[i])) || condition.ptr()[i] == '_')) { ++i; }
      ObString word(i - begin, condition.ptr() + begin);
      if (word.case_compare("status") == 0) { OZ(out.append(state)); }
      else { OZ(out.append(word)); }
    } else {
      OZ(out.append(condition.ptr() + i, 1));
      ++i;
    }
  }
  return ret;
}
}

int AiColumnService::retry(ObExecContext &context, uint64_t table_id,
                           const ObString &column_name, const ObString &condition)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnRetry");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObColumnSchemaV2 *column = nullptr;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schema(table_id, table));
  if (OB_SUCC(ret) && (OB_ISNULL(table) || OB_ISNULL(column = table->get_column_schema(column_name))
      || !column->is_ai_column())) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
  }
  if (OB_SUCC(ret)) {
    const bool saved_internal = ai_column_internal;
    const int saved_force = ai_column_force;
    const uint64_t saved_column = ai_column_retry_id;
    ai_column_internal = false;
    ai_column_force = 2;
    ai_column_retry_id = column->get_column_id();
    uint64_t key_cid = OB_INVALID_ID;
    OZ(table->get_rowkey_info().get_column_id(0, key_cid));
    const ObColumnSchemaV2 *key = table->get_column_schema(key_cid);
    ObSqlString state_name;
    ObSqlString state_expression;
    OZ(state_name.append_fmt("__ai_state_%.*s", column->get_column_name_str().length(), column->get_column_name_str().ptr()));
    OZ(state_expression.append("(CASE COALESCE("));
    OZ(ai_append_identifier(state_expression, state_name.string()));
    OZ(state_expression.append(",0) WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' ELSE 'PENDING' END)"));
    ObSqlString predicate;
    if (!condition.empty()) { OZ(ai_retry_condition(condition, state_expression.string(), predicate)); }
    ObSqlString update;
    OZ(update.append("UPDATE "));
    OZ(ai_append_table(update, *table, guard));
    OZ(update.append(" SET "));
    OZ(ai_append_identifier(update, key->get_column_name_str()));
    OZ(update.append("="));
    OZ(ai_append_identifier(update, key->get_column_name_str()));
    if (!condition.empty()) { OZ(update.append_fmt(" WHERE (%.*s)", predicate.length(), predicate.ptr())); }
    int64_t affected = 0;
    OZ(GCTX.sql_proxy_->write(update.ptr(), affected));
    ai_column_internal = saved_internal;
    ai_column_force = saved_force;
    ai_column_retry_id = saved_column;

  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
