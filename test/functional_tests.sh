cd ..
make newdb
cd test
rm -rf functional_test valuedb
make functional_test
./functional_test 
rm -rf functional_test
