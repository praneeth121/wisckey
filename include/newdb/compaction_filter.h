
#ifndef _compaction_filter_h_
#define _compaction_filter_h_

#include "rocksdb/compaction_filter.h"
#include "rocksdb/slice.h"
#include <set>
// #define DEBUG
namespace newdb {

class NewDbCompactionFilter : public rocksdb::CompactionFilter {
public:
  explicit NewDbCompactionFilter(std::set<uint64_t> *phy_keys_for_gc_list) : garbage_keys_(phy_keys_for_gc_list){};
  static const char *kClassName() { return "NewDbCompactionFilter"; }
  const char *Name() const override { return kClassName(); }

  virtual rocksdb::CompactionFilter::Decision
  FilterV2(int /*level*/, const rocksdb::Slice &key,
           rocksdb::CompactionFilter::ValueType value_type,
           const rocksdb::Slice &existing_value, std::string * /*new_value*/,
           std::string * /*skip_until*/) const override;

private:
  std::set<uint64_t>* garbage_keys_;
};

class NewDbCompactionFilterFactory : public rocksdb::CompactionFilterFactory {
public:
  explicit NewDbCompactionFilterFactory() {};
  ~NewDbCompactionFilterFactory() override {}

  std::unique_ptr<rocksdb::CompactionFilter> CreateCompactionFilter(
      const rocksdb::CompactionFilter::Context &context) override;
  static const char *kClassName() { return "NewDbCompactionFilterFactory"; }
  const char *Name() const override { return kClassName(); }
  void set_garbage_keys(std::set<uint64_t>* garbage_keys) {
    garbage_keys_ = garbage_keys;
  }
private:
  std::set<uint64_t>* garbage_keys_;
};

rocksdb::CompactionFilter::Decision
NewDbCompactionFilter::FilterV2(int /*level*/, const rocksdb::Slice &key,
                                rocksdb::CompactionFilter::ValueType value_type,
                                const rocksdb::Slice &existing_value,
                                std::string * /*new_value*/,
                                std::string * /*skip_until*/) const {

  if(garbage_keys_ == NULL) 
    return rocksdb::CompactionFilter::Decision::kKeep;
  auto it = garbage_keys_->find(*(uint64_t*)key.data());
  if (it != garbage_keys_->end()) {
    printf("%ld is a garbage key\n", (*(uint64_t *)key.data()));
    return rocksdb::CompactionFilter::Decision::kRemove;
  }
  return rocksdb::CompactionFilter::Decision::kKeep;
};

std::unique_ptr<rocksdb::CompactionFilter>
NewDbCompactionFilterFactory::CreateCompactionFilter(
    const rocksdb::CompactionFilter::Context & context) {
 if(context.is_manual_compaction) {
    printf("called an manual compaction\n");
    return std::unique_ptr<rocksdb::CompactionFilter>(
        new NewDbCompactionFilter(garbage_keys_));
 }
  else {
    printf("called an automatic compaction, this should not have happend at all\n");
    return std::unique_ptr<rocksdb::CompactionFilter>(nullptr);
  }
}

} // namespace newdb

#endif