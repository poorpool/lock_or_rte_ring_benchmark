#!/bin/bash

# LD_PRELOAD=libjemalloc.so ./build/locktest $1 $2
# LD_PRELOAD=libjemalloc.so ./build/ringmpsctest $1 $2
# LD_PRELOAD=libjemalloc.so ./build/ringspsctest $1 $2
# LD_PRELOAD=libjemalloc.so ./build/ringccqtest $1 $2
# LD_PRELOAD=libjemalloc.so ./build/ringrwqtest $1 $2
LD_PRELOAD=libjemalloc.so ./build/ringrwqtimetest $1 $2
# LD_PRELOAD=libjemalloc.so ./build/ringccqtimetest $1 $2