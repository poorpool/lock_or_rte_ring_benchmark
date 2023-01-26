#include "3rdparty/ring.h"
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

using std::vector;
rte_ring *ring;
vector<int> vec;

void threadProducer() {
  for (int i = 0; i < 40000; i++) {
    vec.push_back(rand());
    // printf("produce %d\n", vec.back());
  }
  int cnt = 0;
  while (cnt < 40000) {
    int ret = rte_ring_enqueue(ring, &vec[cnt]);
    if (ret != 0) {
      continue;
    }
    printf("%d\n", cnt);
    cnt++;
  }
}
void threadConsumer() {
  int cnt = 0;
  while (cnt < 40000) {
    void *msg;
    int ret;
    while ((ret = rte_ring_dequeue(ring, &msg)) != 0) {
      // printf("ret %d\n", ret);
    }
    cnt++;
    // printf("receive %d\n", *static_cast<int *>(msg));
    printf("read %d\n", cnt);
  }
}
int main() {
  ring = rte_ring_create(1024, RING_F_SP_ENQ | RING_F_SC_DEQ);
  printf("created\n");
  std::thread th1(threadProducer);
  std::thread th2(threadConsumer);
  th1.join();
  th2.join();
  delete ring;
  return 0;
}