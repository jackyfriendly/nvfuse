#
#	NVFUSE (NVMe based File System in Userspace)
#	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
#	First Writing: 30/10/2016
#
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2, as published by the Free Software Foundation.
#
# This program is distributed in the hope it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#

include nvfuse.mk
include spdk_config.mk

LIB_NVFUSE = nvfuse.a
SRCS   = nvfuse_buffer_cache.o nvfuse_inode_cache.o \
nvfuse_core.o nvfuse_gettimeofday.o \
nvfuse_bp_tree.o nvfuse_dirhash.o \
nvfuse_misc.o nvfuse_mkfs.o nvfuse_malloc.o nvfuse_indirect.o \
nvfuse_api.o nvfuse_aio.o \
rbtree.o \
nvfuse_ipc_ring.o nvfuse_control_plane.o \
nvfuse_dep.o nvfuse_flushwork.o \
nvfuse_reactor.o nvfuse_xattr.o

LDFLAGS += -lm -lpthread -laio -lrt -luuid -lcrypto
CFLAGS = $(SPDK_CFLAGS) -Iinclude -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
CFLAGS += -march=native -m64
CFLAGS += $(WARNING_OPTION) -fPIC


OBJS=$(SRCS:.c=.o)

CC=gcc

.SUFFIXES: .c .o

# .PHONY: all clean

.c.o:
	@echo "Compiling $< ..."
	@$(RM) $@
	$(CC) $(OPTIMIZATION) $(CEPH_COMPILE) $(DEBUG) -c -D_GNU_SOURCE $(CFLAGS) -o $@ -ldl $<

#all:  $(LIB_NVFUSE) reactor helloworld libfuse regression_test perf control_plane_proc fsync_test create_1m_files mkfs #fio_plugin 
all:  $(LIB_NVFUSE) helloworld libfuse regression_test perf control_plane_proc fsync_test create_1m_files mkfs #fio_plugin 

$(LIB_NVFUSE)	:	$(OBJS)
	$(AR) rcv $@ $(OBJS)

fio_plugin:
	make -C examples/fio_plugin

reactor:
	make -C examples/reactor

helloworld:
	make -C examples/helloworld

libfuse:
	make -C examples/libfuse

regression_test:
	make -C examples/regression_test

fsync_test:
	make -C examples/fsync_test

mkfs:
	make -C examples/mkfs

xattr_test:
	make -C examples/xattr_test

create_1m_files:
	make -C examples/create_1m_files

perf:
	make -C examples/perf

control_plane_proc:
	make -C examples/control_plane_proc

clean:
	rm -f *.o *.a *~ $(LIB_NVFUSE)
	make -C examples/fio_plugin/ clean
#	make -C examples/reactor/ clean
	make -C examples/helloworld/ clean
	make -C examples/libfuse/ clean
	make -C examples/regression_test/ clean
	make -C examples/create_1m_files/ clean
	make -C examples/fsync_test/ clean
	make -C examples/perf/ clean
	make -C examples/control_plane_proc/ clean
	make -C examples/mkfs/ clean
#	make -C examples/xattr_test/ clean	

distclean:
	rm -f Makefile.bak *.o *.a *~ .depend $(LIB_NVFUSE)
install: 
	chmod 755 $(LIB_NVFUSE)
	mkdir -p ../bin/
	cp -p $(LIB_NVFUSE) /usr/bin/
uninstall:
	rm -f ../bin/$(LIB_NVFUSE)
dep:    depend

depend:
	$(CC) -MM $(CFLAGS) $(SRCS) 1>.depend

#
# include dependency files if they exist
#
ifneq ($(wildcard .depend),)
include .depend
endif

