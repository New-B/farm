CPP = g++ -std=c++11 -O3
INCLUDE = -I../include
LIBS = ../src/libgalloc.a ../src/libpgas.a -libverbs -lpthread
CFLAGS += -g -rdynamic

farm: farm_rw_test farm_rw_benchmark farm_partial_rw_test test_cluster dsm_test #farm_cluster_test

farm_rw_test: farm_rw_test.cc
	$(CPP) $(CFLAGS) $(INCLUDE) -o $@ $^ $(LIBS)

farm_partial_rw_test: farm_partial_rw_test.cc
	$(CPP) $(CFLAGS) $(INCLUDE) -o $@ $^ $(LIBS)

farm_rw_benchmark: farm_rw_benchmark.cc
	$(CPP) $(CFLAGS) $(INCLUDE) -o $@ $^ $(LIBS)

# farm_cluster_test: farm_cluster_test.cc
# 	$(CPP) $(CFLAGS) $(INCLUDE) -o $@ $^ $(LIBS)

test_cluster: test_cluster.cc
	$(CPP) $(CFLAGS) $(INCLUDE) -o $@ $^ $(LIBS) 

dsm_test: dsm_test.cc
	$(CPP) $(CFLAGS) $(INCLUDE) -o $@ $^ $(LIBS)

clean:
	rm -rf farm_rw_test farm_rw_benchmark farm_partial_rw_test farm_cluster_test test_cluster dsm_test