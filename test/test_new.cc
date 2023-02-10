
#include "newdb/db.h"
#include "newdb/iterator.h"
#include "newdb/options.h"
#include "newdb/slice.h"
#include "newdb/status.h"

#include <stdio.h>
#include <stdlib.h>

#define TOTAL_RECORDS 10000

int main() {
  newdb::DB *db_;


  newdb::Options options_;

  rocksdb::Options keydbOptions;
  keydbOptions.IncreaseParallelism();
  keydbOptions.create_if_missing = true;
  keydbOptions.max_open_files = -1;
  keydbOptions.compression = rocksdb::kNoCompression;
  keydbOptions.paranoid_checks = false;
  keydbOptions.allow_mmap_reads = false;
  keydbOptions.allow_mmap_writes = false;
  keydbOptions.use_direct_io_for_flush_and_compaction = true;
  keydbOptions.use_direct_reads = true;
  keydbOptions.write_buffer_size = 64 << 20;
  keydbOptions.target_file_size_base = 64 * 1048576;
  keydbOptions.max_bytes_for_level_base = 64 * 1048576;
  options_.keydbOptions = keydbOptions;

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
  valuedbOptions.write_buffer_size = 1920;
  valuedbOptions.target_file_size_base = 1920;
  valuedbOptions.max_bytes_for_level_base = 1920;
  options_.valuedbOptions = valuedbOptions;

  newdb::Status status = newdb::DB::Open(options_, "", &db_);
  if (status.ok())
    printf("newdb open ok\n");
  else
    printf("newdb open error\n");

  // write some records
  for (int i = 0; i < TOTAL_RECORDS; i++) {
    char key[16] = {0};
    char val[128] = {0};
    sprintf(key, "%0*ld", 16 - 1, i);
    sprintf(val, "value%ld", i);
    newdb::Slice rkey(key, 16);
    newdb::Slice rval(val, 128);

    db_->Put(newdb::WriteOptions(), rkey, rval);

    std::string gval;
    db_->Get(newdb::ReadOptions(), rkey, &gval);
    printf("key %s, value %s\n", rkey.data(), gval.c_str());
  }
  printf("finished load records\n");

  // update in iterator
  const newdb::ReadOptions options;
  newdb::Iterator *it = db_->NewIterator(options);
  it->SeekToFirst();

  int newv = TOTAL_RECORDS;
  while (it->Valid()) {
    newdb::Slice key = it->key();
    newdb::Slice val = it->value();

    char newval[128] = {0};
    sprintf(newval, "value%ld", newv++);
    newdb::Slice rval(newval, 128);
    db_->Put(newdb::WriteOptions(), key, rval);
    it->Next();
  }
  printf("finished update records through iterator\n");
  delete it;



  db_->flushVLog();

  it = db_->NewIterator(options);
  it->SeekToFirst();
  while (it->Valid()) {
    newdb::Slice key = it->key();
    newdb::Slice val = it->value();
    printf("key %s, value %s\n", key.data(), val.data());
    it->Next();
  }
  return 0;
}