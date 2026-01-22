
---

## semantics.md（直接覆盖用）

```md
# semantics.md

本文档描述 `timer_scheduler` 的**真实并发语义与时间语义**（以当前 `timer_scheduler.h` 实现为准）。

---

## 1. 术语与核心数据结构

- **后台线程**：`start()` 创建 1 个线程执行 `event_loop()`。
- **待执行队列**：`std::vector<timer_info> timers`
  - 存放尚未到期的任务
- **到期快照**：`std::vector<timer_info> on_time_timers`
  - 每轮循环把到期任务从 `timers` 移到这里，然后在无锁状态下执行回调
- **取消集合**：`std::unordered_set<size_t> canceled_ids`
  - 用于处理“任务已从 `timers` 移出、但尚未执行回调”的取消竞态

`timer_info` 字段：
- `id`：任务 id（从 1 开始递增）
- `end`：下一次触发的绝对时间点（`steady_clock::time_point`）
- `interval`：周期任务的间隔
- `callback`：回调
- `repeat`：是否周期任务

---

## 2. 生命周期语义（start / stop / destructor）

### 2.1 start()

- 行为：
  - 设置 `running = true`
  - 创建后台线程：`loop_threads = std::thread(&timer_scheduler::event_loop, this)`
- 线程模型：**one scheduler thread**

约束（当前实现）：
- `start()` **不具备幂等保护**：重复调用可能覆盖 `loop_threads`，属于未定义/不推荐用法。
- 推荐用法：每个对象 `start()` 调一次，结束靠析构回收线程。

### 2.2 stop()

- 行为：仅设置 `running = false`
- 不保证：
  - 不会阻塞等待线程退出（不 `join`）
  - 不会立刻唤醒 `cv.wait_for`（当前 `stop()` 未 `notify`）
- 实际退出时机：
  - 后台线程在下一次 `wait_for` 超时或被 `notify_one()` 唤醒后，检测到 `running=false`，退出循环。

### 2.3 析构函数 ~timer_scheduler()

- 行为：
  - 设置 `running=false`
  - `cv.notify_one()`
  - `join()` 后台线程（若 `joinable()`）

因此：**释放资源与线程收敛最终依赖析构**。

---

## 3. 调度语义（schedule_after / schedule_every）

### 3.1 schedule_after(ms, cb)

- 创建一次性任务：
  - `end = now + ms`
  - `repeat = false`
  - `id` 在 `add_timer` 中分配（`id==0` 时分配）
- 返回值：任务 id（>=1）

时间语义（弱保证）：
- 回调不会早于 `end` 之前执行（以 `steady_clock` 判断）。
- 可能晚于 `end` 执行：
  - OS 调度延迟
  - 线程负载
  - `sleep_time` 计算与 `wait_for` 抖动
  - 回调执行耗时（同一线程串行执行）

### 3.2 schedule_every(ms, cb)

- 创建周期任务：
  - 首次 `end = now + ms`
  - `repeat = true`
  - `id` 分配一次并保持不变（重新入队时复用同一 id）

周期语义：
- 当前实现是 **fixed-delay 风格**（更接近“上一轮调度点 now + interval”）：
  - 当任务到期后被取出执行回调
  - 执行完回调后：`end = now + interval` 再次入队  
  - 如果回调耗时较长，会产生漂移/堆积延迟

---

## 4. 取消语义（cancel）

### 4.1 cancel(id) 做了什么

- 在持锁状态下：
  1) 尝试从 `timers` 中删除该 id 的待执行任务（若存在）
  2) 把 `id` 放入 `canceled_ids`
  3) `cv.notify_one()` 唤醒后台线程

返回值（当前实现）：
- `return (id 在 timers 中查找不到)`
- 注意：这并不等价于“回调一定不会执行”，因为存在并发竞态。

### 4.2 并发竞态与保证边界

后台线程每轮会：
- 把到期任务从 `timers` 移到 `on_time_timers`
- 对每个到期任务，在执行回调前检查 `canceled_ids`：
  - 若命中：跳过回调，并把该 id 从 `canceled_ids` 删除（消费掉一次取消信号）
  - 若不命中：执行回调

因此：

#### 情况 A：任务仍在 `timers`（未到期、未被移动）
- `cancel(id)` 从 `timers` 删除后，回调不会执行（强概率保证）
- 但当前实现仍会把 id 放入 `canceled_ids`，该 id 可能永远不会被消费（集合增长风险）

#### 情况 B：任务已到期并被移动到 `on_time_timers`，但回调尚未开始
- `cancel(id)` 会把 id 放入 `canceled_ids`
- 后台线程检查到该 id 后会跳过回调（较强保证）

#### 情况 C：回调已经开始执行
- `cancel(id)` **无法中断**正在执行的回调
- 对周期任务而言：
  - 本次可能已执行
  - 取消信号会阻止后续重新入队（取决于取消发生在“检查取消”之前还是之后）
  - 测试一般允许“取消瞬间多触发 0~1 次”

---

## 5. 复杂度与实现策略

- `timers` 是 `vector`：
  - 每轮扫描找出到期任务：O(n)
  - 找最近到期任务：`min_element`，O(n)
- 唤醒策略：
  - 无任务时默认 `sleep_time=10ms`
  - 有任务时根据最近到期时间计算 sleep_time
- 计时基准：`std::chrono::steady_clock`（适合做相对计时）

---

## 6. 建议的“面试可讲改进点”

（不改变本语义文档的描述，仅列出可改进方向）

1) `stop()` 进行 `notify_one()` 并可选 `join()`，把停止语义做成“可同步收敛”。  
2) `start()` 增加幂等保护，避免重复 start 覆盖线程句柄。  
3) `cancel()`：若在 `timers` 中成功删除（一次性任务），可不插入 `canceled_ids`，避免集合增长。  
4) 用最小堆/优先队列替换 `vector + min_element`，将取最早到期任务降为 O(log n)。  
