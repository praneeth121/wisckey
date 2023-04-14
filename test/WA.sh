rm -rf newdb_benchmark
mkdir newdb_benchmark
cd ..
make newdb 
cd test
make load_benchmark
make aging_benchmark
iostat -d 1 > ExperimentalResults/load_iostats.log &
pid=$!
./load_bench > ExperimentalResults/load_log.txt
kill -9 "$pid"

iostat -d 1 > ExperimentalResults/aging_iostats.log &
pid=$!
./aging_bench > ExperimentalResults/aging_log.txt
kill -9 "$pid"