# Host build of the MiSTer-side ZZ9000 blitter service + tests.
#   make        build and run under AddressSanitizer/UBSan
ROOT    := ../..
CFLAGS  ?= -std=gnu99 -O1 -g -Wall -Wno-unused-function
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer
INC     := -I$(ROOT)/common -I$(ROOT)/mister
SRC     := zz9000_host_test.c $(ROOT)/mister/zz9000_service.c \
           $(ROOT)/mister/zz9000_blit.c $(ROOT)/mister/zz9000_fbmem.c

all: test

zz9000_host_test: $(SRC) $(wildcard $(ROOT)/mister/*.h $(ROOT)/mister/zz9000os/*) $(ROOT)/common/mister_zz9000.h
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ $(SRC) -lm

test: zz9000_host_test
	./zz9000_host_test

clean:
	rm -f zz9000_host_test

.PHONY: all test clean
