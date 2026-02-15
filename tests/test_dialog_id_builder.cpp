// =============================================================================
// Test framework: Google Test
// Add to CMakeLists.txt:
//   find_package(GTest REQUIRED)
//   enable_testing()
//   add_executable(sip_processor_tests tests/*.cpp src/common/*.cpp ...)
//   target_link_libraries(sip_processor_tests GTest::gtest_main ...)
//   add_test(NAME unit_tests COMMAND sip_processor_tests)
// =============================================================================


// =============================================================================
// FILE: tests/test_dialog_id_builder.cpp
// =============================================================================
#include <gtest/gtest.h>
#include "sip/sip_dialog_id.h"

using namespace sip_processor;

TEST(DialogIdBuilder, IsValidRejectsEmpty) {
    EXPECT_FALSE(DialogIdBuilder::is_valid(""));
}

TEST(DialogIdBuilder, IsValidAcceptsNormal) {
    EXPECT_TRUE(DialogIdBuilder::is_valid("abc123;ft=tag1;tt=tag2"));
}

TEST(DialogIdBuilder, IsValidRejectsTooLong) {
    std::string long_id(2000, 'x');
    EXPECT_FALSE(DialogIdBuilder::is_valid(long_id));
}

TEST(DialogIdBuilder, BuildFromNullSipReturnsEmpty) {
    EXPECT_EQ(DialogIdBuilder::build(nullptr), "");
}

TEST(DialogIdBuilder, BuildFromNullHandleReturnsEmpty) {
    EXPECT_EQ(DialogIdBuilder::build_from_handle(nullptr), "");
}
