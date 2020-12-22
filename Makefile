NCORES = $(shell grep -c ^processor /proc/cpuinfo)
DESTDIR ?= rumprun-solo5

build:
	./build-rr.sh -j$(NCORES) -d ${DESTDIR} -o ./obj solo5

build_hw_1:
	CC=gcc ./build-rr.sh -d rumprun-hw -o ./obj hw

build_hw:
	CC=gcc ./build-rr.sh -j$(NCORES) -d rumprun-hw -o ./obj hw

clean:
	rm -rf obj*
	rm -rf rumprun
	rm -rf rumprun-solo5*
	rm -rf rumprun-hw*
	make -C solo5 clean
