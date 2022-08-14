CC=gcc
FLAGS=-Wall -O3 -g -lpthread

all: pm

pm: daemon.o monitor.o pm.o process.o utils.o log.o
	$(CC) $(FLAGS) -o pm daemon.o monitor.o pm.o process.o utils.o log.o

%.o: %.c
	$(CC) $(FLAGS) -c -o $@ $*.c