# Host tests for the platform-independent core.
#   make LIBLTC=/path/to/libltc/src      (default: ./libltc/src, see `make libltc`)
LIBLTC ?= libltc/src
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -I$(LIBLTC)
CFLAGS = -O2 -I$(LIBLTC)
LTC_OBJS = build/ltc.o build/decoder.o build/encoder.o build/timecode.o

all: test

libltc:
	git clone --depth 1 https://github.com/x42/libltc.git

build:
	mkdir -p build

build/%.o: $(LIBLTC)/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/test_core: test_core.cpp ../components/ltc_gen/ltc_core.h $(LTC_OBJS)
	$(CXX) $(CXXFLAGS) $< $(LTC_OBJS) -o $@

build/test_decode: test_decode.cpp ../components/ltc_gen/ltc_core.h $(LTC_OBJS)
	$(CXX) $(CXXFLAGS) $< $(LTC_OBJS) -o $@

build/test_sim: test_sim.cpp ../components/ltc_gen/ltc_core.h | build
	$(CXX) $(CXXFLAGS) $< -o $@

build/gen_wav: gen_wav.cpp ../components/ltc_gen/ltc_core.h | build
	$(CXX) $(CXXFLAGS) $< -o $@

# ltc_compare.py must recover a known offset (+123 us) and drift (2 ppm fast)
test_compare: build/gen_wav
	./build/gen_wav build/cmp.wav 123 2 300
	python3 ../tools/ltc_compare.py build/cmp.wav --csv build/cmp.csv
	python3 -c "import numpy as n; d=n.loadtxt('build/cmp.csv',delimiter=',',skiprows=1); \
	  s=n.polyfit(d[:,0],d[:,1],1); assert abs(s[1]-123)<25 and abs(s[0]+2)<0.05, s; print('test_compare: passed')"

test: build/test_core build/test_decode build/test_sim test_compare
	./build/test_core
	./build/test_decode
	./build/test_sim

clean:
	rm -rf build

.PHONY: all test clean libltc test_compare
