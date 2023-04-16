rm -rf newdb_benchmark
mkdir newdb_benchmark
cd ..
make newdb 
cd test
make load_benchmark
make aging_benchmark

iostat -d 1 nvme0n1 > ExperimentalResults/load_iostats.log &
sleep 5
pid=$!
./load_bench > ExperimentalResults/load_log.txt
sleep 60
kill -9 "$pid"

iostat -d 1 nvme0n1 > ExperimentalResults/aging_iostats.log &
sleep 5
pid=$!
./aging_bench > ExperimentalResults/aging_log.txt
sleep 60
kill -9 "$pid"