# (A) Optimized mode
 OPT ?= -O2 -DNDEBUG
# (B) Debug mode
# OPT ?= -g -O0

INSTALL_PATH=/home/grads/c/celery1124/optW/wisckey/lib/
TARGET=libwisckey.so

HOME=$(shell pwd)
CC=gcc
MKDIR=mkdir
INCLUDES=-I$(HOME)/include
LIBS=-L$(HOME)/libs -Wl,-rpath,$(HOME)/libs -lrt -lpthread -ltbb -lrocksdb
CXXFLAG=-fPIC -w -march=native -std=c++11 $(OPT)

DB_SRCS=$(HOME)/src/db_impl.cc $(HOME)/src/db_iter.cc $(HOME)/src/hash.cc $(HOME)/src/cache/sharded_cache.cc  $(HOME)/src/cache/lru_cache.cc $(HOME)/src/cache/wlfu_cache.cc $(HOME)/src/cache/fifo_cache.cc $(HOME)/src/threadpool.c
SRCS=$(DB_SRCS)

NEWDB_SRCS=$(HOME)/src/newdb/db_impl.cc $(HOME)/src/newdb/db_iter.cc $(HOME)/src/newdb/threadpool.c
NDB_SRCS=$(NEWDB_SRCS)
NEWDB_TARGET=libnewdb.so

all: create_libs wisckey newdb

create_libs:
	$(MKDIR) -p $(HOME)/libs

wisckey:
	$(CC) -shared -o $(HOME)/libs/$(TARGET) $(CXXFLAG) $(SRCS) $(INCLUDES) $(LIBS)

newdb:
	$(CC) -shared -o $(HOME)/libs/$(NEWDB_TARGET) $(CXXFLAG) $(NDB_SRCS) $(INCLUDES) $(LIBS)

install:
	cp $(HOME)/libs/$(TARGET) $(INSTALL_PATH) 

clean:
	rm -rf $(HOME)/libs/$(TARGET) 
