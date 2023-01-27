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

### 为什么这里最快哈希表读只能达到 40Mops，

https://github.com/poorpool/mem-meta-kv-benchmark 这是一个纯净的哈希表读写测试，每个线程只读写自己的哈希表。最快哈希表读能达到 130 Mops

本测试最快的 ring 方法哈希表读 40Mops，这是因为本测试加入了哈希和线程之间的数据交互。比如说一个线程的耗时是 16s，这会包括：

1. 8.6s 的哈希表读写开销
2. 3.4s 的 ring 操作开销
3. 2.1s 的哈希函数计算开销
4. 计时等其他开销

### 编程建议

- 使用 jemalloc
- 使用 SPSC ring 在线程之间传递数据
- moodycamel::readerwriterqueue 因为它是一个纯粹的类，编程可能更方便
- 如果有什么数据放在全局共享的数组，将其对齐 cacheline，防止不停 cache 失效
- 如果要 poll 一个 ring，就连续多 poll 几下它再去干别的事情，poll 到空就停止这一轮的 poll

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

## 附录 moodycamel SPSC time test

```
SPSC readerwriterqueue time test, 25000000 write/read op per thread
time sum 3470045297, ring_ops 61405597, 56.5102 ns/op
hashmap time sum 8688587703, hash_ops 24993756, 347.6303 ns/op
calc hash time sum 2123998401, hash_ops 24993756, 84.9599 ns/op
time sum 3445025170, ring_ops 60068401, 57.3517 ns/op
hashmap time sum 8721484571, hash_ops 25003671, 348.8082 ns/op
calc hash time sum 2141636289, hash_ops 25003671, 85.6655 ns/op
time sum 3462709455, ring_ops 59371671, 58.3226 ns/op
hashmap time sum 8741326935, hash_ops 24995431, 349.7170 ns/op
calc hash time sum 2124264638, hash_ops 24995431, 84.9706 ns/op
time sum 3489395330, ring_ops 63215294, 55.1986 ns/op
hashmap time sum 8616102665, hash_ops 25005148, 344.5732 ns/op
calc hash time sum 2106956148, hash_ops 25005148, 84.2782 ns/op
time sum 3501892019, ring_ops 60353164, 58.0233 ns/op
hashmap time sum 8667303265, hash_ops 24997458, 346.7274 ns/op
calc hash time sum 2141492396, hash_ops 24997458, 85.6597 ns/op
time sum 3383839097, ring_ops 60629236, 55.8120 ns/op
hashmap time sum 8774554724, hash_ops 24996996, 351.0244 ns/op
calc hash time sum 2124448973, hash_ops 24996996, 84.9780 ns/op
time sum 3505773430, ring_ops 62195448, 56.3670 ns/op
hashmap time sum 8637366142, hash_ops 24999139, 345.5065 ns/op
calc hash time sum 2111486307, hash_ops 24999139, 84.4595 ns/op
time sum 3510712836, ring_ops 60815050, 57.7277 ns/op
hashmap time sum 8652383912, hash_ops 24998428, 346.1171 ns/op
calc hash time sum 2141529239, hash_ops 24998428, 85.6612 ns/op
time sum 3512704332, ring_ops 60961798, 57.6214 ns/op
hashmap time sum 8644110903, hash_ops 25005597, 345.6870 ns/op
calc hash time sum 2133130784, hash_ops 25005597, 85.3252 ns/op
[PUT] total 23.7170 Mops, in 16.8656 s
      per-thread 1.4823 Mops
time sum 3494111130, ring_ops 60024805, 58.2111 ns/op
hashmap time sum 8670630765, hash_ops 25005460, 346.7495 ns/op
calc hash time sum 2131207276, hash_ops 25005460, 85.2483 ns/op
time sum 3469572859, ring_ops 63717742, 54.4522 ns/op
hashmap time sum 8624606535, hash_ops 25007482, 344.8810 ns/op
calc hash time sum 2136419547, hash_ops 25007482, 85.4568 ns/op
time sum 3450725705, ring_ops 61740608, 55.8907 ns/op
hashmap time sum 8687531192, hash_ops 24986095, 347.6946 ns/op
calc hash time sum 2136625849, hash_ops 24986095, 85.4650 ns/op
time sum 3490824413, ring_ops 62467122, 55.8826 ns/op
hashmap time sum 8631459167, hash_ops 25003238, 345.2137 ns/op
calc hash time sum 2142014696, hash_ops 25003238, 85.6806 ns/op
time sum 3513666352, ring_ops 63018926, 55.7557 ns/op
hashmap time sum 8625283917, hash_ops 25002367, 344.9787 ns/op
calc hash time sum 2112250934, hash_ops 25002367, 84.4900 ns/op
time sum 3475918863, ring_ops 60286988, 57.6562 ns/op
hashmap time sum 8682173056, hash_ops 24997582, 347.3205 ns/op
calc hash time sum 2152704580, hash_ops 24997582, 86.1082 ns/op
time sum 3490859256, ring_ops 62511650, 55.8433 ns/op
hashmap time sum 8613755841, hash_ops 25002152, 344.5206 ns/op
calc hash time sum 2146759108, hash_ops 25002152, 85.8704 ns/op
time sum 3536871008, ring_ops 60251225, 58.7021 ns/op
[GET] total 23.0811 Mops, in 17.3302 s
      per-thread 1.4426 Mops
time sum 3541713474, ring_ops 60016604, 59.0122 ns/op
time sum 3543995767, ring_ops 62419441, 56.7771 ns/op
time sum 3603588374, ring_ops 62746505, 57.4309 ns/op
time sum 3507432967, ring_ops 60149538, 58.3119 ns/op
time sum 3578478729, ring_ops 63757166, 56.1267 ns/op
time sum 3633347570, ring_ops 62349967, 58.2734 ns/op
time sum 3579629080, ring_ops 63170856, 56.6658 ns/op
time sum 3577079034, ring_ops 62354350, 57.3670 ns/op
time sum 3603788244, ring_ops 61793899, 58.3195 ns/op
time sum 3544561452, ring_ops 62027749, 57.1448 ns/op
time sum 3527305886, ring_ops 61794363, 57.0814 ns/op
time sum 3501909006, ring_ops 60335096, 58.0410 ns/op
time sum 3492204106, ring_ops 61950935, 56.3705 ns/op
time sum 3516531812, ring_ops 60523987, 58.1015 ns/op
time sum 3495712938, ring_ops 59374417, 58.8757 ns/op
```