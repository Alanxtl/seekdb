/* Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

class AIProviderTest : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
  ObJsonObject *parse(const char *text)
  {
    ObString str(text);
    ObJsonObject *object = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, str, object));
    return object;
  }
};

TEST_F(AIProviderTest, restores_embedding_indices)
{
  ObIJsonBase *result = nullptr;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_,
      parse(R"({"data":[{"index":1,"embedding":[3,4]},{"index":0,"embedding":[1,2]}]})"), result));
  ObString str;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator_, result, str));
  EXPECT_EQ(ObString("[[1, 2], [3, 4]]"), str);
}

TEST_F(AIProviderTest, rejects_malformed_embeddings)
{
  const char *invalid[] = {
      R"({})", R"({"data":{}})", R"({"data":[]})", R"({"data":[null]})",
      R"({"data":[{"index":0,"embedding":[]}]})",
      R"({"data":[{"index":0,"embedding":["1"]}]})",
      R"({"data":[{"index":0,"embedding":[true]}]})",
      R"({"data":[{"index":-1,"embedding":[1]}]})",
      R"({"data":[{"index":1,"embedding":[1]}]})",
      R"({"data":[{"index":0.1,"embedding":[1]}]})",
      R"({"data":[{"embedding":[1]}]})",
      R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
      R"({"data":[{"index":0,"embedding":[1]},{"index":1,"embedding":[2,3]}]})"
  };
  for (const char *text : invalid) {
    SCOPED_TRACE(text);
    ObIJsonBase *result = nullptr;
    ObOpenAIUtils::ObOpenAIEmbed provider;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, parse(text), result));
  }
}

TEST_F(AIProviderTest, validates_requested_count_and_dimension)
{
  ObIJsonBase *result = nullptr;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_,
      parse(R"({"data":[{"index":0,"embedding":[1,2]}]})"), result));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::validate_embeddings(result, 1, 2));
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::validate_embeddings(result, 2, 2));
  EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::validate_embeddings(result, 1, 3));
}

TEST_F(AIProviderTest, rejects_invalid_completion_content)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  const char *invalid[] = {R"({})", R"({"choices":[]})",
      R"({"choices":[{"message":{"content":null}}]})",
      R"({"choices":[{"message":{"content":""}}]})",
      R"({"choices":[{"message":{"content":{}}}]})"};
  for (const char *text : invalid) {
    SCOPED_TRACE(text);
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, parse(text), result));
  }
}

TEST_F(AIProviderTest, ollama_native_non_streaming_protocol)
{
  ObAIFuncIComplete *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, ObString("oLlAmA"), provider));
  ObArray<ObString> headers;
  ObString empty, model("model"), prompt("hello");
  EXPECT_EQ(OB_SUCCESS, provider->get_header(allocator_, empty, headers));
  EXPECT_EQ(1, headers.count());
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator_, model, empty, prompt, nullptr, body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  ASSERT_NE(nullptr, body->get_value("prompt"));
}

TEST_F(AIProviderTest, config_must_be_an_object)
{
  for (const char *text : {"[]", "null", "1", "\"hello\""}) {
    ObJsonObject *object = nullptr;
    ObString input(text);
    EXPECT_NE(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, object));
  }
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
