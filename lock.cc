#include <ankerl/unordered_dense.h>
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
constexpr int kGlobalLocksNum = 100000; // 全局为了完成同步有多少对于 key 的锁

pthread_barrier_t barrier1, barrier2, barrier3;

enum OP_TYPE { kOpTypeRead = 1, kOpTypeWrite = 2 };

struct Request {
  OP_TYPE type;
  string key;
  int64_t value;
};

struct GlobalContext {
  int thread_num;
  int start_core;

  vector<ankerl::unordered_dense::map<string, int64_t>> maps; // thread_num 个
  std::shared_mutex
      key_locks[kGlobalLocksNum]; // 对这些互斥量构造 ReadLock/WriteLock 来使用
  vector<std::shared_mutex>
      map_locks; // ankerl 哈希表不支持并发写，所以手动套锁

  vector<thread> threads;
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

  // test put
  pthread_barrier_wait(&barrier1);
  // 主线程计时中
  pthread_barrier_wait(&barrier2);
  for (int i = 0; i < kOpsPerThread; i++) {
    int key_hash = hasher(req[i].key);
    if (key_hash < 0) {
      key_hash = -key_hash;
    }
    WriteLock lock_a(g_ctx.key_locks[key_hash % kGlobalLocksNum]);
    WriteLock lock_b(g_ctx.map_locks[key_hash % g_ctx.thread_num]);
    g_ctx.maps[key_hash % g_ctx.thread_num][req[i].key] = req[i].value;
    lock_b.unlock();
    lock_a.unlock();
  }
  pthread_barrier_wait(&barrier3);

  int cnt = 0;
  // test get
  pthread_barrier_wait(&barrier1);
  // 主线程计时中
  pthread_barrier_wait(&barrier2);
  for (int i = 0; i < kOpsPerThread; i++) {
    int key_hash = hasher(req[i].key);
    if (key_hash < 0) {
      key_hash = -key_hash;
    }
    ReadLock lock_a(g_ctx.key_locks[key_hash % kGlobalLocksNum]);
    ReadLock lock_b(g_ctx.map_locks[key_hash % g_ctx.thread_num]);
    int value = g_ctx.maps[key_hash % g_ctx.thread_num][req[i].key];
    if (value > 0) {
      cnt++;
    }
    lock_b.unlock();
    lock_a.unlock();
  }
  pthread_barrier_wait(&barrier3);

  if (cnt != kOpsPerThread) {
    printf("ERR %d: cnt %d != kOpsPerThread %d\n", idx, cnt, kOpsPerThread);
  }
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <threads_num> <start_core>\n", argv[0]);
    return 0;
  }

  g_ctx.thread_num = atoi(argv[1]);
  g_ctx.start_core = atoi(argv[2]);
  g_ctx.maps.resize(g_ctx.thread_num);
  g_ctx.map_locks = vector<std::shared_mutex>(g_ctx.thread_num);
  for (int i = 0; i < g_ctx.thread_num; i++) {
    g_ctx.maps[i].reserve(kOpsPerThread * 2);
  }

  for (int i = 0; i < g_ctx.thread_num; i++) {
    g_ctx.threads.emplace_back(threadFunc, i);
  }

  // PUT
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

  // PUT 后计时结束
  pthread_barrier_wait(&barrier3);
  pthread_barrier_destroy(&barrier3);
  int64_t used_time_in_us = GetUs() - start_ts;

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