// test.cpp
#include <iostream>

// 你的定时器实现
#include "timer_scheduler.h"

// 测试用例（包含基础 + stress）
#include "test.h"

int main() {
    std::cout << "Timer tests begin...\n";

    // --------------------
    // base tests
    // --------------------
    test_once_after_basic();
    test_cancel_once();
    test_periodic_and_cancel();
    test_multi_tasks_order_not_strict_but_all_fire();

    // --------------------
    // stress tests (try to reproduce race / cancel bugs)
    // --------------------
    test_periodic_cancel_stress();
    test_cancel_once_stress();

    std::cout << "All tests passed.\n";
    return 0;
}
