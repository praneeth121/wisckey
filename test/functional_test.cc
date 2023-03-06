
#include "newdb/db.h"
#include "newdb/iterator.h"
#include "newdb/options.h"
#include "newdb/slice.h"
#include "newdb/status.h"

#include <stdio.h>
#include <stdlib.h>

#define TOTAL_RECORDS 1000

int main() {
  newdb::DB* db_;
  newdb::Options options_;


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
  valuedbOptions.write_buffer_size = 32 * 2048;
  valuedbOptions.target_file_size_base = 32 * 2048;
  valuedbOptions.max_bytes_for_level_base = 32 * 2048;
  options_.valuedbOptions = valuedbOptions;

  newdb::Status status = newdb::DB::Open(options_, "", &db_);
  if (status.ok())
    printf("newdb open ok\n");
  else
    printf("newdb open error\n");

  // Test Put and Get Calls
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
    assert(!strcmp(rval.data(), gval.data()));
  }

  const newdb::ReadOptions options;
  newdb::Iterator *it = db_->NewIterator(options);
  it->SeekToFirst();
  int newv = TOTAL_RECORDS;

  // test iterator and update functions
  int records_count = 0;
  std::vector<std::pair<std::string, std::string> > data;

  while (it->Valid()) {
    newdb::Slice key = it->key();
    newdb::Slice val = it->value();
    char newval[128] = {0};
    sprintf(newval, "value%ld", newv++);
    newdb::Slice rval(newval, 128);
    data.push_back({key.ToString(), rval.ToString()});
    db_->Put(newdb::WriteOptions(), key, rval);
    it->Next();
    records_count++;
  }
  assert(records_count == TOTAL_RECORDS);

  for(int i = 0;i < TOTAL_RECORDS;i++) {
    newdb::Slice key(data[i].first);
    std::string gval;
    db_->Get(newdb::ReadOptions(), key, &gval);
    assert(!strcmp(gval.data(), data[i].second.data()));
  }

  // test delete
  for(int i = 0;i < TOTAL_RECORDS;i++) {
    newdb::Slice key(data[i].first);
    std::string gval;
    if(i%2)
      db_->Delete(newdb::WriteOptions(), key);
  }

  for(int i = 0;i < TOTAL_RECORDS;i++) {
    newdb::Slice key(data[i].first);
    std::string gval;
    newdb::Status s = db_->Get(newdb::ReadOptions(), key, &gval);
    if(i%2)
      assert(s.IsNotFound());
    else 
      assert(!strcmp(gval.data(), data[i].second.data()));
  }


  // test for GC Implementation
  db_->runGC();


}

