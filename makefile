CC = gcc
CC_FLAGS = -I. -Wall -march=native -Og -ggdb

CXX = g++
CXX_FLAGS = -I. -Wall -march=native -Og -ggdb -fpermissive -std=c++17

LD_FLAGS = -lcurl -lxbps -larchive

OBJ += vpkg-install/repodata.o
OBJ += vpkg-install/vpkg-install.o

OBJ += vpkg-query/vpkg-query.o

OBJ += vpkg/config.o
OBJ += vpkg/util.o

OBJ += simdini/ini.o

DEP = $(OBJ:%.o=%.d)

.PHONY: all, clean
all: vpkg-install/vpkg-install vpkg-query/vpkg-query

vpkg-install/vpkg-install: \
	vpkg-install/repodata.o \
	vpkg-install/vpkg-install.o \
	simdini/ini.o \
	vpkg/config.o \
	vpkg/util.o
	$(CXX) $(LD_FLAGS) $^ -o $@

vpkg-query/vpkg-query: \
	vpkg-query/vpkg-query.o \
	simdini/ini.o \
	vpkg/config.o \
	vpkg/util.o
	$(CXX) $(LD_FLAGS) $^ -o $@

clean:
	-rm -f $(OBJ) $(DEP)

%.o: %.c
	$(CC) $(CC_FLAGS) -c -MMD $< -o $@

%.o: %.cc
	$(CXX) $(CXX_FLAGS) -c -MMD $< -o $@

%.o: %.cpp
	$(CXX) $(CXX_FLAGS) -c -MMD $< -o $@

%.o: makefile

-include $(DEP)
