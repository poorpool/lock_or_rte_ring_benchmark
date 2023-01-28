#include "3rdparty/ring.h"
#include "3rdparty/wyhash.h"
#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <pthread.h>
#include <random>
#include <shared_mutex>
#include <string>
#include <sys/time.h>
#include <thread>
#include <vector>

using std::string;
using std::thread;
using std::vector;
using ReadLock = std::shared_lock<std::shared_mutex>;
using WriteLock = std::unique_lock<std::shared_mutex>;

constexpr int kOpsPerThread = 25000000; // 每个线程执行多少次读/写操作
constexpr int kPullNumber = 32;         // 连续 pull 几下

pthread_barrier_t barrier1, barrier2, barrier3;

enum OP_TYPE { kOpTypeRead = 1, kOpTypeWrite = 2 };

struct Request {
  OP_TYPE type;
  string key;
  int64_t value;
};

struct __attribute__((aligned(64))) PaddingInt { // cacheline 对齐
  int val;
};

struct GlobalContext {
  int thread_num;
  int start_core;

  vector<thread> threads;
  vector<PaddingInt> finished_cnt; // thread_num 个
  vector<rte_ring *> rings;        // thread_num 个
};
GlobalContext g_ctx;

int64_t GetUs() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_usec + tv.tv_sec * 1000000L;
}

// 注意：生成的 Key 有重复
void GenerateWriteRequests(vector<Request> &kvs) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dis(1, kOpsPerThread);
  char key_buffer[105];

  for (int i = 0; i < kOpsPerThread; i++) {
    sprintf(key_buffer, "file.mdtest.%d.%d", dis(gen), dis(gen));
    string key = key_buffer;
    int32_t value = dis(gen);

    kvs.push_back({OP_TYPE::kOpTypeWrite, key, value});
  }
}

bool should_thread_run;
void threadFunc(int idx) {
  if (g_ctx.start_core != -1) {
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset); // 初始化CPU集合，将 cpuset 置为空
    CPU_SET(idx + g_ctx.start_core, &cpuset); // 将本进程绑定到 CPU 上

    // 设置线程的 CPU 亲和性
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
      printf("Set CPU affinity failed\n");
      exit(-1);
    }
  }

  std::hash<string> hasher;
  vector<Request> req;
  req.reserve(kOpsPerThread);
  GenerateWriteRequests(req);
  void *deque_requests[kPullNumber];
  ankerl::unordered_dense::map<string, uint64_t>
      hash_map; // 使用线程本地的变量而不是 g_ctx
                // 中的一个哈希表数组，减少访存次数
  hash_map.reserve(kOpsPerThread * 2);

  // test put
  int request_cnt = 0;
  int ret;
  pthread_barrier_wait(&barrier1);
  // 主线程计时中
  pthread_barrier_wait(&barrier2);
  while (should_thread_run) {
    for (int i = 0; request_cnt < kOpsPerThread && i < kPullNumber; i++) {

      // uint64_t key_hash = hasher(req[request_cnt].key);
      uint64_t key_hash =
          wyhash(req[request_cnt].key.c_str(), req[request_cnt].key.length(), 0,
                 _wyp); // 使用快速的 wyhash（还行的优化）
      int to_thread = key_hash % g_ctx.thread_num;
      if (to_thread == idx) { // 就是我，不转移了
        hash_map[req[request_cnt].key] = req[request_cnt].value;
        g_ctx.finished_cnt[idx].val++; // 所有线程 finished_cnt
                                       // 加起来等于总操作数即可结束循环
      } else {
        while ((ret = rte_ring_enqueue(g_ctx.rings[to_thread],
                                       &req[request_cnt])) != 0)
          ;
      }
      request_cnt++;
    }

    unsigned int n = rte_ring_dequeue_burst(g_ctx.rings[idx], deque_requests,
                                            kPullNumber, nullptr);
    for (int i = 0; i < n; i++) {
      auto *r = static_cast<Request *>(deque_requests[i]);
      hash_map[r->key] = r->value;
      g_ctx.finished_cnt[idx].val++;
    }
  }
  pthread_barrier_wait(&barrier3);

  int invalid_cnt = 0;
  request_cnt = 0;
  // test get
  pthread_barrier_wait(&barrier1);
  // 主线程计时中
  pthread_barrier_wait(&barrier2);
  while (should_thread_run) {
    for (int i = 0; request_cnt < kOpsPerThread && i < kPullNumber; i++) {

      // uint64_t key_hash = hasher(req[request_cnt].key);
      uint64_t key_hash = wyhash(req[request_cnt].key.c_str(),
                                 req[request_cnt].key.length(), 0, _wyp);
      int to_thread = key_hash % g_ctx.thread_num;
      if (to_thread == idx) { // 就是我，不转移了
        int value = hash_map[req[request_cnt].key];
        if (value == 0) {
          invalid_cnt++;
        }
        g_ctx.finished_cnt[idx].val++; // 所有线程 finished_cnt
                                       // 加起来等于总操作数即可结束循环
      } else {
        while ((ret = rte_ring_enqueue(g_ctx.rings[to_thread],
                                       &req[request_cnt])) != 0)
          ;
      }
      request_cnt++;
    }
    unsigned int n = rte_ring_dequeue_burst(g_ctx.rings[idx], deque_requests,
                                            kPullNumber, nullptr);
    for (int i = 0; i < n; i++) {
      auto *r = static_cast<Request *>(deque_requests[i]);
      int value = hash_map[r->key];
      if (value == 0) {
        invalid_cnt++;
      }
      g_ctx.finished_cnt[idx].val++;
    }
  }
  pthread_barrier_wait(&barrier3);

  if (invalid_cnt != 0) {
    printf("ERR %d: invalid_cnt %d", idx, invalid_cnt);
  }
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <threads_num> <start_core>\n", argv[0]);
    return 0;
  }
  printf("MPSC rte_ring test, %d write/read op per thread\n", kOpsPerThread);

  g_ctx.thread_num = atoi(argv[1]);
  g_ctx.start_core = atoi(argv[2]);
  g_ctx.finished_cnt.resize(g_ctx.thread_num);
  for (int i = 0; i < g_ctx.thread_num; i++) {
    g_ctx.rings.push_back(
        rte_ring_create(4194304, RING_F_SC_DEQ)); // 单消费者多生产者
  }

  for (int i = 0; i < g_ctx.thread_num; i++) {
    g_ctx.threads.emplace_back(threadFunc, i);
  }
  if (g_ctx.start_core != -1) {
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset); // 初始化CPU集合，将 cpuset 置为空
    CPU_SET(g_ctx.thread_num + g_ctx.start_core,
            &cpuset); // 将本进程绑定到 CPU 上

    // 设置线程的 CPU 亲和性
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
      printf("Set CPU affinity failed\n");
      exit(-1);
    }
  }

  // PUT
  should_thread_run = true;
  pthread_barrier_init(&barrier1, nullptr, g_ctx.thread_num + 1);
  pthread_barrier_init(&barrier2, nullptr, g_ctx.thread_num + 1);
  pthread_barrier_init(&barrier3, nullptr, g_ctx.thread_num + 1);
  // 计时前同步
  pthread_barrier_wait(&barrier1);
  pthread_barrier_destroy(&barrier1);
  pthread_barrier_init(&barrier1, nullptr,
                       g_ctx.thread_num + 1); // 为 GET 做准备

  // PUT 前同步并开始计时
  int64_t start_ts = GetUs();
  pthread_barrier_wait(&barrier2);
  pthread_barrier_destroy(&barrier2);

  // PUT 中……
  while (should_thread_run) {
    int64_t sum = 0;
    for (int i = 0; i < g_ctx.thread_num; i++) {
      sum += g_ctx.finished_cnt[i].val;
    }
    if (sum == static_cast<int64_t>(kOpsPerThread) * g_ctx.thread_num) {
      should_thread_run = false;
    }
  }
  for (int i = 0; i < g_ctx.thread_num; i++) {
    g_ctx.finished_cnt[i].val = 0;
  }

  // PUT 后计时结束
  pthread_barrier_wait(&barrier3);
  pthread_barrier_destroy(&barrier3);
  int64_t used_time_in_us = GetUs() - start_ts;

  should_thread_run = true;
  printf("[PUT] total %.4f Mops, in %.4f s\n"
         "      per-thread %.4f Mops\n",
         static_cast<double>(kOpsPerThread) * g_ctx.thread_num /
             used_time_in_us,
         static_cast<double>(used_time_in_us) / 1000000,
         static_cast<double>(kOpsPerThread) / used_time_in_us);

  // GET
  pthread_barrier_init(&barrier2, nullptr, g_ctx.thread_num + 1);
  pthread_barrier_init(&barrier3, nullptr, g_ctx.thread_num + 1);
  // 计时前同步
  pthread_barrier_wait(&barrier1);
  pthread_barrier_destroy(&barrier1);

  // GET 前同步并开始计时
  start_ts = GetUs();
  pthread_barrier_wait(&barrier2);
  pthread_barrier_destroy(&barrier2);

  // GET 中……
  while (should_thread_run) {
    int64_t sum = 0;
    for (int i = 0; i < g_ctx.thread_num; i++) {
      sum += g_ctx.finished_cnt[i].val;
    }
    if (sum == static_cast<int64_t>(kOpsPerThread) * g_ctx.thread_num) {
      should_thread_run = false;
    }
  }

  // GET 后计时结束
  pthread_barrier_wait(&barrier3);
  pthread_barrier_destroy(&barrier3);
  used_time_in_us = GetUs() - start_ts;

  printf("[GET] total %.4f Mops, in %.4f s\n"
         "      per-thread %.4f Mops\n",
         static_cast<double>(kOpsPerThread) * g_ctx.thread_num /
             used_time_in_us,
         static_cast<double>(used_time_in_us) / 1000000,
         static_cast<double>(kOpsPerThread) / used_time_in_us);

  for (int i = 0; i < g_ctx.thread_num; i++) {
    g_ctx.threads[i].join();
  }
  return 0;
}