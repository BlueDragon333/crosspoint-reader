add_executable(DictionaryDefinitionTest
  DictionaryDefinitionTest.cpp
  ../../src/activities/reader/DictionaryDefinitionActivity.cpp
  ../../src/activities/reader/DictionaryWordSelection.cpp
  ../../src/util/HtmlToPlainText.cpp
  ../../lib/Epub/Epub/htmlEntities.cpp
)
target_include_directories(DictionaryDefinitionTest PRIVATE stubs ../../lib/Memory ../../lib/Epub ../../src)
target_link_libraries(DictionaryDefinitionTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(DictionaryDefinitionTest)
