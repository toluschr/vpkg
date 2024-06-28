CC = gcc
CC_FLAGS = -I include -Wall -march=native -Og -ggdb

CXX = g++
CXX_FLAGS = -I include -I simdjson/singleheader/ -Wall -march=native -Og -ggdb -fpermissive -std=c++17

LD_FLAGS = -lcurl -lxbps -larchive

BIN = vpkg

OBJ += vpkg.o
OBJ += util.o
OBJ += config.o
OBJ += repodata.o
OBJ += simdini/ini.o

OBJ += list.o
OBJ += update.o
OBJ += install.o

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
