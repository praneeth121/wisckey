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
#include <set>
#include <thread>

// #define DEBUG

namespace newdb {

DBImpl::DBImpl(const Options &options, const std::string &dbname)
    : options_(options), dbname_(dbname), inflight_io_count_(0),
      dbstats_(nullptr), sequence_(0) {

  // start thread pool for async i/o
  pool_ = threadpool_create(options.threadPoolThreadsNum,
                            options.threadPoolQueueDepth, 0, &q_sem_);
  sem_init(&q_sem_, 0, options.threadPoolQueueDepth - 1);
  auto keydbOptions = options.keydbOptions;

  rocksdb::BlockBasedTableOptions table_options;
  table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(8, true));
  table_options.block_size = 4096;
  table_options.cache_index_and_filter_blocks = true;
  table_options.pin_l0_filter_and_index_blocks_in_cache = true;
  table_options.cache_index_and_filter_blocks_with_high_priority = true;
  std::shared_ptr<rocksdb::Cache> cache = rocksdb::NewLRUCache(2147483648 * 2);
  table_options.block_cache = cache;

  // table_options.allow_os_buffer = false;
  keydbOptions.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(table_options));
  keydbOptions.statistics = rocksdb::CreateDBStatistics();
  // apply db options
  rocksdb::Status status =
      rocksdb::DB::Open(keydbOptions, dbname + "keydb", &keydb_);
  if (status.ok())
    printf("rocksdb open ok\n");
  else {
    std::string status_str = status.ToString();
    printf("rocksdb open error: %s\n", status_str.c_str());
    exit(-1);
  }
  rocksdb::Options valuedbOptions = options.valuedbOptions;
  valuedbOptions.compaction_filter_factory.reset(
      new NewDbCompactionFilterFactory(keydb_));
  valuedbOptions.comparator = rocksdb::Uint64Comparator();

  status = rocksdb::DB::Open(valuedbOptions, dbname + "valuedb", &valuedb_);
  if (status.ok())
    printf("rocksdb open ok\n");
  else {
    std::string status_str = status.ToString();
    printf("rocksdb open error: %s\n", status_str.c_str());
    exit(-1);
  }
  // CODE FOR PROFILING
  AvgTimeForKeyDBInsertion = 0;
  AvgTimeForValueDBInsertion = 0;
  AvgTimeForValuePreperation = 0;
  // threadpool_add(pool_, &StartGCThread, this, 0);
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

  rocksdb::Slice gc_lkey(key.data(), key.size());
  std::string gc_pkey_str;
  rocksdb::Status gc_s =
      keydb_->Get(rocksdb::ReadOptions(), gc_lkey, &gc_pkey_str);
  if (gc_s.IsNotFound()) {
    RecordTick(options_.statistics.get(), REQ_GET_NOEXIST);
  } else if (gc_s.ok()) {
    {
      std::unique_lock<std::mutex> lock(gc_keys_mutex);
      if (gc_pkey_str.size() != 0)
        phy_keys_for_gc.push_back(*(uint64_t *)gc_pkey_str.data());
    }
  }

  // keydb insertion
  auto KeyDBStartTime = std::chrono::high_resolution_clock::now();
  rocksdb::Slice lkey(key.data(), key.size());
  uint64_t seq;
  seq = get_new_seq();
  char *pkey_str = (char *)malloc(sizeof(uint64_t));
  *((uint64_t *)pkey_str) = seq;
  rocksdb::Slice pkey(pkey_str, sizeof(uint64_t));

  rocksdb::WriteOptions write_options;
  rocksdb::Status s = keydb_->Put(write_options, lkey, pkey);
  assert(s.ok());
  auto KeyDBEndTime = std::chrono::high_resolution_clock::now();
  AvgTimeForKeyDBInsertion +=
      std::chrono::duration<double>(KeyDBEndTime - KeyDBStartTime).count();

  // prepare physical value
  auto ValueDBKeyPreperationStartTime =
      std::chrono::high_resolution_clock::now();
  int pval_size = sizeof(uint8_t) + key.size() + value.size();
  char *pval_str = (char *)malloc(pval_size);
  char *pval_ptr = pval_str;
  *((uint8_t *)pval_ptr) = (uint8_t)key.size();
  pval_ptr += sizeof(uint8_t);
  memcpy(pval_ptr, key.data(), key.size());
  pval_ptr += key.size();
  memcpy(pval_ptr, value.data(), value.size());
  auto ValueDBKeyPreperationEndTime = std::chrono::high_resolution_clock::now();
  AvgTimeForValuePreperation =
      std::chrono::duration<double>(ValueDBKeyPreperationEndTime -
                                    ValueDBKeyPreperationStartTime)
          .count();

  // write pkey-pval in db
  auto ValueDBKeyInsertionStartTime = std::chrono::high_resolution_clock::now();
  rocksdb::Slice pval(pval_str, pval_size);
  s = valuedb_->Put(write_options, pkey, pval);
  assert(s.ok());
  auto ValueDBKeyInsertionEndTime = std::chrono::high_resolution_clock::now();
  AvgTimeForValueDBInsertion +=
      std::chrono::duration<double>(ValueDBKeyInsertionEndTime -
                                    ValueDBKeyInsertionStartTime)
          .count();
  free(pkey_str);
  free(pval_str);

  return Status();
}

Status DBImpl::Delete(const WriteOptions &options, const Slice &key) {

  RecordTick(options_.statistics.get(), REQ_DEL);
  rocksdb::Slice gc_lkey(key.data(), key.size());
  std::string gc_pkey_str;
  rocksdb::Status gc_s =
      keydb_->Get(rocksdb::ReadOptions(), gc_lkey, &gc_pkey_str);
  if (gc_s.IsNotFound()) {
    RecordTick(options_.statistics.get(), REQ_GET_NOEXIST);
  } else if (gc_s.ok()) {
    {
      std::unique_lock<std::mutex> lock(gc_keys_mutex);
      if (gc_pkey_str.size() != 0)
        phy_keys_for_gc.push_back(*(uint64_t *)gc_pkey_str.data());
    }
  }

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

  // read from valuedb
  rocksdb::Slice pkey(pkey_str.data(), pkey_str.size());
  std::string pval_str;
  s = valuedb_->Get(rocksdb::ReadOptions(), pkey, &pval_str);

  if (s.IsNotFound()) {
    RecordTick(options_.statistics.get(), REQ_GET_NOEXIST);
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
  printf("Block cache MISS: %ld\n",
         keydb_->GetOptions().statistics->getTickerCount(
             rocksdb::BLOCK_CACHE_DATA_MISS));
  printf("Block cache HIT: %ld\n",
         keydb_->GetOptions().statistics->getTickerCount(
             rocksdb::BLOCK_CACHE_DATA_HIT));
  std::cout << std::fixed
            << "Avg KeyDBInsertionTime: " << AvgTimeForKeyDBInsertion
            << " ms\n";
  std::cout << std::fixed
            << "Avg ValuePreperation: " << AvgTimeForValuePreperation
            << " ms\n";
  std::cout << std::fixed
            << "Avg ValueDBInsertionTime: " << AvgTimeForValueDBInsertion
            << " ms\n";
}

void DBImpl::vLogGCWorker(int hash, std::vector<std::string> *ukey_list,
                          std::vector<std::string> *vmeta_list, int idx,
                          int size, int *oldLogFD, int *newLogFD){
    // implement this
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

void log_gc_stats(std::vector<std::string> &files) {
  std::cout << "running gc: ";
  for (int i = 0; i < files.size(); i++) {
    std::cout << files[i] << " ";
  }
  std::cout << std::endl;
}

bool cust_comparator_for_sstfiles(GCMetadata &a, GCMetadata &b) {
  if ((*((uint64_t *)a.sstmeta.smallestkey.data())) <
      (*((uint64_t *)b.sstmeta.smallestkey.data())))
    return true;
  return false;
}

void DBImpl::vLogGCWorker(void *args) {
  /**
  this is nlogn algorithm to sort physical keys (Should be optimised) and
  apply binary search to get number of elements in the region are garbage
  adding lock to protect the vector from being modified using PUT method
  **/
  std::string stats;
  valuedb_->GetProperty("rocksdb.stats", &stats);
  printf("stats: %s\n", stats.c_str());
  std::vector<uint64_t>::iterator gc_keys_start, gc_keys_end;
  std::vector<uint64_t> GCKeysVector;
  {
    std::unique_lock<std::mutex> lock(gc_keys_mutex);
    GCKeysVector.insert(GCKeysVector.begin(), phy_keys_for_gc.begin(),
                        phy_keys_for_gc.end());
    phy_keys_for_gc.clear();
  }
  std::sort(GCKeysVector.begin(), GCKeysVector.end());
  std::cout << "Total Keys before GC: " << GCKeysVector.size() << std::endl;
  gc_keys_start = GCKeysVector.begin();
  gc_keys_end = GCKeysVector.end();

#ifdef DEBUG
  printf("vLogGarbageCollect: GC Keys");
  for (auto it = gc_keys_start; it != gc_keys_end; ++it) {
    printf("%ld ", *it);
  }
  printf("\n");
  for (auto it = phy_keys_for_gc.begin(); it != phy_keys_for_gc.end(); ++it) {
    printf("%ld ", *it);
  }
  printf("\n");
#endif

  rocksdb::ColumnFamilyMetaData cf_meta;
  valuedb_->GetColumnFamilyMetaData(&cf_meta);

  // std::vector<std::pair<rocksdb::SstFileMetaData, std::pair<int, bool>>>
  // GCMetadataCompactionFiles: This containes all the sst files that overlap
  // with GC keys region.
  std::vector<GCMetadata> GCMetadataCompactionFiles;
  for (auto level : cf_meta.levels) {
    for (auto file : level.files) {
      // Extract file information
      uint64_t smallest_key = *((uint64_t *)file.smallestkey.data());
      uint64_t largest_key = *((uint64_t *)file.largestkey.data());
      uint64_t smallest_seqno = file.smallest_seqno;
      uint64_t largest_seqno = file.largest_seqno;
      uint64_t file_size = file.size;

      // Find the no of keys with the range of user defined smallest & largest
      // physical keys
      std::vector<uint64_t>::iterator small_itr =
          std::lower_bound(gc_keys_start, gc_keys_end, smallest_key);
      std::vector<uint64_t>::iterator large_itr =
          std::upper_bound(gc_keys_start, gc_keys_end, largest_key);
      int keys_count_ready_for_gc = large_itr - small_itr;

      // TODO: just represents a approximate number of keys not the exact
      // number. This algorithm might not work for generalised user keys case of
      // varying length. Best Approximation is by dividing the
      // SSTFileSize/(KeySize+ValueSize) Possible Approximation for no of keys
      // in the sst files: Uncomment the appropriate approximation
      int total_keys_in_file = (largest_key - smallest_key);
      //  int total_keys_in_file = file_size/ (4113 + 8) // these values
      //      change based klen & vlen during experiment
      if (largest_seqno - smallest_seqno != 0)
        total_keys_in_file =
            (largest_seqno -
             smallest_seqno); // assumption that seq no correlate to records

#ifdef DEBUG
      std::cout << "Total Keys in file approximations: "
                << (largest_key - smallest_key) << " "
                << (file_size / (4113 + 8)) << " "
                << (largest_seqno - smallest_seqno) << std::endl;
#endif
      // Calculate the percent of keys ready for garbage collection
      float garbage_percent =
          ((keys_count_ready_for_gc)*100) / (total_keys_in_file);

      // Create a GCMetadata Object that will be used during GC Compaction
      // process
      GCMetadata gc_mt_obj;
      gc_mt_obj.sstmeta = file;
      gc_mt_obj.level = level.level;
      gc_mt_obj.smallest_itr = small_itr;
      gc_mt_obj.largest_itr = large_itr;
      if (file.being_compacted)
        gc_mt_obj.ready_for_gc = false;
      else if (garbage_percent >= this->GARBAGE_THRESHOLD)
        gc_mt_obj.ready_for_gc = true;
      else
        gc_mt_obj.ready_for_gc = false;
      GCMetadataCompactionFiles.push_back(gc_mt_obj);
#ifdef DEBUG
      std::cout << file.name << ": " << garbage_percent << " "
                << gc_mt_obj.ready_for_gc << " " << smallest_key << "->"
                << largest_key << std::endl;
#endif
    }
  }

  // sort the files
  std::sort(GCMetadataCompactionFiles.begin(), GCMetadataCompactionFiles.end(),
            cust_comparator_for_sstfiles);

  // This set will be used for filtering/removing dead keys during compaction
  // filtering process
  std::set<uint64_t> GCKeysSet;

  auto *compaction_filter_factory =
      reinterpret_cast<NewDbCompactionFilterFactory *>(
          valuedb_->GetOptions().compaction_filter_factory.get());

  // temporary variables used during compaction process
  int start_idx = 0, file_idx = 0;
  std::vector<GCCollectedKeys> deletion_keys;
  std::vector<GCMetadata> compaction_files_ready_for_gc;
  std::vector<GCCollectedKeys> deletion_keys_tmp;
  GCCollectedKeys gc_collected_keys_obj;

  // Actual Compaction process
  while (file_idx < GCMetadataCompactionFiles.size()) {

    // insert additional new keys (this does not mean dead keys) into set so
    // that they will considered during next round of compaction
    GCKeysSet.insert(GCMetadataCompactionFiles[file_idx].smallest_itr,
                     GCMetadataCompactionFiles[file_idx].largest_itr);

    if (GCMetadataCompactionFiles[file_idx].ready_for_gc) {
      // add the file for compaction during next round
      compaction_files_ready_for_gc.push_back(
          GCMetadataCompactionFiles[file_idx]);
    } else {

      // Run compaction only if the current compaction process has more than 1
      // sstfile ready for GC
      if (compaction_files_ready_for_gc.size() > 1) {

        // TODO: this sets the reference (can be removed)
        compaction_filter_factory->set_garbage_keys(&GCKeysSet);

        // Divide the total compaction process into subcompactions so that
        // merging files does not created overly skewed sst files
        std::vector<std::string> subcompaction_file_names;
        int max_level_ = 0;
        for (int l = 0; l < compaction_files_ready_for_gc.size(); l++) {
          if (l != 0 && l % this->MAX_NUMBER_OF_FILES_FOR_COMPACTION == 0) {
            log_gc_stats(subcompaction_file_names);
            valuedb_->CompactFiles(rocksdb::CompactionOptions(),
                                   subcompaction_file_names, max_level_ + 1);
            // reset after every subcompaction
            subcompaction_file_names.clear();
            max_level_ = 0;
          }
          subcompaction_file_names.push_back(
              compaction_files_ready_for_gc[l].sstmeta.name);
          max_level_ =
              std::max(max_level_, compaction_files_ready_for_gc[l].level);
        }

        // EDGE CASE: Pending files. Same as above process.
        if (subcompaction_file_names.size()) {
          log_gc_stats(subcompaction_file_names);
          valuedb_->CompactFiles(rocksdb::CompactionOptions(),
                                 subcompaction_file_names, max_level_ + 1);
        }

#ifdef DEBUG
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
#endif
      }
      compaction_files_ready_for_gc.clear();
    }
    file_idx++;
  }

  // EDGE CASE: PENDING COMPACTION FILES
  // Run compaction only if the current compaction process has more than 1
  // sstfile ready for GC
  if (compaction_files_ready_for_gc.size() > 1) {

    // TODO: this sets the reference (can be removed)
    compaction_filter_factory->set_garbage_keys(&GCKeysSet);

    // Divide the total compaction process into subcompactions so that merging
    // files does not created overly skewed sst files
    std::vector<std::string> subcompaction_file_names;
    int max_level_ = 0;
    for (int l = 0; l < compaction_files_ready_for_gc.size(); l++) {
      if (l != 0 && l % this->MAX_NUMBER_OF_FILES_FOR_COMPACTION == 0) {
        log_gc_stats(subcompaction_file_names);
        valuedb_->CompactFiles(rocksdb::CompactionOptions(),
                               subcompaction_file_names, max_level_ + 1);
        // reset after every subcompaction
        subcompaction_file_names.clear();
        max_level_ = 0;
      }
      subcompaction_file_names.push_back(
          compaction_files_ready_for_gc[l].sstmeta.name);
      max_level_ = std::max(max_level_, compaction_files_ready_for_gc[l].level);
    }

    // EDGE CASE: Pending files. Same as above process.
    if (subcompaction_file_names.size()) {
      log_gc_stats(subcompaction_file_names);
      valuedb_->CompactFiles(rocksdb::CompactionOptions(),
                             subcompaction_file_names, max_level_ + 1);
    }

#ifdef DEBUG
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
#endif
  }
  compaction_files_ready_for_gc.clear();

#ifdef DEBUG
  // Print all the keys considered during GC Process
  for (auto it = GCKeysVector.begin(); it != GCKeysVector.end(); ++it) {
    printf("%ld ", *it);
  }
  printf("\n");
#endif

  GCKeysVector.clear();
  // inserting the pending keys for next session

  std::cout << "Pending Keys After GC: " << GCKeysSet.size() << std::endl;

  {
    std::unique_lock<std::mutex> lock(gc_keys_mutex);
    phy_keys_for_gc.insert(phy_keys_for_gc.end(), GCKeysSet.begin(),
                           GCKeysSet.end());
  }

#ifdef DEBUG
  rocksdb::ColumnFamilyMetaData cf_meta_2;
  valuedb_->GetColumnFamilyMetaData(&cf_meta_2);

  // std::vector<std::pair<rocksdb::SstFileMetaData, std::pair<int, bool>>>
  // GCMetadataCompactionFiles;
  std::cout << "sst files after Garbage Collection:\n";
  GCMetadataCompactionFiles.clear();
  for (auto level : cf_meta_2.levels) {
    for (auto file : level.files) {
      std::cout << file.name << std::endl;
    }
  }
  valuedb_->GetProperty("rocksdb.stats", &stats);
  printf("stats: %s\n", stats.c_str());
#endif
}; // namespace newdb

void DBImpl::vLogGarbageCollect() {
  void *args;
  vLogGCWorker(args);
};

} // namespace newdb
