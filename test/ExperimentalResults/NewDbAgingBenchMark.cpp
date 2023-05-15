#include "omp.h"
#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include <assert.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include "newdb/db.h"
#include "newdb/slice.h"

#define MILLION 1000000
#define STATS_POOL_INT 10
#define ACCUM_GRANU 10
#define NO_OF_IOS 50 * 1024 * 1024

void usage(char *program) {
  printf("==============\n");
  printf("usage: %s -d device_path [-n num_ios] [-o op_type] [-k klen] [-v "
         "vlen] [-t threads]\n",
         program);
  printf("-d      device_path  :  kvssd device path. e.g. emul: /dev/kvemul; "
         "kdd: /dev/nvme0n1; udd: 0000:06:00.0\n");
  printf("-n      num_ios      :  total number of ios (ignore this for "
         "iterator)\n");
  printf("-o      op_type      :  1: write; 2: read; 3: delete; 4: iterator; "
         "5: key exist check\n");
  printf("-k      klen         :  key length (ignore this for iterator)\n");
  printf("-v      vlen         :  value length (ignore this for iterator)\n");
  printf("-t      threads      :  number of threads\n");
  printf("==============\n");
}

class Random {
private:
  uint32_t seed_;

public:
  explicit Random(uint32_t s) : seed_(s & 0x7fffffffu) {
    // Avoid bad seeds.
    if (seed_ == 0 || seed_ == 2147483647L) {
      seed_ = 1;
    }
  }
  uint32_t Next() {
    static const uint32_t M = 2147483647L; // 2^31-1
    static const uint64_t A = 16807;       // bits 14, 8, 7, 5, 2, 1, 0
    // We are computing
    //       seed_ = (seed_ * A) % M,    where M = 2^31-1
    //
    // seed_ must not be zero or M, or else all subsequent computed values
    // will be zero or M respectively.  For all other values, seed_ will end
    // up cycling through every number in [1,M-1]
    uint64_t product = seed_ * A;

    // Compute (product % M) using the fact that ((x << 31) % M) == x.
    seed_ = static_cast<uint32_t>((product >> 31) + (product & M));
    // The first reduction may overflow by 1 bit, so we may need to
    // repeat.  mod == M is not possible; using > allows the faster
    // sign-bit-based test.
    if (seed_ > M) {
      seed_ -= M;
    }
    return seed_;
  }
  // Returns a uniformly distributed value in the range [0..n-1]
  // REQUIRES: n > 0
  uint32_t Uniform(int n) { return Next() % n; }

  // Randomly returns true ~"1/n" of the time, and false otherwise.
  // REQUIRES: n > 0
  bool OneIn(int n) { return (Next() % n) == 0; }

  // Skewed: pick "base" uniformly from range [0,max_log] and then
  // return "base" random bits.  The effect is to pick a number in the
  // range [0,2^max_log-1] with exponential bias towards smaller numbers.
  uint32_t Skewed(int max_log) { return Uniform(1 << Uniform(max_log + 1)); }
};

class RandomGenerator {
private:
  std::string data_;
  int pos_;

public:
  RandomGenerator() {
    Random rdn(0);
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    while (data_.size() < 1048576) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      data_.append(1, (char)(' ' + rdn.Uniform(95)));
    }
    pos_ = 0;
  }

  char *Generate(size_t len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return (char *)(data_.data() + pos_ - len);
  }
};

const long FNV_OFFSET_BASIS_64 = 0xCBF29CE484222325L;
const long FNV_PRIME_64 = 1099511628211L;
long fnvhash64(long val) {
  // from http://en.wikipedia.org/wiki/Fowler_Noll_Vo_hash
  long hashval = FNV_OFFSET_BASIS_64;

  for (int i = 0; i < 8; i++) {
    long octet = val & 0x00ff;
    val = val >> 8;

    hashval = hashval ^ octet;
    hashval = hashval * FNV_PRIME_64;
    // hashval = hashval ^ octet;
  }
  return hashval;
}

std::atomic<bool> stats_end(false);
std::atomic<uint64_t> ops_keys(0);
void stats_thread(int total_ops, int pool_interval_milisec,
                  int print_interval_million, int print_interval_sec,
                  int mode /*0-by record, 1-by time*/) {
  const auto timeWindow = std::chrono::milliseconds(pool_interval_milisec);
  const auto printTimeWindow = std::chrono::seconds(print_interval_sec);
  uint64_t prev_keys = 0;
  auto prev_ts = std::chrono::system_clock::now();
  if (mode == 0) {
    while (stats_end.load(std::memory_order_relaxed) == false) {
      // print when exceed
      uint64_t curr_keys = ops_keys.load(std::memory_order_relaxed);
      if (curr_keys - prev_keys / MILLION * MILLION >= MILLION) {
        auto curr_ts = std::chrono::system_clock::now();
        std::chrono::duration<double> wctduration = (curr_ts - prev_ts);
        fprintf(
            stderr, "[%.3f sec] Throughput %.6f ops/sec, current keys %lu\n",
            wctduration.count(),
            (double)(curr_keys - prev_keys) / wctduration.count(), curr_keys);
        prev_ts = curr_ts;
        prev_keys = curr_keys;
      }

      // sleep
      std::this_thread::sleep_for(timeWindow);
    }
  } else if (mode == 1) {
    while (stats_end.load(std::memory_order_relaxed) == false) {
      // sleep
      std::this_thread::sleep_for(printTimeWindow);
      if (stats_end.load(std::memory_order_relaxed) == true)
        break;

      // print when exceed
      auto curr_ts = std::chrono::system_clock::now();
      std::chrono::duration<double> wctduration = (curr_ts - prev_ts);
      uint64_t curr_keys = ops_keys.load(std::memory_order_relaxed);
      double curr_tps = (double)(curr_keys - prev_keys) / print_interval_sec;
      fprintf(stderr,
              "[%.3f sec] Throughput %.6f ops/sec, current keys %lu, remain "
              "time (%.3f minutes)\n",
              wctduration.count(), curr_tps, curr_keys,
              (double)(total_ops - curr_keys) / curr_tps / 60);
      prev_ts = curr_ts;
      prev_keys = curr_keys;
    }
  }
}

int main(int argc, char *argv[]) {
  char *dev_path = NULL;
  char *db_type = NULL;
  int num_ios = NO_OF_IOS;
  int num_rds = 52224800;
  int op_type = 1;
  uint8_t klen = 16;
  uint32_t vlen = 4096;
  int ret, c, t = 1;
  int stats_mode = 1;
  int kmode = 0;
  int start_record = 0;
  int packThres = 4096;
  int key_offset = 0;

  while ((c = getopt(argc, argv, "d:n:o:k:v:t:m:e:g:r:s:p:i:h:db_type")) !=
         -1) {
    switch (c) {
    case 'db_type':
      db_type = optarg;
      break;
    case 'd':
      dev_path = optarg;
      break;
    case 'n':
      num_ios = atoi(optarg);
      break;
    case 'o':
      op_type = atoi(optarg);
      break;
    case 'k':
      klen = atoi(optarg);
      break;
    case 'v':
      vlen = atoi(optarg);
      break;
    case 't':
      t = atoi(optarg);
      break;
    case 'm':
      stats_mode = atoi(optarg);
      break;
    case 'e':
      kmode = atoi(optarg);
      break;
    case 'r':
      num_rds = atoi(optarg);
      break;
    case 's':
      start_record = atoi(optarg);
      break;
    case 'h':
      usage(argv[0]);
      return 0;
    default:
      usage(argv[0]);
      return 0;
    }
  }
  std::cout << num_ios << " " << klen  << " " << vlen << std::endl;
  newdb::Options options;
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
  keydbOptions.target_file_size_base = 64 * 1024 * 1024;
  keydbOptions.max_bytes_for_level_base = 64 * 1024 * 1024;
  // keydbOptions.allow_os_buffer = false;
  options.keydbOptions = keydbOptions;

  rocksdb::Options valuedbOptions;
  valuedbOptions.IncreaseParallelism();
  valuedbOptions.create_if_missing = true;
  valuedbOptions.max_open_files = -1;
  valuedbOptions.compression = rocksdb::kNoCompression;
  valuedbOptions.paranoid_checks = false;
  // valuedbOptions.allow_mmap_reads = false;
  // valuedbOptions.allow_mmap_writes = false;
  valuedbOptions.use_direct_io_for_flush_and_compaction = true;
  valuedbOptions.use_direct_reads = true;
  valuedbOptions.write_buffer_size = 64 << 20;
  valuedbOptions.target_file_size_base = 64 * 1048576;
  valuedbOptions.max_bytes_for_level_base = 64 * 1048576;
  // valuedbOptions.allow_os_buffer = false;
  options.valuedbOptions = valuedbOptions;

  options.statistics = newdb::Options::CreateDBStatistics();

  newdb::DB *db = NULL;
  newdb::DB::Open(options, "./newdb_benchmark/", &db);
  db->MAX_NUMBER_OF_FILES_FOR_COMPACTION = 2;
  db->GARBAGE_THRESHOLD = 20;

  db->setSequenceNumber(num_ios + 100);
  // start of the thread
  struct timespec t1, t2;
  clock_gettime(CLOCK_REALTIME, &t1);
  int total_ops = num_ios / 10;

  omp_set_num_threads(t);
  long test_count = 0;
  int new_key = num_ios + 1;
  long update_keys = 0, delete_keys = 0, insert_keys = 0;

  for (int aging = 0; aging < 3; aging++) {
    printf("started aging process %d\n", aging);
    srand(aging);
#pragma omp parallel for num_threads(t) shared(update_keys)                    \
    shared(delete_keys) shared(insert_keys) shared(test_count) shared(new_key)
    for (int i = 0; i < t; i++) {
#pragma omp atomic
      test_count++;
      int tid = omp_get_thread_num();
      int nthreads = omp_get_num_threads();
      printf("Thread %d started\n", tid);
      newdb::WriteOptions wropts;
      RandomGenerator gen;
      char *key = (char *)malloc(klen);
      char *value = (char *)malloc(vlen);
      int count = (total_ops) / nthreads;
      int start_key = tid * count + key_offset;
      for (int i = start_key; i < start_key + count; i++) {
        int op = rand() % 4;
        char *key_str = (char *)malloc(sizeof(long) * 2 + 1);
        long hash_key = (op == 2) ? fnvhash64(new_key) : fnvhash64(i);
        sprintf(key_str, "%0*lX", (int)sizeof(long) * 2, hash_key);
        for (int jj = 0; jj < klen; jj++) {
          memcpy(key + jj, &key_str[jj % (sizeof(long) * 2)], 1);
        }
        free(key_str);
        char *rand_val = gen.Generate(vlen);
        memcpy(value, rand_val, vlen);

        newdb::Slice db_key(key, klen);
        newdb::Slice db_val(value, vlen);

        if (op == 0) {
// perform update
#pragma omp atomic
          update_keys++;
          newdb::Status ret = db->Put(wropts, db_key, db_val);
          if (!ret.ok()) {
            printf("something wrong in update\n");
          }
        } else if (op == 1) {
          // perform delete
          newdb::Status ret = db->Delete(wropts, db_key);
#pragma omp atomic
          delete_keys++;
          if (!ret.ok()) {
            printf("something wrong in delete\n");
          }
        } else if (op == 2) {
// perform insertion
#pragma omp atomic
          insert_keys++;
#pragma omp atomic
          new_key++;
          newdb::Status ret = db->Put(wropts, db_key, db_val);
          if (!ret.ok()) {
            printf("something wrong in insert\n");
          }
          ops_keys.fetch_add(ACCUM_GRANU, std::memory_order_relaxed);
        }
      }
      if (key)
        free(key);
      if (value)
        free(value);
    }
    std::cout << "Aging Process " << aging << " done\n";
    db->vLogGarbageCollect();
  }
  std::cout << "loop count " << test_count << std::endl;
  std::cout << "keys updated " << update_keys << std::endl;
  std::cout << "keys deleted " << delete_keys << std::endl;
  std::cout << "keys inserted " << insert_keys << std::endl;
  // avg throughtput
  clock_gettime(CLOCK_REALTIME, &t2);
  unsigned long long start, end;
  start = t1.tv_sec * 1000000000L + t1.tv_nsec;
  end = t2.tv_sec * 1000000000L + t2.tv_nsec;
  double sec = (double)(end - start) / 1000000000L;
  fprintf(stdout, "Total time %.2f sec; Throughput %.2f ops/sec\n", sec,
          (double)total_ops / sec);

  // total keys operated on
  stats_end.store(true);
  sleep(1);
  // stat_thread.join();
  fprintf(stdout, "Total operation keys %lu\n", ops_keys.load());

  db->flushVLog();

  delete db;
  return 0;
}