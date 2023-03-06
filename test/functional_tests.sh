cd ..
cd rocksdb-5.8
make shared_lib
cp *.so ../libs/
cd ..
make newdb
cd test
rm -rf functional_test valuedb

make functional_test
./functional_test 
rm -rf functional_test
