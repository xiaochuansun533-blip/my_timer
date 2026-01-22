# timer_scheduler — Semantics（以当前实现为准）

本文档描述当前 `timer_scheduler` 的**真实可观察行为**（以代码为准）。  
实现若变更（尤其是线程启动/停止与 cancel 语义），需同步更新本文档与测试。

---

## 1. 总览：模型与线程

### 1.1 线程模型
- `timer_scheduler` **默认构造不创建线程**：`timer_scheduler() = default;`
- 调度线程在 `start()` 中创建：`loop_threads = std::thread(&timer_scheduler::event_loop, this);`
- 所有定时器回调 `callback` 在 **调度线程**（`loop_threads`）中执行。

### 1.2 启停模型（重要）
- `start()` 会做两件事（在同一把 mutex 下）：
  1) 创建调度线程
  2) 设置 `running = true`
- `stop()` 仅设置 `running = false`（同样在 mutex 下），不会 join 线程、不会 notify 条件变量。
- 析构函数会把 `running=false`，`cv.notify_one()`，然后 `join()`（若线程 joinable）。

> 注意：当前实现中 `start()` 是“创建线程 + 置 running=true”的组合操作；如果多次调用 `start()`，会覆盖 `loop_threads` 并导致未 join 的线程对象被赋值（这是未定义/危险用法），因此**语义上要求：start() 只调用一次**。

---

## 2. 任务、ID 与数据结构

### 2.1 任务类型
- once：`schedule_after(ms, cb)` 创建，一次性触发。
- periodic：`schedule_every(ms, cb)` 创建，周期性触发。

### 2.2 任务 ID
- `next_id` 初始为 `1`。
- `add_timer()` 内：若 `timer.id == 0`，分配新 id（`timer.id = next_id++`）。
- 对 periodic：每次重排期会把同一个 `timer_info`（含原 id）再次 `add_timer(timer)`，因此 **周期任务 id 不变**。

### 2.3 队列与集合
- `timers`：等待队列（未到期的任务 + 周期任务下一次触发）。
- `on_time_timers`：本轮扫描得到的“已到期任务列表”（从 `timers` 搬运出来，随后执行）。
- `canceled_ids`：取消集合。`cancel(id)` 会把 id 插入该集合；调度线程在执行回调前会检查该集合。

---

## 3. 调度循环（event_loop）的真实流程

调度线程循环条件：`while (running)`

每一轮做以下事情：

1) `now = steady_clock::now()`  
2) 清空 `on_time_timers` / `on_time_ids`  
3) **持锁扫描 timers**：  
   - 对所有 `now >= it->end` 的任务：
     - `on_time_timers.push_back(*it)`
     - `timers.erase(it)`
4) **锁外遍历 on_time_timers**，对每个到期任务：
   - 先检查是否被取消（见 4.1）
   - 未取消则执行 `timer.callback()`
   - 若 `timer.repeat == true`，则做重排期：`timer.end = now + timer.interval; add_timer(timer);`
5) 计算下一次等待时长：
   - 若 `timers` 非空：找出 `end` 最小的任务，计算 `time_until_next = next_end - steady_clock::now()`；
   - 若 `time_until_next > 0`，`sleep_time = time_until_next`，否则 `sleep_time = 1ms`；
   - 若 `timers` 为空：`sleep_time = 10ms`
6) 进入 `cv.wait_for(lock, sleep_time)` 等待（可能超时返回，也可能被 notify 唤醒）

---

## 4. cancel 语义（以当前实现为准）

### 4.1 cancel 的作用点
`cancel(id)` 的动作（在 mutex 下）：

1) 在 `timers` 中查找该 id 的任务，若找到则 erase（最多删除一个）。
2) 把该 id 插入 `canceled_ids`。
3) `cv.notify_one()` 尝试唤醒调度线程（缩短等待）。

调度线程在执行每个到期任务前，会再次检查 `canceled_ids`：
- 若发现 `id` 存在于 `canceled_ids`：
  - 认为该任务已取消
  - 立即 `canceled_ids.erase(id)`
  - 跳过该任务（不执行回调；对 periodic 也不重排期）
- 若不存在，则正常执行回调与（若是 periodic）重排期

### 4.2 取消对“已到期并被搬运”的任务的影响
由于调度线程会在执行回调前检查 `canceled_ids`，因此：

- 若 `cancel(id)` 发生在调度线程检查 `canceled_ids` 之前，则本轮可阻止回调执行；
- 若 `cancel(id)` 发生在调度线程完成检查之后（或回调已开始执行），则本轮无法阻止回调。

这意味着 cancel 在并发下存在不可避免的**竞态窗口**，但实现已经通过 “canceled_ids 二次检查” 尽量缩小窗口。

### 4.3 cancel 返回值的真实含义
当前实现的返回值是：

- 先（可能）从 `timers` 删除一个匹配项
- 再次在 `timers` 中 find `id`
- 返回 `is_find == timers.end()`

因此该返回值表示：

- **调用结束时**：`timers` 容器中已经找不到该 `id`（至少当前瞬间如此）。

它**不保证**：
- 该 `id` 是否曾经存在；
- 该任务是否已经被搬运到 `on_time_timers`；
- 该任务回调是否已经执行/正在执行。

换言之：返回 `true` 只是说明“此刻 pending 队列里不再有该 id”。

---

## 5. 时间与周期语义

### 5.1 once：schedule_after(ms, cb)
- 计划时间 `end = now + ms`。
- 实际触发时间不会早于 end（除非系统时钟/调度异常）；允许延迟。
- 触发顺序不提供严格保证（见 6）。

### 5.2 periodic：schedule_every(ms, cb)
- 首次计划：`end = now + ms`
- 重排期策略：`timer.end = now + interval`（其中 `now` 是“本轮循环开头取到的 now”，不是回调结束时刻）
- 因此该实现属于“基于扫描时刻的 fixed-delay”策略，可能产生漂移（drift），尤其在回调耗时较大或系统负载较高时。

---

## 6. 顺序与并发保证

- `timers` 使用 `std::vector` 存储，不按 `end` 排序；每轮扫描是线性遍历。
- 同一轮内多个到期任务的执行顺序等于它们被扫描到并搬运的顺序（通常与插入顺序相关），**不保证严格按到期时间排序**。
- 回调在调度线程执行，用户若在回调里做长耗时操作，会阻塞后续任务执行。

---

## 7. stop / 析构语义

### 7.1 stop()
- `stop()` 只设置 `running=false`，不 notify。
- 调度线程退出时机：
  - 若线程正在 `wait_for`，可能要等到超时或被其它 `notify_one()` 唤醒后才退出；
  - 若线程正在执行回调，需等回调返回后下一轮检查 `while(running)` 才会退出。

### 7.2 析构
- 析构在 mutex 下设置 `running=false`，`cv.notify_one()`，然后 `join()`。
- 析构不承诺“执行完所有未触发任务”；停止后剩余任务会被放弃。

---

## 8. 使用约束（由当前实现推出）

为避免未定义行为或不可预期结果，使用方应遵守：
- `start()` 只调用一次；不要重复 start。
- 在 `start()` 之前不要依赖定时器回调执行（可以先 schedule，但线程未启动前不会触发）。
- 回调函数应尽量轻量，避免阻塞。
- 对 cancel：不要把返回值当作“回调一定不会执行”的强保证；它只保证 pending 队列不再含该 id，并尽力阻止后续执行。
