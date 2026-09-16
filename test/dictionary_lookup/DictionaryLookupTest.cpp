#include <Arduino.h>
#include <gtest/gtest.h>

#include "util/DictZip.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

namespace DictionaryRegistry {
bool resolveBasePath(const char*, std::string& out) {
  out = "/dictionaries/test/words";
  return true;
}
}  // namespace DictionaryRegistry

namespace DictZip {
bool extractEntry(const char*, uint32_t, uint32_t, HalFile&, ExtractError* error) {
  if (error) *error = ExtractError::Decompress;
  return false;
}
}  // namespace DictZip

namespace {
constexpr const char* BASE = "/dictionaries/test/words";

void appendBe32(std::string& text, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) text.push_back(static_cast<char>(value >> shift));
}

class DictionaryLookupTest : public testing::Test {
 protected:
  Dictionary dictionary;
  void SetUp() override {
    fake::reset();
    ESP.largestBlock = 200 * 1024;
    std::string index;
    index.append("cat", 4);
    appendBe32(index, 0);
    appendBe32(index, 6);
    index.append("mouse", 6);
    appendBe32(index, 6);
    appendBe32(index, 6);
    std::string synonyms("mice", 5);
    appendBe32(synonyms, 1);
    fake::add(std::string(BASE) + ".idx", index);
    fake::add(std::string(BASE) + ".syn", synonyms);
    fake::add(std::string(BASE) + ".dict", "felinerodent");
    ASSERT_TRUE(dictionary.open("test"));
    ASSERT_TRUE(dictionary.buildIndex());
  }
};

TEST_F(DictionaryLookupTest, FindsEntriesWithoutReadingDefinitions) {
  fake::files.erase(std::string(BASE) + ".dict");
  std::string headword;
  Dictionary::LookupResult result;
  // Exact, cleaned, stemmed and synonym searches all use the same search-only API.
  for (const char* query : {"cat", "Cat!", "cats", "mice"}) {
    const auto location = dictionary.findEntry(query, headword, &result);
    ASSERT_TRUE(location.found) << query;
    EXPECT_EQ(result, Dictionary::LookupResult::Found);
    EXPECT_EQ(headword, std::string(query) == "mice" ? "mouse" : "cat");
  }
  EXPECT_FALSE(dictionary.findEntry("missing", headword, &result).found);
  EXPECT_EQ(result, Dictionary::LookupResult::NotFound);
}

TEST_F(DictionaryLookupTest, CanReleaseOldDefinitionBetweenSearchAndRead) {
  std::string definition(64 * 1024, 'x');
  std::string headword;
  Dictionary::LookupResult result;
  ESP.largestBlock = 0;
  const auto location = dictionary.findEntry("mice", headword, &result);
  ASSERT_TRUE(location.found);
  EXPECT_EQ(definition.size(), 64 * 1024);
  std::string().swap(definition);
  ESP.largestBlock = 200 * 1024;
  ASSERT_TRUE(dictionary.readDefinition(location, definition, &result));
  EXPECT_EQ(definition, "rodent");
  EXPECT_EQ(result, Dictionary::LookupResult::Found);
}

TEST_F(DictionaryLookupTest, MissingWordDoesNotReplaceExistingDefinition) {
  std::string definition = "keep this definition";
  std::string headword;
  Dictionary::LookupResult result;
  EXPECT_FALSE(dictionary.lookup("absent", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::NotFound);
  EXPECT_EQ(definition, "keep this definition");
}

TEST_F(DictionaryLookupTest, ReportsReadAndMemoryFailuresAfterSuccessfulSearch) {
  std::string definition;
  std::string headword;
  Dictionary::LookupResult result;
  const auto location = dictionary.findEntry("cat", headword, &result);
  ASSERT_TRUE(location.found);
  ESP.largestBlock = 0;
  EXPECT_FALSE(dictionary.readDefinition(location, definition, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::LowMemory);
  ESP.largestBlock = 200 * 1024;
  fake::failRead = 0;
  EXPECT_FALSE(dictionary.readDefinition(location, definition, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
  EXPECT_TRUE(dictionary.lookup("cat", definition, headword, &result));
  EXPECT_EQ(definition, "feline");
}

TEST_F(DictionaryLookupTest, IndexFailureIsNotReportedAsMissingWord) {
  fake::files.erase(std::string(BASE) + ".idx");
  std::string headword;
  Dictionary::LookupResult result;
  const auto location = dictionary.findEntry("cat", headword, &result);
  EXPECT_FALSE(location.found);
  EXPECT_TRUE(location.readError);
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
}

TEST_F(DictionaryLookupTest, CompressedEntryCanBeFoundBeforeDecompressionFails) {
  fake::files.erase(std::string(BASE) + ".dict");
  fake::add(std::string(BASE) + ".dict.dz", "broken compressed data");
  ASSERT_TRUE(dictionary.open("test"));
  std::string definition;
  std::string headword;
  Dictionary::LookupResult result;
  const auto location = dictionary.findEntry("cat", headword, &result);
  ASSERT_TRUE(location.found);
  EXPECT_FALSE(dictionary.readDefinition(location, definition, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::Decompress);
}
}  // namespace
