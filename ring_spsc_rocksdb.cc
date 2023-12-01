#include "3rdparty/readerwriterqueue.h"
#include "3rdparty/wyhash.h"
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <pthread.h>
#include <random>
#include <rocksdb/db.h>
#include <rocksdb/utilities/options_util.h>
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

constexpr int kOpsPerThread = 1000000; // 每个线程执行多少次读/写操作
constexpr int kPullNumber = 32;        // 连续 pull 几下

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

using MoodyQueue = moodycamel::ReaderWriterQueue<Request *>;
struct GlobalContext {
  int thread_num;
  int start_core;

  vector<thread> threads;
  vector<PaddingInt> finished_cnt;  // thread_num 个
  vector<vector<MoodyQueue>> rings; // thread_num^2 个
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
  rocksdb::DB *db;
  rocksdb::DBOptions options;
  std::vector<rocksdb::ColumnFamilyDescriptor> loaded_cf_descs;
  rocksdb::ConfigOptions config_options;
  rocksdb::Status s = rocksdb::LoadOptionsFromFile(
      config_options, "rocksdb_options.ini", &options, &loaded_cf_descs);
  if (!s.ok()) {
    std::cout << s.ToString() << std::endl;
    exit(-1);
  }

  options.create_if_missing = true;
  // loaded_cf_descs[0].options.bottommost_compression_opts =

  string db_p = string("./rocks/") + std::to_string(idx);
  std::vector<rocksdb::ColumnFamilyHandle *> handles;
  s = rocksdb::DB::Open(options, db_p, loaded_cf_descs, &handles, &db);
  if (!s.ok()) {
    std::cout << s.ToString() << std::endl;
    exit(-1);
  }

  // test put
  int request_cnt = 0;
  bool ret;
  pthread_barrier_wait(&barrier1);
  // 主线程计时中
  pthread_barrier_wait(&barrier2);
  rocksdb::WriteOptions wops;
  wops.disableWAL = true;
  while (should_thread_run) {
    for (int i = 0; request_cnt < kOpsPerThread && i < kPullNumber; i++) {
      uint64_t key_hash = wyhash(req[request_cnt].key.c_str(),
                                 req[request_cnt].key.length(), 0, _wyp);
      int to_thread = key_hash % g_ctx.thread_num;
      if (to_thread == idx) { // 就是我，不转移了
        s = db->Put(wops, req[request_cnt].key,
                    std::to_string(req[request_cnt].value));
        g_ctx.finished_cnt[idx].val++; // 所有线程 finished_cnt
                                       // 加起来等于总操作数即可结束循环
      } else {
        g_ctx.rings[to_thread][idx].enqueue(&req[request_cnt]);
      }
      request_cnt++;
    }
    Request *r;
    for (int i = 0; i < g_ctx.thread_num; i++) {
      for (int j = 0; j < kPullNumber; j++) {
        ret = g_ctx.rings[idx][i].try_dequeue(r);
        if (ret) {
          s = db->Put(wops, r->key, std::to_string(r->value));
          g_ctx.finished_cnt[idx].val++;
        } else {
          break;
        }
      }
    }
  }
  pthread_barrier_wait(&barrier3);

  int invalid_cnt = 0;
  request_cnt = 0;
  // test get
  pthread_barrier_wait(&barrier1);
  // 主线程计时中
  pthread_barrier_wait(&barrier2);
  string v;
  while (should_thread_run) {
    for (int i = 0; request_cnt < kOpsPerThread && i < kPullNumber; i++) {
      uint64_t key_hash = wyhash(req[request_cnt].key.c_str(),
                                 req[request_cnt].key.length(), 0, _wyp);
      int to_thread = key_hash % g_ctx.thread_num;
      if (to_thread == idx) { // 就是我，不转移了
        s = db->Get(rocksdb::ReadOptions(), req[request_cnt].key, &v);
        if (stoi(v) != req[request_cnt].value) {
          // 因为随机生成 key 的时候可能有冲突，不报错
          req[request_cnt].value = stoi(v);
        }
        g_ctx.finished_cnt[idx].val++; // 所有线程 finished_cnt
                                       // 加起来等于总操作数即可结束循环
      } else {
        g_ctx.rings[to_thread][idx].enqueue(&req[request_cnt]);
      }
      request_cnt++;
    }
    Request *r;
    for (int i = 0; i < g_ctx.thread_num; i++) {
      for (int j = 0; j < kPullNumber; j++) {
        ret = g_ctx.rings[idx][i].try_dequeue(r);
        if (ret) {
          s = db->Get(rocksdb::ReadOptions(), r->key, &v);
          if (stoi(v) != r->value) {
            // 因为随机生成 key 的时候可能有冲突，不报错
            r->value = stoi(v);
          }
          g_ctx.finished_cnt[idx].val++;
        } else {
          break;
        }
      }
    }
  }
  pthread_barrier_wait(&barrier3);

  // test delete
  request_cnt = 0;
  pthread_barrier_wait(&barrier1);
  // 主线程计时中
  pthread_barrier_wait(&barrier2);
  while (should_thread_run) {
    for (int i = 0; request_cnt < kOpsPerThread && i < kPullNumber; i++) {
      uint64_t key_hash = wyhash(req[request_cnt].key.c_str(),
                                 req[request_cnt].key.length(), 0, _wyp);
      int to_thread = key_hash % g_ctx.thread_num;
      if (to_thread == idx) { // 就是我，不转移了
        db->Delete(wops, req[request_cnt].key);
        g_ctx.finished_cnt[idx].val++; // 所有线程 finished_cnt
                                       // 加起来等于总操作数即可结束循环
      } else {
        g_ctx.rings[to_thread][idx].enqueue(&req[request_cnt]);
      }
      request_cnt++;
    }
    Request *r;
    for (int i = 0; i < g_ctx.thread_num; i++) {
      for (int j = 0; j < kPullNumber; j++) {
        ret = g_ctx.rings[idx][i].try_dequeue(r);
        if (ret) {
          db->Delete(wops, r->key);
          g_ctx.finished_cnt[idx].val++;
        } else {
          break;
        }
      }
    }
  }
  pthread_barrier_wait(&barrier3);
  for (auto *handle : handles) {
    delete handle;
  }
  delete db;
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <threads_num> <start_core>\n", argv[0]);
    return 0;
  }
  printf("SPSC rocksdb test, %d write/read op per thread\n", kOpsPerThread);

  g_ctx.thread_num = atoi(argv[1]);
  g_ctx.start_core = atoi(argv[2]);
  g_ctx.finished_cnt.resize(g_ctx.thread_num);
  for (int i = 0; i < g_ctx.thread_num; i++) {
    g_ctx.rings.emplace_back();
    for (int j = 0; j < g_ctx.thread_num; j++) {
      g_ctx.rings[i].emplace_back(4194304);
    }
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
  pthread_barrier_init(&barrier1, nullptr,
                       g_ctx.thread_num + 1); // 为 DELETE 做准备

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
  for (int i = 0; i < g_ctx.thread_num; i++) {
    g_ctx.finished_cnt[i].val = 0;
  }

  // GET 后计时结束
  pthread_barrier_wait(&barrier3);
  pthread_barrier_destroy(&barrier3);
  should_thread_run = true;
  used_time_in_us = GetUs() - start_ts;

  printf("[GET] total %.4f Mops, in %.4f s\n"
         "      per-thread %.4f Mops\n",
         static_cast<double>(kOpsPerThread) * g_ctx.thread_num /
             used_time_in_us,
         static_cast<double>(used_time_in_us) / 1000000,
         static_cast<double>(kOpsPerThread) / used_time_in_us);

  // DELETE
  pthread_barrier_init(&barrier2, nullptr, g_ctx.thread_num + 1);
  pthread_barrier_init(&barrier3, nullptr, g_ctx.thread_num + 1);
  // 计时前同步
  pthread_barrier_wait(&barrier1);
  pthread_barrier_destroy(&barrier1);

  // DELETE 前同步并开始计时
  start_ts = GetUs();
  pthread_barrier_wait(&barrier2);
  pthread_barrier_destroy(&barrier2);

  // DELETE 中……
  while (should_thread_run) {
    int64_t sum = 0;
    for (int i = 0; i < g_ctx.thread_num; i++) {
      sum += g_ctx.finished_cnt[i].val;
    }
    if (sum == static_cast<int64_t>(kOpsPerThread) * g_ctx.thread_num) {
      should_thread_run = false;
    }
  }

  // DELETE 后计时结束
  pthread_barrier_wait(&barrier3);
  pthread_barrier_destroy(&barrier3);
  used_time_in_us = GetUs() - start_ts;

  printf("[DELETE] total %.4f Mops, in %.4f s\n"
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