

CC=g++
INCLUDES=-I../include
LIBS=-L../libs -pthread -lrocksdb -lnewdb -std=c++11
CXXFLAG=-O2 -fPIC -w -march=native
CXXFLAG_DBG=-O0 -fPIC -w -march=native -g 

all: test_rocks test_wisc

test_rocks:
	$(CC) test_rocks.cc -o test_rocks $(INCLUDES) $(LIBS) $(CXXFLAG)

test_wisc:
	$(CC) test_wisckey.cc -o test_wisc $(INCLUDES) $(LIBS) $(CXXFLAG)

test_new:
	$(CC) test_new.cc -o test_new $(INCLUDES) $(LIBS) $(CXXFLAG)

rocks_perf:
	$(CC) rocks_perf.cc -o rocks_perf $(INCLUDES) $(LIBS) $(CXXFLAG)

newdb_perf:
	$(CC) newdb_perf.cc -o newdb_perf $(INCLUDES) $(LIBS) $(CXXFLAG)

clean:
	rm -rf test_rocks test_wisc