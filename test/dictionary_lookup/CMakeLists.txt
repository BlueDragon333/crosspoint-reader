add_executable(DictionaryLookupTest DictionaryLookupTest.cpp ../../src/util/Dictionary.cpp)
target_include_directories(DictionaryLookupTest PRIVATE
  stubs
  ../library_builder/stubs
  ../../src
)
target_link_libraries(DictionaryLookupTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(DictionaryLookupTest)
