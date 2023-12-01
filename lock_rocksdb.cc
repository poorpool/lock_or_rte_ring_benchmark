#include <ankerl/unordered_dense.h>
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

pthread_barrier_t barrier1, barrier2, barrier3;

rocksdb::DB *db;

enum OP_TYPE { kOpTypeRead = 1, kOpTypeWrite = 2 };

struct Request {
  OP_TYPE type;
  string key;
  int64_t value;
};

struct GlobalContext {
  int thread_num;
  int start_core;

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
  rocksdb::Status s;
  // test put
  pthread_barrier_wait(&barrier1);
  // 主线程计时中
  pthread_barrier_wait(&barrier2);
  rocksdb::WriteOptions wops;
  wops.disableWAL = true;
  for (const auto &x : req) {
    s = db->Put(wops, x.key, std::to_string(x.value));
  }
  pthread_barrier_wait(&barrier3);

  // test get
  pthread_barrier_wait(&barrier1);
  // 主线程计时中
  pthread_barrier_wait(&barrier2);
  std::string v;
  for (auto &x : req) {
    s = db->Get(rocksdb::ReadOptions(), x.key, &v);
    if (stoi(v) != x.value) {
      // 因为随机生成 key 的时候可能有冲突，不报错
      x.value = stoi(v);
    }
  }
  pthread_barrier_wait(&barrier3);

  // test delete
  pthread_barrier_wait(&barrier1);
  // 主线程计时中
  pthread_barrier_wait(&barrier2);
  for (const auto &x : req) {
    db->Delete(wops, x.key);
  }
  pthread_barrier_wait(&barrier3);
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <threads_num> <start_core>\n", argv[0]);
    return 0;
  }
  printf("single rocksdb test, %d write/read op per thread\n", kOpsPerThread);

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

  string db_p = string("./rocks/") + "instance";
  std::vector<rocksdb::ColumnFamilyHandle *> handles;
  s = rocksdb::DB::Open(options, db_p, loaded_cf_descs, &handles, &db);
  if (!s.ok()) {
    std::cout << s.ToString() << std::endl;
    exit(-1);
  }

  g_ctx.thread_num = atoi(argv[1]);
  g_ctx.start_core = atoi(argv[2]);

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
  pthread_barrier_init(&barrier1, nullptr,
                       g_ctx.thread_num + 1); // 为 DELETE 做准备

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
  for (auto *handle : handles) {
    delete handle;
  }
  delete db;
  return 0;
}