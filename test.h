// test.h
#pragma once
#include <cassert>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "timer_scheduler.h"

// -------------------- small helpers --------------------
static inline long long ms_since(std::chrono::steady_clock::time_point t0) {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

static inline void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static inline void print_ok(const std::string& name) {
    std::cout << "[OK] " << name << "\n";
}

// 等待某个条件成立（轮询 + 小睡），避免写死 sleep 造成用例更抖
static inline bool wait_until(std::function<bool()> pred,
                              int timeout_ms,
                              int step_ms = 1) {
    auto t0 = std::chrono::steady_clock::now();
    while (ms_since(t0) < timeout_ms) {
        if (pred()) return true;
        sleep_ms(step_ms);
    }
    return pred();
}

// -------------------- base tests --------------------
static void test_once_after_basic() {
    using namespace std::chrono;

    timer_scheduler ts;
    ts.start();

    std::atomic<bool> fired{false};
    auto t0 = steady_clock::now();

    // 150ms 后触发一次
    (void)ts.schedule_after(150, [&] {
        fired = true;
        auto dt = ms_since(t0);
        assert(dt >= 100 && "too early");
    });

    sleep_ms(80);
    assert(!fired);

    sleep_ms(150);
    assert(fired);

    ts.stop();
    print_ok("test_once_after_basic");
}

static void test_cancel_once() {
    timer_scheduler ts;
    ts.start();

    std::atomic<int> cnt{0};

    auto id = ts.schedule_after(200, [&] {
        cnt.fetch_add(1, std::memory_order_relaxed);
    });

    bool ok = ts.cancel(id);
    assert(ok);

    sleep_ms(350);
    assert(cnt.load(std::memory_order_relaxed) == 0);

    ts.stop();
    print_ok("test_cancel_once");
}

static void test_periodic_and_cancel() {
    using namespace std::chrono;

    timer_scheduler ts;
    ts.start();

    std::atomic<int> ticks{0};
    auto t0 = steady_clock::now();

    auto id = ts.schedule_every(100, [&] {
        ticks.fetch_add(1, std::memory_order_relaxed);
    });

    sleep_ms(550);
    int n = ticks.load(std::memory_order_relaxed);
    assert(n >= 3 && n <= 8);

    bool ok = ts.cancel(id);
    assert(ok);

    int before = ticks.load(std::memory_order_relaxed);
    sleep_ms(550);
    int after = ticks.load(std::memory_order_relaxed);

    // 允许 0 或 +1（取消瞬间竞态）
    assert(after == before || after == before + 1);

    auto total_ms = ms_since(t0);
    assert(total_ms >= 800);

    ts.stop();
    print_ok("test_periodic_and_cancel");
}

static void test_multi_tasks_order_not_strict_but_all_fire() {
    timer_scheduler ts;
    ts.start();

    std::mutex mu;
    std::vector<int> fired;

    ts.schedule_after(80, [&] {
        std::lock_guard<std::mutex> lk(mu);
        fired.push_back(80);
    });
    ts.schedule_after(30, [&] {
        std::lock_guard<std::mutex> lk(mu);
        fired.push_back(30);
    });
    ts.schedule_after(120, [&] {
        std::lock_guard<std::mutex> lk(mu);
        fired.push_back(120);
    });

    sleep_ms(250);

    {
        std::lock_guard<std::mutex> lk(mu);
        assert(fired.size() == 3);

        bool has30 = false, has80 = false, has120 = false;
        for (int v : fired) {
            if (v == 30) has30 = true;
            if (v == 80) has80 = true;
            if (v == 120) has120 = true;
        }
        assert(has30 && has80 && has120);
    }

    ts.stop();
    print_ok("test_multi_tasks_order_not_strict_but_all_fire");
}

// -------------------- stress tests: try to reproduce bugs --------------------

// 目标：放大 “cancel 后周期任务还在持续跑” 的间歇性问题
// 策略：短周期 + 多次重复 + cancel 后短时间观察是否还在增长
static void test_periodic_cancel_stress() {
    timer_scheduler ts;
    ts.start();

    constexpr int kRounds = 80;
    constexpr int kIntervalMs = 5;

    for (int round = 1; round <= kRounds; ++round) {
        std::atomic<int> ticks{0};

        auto id = ts.schedule_every(kIntervalMs, [&] {
            ticks.fetch_add(1, std::memory_order_relaxed);
        });

        // 等到至少触发几次（避免“还没跑起来就 cancel”）
        bool ok_started = wait_until([&] {
            return ticks.load(std::memory_order_relaxed) >= 3;
        }, /*timeout_ms=*/300);

        if (!ok_started) {
            std::cerr << "[FAIL] stress round " << round
                      << ": periodic didn't start in time\n";
            assert(false);
        }

        bool ok = ts.cancel(id);
        assert(ok);

        int before = ticks.load(std::memory_order_relaxed);

        // 观察窗口：50ms 内不应再增长（允许 +1 竞态）
        sleep_ms(50);
        int after = ticks.load(std::memory_order_relaxed);

        if (!(after == before || after == before + 1)) {
            std::cerr << "[FAIL] stress round " << round
                      << ": before=" << before << " after=" << after << "\n";
            assert(false);
        }

        // 给调度线程一点“空窗期”，减少 round 间互相影响
        sleep_ms(5);
    }

    ts.stop();
    print_ok("test_periodic_cancel_stress");
}

// 目标：放大 “一次性任务 cancel 后仍然执行” 的问题（如果存在）
// 策略：延迟稍长（保证可 cancel），重复多次
static void test_cancel_once_stress() {
    timer_scheduler ts;
    ts.start();

    constexpr int kRounds = 200;

    for (int round = 1; round <= kRounds; ++round) {
        std::atomic<int> cnt{0};

        auto id = ts.schedule_after(50, [&] {
            cnt.fetch_add(1, std::memory_order_relaxed);
        });

        bool ok = ts.cancel(id);
        assert(ok);

        // 等待足够超过 50ms
        sleep_ms(90);

        int v = cnt.load(std::memory_order_relaxed);
        if (v != 0) {
            std::cerr << "[FAIL] cancel_once_stress round " << round
                      << ": cnt=" << v << "\n";
            assert(false);
        }
    }

    ts.stop();
    print_ok("test_cancel_once_stress");
}


static void test_start_twice_bug_demo_DO_NOT_RUN_BY_DEFAULT() {
    timer_scheduler ts;
    ts.start();
    ts.stop();
    // 第二次 start 在你的当前实现下很可能直接 std::terminate
    ts.start();
    ts.stop();
}
