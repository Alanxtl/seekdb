/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}

namespace oceanbase
{
namespace sql
{
namespace
{
const char *describe_type_name(const ObObjType type)
{
  switch (type) {
    case common::ObTinyIntType:
    case common::ObSmallIntType:
    case common::ObMediumIntType:
    case common::ObInt32Type:
      return "BOOLEAN";
    case common::ObIntType:
    case common::ObUInt64Type:
      return "BIGINT";
    case common::ObFloatType:
    case common::ObDoubleType:
    case common::ObUFloatType:
    case common::ObUDoubleType:
      return "DOUBLE";
    case common::ObDateType:
      return "DATE";
    case common::ObDateTimeType:
    case common::ObTimestampType:
      return "DATETIME";
    case common::ObNullType:
      return "NULL";
    default:
      return "VARCHAR";
  }
}

int append_sql_string(ObSqlString &sql, const ObString &value)
{
  int ret = common::OB_SUCCESS;
  OZ (sql.append("'"));
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if (c == '\'' || c == '\\') {
      OZ (sql.append(&c, 1));
    }
    OZ (sql.append(&c, 1));
  }
  OZ (sql.append("'"));
  return ret;
}
} // namespace

int ObExplainResolver::resolve_describe_select(const ParseNode &select_node)
{
  int ret = common::OB_SUCCESS;
  ObResolver inner_resolver(params_);
  ObStmt *inner_stmt = nullptr;
  ObSelectStmt *select_stmt = nullptr;
  ObSqlString sql;
  if (OB_FAIL(inner_resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, select_node, inner_stmt))) {
  } else if (OB_ISNULL(select_stmt = static_cast<ObSelectStmt *>(inner_stmt))) {
    ret = common::OB_ERR_UNEXPECTED;
  } else {
    const ObIArray<SelectItem> &items = select_stmt->get_select_items();
    for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
      const SelectItem &item = items.at(i);
      const ObString &name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      const ObExprResType &type = item.expr_->get_result_type();
      const char *null_flag = type.has_result_flag(NOT_NULL_FLAG) ? "NO" : "YES";
      if (i > 0) {
        OZ (sql.append(" UNION ALL "));
      }
      OZ (sql.append("SELECT "));
      OZ (append_sql_string(sql, name));
      OZ (sql.append(" AS column_name, "));
      OZ (append_sql_string(sql, ObString::make_string(describe_type_name(type.get_type()))));
      OZ (sql.append(" AS column_type, "));
      OZ (append_sql_string(sql, ObString::make_string(null_flag)));
      OZ (sql.append(" AS `null`, NULL AS `key`, NULL AS `default`, NULL AS extra"));
    }
    if (OB_SUCC(ret)) {
      ParseResult parse_result;
      ObParser parser(*allocator_, session_info_->get_sql_mode(), session_info_->get_charsets4parser());
      ObStmt *meta_stmt = nullptr;
      if (OB_FAIL(parser.parse(sql.string(), parse_result))) {
      } else if (OB_ISNULL(parse_result.result_tree_) ||
                 OB_ISNULL(parse_result.result_tree_->children_) ||
                 parse_result.result_tree_->num_child_ < 1 ||
                 OB_ISNULL(parse_result.result_tree_->children_[0])) {
        ret = common::OB_ERR_UNEXPECTED;
      } else {
        ObResolver meta_resolver(params_);
        if (OB_FAIL(meta_resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                          *parse_result.result_tree_->children_[0], meta_stmt))) {
        } else if (OB_ISNULL(meta_stmt)) {
          ret = common::OB_ERR_UNEXPECTED;
        } else {
          set_basic_stmt(meta_stmt);
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
