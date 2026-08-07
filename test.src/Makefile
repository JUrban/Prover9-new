# LADR/apps.src

CC = gcc

# XFLAGS can be specified on the command line (see XFLAGS below)

CFLAGS = $(XFLAGS) -O -Wall
# CFLAGS = $(XFLAGS) -O6 -Wall
# CFLAGS = $(XFLAGS) -g  -O -Wall
CFLAGS = $(XFLAGS) -g  -O0 -Wall
# CFLAGS = $(XFLAGS) -pg -O -Wall
# CFLAGS = $(XFLAGS)  -Wall -pedantic

PROGRAMS = avltest memory_lifecycle_test bookkeeping_lifecycle_test

all: ladr apps

ladr:
	make clean
	cd ../ladr && $(MAKE) libladr.a

clean:
	/bin/rm -f *.o

realclean:
	/bin/rm -f *.o $(PROGRAMS)

tags:
	etags *.c ../ladr/*.c

apps: $(PROGRAMS)

avltest: avltest.o
	$(CC) $(CFLAGS) -o avltest avltest.o ../ladr/libladr.a -lm

memory_lifecycle_test: memory_lifecycle_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o memory_lifecycle_test memory_lifecycle_test.o ../ladr/libladr.a -lm

bookkeeping_lifecycle_test: bookkeeping_lifecycle_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o bookkeeping_lifecycle_test bookkeeping_lifecycle_test.o ../ladr/libladr.a -lm

bookkeeping_lifecycle_test.o: ../ladr/topform.h ../ladr/clause_store.h ../ladr/clauseid.h

memory-tests: memory_lifecycle_test bookkeeping_lifecycle_test
	./memory_lifecycle_test
	./bookkeeping_lifecycle_test

bookkeeping-tests: bookkeeping_lifecycle_test
	./bookkeeping_lifecycle_test

tptp_test: tptp_test.o
	$(CC) $(CFLAGS) -o tptp_test tptp_test.o ../ladr/libladr.a -lm
