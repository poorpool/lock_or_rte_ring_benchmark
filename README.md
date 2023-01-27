# lock_or_rte_ring_benchmark

全部开启 O3 优化，先是纯写，再是纯读。全部使用 jemalloc。

哈希表默认预留 每个线程产生的请求数（25000000） 的两倍空间，key 锁默认 100000 个，map 锁和 map 个数相同，ring 的预留大小为 4194304。

## 结论

|方法|写|读|
|---|---|---
|lock|慢|快|
|ring|最快|最快|

最快的方式是**使用 Jemalloc 的 SPSC ring**（moodycamel::readerwriterqueue，rte_ring也行，速度差不多）。

jemalloc 对于 SPSC ring 场景是一个好的优化，其他场景不明显。

moodycamel ring 是一个 header-only 的 c++ 类库，使用很方便、很自然。MPMC 版本叫 concurrentqueue，SPSC 版本叫 readerwriterqueue。

SPSC moodycamel 略快于 SPSC rte_ring 远快于 MPSC rte_ring 快于 MPMC moodycamel

在单线程 SPSC 条件下，一次哈希表写操作 200ns，SPSC ring 操作 30ns。16 SPSC 线程条件下，一次哈希表写操作 350ns，SPSC ring 操作 55ns。

在多线程 SPSC 条件下，一次哈希表写操作 200ns，SPSC ring 操作 20ns（ring 是二维数组存放的，可能跟访存顺序有关）。16 SPSC 线程条件下，一次哈希表写操作 450ns，SPSC ring 操作 180ns。

## lock

```cpp
using ReadLock = std::shared_lock<std::shared_mutex>;
using WriteLock = std::unique_lock<std::shared_mutex>;
```

有 thread_num 个 map，按照 `hash(key) % thread_num` 来决定放哪个 map。

执行 write 的时候，先给 `hash(key) % lock_num` 上一个 key 锁，再上对应的 map 锁。

key 锁 100000 个和 10000000 个的性能差距基本没有，主要在 map 锁上面。

每个线程循环做这种事情：先处理 32 个请求，再 poll 32 次 ring 处理别人发过来的请求。

如果 poll 到空就停止，下一轮再 poll，这样可以有效减少空 poll 次数，总 pull 降低为原来的六分之一吧。ring 耗时大概 50ns 一次。一次操作平均要造成两次多的入队出队（空 poll 也算上）

### lock 先读后写 1 线程

```
lock test, 25000000 write/read op per thread
[PUT] total 4.1310 Mops, in 6.0518 s
      per-thread 4.1310 Mops
[GET] total 5.0279 Mops, in 4.9722 s
      per-thread 5.0279 Mops
```

### lock 先读后写 16 线程

```
lock test, 25000000 write/read op per thread
[PUT] total 9.1336 Mops, in 43.7943 s
      per-thread 0.5709 Mops
[GET] total 17.7169 Mops, in 22.5773 s
      per-thread 1.1073 Mops
```

## ring

和 lock 相比，哈希表放在线程里面（而不是开一个全局数组用线程 id 访问），减少访存次数。

使用 wyhash。

每个线程每完成一次请求，就 `finished_cnt[thread_id]++`。主线程统计 finished_cnt 来判断是不是都运行完了。finished_cnt 要 cacheline 对齐，不然会严重影响性能。

### rte_ring MPSC 1 线程

```
MPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 5.0949 Mops, in 4.9069 s
      per-thread 5.0949 Mops
[GET] total 5.8182 Mops, in 4.2969 s
      per-thread 5.8182 Mops
```

### rte_ring MPSC 16 线程

```
MPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 24.9208 Mops, in 16.0508 s
      per-thread 1.5576 Mops
[GET] total 26.3405 Mops, in 15.1857 s
      per-thread 1.6463 Mops
```

### rte_ring SPSC 1 线程

```
SPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 4.9330 Mops, in 5.0679 s
      per-thread 4.9330 Mops
[GET] total 5.8892 Mops, in 4.2450 s
      per-thread 5.8892 Mops
```

### rte_ring SPSC 16 线程

```
SPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 37.8342 Mops, in 10.5724 s
      per-thread 2.3646 Mops
[GET] total 38.1623 Mops, in 10.4815 s
      per-thread 2.3851 Mops
```

### moodycamel MPSC 1 线程

https://github.com/cameron314/concurrentqueue

```
ring moodycamel test, 25000000 write/read op per thread
[PUT] total 5.0460 Mops, in 4.9544 s
      per-thread 5.0460 Mops
[GET] total 5.2871 Mops, in 4.7285 s
      per-thread 5.2871 Mops
```

### moodycamel MPSC 16 线程

```
ring moodycamel test, 25000000 write/read op per thread
[PUT] total 19.5609 Mops, in 20.4490 s
      per-thread 1.2226 Mops
[GET] total 15.9355 Mops, in 25.1012 s
      per-thread 0.9960 Mops
```

### moodycamel SPSC 1 线程

https://github.com/cameron314/readerwriterqueue

```
SPSC readerwriterqueue test, 25000000 write/read op per thread
[PUT] total 5.1828 Mops, in 4.8236 s
      per-thread 5.1828 Mops
[GET] total 5.9561 Mops, in 4.1974 s
      per-thread 5.9561 Mops
```

### moodycamel SPSC 16 线程

```
SPSC readerwriterqueue test, 25000000 write/read op per thread
[PUT] total 38.7467 Mops, in 10.3235 s
      per-thread 2.4217 Mops
[GET] total 39.3582 Mops, in 10.1631 s
      per-thread 2.4599 Mops
```
