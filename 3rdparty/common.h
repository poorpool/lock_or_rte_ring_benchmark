#pragma once
// NOLINTBEGIN
#include <emmintrin.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h> //offsetof
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

using std::string;
using std::vector;

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2 * !!(condition)]))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define UNUSED(x) ((void)(x))

#define dump_on(x)                                                             \
  {                                                                            \
    if (unlikely(x)) {                                                         \
      *((int *)0) = 1;                                                         \
    }                                                                          \
  }
#define setoffval(ptr, off, type) (*(type *)((char *)(ptr) + (int)(off)))

#define barrier() asm volatile("" : : : "memory")

#if defined(__x86_64__)
#define rmb() asm volatile("lfence" ::: "memory")
#define wmb() asm volatile("sfence" ::: "memory")
#define mb() asm volatile("mfence" ::: "memory")

#define cpu_relax() asm volatile("pause\n" : : : "memory")
#define cpu_pause() __mm_pause()

#elif defined(__arm64__) || defined(__aarch64__)

#define rmb() asm volatile("dmb ishld" : : : "memory")
#define wmb() asm volatile("dmb ishst" : : : "memory")
#define mb() asm volatile("dmb ish" : : : "memory")
#define cpu_relax() asm volatile("yield" ::: "memory")
#else
#error "unsupported arch"
#endif

#define atomic_xadd(P, V) __sync_fetch_and_add((P), (V))
#define cmpxchg(P, O, N) __sync_bool_compare_and_swap((P), (O), (N))
#define atomic_inc(P) __sync_add_and_fetch((P), 1)
#define atomic_dec(P) __sync_add_and_fetch((P), -1)
#define atomic_add(P, V) __sync_add_and_fetch((P), (V))
#define atomic_set_bit(P, V) __sync_or_and_fetch((P), 1 << (V))
#define atomic_clear_bit(P, V) __sync_and_and_fetch((P), ~(1 << (V)))
#define atomic_xor(P, V) __sync_xor_and_fetch((P), (V))

#define ALIGN_DOWN(val, align)                                                 \
  (decltype(val))((val) & (~((decltype(val))((align)-1))))
#define ALIGN_UP(val, align)                                                   \
  ALIGN_DOWN(((val) + ((decltype(val))(align)-1)), align)
#define POWEROF2(x) ((((x)-1) & (x)) == 0)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define FN_LEN 256
#define URL_LEN 128
#define CL_SFT 6
#define CL_SIZE (1 << CL_SFT)
#define CL_MASK (CL_SIZE - 1)
#define CACHE_ALIGNED __attribute__((aligned(64)))

#define ALWAYS_INLINE inline __attribute__((always_inline))

// #define FLASH_INLINE
#define FLASH_INLINE inline

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// DO NOT USE PAGE_MASK
#define PAGE_UMASK (PAGE_SIZE - 1)

#define KB (1UL << 10)
#define MB (KB << 10)
#define GB (MB << 10)

#define RESET "\033[0m"
#define BLACK "\033[30m"              /* Black */
#define RED "\033[31m"                /* Red */
#define GREEN "\033[32m"              /* Green */
#define YELLOW "\033[33m"             /* Yellow */
#define BLUE "\033[34m"               /* Blue */
#define MAGENTA "\033[35m"            /* Magenta */
#define CYAN "\033[36m"               /* Cyan */
#define WHITE "\033[37m"              /* White */
#define BOLDBLACK "\033[1m\033[30m"   /* Bold Black */
#define BOLDRED "\033[1m\033[31m"     /* Bold Red */
#define BOLDGREEN "\033[1m\033[32m"   /* Bold Green */
#define BOLDYELLOW "\033[1m\033[33m"  /* Bold Yellow */
#define BOLDBLUE "\033[1m\033[34m"    /* Bold Blue */
#define BOLDMAGENTA "\033[1m\033[35m" /* Bold Magenta */
#define BOLDCYAN "\033[1m\033[36m"    /* Bold Cyan */
#define BOLDWHITE "\033[1m\033[37m"   /* Bold White */

#define p_info(fmt, ...) printf("DHMS info: " fmt RESET "\n", ##__VA_ARGS__)
#define p_err(fmt, ...)                                                        \
  fprintf(stderr, "ERROR %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

void print_trace(void) {
  void *array[20];
  size_t size;
  char **strings;
  size_t i;
  size = backtrace(array, 20);
  strings = backtrace_symbols(array, size);
  printf("Obtained %zd stack frames.\n", size);
  for (i = 0; i < size; i++)
    printf("%s\n", strings[i]);
  free(strings);
}

#define p_assert(b, fmt, ...)                                                  \
  {                                                                            \
    if (unlikely(!(b))) {                                                      \
      fflush(stdout);                                                          \
      fprintf(stderr, "%s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);   \
      print_trace();                                                           \
      getchar();                                                               \
      *(long *)0 = 0;                                                          \
      exit(100);                                                               \
    }                                                                          \
  }

#define FLASH_ALIGNED(x) __attribute__((aligned(x)))
#define FLASH_PACKED __attribute__((__packed__))

const auto str_hash = std::hash<string>();

static inline void *safe_alloc(size_t siz, bool clear) {
  void *mem;
  mem = malloc(siz);
  dump_on(mem == NULL);
  if (clear)
    memset(mem, 0, siz);
  return mem;
}

static inline unsigned safe_rand(unsigned &seed) {
  return seed = ((seed * 19260817) + 231);
}

static inline void *safe_align(size_t siz, size_t ali, bool clear) {
  void *mem;
  mem = memalign(ali, siz);
  dump_on(mem == NULL);
  if (clear)
    memset(mem, 0, siz);
  return mem;
}

static inline int64_t min2power(int64_t v) {
  int64_t res = 1;
  while (res < v)
    res <<= 1;
  return res;
}

static inline uint64_t hash_long(uint64_t val) {
  uint64_t hash = val;

  /*  Sigh, gcc can't optimise this alone like it does for 32 bits. */
  uint64_t n = hash;
  n <<= 18;
  hash -= n;
  n <<= 33;
  hash -= n;
  n <<= 3;
  hash += n;
  n <<= 3;
  hash -= n;
  n <<= 4;
  hash += n;
  n <<= 2;
  hash += n;

  /* High bits are more random, so use them. */
  return hash;
}

static inline uint64_t cstr_hash(const char *str) {
  const uint64_t hash_mult = 2654435387U;
  uint64_t h = 0;
  while (*str)
    h = (h + (uint64_t)*str++) * hash_mult;
  return h;
}

static inline string get_ymdhs() {
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char s[64];
  sprintf(s, "%04d-%02d-%02d-%02d:%02d:%02d", 1900 + tm.tm_year, tm.tm_mon + 1,
          tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return string(s);
}

#ifdef FLASH_DEBUG
#define LOG(fmt, ...)                                                          \
  printf("LOG %s:	%d : " fmt RESET "\n", __func__, __LINE__,             \
         ##__VA_ARGS__)
#else
#define LOG(fmt, ...) ((void)fmt)
#endif

// #define FLASH_YIELD sched_yield()
#define FLASH_YIELD ((void)(0))
// NOLINTEND