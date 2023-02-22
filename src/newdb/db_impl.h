/******* newdb *******/
/* db_impl.h
 * 07/23/2019
 * by Mian Qin
 */

#ifndef _db_impl_h_
#define _db_impl_h_

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "newdb/db.h"
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <unordered_map>

#include "threadpool.h"

#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"

#define MAX_THREAD_CNT 64
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define LOG_PARTITION 4

#define WAL_FLUSH_CNT 16

namespace newdb {

class CacheEntry {
public:
  char *val;
  int size;
  CacheEntry() : val(NULL), size(0){};
  CacheEntry(char *v, int s) : size(s) {
    val = (char *)malloc(s);
    memcpy(val, v, size);
  }
  ~CacheEntry() {
    if (val)
      free(val);
  }
};
template <class T> static void DeleteEntry(const Slice & /*key*/, void *value) {
  T *typed_value = reinterpret_cast<T *>(value);
  delete typed_value;
}

class DBImpl : public DB {
  friend class DBIterator;

public:
  DBImpl(const Options &options, const std::string &dbname);
  ~DBImpl();

  // Implementations of the DB interface
  Status Put(const WriteOptions &, const Slice &key, const Slice &value);
  Status Delete(const WriteOptions &, const Slice &key);
  // Status Write(const WriteOptions& options, WriteBatch* updates);
  Status Get(const ReadOptions &options, const Slice &key, std::string *value);
  Iterator *NewIterator(const ReadOptions &);
  void vLogGarbageCollect();

private:
  Options options_;
  std::string dbname_;
  std::mutex gc_keys_mutex;
  // rocksdb for key-offset
public:
  rocksdb::DB *keydb_;
  rocksdb::DB *valuedb_;
  // GC* gc_obj;

private:
  std::shared_ptr<rocksdb::Statistics> dbstats_;
  uint64_t sequence_;
  std::mutex seq_mutex_;
  std::vector<uint64_t> phy_keys_for_gc;

  uint64_t get_new_seq() {
    uint64_t seq;
    {
      std::unique_lock<std::mutex> lock(seq_mutex_);
      seq = sequence_++;
    }
    return seq;
  }

  uint64_t get_curr_seq() { return sequence_; }

  void flushVLog();
  void vLogGCWorker(int hash, std::vector<std::string> *ukey_list,
                    std::vector<std::string> *vmeta_list, int idx, int size,
                    int *oldLogFD, int *newLogFD);
  // rocksdb::DB* get_keydb() { return keydb_;};
  // rocksdb::DB* get_valuedb() { return valuedb_;};

  // thread pool
  threadpool_t *pool_;
  sem_t q_sem_;
  // I/O request conter (read only for now)
  std::atomic<int64_t> inflight_io_count_;
};

class GCMetadata {
public:
  rocksdb::SstFileMetaData sstmeta;
  bool ready_for_gc;
  int level;
  std::vector<uint64_t>::iterator smallest_itr;
  std::vector<uint64_t>::iterator largest_itr;
};

// class GC {
//   public:

//   GC() {

//   }
// }
} // namespace newdb

#endif
