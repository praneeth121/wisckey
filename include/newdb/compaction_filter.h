
#ifndef _compaction_filter_h_
#define _compaction_filter_h_

#include "rocksdb/compaction_filter.h"
#include "rocksdb/slice.h"
// #define DEBUG
namespace newdb {

class NewDbCompactionFilter : public rocksdb::CompactionFilter {
public:
  explicit NewDbCompactionFilter(rocksdb::DB *keydb) : keydb_(keydb){};
  static const char *kClassName() { return "NewDbCompactionFilter"; }
  const char *Name() const override { return kClassName(); }

  virtual rocksdb::CompactionFilter::Decision
  FilterV2(int /*level*/, const rocksdb::Slice &key,
           rocksdb::CompactionFilter::ValueType value_type,
           const rocksdb::Slice &existing_value, std::string * /*new_value*/,
           std::string * /*skip_until*/) const override;

private:
  rocksdb::DB *keydb_;
};

class NewDbCompactionFilterFactory : public rocksdb::CompactionFilterFactory {
public:
  explicit NewDbCompactionFilterFactory(rocksdb::DB *keydb) : keydb_(keydb){};
  ~NewDbCompactionFilterFactory() override {}

  std::unique_ptr<rocksdb::CompactionFilter> CreateCompactionFilter(
      const rocksdb::CompactionFilter::Context &context) override;
  static const char *kClassName() { return "NewDbCompactionFilterFactory"; }
  const char *Name() const override { return kClassName(); }

private:
  rocksdb::DB *keydb_;
};

rocksdb::CompactionFilter::Decision
NewDbCompactionFilter::FilterV2(int /*level*/, const rocksdb::Slice &key,
                                rocksdb::CompactionFilter::ValueType value_type,
                                const rocksdb::Slice &existing_value,
                                std::string * /*new_value*/,
                                std::string * /*skip_until*/) const {

  fprintf(stdout, "compaction filter is called\n");
  const char *ptr = existing_value.data();
  int key_len = *((uint8_t *)ptr);
  ptr += sizeof(uint8_t);

  std::string lkey_str(ptr, key_len);
  rocksdb::Slice lkey(lkey_str.data(), key_len);
  std::string pkey;

  rocksdb::Status s = keydb_->Get(rocksdb::ReadOptions(), lkey, &pkey);
  if (s.IsNotFound()) {
#ifdef DEBUG
    printf("%s key not found\n", lkey.data());
#endif
    return rocksdb::CompactionFilter::Decision::kRemove;
  }
  if (key.ToString() != pkey) {
#ifdef DEBUG
    printf("%ld key did not match %ld\n", (*(uint64_t *)key.data()),
           (*(uint64_t *)pkey.data()));
#endif
    return rocksdb::CompactionFilter::Decision::kRemove;
  }
#ifdef DEBUG
  printf("%ld key match %ld\n", (*(uint64_t *)key.data()),
         (*(uint64_t *)pkey.data()));
#endif
  return rocksdb::CompactionFilter::Decision::kKeep;
};

std::unique_ptr<rocksdb::CompactionFilter>
NewDbCompactionFilterFactory::CreateCompactionFilter(
    const rocksdb::CompactionFilter::Context & /*context*/) {
  return std::unique_ptr<rocksdb::CompactionFilter>(
      new NewDbCompactionFilter(keydb_));
}

} // namespace newdb

#endif