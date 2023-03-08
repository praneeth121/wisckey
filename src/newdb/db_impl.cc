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
  valuedbOptions.compaction_filter_factory.reset(
      new NewDbCompactionFilterFactory());
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
  phy_keys_for_gc.clear();
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
      printf("pushing %ld key to GC\n", oKey.phykey);
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
      printf("pushing %ld key to GC\n", pKey.phykey);
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

bool cust_comparator_for_sstfiles(GCMetadata &a, GCMetadata &b) {
  if ((*((uint64_t *)a.sstmeta.smallestkey.data())) <
      (*((uint64_t *)b.sstmeta.smallestkey.data())))
    return true;
  return false;
}

void DBImpl::runGC() {
  // Have to parallized
  std::string stats;
  valuedb_->GetProperty("rocksdb.stats", &stats);
  fprintf(stdout, "stats: %s\n", stats.c_str());

  fprintf(stdout, "running garbage collection:\n");

  std::vector<uint64_t> tmp_gc_keys;
  std::vector<uint64_t>::iterator gc_keys_start, gc_keys_end;
  {
    std::unique_lock<std::mutex> lock(gc_keys_mutex);
    // make a copy from main gc_key to workspace
    tmp_gc_keys.insert(tmp_gc_keys.end(), phy_keys_for_gc.begin(),
                       phy_keys_for_gc.end());
    // clean the global keys
    phy_keys_for_gc.clear();
  }
  std::sort(tmp_gc_keys.begin(), tmp_gc_keys.end());
  std::unordered_set<uint64_t> CompactionFilterGCSet(tmp_gc_keys.begin(),
                                           tmp_gc_keys.end());
  gc_keys_start = tmp_gc_keys.begin();
  gc_keys_end = tmp_gc_keys.end();

  fprintf(stdout, "Following keys are considered for GC\n");
  for (auto it = gc_keys_start; it != gc_keys_end; ++it) {
    printf("%ld ", *it);
  }
  printf("\n");

  rocksdb::ColumnFamilyMetaData cf_meta;
  valuedb_->GetColumnFamilyMetaData(&cf_meta);

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
      else if (garbage_percent >= 40)
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

  std::sort(input_files.begin(), input_files.end(),
            cust_comparator_for_sstfiles);

  for (auto it = input_files.begin(); it != input_files.end(); ++it) {
    printf("file start %ld and end: %ld\n",
           *((uint64_t *)it->sstmeta.smallestkey.data()),
           *((uint64_t *)it->sstmeta.largestkey.data()));
  }

  std::vector<GCCollectedKeys> deletion_keys;
  int file_idx = 0;
  std::vector<std::string> compaction_files;
  std::vector<GCCollectedKeys> deletion_keys_tmp;
  GCCollectedKeys gc_collected_keys_obj;
  int next_lvl = 0;
  auto *compaction_filter_factory =
      reinterpret_cast<NewDbCompactionFilterFactory *>(
          valuedb_->GetOptions().compaction_filter_factory.get());
  compaction_filter_factory->set_garbage_keys(&CompactionFilterGCSet);

  while (file_idx < input_files.size()) {
    if (input_files[file_idx].ready_for_gc) {
      printf("file added for GC\n");
      // can be optimised using vector iterator technique
      compaction_files.push_back(input_files[file_idx].sstmeta.name);
      next_lvl = std::max(next_lvl, input_files[file_idx].level + 1);
    } else {
      if (compaction_files.size() > 1) {
        printf("running a garbage collection on %d files\n",
               compaction_files.size());
        for (int i = 0; i < compaction_files.size(); i++) {
          printf("%s ", compaction_files[i].data());
        }
        printf("\n");
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
    }
    file_idx++;
  }

  // final run
  if (compaction_files.size() > 1) {
    printf("running a garbage collection on %d files\n",
           compaction_files.size());
    for (int i = 0; i < compaction_files.size(); i++) {
      printf("%s ", compaction_files[i].data());
    }
    printf("\n");
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

  // cleaning the gc_keys
  for (auto it = tmp_gc_keys.begin(); it != tmp_gc_keys.end(); ++it) {
    printf("%ld ", *it);
  }
  printf("\n");
  tmp_gc_keys.clear();
  // deleting from the reverse order
  {
    std::unique_lock<std::mutex> lock(gc_keys_mutex);
    phy_keys_for_gc.insert(phy_keys_for_gc.end(), CompactionFilterGCSet.begin(),
                           CompactionFilterGCSet.end());
  }
  //
  for (auto it = phy_keys_for_gc.begin(); it != phy_keys_for_gc.end(); ++it) {
    printf("%ld ", *it);
  }
  printf("\n");

  valuedb_->GetProperty("rocksdb.stats", &stats);
  printf("stats: %s\n", stats.c_str());
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
