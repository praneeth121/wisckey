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

  // apply db options
  rocksdb::Status status =
      rocksdb::DB::Open(options.keydbOptions, dbname + "keydb", &keydb_);
  if (status.ok())
    printf("rocksdb open ok\n");
  else {
    std::string status_str = status.ToString();
    printf("rocksdb open error: %s\n", status_str.c_str());
    exit(-1);
  }
  rocksdb::Options valuedbOptions = options.valuedbOptions;
  valuedbOptions.compaction_filter_factory.reset(new NewDbCompactionFilterFactory(keydb_));
  valuedbOptions.comparator = rocksdb::Uint64Comparator();

  status = rocksdb::DB::Open(valuedbOptions, dbname + "valuedb", &valuedb_);
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
  rocksdb::CancelAllBackgroundWork(keydb_, true);
  rocksdb::CancelAllBackgroundWork(valuedb_, true);
  delete keydb_;
  delete valuedb_;

  // thread pool
  threadpool_destroy(pool_, 1);
  sem_destroy(&q_sem_);

  // if (dbstats_)
  //   fprintf(stdout, "STATISTICS:\n%s\n", dbstats_->ToString().c_str());
}

Status DBImpl::Put(const WriteOptions &options, const Slice &key,
                   const Slice &value) {

  RecordTick(options_.statistics.get(), REQ_PUT);
  // write phy-log key mapping in db
  rocksdb::Slice lkey(key.data(), key.size());
  uint64_t seq;
  seq = get_new_seq();
  char *pkey_str = (char *)malloc(sizeof(uint64_t));
  *((uint64_t *)pkey_str) = seq;
  rocksdb::Slice pkey(pkey_str, sizeof(uint64_t));

  rocksdb::WriteOptions write_options;
  rocksdb::Status s = keydb_->Put(write_options, lkey, pkey);
  int wi_retry_cnt = 0;
  while (!s.ok()) {
    fprintf(stderr, "[rocks index put] err: %s\n", s.ToString().c_str());
    s = keydb_->Put(write_options, lkey, pkey);
    if (wi_retry_cnt++ >= 3)
      return Status().IOError(Slice());
  }

  // prepare physical value
  int pval_size = sizeof(uint8_t) + key.size() + value.size();
  char *pval_str = (char *)malloc(pval_size);
  char *pval_ptr = pval_str;
  *((uint8_t *)pval_ptr) = (uint8_t)key.size();
  pval_ptr += sizeof(uint8_t);
  memcpy(pval_ptr, key.data(), key.size());
  pval_ptr += key.size();
  memcpy(pval_ptr, value.data(), value.size());

  // write pkey-pval in db
  rocksdb::Slice pval(pval_str, pval_size);
  s = valuedb_->Put(write_options, pkey, pval);

  wi_retry_cnt = 0;
  while (!s.ok()) {
    fprintf(stderr, "[rocks index put] err: %s\n", s.ToString().c_str());
    s = valuedb_->Put(write_options, pkey, pval);
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

  rocksdb::WriteOptions write_options;
  rocksdb::Slice rocks_key(key.data(), key.size());
  rocksdb::Status s = keydb_->Delete(write_options, rocks_key);

  return Status();
}

Status DBImpl::Get(const ReadOptions &options, const Slice &key,
                   std::string *value) {
  RecordTick(options_.statistics.get(), REQ_GET);

  // read from keydb
  rocksdb::Slice lkey(key.data(), key.size());
  std::string pkey_str;
  rocksdb::Status s = keydb_->Get(rocksdb::ReadOptions(), lkey, &pkey_str);

  if (s.IsNotFound()) {
    RecordTick(options_.statistics.get(), REQ_GET_NOEXIST);
    return Status().NotFound(Slice());
  }

  int ri_retry_cnt = 0;
  while (!(s.ok())) { // read index retry, rare
    usleep(100);
    s = keydb_->Get(rocksdb::ReadOptions(), lkey, &pkey_str);
    if (ri_retry_cnt++ >= 3)
      return Status().NotFound(Slice());
  }

  // read from valuedb
  rocksdb::Slice pkey(pkey_str.data(), pkey_str.size());
  std::string pval_str;
  s = valuedb_->Get(rocksdb::ReadOptions(), pkey, &pval_str);

  if (s.IsNotFound()) {
    RecordTick(options_.statistics.get(), REQ_GET_NOEXIST);
    return Status().NotFound(Slice());
  }

  ri_retry_cnt = 0;
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
  // implement this
  // iterator
  rocksdb::ReadOptions rdopts;
  rocksdb::Iterator *it = keydb_->NewIterator(rdopts);
  it->SeekToFirst();
  printf("key iterator starts: \n");
  while (it->Valid()) {
    rocksdb::Slice key = it->key();
    rocksdb::Slice val = it->value();
    printf("key %s, value %ld\n", key.data(), *((uint64_t *)val.data()));
    it->Next();
  }

  it = valuedb_->NewIterator(rdopts);
  it->SeekToFirst();
  printf("value iterator starts: \n");
  while (it->Valid()) {
    rocksdb::Slice key = it->key();
    rocksdb::Slice val = it->value();
    printf("key %ld, value %s\n", *((uint64_t *)key.data()), val.data());
    it->Next();
  }
  std::string stats;
  valuedb_->GetProperty("rocksdb.stats", &stats);
  printf("stats: %s\n", stats.c_str());
  printf("compaction started\n");
  stats = "";
  valuedb_->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
  valuedb_->GetProperty("rocksdb.stats", &stats);
  printf("stats: %s\n", stats.c_str());
  printf("compaction done\n");

  it = keydb_->NewIterator(rdopts);
  it->SeekToFirst();
  printf("key iterator starts: \n");
  while (it->Valid()) {
    rocksdb::Slice key = it->key();
    rocksdb::Slice val = it->value();
    printf("key %s, value %ld\n", key.data(), *((uint64_t *)val.data()));
    it->Next();
  }

  it = valuedb_->NewIterator(rdopts);
  it->SeekToFirst();
  printf("value iterator starts: \n");
  while (it->Valid()) {
    rocksdb::Slice key = it->key();
    rocksdb::Slice val = it->value();
    printf("key %ld, value %s\n", *((uint64_t *)key.data()), val.data());
    it->Next();
  }
}

void DBImpl::vLogGCWorker(int hash, std::vector<std::string> *ukey_list,
                          std::vector<std::string> *vmeta_list, int idx,
                          int size, int *oldLogFD, int *newLogFD){
    // implement this
};

void DBImpl::vLogGarbageCollect(){
    // TODO
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
