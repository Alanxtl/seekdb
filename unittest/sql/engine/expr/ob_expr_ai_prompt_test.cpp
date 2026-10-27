/* Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

class AIPromptTest : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
  int expand(const char *text, ObString &output)
  {
    ObJsonObject *object = nullptr;
    ObString input(text);
    int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, object);
    if (ret == OB_SUCCESS) {
      ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, object, output);
    }
    return ret;
  }
};

TEST_F(AIPromptTest, repeated_large_arguments)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, expand(R"({"template":"{0}{0}{0}{0}","args":["abcdefghijklmnop"]})", result));
  EXPECT_EQ(ObString("abcdefghijklmnopabcdefghijklmnopabcdefghijklmnopabcdefghijklmnop"), result);
}

TEST_F(AIPromptTest, composites_and_nested_prompt)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, expand(R"({"template":"{0}/{1}/{2}","args":[[1,2],{"a":1},{"template":"你好{0}","args":["世界"]}]})", result));
  EXPECT_EQ(ObString("[1, 2]/{\"a\": 1}/你好世界"), result);
}

TEST_F(AIPromptTest, preserves_escapes_and_embedded_nul)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, expand(R"({"template":"{0}:{0}","args":["x\u0000y"]})", result));
  EXPECT_EQ(7, result.length());
  EXPECT_EQ(0, MEMCMP("x\0y:x\0y", result.ptr(), 7));
}

TEST_F(AIPromptTest, invalid_indices_do_not_overflow_or_read_outside_arguments)
{
  for (const char *text : {
      R"({"template":"{1}","args":["x"]})",
      R"({"template":"{9999999999999999999999999999999999999}","args":["x"]})",
      R"({"template":"{-1}","args":["x"]})",
      R"({"template":"{}","args":["x"]})",
      R"({"template":"{0}","args":[null]})"}) {
    SCOPED_TRACE(text);
    ObString result;
    EXPECT_NE(OB_SUCCESS, expand(text, result));
  }
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
