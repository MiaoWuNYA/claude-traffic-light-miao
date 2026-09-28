# link_cmd 的 host 单测：不需要 ESP-IDF，也不需要硬件
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11

test_link_cmd: test_link_cmd.c ../main/link_cmd.c ../main/link_cmd.h
	$(CC) $(CFLAGS) -o $@ test_link_cmd.c ../main/link_cmd.c

.PHONY: check clean
check: test_link_cmd
	./test_link_cmd

clean:
	rm -f test_link_cmd
