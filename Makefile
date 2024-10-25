include config.mk

DESTDIR := /usr/local

CC := gcc
CC_FLAGS += -I . -Wall -Wextra -march=native -Og -ggdb

CXX := g++
CXX_FLAGS += -I . -Wall -Wextra -march=native -Og -ggdb -fpermissive -std=c++20

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

all: vpkg-install/vpkg-install vpkg-query/vpkg-query vpkg-locate/vpkg-locate vpkg-sync/vpkg-sync

vpkg-locate/vpkg-locate: vpkg-locate/vpkg-locate.py
	install -m 0755 $< $@

vpkg-sync/vpkg-sync: vpkg-sync/vpkg-sync.py
	install -m 0755 $< $@

vpkg-install/vpkg-install: \
	vpkg/config.hh \
	vpkg-install/repodata.o \
	vpkg-install/vpkg-install.o \
	tqueue/tqueue.o \
	simdini/ini.o \
	vpkg/config.o \
	vpkg/util.o
	$(CXX) $(LD_FLAGS) $^ -o $@

vpkg-query/vpkg-query: \
	vpkg/config.hh \
	vpkg-query/vpkg-query.o \
	simdini/ini.o \
	vpkg/config.o \
	vpkg/util.o
	$(CXX) $(LD_FLAGS) $^ -o $@

clean:
	-rm -f vpkg/config.hh vpkg-install/vpkg-install vpkg-query/vpkg-query vpkg-sync/vpkg-sync vpkg-sync/vpkg-sync.py $(OBJ) $(DEP)

install:
	install -m644 -t /etc vpkg-sync/vpkg-sync.toml
	install -m755 -t $(DESTDIR)/bin vpkg-locate/vpkg-locate vpkg-sync/vpkg-sync vpkg-query/vpkg-query vpkg-install/vpkg-install

%.o: %.c Makefile
	$(CC) $(CC_FLAGS) -c -MMD $< -o $@

%.o: %.cc Makefile
	$(CXX) $(CXX_FLAGS) -c -MMD $< -o $@

%.o: %.cpp Makefile
	$(CXX) $(CXX_FLAGS) -c -MMD $< -o $@

%: %.in Makefile config.mk
	sed -e 's|@@VPKG_REVISION@@|$(VPKG_REVISION)|g' \
	    -e 's|@@VPKG_TEMPDIR_PATH@@|$(VPKG_TEMPDIR_PATH)|g' \
	    -e 's|@@VPKG_BINPKGS_PATH@@|$(VPKG_BINPKGS_PATH)|g' \
	    -e 's|@@VPKG_INSTALL_CONFIG_PATH@@|$(VPKG_INSTALL_CONFIG_PATH)|g' \
	    -e 's|@@VPKG_XDEB_SHLIBS_PATH@@|$(VPKG_XDEB_SHLIBS_PATH)|g' \
	    -e 's|@@VPKG_SYNC_CONFIG_PATH@@|$(VPKG_SYNC_CONFIG_PATH)|g' $< > $@

-include $(DEP)
