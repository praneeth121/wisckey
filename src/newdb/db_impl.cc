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

  rocksdb::Options valuedbOptions;
  valuedbOptions.IncreaseParallelism();
  valuedbOptions.create_if_missing = true;
  valuedbOptions.max_open_files = -1;
  valuedbOptions.compression = rocksdb::kNoCompression;
  valuedbOptions.paranoid_checks = false;
  valuedbOptions.allow_mmap_reads = false;
  valuedbOptions.allow_mmap_writes = false;
  valuedbOptions.use_direct_io_for_flush_and_compaction = true;
  valuedbOptions.use_direct_reads = true;
  valuedbOptions.write_buffer_size = 4096;
  valuedbOptions.target_file_size_base = 2048;
  valuedbOptions.max_bytes_for_level_base = 2048;
  valuedbOptions.comparator = rocksdb::Uint64Comparator();

  // rocksdb::Options valuedbOptions = options.valuedbOptions;
  valuedbOptions.compaction_filter_factory.reset(
      new NewDbCompactionFilterFactory(keydb_));

  status = rocksdb::DB::Open(valuedbOptions, dbname + "valuedb", &valuedb_);
  if (status.ok())
    printf("rocksdb open ok\n");
  else {
    std::string status_str = status.ToString();
    printf("rocksdb open error: %s\n", status_str.c_str());
    exit(-1);
  }

  // start thread pool for async i/o
  pool_ = threadpool_create(options.threadPoolThreadsNum, options.threadPoolQueueDepth, 0, &q_sem_);
  sem_init(&q_sem_, 0, options.threadPoolQueueDepth-1);

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
}

Status DBImpl::Put(const WriteOptions &options, const Slice &key,
                   const Slice &value) {

  RecordTick(options_.statistics.get(), REQ_PUT);

  // get existing physical key for gc
  // performance is not affected drastically because the keydb is mostly on
  // MainMemory/ NoDiskFlush so adjust the sst table value so that there is no
  // flush triggered

  /** READ FROM KEY_DB TO GET PHYSICAL **/
  // TODO: Can implement a bloom filter
  // Add User Defined update and insert mode
  rocksdb::Slice gc_lkey(key.data(), key.size());
  std::string gc_pkey_str;
  rocksdb::Status s =
      keydb_->Get(rocksdb::ReadOptions(), gc_lkey, &gc_pkey_str);

  int ri_retry_cnt = 0;
  while (!(s.ok())) { // read index retry, rare
    usleep(100);
    s = keydb_->Get(rocksdb::ReadOptions(), gc_lkey, &gc_pkey_str);
    if (ri_retry_cnt++ >= 3)
      break;
  }

  if (s.ok()) {
    // TODO: Dynamic memory limitations
    {
      std::unique_lock<std::mutex> lock(gc_keys_mutex);
      if (gc_pkey_str.size() != 0)
        phy_keys_for_gc.push_back(*(uint64_t *)gc_pkey_str.data());
    }
  }

  /** COLLECTED THE METADATA INFORMATION NEEDED FOR GC **/

  // write phy-log key mapping in db
  rocksdb::Slice lkey(key.data(), key.size());
  uint64_t seq;
  seq = get_new_seq();
  char *pkey_str = (char *)malloc(sizeof(uint64_t));
  *((uint64_t *)pkey_str) = seq;
  rocksdb::Slice pkey(pkey_str, sizeof(uint64_t));

  rocksdb::WriteOptions write_options;
  s = keydb_->Put(write_options, lkey, pkey);
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

bool cust_comparator_for_sstfiles(GCMetadata &a, GCMetadata &b) {
  if ((*((uint64_t *)a.sstmeta.smallestkey.data())) <
      (*((uint64_t *)b.sstmeta.smallestkey.data())))
    return true;
  return false;
}

void DBImpl::vLogGCWorker(void* args){
  /**
  this is nlogn algorithm to sort physical keys (Should be optimised) and
  apply binary search to get number of elements in the region are garbage
  adding lock to protect the vector from being modified using PUT method
  **/
  std::string stats;
  valuedb_->GetProperty("rocksdb.stats", &stats);
  printf("stats: %s\n", stats.c_str());
  std::vector<uint64_t>::iterator gc_keys_start, gc_keys_end;
  {
    std::unique_lock<std::mutex> lock(gc_keys_mutex);
    std::sort(phy_keys_for_gc.begin(), phy_keys_for_gc.end());
    gc_keys_start = phy_keys_for_gc.begin();
    gc_keys_end = phy_keys_for_gc.end();
  }
// #ifdef DEBUG
  printf("vLogGarbageCollect: GC Keys");
  for (auto it = gc_keys_start; it != gc_keys_end; ++it) {
    printf("%ld ", *it);
  }
  printf("\n");
  for (auto it = phy_keys_for_gc.begin(); it != phy_keys_for_gc.end();++it) {
    printf("%ld ", *it);
  }
  printf("\n");
// #endif

  rocksdb::ColumnFamilyMetaData cf_meta;
  valuedb_->GetColumnFamilyMetaData(&cf_meta);

  // std::vector<std::pair<rocksdb::SstFileMetaData, std::pair<int, bool>>>
  // input_files;
  std::vector<GCMetadata> input_files;
  for (auto level : cf_meta.levels) {
    for (auto file : level.files) {
      uint64_t smallest_key = *((uint64_t *)file.smallestkey.data());
      uint64_t largest_key = *((uint64_t *)file.largestkey.data());
      std::vector<uint64_t>::iterator small_itr =
          std::lower_bound(gc_keys_start, gc_keys_end, smallest_key);
      std::vector<uint64_t>::iterator large_itr =
          std::upper_bound(gc_keys_start, gc_keys_end, largest_key);
      int no_of_keys = large_itr - small_itr;
      // TODO: just represents a approximate number of keys not the exact number
      int number_of_key_in_file = (largest_key - smallest_key);
      printf("%d -> %d", large_itr - gc_keys_start, small_itr - gc_keys_start);
      float garbage_percent = ((no_of_keys)*100) / (number_of_key_in_file);
      GCMetadata gc_mt_obj;
      gc_mt_obj.sstmeta = file;
      gc_mt_obj.level = level.level;
      gc_mt_obj.smallest_itr = small_itr;
      gc_mt_obj.largest_itr = large_itr;
      if (file.being_compacted)
        gc_mt_obj.ready_for_gc = false;
      else if (garbage_percent >= 1)
        gc_mt_obj.ready_for_gc = true;
      else
        gc_mt_obj.ready_for_gc = false;
      input_files.push_back(gc_mt_obj);
      printf("%s is a grabage file with a %.2f percent garbage and st_key:%ld "
             "-> end_key:%ld\n",
             gc_mt_obj.sstmeta.name.data(), garbage_percent, smallest_key,
             largest_key);
    }
  }

  std::sort(input_files.begin(), input_files.end(),cust_comparator_for_sstfiles);
  for (auto it = input_files.begin(); it != input_files.end(); ++it) {
    printf("file start %ld and end: %ld\n",
           *((uint64_t *)it->sstmeta.smallestkey.data()),
           *((uint64_t *)it->sstmeta.largestkey.data()));
  }
  std::vector<GCCollectedKeys> deletion_keys;

  int file_idx = 0;
  std::vector<std::string> compaction_files;
  std::set<uint64_t> gc_keys_set;
  std::vector<GCCollectedKeys> deletion_keys_tmp;
  GCCollectedKeys gc_collected_keys_obj;
  int next_lvl = 0;
  auto *compaction_filter_factory =
      reinterpret_cast<NewDbCompactionFilterFactory *>(
          valuedb_->GetOptions().compaction_filter_factory.get());
  while (file_idx < input_files.size()) {
    if (input_files[file_idx].ready_for_gc) {
      // can be optimised using vector iterator technique
      gc_collected_keys_obj.smallest_itr = input_files[file_idx].smallest_itr;
      gc_collected_keys_obj.largest_itr = input_files[file_idx].largest_itr;
      deletion_keys_tmp.push_back(gc_collected_keys_obj);
      gc_keys_set.insert(input_files[file_idx].smallest_itr,
                         input_files[file_idx].largest_itr);
      compaction_files.push_back(input_files[file_idx].sstmeta.name);
      next_lvl = std::max(next_lvl, input_files[file_idx].level + 1);
    } else {
      if (compaction_files.size() > 1) {
        deletion_keys.insert(deletion_keys.end(), deletion_keys_tmp.begin(), deletion_keys_tmp.end());
        printf("running a garbage collection on %d files\n",
               compaction_files.size());
        for(int i = 0;i < compaction_files.size();i++) {
          printf("%s ", compaction_files[i].data());
        }
        printf("\n");
        compaction_filter_factory->set_garbage_keys(&gc_keys_set);
        
        valuedb_->CompactFiles(rocksdb::CompactionOptions(), compaction_files,
                               next_lvl);               
        rocksdb::ReadOptions rdopts;
        rocksdb::Iterator *it = valuedb_->NewIterator(rdopts);
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
      next_lvl = 0;
      compaction_files.clear();
      gc_keys_set.clear();
      deletion_keys_tmp.clear();
    }
    file_idx++;
  }

  // cleaning the gc_keys
  for (auto it = phy_keys_for_gc.begin(); it != phy_keys_for_gc.end();++it) {
    printf("%ld ", *it);
  }
  printf("\n");
  // deleting from the reverse order
  printf("size: %ld\n", deletion_keys.size());
  for(int i = deletion_keys.size()-1;i >= 0;i--) {
    phy_keys_for_gc.erase(deletion_keys[i].smallest_itr, deletion_keys[i].largest_itr);
  }
  
  //
  for (auto it = phy_keys_for_gc.begin(); it != phy_keys_for_gc.end();++it) {
    printf("%ld ", *it);
  }
  printf("\n");

  valuedb_->GetProperty("rocksdb.stats", &stats);
  printf("stats: %s\n", stats.c_str());



};




void DBImpl::vLogGarbageCollect() {
  // sem_wait(&(q_sem_));
  // void* args;
  // if (threadpool_add(pool_, &vLogGCWorker, args, 0) < 0) {
  //   printf("async_pread pool_add error, fd %d, offset %llu\n");
  //   exit(1);
  // }
  void* args;
  vLogGCWorker(args);
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
