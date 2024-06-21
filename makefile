CC = gcc
CC_FLAGS = -Wall -march=native

CXX = g++
CXX_FLAGS = -Wall -march=native

LD_FLAGS = -lcurl -lxbps

BIN = vpkg

OBJ += vpkg.o
OBJ += config.o
OBJ += simdini/ini.o

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

%.o: makefile

-include "$(DEP)"
