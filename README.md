# timer_scheduler (C++17)

一个轻量的 C++17 定时器调度器（header-only），支持：
- 一次性定时任务（delay once）
- 周期性定时任务（fixed-delay interval）
- 取消任务（best-effort cancel）
- 线程安全地添加/取消（通过互斥锁 + 条件变量）

> 本项目更偏“并发/语义/工程实践展示”，包含可运行测试与压力测试（stress），适合作为简历/面试作品。

---

## 目录

- [环境与依赖](#环境与依赖)
- [快速开始](#快速开始)
- [API 概览](#api-概览)
- [使用示例](#使用示例)
- [线程模型](#线程模型)
- [已知限制与注意事项](#已知限制与注意事项)
- [项目结构](#项目结构)
- [License](#license)

---

## 环境与依赖

- 编译器：GCC / Clang（支持 C++17）
- 平台：Linux / WSL2（Ubuntu）
- 依赖：仅使用标准库（`<thread> <mutex> <condition_variable> <chrono> ...`）

---

## 快速开始

### 1) 编译并运行测试

在项目根目录：

```bash
g++ -std=c++17 -O2 -pthread test.cpp -o test
./test
