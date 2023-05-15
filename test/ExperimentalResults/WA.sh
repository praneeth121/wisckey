# newdb
echo "Starting Newdb BenchMarking"
current_time=$(date +%Y-%m-%d_%H-%M-%S)
echo $current_time
rm -rf newdb_benchmark
mkdir newdb_benchmark
cd ../../
make newdb 
cd test/ExperimentalResults/
make newdb_load_benchmark
make newdb_aging_benchmark

# iostat -d 1 nvme0n1 > rocksdb_load_iostats.log &
echo "Starting Newdb Load BenchMarking"
iostat -d nvme0n1 > newdb_load_iostats_$current_time.log
./load_bench -n 52428800 -v 4096 > newdb_load_log_$current_time.txt
sleep 10
iostat -d nvme0n1 >> newdb_load_iostats_$current_time.log
echo "Starting Newdb Aging BenchMarking"
iostat -d nvme0n1 > newdb_aging_iostats_$current_time.log
./aging_bench -n 52428800 -v 4096 > newdb_aging_log_$current_time.txt
sleep 10
iostat -d nvme0n1 >> newdb_aging_iostats_$current_time.log

# # rocksdb
# rm -rf rocksdb_benchmark
# mkdir rocksdb_benchmark
# cd ../../
# make newdb 
# cd test/ExperimentalResults
# make rocksdb_load_benchmark
# make rocksdb_aging_benchmark

# # iostat -d 1 nvme0n1 > rocksdb_load_iostats.log &
# iostat -d nvme0n1 > rocksdb_load_iostats_$(date +%Y-%m-%d_%H-%M-%S).log
# ./load_bench > rocksdb_load_log_$(date +%Y-%m-%d_%H-%M-%S).txt
# sleep 10
# iostat -d nvme0n1 >> rocksdb_load_iostats_$(date +%Y-%m-%d_%H-%M-%S).log

# iostat -d nvme0n1 > rocksdb_aging_iostats_$(date +%Y-%m-%d_%H-%M-%S).log &
# ./aging_bench > rocksdb_aging_log_$(date +%Y-%m-%d_%H-%M-%S).txt
# sleep 10
# iostat -d nvme0n1 >> rocksdb_aging_iostats_$(date +%Y-%m-%d_%H-%M-%S).log &


rm -rf wisckeydb_benchmark
mkdir wisckeydb_benchmark
make wisckeydb_load_benchmark
make wisckeydb_aging_benchmark

# iostat -d 1 nvme0n1 > rocksdb_load_iostats.log &
current_time=$(date +%Y-%m-%d_%H-%M-%S)
echo $current_time
iostat -d nvme0n1 > wisckeydb_load_iostats_$current_time.log
./load_bench -n 52428800 -v 4096 > wisckeydb_load_log_$current_time.txt
sleep 10
iostat -d nvme0n1 >> wisckeydb_load_iostats_$current_time.log

iostat -d nvme0n1 > wisckeydb_aging_iostats_$current_time.log &
./aging_bench  -n 52428800 -v 4096 > wisckeydb_aging_log_$current_time.txt
sleep 10
iostat -d nvme0n1 >> wisckeydb_aging_iostats_$current_time.log &
