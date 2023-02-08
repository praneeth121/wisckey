
#include "newdb/db.h"
#include "newdb/iterator.h"
#include "newdb/options.h"
#include "newdb/slice.h"
#include "newdb/status.h"

#include <stdio.h>
#include <stdlib.h>

#define TOTAL_RECORDS 10

int main() {
  newdb::DB *db_;
  newdb::Options options_;
  options_.statistics = newdb::Options::CreateDBStatistics();

  // apply db options
  newdb::Status status = newdb::DB::Open(options_, "test_newdb", &db_);
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

  // read back updated value
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