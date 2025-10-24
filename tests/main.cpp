#include <gtest/gtest.h>

// main 函数由 gtest_main 库提供，
// 但如果我们需要做全局设置(Setup/Teardown)，
// 我们可以写自己的 main。
// 为简单起见，我们依赖 gtest_main，
// 但如果 CMake 链接 gtest_main 失败，
// 则取消下面的注释：

/*
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
*/