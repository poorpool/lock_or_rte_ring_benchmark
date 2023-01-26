#pragma once
// NOLINTBEGIN
#include "common.h"
#include <stdint.h>

#define RTE_TAILQ_RING_NAME "RTE_RING"

/** enqueue/dequeue behavior types */
enum rte_ring_queue_behavior {
  /** Enq/Deq a fixed number of items from a ring */
  RTE_RING_QUEUE_FIXED = 0,
  /** Enq/Deq as many items as possible from ring */
  RTE_RING_QUEUE_VARIABLE
};

/** prod/cons sync types */
enum rte_ring_sync_type {
  RTE_RING_SYNC_MT,     /**< multi-thread safe (default mode) */
  RTE_RING_SYNC_ST,     /**< single thread only */
  RTE_RING_SYNC_MT_RTS, /**< multi-thread relaxed tail sync */
  RTE_RING_SYNC_MT_HTS, /**< multi-thread head/tail sync */
};

/**
 * structures to hold a pair of head/tail values and other metadata.
 * Depending on sync_type format of that structure might be different,
 * but offset for *sync_type* and *tail* values should remain the same.
 */
struct rte_ring_headtail {
  volatile uint32_t head; /**< prod/consumer head. */
  volatile uint32_t tail; /**< prod/consumer tail. */
  union {
    /** sync type of prod/cons */
    enum rte_ring_sync_type sync_type;
    /** deprecated -  True if single prod/cons */
    uint32_t single;
  };
};

union __rte_ring_rts_poscnt {
  /** raw 8B value to read/write *cnt* and *pos* as one atomic op */
  uint64_t raw FLASH_ALIGNED(8);
  struct {
    uint32_t cnt; /**< head/tail reference counter */
    uint32_t pos; /**< head/tail position */
  } val;
};

struct rte_ring_rts_headtail {
  volatile union __rte_ring_rts_poscnt tail;
  enum rte_ring_sync_type sync_type; /**< sync type of prod/cons */
  uint32_t htd_max; /**< max allowed distance between head/tail */
  volatile union __rte_ring_rts_poscnt head;
};

union __rte_ring_hts_pos {
  /** raw 8B value to read/write *head* and *tail* as one atomic op */
  uint64_t raw FLASH_ALIGNED(8);
  struct {
    uint32_t head; /**< head position */
    uint32_t tail; /**< tail position */
  } pos;
};

struct rte_ring_hts_headtail {
  volatile union __rte_ring_hts_pos ht;
  enum rte_ring_sync_type sync_type; /**< sync type of prod/cons */
};

/**
 * An RTE ring structure.
 *
 * The producer and the consumer have a head and a tail index. The particularity
 * of these index is that they are not between 0 and size(ring). These indexes
 * are between 0 and 2^32, and we mask their value when we access the ring[]
 * field. Thanks to this assumption, we can do subtractions between 2 index
 * values in a modulo-32bit base: that's why the overflow of the indexes is not
 * a problem.
 */
struct rte_ring {
  char pad CACHE_ALIGNED;
  /**< Name of the ring. */
  int flags; /**< Flags supplied at creation. */
  const struct rte_memzone *memzone;
  /**< Memzone, if any, containing the rte_ring */
  uint32_t size;     /**< Size of ring. */
  uint32_t mask;     /**< Mask (size-1) of ring. */
  uint32_t capacity; /**< Usable size of ring */

  char pad0 CACHE_ALIGNED; /**< empty cache line */

  /** Ring producer status. */
  union {
    struct rte_ring_headtail prod;
    struct rte_ring_hts_headtail hts_prod;
    struct rte_ring_rts_headtail rts_prod;
  } CACHE_ALIGNED;

  char pad1 CACHE_ALIGNED; /**< empty cache line */

  /** Ring consumer status. */
  union {
    struct rte_ring_headtail cons;
    struct rte_ring_hts_headtail hts_cons;
    struct rte_ring_rts_headtail rts_cons;
  } CACHE_ALIGNED;

  char pad2 CACHE_ALIGNED; /**< empty cache line */
};

#define RING_F_SP_ENQ 0x0001 /**< The default enqueue is "single-producer". */
#define RING_F_SC_DEQ 0x0002 /**< The default dequeue is "single-consumer". */
/**
 * Ring is to hold exactly requested number of entries.
 * Without this flag set, the ring size requested must be a power of 2, and the
 * usable space will be that size - 1. With the flag, the requested size will
 * be rounded up to the next power of two, but the usable space will be exactly
 * that requested. Worst case, if a power-of-2 size is requested, half the
 * ring space will be wasted.
 */
#define RING_F_EXACT_SZ 0x0004
#define RTE_RING_SZ_MASK (0x7fffffffU) /**< Ring size mask */

#define RING_F_MP_RTS_ENQ 0x0008 /**< The default enqueue is "MP RTS". */
#define RING_F_MC_RTS_DEQ 0x0010 /**< The default dequeue is "MC RTS". */

#define RING_F_MP_HTS_ENQ 0x0020 /**< The default enqueue is "MP HTS". */
#define RING_F_MC_HTS_DEQ 0x0040 /**< The default dequeue is "MC HTS". */

#define RING_F_MASK                                                            \
  (RING_F_SP_ENQ | RING_F_SC_DEQ | RING_F_EXACT_SZ | RING_F_MP_RTS_ENQ |       \
   RING_F_MC_RTS_DEQ | RING_F_MP_HTS_ENQ | RING_F_MC_HTS_DEQ)

#define HTD_MAX_DEF 8

static inline uint32_t rte_combine32ms1b(uint32_t x) {
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;

  return x;
}

static inline uint32_t rte_align32pow2(uint32_t x) {
  x--;
  x = rte_combine32ms1b(x);

  return x + 1;
}

/* return the size of memory occupied by a ring */
static inline int64_t rte_ring_get_memsize_elem(unsigned int count) {
  int64_t sz;

  /* count must be a power of 2 */
  if ((!POWEROF2(count)) || (count > RTE_RING_SZ_MASK)) {
    p_err("Requested number of elements is invalid, must be power of 2, and "
          "not exceed %u\n",
          RTE_RING_SZ_MASK);
    return -EINVAL;
  }

  sz = sizeof(struct rte_ring) + count * sizeof(void *);
  sz = ALIGN_UP(sz, CL_SIZE);
  return sz;
}

/*
 * helper function, calculates sync_type values for prod and cons
 * based on input flags. Returns zero at success or negative
 * errno value otherwise.
 */
static inline int get_sync_type(uint32_t flags,
                                enum rte_ring_sync_type *prod_st,
                                enum rte_ring_sync_type *cons_st) {
  static const uint32_t prod_st_flags =
      (RING_F_SP_ENQ | RING_F_MP_RTS_ENQ | RING_F_MP_HTS_ENQ);
  static const uint32_t cons_st_flags =
      (RING_F_SC_DEQ | RING_F_MC_RTS_DEQ | RING_F_MC_HTS_DEQ);

  switch (flags & prod_st_flags) {
  case 0:
    *prod_st = RTE_RING_SYNC_MT;
    break;
  case RING_F_SP_ENQ:
    *prod_st = RTE_RING_SYNC_ST;
    break;
  case RING_F_MP_RTS_ENQ:
    *prod_st = RTE_RING_SYNC_MT_RTS;
    break;
  case RING_F_MP_HTS_ENQ:
    *prod_st = RTE_RING_SYNC_MT_HTS;
    break;
  default:
    return -EINVAL;
  }

  switch (flags & cons_st_flags) {
  case 0:
    *cons_st = RTE_RING_SYNC_MT;
    break;
  case RING_F_SC_DEQ:
    *cons_st = RTE_RING_SYNC_ST;
    break;
  case RING_F_MC_RTS_DEQ:
    *cons_st = RTE_RING_SYNC_MT_RTS;
    break;
  case RING_F_MC_HTS_DEQ:
    *cons_st = RTE_RING_SYNC_MT_HTS;
    break;
  default:
    return -EINVAL;
  }

  return 0;
}

static inline int rte_ring_set_prod_htd_max(struct rte_ring *r, uint32_t v) {
  if (r->prod.sync_type != RTE_RING_SYNC_MT_RTS)
    return -ENOTSUP;

  r->rts_prod.htd_max = v;
  return 0;
}

static inline int rte_ring_set_cons_htd_max(struct rte_ring *r, uint32_t v) {
  if (r->cons.sync_type != RTE_RING_SYNC_MT_RTS)
    return -ENOTSUP;

  r->rts_cons.htd_max = v;
  return 0;
}

static inline int rte_ring_init(struct rte_ring *r, unsigned int count,
                                unsigned int flags) {
  int ret;

  /* compilation-time checks */
  static_assert((sizeof(struct rte_ring) & CL_MASK) == 0, "");
  static_assert((offsetof(struct rte_ring, cons) & CL_MASK) == 0, "");
  static_assert((offsetof(struct rte_ring, prod) & CL_MASK) == 0, "");

  static_assert(offsetof(struct rte_ring_headtail, sync_type) ==
                    offsetof(struct rte_ring_hts_headtail, sync_type),
                "");
  static_assert(offsetof(struct rte_ring_headtail, tail) ==
                    offsetof(struct rte_ring_hts_headtail, ht.pos.tail),
                "");

  static_assert(offsetof(struct rte_ring_headtail, sync_type) ==
                    offsetof(struct rte_ring_rts_headtail, sync_type),
                "");
  static_assert(offsetof(struct rte_ring_headtail, tail) ==
                    offsetof(struct rte_ring_rts_headtail, tail.val.pos),
                "");

  /* future proof flags, only allow supported values */
  if (flags & ~RING_F_MASK) {
    p_err("Unsupported flags requested %#x\n", flags);
    return -EINVAL;
  }

  /* init the ring structure */
  memset(r, 0, sizeof(*r));
  r->flags = flags;
  ret = get_sync_type(flags, &r->prod.sync_type, &r->cons.sync_type);
  if (ret != 0)
    return ret;

  if (flags & RING_F_EXACT_SZ) {
    r->size = rte_align32pow2(count + 1);
    r->mask = r->size - 1;
    r->capacity = count;
  } else {
    if ((!POWEROF2(count)) || (count > RTE_RING_SZ_MASK)) {
      p_err("Requested size is invalid, must be power of 2, and not exceed the "
            "size limit %u\n",
            RTE_RING_SZ_MASK);
      return -EINVAL;
    }
    r->size = count;
    r->mask = count - 1;
    r->capacity = r->mask;
  }

  /* set default values for head-tail distance */
  if (flags & RING_F_MP_RTS_ENQ)
    rte_ring_set_prod_htd_max(r, r->capacity / HTD_MAX_DEF);
  if (flags & RING_F_MC_RTS_DEQ)
    rte_ring_set_cons_htd_max(r, r->capacity / HTD_MAX_DEF);

  return 0;
}

/* create the ring */
static inline struct rte_ring *rte_ring_create(uint32_t count, uint32_t flags) {
  int64_t ring_size;
  struct rte_ring *r;
  count = rte_align32pow2(count + 1);
  ring_size = rte_ring_get_memsize_elem(count);
  if (ring_size < 0)
    return NULL;
  r = (struct rte_ring *)memalign(CL_SIZE, ring_size);
  if (r) {
    rte_ring_init(r, count, flags);
  } else {
    p_err("Cannot reserve memory\n");
  }
  return r;
}

static ALWAYS_INLINE void rte_wait_until_equal_32(volatile uint32_t *addr,
                                                  uint32_t expected,
                                                  int memorder) {
  p_assert(memorder == __ATOMIC_ACQUIRE || memorder == __ATOMIC_RELAXED, "");

  while (__atomic_load_n(addr, memorder) != expected)
    _mm_pause();
}

static ALWAYS_INLINE void
__rte_ring_update_tail(struct rte_ring_headtail *ht, uint32_t old_val,
                       uint32_t new_val, uint32_t single, uint32_t enqueue) {
  UNUSED(enqueue);

  /*
   * If there are other enqueues/dequeues in progress that preceded us,
   * we need to wait for them to complete
   */
  if (!single)
    rte_wait_until_equal_32(&ht->tail, old_val, __ATOMIC_RELAXED);

  __atomic_store_n(&ht->tail, new_val, __ATOMIC_RELEASE);
}

static ALWAYS_INLINE unsigned int
__rte_ring_move_prod_head(struct rte_ring *r, unsigned int is_sp,
                          unsigned int n, enum rte_ring_queue_behavior behavior,
                          uint32_t *old_head, uint32_t *new_head,
                          uint32_t *free_entries) {
  const uint32_t capacity = r->capacity;
  uint32_t cons_tail;
  unsigned int max = n;
  int success;

  *old_head = __atomic_load_n(&r->prod.head, __ATOMIC_RELAXED);
  do {
    /* Reset n to the initial burst count */
    n = max;

    /* Ensure the head is read before tail */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    /* load-acquire synchronize with store-release of ht->tail
     * in update_tail.
     */
    cons_tail = __atomic_load_n(&r->cons.tail, __ATOMIC_ACQUIRE);

    /* The subtraction is done between two unsigned 32bits value
     * (the result is always modulo 32 bits even if we have
     * *old_head > cons_tail). So 'free_entries' is always between 0
     * and capacity (which is < size).
     */
    *free_entries = (capacity + cons_tail - *old_head);

    /* check that we have enough room in ring */
    if (unlikely(n > *free_entries))
      n = (behavior == RTE_RING_QUEUE_FIXED) ? 0 : *free_entries;

    if (n == 0)
      return 0;

    *new_head = *old_head + n;
    if (is_sp)
      r->prod.head = *new_head, success = 1;
    else
      /* on failure, *old_head is updated */
      success =
          __atomic_compare_exchange_n(&r->prod.head, old_head, *new_head, 0,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  } while (unlikely(success == 0));
  return n;
}

static ALWAYS_INLINE unsigned int
__rte_ring_move_cons_head(struct rte_ring *r, int is_sc, unsigned int n,
                          enum rte_ring_queue_behavior behavior,
                          uint32_t *old_head, uint32_t *new_head,
                          uint32_t *entries) {
  unsigned int max = n;
  uint32_t prod_tail;
  int success;

  /* move cons.head atomically */
  *old_head = __atomic_load_n(&r->cons.head, __ATOMIC_RELAXED);
  do {
    /* Restore n as it may change every loop */
    n = max;

    /* Ensure the head is read before tail */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    /* this load-acquire synchronize with store-release of ht->tail
     * in update_tail.
     */
    prod_tail = __atomic_load_n(&r->prod.tail, __ATOMIC_ACQUIRE);

    /* The subtraction is done between two unsigned 32bits value
     * (the result is always modulo 32 bits even if we have
     * cons_head > prod_tail). So 'entries' is always between 0
     * and size(ring)-1.
     */
    *entries = (prod_tail - *old_head);

    /* Set the actual entries for dequeue */
    if (n > *entries)
      n = (behavior == RTE_RING_QUEUE_FIXED) ? 0 : *entries;

    if (unlikely(n == 0))
      return 0;

    *new_head = *old_head + n;
    if (is_sc)
      r->cons.head = *new_head, success = 1;
    else
      /* on failure, *old_head will be updated */
      success =
          __atomic_compare_exchange_n(&r->cons.head, old_head, *new_head, 0,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  } while (unlikely(success == 0));
  return n;
}

static ALWAYS_INLINE void __rte_ring_enqueue_elems_64(struct rte_ring *r,
                                                      uint32_t prod_head,
                                                      const void *obj_table,
                                                      uint32_t n) {
  unsigned int i;
  const uint32_t size = r->size;
  uint32_t idx = prod_head & r->mask;
  uint64_t *ring = (uint64_t *)&r[1];
  const uint64_t *obj = (const uint64_t *)obj_table;
  if (likely(idx + n < size)) {
    for (i = 0; i < (n & ~0x3); i += 4, idx += 4) {
      ring[idx] = obj[i];
      ring[idx + 1] = obj[i + 1];
      ring[idx + 2] = obj[i + 2];
      ring[idx + 3] = obj[i + 3];
    }
    switch (n & 0x3) {
    case 3:
      ring[idx++] = obj[i++]; /* fallthrough */
    case 2:
      ring[idx++] = obj[i++]; /* fallthrough */
    case 1:
      ring[idx++] = obj[i++];
    }
  } else {
    for (i = 0; idx < size; i++, idx++)
      ring[idx] = obj[i];
    /* Start at the beginning */
    for (idx = 0; i < n; i++, idx++)
      ring[idx] = obj[i];
  }
}

static ALWAYS_INLINE void
__rte_ring_enqueue_elems(struct rte_ring *r, uint32_t prod_head,
                         const void *obj_table, uint32_t esize, uint32_t num) {
  /* 8B and 16B copies implemented individually to retain
   * the current performance.
   */
  if (esize == 8)
    __rte_ring_enqueue_elems_64(r, prod_head, obj_table, num);
  else
    p_assert(0, "");
}

static ALWAYS_INLINE unsigned int
__rte_ring_do_enqueue_elem(struct rte_ring *r, const void *obj_table,
                           unsigned int esize, unsigned int n,
                           enum rte_ring_queue_behavior behavior,
                           unsigned int is_sp, unsigned int *free_space) {
  uint32_t prod_head, prod_next;
  uint32_t free_entries;

  n = __rte_ring_move_prod_head(r, is_sp, n, behavior, &prod_head, &prod_next,
                                &free_entries);
  if (n == 0)
    goto end;

  __rte_ring_enqueue_elems(r, prod_head, obj_table, esize, n);

  __rte_ring_update_tail(&r->prod, prod_head, prod_next, is_sp, 1);
end:
  if (free_space != NULL)
    *free_space = free_entries - n;
  return n;
}

static ALWAYS_INLINE unsigned int
rte_ring_mp_enqueue_bulk_elem(struct rte_ring *r, const void *obj_table,
                              unsigned int esize, unsigned int n,
                              unsigned int *free_space) {
  return __rte_ring_do_enqueue_elem(r, obj_table, esize, n,
                                    RTE_RING_QUEUE_FIXED, RTE_RING_SYNC_MT,
                                    free_space);
}

static ALWAYS_INLINE unsigned int
rte_ring_sp_enqueue_bulk_elem(struct rte_ring *r, const void *obj_table,
                              unsigned int esize, unsigned int n,
                              unsigned int *free_space) {
  return __rte_ring_do_enqueue_elem(r, obj_table, esize, n,
                                    RTE_RING_QUEUE_FIXED, RTE_RING_SYNC_ST,
                                    free_space);
}

static ALWAYS_INLINE void
__rte_ring_hts_head_wait(const struct rte_ring_hts_headtail *ht,
                         union __rte_ring_hts_pos *p) {
  while (p->pos.head != p->pos.tail) {
    _mm_pause();
    p->raw = __atomic_load_n(&ht->ht.raw, __ATOMIC_ACQUIRE);
  }
}

static ALWAYS_INLINE unsigned int
__rte_ring_hts_move_prod_head(struct rte_ring *r, unsigned int num,
                              enum rte_ring_queue_behavior behavior,
                              uint32_t *old_head, uint32_t *free_entries) {
  uint32_t n;
  union __rte_ring_hts_pos np, op;

  const uint32_t capacity = r->capacity;

  op.raw = __atomic_load_n(&r->hts_prod.ht.raw, __ATOMIC_ACQUIRE);

  do {
    /* Reset n to the initial burst count */
    n = num;

    /*
     * wait for tail to be equal to head,
     * make sure that we read prod head/tail *before*
     * reading cons tail.
     */
    __rte_ring_hts_head_wait(&r->hts_prod, &op);

    /*
     *  The subtraction is done between two unsigned 32bits value
     * (the result is always modulo 32 bits even if we have
     * *old_head > cons_tail). So 'free_entries' is always between 0
     * and capacity (which is < size).
     */
    *free_entries = capacity + r->cons.tail - op.pos.head;

    /* check that we have enough room in ring */
    if (unlikely(n > *free_entries))
      n = (behavior == RTE_RING_QUEUE_FIXED) ? 0 : *free_entries;

    if (n == 0)
      break;

    np.pos.tail = op.pos.tail;
    np.pos.head = op.pos.head + n;

    /*
     * this CAS(ACQUIRE, ACQUIRE) serves as a hoist barrier to prevent:
     *  - OOO reads of cons tail value
     *  - OOO copy of elems from the ring
     */
  } while (__atomic_compare_exchange_n(&r->hts_prod.ht.raw, &op.raw, np.raw, 0,
                                       __ATOMIC_ACQUIRE,
                                       __ATOMIC_ACQUIRE) == 0);

  *old_head = op.pos.head;
  return n;
}

static ALWAYS_INLINE void
__rte_ring_hts_update_tail(struct rte_ring_hts_headtail *ht, uint32_t old_tail,
                           uint32_t num, uint32_t enqueue) {
  uint32_t tail;

  UNUSED(enqueue);

  tail = old_tail + num;
  __atomic_store_n(&ht->ht.pos.tail, tail, __ATOMIC_RELEASE);
}

static ALWAYS_INLINE unsigned int __rte_ring_do_hts_enqueue_elem(
    struct rte_ring *r, const void *obj_table, uint32_t esize, uint32_t n,
    enum rte_ring_queue_behavior behavior, uint32_t *free_space) {
  uint32_t free, head;

  n = __rte_ring_hts_move_prod_head(r, n, behavior, &head, &free);

  if (n != 0) {
    __rte_ring_enqueue_elems(r, head, obj_table, esize, n);
    __rte_ring_hts_update_tail(&r->hts_prod, head, n, 1);
  }

  if (free_space != NULL)
    *free_space = free - n;
  return n;
}

static ALWAYS_INLINE unsigned int
rte_ring_mp_hts_enqueue_bulk_elem(struct rte_ring *r, const void *obj_table,
                                  unsigned int esize, unsigned int n,
                                  unsigned int *free_space) {
  return __rte_ring_do_hts_enqueue_elem(r, obj_table, esize, n,
                                        RTE_RING_QUEUE_FIXED, free_space);
}

static ALWAYS_INLINE unsigned int
rte_ring_enqueue_bulk_elem(struct rte_ring *r, const void *obj_table,
                           unsigned int esize, unsigned int n,
                           unsigned int *free_space) {
  switch (r->prod.sync_type) {
  case RTE_RING_SYNC_MT:
    return rte_ring_mp_enqueue_bulk_elem(r, obj_table, esize, n, free_space);
  case RTE_RING_SYNC_ST:
    return rte_ring_sp_enqueue_bulk_elem(r, obj_table, esize, n, free_space);
  case RTE_RING_SYNC_MT_HTS:
    return rte_ring_mp_hts_enqueue_bulk_elem(r, obj_table, esize, n,
                                             free_space);
  }

  /* valid ring should never reach this point */
  p_assert(0, "");
  if (free_space != NULL)
    *free_space = 0;
  return 0;
}

static ALWAYS_INLINE int rte_ring_enqueue_elem(struct rte_ring *r, void *obj,
                                               unsigned int esize) {
  return rte_ring_enqueue_bulk_elem(r, obj, esize, 1, NULL) ? 0 : -ENOBUFS;
}

static ALWAYS_INLINE int rte_ring_enqueue(struct rte_ring *r, void *obj) {
  return rte_ring_enqueue_elem(r, &obj, sizeof(void *));
}

static ALWAYS_INLINE unsigned int
rte_ring_mp_enqueue_burst_elem(struct rte_ring *r, const void *obj_table,
                               unsigned int esize, unsigned int n,
                               unsigned int *free_space) {
  return __rte_ring_do_enqueue_elem(r, obj_table, esize, n,
                                    RTE_RING_QUEUE_VARIABLE, RTE_RING_SYNC_MT,
                                    free_space);
}

static ALWAYS_INLINE unsigned int
rte_ring_sp_enqueue_burst_elem(struct rte_ring *r, const void *obj_table,
                               unsigned int esize, unsigned int n,
                               unsigned int *free_space) {
  return __rte_ring_do_enqueue_elem(r, obj_table, esize, n,
                                    RTE_RING_QUEUE_VARIABLE, RTE_RING_SYNC_ST,
                                    free_space);
}

static ALWAYS_INLINE unsigned int
rte_ring_mp_hts_enqueue_burst_elem(struct rte_ring *r, const void *obj_table,
                                   unsigned int esize, unsigned int n,
                                   unsigned int *free_space) {
  return __rte_ring_do_hts_enqueue_elem(r, obj_table, esize, n,
                                        RTE_RING_QUEUE_VARIABLE, free_space);
}

static ALWAYS_INLINE unsigned int
rte_ring_enqueue_burst_elem(struct rte_ring *r, const void *obj_table,
                            unsigned int esize, unsigned int n,
                            unsigned int *free_space) {
  switch (r->prod.sync_type) {
  case RTE_RING_SYNC_MT:
    return rte_ring_mp_enqueue_burst_elem(r, obj_table, esize, n, free_space);
  case RTE_RING_SYNC_ST:
    return rte_ring_sp_enqueue_burst_elem(r, obj_table, esize, n, free_space);
  case RTE_RING_SYNC_MT_HTS:
    return rte_ring_mp_hts_enqueue_burst_elem(r, obj_table, esize, n,
                                              free_space);
  }

  /* valid ring should never reach this point */
  p_assert(0, "");
  if (free_space != NULL)
    *free_space = 0;
  return 0;
}

static ALWAYS_INLINE unsigned int
rte_ring_enqueue_burst(struct rte_ring *r, void *const *obj_table,
                       unsigned int n, unsigned int *free_space) {
  return rte_ring_enqueue_burst_elem(r, obj_table, sizeof(void *), n,
                                     free_space);
}

static ALWAYS_INLINE void __rte_ring_dequeue_elems_64(struct rte_ring *r,
                                                      uint32_t prod_head,
                                                      void *obj_table,
                                                      uint32_t n) {
  unsigned int i;
  const uint32_t size = r->size;
  uint32_t idx = prod_head & r->mask;
  uint64_t *ring = (uint64_t *)&r[1];
  uint64_t *obj = (uint64_t *)obj_table;
  if (likely(idx + n < size)) {
    for (i = 0; i < (n & ~0x3); i += 4, idx += 4) {
      obj[i] = ring[idx];
      obj[i + 1] = ring[idx + 1];
      obj[i + 2] = ring[idx + 2];
      obj[i + 3] = ring[idx + 3];
    }
    switch (n & 0x3) {
    case 3:
      obj[i++] = ring[idx++]; /* fallthrough */
    case 2:
      obj[i++] = ring[idx++]; /* fallthrough */
    case 1:
      obj[i++] = ring[idx++]; /* fallthrough */
    }
  } else {
    for (i = 0; idx < size; i++, idx++)
      obj[i] = ring[idx];
    /* Start at the beginning */
    for (idx = 0; i < n; i++, idx++)
      obj[i] = ring[idx];
  }
}

static ALWAYS_INLINE void
__rte_ring_dequeue_elems(struct rte_ring *r, uint32_t cons_head,
                         void *obj_table, uint32_t esize, uint32_t num) {
  /* 8B and 16B copies implemented individually to retain
   * the current performance.
   */
  if (esize == 8)
    __rte_ring_dequeue_elems_64(r, cons_head, obj_table, num);
  else
    p_assert(0, "");
}

static ALWAYS_INLINE unsigned int
__rte_ring_do_dequeue_elem(struct rte_ring *r, void *obj_table,
                           unsigned int esize, unsigned int n,
                           enum rte_ring_queue_behavior behavior,
                           unsigned int is_sc, unsigned int *available) {
  uint32_t cons_head, cons_next;
  uint32_t entries;

  n = __rte_ring_move_cons_head(r, (int)is_sc, n, behavior, &cons_head,
                                &cons_next, &entries);
  if (n == 0)
    goto end;

  __rte_ring_dequeue_elems(r, cons_head, obj_table, esize, n);

  __rte_ring_update_tail(&r->cons, cons_head, cons_next, is_sc, 0);

end:
  if (available != NULL)
    *available = entries - n;
  return n;
}

static ALWAYS_INLINE unsigned int
rte_ring_sc_dequeue_bulk_elem(struct rte_ring *r, void *obj_table,
                              unsigned int esize, unsigned int n,
                              unsigned int *available) {
  return __rte_ring_do_dequeue_elem(r, obj_table, esize, n,
                                    RTE_RING_QUEUE_FIXED, RTE_RING_SYNC_ST,
                                    available);
}

static ALWAYS_INLINE unsigned int
rte_ring_mc_dequeue_bulk_elem(struct rte_ring *r, void *obj_table,
                              unsigned int esize, unsigned int n,
                              unsigned int *available) {
  return __rte_ring_do_dequeue_elem(r, obj_table, esize, n,
                                    RTE_RING_QUEUE_FIXED, RTE_RING_SYNC_MT,
                                    available);
}

static ALWAYS_INLINE unsigned int
__rte_ring_hts_move_cons_head(struct rte_ring *r, unsigned int num,
                              enum rte_ring_queue_behavior behavior,
                              uint32_t *old_head, uint32_t *entries) {
  uint32_t n;
  union __rte_ring_hts_pos np, op;

  op.raw = __atomic_load_n(&r->hts_cons.ht.raw, __ATOMIC_ACQUIRE);

  /* move cons.head atomically */
  do {
    /* Restore n as it may change every loop */
    n = num;

    /*
     * wait for tail to be equal to head,
     * make sure that we read cons head/tail *before*
     * reading prod tail.
     */
    __rte_ring_hts_head_wait(&r->hts_cons, &op);

    /* The subtraction is done between two unsigned 32bits value
     * (the result is always modulo 32 bits even if we have
     * cons_head > prod_tail). So 'entries' is always between 0
     * and size(ring)-1.
     */
    *entries = r->prod.tail - op.pos.head;

    /* Set the actual entries for dequeue */
    if (n > *entries)
      n = (behavior == RTE_RING_QUEUE_FIXED) ? 0 : *entries;

    if (unlikely(n == 0))
      break;

    np.pos.tail = op.pos.tail;
    np.pos.head = op.pos.head + n;

    /*
     * this CAS(ACQUIRE, ACQUIRE) serves as a hoist barrier to prevent:
     *  - OOO reads of prod tail value
     *  - OOO copy of elems from the ring
     */
  } while (__atomic_compare_exchange_n(&r->hts_cons.ht.raw, &op.raw, np.raw, 0,
                                       __ATOMIC_ACQUIRE,
                                       __ATOMIC_ACQUIRE) == 0);

  *old_head = op.pos.head;
  return n;
}

static ALWAYS_INLINE unsigned int __rte_ring_do_hts_dequeue_elem(
    struct rte_ring *r, void *obj_table, uint32_t esize, uint32_t n,
    enum rte_ring_queue_behavior behavior, uint32_t *available) {
  uint32_t entries, head;

  n = __rte_ring_hts_move_cons_head(r, n, behavior, &head, &entries);

  if (n != 0) {
    __rte_ring_dequeue_elems(r, head, obj_table, esize, n);
    __rte_ring_hts_update_tail(&r->hts_cons, head, n, 0);
  }

  if (available != NULL)
    *available = entries - n;
  return n;
}

static ALWAYS_INLINE unsigned int
rte_ring_mc_hts_dequeue_bulk_elem(struct rte_ring *r, void *obj_table,
                                  unsigned int esize, unsigned int n,
                                  unsigned int *available) {
  return __rte_ring_do_hts_dequeue_elem(r, obj_table, esize, n,
                                        RTE_RING_QUEUE_FIXED, available);
}

static ALWAYS_INLINE unsigned int
rte_ring_dequeue_bulk_elem(struct rte_ring *r, void *obj_table,
                           unsigned int esize, unsigned int n,
                           unsigned int *available) {
  switch (r->cons.sync_type) {
  case RTE_RING_SYNC_MT:
    return rte_ring_mc_dequeue_bulk_elem(r, obj_table, esize, n, available);
  case RTE_RING_SYNC_ST:
    return rte_ring_sc_dequeue_bulk_elem(r, obj_table, esize, n, available);
  case RTE_RING_SYNC_MT_HTS:
    return rte_ring_mc_hts_dequeue_bulk_elem(r, obj_table, esize, n, available);
  }

  /* valid ring should never reach this point */
  p_assert(0, "");
  if (available != NULL)
    *available = 0;
  return 0;
}

static ALWAYS_INLINE int rte_ring_dequeue_elem(struct rte_ring *r, void *obj_p,
                                               unsigned int esize) {
  return rte_ring_dequeue_bulk_elem(r, obj_p, esize, 1, NULL) ? 0 : -ENOENT;
}

static ALWAYS_INLINE int rte_ring_dequeue(struct rte_ring *r, void **obj_p) {
  return rte_ring_dequeue_elem(r, obj_p, sizeof(void *));
}

static ALWAYS_INLINE unsigned int
rte_ring_mc_dequeue_burst_elem(struct rte_ring *r, void *obj_table,
                               unsigned int esize, unsigned int n,
                               unsigned int *available) {
  return __rte_ring_do_dequeue_elem(r, obj_table, esize, n,
                                    RTE_RING_QUEUE_VARIABLE, RTE_RING_SYNC_MT,
                                    available);
}

static ALWAYS_INLINE unsigned int
rte_ring_sc_dequeue_burst_elem(struct rte_ring *r, void *obj_table,
                               unsigned int esize, unsigned int n,
                               unsigned int *available) {
  return __rte_ring_do_dequeue_elem(r, obj_table, esize, n,
                                    RTE_RING_QUEUE_VARIABLE, RTE_RING_SYNC_ST,
                                    available);
}

static ALWAYS_INLINE unsigned int
rte_ring_mc_hts_dequeue_burst_elem(struct rte_ring *r, void *obj_table,
                                   unsigned int esize, unsigned int n,
                                   unsigned int *available) {
  return __rte_ring_do_hts_dequeue_elem(r, obj_table, esize, n,
                                        RTE_RING_QUEUE_VARIABLE, available);
}

static ALWAYS_INLINE unsigned int
rte_ring_dequeue_burst_elem(struct rte_ring *r, void *obj_table,
                            unsigned int esize, unsigned int n,
                            unsigned int *available) {
  switch (r->cons.sync_type) {
  case RTE_RING_SYNC_MT:
    return rte_ring_mc_dequeue_burst_elem(r, obj_table, esize, n, available);
  case RTE_RING_SYNC_ST:
    return rte_ring_sc_dequeue_burst_elem(r, obj_table, esize, n, available);
  case RTE_RING_SYNC_MT_HTS:
    return rte_ring_mc_hts_dequeue_burst_elem(r, obj_table, esize, n,
                                              available);
  }

  /* valid ring should never reach this point */
  p_assert(0, "");
  if (available != NULL)
    *available = 0;
  return 0;
}

static ALWAYS_INLINE unsigned int
rte_ring_dequeue_burst(struct rte_ring *r, void **obj_table, unsigned int n,
                       unsigned int *available) {
  return rte_ring_dequeue_burst_elem(r, obj_table, sizeof(void *), n,
                                     available);
}
// NOLINTEND