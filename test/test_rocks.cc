
#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"

#include <stdio.h>
#include <stdlib.h>

#define TOTAL_RECORDS 1000

int main() {
  rocksdb::DB *rdb_;

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
  valuedbOptions.write_buffer_size = 64 << 20;
  valuedbOptions.target_file_size_base = 64 * 1048576;
  valuedbOptions.max_bytes_for_level_base = 64 * 1048576;
  // options_.valuedbOptions = valuedbOptions;

  // apply db options
  rocksdb::Status status =
      rocksdb::DB::Open(valuedbOptions, "../../newdb/valuedb", &rdb_);
  if (status.ok())
    printf("rocksdb open ok\n");
  else
    printf("rocksdb open error\n");
  
  rocksdb::ColumnFamilyMetaData cf_meta;
  rdb_->GetColumnFamilyMetaData(&cf_meta);

  std::vector<std::string> input_file_names;
  int level_number = 0;
  for (auto level : cf_meta.levels) {
    printf("level %d: files:", level_number);
    for (auto file : level.files) {
      uint64_t smallest_key = *((uint*)file.smallestkey.data());
      uint64_t largest_key = *((uint*)file.largestkey.data());
      printf("file %s: smallest: %ld largest: %d\n", file.name.data(), smallest_key, largest_key);
      input_file_names.push_back(file.name);
    }
    level_number++;
  }



  // std::string stats;
  // rdb_->GetProperty("rocksdb.stats", &stats);
  // printf("compaction_stats: %s\n", stats.data());
  // write some records
  // for (int i = 0; i < TOTAL_RECORDS; i++) {
  //   char key[16] = {0};
  //   char val[128] = {0};
  //   sprintf(key, "%0*ld", 16 - 1, i);
  //   sprintf(val, "value%ld", i);
  //   rocksdb::Slice rkey(key, 16);
  //   rocksdb::Slice rval(val, 128);

  //   rdb_->Put(rocksdb::WriteOptions(), rkey, rval);
  // }
  // printf("finished load records\n");

  // // update in iterator
  // const rocksdb::ReadOptions options;
  // rocksdb::Iterator *it = rdb_->NewIterator(options);
  // it->SeekToFirst();

  // int newv = TOTAL_RECORDS;
  // while (it->Valid()) {
  //   rocksdb::Slice key = it->key();
  //   rocksdb::Slice val = it->value();

  //   char newval[128] = {0};
  //   sprintf(newval, "value%ld", newv++);
  //   rocksdb::Slice rval(newval, 128);
  //   rdb_->Put(rocksdb::WriteOptions(), key, rval);
  //   it->Next();
  // }
  // printf("finished update records through iterator\n");
  // delete it;

  // // read back updated value
  // it = rdb_->NewIterator(options);
  // it->SeekToFirst();
  // while (it->Valid()) {
  //   rocksdb::Slice key = it->key();
  //   rocksdb::Slice val = it->value();
  //   printf("key %s, value %s\n", key.data(), val.data());
  //   it->Next();
  // }

  return 0;
}