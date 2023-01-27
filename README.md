# lock_or_rte_ring_benchmark

全部开启 O3 优化，先是纯写，再是纯读。

哈希表默认预留 每个线程产生的请求数（25000000） 的两倍空间，key 锁默认 100000 个，map 锁和 map 个数相同，ring 的预留大小为 4194304。

## 结论

|方法|写|读|
|---|---|---
|lock|慢|极快|
|rte_ring|极快|很快|

moodycamel ring 比 rte_ring 稍慢，优点是它是一个 header-only 的 c++ 类库，使用很方便、很自然。

## lock

```cpp
using ReadLock = std::shared_lock<std::shared_mutex>;
using WriteLock = std::unique_lock<std::shared_mutex>;
```

有 thread_num 个 map，按照 `hash(key) % thread_num` 来决定放哪个 map。

执行 write 的时候，先给 `hash(key) % lock_num` 上一个 key 锁，再上对应的 map 锁。

key 锁 100000 个和 10000000 个的性能差距基本没有，主要在 map 锁上面。

### lock 先读后写 1 线程

```
lock test, 25000000 write/read op per thread
[PUT] total 3.9633 Mops, in 6.3079 s
      per-thread 3.9633 Mops
[GET] total 5.4860 Mops, in 4.5571 s
      per-thread 5.4860 Mops
```

### lock 先读后写 16 线程

```
lock test, 25000000 write/read op per thread
[PUT] total 9.0414 Mops, in 44.2411 s
      per-thread 0.5651 Mops
[GET] total 20.0871 Mops, in 19.9133 s
      per-thread 1.2554 Mops
```

## ring

和 lock 相比，哈希表放在线程里面（而不是开一个全局数组用线程 id 访问），减少访存次数。

使用 wyhash。

每个线程每完成一次请求，就 `finished_cnt[thread_id]++`。主线程统计 finished_cnt 来判断是不是都运行完了。finished_cnt 要 cacheline 对齐，不然会严重影响性能。

### rte_ring MPSC 1 线程

```
MPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 4.4691 Mops, in 5.5940 s
      per-thread 4.4691 Mops
[GET] total 5.5161 Mops, in 4.5322 s
      per-thread 5.5161 Mops
```

### rte_ring MPSC 16 线程

```
MPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 20.4936 Mops, in 19.5183 s
      per-thread 1.2809 Mops
[GET] total 17.4509 Mops, in 22.9214 s
      per-thread 1.0907 Mops
```

### rte_ring SPSC 1 线程

```
SPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 4.5746 Mops, in 5.4649 s
      per-thread 4.5746 Mops
[GET] total 4.8967 Mops, in 5.1054 s
      per-thread 4.8967 Mops
```

### rte_ring SPSC 16 线程

```
SPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 21.9137 Mops, in 18.2534 s
      per-thread 1.3696 Mops
[GET] total 14.8065 Mops, in 27.0151 s
      per-thread 0.9254 Mops
```

### moodycamel MPSC 1 线程

https://github.com/cameron314/concurrentqueue

```
ring moodycamel test, 25000000 write/read op per thread
[PUT] total 4.7539 Mops, in 5.2588 s
      per-thread 4.7539 Mops
[GET] total 6.1694 Mops, in 4.0522 s
      per-thread 6.1694 Mops
```

### moodycamel MPSC 16 线程

```
ring moodycamel test, 25000000 write/read op per thread
[PUT] total 19.0847 Mops, in 20.9592 s
      per-thread 1.1928 Mops
[GET] total 16.4463 Mops, in 24.3215 s
      per-thread 1.0279 Mops
```