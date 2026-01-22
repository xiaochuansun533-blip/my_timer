
---

## `semantics.md`

```markdown
# semantics.md

本文档描述 `timer_scheduler` 的**真实并发语义与时间语义**（以当前 `timer_scheduler.h` 实现为准）。  
目标：让使用者明确“哪些保证存在、哪些只是最佳努力（best-effort）、哪些是竞态边界”。

---

## 0. 总览

- 线程模型：**one scheduler thread**（`start()` 创建 1 个后台线程执行 `event_loop()`）
- 执行模型：回调在后台线程**串行执行**
- 时间基准：`std::chrono::steady_clock`
- 关键原则：**回调执行不持锁**（避免 API 线程被长回调阻塞）

---

## 1. 术语与核心数据结构

- **后台线程**：`start()` 创建的调度线程，循环运行 `event_loop()`
- **待执行队列**：`std::vector<timer_info> timers`
  - 保存“尚未到期/尚未被搬运”的任务
- **到期快照**：`std::vector<timer_info> on_time_timers`
  - 每轮循环把到期任务从 `timers` 搬运到这里，然后在无锁状态执行回调
- **取消集合**：`std::unordered_set<size_t> canceled_ids`
  - 处理“任务已从 timers 移出，但尚未执行回调”的取消竞态

`timer_info` 字段语义：
- `id`：任务 id（从 1 开始递增）
- `end`：下一次触发的绝对时间点（`steady_clock::time_point`）
- `interval`：周期任务间隔
- `callback`：回调函数
- `repeat`：是否周期任务

---

## 2. 生命周期语义（start / stop / destructor）

### 2.1 `start()`

行为：
- 设置 `running = true`
- 创建后台线程：执行 `event_loop()`

约束（当前实现）：
- **不具备幂等保护**：重复调用可能覆盖线程句柄，属于不推荐/潜在未定义用法
- 推荐：每个对象仅调用一次 `start()`，退出依赖析构收敛线程

### 2.2 `stop()`

行为：
- 仅设置 `running = false`

不保证：
- 不 `join()`：不会同步等待线程退出
- 未必立即唤醒：当前 `stop()` 不负责 `notify`

实际退出时机：
- 后台线程在下一次 `cv.wait_for(...)` 超时或被 `notify` 唤醒后，检测到 `running=false` 才退出循环

### 2.3 析构 `~timer_scheduler()`

析构负责最终收敛：
- 设置 `running=false`
- `cv.notify_one()`
- 若线程 `joinable()`，执行 `join()`

结论：
- **释放资源与线程收敛最终依赖析构**（而不是 `stop()`）

---

## 3. 调度语义（schedule_after / schedule_every）

### 3.1 `schedule_after(delay, cb)`

创建一次性任务：
- `end = now + delay`
- `repeat = false`
- `id` 在内部添加逻辑中分配（`id >= 1`）

时间语义（弱保证）：
- 回调**不会早于** `end` 执行（以 `steady_clock` 判断）
- 回调**可能晚于** `end` 执行，常见原因包括：
  - OS 调度延迟、线程负载
  - `wait_for` 抖动
  - 回调串行执行的排队效应（前序回调耗时）

### 3.2 `schedule_every(interval, cb)`

创建周期任务：
- 首次 `end = now + interval`
- `repeat = true`
- 重新入队时复用同一 `id`

周期语义（当前实现）：
- **fixed-delay 风格**（更接近“本次执行完成后，再以当下 now + interval 作为下次触发点”）
- 若回调耗时较长，会产生漂移（drift）与延迟堆积

---

## 4. 取消语义（cancel）

### 4.1 `cancel(id)` 做了什么

在持锁状态下：
1) 尝试从 `timers` 中删除该 id 的待执行任务（若存在）  
2) 把 `id` 放入 `canceled_ids`  
3) `cv.notify_one()` 唤醒后台线程

返回值（当前实现的直观含义）：
- `true/false` 与 “id 是否还能在 timers 中找到”相关  
- **注意**：返回值不等价于“回调一定不会执行”，因为存在并发竞态（见 4.2）

### 4.2 并发竞态与保证边界

后台线程每轮：
- 把到期任务从 `timers` 搬运到 `on_time_timers`
- 对每个到期任务，在执行回调前检查 `canceled_ids`：
  - 若命中：跳过回调，并把该 id 从 `canceled_ids` 删除（消费一次取消信号）
  - 若不命中：执行回调

因此存在三类关键情况：

#### 情况 A：任务仍在 `timers`（未到期、未搬运）
- `cancel(id)` 若成功从 `timers` 删除：回调不会执行（强概率/近似强保证）
- 但：当前实现仍会把 id 放入 `canceled_ids`，可能出现“永远不被消费”的集合增长风险

#### 情况 B：任务已到期并搬运到 `on_time_timers`，但回调尚未开始
- `cancel(id)` 写入 `canceled_ids`
- 后台线程检查到该 id 后跳过回调（较强保证）

#### 情况 C：回调已经开始执行
- `cancel(id)` **无法中断**正在执行的回调
- 对周期任务：
  - 可能出现“取消瞬间多触发 0~1 次”的现象
  - 后续是否继续入队，取决于取消发生在后台线程“检查取消/重入队”的相对时序

---

## 5. 复杂度与实现策略

- `timers` 采用 `vector`：
  - 每轮扫描找出到期任务：O(n)
  - 找最近到期任务：`min_element`，O(n)
- 唤醒策略：
  - 无任务时：默认 `sleep_time = 10ms`
  - 有任务时：根据最近到期时间计算 `sleep_time`
- 计时基准：`steady_clock`（适合相对计时，不受系统时间跳变影响）

---

## 6. 建议的“可讲改进点”（不改变本文对现状语义的描述）

1) `stop()` 增加 `notify_one()`，并可选 `join()`，把停止语义做成“可同步收敛”  
2) `start()` 增加幂等保护，避免重复 start 覆盖线程句柄  
3) `cancel()`：若在 `timers` 中成功删除（一次性任务），可不插入 `canceled_ids`，避免集合增长  
4) 用最小堆/优先队列替换 `vector + min_element`，将取最早到期任务降为 O(log n)
