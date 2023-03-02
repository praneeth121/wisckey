/******* newdb *******/
/* db_impl.cc
 * 07/23/2019
 * by Mian Qin
 */
#include "db_impl.h"
#include "db_iter.h"
#include "newdb/compaction_filter.h"
#include "newdb/db.h"
#include "newdb/iterator.h"
#include "rocksdb/convenience.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

namespace newdb {

DBImpl::DBImpl(const Options &options, const std::string &dbname)
    : options_(options), dbname_(dbname), inflight_io_count_(0),
      dbstats_(nullptr), sequence_(0) {

  // start thread pool for async i/o
  pool_ = threadpool_create(options.threadPoolThreadsNum,
                            options.threadPoolQueueDepth, 0, &q_sem_);
  sem_init(&q_sem_, 0, options.threadPoolQueueDepth - 1);

  // create key_map
  key_map_ = NewMemMap();
  // apply db options
  rocksdb::Options valuedbOptions = options.valuedbOptions;
  valuedbOptions.comparator = rocksdb::Uint64Comparator();

  rocksdb::Status status =
      rocksdb::DB::Open(valuedbOptions, dbname + "valuedb", &valuedb_);
  if (status.ok())
    printf("rocksdb open ok\n");
  else {
    std::string status_str = status.ToString();
    printf("rocksdb open error: %s\n", status_str.c_str());
    exit(-1);
  }
}

DBImpl::~DBImpl() {

  // delete databases
  rocksdb::CancelAllBackgroundWork(valuedb_, true);
  delete valuedb_;
  delete key_map_;

  // thread pool
  threadpool_destroy(pool_, 1);
  sem_destroy(&q_sem_);
}

Status DBImpl::Put(const WriteOptions &options, const Slice &key,
                   const Slice &value) {

  RecordTick(options_.statistics.get(), REQ_PUT);

  // insert (log_key, phy_key) into mapping table
  // collect the garbage
  uint64_t seq = get_new_seq();
  phy_key oKey;
  phy_key nKey(seq);

  std::string log_key = key.ToString();

  bool modified = key_map_->readmodifywrite(&log_key, &oKey, &nKey);
  if (modified) {
    {
      std::unique_lock<std::mutex> lock(gc_keys_mutex);
      phy_keys_for_gc.push_back(oKey.phykey);
    }
  }

  // prepare physical key
  char *pkey_str = (char *)malloc(sizeof(uint64_t));
  *((uint64_t *)pkey_str) = seq;
  rocksdb::Slice pkey(pkey_str, sizeof(uint64_t));

  // prepare physical value
  int pval_size = sizeof(uint8_t) + key.size() + value.size();
  char *pval_str = (char *)malloc(pval_size);
  char *pval_ptr = pval_str;
  *((uint8_t *)pval_ptr) = (uint8_t)key.size();
  pval_ptr += sizeof(uint8_t);
  memcpy(pval_ptr, key.data(), key.size());
  pval_ptr += key.size();
  memcpy(pval_ptr, value.data(), value.size());
  rocksdb::Slice pval(pval_str, pval_size);

  // insert into db
  rocksdb::Status s = valuedb_->Put(rocksdb::WriteOptions(), pkey, pval);

  int wi_retry_cnt = 0;
  while (!s.ok()) {
    fprintf(stderr, "[rocks index put] err: %s\n", s.ToString().c_str());
    s = valuedb_->Put(rocksdb::WriteOptions(), pkey, pval);
    if (wi_retry_cnt++ >= 3)
      return Status().IOError(Slice());
  }

  assert(s.ok());

  free(pkey_str);
  free(pval_str);

  return Status();
}

Status DBImpl::Delete(const WriteOptions &options, const Slice &key) {

  RecordTick(options_.statistics.get(), REQ_DEL);
  // This implementation has an issue with delete
  std::string log_key = key.ToString();
  phy_key pKey;
  bool exists = key_map_->lookup(&log_key, &pKey);
  if (exists) {
    key_map_->erase(&log_key);
    {
      std::unique_lock<std::mutex> lock(gc_keys_mutex);
      phy_keys_for_gc.push_back(pKey.phykey);
    }
  }
  return Status();
}

Status DBImpl::Get(const ReadOptions &options, const Slice &key,
                   std::string *value) {
  RecordTick(options_.statistics.get(), REQ_GET);

  // read from keydb
  std::string log_key = key.ToString();
  phy_key pKey;
  bool found = key_map_->lookup(&log_key, &pKey);
  if (!found) {
    RecordTick(options_.statistics.get(), REQ_GET_NOEXIST);
    return Status().NotFound(Slice());
  }

  // prepare physical key
  char *pkey_str = (char *)malloc(sizeof(uint64_t));
  *((uint64_t *)pkey_str) = pKey.phykey;

  // Get Call
  rocksdb::Slice pkey(pkey_str, sizeof(uint64_t));
  std::string pval_str;
  rocksdb::Status s = valuedb_->Get(rocksdb::ReadOptions(), pkey, &pval_str);

  if (s.IsNotFound()) {
    RecordTick(options_.statistics.get(), REQ_GET_NOEXIST);
    return Status().NotFound(Slice());
  }

  int ri_retry_cnt = 0;
  while (!(s.ok())) { // read index retry, rare
    usleep(100);
    s = valuedb_->Get(rocksdb::ReadOptions(), pkey, &pval_str);
    if (ri_retry_cnt++ >= 3)
      return Status().NotFound(Slice());
  }

  const char *ptr = pval_str.data();
  uint8_t key_len = *((uint8_t *)ptr);
  ptr += sizeof(uint8_t);
  ptr += key_len;
  value->append(ptr);
  return Status();
}

void DBImpl::flushVLog() {
  // auto it_ = key_map_->NewMapIterator();
  // printf("flushVlog: Key Iterator");
  // it_->SeekToFirst();
  // while(it_->Valid()) {
  //   std::string key = it_->Key();
  //   std::string val = it_->Value();
  //   printf("%s -> %ld\n", key.data(), *((uint64_t*)val.data()));
  //   it_->Next();
  // }

  // rocksdb::Iterator* it_1 = valuedb_->NewIterator(rocksdb::ReadOptions());
  // it_1->SeekToFirst();
  // while (it_1->Valid()) {
  //   rocksdb::Slice key = it_1->key();
  //   rocksdb::Slice val = it_1->value();
  //   printf("key %ld, value %s\n", *(uint64_t*)key.data(), val.data());
  //   it_1->Next();
  // }
}

void DBImpl::vLogGCWorker(int hash, std::vector<std::string> *ukey_list,
                          std::vector<std::string> *vmeta_list, int idx,
                          int size, int *oldLogFD, int *newLogFD){
    // implement this
};

void DBImpl::vLogGarbageCollect(){
    // TODO
    // valuedb.statistics.get().reportStats();
};

Iterator *DBImpl::NewIterator(const ReadOptions &options) {
  return NewDBIterator(this, options);
}

Status DB::Open(const Options &options, const std::string &dbname, DB **dbptr) {

  *dbptr = NULL;

  DB *db = new DBImpl(options, dbname);
  *dbptr = db;
  return Status(Status::OK());
}

} // namespace newdb
