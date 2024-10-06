DESTDIR := /usr/local

CC := gcc
CC_FLAGS += -I . -Wall -march=native -Og -ggdb

CXX := g++
CXX_FLAGS += -I . -Wall -march=native -Og -ggdb -fpermissive -std=c++20

CC_FLAGS += -DVPKG_REVISION='"$(shell git rev-parse --short HEAD)"'
CXX_FLAGS += -DVPKG_REVISION='"$(shell git rev-parse --short HEAD)"'

LD_FLAGS += -lcurl -lxbps -larchive

OBJ += vpkg-install/repodata.o
OBJ += vpkg-install/vpkg-install.o

OBJ += vpkg-query/vpkg-query.o

OBJ += vpkg/config.o
OBJ += vpkg/util.o

OBJ += simdini/ini.o

OBJ += tqueue/tqueue.o

DEP = $(OBJ:%.o=%.d)

.PHONY: all, clean, install

all: vpkg-install/vpkg-install vpkg-query/vpkg-query

vpkg-install/vpkg-install: \
	vpkg-install/repodata.o \
	vpkg-install/vpkg-install.o \
	tqueue/tqueue.o \
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
	-rm -f vpkg-install/vpkg-install vpkg-query/vpkg-query $(OBJ) $(DEP)

install:
	install -m644 -t /etc vpkg-sync/vpkg-sync.toml
	install -m755 -t $(DESTDIR)/bin vpkg-sync/vpkg-sync vpkg-query/vpkg-query vpkg-install/vpkg-install

%.o: %.c Makefile
	$(CC) $(CC_FLAGS) -c -MMD $< -o $@

%.o: %.cc Makefile
	$(CXX) $(CXX_FLAGS) -c -MMD $< -o $@

%.o: %.cpp Makefile
	$(CXX) $(CXX_FLAGS) -c -MMD $< -o $@

-include $(DEP)
