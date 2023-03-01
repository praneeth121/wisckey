#include "newdb/mapping_table.h"
#include "libcuckoo/cuckoohash_map.hh"
#include <fstream>
#include <map>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <unordered_map>

namespace newdb {

class MemMap : public Map {
  friend class MemMapIterator;

private:
  std::unordered_map<std::string, phy_key> key_map_;
  std::mutex lock_;
  uint64_t serializedSize();
  void serialize(char *filename);
  void deserialize(char *filename);

public:
  class MemMapIterator : public MapIterator {
  private:
    MemMap *map_;
    std::unordered_map<std::string, phy_key>::iterator it_;
    std::string curr_key_;
    std::string curr_val_;

  public:
    explicit MemMapIterator(MemMap *map) : map_(map), it_(NULL) {}
    ~MemMapIterator() {}
    void Seek(std::string &key) {
      std::lock_guard<std::mutex> guard(map_->lock_);
      // it_ = map_->key_map_.lower_bound(key);
      it_ = map_->key_map_.end();
      if (it_ != map_->key_map_.end()) {
        curr_key_ = it_->first;
        curr_val_ = it_->second.ToString();
      }
    }
    void SeekToFirst() {
      std::lock_guard<std::mutex> guard(map_->lock_);
      it_ = map_->key_map_.begin();
      if (it_ != map_->key_map_.end()) {
        curr_key_ = it_->first;
        curr_val_ = it_->second.ToString();
      }
    }
    void Next() {
      std::lock_guard<std::mutex> guard(map_->lock_);
      ++it_;
      if (it_ != map_->key_map_.end()) {
        curr_key_ = it_->first;
        curr_val_ = it_->second.ToString();
      }
    }
    bool Valid() {
      std::lock_guard<std::mutex> guard(map_->lock_);
      return it_ != map_->key_map_.end();
    }
    std::string &Key() { return curr_key_; }
    std::string &Value() { return curr_val_; }
  };

  MemMap() {
    key_map_.reserve(200e6); // reserve 200M buckets (avoid rehashing)
    std::ifstream f("mapping_table.log", std::ifstream::in | std::ios::binary);
    if (f) {
      deserialize("mapping_table.log");
    }
  };

  ~MemMap() { serialize("mapping_table.log"); };

  bool lookup(std::string *key,
              phy_key *val) { // returns the value when available
    std::lock_guard<std::mutex> guard(lock_);
    auto it = key_map_.find(*key);
    bool exist = (it != key_map_.end());
    if (exist)
      *val = it->second;

    return exist;
  }

  bool readmodifywrite(std::string *key, phy_key *rd_val, phy_key *wr_val) {
    // updated this function to update the key irrespective of whether keys
    // exists in the mapping_table
    bool exist;
    std::lock_guard<std::mutex> guard(lock_);
    auto it = key_map_.find(*key);
    assert(it != key_map_.end());
    exist = (it != key_map_.end());
    if (exist) {
      *rd_val = it->second;
    }
    key_map_[*key] = *wr_val;
    return exist;
  }

  bool readtestupdate(std::string *key, phy_key *rd_val, phy_key *old_val,
                      phy_key *new_val) {
    bool match;
    std::lock_guard<std::mutex> guard(lock_);
    auto it = key_map_.find(*key);
    assert(it != key_map_.end());
    *rd_val = it->second;
    match = (*rd_val == *old_val);
    if (match) { // active KV not being updated
      key_map_[*key] = *new_val;
    } else { // active KV got updated before REPLACE
             // printf("rare case when doing GC\n"); // TODO
    }

    return match;
  }

  void insert(std::string *key, phy_key *val) {
    std::lock_guard<std::mutex> guard(lock_);
    key_map_.insert(std::make_pair(*key, *val));
  }

  void update(std::string *key, phy_key *val) {
    std::lock_guard<std::mutex> guard(lock_);
    // already know key exist
    key_map_[*key] = *val;
  }

  void erase(std::string *key) {
    std::lock_guard<std::mutex> guard(lock_);
    key_map_.erase(*key);
  }

  MapIterator *NewMapIterator() { return NULL; }
};

uint64_t MemMap::serializedSize() {
  uint64_t size = 0;
  for (auto it = key_map_.begin(); it != key_map_.end(); ++it) {
    // log_key str, len(u8), phy_key(u64)
    size += it->first.size() + sizeof(uint64_t);
  }
  return size; // first u64, blob size;
}

void MemMap::serialize(char *filename) {
  // TODO: Needs to be updated
  // save data to archive
  uint64_t size = serializedSize();
  char *buf = (char *)malloc(size);
  char *data = buf;
  *(uint64_t *)data = size - sizeof(uint64_t);
  data += sizeof(uint64_t);
  for (auto it = key_map_.begin(); it != key_map_.end(); ++it) {
    uint8_t key_size = (uint8_t)it->first.size();
    // log_key len (u8)
    *(uint8_t *)data = key_size;
    data += sizeof(uint8_t);
    // log_key str
    memcpy(data, it->first.c_str(), key_size);
    data += key_size;
    // phy_key
    *(uint64_t *)data = it->second.get_phykey();
    data += sizeof(uint64_t);
  }
  // write to file
  std::ofstream ofs(filename, std::ofstream::out | std::ios::binary);
  ofs.write(buf, size);

  // clean up
  free(buf);
}

void MemMap::deserialize(char *filename) {
  // TODO: Needs to be updated
  std::ifstream ifs(filename, std::ifstream::in | std::ios::binary);
  // create and open an archive for input
  uint64_t blob_size;
  ifs.read((char *)&blob_size, sizeof(uint64_t));
  char *data = (char *)malloc(blob_size);
  ifs.read(data, blob_size);
  // read from archive to data structure
  char *p = data;
  while (blob_size > 0) {
    // key len (u8)
    uint8_t key_size = *(uint8_t *)p;
    p += sizeof(uint8_t);
    blob_size -= sizeof(uint8_t);
    // log_key
    std::string logkey(p, key_size);
    p += key_size;
    blob_size -= key_size;
    // phy_key
    phy_key phykey(*(uint64_t *)p);
    p += sizeof(uint64_t);
    blob_size -= sizeof(uint64_t);

    key_map_.insert(std::make_pair(logkey, phykey));
  }

  // clean up
  free(data);
}

class CuckooMap : public Map {
  friend class CuckooMapIterator;

private:
  cuckoohash_map<std::string, phy_key> key_map_;

  uint64_t serializedSize();
  void serialize(char *filename);
  void deserialize(char *filename);

public:
  class CuckooMapIterator : public MapIterator {
  private:
    cuckoohash_map<std::string, phy_key> *key_map_;
    cuckoohash_map<std::string, phy_key>::locked_table::iterator it_;
    std::string curr_key_;
    std::string curr_val_;

  public:
    explicit CuckooMapIterator(cuckoohash_map<std::string, phy_key> *map)
        : key_map_(map) {}
    ~CuckooMapIterator() {}
    void Seek(std::string &key) {
      auto lt = key_map_->lock_table();
      it_ = lt.find(key);
      curr_key_ = it_->first;
      curr_val_ = it_->second.ToString();
    }
    void SeekToFirst() {
      auto lt = key_map_->lock_table();
      it_ = lt.begin();
      if (it_ != lt.end()) {
        curr_key_ = it_->first;
        curr_val_ = it_->second.ToString();
      }
    }
    void Next() {
      auto lt = key_map_->lock_table();
      ++it_;
      if (it_ != lt.end()) {
        curr_key_ = it_->first;
        curr_val_ = it_->second.ToString();
      }
    }
    bool Valid() {
      auto lt = key_map_->lock_table();
      return it_ != lt.end();
    }
    std::string &Key() { return curr_key_; }
    std::string &Value() { return curr_val_; }
  };
  CuckooMap() {
    key_map_.reserve(200e6); // reserve 200M buckets (avoid rehashing)
    std::ifstream f("mapping_table.log", std::ifstream::in | std::ios::binary);
    if (f) {
      deserialize("mapping_table.log");
    }
  };
  ~CuckooMap() { serialize("mapping_table.log"); };

  bool lookup(std::string *key, phy_key *val) {
    bool exist = key_map_.find(*key, *val);

    return exist;
  }

  bool readmodifywrite(std::string *key, phy_key *rd_val, phy_key *wr_val) {
    int retry_cnt = 0;
    bool exist = key_map_.find(*key, *rd_val);
    while (!exist && retry_cnt < 3) {
      usleep(100);
      retry_cnt++;
      exist = key_map_.find(*key, *rd_val);
    }
    assert(exist);
    if (exist) {
      key_map_.update(*key, *wr_val);
    }

    return exist;
  }

  bool readtestupdate(std::string *key, phy_key *rd_val, phy_key *old_val,
                      phy_key *new_val) {
    bool match;
    int retry_cnt = 0;
    bool exist = key_map_.find(*key, *rd_val);
    while (!exist && retry_cnt < 3) {
      usleep(100);
      retry_cnt++;
      exist = key_map_.find(*key, *rd_val);
    }
    assert(exist);
    match = (*rd_val == *old_val);
    if (match) { // active KV not being updated
      key_map_.update(*key, *new_val);
    } else { // active KV got updated before REPLACE
             // printf("rare case when doing GC\n"); // TODO
    }

    return match;
  }

  void insert(std::string *key, phy_key *val) { key_map_.insert(*key, *val); }

  void update(std::string *key, phy_key *val) { key_map_.update(*key, *val); }

  void erase(std::string *key) { key_map_.erase(*key); }

  MapIterator *NewMapIterator() { return new CuckooMapIterator(&key_map_); }
};

uint64_t CuckooMap::serializedSize() {
  uint64_t size = 0;
  auto lt = key_map_.lock_table();
  for (const auto &it : lt) {
    // log_key str, len(u8), phy_key(u64)
    size += it.first.size() + sizeof(uint8_t) + sizeof(uint64_t);
  }
  return size + sizeof(uint64_t); // first u64, blob size;
}

void CuckooMap::serialize(char *filename) {
  // save data to archive
  uint64_t size = serializedSize();
  char *buf = (char *)malloc(size);
  char *data = buf;
  *(uint64_t *)data = size - sizeof(uint64_t);
  data += sizeof(uint64_t);
  auto lt = key_map_.lock_table();
  for (const auto &it : lt) {
    uint8_t key_size = (uint8_t)it.first.size();
    // log_key len (u8)
    *(uint8_t *)data = key_size;
    data += sizeof(uint8_t);
    // log_key str
    memcpy(data, it.first.c_str(), key_size);
    data += key_size;
    // phy_key
    *(uint64_t *)data = it.second.get_phykey();
    data += sizeof(uint64_t);
  }
  // write to file
  std::ofstream ofs(filename, std::ofstream::out | std::ios::binary |
                                  std::ofstream::trunc);
  ofs.write(buf, size);

  // clean up
  free(buf);
}

void CuckooMap::deserialize(char *filename) {
  std::ifstream ifs(filename, std::ifstream::in | std::ios::binary);
  // create and open an archive for input
  uint64_t blob_size;
  ifs.read((char *)&blob_size, sizeof(uint64_t));
  char *data = (char *)malloc(blob_size);
  ifs.read(data, blob_size);
  // read from archive to data structure
  char *p = data;
  while (blob_size > 0) {
    // key len (u8)
    uint8_t key_size = *(uint8_t *)p;
    p += sizeof(uint8_t);
    blob_size -= sizeof(uint8_t);
    // log_key
    std::string logkey(p, key_size);
    p += key_size;
    blob_size -= key_size;
    // phy_key
    phy_key phykey(*(uint64_t *)p);
    p += sizeof(uint64_t);
    blob_size -= sizeof(uint64_t);

    key_map_.insert(logkey, phykey);
  }

  // clean up
  free(data);
}

Map *NewMemMap() { return new MemMap; }

Map *NewCuckooMap() { return new CuckooMap; }

} // namespace newdb