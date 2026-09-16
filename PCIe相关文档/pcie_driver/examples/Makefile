# Makefile for example programs

CC = gcc
CFLAGS = -Wall -Wextra -O2

all: read_link_status

read_link_status: read_link_status.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f read_link_status

.PHONY: all clean

