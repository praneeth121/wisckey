

CC=g++ -fopenmp
INCLUDES=-I../include
LIBS=-L../libs -pthread -lwisckey -lrocksdb -lnewdb -std=c++11
CXXFLAG=-O2 -fPIC -w -march=native -g
CXXFLAG_DBG=-O0 -fPIC -w -march=native -g 

all: test_rocks test_wisc

test_rocks:
	$(CC) test_rocks.cc -o test_rocks $(INCLUDES) $(LIBS) $(CXXFLAG)

test_wisc_:
	$(CC) test_wisckey.cc -o test_wisc $(INCLUDES) $(LIBS) $(CXXFLAG)

test_new_:
	$(CC) test_new.cc -o test_new $(INCLUDES) $(LIBS) $(CXXFLAG)

rocks_perf:
	$(CC) rocks_perf.cc -o rocks_perf $(INCLUDES) $(LIBS) $(CXXFLAG)

newdb_load_benchmark:
	$(CC) NewDbLoadBenchMark.cpp -o load_bench $(INCLUDES) $(LIBS) $(CXXFLAG)

newdb_aging_benchmark:
	$(CC) NewDbAgingBenchMark.cpp -o aging_bench $(INCLUDES) $(LIBS) $(CXXFLAG)

wisckeydb_load_benchmark:
	$(CC) WisckeyDbLoadBenchMark.cpp -o load_bench $(INCLUDES) $(LIBS) $(CXXFLAG)

wisckeydb_aging_benchmark:
	$(CC) WisckeyDbAgingBenchMark.cpp -o aging_bench $(INCLUDES) $(LIBS) $(CXXFLAG)

newdb_perf:
	$(CC) newdb_perf.cc -o newdb_perf $(INCLUDES) $(LIBS) $(CXXFLAG)

wisckey_perf:
	$(CC) wisckeydb_perf.cc -o wisckeydb_perf $(INCLUDES) $(LIBS) $(CXXFLAG)

db_bench: db_bench.cc
	$(CC) db_bench.cc -o db_bench $(INCLUDES) $(LIBS) $(CXXFLAG)

clean:
	rm -rf test_rocks test_wisc
