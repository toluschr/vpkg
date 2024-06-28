CC = gcc
CC_FLAGS = -I. -Wall -march=native -Og -ggdb

CXX = g++
CXX_FLAGS = -I. -Wall -march=native -Og -ggdb -fpermissive -std=c++17

LD_FLAGS = -lcurl -lxbps -larchive

BIN = vpkg/vpkg

OBJ += vpkg/vpkg.o
OBJ += vpkg/util.o
OBJ += vpkg/config.o
OBJ += vpkg/repodata.o
OBJ += simdini/ini.o

OBJ += vpkg/list.o
OBJ += vpkg/update.o
OBJ += vpkg/install.o

DEP = $(OBJ:%.o=%.d)

.PHONY: all, clean
all: $(BIN)

clean:
	-rm -f $(BIN) $(OBJ) $(DEP)

$(BIN): $(OBJ)
	$(CXX) $(LD_FLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CC_FLAGS) -c -MMD $< -o $@

%.o: %.cc
	$(CXX) $(CXX_FLAGS) -c -MMD $< -o $@

%.o: %.cpp
	$(CXX) $(CXX_FLAGS) -c -MMD $< -o $@

%.o: makefile

-include $(DEP)
