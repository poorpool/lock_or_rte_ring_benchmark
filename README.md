# lock_or_rte_ring_benchmark

全部开启 O3 优化，先是纯写，再是纯读。全部使用 jemalloc。

哈希表默认预留 每个线程产生的请求数（25000000） 的两倍空间，key 锁默认 100000 个，map 锁和 map 个数相同，ring 的预留大小为 4194304。

## 结论

|方法|写|读|
|---|---|---
|lock|慢|快|
|ring|最快|最快|

最快的方式是**使用 Jemalloc 的 SPSC rte_ring**。

jemalloc 对于 SPSC ring 场景是一个好的优化，其他场景不明显。

moodycamel ring 是一个 header-only 的 c++ 类库，使用很方便、很自然。MPMC 版本叫 concurrentqueue（本测试中只使用到 MPSC），SPSC 版本叫 readerwriterqueue。

SPSC rte_ring 快于 SPSC moodycamel 快于 MPSC moodycamel 快于 MPSC rte_ring

### 操作时间分析

使用 RDTSCP 指令对哈希函数、哈希表、ring 操作进行测试。（RDTSCP 指令本身也有开销，会让性能下降一些，但是相比 std::chrono 已经比较快了）

运行时 CPU 频率 2.9GHz,也就是 3 个cycle 为 1ns.

- MPSC rte_ring 1 线程：哈希函数 50 cycle，哈希表写 360 cycle，ring 40 cycle。
- MPSC rte_ring 16 线程：哈希函数 50 cycle，哈希表写 900 cycle，ring 85 cycle。
- SPSC rte_ring 1 线程：哈希函数 50 cycle，哈希表写 390 cycle，ring 40 cycle。
- SPSC rte_ring 16 线程：哈希函数 50 cycle，哈希表写 800 cycle，ring 85 cycle。

SPSC 和 MPSC 的 ring 的单次操作时间差不多，但是 MPSC 的 poll 次数更多，快翻倍了（我这里使用的还是“poll n 个算 n 次，poll 0 个算 1 次”的计数法，挺反直觉的），所以 MPSC 在 ring 上耗时多。

因为 moodycamel 的 SPSC ring 不支持一次批量出队多个元素，所以只测了 rte_ring。读阶段的哈希表似乎要多 200 个 cycle。

### 为什么这里最快哈希表读只能达到 40Mops，

https://github.com/poorpool/mem-meta-kv-benchmark 这是一个纯净的哈希表读写测试，每个线程只读写自己的哈希表。最快哈希表读能达到 130 Mops

本测试最快的 ring 方法哈希表读 47Mops，这是因为本测试加入了线程和线程之间的数据交互。比如说一个线程的耗时是 15s，这会包括：

1. 7.6s 的哈希表读写开销
2. 1.8s 的 ring 操作开销
3. 0.4s 的哈希函数计算开销
4. 其他开销
      1. 考虑到大概进行了 kOpsPerThread * 10 = 2.5亿次 rdtscp 操作，网上说一次大概 30 cycle，也就是 2.5s 的计时开销
      2. 更新全局的计数器、计算取模、控制流等等其他开销

### 编程建议

- 使用 jemalloc
- 使用 SPSC ring 在线程之间传递数据
- moodycamel 因为它是一个纯粹的类，编程可能更方便，性能则没有特别突出的地方
- 如果有什么数据放在全局共享的数组，将其对齐 cacheline，防止不停 cache 失效
- ring 使用批量 poll 接口

## lock

```cpp
using ReadLock = std::shared_lock<std::shared_mutex>;
using WriteLock = std::unique_lock<std::shared_mutex>;
```

有 thread_num 个 map，按照 `hash(key) % thread_num` 来决定放哪个 map。

执行 write 的时候，先给 `hash(key) % lock_num` 上一个 key 锁，再上对应的 map 锁。

key 锁 100000 个和 10000000 个的性能差距基本没有，主要在 map 锁上面。

每个线程循环做这种事情：先处理 32 个请求，再 poll 32 次 ring 处理别人发过来的请求。Poll 尽可能使用批量 poll 接口（rte_ring_dequeue_burst、concurrentqueue 的 try_dequeue_bulk，readerwriaterqueue 无批量接口）

### lock 先读后写 1 线程

```
lock test, 25000000 write/read op per thread
[PUT] total 3.8458 Mops, in 6.5006 s
      per-thread 3.8458 Mops
[GET] total 5.0112 Mops, in 4.9888 s
      per-thread 5.0112 Mops
```

### lock 先读后写 16 线程

```
lock test, 25000000 write/read op per thread
[PUT] total 8.0801 Mops, in 49.5043 s
      per-thread 0.5050 Mops
[GET] total 17.6064 Mops, in 22.7190 s
      per-thread 1.1004 Mops
```

## ring

和 lock 相比，哈希表放在线程里面（而不是开一个全局数组用线程 id 访问），减少访存次数。

使用 wyhash。

每个线程每完成一次请求，就 `finished_cnt[thread_id]++`。主线程统计 finished_cnt 来判断是不是都运行完了。finished_cnt 要 cacheline 对齐，不然会严重影响性能。

### rte_ring MPSC 1 线程

```
MPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 5.1391 Mops, in 4.8647 s
      per-thread 5.1391 Mops
[GET] total 5.3467 Mops, in 4.6758 s
      per-thread 5.3467 Mops
```

### rte_ring MPSC 16 线程

```
MPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 32.7922 Mops, in 12.1980 s
      per-thread 2.0495 Mops
[GET] total 31.3813 Mops, in 12.7464 s
      per-thread 1.9613 Mops
```

### rte_ring SPSC 1 线程

```
SPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 5.1360 Mops, in 4.8676 s
      per-thread 5.1360 Mops
[GET] total 6.2702 Mops, in 3.9871 s
      per-thread 6.2702 Mops
```

### rte_ring SPSC 16 线程

```
SPSC rte_ring test, 25000000 write/read op per thread
[PUT] total 42.9301 Mops, in 9.3175 s
      per-thread 2.6831 Mops
[GET] total 47.3997 Mops, in 8.4389 s
      per-thread 2.9625 Mops
```

### moodycamel MPSC 1 线程

https://github.com/cameron314/concurrentqueue

```
ring moodycamel MPMC test, 25000000 write/read op per thread
[PUT] total 5.1532 Mops, in 4.8513 s
      per-thread 5.1532 Mops
[GET] total 5.2392 Mops, in 4.7717 s
      per-thread 5.2392 Mops
```

### moodycamel MPSC 16 线程

```
ring moodycamel MPMC test, 25000000 write/read op per thread
[PUT] total 37.8641 Mops, in 10.5641 s
      per-thread 2.3665 Mops
[GET] total 40.0319 Mops, in 9.9920 s
      per-thread 2.5020 Mops
```

### moodycamel SPSC 1 线程

https://github.com/cameron314/readerwriterqueue

```
SPSC readerwriterqueue test, 25000000 write/read op per thread
[PUT] total 5.1628 Mops, in 4.8423 s
      per-thread 5.1628 Mops
[GET] total 5.6211 Mops, in 4.4475 s
      per-thread 5.6211 Mops
```

### moodycamel SPSC 16 线程

```
SPSC readerwriterqueue test, 25000000 write/read op per thread
[PUT] total 39.3403 Mops, in 10.1677 s
      per-thread 2.4588 Mops
[GET] total 43.8345 Mops, in 9.1252 s
      per-thread 2.7397 Mops
```

## 附录 MPSC rte_ring 时间测试

```
MPSC rte_ring rdtsc time test, 25000000 write/read op per thread
#7 hash func cycle 1147211640, op_num 25000000, cycle/op 45.8885
#7 hashmap write cycle 17416718012, op_num 24998637, cycle/op 696.7067
#7 ring cycle 10197718404, op_num 131894206, cycle/op 77.3174
#9 hash func cycle 1163791228, op_num 25000000, cycle/op 46.5516
#9 hashmap write cycle 17481173006, op_num 25000763, cycle/op 699.2256
#9 ring cycle 10136805684, op_num 130519805, cycle/op 77.6649

#2 hash func cycle 1138685350, op_num 25000000, cycle/op 45.5474
#2 hashmap write cycle 17411127114, op_num 25008064, cycle/op 696.2205
#2 ring cycle 10206244622, op_num 131725431, cycle/op 77.4812

#0 hash func cycle 1148582742, op_num 25000000, cycle/op 45.9433
#0 hashmap write cycle 22830273784, op_num 25004498, cycle/op 913.0467
#0 ring cycle 7017344640, op_num 46922829, cycle/op 149.5508

#10 hash func cycle 1163240034, op_num 25000000, cycle/op 46.5296
#10 hashmap write cycle 17442063476, op_num 24993588, cycle/op 697.8615
#10 ring cycle 10237779240, op_num 127154256, cycle/op 80.5146

#6 hash func cycle 1412468316, op_num 25000000, cycle/op 56.4987
#6 hashmap write cycle 17257044576, op_num 24999470, cycle/op 690.2964
#6 ring cycle 10100060506, op_num 131367813, cycle/op 76.8838

#14 hash func cycle 1779481842, op_num 25000000, cycle/op 71.1793
#14 hashmap write cycle 17234512228, op_num 24995050, cycle/op 689.5170
#14 ring cycle 9892590046, op_num 125193095, cycle/op 79.0187

#11 hash func cycle 1162936322, op_num 25000000, cycle/op 46.5175
#11 hashmap write cycle 17416733954, op_num 25003434, cycle/op 696.5737
#11 ring cycle 10140225740, op_num 131542398, cycle/op 77.0871

#4 hash func cycle 1174012132, op_num 25000000, cycle/op 46.9605
#4 hashmap write cycle 17447876712, op_num 25006454, cycle/op 697.7349
#4 ring cycle 10180027018, op_num 129327095, cycle/op 78.7153

#12 hash func cycle 1168349708, op_num 25000000, cycle/op 46.7340
#12 hashmap write cycle 17412449872, op_num 24997876, cycle/op 696.5572
#12 ring cycle 10203156984, op_num 130006079, cycle/op 78.4822

#5 hash func cycle 1138740608, op_num 25000000, cycle/op 45.5496
#5 hashmap write cycle 17440737248, op_num 24996261, cycle/op 697.7338
#5 ring cycle 10323350518, op_num 126001029, cycle/op 81.9307

[PUT] total 25.0006 Mops, in 15.9996 s
      per-thread 1.5625 Mops

#13 hash func cycle 1177225170, op_num 25000000, cycle/op 47.0890
#13 hashmap write cycle 17463509636, op_num 24998186, cycle/op 698.5911
#13 ring cycle 10133841454, op_num 130230338, cycle/op 77.8148

#3 hash func cycle 1150167040, op_num 25000000, cycle/op 46.0067
#3 hashmap write cycle 17428579654, op_num 24995659, cycle/op 697.2643
#3 ring cycle 10219136664, op_num 130144608, cycle/op 78.5214

#15 hash func cycle 1167037734, op_num 25000000, cycle/op 46.6815
#15 hashmap write cycle 17388133224, op_num 24997394, cycle/op 695.5978
#15 ring cycle 10198944856, op_num 130768766, cycle/op 77.9922

#8 hash func cycle 1164627710, op_num 25000000, cycle/op 46.5851
#8 hashmap write cycle 17387691238, op_num 25003892, cycle/op 695.3994
#8 ring cycle 10184031500, op_num 131812876, cycle/op 77.2613

#1 hash func cycle 1167148430, op_num 25000000, cycle/op 46.6859
#1 hashmap write cycle 20728148258, op_num 25000774, cycle/op 829.1003
#1 ring cycle 8215244812, op_num 80039982, cycle/op 102.6393

#7 hash func cycle 1241378234, op_num 25000000, cycle/op 49.6551
#7 hashmap read cycle 25248741332, op_num 24998637, cycle/op 1010.0047
#7 ring cycle 9448911608, op_num 137749249, cycle/op 68.5950

#0 hash func cycle 1127890644, op_num 25000000, cycle/op 45.1156
#0 hashmap read cycle 27762057194, op_num 25004498, cycle/op 1110.2825
#0 ring cycle 8088790120, op_num 99993764, cycle/op 80.8929

#3 hash func cycle 1307505678, op_num 25000000, cycle/op 52.3002
#3 hashmap read cycle 25368036962, op_num 24995659, cycle/op 1014.8977
#3 ring cycle 9376687194, op_num 133652063, cycle/op 70.1574

#12 hash func cycle 1451860484, op_num 25000000, cycle/op 58.0744
#12 hashmap read cycle 26297992238, op_num 24997876, cycle/op 1052.0091
#12 ring cycle 8733943606, op_num 117896467, cycle/op 74.0815

#9 hash func cycle 1390990442, op_num 25000000, cycle/op 55.6396
#9 hashmap read cycle 28347679658, op_num 25000763, cycle/op 1133.8726
#9 ring cycle 7562895904, op_num 87966729, cycle/op 85.9745

#2 hash func cycle 1185843498, op_num 25000000, cycle/op 47.4337
#2 hashmap read cycle 28260449724, op_num 25008064, cycle/op 1130.0535
#2 ring cycle 7703408944, op_num 93590686, cycle/op 82.3096

#11 hash func cycle 1518790318, op_num 25000000, cycle/op 60.7516
#11 hashmap read cycle 26068219706, op_num 25003434, cycle/op 1042.5856
#11 ring cycle 8778808278, op_num 122171875, cycle/op 71.8562

#5 hash func cycle 1051821672, op_num 25000000, cycle/op 42.0729
#5 hashmap read cycle 25443576220, op_num 24996261, cycle/op 1017.8953
#5 ring cycle 9557658130, op_num 133547622, cycle/op 71.5674

#10 hash func cycle 1187297778, op_num 25000000, cycle/op 47.4919
#10 hashmap read cycle 25365785754, op_num 24993588, cycle/op 1014.8917
#10 ring cycle 9490012086, op_num 133800246, cycle/op 70.9267

#14 hash func cycle 1087525170, op_num 25000000, cycle/op 43.5010
#14 hashmap read cycle 25485463696, op_num 24995050, cycle/op 1019.6204
#14 ring cycle 9415225446, op_num 135880281, cycle/op 69.2906

#13 hash func cycle 1430279094, op_num 25000000, cycle/op 57.2112
#13 hashmap read cycle 27006654842, op_num 24998186, cycle/op 1080.3446
#13 ring cycle 8313313214, op_num 108154534, cycle/op 76.8651

#1 hash func cycle 1308130442, op_num 25000000, cycle/op 52.3252
#1 hashmap read cycle 30975732514, op_num 25000774, cycle/op 1238.9909
#1 ring cycle 6124138826, op_num 46904966, cycle/op 130.5648

[GET] total 20.5232 Mops, in 19.4901 s
      per-thread 1.2827 Mops
#15 hash func cycle 1268916604, op_num 25000000, cycle/op 50.7567
#15 hashmap read cycle 25217784192, op_num 24997394, cycle/op 1008.8165
#15 ring cycle 9465609496, op_num 136829638, cycle/op 69.1781

#4 hash func cycle 1224842092, op_num 25000000, cycle/op 48.9937
#4 hashmap read cycle 25465201698, op_num 25006454, cycle/op 1018.3452
#4 ring cycle 9362542288, op_num 133282827, cycle/op 70.2457

#6 hash func cycle 1414628934, op_num 25000000, cycle/op 56.5852
#6 hashmap read cycle 25946664160, op_num 24999470, cycle/op 1037.8886
#6 ring cycle 8889907320, op_num 126341494, cycle/op 70.3641

#8 hash func cycle 1473956524, op_num 25000000, cycle/op 58.9583
#8 hashmap read cycle 30492723322, op_num 25003892, cycle/op 1219.5191
#8 ring cycle 6303432864, op_num 52045705, cycle/op 121.1134
```

### 附录 SPSC rte_ring 时间测试

```
SPSC rte_ring rdtsc time test, 25000000 write/read op per thread
#3 hash func cycle 1136433738, op_num 25000000, cycle/op 45.4573
#3 hashmap write cycle 17491338216, op_num 24995081, cycle/op 699.7912
#3 ring cycle 5349615182, op_num 63529393, cycle/op 84.2069
#11 hash func cycle 1195327294, op_num 25000000, cycle/op 47.8131
#11 hashmap write cycle 17329752404, op_num 25003662, cycle/op 693.0886
#11 ring cycle 5404389484, op_num 64818509, cycle/op 83.3773

#9 hash func cycle 1166220360, op_num 25000000, cycle/op 46.6488
#9 hashmap write cycle 17475797434, op_num 25004670, cycle/op 698.9013
#9 ring cycle 5346068280, op_num 63168679, cycle/op 84.6316

#1 hash func cycle 1175644188, op_num 25000000, cycle/op 47.0258
#1 hashmap write cycle 17458897290, op_num 25005212, cycle/op 698.2103
#1 ring cycle 5348914704, op_num 62071254, cycle/op 86.1738
#0 hash func cycle 1153598206, op_num 25000000, cycle/op 46.1439
#0 hashmap write cycle 17648364382, op_num 25000068, cycle/op 705.9327
#0 ring cycle 5275626086, op_num 58941914, cycle/op 89.5055

[PUT] total 30.7220 Mops, in 13.0200 s
      per-thread 1.9201 Mops
#6 hash func cycle 1164016568, op_num 25000000, cycle/op 46.5607
#6 hashmap write cycle 17264298518, op_num 25001751, cycle/op 690.5236
#6 ring cycle 5469352256, op_num 65593711, cycle/op 83.3823

#4 hash func cycle 1164431808, op_num 25000000, cycle/op 46.5773
#4 hashmap write cycle 17559282442, op_num 25002096, cycle/op 702.3124
#4 ring cycle 5326238952, op_num 60847393, cycle/op 87.5344


#5 hash func cycle 1823068980, op_num 25000000, cycle/op 72.9228
#5 hashmap write cycle 17503339316, op_num 25006546, cycle/op 699.9503
#5 ring cycle 5001155456, op_num 50659548, cycle/op 98.7209

#15 hash func cycle 1184118730, op_num 25000000, cycle/op 47.3647
#15 hashmap write cycle 17353359330, op_num 25000368, cycle/op 694.1242
#15 ring cycle 5407095952, op_num 64183082, cycle/op 84.2449

#13 hash func cycle 1167431474, op_num 25000000, cycle/op 46.6973
#13 hashmap write cycle 17400059136, op_num 24993734, cycle/op 696.1769
#13 ring cycle 5376880618, op_num 64214201, cycle/op 83.7335

#2 hash func cycle 1166175416, op_num 25000000, cycle/op 46.6470
#2 hashmap write cycle 17364436752, op_num 24999511, cycle/op 694.5911
#2 ring cycle 5411762454, op_num 64666827, cycle/op 83.6868

#7 hash func cycle 1143403730, op_num 25000000, cycle/op 45.7361
#7 hashmap write cycle 17372779332, op_num 24994355, cycle/op 695.0681
#7 ring cycle 5420262274, op_num 64419765, cycle/op 84.1397

#12 hash func cycle 1618898012, op_num 25000000, cycle/op 64.7559
#12 hashmap write cycle 17390696584, op_num 24996908, cycle/op 695.7139
#12 ring cycle 5117575022, op_num 58301106, cycle/op 87.7784

#14 hash func cycle 1139539218, op_num 25000000, cycle/op 45.5816
#14 hashmap write cycle 17521017296, op_num 24998895, cycle/op 700.8717
#14 ring cycle 5349228852, op_num 61775022, cycle/op 86.5921

#8 hash func cycle 1162671666, op_num 25000000, cycle/op 46.5069
#8 hashmap write cycle 17339275678, op_num 25001410, cycle/op 693.5319
#8 ring cycle 5415742978, op_num 64817854, cycle/op 83.5533


#10 hash func cycle 1147262184, op_num 25000000, cycle/op 45.8905
#10 hashmap write cycle 17468855886, op_num 24995733, cycle/op 698.8735
#10 ring cycle 5380949006, op_num 62766687, cycle/op 85.7294

#11 hash func cycle 1716468184, op_num 25000000, cycle/op 68.6587
#11 hashmap read cycle 22124665272, op_num 25003662, cycle/op 884.8570
#11 ring cycle 4981998602, op_num 53756974, cycle/op 92.6763

#9 hash func cycle 1492027814, op_num 25000000, cycle/op 59.6811
#9 hashmap read cycle 22335298166, op_num 25004670, cycle/op 893.2451
#9 ring cycle 5013816126, op_num 53493671, cycle/op 93.7273

#0 hash func cycle 1109373648, op_num 25000000, cycle/op 44.3749
#0 hashmap read cycle 22396875864, op_num 25000068, cycle/op 895.8726
#0 ring cycle 5218746148, op_num 55995826, cycle/op 93.1988

#2 hash func cycle 1118143556, op_num 25000000, cycle/op 44.7257
#2 hashmap read cycle 22303938336, op_num 24999511, cycle/op 892.1750
#2 ring cycle 5249317398, op_num 58729054, cycle/op 89.3820

#14 hash func cycle 1106076406, op_num 25000000, cycle/op 44.2431
#14 hashmap read cycle 22385985768, op_num 24998895, cycle/op 895.4790
#14 ring cycle 5241134792, op_num 56904827, cycle/op 92.1035

#12 hash func cycle 1309442438, op_num 25000000, cycle/op 52.3777
#12 hashmap read cycle 22241923692, op_num 24996908, cycle/op 889.7870
#12 ring cycle 5166639336, op_num 57457469, cycle/op 89.9211

#1 hash func cycle 1158412110, op_num 25000000, cycle/op 46.3365
#1 hashmap read cycle 22261283716, op_num 25005212, cycle/op 890.2657
#1 ring cycle 5272035158, op_num 57794071, cycle/op 91.2210

#5 hash func cycle 1124665396, op_num 25000000, cycle/op 44.9866
#5 hashmap read cycle 22259971712, op_num 25006546, cycle/op 890.1658
#5 ring cycle 5332811684, op_num 57186117, cycle/op 93.2536

#6 hash func cycle 1127607926, op_num 25000000, cycle/op 45.1043
#6 hashmap read cycle 22147411622, op_num 25001751, cycle/op 885.8344
#6 ring cycle 5354722914, op_num 60189163, cycle/op 88.9649

#13 hash func cycle 1120548050, op_num 25000000, cycle/op 44.8219
#13 hashmap read cycle 22338860018, op_num 24993734, cycle/op 893.7784
#13 ring cycle 5232771480, op_num 58330693, cycle/op 89.7087

[GET] total 26.3649 Mops, in 15.1717 s
      per-thread 1.6478 Mops
#3 hash func cycle 1097251380, op_num 25000000, cycle/op 43.8901
#3 hashmap read cycle 22433354270, op_num 24995081, cycle/op 897.5108
#3 ring cycle 5184837356, op_num 57490748, cycle/op 90.1856

#4 hash func cycle 1119726758, op_num 25000000, cycle/op 44.7891
#4 hashmap read cycle 22463890922, op_num 25002096, cycle/op 898.4803
#4 ring cycle 5199786716, op_num 55458651, cycle/op 93.7597

#8 hash func cycle 1133493378, op_num 25000000, cycle/op 45.3397
#8 hashmap read cycle 22272342354, op_num 25001410, cycle/op 890.8435
#8 ring cycle 5264839208, op_num 58776768, cycle/op 89.5735

#10 hash func cycle 1739500552, op_num 25000000, cycle/op 69.5800
#10 hashmap read cycle 22213004208, op_num 24995733, cycle/op 888.6718
#10 ring cycle 4978902308, op_num 50336468, cycle/op 98.9124

#7 hash func cycle 1101331728, op_num 25000000, cycle/op 44.0533
#7 hashmap read cycle 22325896090, op_num 24994355, cycle/op 893.2375
#7 ring cycle 5245113294, op_num 58928392, cycle/op 89.0083

#15 hash func cycle 1131834892, op_num 25000000, cycle/op 45.2734
#15 hashmap read cycle 22185822002, op_num 25000368, cycle/op 887.4198
#15 ring cycle 5333135148, op_num 59560998, cycle/op 89.5407
```