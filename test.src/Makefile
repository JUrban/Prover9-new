# LADR/apps.src

CC = gcc

# XFLAGS can be specified on the command line (see XFLAGS below)

CFLAGS = $(XFLAGS) -O -Wall
# CFLAGS = $(XFLAGS) -O6 -Wall
# CFLAGS = $(XFLAGS) -g  -O -Wall
CFLAGS = $(XFLAGS) -g  -O0 -Wall
# CFLAGS = $(XFLAGS) -pg -O -Wall
# CFLAGS = $(XFLAGS)  -Wall -pedantic

PROGRAMS = avltest memory_lifecycle_test allocator_churn_test bookkeeping_lifecycle_test ancestor_store_test ancestor_store_scale_test checkpoint_selector_test dense_selector_compaction_test hint_postings_test hint_preview_test compressed_unit_match_test paramod_iterator_test hyper_iterator_test rewrite_only_store_test compact_rewrite_test

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

allocator_churn_test: allocator_churn_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o allocator_churn_test allocator_churn_test.o ../ladr/libladr.a -lm

bookkeeping_lifecycle_test: bookkeeping_lifecycle_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o bookkeeping_lifecycle_test bookkeeping_lifecycle_test.o ../ladr/libladr.a -lm

ancestor_store_test: ancestor_store_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o ancestor_store_test ancestor_store_test.o ../ladr/libladr.a -lm

ancestor_store_scale_test: ancestor_store_scale_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o ancestor_store_scale_test ancestor_store_scale_test.o ../ladr/libladr.a -lm

checkpoint_selector_test: checkpoint_selector_test.o ../provers.src/giv_select.o ../provers.src/semantics.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o checkpoint_selector_test checkpoint_selector_test.o ../provers.src/giv_select.o ../provers.src/semantics.o ../ladr/libladr.a -lm

dense_selector_compaction_test: dense_selector_compaction_test.o ../provers.src/giv_select.o ../provers.src/semantics.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o dense_selector_compaction_test dense_selector_compaction_test.o ../provers.src/giv_select.o ../provers.src/semantics.o ../ladr/libladr.a -lm

hint_postings_test: hint_postings_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o hint_postings_test hint_postings_test.o ../ladr/libladr.a -lm

hint_preview_test: hint_preview_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o hint_preview_test hint_preview_test.o ../ladr/libladr.a -lm

compressed_unit_match_test: compressed_unit_match_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o compressed_unit_match_test compressed_unit_match_test.o ../ladr/libladr.a -lm

paramod_iterator_test: paramod_iterator_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o paramod_iterator_test paramod_iterator_test.o ../ladr/libladr.a -lm

hyper_iterator_test: hyper_iterator_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o hyper_iterator_test hyper_iterator_test.o ../ladr/libladr.a -lm

rewrite_only_store_test: rewrite_only_store_test.o ../provers.src/rewrite_only_store.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o rewrite_only_store_test rewrite_only_store_test.o ../provers.src/rewrite_only_store.o ../ladr/libladr.a -lm

compact_rewrite_test: compact_rewrite_test.o ../provers.src/compact_rewrite.o ../provers.src/demodulate.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o compact_rewrite_test compact_rewrite_test.o ../provers.src/compact_rewrite.o ../provers.src/demodulate.o ../ladr/libladr.a -lm

memory_lifecycle_test.o allocator_churn_test.o bookkeeping_lifecycle_test.o ancestor_store_test.o: ../ladr/topform.h

bookkeeping_lifecycle_test.o: ../ladr/clause_store.h ../ladr/clauseid.h

ancestor_store_test.o: ../ladr/clause_store.h ../ladr/clauseid.h ../ladr/compress.h

memory-tests: memory_lifecycle_test allocator_churn_test bookkeeping_lifecycle_test ancestor_store_test
	./memory_lifecycle_test
	./allocator_churn_test
	./bookkeeping_lifecycle_test
	./ancestor_store_test

# The object-count bound keeps this suitable for Linux/macOS CI and for a
# Node-hosted Emscripten build.  The test checks RSS contraction only on hosts
# that expose current RSS and can return pages; logical reclamation is always
# mandatory (including WebAssembly, whose linear heap cannot contract).
allocator-churn-bounded: allocator_churn_test
	./allocator_churn_test 150000

checkpoint-tests: checkpoint_selector_test dense_selector_compaction_test
	./checkpoint_selector_test
	./dense_selector_compaction_test

bookkeeping-tests: bookkeeping_lifecycle_test
	./bookkeeping_lifecycle_test

hint-postings-test: hint_postings_test hint_preview_test compressed_unit_match_test
	./hint_postings_test
	./hint_preview_test
	./compressed_unit_match_test

iterator-tests: paramod_iterator_test hyper_iterator_test
	./paramod_iterator_test
	./hyper_iterator_test

rewrite-only-store-test: rewrite_only_store_test
	./rewrite_only_store_test

compact-rewrite-test: compact_rewrite_test
	./compact_rewrite_test

ancestor-store-scale: ancestor_store_scale_test
	./ancestor_store_scale_test

tptp_test: tptp_test.o
	$(CC) $(CFLAGS) -o tptp_test tptp_test.o ../ladr/libladr.a -lm
