/******* newdb *******/
/* db_iter.cc
 * 08/06/2019
 * by Mian Qin
 */
#include "db_iter.h"
#include "db_impl.h"
#include "newdb/iterator.h"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace newdb {

typedef struct {
  int fd;
  void *buf;
  size_t count;
  off_t offset;

  void (*callback)(void *);
  void *argument;
} dev_io_context;

static void io_task(void *arg) {
  dev_io_context *ctx = (dev_io_context *)arg;
  int ret;

  pread(ctx->fd, ctx->buf, ctx->count, ctx->offset);
  if (ctx->callback != NULL)
    ctx->callback(ctx->argument);
  delete ctx;
}

// Monitor for async I/O
class Monitor {
public:
  std::mutex mtx_;
  std::condition_variable cv_;
  bool ready_;
  Monitor() : ready_(false) {}
  ~Monitor() {}
  void reset() { ready_ = false; };
  void notify() {
    std::unique_lock<std::mutex> lck(mtx_);
    ready_ = true;
    cv_.notify_one();
  }
  void notifyAll() {
    std::unique_lock<std::mutex> lck(mtx_);
    ready_ = true;
    cv_.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> lck(mtx_);
    while (!ready_)
      cv_.wait(lck);
  }
};

class Prefetch_context {
public:
  std::atomic<int> prefetch_cnt;
  int prefetch_num;
  Monitor *mon;
  Prefetch_context(int prefetch_num_, Monitor *mon_)
      : prefetch_cnt(0), prefetch_num(prefetch_num_), mon(mon_) {}
};

void on_prefetch_complete(void *args) {
  Prefetch_context *prefetch_ctx = (Prefetch_context *)args;
  if (prefetch_ctx->prefetch_cnt.fetch_add(1) == prefetch_ctx->prefetch_num - 1)
    prefetch_ctx->mon->notify();
}

class DBIterator : public Iterator {
public:
  DBIterator(DBImpl *db, const ReadOptions &options);
  ~DBIterator();

  bool Valid() const { return valid_; }
  void SeekToFirst();
  void SeekToLast() { /* NOT IMPLEMENT */
  }
  void Seek(const Slice &target);
  void Next();
  void Prev();
  Slice key() const;
  Slice value();

private:
  DBImpl *db_;

  const ReadOptions &options_;
  rocksdb::Slice s_upper_key_;
  MapIterator *it_;
  std::string value_;
  bool valid_;

  // upper key hint
  Slice upper_key_;

  // for value prefetch
  bool prefetch_ena_;
  std::string *key_queue_;
  std::string *pkey_queue_;
  std::string *val_queue_;
  bool *valid_queue_;
  int prefetch_depth_;
  int queue_cur_;

  bool async_pread(int fd, void *buf, size_t count, off_t offset,
                   void (*callback)(void *), void *args);
  void prefetch_value(std::vector<int> fd_list,
                      std::vector<std::string> &pkey_list,
                      std::vector<std::string> &val_list);
};

bool DBIterator::async_pread(int fd, void *buf, size_t count, off_t offset,
                             void (*callback)(void *), void *args) {
  dev_io_context *dev_ctx =
      new dev_io_context{fd, buf, count, offset, callback, args};
  sem_wait(&(db_->q_sem_));
  if (threadpool_add(db_->pool_, &io_task, dev_ctx, 0) < 0) {
    printf("async_pread pool_add error, fd %d, offset %llu\n", fd, offset);
    exit(1);
  }
  return true;
}

void DBIterator::prefetch_value(std::vector<int> fd_list,
                                std::vector<std::string> &pkey_list,
                                std::vector<std::string> &val_list) {
  // TODO:
}

DBIterator::DBIterator(DBImpl *db, const ReadOptions &options)
    : db_(db), options_(options), valid_(false), queue_cur_(0) {

  rocksdb::ReadOptions rdopts;
  if (options_.upper_key != NULL) {
    upper_key_ = *(options_.upper_key);
    rocksdb::Slice r_upper_key(upper_key_.data(), upper_key_.size());
    s_upper_key_ = r_upper_key;
    rdopts.iterate_upper_bound = &s_upper_key_;
  }

  // whether to prefetch?
  prefetch_ena_ = db_->options_.prefetchEnabled &&
                  (db_->inflight_io_count_.load(std::memory_order_relaxed) <
                   db_->options_.prefetchReqThres);
  prefetch_depth_ = 1;
  if (prefetch_ena_) {
    int prefetch_depth = db_->options_.prefetchDepth;
    key_queue_ = new std::string[prefetch_depth];
    pkey_queue_ = new std::string[prefetch_depth];
    val_queue_ = new std::string[prefetch_depth];
    valid_queue_ = new bool[prefetch_depth];
    for (int i = 0; i < prefetch_depth; i++) {
      valid_queue_[i] = false;
      val_queue_[i].clear();
    }
  }

  // not an optimal one currently
  it_ = db_->key_map_->NewMapIterator();
}

DBIterator::~DBIterator() {
  delete it_;

  if (prefetch_ena_) {
    delete[] key_queue_;
    delete[] pkey_queue_;

    delete[] val_queue_;
    delete[] valid_queue_;
  }
}

void DBIterator::SeekToFirst() {
  it_->SeekToFirst();
  valid_ = it_->Valid();
}

void DBIterator::Seek(const Slice &target) {
  RecordTick(db_->options_.statistics.get(), REQ_SEEK);
  std::string target_key = target.ToString();
  it_->Seek(target_key);
  valid_ = it_->Valid();
}

void DBIterator::Prev() { /* NOT FULLY IMPLEMENT, Suppose ONLY CALL BEFORE next
                           */
  assert(valid_);
}

void DBIterator::Next() {
  RecordTick(db_->options_.statistics.get(), REQ_NEXT);
  it_->Next();
  valid_ = it_->Valid();
}

Slice DBIterator::key() const {
  std::string log_key = it_->Key();

  char *ptr = (char *)malloc(log_key.size());
  memcpy(ptr, log_key.data(), log_key.size());
  Slice it_key(ptr, log_key.size());
  return it_key;
}

Slice DBIterator::value() {
  assert(valid_);
  Slice curr_key = key();
  std::string pval;
  std::string pKey = it_->Value();
  rocksdb::Status s =
      db_->valuedb_->Get(rocksdb::ReadOptions(), rocksdb::Slice(pKey), &pval);
  const char *ptr = pval.data();
  uint8_t key_len = *((uint8_t *)ptr);
  ptr += sizeof(uint8_t);
  ptr += key_len;
  std::string value;
  value.append(ptr);

  char *v_ptr = (char *)malloc(value.size());
  memcpy(v_ptr, value.data(), value.size());
  return Slice(v_ptr, value.size());
}

Iterator *NewDBIterator(DBImpl *db, const ReadOptions &options) {
  return new DBIterator(db, options);
}

} // end namespace newdb
