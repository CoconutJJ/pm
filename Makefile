CC=gcc
FLAGS=-Wall -Og -g -lpthread

all: pm clean

pm: daemon.o monitor.o pm.o process.o utils.o log.o
	$(CC) $(FLAGS) -o pm daemon.o monitor.o pm.o process.o utils.o log.o

%.o: %.c
	$(CC) $(FLAGS) -c -o $@ $*.c

clean:
	rm *.o
