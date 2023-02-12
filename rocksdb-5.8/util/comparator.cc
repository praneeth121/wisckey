//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <algorithm>
#include <memory>
#include <stdint.h>
#include "rocksdb/comparator.h"
#include "rocksdb/slice.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/coding.h"

namespace rocksdb {

Comparator::~Comparator() { }

namespace {
class BytewiseComparatorImpl : public Comparator {
 public:
  BytewiseComparatorImpl() { }

  virtual const char* Name() const override {
    return "leveldb.BytewiseComparator";
  }

  virtual int Compare(const Slice& a, const Slice& b) const override {
    return a.compare(b);
  }

  virtual bool Equal(const Slice& a, const Slice& b) const override {
    return a == b;
  }

  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const override {
    // Find length of common prefix
    size_t min_length = std::min(start->size(), limit.size());
    size_t diff_index = 0;
    while ((diff_index < min_length) &&
           ((*start)[diff_index] == limit[diff_index])) {
      diff_index++;
    }

    if (diff_index >= min_length) {
      // Do not shorten if one string is a prefix of the other
    } else {
      uint8_t start_byte = static_cast<uint8_t>((*start)[diff_index]);
      uint8_t limit_byte = static_cast<uint8_t>(limit[diff_index]);
      if (start_byte >= limit_byte) {
        // Cannot shorten since limit is smaller than start or start is
        // already the shortest possible.
        return;
      }
      assert(start_byte < limit_byte);

      if (diff_index < limit.size() - 1 || start_byte + 1 < limit_byte) {
        (*start)[diff_index]++;
        start->resize(diff_index + 1);
      } else {
        //     v
        // A A 1 A A A
        // A A 2
        //
        // Incrementing the current byte will make start bigger than limit, we
        // will skip this byte, and find the first non 0xFF byte in start and
        // increment it.
        diff_index++;

        while (diff_index < start->size()) {
          // Keep moving until we find the first non 0xFF byte to
          // increment it
          if (static_cast<uint8_t>((*start)[diff_index]) <
              static_cast<uint8_t>(0xff)) {
            (*start)[diff_index]++;
            start->resize(diff_index + 1);
            break;
          }
          diff_index++;
        }
      }
      assert(Compare(*start, limit) < 0);
    }
  }

  virtual void FindShortSuccessor(std::string* key) const override {
    // Find first character that can be incremented
    size_t n = key->size();
    for (size_t i = 0; i < n; i++) {
      const uint8_t byte = (*key)[i];
      if (byte != static_cast<uint8_t>(0xff)) {
        (*key)[i] = byte + 1;
        key->resize(i+1);
        return;
      }
    }
    // *key is a run of 0xffs.  Leave it alone.
  }
};

class ReverseBytewiseComparatorImpl : public BytewiseComparatorImpl {
 public:
  ReverseBytewiseComparatorImpl() { }

  virtual const char* Name() const override {
    return "rocksdb.ReverseBytewiseComparator";
  }

  virtual int Compare(const Slice& a, const Slice& b) const override {
    return -a.compare(b);
  }
};

class Uint64ComparatorImpl : public Comparator {
 public:
  Uint64ComparatorImpl() { }

  virtual const char* Name() const override {
    return "leveldb.BytewiseComparator";
  }

  virtual int Compare(const Slice& a, const Slice& b) const override {
    assert(a.size() == sizeof(uint64_t) && b.size() == sizeof(uint64_t));
    const uint64_t* left = reinterpret_cast<const uint64_t*>(a.data());
    const uint64_t* right = reinterpret_cast<const uint64_t*>(b.data());
    uint64_t leftValue;
    uint64_t rightValue;
    GetUnaligned(left, &leftValue);
    GetUnaligned(right, &rightValue);
    if (leftValue == rightValue) {
      return 0;
    } else if (leftValue < rightValue) {
      return -1;
    } else {
      return 1;
    }
  }

  virtual void FindShortestSeparator(std::string* start,
      const Slice& limit) const override {
    return;
  }

  virtual void FindShortSuccessor(std::string* key) const override {
    return;
  }
};

}// namespace

const Comparator* BytewiseComparator() {
  static BytewiseComparatorImpl bytewise;
  return &bytewise;
}

const Comparator* ReverseBytewiseComparator() {
  static ReverseBytewiseComparatorImpl rbytewise;
  return &rbytewise;
}

const Comparator* Uint64Comparator() {
  static Uint64ComparatorImpl uint64comparator;
  return &uint64comparator;
}

}  // namespace rocksdb
