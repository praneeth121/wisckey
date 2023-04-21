rm -rf newdb_benchmark
mkdir newdb_benchmark
cd ..
make newdb 
cd test
make newdb_load_benchmark
make newdb_aging_benchmark

# iostat -d 1 nvme0n1 > ExperimentalResults/load_iostats.log &
iostat -d nvme0n1 > ExperimentalResults/load_iostats.log
./load_bench > ExperimentalResults/load_log.txt
sleep 10
iostat -d nvme0n1 >> ExperimentalResults/load_iostats.log

iostat -d nvme0n1 > ExperimentalResults/aging_iostats.log &
./aging_bench > ExperimentalResults/aging_log.txt
sleep 10
iostat -d nvme0n1 >> ExperimentalResults/aging_iostats.log &





# rm -rf wisckey_benchmark
# mkdir wisckey_benchmark
# cd ..
# make newdb 
# cd test
# make wisckeydb_load_benchmark
# make wisckeydb_aging_benchmark

# iostat -d 1 nvme0n1 > ExperimentalResults/wisckey_load_iostats.log &
# sleep 5
# pid=$!
# ./load_bench > ExperimentalResults/wisckey_load_log.txt
# sleep 10
# kill -9 "$pid"

# iostat -d 1 nvme0n1 > ExperimentalResults/wisckey_aging_iostats.log &
# sleep 5
# pid=$!
# ./aging_bench > ExperimentalResults/wisckey_aging_log.txt
# sleep 10
# kill -9 "$pid"
