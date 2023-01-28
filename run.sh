#!/bin/bash

# LD_PRELOAD=libjemalloc.so ./build/locktest $1 $2
# LD_PRELOAD=libjemalloc.so ./build/ring_mpsc_rte_test $1 $2
# LD_PRELOAD=libjemalloc.so ./build/ringspsctest $1 $2
# LD_PRELOAD=libjemalloc.so ./build/ringccqtest $1 $2
# LD_PRELOAD=libjemalloc.so ./build/ringrwqtest $1 $2
LD_PRELOAD=libjemalloc.so ./build/ring_time_mpsc_rte $1 $2
# LD_PRELOAD=libjemalloc.so ./build/ring_time_spsc_rte $1 $2