/* Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0. */

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/parser/parse_node.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "share/ob_errno.h"
#include "lib/utility/ob_print_utils.h"
#include <glob.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace
{
using common::ObString;
using common::ObObj;
using common::ObObjType;

struct FileColumn
{
  std::string name_;
  ObObjType type_;
  bool nullable_;
  int64_t accuracy_;
  FileColumn() : type_(common::ObVarcharType), nullable_(true), accuracy_(0) {}
};

struct FileCell
{
  bool null_;
  ObObj obj_;
  std::string text_;
  FileCell() : null_(true) {}
};

struct FileTableData
{
  std::vector<FileColumn> columns_;
  std::vector<std::vector<FileCell> > rows_;
};

enum FileValueKind
{
  FV_NULL = 0,
  FV_BOOL,
  FV_INT,
  FV_DOUBLE,
  FV_DATE,
  FV_DATETIME,
  FV_STRING
};

struct InferredType
{
  FileValueKind kind_;
  bool nullable_;
  InferredType() : kind_(FV_NULL), nullable_(false) {}
};

bool ci_equal(const std::string &a, const char *b)
{
  const size_t n = strlen(b);
  if (a.size() != n) { return false; }
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) { return false; }
  }
  return true;
}

bool is_integer_text(const std::string &s)
{
  if (s.empty()) { return false; }
  size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
  if (i == s.size()) { return false; }
  for (; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) { return false; }
  }
  return true;
}

bool is_double_text(const std::string &s)
{
  if (s.empty()) { return false; }
  char *end = nullptr;
  errno = 0;
  (void)std::strtod(s.c_str(), &end);
  return errno == 0 && end != nullptr && *end == '\0' && end != s.c_str();
}

bool parse_date_text(const std::string &s, int &y, int &m, int &d)
{
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') { return false; }
  for (int i : {0, 1, 2, 3, 5, 6, 8, 9}) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) { return false; }
  }
  y = std::atoi(s.substr(0, 4).c_str());
  m = std::atoi(s.substr(5, 2).c_str());
  d = std::atoi(s.substr(8, 2).c_str());
  return y >= 1 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

bool parse_datetime_text(const std::string &s, int &y, int &m, int &d,
                         int &hh, int &mm, int &ss)
{
  if (s.size() != 19 || (s[10] != ' ' && s[10] != 'T') ||
      s[4] != '-' || s[7] != '-' || s[13] != ':' || s[16] != ':') {
    return false;
  }
  if (!parse_date_text(s.substr(0, 10), y, m, d)) { return false; }
  for (int i : {11, 12, 14, 15, 17, 18}) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) { return false; }
  }
  hh = std::atoi(s.substr(11, 2).c_str());
  mm = std::atoi(s.substr(14, 2).c_str());
  ss = std::atoi(s.substr(17, 2).c_str());
  return hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59 && ss >= 0 && ss <= 59;
}

// Days since 1970-01-01, Howard Hinnant's civil_from_days inverse.
int64_t days_from_civil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

FileValueKind classify_text(const std::string &s)
{
  if (s.empty()) { return FV_NULL; }
  if (ci_equal(s, "true") || ci_equal(s, "false")) { return FV_BOOL; }
  int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
  if (parse_datetime_text(s, y, m, d, hh, mm, ss)) { return FV_DATETIME; }
  if (parse_date_text(s, y, m, d)) { return FV_DATE; }
  if (is_integer_text(s)) { return FV_INT; }
  if (is_double_text(s)) { return FV_DOUBLE; }
  return FV_STRING;
}

void merge_kind(InferredType &type, FileValueKind kind)
{
  if (FV_NULL == kind) {
    type.nullable_ = true;
    return;
  }
  if (FV_NULL == type.kind_) {
    type.kind_ = kind;
  } else if (type.kind_ == kind) {
    // same
  } else if ((FV_INT == type.kind_ && FV_DOUBLE == kind) ||
             (FV_DOUBLE == type.kind_ && FV_INT == kind)) {
    type.kind_ = FV_DOUBLE;
  } else if ((FV_DATE == type.kind_ && FV_DATETIME == kind) ||
             (FV_DATETIME == type.kind_ && FV_DATE == kind)) {
    type.kind_ = FV_DATETIME;
  } else {
    type.kind_ = FV_STRING;
  }
}

ObObjType obj_type_of(FileValueKind kind)
{
  switch (kind) {
    case FV_BOOL: return common::ObTinyIntType;
    case FV_INT: return common::ObIntType;
    case FV_DOUBLE: return common::ObDoubleType;
    case FV_DATE: return common::ObDateType;
    case FV_DATETIME: return common::ObDateTimeType;
    case FV_STRING: return common::ObVarcharType;
    default: return common::ObVarcharType;
  }
}

int build_cell_obj(const std::string &text, FileValueKind kind, ObObj &obj)
{
  int ret = common::OB_SUCCESS;
  if (FV_NULL == kind) {
    obj.set_null();
  } else if (FV_BOOL == kind) {
    obj.set_bool(ci_equal(text, "true"));
  } else if (FV_INT == kind) {
    obj.set_int(std::strtoll(text.c_str(), nullptr, 10));
  } else if (FV_DOUBLE == kind) {
    obj.set_double(std::strtod(text.c_str(), nullptr));
  } else if (FV_DATE == kind) {
    int y = 0, m = 0, d = 0;
    if (!parse_date_text(text, y, m, d)) {
      ret = common::OB_INVALID_DATE_VALUE;
    } else {
      obj.set_date(static_cast<int32_t>(days_from_civil(y, m, d)));
      obj.set_scale(0);
    }
  } else if (FV_DATETIME == kind) {
    int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (!parse_datetime_text(text, y, m, d, hh, mm, ss)) {
      ret = common::OB_INVALID_DATE_VALUE;
    } else {
      const int64_t usec = (days_from_civil(y, m, d) * 86400LL + hh * 3600LL + mm * 60LL + ss) * 1000000LL;
      obj.set_datetime(usec);
      obj.set_scale(0);
    }
  } else {
    // caller copies the string into the statement allocator
    obj.set_string(common::ObVarcharType, ObString(text.size(), text.data()));
    obj.set_collation_type(common::CS_TYPE_UTF8MB4_GENERAL_CI);
  }
  return ret;
}

int read_whole_file(const std::string &path, std::string &content)
{
  int ret = common::OB_SUCCESS;
  std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
  if (!input.is_open()) {
    ret = common::OB_FILE_NOT_EXIST;
  } else {
    input.seekg(0, std::ios::end);
    std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size > 0) {
      content.resize(static_cast<size_t>(size));
      input.read(&content[0], size);
      if (!input) { ret = common::OB_IO_ERROR; }
    }
  }
  return ret;
}

int parse_csv_records(const std::string &content, char delimiter, char quote,
                      std::vector<std::vector<std::string> > &records,
                      std::vector<std::vector<bool> > &quoted)
{
  int ret = common::OB_SUCCESS;
  std::vector<std::string> row;
  std::vector<bool> row_quoted;
  std::string field;
  bool field_quoted = false;
  bool in_quotes = false;
  bool at_field_start = true;
  size_t i = 0;
  if (content.size() >= 3 &&
      static_cast<unsigned char>(content[0]) == 0xEF &&
      static_cast<unsigned char>(content[1]) == 0xBB &&
      static_cast<unsigned char>(content[2]) == 0xBF) {
    i = 3;
  }
  for (; i < content.size() && common::OB_SUCCESS == ret; ++i) {
    const char c = content[i];
    if (in_quotes) {
      if (c == quote) {
        if (i + 1 < content.size() && content[i + 1] == quote) {
          field.push_back(quote);
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        field.push_back(c);
      }
    } else if (at_field_start && c == quote) {
      in_quotes = true;
      field_quoted = true;
      at_field_start = false;
    } else if (c == delimiter) {
      row.push_back(field);
      row_quoted.push_back(field_quoted);
      field.clear();
      field_quoted = false;
      at_field_start = true;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < content.size() && content[i + 1] == '\n') { ++i; }
      row.push_back(field);
      row_quoted.push_back(field_quoted);
      records.push_back(row);
      quoted.push_back(row_quoted);
      row.clear();
      row_quoted.clear();
      field.clear();
      field_quoted = false;
      at_field_start = true;
    } else {
      field.push_back(c);
      at_field_start = false;
    }
  }
  if (in_quotes) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (!field.empty() || !row.empty() || field_quoted) {
    row.push_back(field);
    row_quoted.push_back(field_quoted);
    records.push_back(row);
    quoted.push_back(row_quoted);
  }
  return ret;
}

std::string make_unique_name(const std::vector<std::string> &used, const std::string &base)
{
  std::string name = base.empty() ? "column" : base;
  if (std::find(used.begin(), used.end(), name) == used.end()) { return name; }
  for (int suffix = 2; ; ++suffix) {
    std::string candidate = name + "__" + std::to_string(suffix);
    if (std::find(used.begin(), used.end(), candidate) == used.end()) { return candidate; }
  }
}

int load_csv(const std::string &path, char delimiter, char quote, bool header,
             FileTableData &data)
{
  int ret = common::OB_SUCCESS;
  std::string content;
  std::vector<std::vector<std::string> > records;
  std::vector<std::vector<bool> > quoted;
  if (OB_FAIL(read_whole_file(path, content))) {
  } else if (OB_FAIL(parse_csv_records(content, delimiter, quote, records, quoted))) {
  } else if (records.empty()) {
    // zero-column, zero-row table is not representable; return one empty column
    FileColumn col;
    col.name_ = "column0";
    col.type_ = common::ObVarcharType;
    col.nullable_ = true;
    data.columns_.push_back(col);
  } else {
    const int64_t column_count = static_cast<int64_t>(records[0].size());
    size_t first_data = 0;
    std::vector<std::string> names;
    if (header) {
      first_data = 1;
      for (int64_t i = 0; i < column_count; ++i) {
        names.push_back(make_unique_name(names, records[0][i]));
      }
    } else {
      for (int64_t i = 0; i < column_count; ++i) {
        names.push_back(std::string("column") + std::to_string(i));
      }
    }
    std::vector<InferredType> inferred(column_count);
    for (size_t r = first_data; r < records.size(); ++r) {
      if (static_cast<int64_t>(records[r].size()) != column_count) {
        ret = common::OB_INVALID_ARGUMENT;
        break;
      }
      for (int64_t c = 0; c < column_count; ++c) {
        const std::string &text = records[r][c];
        const bool is_null = text.empty() && !quoted[r][c];
        if (is_null) {
          inferred[c].nullable_ = true;
        } else {
          merge_kind(inferred[c], classify_text(text));
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t c = 0; c < column_count; ++c) {
        FileColumn col;
        col.name_ = names[c];
        col.type_ = obj_type_of(FV_NULL == inferred[c].kind_ ? FV_STRING : inferred[c].kind_);
        col.nullable_ = inferred[c].nullable_;
        col.accuracy_ = 0;
        data.columns_.push_back(col);
      }
      for (size_t r = first_data; r < records.size(); ++r) {
        std::vector<FileCell> row;
        for (int64_t c = 0; c < column_count; ++c) {
          FileCell cell;
          const std::string &text = records[r][c];
          cell.null_ = text.empty() && !quoted[r][c];
          if (!cell.null_) {
            cell.text_ = text;
            cell.obj_.set_null();
          } else {
            cell.obj_.set_null();
          }
          row.push_back(cell);
        }
        data.rows_.push_back(row);
      }
    }
  }
  return ret;
}

// Minimal flat JSON object parser for JSONL: strings, numbers, booleans, null.
struct JsonCursor
{
  const std::string &s_;
  size_t p_;
  explicit JsonCursor(const std::string &s) : s_(s), p_(0) {}
  void skip_ws() { while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) { ++p_; } }
  bool consume(char c) { skip_ws(); if (p_ < s_.size() && s_[p_] == c) { ++p_; return true; } return false; }
  bool parse_string(std::string &out)
  {
    skip_ws();
    if (p_ >= s_.size() || s_[p_] != '"') { return false; }
    ++p_;
    while (p_ < s_.size()) {
      char c = s_[p_++];
      if (c == '"') { return true; }
      if (c == '\\') {
        if (p_ >= s_.size()) { return false; }
        char e = s_[p_++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (p_ + 4 > s_.size()) { return false; }
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              const char h = s_[p_++];
              code <<= 4;
              if (h >= '0' && h <= '9') { code |= h - '0'; }
              else if (h >= 'a' && h <= 'f') { code |= h - 'a' + 10; }
              else if (h >= 'A' && h <= 'F') { code |= h - 'A' + 10; }
              else { return false; }
            }
            // Encode BMP as UTF-8; surrogate pairs are uncommon in flat files.
            if (code < 0x80) { out.push_back(static_cast<char>(code)); }
            else if (code < 0x800) {
              out.push_back(static_cast<char>(0xC0 | (code >> 6)));
              out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
              out.push_back(static_cast<char>(0xE0 | (code >> 12)));
              out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
              out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            break;
          }
          default: return false;
        }
      } else {
        out.push_back(c);
      }
    }
    return false;
  }
  bool parse_scalar(std::string &text, FileValueKind &kind)
  {
    skip_ws();
    if (p_ >= s_.size()) { return false; }
    if (s_[p_] == '"') {
      kind = FV_STRING;
      return parse_string(text);
    }
    if (s_.compare(p_, 4, "true") == 0) { p_ += 4; kind = FV_BOOL; text = "true"; return true; }
    if (s_.compare(p_, 5, "false") == 0) { p_ += 5; kind = FV_BOOL; text = "false"; return true; }
    if (s_.compare(p_, 4, "null") == 0) { p_ += 4; kind = FV_NULL; text.clear(); return true; }
    const size_t begin = p_;
    if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+')) { ++p_; }
    while (p_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[p_])) ||
           s_[p_] == '.' || s_[p_] == 'e' || s_[p_] == 'E' || s_[p_] == '-' || s_[p_] == '+')) { ++p_; }
    if (p_ == begin) { return false; }
    text.assign(s_, begin, p_ - begin);
    kind = text.find_first_of(".eE") == std::string::npos ? FV_INT : FV_DOUBLE;
    return true;
  }
};

int load_jsonl(const std::string &path, FileTableData &data)
{
  int ret = common::OB_SUCCESS;
  std::string content;
  if (OB_FAIL(read_whole_file(path, content))) {
  } else {
    std::vector<std::string> names;
    std::vector<InferredType> inferred;
    std::vector<std::vector<FileCell> > raw_rows;
    size_t line_start = 0;
    while (line_start < content.size() && OB_SUCC(ret)) {
      size_t line_end = content.find('\n', line_start);
      if (std::string::npos == line_end) { line_end = content.size(); }
      std::string line = content.substr(line_start, line_end - line_start);
      line_start = line_end + 1;
      if (!line.empty() && line.back() == '\r') { line.pop_back(); }
      if (line.find_first_not_of(" \t\r\n") == std::string::npos) { continue; }
      JsonCursor cursor(line);
      std::vector<FileCell> row;
      if (!cursor.consume('{')) {
        ret = common::OB_ERR_INVALID_JSON_TEXT;
        break;
      }
      cursor.skip_ws();
      if (!cursor.consume('}')) {
        while (OB_SUCC(ret)) {
          std::string key;
          std::string text;
          FileValueKind kind = FV_NULL;
          if (!cursor.parse_string(key)) {
            ret = common::OB_ERR_INVALID_JSON_TEXT;
          } else if (!cursor.consume(':')) {
            ret = common::OB_ERR_INVALID_JSON_TEXT;
          } else {
            cursor.skip_ws();
            if (cursor.p_ < cursor.s_.size() &&
                (cursor.s_[cursor.p_] == '{' || cursor.s_[cursor.p_] == '[')) {
              ret = common::OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested JSON value");
            } else if (!cursor.parse_scalar(text, kind)) {
              ret = common::OB_ERR_INVALID_JSON_TEXT;
            }
          }
          if (OB_SUCC(ret)) {
            int64_t idx = -1;
            for (int64_t i = 0; i < static_cast<int64_t>(names.size()); ++i) {
              if (names[i] == key) { idx = i; break; }
            }
            if (idx < 0) {
              idx = static_cast<int64_t>(names.size());
              names.push_back(key);
              inferred.push_back(InferredType());
              for (auto &r : raw_rows) { r.push_back(FileCell()); }
            }
            FileCell cell;
            cell.null_ = FV_NULL == kind;
            if (!cell.null_) {
              cell.text_ = text;
              if (FV_STRING == kind) {
                merge_kind(inferred[idx], classify_text(text));
              } else {
                merge_kind(inferred[idx], kind);
              }
            } else {
              inferred[idx].nullable_ = true;
            }
            if (static_cast<int64_t>(row.size()) <= idx) { row.resize(idx + 1); }
            row[idx] = cell;
          }
          if (OB_SUCC(ret) && cursor.consume(',')) {
            continue;
          }
          break;
        }
        if (OB_SUCC(ret) && !cursor.consume('}')) { ret = common::OB_ERR_INVALID_JSON_TEXT; }
      }
      if (OB_SUCC(ret)) {
        if (static_cast<int64_t>(row.size()) < static_cast<int64_t>(names.size())) {
          row.resize(names.size());
        }
        raw_rows.push_back(row);
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t c = 0; c < static_cast<int64_t>(names.size()); ++c) {
        FileColumn col;
        col.name_ = make_unique_name(std::vector<std::string>(), names[c]);
        col.type_ = obj_type_of(FV_NULL == inferred[c].kind_ ? FV_STRING : inferred[c].kind_);
        col.nullable_ = inferred[c].nullable_;
        col.accuracy_ = 0;
        data.columns_.push_back(col);
      }
      for (auto &raw : raw_rows) {
        std::vector<FileCell> row;
        for (int64_t c = 0; c < static_cast<int64_t>(names.size()); ++c) {
          FileCell cell = raw[c];
          if (!cell.null_ && FV_STRING == inferred[c].kind_) {
            // Keep raw text; conversion happens against the final type below.
          }
          row.push_back(cell);
        }
        data.rows_.push_back(row);
      }
    }
  }
  return ret;
}


// ---------------------------------------------------------------------------
// Minimal Parquet reader for flat schemas.  It implements the Thrift compact
// protocol metadata, PLAIN / dictionary pages, definition levels and Snappy
// block decompression, which covers the file shapes used by the contest.
// ---------------------------------------------------------------------------
struct PqSchemaLeaf
{
  std::string name_;
  int32_t physical_type_;
  int32_t converted_type_;
  int32_t type_length_;
  int32_t repetition_type_;
  int32_t max_def_level_;
  int32_t max_rep_level_;
  PqSchemaLeaf() : physical_type_(0), converted_type_(-1), type_length_(0),
                   repetition_type_(0), max_def_level_(0), max_rep_level_(0) {}
};

struct PqColumnMeta
{
  int32_t physical_type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  PqColumnMeta() : physical_type_(0), codec_(0), num_values_(0),
                   data_page_offset_(0), dictionary_page_offset_(-1) {}
};

struct PqRowGroup
{
  int64_t num_rows_;
  std::vector<PqColumnMeta> columns_;
  PqRowGroup() : num_rows_(0) {}
};

struct PqMeta
{
  std::vector<PqSchemaLeaf> leaves_;
  std::vector<PqRowGroup> row_groups_;
  int64_t num_rows_;
  PqMeta() : num_rows_(0) {}
};

class PqCursor
{
public:
  PqCursor(const char *data, int64_t size) : data_(data), size_(size), pos_(0) {}
  bool ok() const { return pos_ <= size_; }
  int64_t pos() const { return pos_; }
  void seek(int64_t pos) { pos_ = pos; }
  bool need(int64_t n) const { return n >= 0 && pos_ + n <= size_; }
  bool read_byte(uint8_t &v)
  {
    if (!need(1)) { return false; }
    v = static_cast<uint8_t>(data_[pos_++]);
    return true;
  }
  bool read_varint(uint64_t &v)
  {
    v = 0;
    int shift = 0;
    while (need(1) && shift < 70) {
      uint8_t b = static_cast<uint8_t>(data_[pos_++]);
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) { return true; }
      shift += 7;
    }
    return false;
  }
  bool read_zigzag(int64_t &v)
  {
    uint64_t u = 0;
    if (!read_varint(u)) { return false; }
    v = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
    return true;
  }
  bool read_i32(int32_t &v) { int64_t t = 0; if (!read_zigzag(t)) return false; v = static_cast<int32_t>(t); return true; }
  bool read_i64(int64_t &v) { return read_zigzag(v); }
  bool read_binary(std::string &v)
  {
    uint64_t n = 0;
    if (!read_varint(n) || n > static_cast<uint64_t>(size_ - pos_)) { return false; }
    v.assign(data_ + pos_, static_cast<size_t>(n));
    pos_ += n;
    return true;
  }
  bool read_bytes(std::string &v, int64_t n)
  {
    if (n < 0 || !need(n)) { return false; }
    v.assign(data_ + pos_, static_cast<size_t>(n));
    pos_ += n;
    return true;
  }
  bool read_double(double &v)
  {
    if (!need(8)) { return false; }
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
      bits |= static_cast<uint64_t>(static_cast<uint8_t>(data_[pos_ + i])) << (8 * i);
    }
    memcpy(&v, &bits, sizeof(v));
    pos_ += 8;
    return true;
  }
  bool skip(uint8_t type, int depth = 0);
  const char *data_;
  int64_t size_;
  int64_t pos_;
};

bool PqCursor::skip(uint8_t type, int depth)
{
  if (depth > 32) { return false; }
  switch (type & 0x0F) {
    case 1: case 2: return true; // bool true/false
    case 3: { if (!need(1)) return false; ++pos_; return true; }
    case 4: case 5: case 6: { int64_t v = 0; return read_zigzag(v); }
    case 7: { double d = 0; return read_double(d); }
    case 8: { std::string s; return read_binary(s); }
    case 9: case 10: {
      uint8_t h = 0;
      if (!read_byte(h)) return false;
      uint64_t n = h >> 4;
      uint8_t et = h & 0x0F;
      if (n == 15 && !read_varint(n)) return false;
      for (uint64_t i = 0; i < n; ++i) { if (!skip(et, depth + 1)) return false; }
      return true;
    }
    case 11: {
      uint64_t n = 0;
      if (!read_varint(n)) return false;
      if (n > 0) {
        uint8_t kv = 0;
        if (!read_byte(kv)) return false;
        for (uint64_t i = 0; i < n; ++i) {
          if (!skip(kv >> 4, depth + 1) || !skip(kv & 0x0F, depth + 1)) return false;
        }
      }
      return true;
    }
    case 12: {
      int16_t last = 0;
      while (true) {
        uint8_t h = 0;
        if (!read_byte(h)) return false;
        if (h == 0) return true;
        int16_t id = last;
        uint8_t ft = h & 0x0F;
        int16_t delta = (h >> 4) & 0x0F;
        if (delta == 0) {
          int64_t fid = 0;
          if (!read_zigzag(fid)) return false;
          id = static_cast<int16_t>(fid);
        } else { id = last + delta; }
        last = id;
        if (!skip(ft, depth + 1)) return false;
      }
    }
    default: return false;
  }
}

struct PqField
{
  int16_t id_;
  uint8_t type_;
};

bool pq_read_field(PqCursor &c, int16_t &last, PqField &f)
{
  uint8_t h = 0;
  if (!c.read_byte(h)) { return false; }
  if (h == 0) { f.id_ = 0; f.type_ = 0; return true; }
  f.type_ = h & 0x0F;
  const int16_t delta = (h >> 4) & 0x0F;
  if (0 == delta) {
    int64_t id = 0;
    if (!c.read_zigzag(id)) { return false; }
    f.id_ = static_cast<int16_t>(id);
  } else {
    f.id_ = last + delta;
  }
  last = f.id_;
  return true;
}

bool pq_read_list(PqCursor &c, uint8_t &elem_type, uint64_t &count)
{
  uint8_t h = 0;
  if (!c.read_byte(h)) { return false; }
  elem_type = h & 0x0F;
  count = h >> 4;
  if (count == 15 && !c.read_varint(count)) { return false; }
  return true;
}

bool pq_read_schema_element(PqCursor &c, int32_t &type, int32_t &type_length,
                            int32_t &repetition, std::string &name,
                            int32_t &num_children, int32_t &converted)
{
  int16_t last = 0;
  type = -1;
  type_length = 0;
  repetition = 0;
  num_children = 0;
  converted = -1;
  while (true) {
    PqField f;
    if (!pq_read_field(c, last, f)) { return false; }
    if (0 == f.type_) { return true; }
    switch (f.id_) {
      case 1: if (!c.read_i32(type)) return false; break;
      case 2: if (!c.read_i32(type_length)) return false; break;
      case 3: if (!c.read_i32(repetition)) return false; break;
      case 4: if (!c.read_binary(name)) return false; break;
      case 5: if (!c.read_i32(num_children)) return false; break;
      case 6: if (!c.read_i32(converted)) return false; break;
      default: if (!c.skip(f.type_)) return false; break;
    }
  }
}

bool pq_read_column_meta(PqCursor &c, PqColumnMeta &m)
{
  int16_t last = 0;
  while (true) {
    PqField f;
    if (!pq_read_field(c, last, f)) { return false; }
    if (0 == f.type_) { return true; }
    switch (f.id_) {
      case 1: if (!c.read_i32(m.physical_type_)) return false; break;
      case 4: if (!c.read_i32(m.codec_)) return false; break;
      case 5: if (!c.read_i64(m.num_values_)) return false; break;
      case 9: if (!c.read_i64(m.data_page_offset_)) return false; break;
      case 11: if (!c.read_i64(m.dictionary_page_offset_)) return false; break;
      default: if (!c.skip(f.type_)) return false; break;
    }
  }
}

bool pq_read_row_group(PqCursor &c, PqRowGroup &rg)
{
  int16_t last = 0;
  while (true) {
    PqField f;
    if (!pq_read_field(c, last, f)) { return false; }
    if (0 == f.type_) { return true; }
    if (f.id_ == 1) {
      uint8_t et = 0; uint64_t n = 0;
      if (!pq_read_list(c, et, n)) return false;
      for (uint64_t i = 0; i < n; ++i) {
        int16_t clast = 0;
        PqColumnMeta m;
        while (true) {
          PqField cf;
          if (!pq_read_field(c, clast, cf)) return false;
          if (0 == cf.type_) break;
          if (cf.id_ == 3) {
            if (!pq_read_column_meta(c, m)) return false;
          } else if (cf.id_ == 2) {
            int64_t off = 0;
            if (!c.read_i64(off)) return false;
          } else if (!c.skip(cf.type_)) return false;
        }
        rg.columns_.push_back(m);
      }
    } else if (f.id_ == 3) {
      if (!c.read_i64(rg.num_rows_)) return false;
    } else if (!c.skip(f.type_)) {
      return false;
    }
  }
}

bool pq_read_file_meta(const std::string &content, PqMeta &meta)
{
  if (content.size() < 12 || content.compare(content.size() - 4, 4, "PAR1") != 0) { return false; }
  const uint8_t *p = reinterpret_cast<const uint8_t *>(content.data() + content.size() - 8);
  const uint32_t meta_len = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                            (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  if (meta_len > content.size() - 8) { return false; }
  const int64_t meta_start = static_cast<int64_t>(content.size()) - 8 - meta_len;
  PqCursor c(content.data() + meta_start, meta_len);
  int16_t last = 0;
  std::vector< std::pair<int32_t, int32_t> > schema_stack; // (children left, depth)
  while (true) {
    PqField f;
    if (!pq_read_field(c, last, f)) { return false; }
    if (0 == f.type_) { break; }
    if (f.id_ == 2) {
      uint8_t et = 0; uint64_t n = 0;
      if (!pq_read_list(c, et, n)) return false;
      // Flat schema: root followed by n leaves/groups.  Track nesting to skip groups.
      for (uint64_t i = 0; i < n; ++i) {
        int32_t type = -1, type_length = 0, repetition = 0, num_children = 0, converted = -1;
        std::string name;
        if (!pq_read_schema_element(c, type, type_length, repetition, name, num_children, converted)) return false;
        if (i == 0) {
        } else if (num_children == 0) {
          PqSchemaLeaf leaf;
          leaf.name_ = name;
          leaf.physical_type_ = type;
          leaf.converted_type_ = converted;
          leaf.type_length_ = type_length;
          leaf.repetition_type_ = repetition;
          leaf.max_def_level_ = (repetition == 0) ? 0 : 1;
          leaf.max_rep_level_ = 0;
          meta.leaves_.push_back(leaf);
        } else {
          // Nested schemas are outside the contest scope; keep parsing metadata
          // but do not expose a nested column as a flat table column.
        }
      }
    } else if (f.id_ == 3) {
      if (!c.read_i64(meta.num_rows_)) return false;
    } else if (f.id_ == 4) {
      uint8_t et = 0; uint64_t n = 0;
      if (!pq_read_list(c, et, n)) return false;
      for (uint64_t i = 0; i < n; ++i) {
        PqRowGroup rg;
        if (!pq_read_row_group(c, rg)) return false;
        meta.row_groups_.push_back(rg);
      }
    } else if (!c.skip(f.type_)) {
      return false;
    }
  }
  return true;
}

bool pq_snappy_decompress(const char *data, int64_t size, std::string &out)
{
  PqCursor c(data, size);
  uint64_t expected = 0;
  if (!c.read_varint(expected)) { return false; }
  out.clear();
  out.reserve(static_cast<size_t>(expected));
  while (c.pos() < c.size_ && out.size() < expected) {
    uint8_t tag = 0;
    if (!c.read_byte(tag)) { return false; }
    const int type = tag & 3;
    uint64_t len = 0;
    if (0 == type) {
      len = (tag >> 2) + 1;
      if (len > 60) {
        const int extra = static_cast<int>(len - 60);
        uint64_t v = 0;
        for (int i = 0; i < extra; ++i) {
          uint8_t b = 0;
          if (!c.read_byte(b)) { return false; }
          v |= static_cast<uint64_t>(b) << (8 * i);
        }
        len = v + 1;
      }
      if (!c.need(static_cast<int64_t>(len))) { return false; }
      out.append(c.data_ + c.pos_, static_cast<size_t>(len));
      c.pos_ += len;
    } else {
      uint64_t offset = 0;
      if (1 == type) {
        if (!c.need(1)) { return false; }
        len = 4 + ((tag >> 2) & 7);
        offset = ((tag & 0xE0) << 3) | static_cast<uint8_t>(c.data_[c.pos_++]);
      } else if (2 == type) {
        if (!c.need(2)) { return false; }
        len = 1 + (tag >> 2);
        offset = static_cast<uint8_t>(c.data_[c.pos_]) |
                 (static_cast<uint64_t>(static_cast<uint8_t>(c.data_[c.pos_ + 1])) << 8);
        c.pos_ += 2;
      } else {
        if (!c.need(4)) { return false; }
        len = 1 + (tag >> 2);
        for (int i = 0; i < 4; ++i) {
          offset |= static_cast<uint64_t>(static_cast<uint8_t>(c.data_[c.pos_ + i])) << (8 * i);
        }
        c.pos_ += 4;
      }
      if (offset == 0 || offset > out.size()) { return false; }
      for (uint64_t i = 0; i < len; ++i) {
        out.push_back(out[out.size() - offset]);
      }
    }
  }
  return out.size() == expected;
}

bool pq_read_hybrid(const char *data, int64_t size, int bit_width, int64_t count,
                    std::vector<uint32_t> &values, int64_t &consumed)
{
  PqCursor c(data, size);
  values.clear();
  values.reserve(static_cast<size_t>(count));
  while (values.size() < static_cast<size_t>(count) && c.pos() < size) {
    uint64_t header = 0;
    if (!c.read_varint(header)) { return false; }
    if (header & 1) {
      const uint64_t groups = header >> 1;
      const int64_t bytes = static_cast<int64_t>(groups) * bit_width;
      if (!c.need(bytes)) { return false; }
      const uint8_t *p = reinterpret_cast<const uint8_t *>(c.data_ + c.pos_);
      const uint64_t total = groups * 8;
      for (uint64_t i = 0; i < total && values.size() < static_cast<size_t>(count); ++i) {
        uint32_t v = 0;
        for (int b = 0; b < bit_width; ++b) {
          const int64_t bit = static_cast<int64_t>(i) * bit_width + b;
          if (p[bit >> 3] & (1u << (bit & 7))) { v |= (1u << b); }
        }
        values.push_back(v);
      }
      c.pos_ += bytes;
    } else {
      const uint64_t run = header >> 1;
      uint32_t v = 0;
      const int bytes = (bit_width + 7) / 8;
      for (int i = 0; i < bytes; ++i) {
        uint8_t b = 0;
        if (!c.read_byte(b)) { return false; }
        v |= static_cast<uint32_t>(b) << (8 * i);
      }
      for (uint64_t i = 0; i < run && values.size() < static_cast<size_t>(count); ++i) {
        values.push_back(v);
      }
    }
  }
  consumed = c.pos_;
  return values.size() == static_cast<size_t>(count);
}

std::string pq_format_date(int32_t days)
{
  // Convert days since 1970-01-01 to YYYY-MM-DD.
  int64_t z = static_cast<int64_t>(days) + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", static_cast<long long>(y), m, d);
  return std::string(buf);
}

std::string pq_format_datetime(int64_t usec)
{
  const int64_t days = usec / 86400000000LL;
  int64_t rem = usec % 86400000000LL;
  if (rem < 0) { rem += 86400000000LL; }
  const int hh = static_cast<int>(rem / 3600000000LL); rem %= 3600000000LL;
  const int mm = static_cast<int>(rem / 60000000LL); rem %= 60000000LL;
  const int ss = static_cast<int>(rem / 1000000LL);
  char buf[48];
  snprintf(buf, sizeof(buf), "%s %02d:%02d:%02d", pq_format_date(static_cast<int32_t>(days)).c_str(), hh, mm, ss);
  return std::string(buf);
}

bool pq_read_page_header(PqCursor &c, int32_t &type, int32_t &uncompressed, int32_t &compressed,
                         int32_t &num_values, int32_t &encoding, int32_t &def_encoding,
                         int32_t &rep_encoding, int32_t &def_len, int32_t &rep_len,
                         bool &is_v2, bool &v2_compressed)
{
  int16_t last = 0;
  type = -1; uncompressed = 0; compressed = 0; num_values = 0; encoding = 0;
  def_encoding = 0; rep_encoding = 0; def_len = 0; rep_len = 0;
  is_v2 = false; v2_compressed = true;
  while (true) {
    PqField f;
    if (!pq_read_field(c, last, f)) { return false; }
    if (0 == f.type_) { return true; }
    if (f.id_ == 1) { if (!c.read_i32(type)) return false; }
    else if (f.id_ == 2) { if (!c.read_i32(uncompressed)) return false; }
    else if (f.id_ == 3) { if (!c.read_i32(compressed)) return false; }
    else if (f.id_ == 5) {
      int16_t last2 = 0;
      while (true) {
        PqField g;
        if (!pq_read_field(c, last2, g)) return false;
        if (0 == g.type_) break;
        if (g.id_ == 1) { if (!c.read_i32(num_values)) return false; }
        else if (g.id_ == 2) { if (!c.read_i32(encoding)) return false; }
        else if (g.id_ == 3) { if (!c.read_i32(def_encoding)) return false; }
        else if (g.id_ == 4) { if (!c.read_i32(rep_encoding)) return false; }
        else if (!c.skip(g.type_)) return false;
      }
    } else if (f.id_ == 7) {
      int16_t last2 = 0;
      while (true) {
        PqField g;
        if (!pq_read_field(c, last2, g)) return false;
        if (0 == g.type_) break;
        if (g.id_ == 1) { if (!c.read_i32(num_values)) return false; }
        else if (g.id_ == 2) { if (!c.read_i32(encoding)) return false; }
        else if (!c.skip(g.type_)) return false;
      }
    } else if (f.id_ == 8) {
      is_v2 = true;
      int16_t last2 = 0;
      while (true) {
        PqField g;
        if (!pq_read_field(c, last2, g)) return false;
        if (0 == g.type_) break;
        if (g.id_ == 1) { if (!c.read_i32(num_values)) return false; }
        else if (g.id_ == 4) { if (!c.read_i32(encoding)) return false; }
        else if (g.id_ == 5) { if (!c.read_i32(def_len)) return false; }
        else if (g.id_ == 6) { if (!c.read_i32(rep_len)) return false; }
        else if (g.id_ == 7) { v2_compressed = (1 == g.type_); }
        else if (!c.skip(g.type_)) return false;
      }
    } else if (!c.skip(f.type_)) {
      return false;
    }
  }
}

bool pq_decode_plain(const char *data, int64_t size, int32_t physical_type, int32_t type_length,
                     int64_t count, std::vector<std::string> &values)
{
  PqCursor c(data, size);
  values.clear();
  for (int64_t i = 0; i < count; ++i) {
    if (0 == physical_type) { // BOOLEAN bit-packed
      const int64_t byte = i / 8;
      if (!c.need(byte + 1)) return false;
      const bool v = (static_cast<uint8_t>(data[byte]) >> (i % 8)) & 1;
      values.push_back(v ? "true" : "false");
    } else if (1 == physical_type) { // INT32
      int32_t v = 0;
      if (!c.need(4)) return false;
      memcpy(&v, data + c.pos_, 4);
      c.pos_ += 4;
      values.push_back(std::to_string(v));
    } else if (2 == physical_type) { // INT64
      int64_t v = 0;
      if (!c.need(8)) return false;
      memcpy(&v, data + c.pos_, 8);
      c.pos_ += 8;
      values.push_back(std::to_string(v));
    } else if (3 == physical_type) { // FLOAT
      float v = 0;
      if (!c.need(4)) return false;
      memcpy(&v, data + c.pos_, 4);
      c.pos_ += 4;
      char buf[64]; snprintf(buf, sizeof(buf), "%.9g", v);
      values.push_back(buf);
    } else if (4 == physical_type) { // DOUBLE
      double v = 0;
      if (!c.need(8)) return false;
      memcpy(&v, data + c.pos_, 8);
      c.pos_ += 8;
      char buf[64]; snprintf(buf, sizeof(buf), "%.17g", v);
      values.push_back(buf);
    } else if (5 == physical_type || 6 == physical_type) { // BYTE_ARRAY / FIXED
      int64_t len = type_length;
      if (5 == physical_type) {
        int32_t l = 0;
        if (!c.need(4)) return false;
        memcpy(&l, data + c.pos_, 4);
        c.pos_ += 4;
        len = l;
      }
      if (len < 0 || !c.need(len)) return false;
      values.push_back(std::string(data + c.pos_, static_cast<size_t>(len)));
      c.pos_ += len;
    } else {
      return false;
    }
  }
  return true;
}

bool pq_read_chunk(const std::string &content, const PqColumnMeta &meta,
                   const PqSchemaLeaf &leaf, std::vector<std::string> &values,
                   std::vector<bool> &nulls)
{
  values.clear();
  nulls.clear();
  int64_t offset = meta.data_page_offset_;
  if (meta.dictionary_page_offset_ >= 0 && meta.dictionary_page_offset_ < offset) {
    offset = meta.dictionary_page_offset_;
  }
  if (offset < 0 || offset >= static_cast<int64_t>(content.size())) { return false; }
  std::vector<std::string> dictionary;
  int64_t produced = 0;
  while (produced < meta.num_values_ && offset < static_cast<int64_t>(content.size())) {
    PqCursor hc(content.data() + offset, content.size() - offset);
    int32_t page_type = -1, uncompressed = 0, compressed = 0, page_values = 0;
    int32_t encoding = 0, def_encoding = 0, rep_encoding = 0, def_len = 0, rep_len = 0;
    bool is_v2 = false, v2_compressed = true;
    if (!pq_read_page_header(hc, page_type, uncompressed, compressed, page_values,
                             encoding, def_encoding, rep_encoding, def_len, rep_len,
                             is_v2, v2_compressed)) {
      return false;
    }
    const int64_t header_size = hc.pos();
    if (compressed < 0 || offset + header_size + compressed > static_cast<int64_t>(content.size())) {
      return false;
    }
    const char *raw = content.data() + offset + header_size;
    std::string decompressed;
    const char *page_data = raw;
    int64_t page_size = compressed;
    if (1 == meta.codec_ && page_type != 3) {
      if (!pq_snappy_decompress(raw, compressed, decompressed)) { return false; }
      page_data = decompressed.data();
      page_size = static_cast<int64_t>(decompressed.size());
    }
    if (3 == page_type) {
      // V2: repetition/definition levels are stored uncompressed in front of
      // the (possibly compressed) value section.
      const int64_t levels_len = rep_len + def_len;
      if (levels_len < 0 || levels_len > compressed) { return false; }
      std::vector<uint32_t> defs;
      int64_t def_consumed = 0;
      if (leaf.max_def_level_ > 0 && def_len > 0) {
        if (!pq_read_hybrid(raw + rep_len, def_len, 1, page_values, defs, def_consumed)) {
          return false;
        }
      }
      int64_t non_null = page_values;
      for (uint32_t d : defs) {
        if (leaf.max_def_level_ > 0 && d < static_cast<uint32_t>(leaf.max_def_level_)) {
          --non_null;
        }
      }
      const char *data_raw = raw + levels_len;
      const int64_t data_compressed = compressed - levels_len;
      std::string v2_buf;
      const char *data_ptr = data_raw;
      int64_t data_size = data_compressed;
      if (1 == meta.codec_ && v2_compressed && data_compressed > 0) {
        if (!pq_snappy_decompress(data_raw, data_compressed, v2_buf)) {
          return false;
        }
        data_ptr = v2_buf.data();
        data_size = static_cast<int64_t>(v2_buf.size());
      }
      std::vector<std::string> page_vals;
      if (0 == encoding) {
        if (!pq_decode_plain(data_ptr, data_size, leaf.physical_type_, leaf.type_length_, non_null, page_vals)) {
          return false;
        }
      } else if (2 == encoding || 8 == encoding) {
        if (data_size < 1) { return false; }
        const int bit_width = static_cast<uint8_t>(data_ptr[0]);
        std::vector<uint32_t> idx;
        int64_t consumed = 0;
        if (!pq_read_hybrid(data_ptr + 1, data_size - 1, bit_width, non_null, idx, consumed)) {
          return false;
        }
        for (uint32_t i : idx) {
          if (i >= dictionary.size()) { return false; }
          page_vals.push_back(dictionary[i]);
        }
      } else {
        return false;
      }
      int64_t vi = 0;
      for (int64_t i = 0; i < page_values; ++i) {
        bool is_null = leaf.max_def_level_ > 0 &&
                       (defs.empty() || defs[static_cast<size_t>(i)] <
                        static_cast<uint32_t>(leaf.max_def_level_));
        nulls.push_back(is_null);
        if (is_null) {
          values.push_back(std::string());
        } else if (vi < static_cast<int64_t>(page_vals.size())) {
          values.push_back(page_vals[vi++]);
        } else {
          return false;
        }
      }
      produced += page_values;
      offset += header_size + compressed;
      continue;
    }
    if (2 == page_type) { // dictionary page
      std::vector<std::string> dict;
      if (!pq_decode_plain(page_data, page_size, leaf.physical_type_, leaf.type_length_,
                           page_values, dict)) {
        return false;
      }
      dictionary = dict;
    } else if (0 == page_type || 3 == page_type) {
      int64_t values_count = page_values;
      const char *p = page_data;
      int64_t remain = page_size;
      std::vector<uint32_t> def_levels;
      if (3 == page_type) {
        if (rep_len > 0) { p += rep_len; remain -= rep_len; }
        if (def_len > 0) {
          const int64_t bw = leaf.max_def_level_ > 0 ? 1 : 0;
          int64_t consumed = 0;
          if (!pq_read_hybrid(p, def_len, bw, values_count, def_levels, consumed)) {
            return false;
          }
          p += def_len; remain -= def_len;
        }
      } else {
        if (leaf.max_rep_level_ > 0) {
          if (remain < 4) return false;
          int32_t len = 0; memcpy(&len, p, 4); p += 4 + len; remain -= 4 + len;
        }
        if (leaf.max_def_level_ > 0) {
          if (remain < 4) return false;
          int32_t len = 0; memcpy(&len, p, 4); p += 4; remain -= 4;
          if (len < 0 || len > remain) return false;
          int64_t consumed = 0;
          if (!pq_read_hybrid(p, len, 1, values_count, def_levels, consumed)) {
            return false;
          }
          p += len; remain -= len;
        }
      }
      int64_t non_null = values_count;
      for (uint32_t d : def_levels) {
        if (leaf.max_def_level_ > 0 && d < static_cast<uint32_t>(leaf.max_def_level_)) {
          --non_null;
        }
      }
      std::vector<std::string> page_vals;
      if (0 == encoding) {
        if (!pq_decode_plain(p, remain, leaf.physical_type_, leaf.type_length_, non_null, page_vals)) {
          return false;
        }
      } else if (2 == encoding || 8 == encoding) {
        if (remain < 1) return false;
        const int bit_width = static_cast<uint8_t>(p[0]);
        ++p; --remain;
        std::vector<uint32_t> idx;
        int64_t consumed = 0;
        if (!pq_read_hybrid(p, remain, bit_width, non_null, idx, consumed)) {
          return false;
        }
        for (uint32_t i : idx) {
          if (i >= dictionary.size()) { return false; }
          page_vals.push_back(dictionary[i]);
        }
      } else {
        return false;
      }
      int64_t vi = 0;
      for (int64_t i = 0; i < values_count; ++i) {
        bool is_null = leaf.max_def_level_ > 0 &&
                       (def_levels.empty() || def_levels[static_cast<size_t>(i)] <
                        static_cast<uint32_t>(leaf.max_def_level_));
        nulls.push_back(is_null);
        if (is_null) {
          values.push_back(std::string());
        } else if (vi < static_cast<int64_t>(page_vals.size())) {
          values.push_back(page_vals[vi++]);
        } else {
          return false;
        }
      }
      produced += values_count;
    } else {
      // index page / unknown page type: skip
      produced = produced;
    }
    offset += header_size + compressed;
  }
  return produced >= meta.num_values_;
}

int load_parquet(const std::string &path, FileTableData &data)
{
  int ret = common::OB_SUCCESS;
  std::string content;
  PqMeta meta;
  if (OB_FAIL(read_whole_file(path, content))) {
  } else if (!pq_read_file_meta(content, meta)) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (meta.leaves_.empty() || meta.row_groups_.empty()) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    for (auto &leaf : meta.leaves_) {
      FileColumn col;
      col.name_ = leaf.name_;
      col.nullable_ = leaf.repetition_type_ != 0;
      col.accuracy_ = 0;
      switch (leaf.physical_type_) {
        case 0: col.type_ = common::ObTinyIntType; break;
        case 3: case 4: col.type_ = common::ObDoubleType; break;
        case 5: case 6: col.type_ = common::ObVarcharType; break;
        default: col.type_ = common::ObIntType; break;
      }
      if (6 == leaf.converted_type_) {
        col.type_ = common::ObDateType;
      } else if (9 == leaf.converted_type_ || 10 == leaf.converted_type_) {
        col.type_ = common::ObDateTimeType;
      } else if (0 == leaf.converted_type_) {
        col.type_ = common::ObVarcharType;
      }
      data.columns_.push_back(col);
    }
    std::vector< std::vector<std::string> > group_values(data.columns_.size());
    std::vector< std::vector<bool> > group_nulls(data.columns_.size());
    for (auto &rg : meta.row_groups_) {
      if (static_cast<int64_t>(rg.columns_.size()) != static_cast<int64_t>(data.columns_.size())) {
        ret = common::OB_INVALID_ARGUMENT;
        break;
      }
      for (int64_t c = 0; OB_SUCC(ret) && c < static_cast<int64_t>(data.columns_.size()); ++c) {
        group_values[c].clear();
        group_nulls[c].clear();
        if (!pq_read_chunk(content, rg.columns_[c], meta.leaves_[c], group_values[c], group_nulls[c])) {
          ret = common::OB_INVALID_ARGUMENT;
          break;
        }
      }
      if (OB_SUCC(ret)) {
        const int64_t rows = std::min(rg.num_rows_, static_cast<int64_t>(group_values[0].size()));
        for (int64_t r = 0; r < rows; ++r) {
          std::vector<FileCell> row;
          for (int64_t c = 0; c < static_cast<int64_t>(data.columns_.size()); ++c) {
            FileCell cell;
            cell.null_ = (r < static_cast<int64_t>(group_nulls[c].size()) && group_nulls[c][r]) ||
                         (r >= static_cast<int64_t>(group_values[c].size()));
            if (!cell.null_) {
              cell.text_ = group_values[c][r];
            }
            row.push_back(cell);
          }
          data.rows_.push_back(row);
        }
      }
    }
    // Normalize Parquet physical representations to the declared logical type.
    for (int64_t c = 0; OB_SUCC(ret) && c < static_cast<int64_t>(data.columns_.size()); ++c) {
      const PqSchemaLeaf &leaf = meta.leaves_[c];
      if (6 == leaf.converted_type_) {
        for (auto &row : data.rows_) {
          if (!row[c].null_) {
            row[c].text_ = pq_format_date(std::atoi(row[c].text_.c_str()));
          }
        }
      } else if (9 == leaf.converted_type_ || 10 == leaf.converted_type_) {
        const int64_t multiplier = (9 == leaf.converted_type_) ? 1000 : 1;
        for (auto &row : data.rows_) {
          if (!row[c].null_) {
            row[c].text_ = pq_format_datetime(std::atoll(row[c].text_.c_str()) * multiplier);
          }
        }
      }
    }
  }
  return ret;
}

int load_glob(const std::string &pattern, FileTableData &data)
{
  int ret = common::OB_SUCCESS;
  glob_t matches;
  MEMSET(&matches, 0, sizeof(matches));
  const int rc = ::glob(pattern.c_str(), GLOB_NOSORT, nullptr, &matches);
  if (0 != rc && GLOB_NOMATCH != rc) {
    ret = common::OB_IO_ERROR;
  } else {
    FileColumn col;
    col.name_ = "file";
    col.type_ = common::ObVarcharType;
    col.nullable_ = false;
    col.accuracy_ = 65535;
    data.columns_.push_back(col);
    for (size_t i = 0; i < matches.gl_pathc; ++i) {
      FileCell cell;
      cell.null_ = false;
      cell.text_ = matches.gl_pathv[i];
      std::vector<FileCell> row;
      row.push_back(cell);
      data.rows_.push_back(row);
    }
  }
  ::globfree(&matches);
  return ret;
}

void collect_expr_nodes(const ParseNode *node, std::vector<const ParseNode *> &out)
{
  if (nullptr == node) { return; }
  if (T_EXPR_LIST == node->type_ && nullptr != node->children_) {
    for (int64_t i = 0; i < node->num_child_; ++i) {
      collect_expr_nodes(node->children_[i], out);
    }
  } else {
    out.push_back(node);
  }
}

const char *node_string(const ParseNode *node, int64_t &len)
{
  len = 0;
  if (nullptr == node) { return nullptr; }
  if (T_VARCHAR == node->type_ || T_IDENT == node->type_ ||
      T_HEX_STRING == node->type_) {
    len = node->str_len_;
    return node->str_value_;
  }
  return nullptr;
}

int option_name(const ParseNode *node, std::string &name)
{
  int ret = common::OB_SUCCESS;
  const ParseNode *name_node = nullptr;
  if (nullptr == node) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (T_OP_EQ == node->type_ || T_OP_ASSIGN == node->type_) {
    name_node = node->children_[0];
  } else {
    ret = common::OB_INVALID_ARGUMENT;
  }
  if (OB_SUCC(ret)) {
    if (nullptr == name_node) {
      ret = common::OB_INVALID_ARGUMENT;
    } else if (T_COLUMN_REF == name_node->type_ && nullptr != name_node->children_ &&
               name_node->num_child_ >= 3 && nullptr != name_node->children_[2]) {
      const ParseNode *col = name_node->children_[2];
      name.assign(col->str_value_, col->str_len_);
    } else {
      int64_t len = 0;
      const char *str = node_string(name_node, len);
      if (nullptr == str) { ret = common::OB_INVALID_ARGUMENT; }
      else { name.assign(str, len); }
    }
  }
  return ret;
}

const common::ObObjParam *questionmark_param(
    const ParseNode *node, const common::ParamStore *param_list)
{
  const common::ObObjParam *param = nullptr;
  if (nullptr != node && T_QUESTIONMARK == node->type_ && nullptr != param_list &&
      node->value_ >= 0 && node->value_ < param_list->count()) {
    param = &param_list->at(node->value_);
  }
  return param;
}

int literal_bool(const ParseNode *node, const common::ParamStore *param_list, bool &value)
{
  int ret = common::OB_SUCCESS;
  const common::ObObjParam *param = questionmark_param(node, param_list);
  if (nullptr == node) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (nullptr != param) {
    if (param->is_integer_type()) {
      value = 0 != param->get_int();
    } else if (param->is_string_type()) {
      ObString str;
      OZ (param->get_string(str));
      value = ci_equal(std::string(str.ptr(), str.length()), "true") ||
              std::string(str.ptr(), str.length()) == "1";
    } else {
      ret = common::OB_INVALID_ARGUMENT;
    }
  } else if (T_INT == node->type_) {
    value = 0 != node->value_;
  } else if (T_NULL == node->type_) {
    value = false;
  } else {
    int64_t len = 0;
    const char *str = node_string(node, len);
    if (nullptr == str) { ret = common::OB_INVALID_ARGUMENT; }
    else { value = ci_equal(std::string(str, len), "true") || std::string(str, len) == "1"; }
  }
  return ret;
}

int literal_int(const ParseNode *node, const common::ParamStore *param_list, int64_t &value)
{
  int ret = common::OB_SUCCESS;
  const common::ObObjParam *param = questionmark_param(node, param_list);
  if (nullptr == node) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (nullptr != param) {
    if (param->is_integer_type() || param->is_numeric_type()) {
      value = param->get_int();
    } else {
      ret = common::OB_INVALID_ARGUMENT;
    }
  } else if (T_INT == node->type_) {
    value = node->value_;
  } else {
    int64_t len = 0;
    const char *str = node_string(node, len);
    if (nullptr == str) { ret = common::OB_INVALID_ARGUMENT; }
    else { value = std::strtoll(std::string(str, len).c_str(), nullptr, 10); }
  }
  return ret;
}

int literal_string(const ParseNode *node, const common::ParamStore *param_list, std::string &value)
{
  int ret = common::OB_SUCCESS;
  const common::ObObjParam *param = questionmark_param(node, param_list);
  if (nullptr != param) {
    if (param->is_string_type()) {
      ObString str;
      OZ (param->get_string(str));
      value.assign(str.ptr(), str.length());
    } else {
      ret = common::OB_INVALID_ARGUMENT;
    }
  } else {
    int64_t len = 0;
    const char *str = node_string(node, len);
    if (nullptr == str) { ret = common::OB_INVALID_ARGUMENT; }
    else { value.assign(str, len); }
  }
  return ret;
}
} // namespace

int ObDMLResolver::resolve_file_table_item(const ParseNode &parse_tree, TableItem *&table_item)
{
  int ret = common::OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  TableItem *item = nullptr;
  ObValuesTableDef *table_def = nullptr;
  const ParseNode *name_node = nullptr;
  const ParseNode *args_node = nullptr;
  const ParseNode *alias_node = nullptr;

  if (OB_ISNULL(stmt) || OB_ISNULL(allocator_) || OB_ISNULL(session_info_) ||
      OB_ISNULL(params_.expr_factory_)) {
    ret = common::OB_NOT_INIT;
  } else if (T_FILE_TABLE != parse_tree.type_ || 3 != parse_tree.num_child_) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(name_node = parse_tree.children_[0]) ||
             OB_ISNULL(args_node = parse_tree.children_[1])) {
    ret = common::OB_ERR_UNEXPECTED;
  } else {
    bool enabled = false;
    if (OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    } else if (!enabled) {
      ret = common::OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql is disabled, set enable_file_sql = ON");
    }
  }

  std::string function_name;
  std::string path;
  std::string format;
  char delimiter = ',';
  char quote = '"';
  bool header = true;
  int64_t sample_size = -1;
  if (OB_SUCC(ret)) {
    function_name.assign(name_node->str_value_, name_node->str_len_);
    std::vector<const ParseNode *> args;
    collect_expr_nodes(args_node, args);
    if (args.empty()) {
      ret = common::OB_ERR_PARAM_SIZE;
      LOG_USER_ERROR(OB_ERR_PARAM_SIZE, static_cast<int>(strlen("file table function")), "file table function");
    } else if (OB_FAIL(literal_string(args[0], params_.param_list_, path))) {
      LOG_ERROR("file table path argument is not a string", "argc", args.size(),
               "type", args[0]->type_, "num_child", args[0]->num_child_,
               "str_len", args[0]->str_len_);
      ret = common::OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path must be a string literal");
    } else {
      for (size_t i = 1; OB_SUCC(ret) && i < args.size(); ++i) {
        std::string opt_name;
        const ParseNode *value_node = nullptr;
        if (OB_FAIL(option_name(args[i], opt_name))) {
          ret = common::OB_SUCCESS;
          if (1 == i) {
            std::string positional;
            if (OB_FAIL(literal_string(args[i], params_.param_list_, positional))) {
              ret = common::OB_ERR_PARSE_SQL;
            } else if (!ci_equal(positional, "csv")) {
              ret = common::OB_ERR_PARSE_SQL;
            }
          }
        } else if (args[i]->num_child_ < 2 || OB_ISNULL(value_node = args[i]->children_[1])) {
          ret = common::OB_INVALID_ARGUMENT;
        } else if (ci_equal(opt_name, "header")) {
          OZ (literal_bool(value_node, params_.param_list_, header));
        } else if (ci_equal(opt_name, "sample_size")) {
          OZ (literal_int(value_node, params_.param_list_, sample_size));
        } else if (ci_equal(opt_name, "format")) {
          OZ (literal_string(value_node, params_.param_list_, format));
        } else if (ci_equal(opt_name, "delimiter") || ci_equal(opt_name, "sep")) {
          std::string s;
          OZ (literal_string(value_node, params_.param_list_, s));
          if (OB_SUCC(ret) && !s.empty()) { delimiter = s[0]; }
        } else if (ci_equal(opt_name, "quote")) {
          std::string s;
          OZ (literal_string(value_node, params_.param_list_, s));
          if (OB_SUCC(ret) && !s.empty()) { quote = s[0]; }
        }
      }
      if (OB_SUCC(ret) && sample_size != -1) {
        ret = common::OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size other than -1");
      }
    }
  }

  if (OB_SUCC(ret)) {
    common::ObString secure_priv;
    common::ObString path_str(static_cast<int32_t>(path.size()), path.data());
    if (OB_FAIL(session_info_->get_secure_file_priv(secure_priv))) {
    } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure_priv, path_str))) {
      LOG_WARN("file sql path is outside secure_file_priv", K(ret), K(path_str), K(secure_priv));
    }
  }

  FileTableData data;
  if (OB_SUCC(ret)) {
    if (ci_equal(function_name, "read_csv")) {
      OZ (load_csv(path, delimiter, quote, header, data));
    } else if (ci_equal(function_name, "read_json") || ci_equal(function_name, "read_ndjson")) {
      OZ (load_jsonl(path, data));
    } else if (ci_equal(function_name, "read_parquet")) {
      OZ (load_parquet(path, data));
    } else if (ci_equal(function_name, "glob")) {
      OZ (load_glob(path, data));
    } else {
      ret = common::OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file table function");
    }
  }
  if (OB_SUCC(ret)) {
    ObString alias_name;
    if (nullptr != alias_node) {
      alias_name.assign_ptr(alias_node->str_value_, alias_node->str_len_);
    } else {
      std::string generated = function_name + "_file";
      char *buf = static_cast<char *>(allocator_->alloc(generated.size()));
      if (OB_ISNULL(buf)) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, generated.data(), generated.size());
        alias_name.assign_ptr(buf, static_cast<int32_t>(generated.size()));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(item = stmt->create_table_item(*allocator_))) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        item->table_id_ = generate_table_id();
        item->table_name_ = alias_name;
        item->alias_name_ = alias_name;
        item->type_ = TableItem::VALUES_TABLE;
        item->is_view_table_ = false;
        item->is_system_table_ = false;
      }
    }
    if (OB_SUCC(ret)) {
      void *buf = allocator_->alloc(sizeof(ObValuesTableDef));
      if (OB_ISNULL(buf)) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        table_def = new (buf) ObValuesTableDef();
        item->values_table_def_ = table_def;
        table_def->column_cnt_ = static_cast<int64_t>(data.columns_.size());
        table_def->row_cnt_ = static_cast<int64_t>(data.rows_.size());
        table_def->access_type_ = ObValuesTableDef::ACCESS_EXPR;
        table_def->is_const_ = true;
      }
    }
    if (OB_SUCC(ret)) {
      for (auto &col : data.columns_) {
        ObRawExprResType res_type;
        res_type.set_type(col.type_);
        if (common::ObVarcharType == col.type_) {
          res_type.set_collation_type(common::CS_TYPE_UTF8MB4_GENERAL_CI);
          res_type.set_accuracy(col.accuracy_);
        } else if (!col.nullable_) {
          res_type.set_result_flag(NOT_NULL_FLAG);
        }
        if (OB_FAIL(table_def->column_types_.push_back(res_type))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (auto &row : data.rows_) {
        for (int64_t c = 0; OB_SUCC(ret) && c < static_cast<int64_t>(data.columns_.size()); ++c) {
          const FileColumn &col = data.columns_[c];
          const FileCell &cell = row[c];
          ObConstRawExpr *const_expr = nullptr;
          if (cell.null_) {
            ObRawExpr *null_expr = nullptr;
            OZ (ObRawExprUtils::build_null_expr(*params_.expr_factory_, null_expr));
            const_expr = static_cast<ObConstRawExpr *>(null_expr);
          } else if (common::ObTinyIntType == col.type_) {
            OZ (ObRawExprUtils::build_const_int_expr(*params_.expr_factory_, common::ObTinyIntType,
                ci_equal(cell.text_, "true") ? 1 : 0, const_expr));
          } else if (common::ObIntType == col.type_) {
            OZ (ObRawExprUtils::build_const_int_expr(*params_.expr_factory_, common::ObIntType,
                std::strtoll(cell.text_.c_str(), nullptr, 10), const_expr));
          } else if (common::ObDoubleType == col.type_) {
            OZ (ObRawExprUtils::build_const_double_expr(*params_.expr_factory_, common::ObDoubleType,
                std::strtod(cell.text_.c_str(), nullptr), const_expr));
          } else if (common::ObDateType == col.type_) {
            int y = 0, m = 0, d = 0;
            if (!parse_date_text(cell.text_, y, m, d)) {
              ret = common::OB_INVALID_DATE_VALUE;
            } else {
              OZ (ObRawExprUtils::build_const_date_expr(*params_.expr_factory_,
                  days_from_civil(y, m, d), const_expr));
            }
          } else if (common::ObDateTimeType == col.type_) {
            int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
            if (!parse_datetime_text(cell.text_, y, m, d, hh, mm, ss)) {
              ret = common::OB_INVALID_DATE_VALUE;
            } else {
              const int64_t usec = (days_from_civil(y, m, d) * 86400LL + hh * 3600LL + mm * 60LL + ss) * 1000000LL;
              OZ (ObRawExprUtils::build_const_datetime_expr(*params_.expr_factory_, usec, const_expr));
            }
          } else {
            ObString copied;
            OZ (ob_write_string(*allocator_, ObString(static_cast<int32_t>(cell.text_.size()), cell.text_.data()), copied));
            OZ (ObRawExprUtils::build_const_string_expr(*params_.expr_factory_, common::ObVarcharType,
                copied, common::CS_TYPE_UTF8MB4_GENERAL_CI, const_expr));
          }
          if (OB_SUCC(ret) && nullptr != const_expr) {
            OZ (const_expr->formalize(session_info_));
            OZ (table_def->access_exprs_.push_back(const_expr));
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      OZ (estimate_values_table_stats(*table_def));
    }
    if (OB_SUCC(ret)) {
      OZ (try_add_cast_to_values(table_def->column_types_, table_def->access_exprs_));
    }
    if (OB_SUCC(ret)) {
      OZ (stmt->add_table_item(session_info_, item));
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(data.columns_.size()); ++i) {
        const FileColumn &col = data.columns_[i];
        ObString column_name;
        OZ (ob_write_string(*allocator_, ObString(static_cast<int32_t>(col.name_.size()), col.name_.data()), column_name));
        ObColumnRefRawExpr *column_expr = nullptr;
        OZ (params_.expr_factory_->create_raw_expr(T_REF_COLUMN, column_expr));
        if (OB_SUCC(ret)) {
          ObRawExprResType res_type;
          res_type = table_def->column_types_.at(i);
          if (table_def->access_exprs_.count() > i) {
            const ObRawExprResType &expr_type = table_def->access_exprs_.at(i)->get_result_type();
            if (!expr_type.is_null()) {
              res_type = expr_type;
            }
          }
          res_type.unset_result_flag(NOT_NULL_FLAG);
          if (!col.nullable_) {
            res_type.set_result_flag(NOT_NULL_FLAG);
          }
          column_expr->set_result_type(res_type);
          column_expr->set_ref_id(item->table_id_, OB_APP_MIN_COLUMN_ID + i);
          column_expr->set_column_attr(item->table_name_, column_name);
          OZ (column_expr->add_flag(IS_COLUMN));
          ColumnItem column_item;
          column_item.expr_ = column_expr;
          column_item.table_id_ = column_expr->get_table_id();
          column_item.column_id_ = column_expr->get_column_id();
          column_item.column_name_ = column_expr->get_column_name();
          OZ (stmt->add_column_item(column_item));
        }
      }
    }
    if (OB_SUCC(ret)) {
      table_item = item;
      if (nullptr != stmt->get_query_ctx()) {
        stmt->get_query_ctx()->get_query_hint_for_update().get_global_hint().merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
