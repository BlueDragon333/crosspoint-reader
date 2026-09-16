#include <gtest/gtest.h>

#include "TestPlatform.h"

#define private public
#include "activities/reader/DictionaryDefinitionActivity.h"
#undef private

namespace {
using Navigation = DictionaryDefinitionActivity::Navigation;

class DictionaryDefinitionTest : public testing::TestWithParam<bool> {
 protected:
  GfxRenderer renderer;
  MappedInputManager input;
  Dictionary dictionary;
  std::unique_ptr<DictionaryDefinitionActivity> activity;

  void SetUp() override {
    dictionary.html = GetParam();
    dictionary.entries = {{"alpha", {"beta"}}, {"beta", {"gamma"}}, {"gamma", {"missing"}}};
    Activity::finished = false;
    activity = std::make_unique<DictionaryDefinitionActivity>(renderer, input, dictionary, "alpha", "alpha", "beta");
    activity->onEnter();
    dictionary.beforeRead = [this] {
      EXPECT_TRUE(activity->pages.empty());
      EXPECT_TRUE(activity->lines.empty());
      EXPECT_TRUE(activity->definition.empty());
      EXPECT_EQ(activity->pages.capacity(), 0u);
      EXPECT_EQ(activity->lines.capacity(), 0u);
      EXPECT_EQ(activity->definition.capacity(), std::string().capacity());
      EXPECT_EQ(activity->words, nullptr);
    };
  }

  void forward() {
    activity->startSelection();
    ASSERT_NE(activity->words, nullptr);
    activity->navigate(Navigation::Forward);
  }
};

TEST_P(DictionaryDefinitionTest, MissingWordPreservesDefinitionAndHistoryWithoutReading) {
  forward();
  forward();
  ASSERT_EQ(activity->headword, "gamma");
  ASSERT_EQ(activity->historySize, 2u);
  const auto reads = dictionary.reads.size();
  activity->startSelection();
  const auto* words = activity->words.get();
  activity->navigate(Navigation::Forward);
  EXPECT_EQ(activity->message, StrId::STR_DICT_NOT_FOUND);
  EXPECT_EQ(activity->headword, "gamma");
  EXPECT_EQ(activity->historySize, 2u);
  EXPECT_EQ(activity->words.get(), words);
  EXPECT_EQ(dictionary.reads.size(), reads);
}

TEST_P(DictionaryDefinitionTest, FailedReplacementReloadsPreviousWordWithoutPushingHistory) {
  dictionary.entries["beta"].failures = 1;
  forward();
  EXPECT_EQ(dictionary.reads, (std::vector<std::string>{"beta", "alpha"}));
  EXPECT_EQ(activity->headword, "alpha");
  EXPECT_EQ(activity->historySize, 0u);
  EXPECT_STREQ(activity->currentQuery, "alpha");
}

TEST_P(DictionaryDefinitionTest, BackFailureKeepsHistoryAndSuccessfulBackConsumesOneEntry) {
  forward();
  dictionary.entries["alpha"].failures = 1;
  activity->navigate(Navigation::Back);
  EXPECT_EQ(activity->historySize, 1u);
  EXPECT_EQ(activity->headword, "beta");
  activity->navigate(Navigation::Back);
  EXPECT_EQ(activity->historySize, 0u);
  EXPECT_EQ(activity->headword, "alpha");
  EXPECT_EQ(activity->words, nullptr);
}

TEST_P(DictionaryDefinitionTest, HistoryLimitAndBranchingPreserveBackPath) {
  dictionary.entries["gamma"].definition = "alpha";
  for (size_t i = 0; i < activity->HISTORY_CAPACITY; ++i) forward();
  const auto searches = dictionary.searches.size();
  forward();
  EXPECT_EQ(activity->message, StrId::STR_DICT_HISTORY_FULL);
  EXPECT_EQ(dictionary.searches.size(), searches);
  activity->navigate(Navigation::Back);
  EXPECT_EQ(activity->historySize, activity->HISTORY_CAPACITY - 1);
  forward();
  EXPECT_EQ(activity->historySize, activity->HISTORY_CAPACITY);
  for (size_t i = 0; i < activity->HISTORY_CAPACITY; ++i) activity->navigate(Navigation::Back);
  EXPECT_EQ(activity->headword, "alpha");
  EXPECT_EQ(activity->historySize, 0u);
}

TEST_P(DictionaryDefinitionTest, FailedRecoveryReturnsToWordSelection) {
  forward();
  dictionary.entries["gamma"].failures = 1;
  dictionary.entries["beta"].failures = 1;
  forward();
  EXPECT_TRUE(Activity::finished);
  EXPECT_EQ(activity->historySize, 1u);
}

TEST_P(DictionaryDefinitionTest, UnicodeQuerySurvivesRoundTrip) {
  dictionary.entries["beta"].definition = "éclair";
  dictionary.entries["éclair"] = {"alpha"};
  forward();
  forward();
  EXPECT_STREQ(activity->currentQuery, "éclair");
  forward();
  activity->navigate(Navigation::Back);
  EXPECT_STREQ(activity->currentQuery, "éclair");
}

INSTANTIATE_TEST_SUITE_P(PlainAndStyled, DictionaryDefinitionTest, testing::Bool());
}  // namespace
