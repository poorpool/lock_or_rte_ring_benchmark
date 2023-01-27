# lock_or_rte_ring_benchmark

全部开启 O3 优化，先是纯写，再是纯读。

哈希表默认预留 每个线程产生的请求数（25000000） 的两倍空间，key 锁默认 100000 个，map 锁和 map 个数相同，ring 的预留大小为 4194304。

## 结论

|方法|写|读|
|---|---|---
|lock|慢|较快|
|ring|最快|最快|

读写均为 ring 最快。

moodycamel ring 是一个 header-only 的 c++ 类库，使用很方便、很自然。MPMC 版本叫 concurrentqueue，SPSC 版本叫 readerwriterqueue。

**SPSC moodycamel** 略快于 SPSC rte_ring 快于 MPSC rte_ring 快于 MPMC concurrentqueue

## lock

```cpp
using ReadLock = std::shared_lock<std::shared_mutex>;
using WriteLock = std::unique_lock<std::shared_mutex>;
```

有 thread_num 个 map，按照 `hash(key) % thread_num` 来决定放哪个 map。

执行 write 的时候，先给 `hash(key) % lock_num` 上一个 key 锁，再上对应的 map 锁。

key 锁 100000 个和 10000000 个的性能差距基本没有，主要在 map 锁上面。

每个线程循环做这种事情：先处理 32 个请求，再 pull 32 次 ring 处理别人发过来的请求。

### lock 先读后写 1 线程

```
lock test, 25000000 write/read op per thread
[PUT] total 3.9573 Mops, in 6.3174 s
      per-thread 3.9573 Mops
[GET] total 5.4656 Mops, in 4.5741 s
      per-thread 5.4656 Mops
```

### lock 先读后写 16 线程

```
lock test, 25000000 write/read op per thread
[PUT] total 9.0534 Mops, in 44.1821 s
      per-thread 0.5658 Mops
[GET] total 19.7986 Mops, in 20.2035 s
      per-thread 1.2374 Mops
```

## ring

和 lock 相比，哈希表放在线程里面（而不是开一个全局数组用线程 id 访问），减少访存次数。

使用 wyhash。

每个线程每完成一次请求，就 `finished_cnt[thread_id]++`。主线程统计 finished_cnt 来判断是不是都运行完了。finished_cnt 要 cacheline 对齐，不然会严重影响性能。

### rte_ring MPSC 1 线程

```
MPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 4.5535 Mops, in 5.4903 s
      per-thread 4.5535 Mops
[GET] total 5.6969 Mops, in 4.3884 s
      per-thread 5.6969 Mops
```

### rte_ring MPSC 16 线程

```
MPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 21.4909 Mops, in 18.6125 s
      per-thread 1.3432 Mops
[GET] total 26.0146 Mops, in 15.3760 s
      per-thread 1.6259 Mops
```

### rte_ring SPSC 1 线程

```
SPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 4.4190 Mops, in 5.6574 s
      per-thread 4.4190 Mops
[GET] total 5.5303 Mops, in 4.5206 s
      per-thread 5.5303 Mops
```

### rte_ring SPSC 16 线程

```
SPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 25.0270 Mops, in 15.9828 s
      per-thread 1.5642 Mops
[GET] total 28.0200 Mops, in 14.2755 s
      per-thread 1.7512 Mops
```

### moodycamel MPSC 1 线程

https://github.com/cameron314/concurrentqueue

```
ring moodycamel test, 25000000 write/read op per thread
[PUT] total 4.4849 Mops, in 5.5742 s
      per-thread 4.4849 Mops
[GET] total 5.7694 Mops, in 4.3332 s
      per-thread 5.7694 Mops
```

### moodycamel MPSC 16 线程

```
ring moodycamel test, 25000000 write/read op per thread
[PUT] total 20.3612 Mops, in 19.6452 s
      per-thread 1.2726 Mops
[GET] total 17.3446 Mops, in 23.0620 s
      per-thread 1.0840 Mops
```

### moodycamel SPSC 1 线程

https://github.com/cameron314/readerwriterqueue

```
SPSC readerwriterqueue test, 25000000 write/read op per thread
[PUT] total 4.6672 Mops, in 5.3565 s
      per-thread 4.6672 Mops
[GET] total 5.1669 Mops, in 4.8385 s
      per-thread 5.1669 Mops
```

### moodycamel SPSC 16 线程

```
SPSC readerwriterqueue test, 25000000 write/read op per thread
[PUT] total 26.3566 Mops, in 15.1764 s
      per-thread 1.6473 Mops
[GET] total 30.0958 Mops, in 13.2909 s
      per-thread 1.8810 Mops
```