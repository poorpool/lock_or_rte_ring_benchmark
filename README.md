# lock_or_rte_ring_benchmark

谁会赢？

## lock

有 thread_num 个 map，按照 `hash(key) % thread_num` 来决定放哪个 map。

### lock 先读后写 1 线程

每线程 25000000 次写，然后 25000000 次读。map reserve 两倍空间。

对于 key 的锁有 100000 个，对于 map 的锁有 thread_num，即 map_num 个。

```
[PUT] total 1.8475 Mops, in 13.5317 s
      per-thread 1.8475 Mops
[GET] total 1.9823 Mops, in 12.6114 s
      per-thread 1.9823 Mops
```

### lock 先读后写 16 线程

```
[PUT] total 5.4841 Mops, in 72.9388 s
      per-thread 0.3428 Mops
[GET] total 14.1087 Mops, in 28.3513 s
      per-thread 0.8818 Mops
```

## ring

### 1 线程

```
[PUT] total 1.7455 Mops, in 14.3226 s
      per-thread 1.7455 Mops
[GET] total 2.1856 Mops, in 11.4385 s
      per-thread 2.1856 Mops
```

### 16 线程

```
[PUT] total 9.8068 Mops, in 40.7880 s
      per-thread 0.6129 Mops
[GET] total 10.1437 Mops, in 39.4334 s
      per-thread 0.6340 Mops
```
