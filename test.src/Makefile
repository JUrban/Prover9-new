# LADR/apps.src

CC = gcc

# XFLAGS can be specified on the command line (see XFLAGS below)

CFLAGS = $(XFLAGS) -O -Wall
# CFLAGS = $(XFLAGS) -O6 -Wall
# CFLAGS = $(XFLAGS) -g  -O -Wall
CFLAGS = $(XFLAGS) -g  -O0 -Wall
# CFLAGS = $(XFLAGS) -pg -O -Wall
# CFLAGS = $(XFLAGS)  -Wall -pedantic

PROGRAMS = avltest comma_num_test symbol_table_test default_weight_test renumber_variables_test memory_lifecycle_test allocator_churn_test bookkeeping_lifecycle_test ancestor_store_test ancestor_store_scale_test checkpoint_selector_test dense_selector_compaction_test dense_selector_file_scale_test dense_selector_file_order_test cold_passive_store_test hint_postings_test hint_preview_test compressed_unit_match_test hint_term_table_test hint_generalization_hash_test paramod_iterator_test hyper_iterator_test rewrite_only_store_test compact_term_pool_test compact_id_map_test compact_rewrite_test compact_unit_index_test compact_back_demod_test compact_feature_index_test compact_long_run_test proof_parent_guide_test

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

# Focused tests deliberately use debug/sanitizer CFLAGS.  Keep their copies
# of prover implementation objects local so a test build cannot silently
# replace an optimized production object that a later prover9 link reuses.
provers_%.o: ../provers.src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

avltest: avltest.o
	$(CC) $(CFLAGS) -o avltest avltest.o ../ladr/libladr.a -lm

comma_num_test: comma_num_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o comma_num_test comma_num_test.o ../ladr/libladr.a -lm

symbol_table_test: symbol_table_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o symbol_table_test symbol_table_test.o ../ladr/libladr.a -lm

default_weight_test: default_weight_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o default_weight_test default_weight_test.o ../ladr/libladr.a -lm

renumber_variables_test: renumber_variables_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o renumber_variables_test renumber_variables_test.o ../ladr/libladr.a -lm

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

checkpoint_selector_test: checkpoint_selector_test.o provers_giv_select.o provers_semantics.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o checkpoint_selector_test checkpoint_selector_test.o provers_giv_select.o provers_semantics.o ../ladr/libladr.a -lm

dense_selector_compaction_test: dense_selector_compaction_test.o provers_giv_select.o provers_semantics.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o dense_selector_compaction_test dense_selector_compaction_test.o provers_giv_select.o provers_semantics.o ../ladr/libladr.a -lm

dense_selector_file_scale_test: dense_selector_file_scale_test.o provers_giv_select.o provers_semantics.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o dense_selector_file_scale_test dense_selector_file_scale_test.o provers_giv_select.o provers_semantics.o ../ladr/libladr.a -lm

dense_selector_file_order_test: dense_selector_file_order_test.o provers_giv_select.o provers_semantics.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o dense_selector_file_order_test dense_selector_file_order_test.o provers_giv_select.o provers_semantics.o ../ladr/libladr.a -lm

cold_passive_store_test: cold_passive_store_test.o provers_cold_passive_store.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o cold_passive_store_test cold_passive_store_test.o provers_cold_passive_store.o ../ladr/libladr.a -lm

hint_postings_test: hint_postings_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o hint_postings_test hint_postings_test.o ../ladr/libladr.a -lm

hint_preview_test: hint_preview_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o hint_preview_test hint_preview_test.o ../ladr/libladr.a -lm

compressed_unit_match_test: compressed_unit_match_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o compressed_unit_match_test compressed_unit_match_test.o ../ladr/libladr.a -lm

hint_term_table_test: hint_term_table_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o hint_term_table_test hint_term_table_test.o ../ladr/libladr.a -lm

hint_generalization_hash_test: hint_generalization_hash_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o hint_generalization_hash_test hint_generalization_hash_test.o ../ladr/libladr.a -lm

paramod_iterator_test: paramod_iterator_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o paramod_iterator_test paramod_iterator_test.o ../ladr/libladr.a -lm

hyper_iterator_test: hyper_iterator_test.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o hyper_iterator_test hyper_iterator_test.o ../ladr/libladr.a -lm

rewrite_only_store_test: rewrite_only_store_test.o provers_rewrite_only_store.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o rewrite_only_store_test rewrite_only_store_test.o provers_rewrite_only_store.o ../ladr/libladr.a -lm

compact_term_pool_test: compact_term_pool_test.o provers_compact_term_pool.o provers_compact_id_map.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o compact_term_pool_test compact_term_pool_test.o provers_compact_term_pool.o provers_compact_id_map.o ../ladr/libladr.a -lm

compact_id_map_test: compact_id_map_test.o provers_compact_id_map.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o compact_id_map_test compact_id_map_test.o provers_compact_id_map.o ../ladr/libladr.a -lm

compact_rewrite_test: compact_rewrite_test.o provers_compact_rewrite.o provers_compact_term_pool.o provers_compact_id_map.o provers_compact_back_demod.o provers_demodulate.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o compact_rewrite_test compact_rewrite_test.o provers_compact_rewrite.o provers_compact_term_pool.o provers_compact_id_map.o provers_compact_back_demod.o provers_demodulate.o ../ladr/libladr.a -lm

compact_unit_index_test: compact_unit_index_test.o provers_compact_unit_index.o provers_compact_term_pool.o provers_compact_id_map.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o compact_unit_index_test compact_unit_index_test.o provers_compact_unit_index.o provers_compact_term_pool.o provers_compact_id_map.o ../ladr/libladr.a -lm

compact_back_demod_test: compact_back_demod_test.o provers_compact_back_demod.o provers_compact_term_pool.o provers_compact_id_map.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o compact_back_demod_test compact_back_demod_test.o provers_compact_back_demod.o provers_compact_term_pool.o provers_compact_id_map.o ../ladr/libladr.a -lm

compact_back_demod_test.o: ../provers.src/compact_back_demod.h

compact_feature_index_test: compact_feature_index_test.o provers_compact_feature_index.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o compact_feature_index_test compact_feature_index_test.o provers_compact_feature_index.o ../ladr/libladr.a -lm

compact_long_run_test: compact_long_run_test.o provers_compact_feature_index.o provers_compact_back_demod.o provers_compact_term_pool.o provers_compact_id_map.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o compact_long_run_test compact_long_run_test.o provers_compact_feature_index.o provers_compact_back_demod.o provers_compact_term_pool.o provers_compact_id_map.o ../ladr/libladr.a -lm

proof_parent_guide_test: proof_parent_guide_test.o provers_proof_parent_guide.o ../ladr/libladr.a
	$(CC) $(CFLAGS) -o proof_parent_guide_test proof_parent_guide_test.o provers_proof_parent_guide.o ../ladr/libladr.a -lm

compact_long_run_test.o: ../provers.src/compact_back_demod.h ../provers.src/compact_feature_index.h

memory_lifecycle_test.o allocator_churn_test.o bookkeeping_lifecycle_test.o ancestor_store_test.o: ../ladr/topform.h

bookkeeping_lifecycle_test.o: ../ladr/clause_store.h ../ladr/clauseid.h

ancestor_store_test.o: ../ladr/clause_store.h ../ladr/clauseid.h ../ladr/compress.h

cold_passive_store_test.o: ../provers.src/cold_passive_store.h

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

dense-selector-file-scale: dense_selector_file_scale_test
	./dense_selector_file_scale_test

cold-passive-store-test: cold_passive_store_test
	./cold_passive_store_test

bookkeeping-tests: bookkeeping_lifecycle_test
	./bookkeeping_lifecycle_test

hint-postings-test: hint_postings_test hint_preview_test compressed_unit_match_test hint_term_table_test hint_generalization_hash_test
	./hint_postings_test
	./hint_preview_test
	./compressed_unit_match_test
	./hint_term_table_test
	./hint_generalization_hash_test
	./hint_compiled_census_test.sh
	./hash_inference_gate_test.sh

weight-test: default_weight_test
	./default_weight_test

iterator-tests: paramod_iterator_test hyper_iterator_test
	./paramod_iterator_test
	./hyper_iterator_test

rewrite-only-store-test: rewrite_only_store_test
	./rewrite_only_store_test

compact-rewrite-test: compact_rewrite_test
	./compact_rewrite_test

compact-term-pool-test: compact_term_pool_test
	./compact_term_pool_test

compact-id-map-test: compact_id_map_test
	./compact_id_map_test

compact-unit-index-test: compact_unit_index_test
	./compact_unit_index_test

compact-back-demod-test: compact_back_demod_test
	./compact_back_demod_test

compact-feature-index-test: compact_feature_index_test
	./compact_feature_index_test

comma-num-test: comma_num_test
	./comma_num_test

symbol-table-test: symbol_table_test
	./symbol_table_test

compact-long-run-report-test: comma-num-test
	python3 compact_long_run_report_test.py
	python3 josef01_progress_audit_test.py
	python3 proof_parent_guide_extract_test.py
	sh -n cgroup_job_memory.sh
	@probe=$$(mktemp /tmp/prover9-cgroup-probe.XXXXXX); \
	status=0; ./cgroup_job_memory.sh $$probe /bin/true >/dev/null 2>&1 || status=$$?; \
	/bin/rm -f $$probe; test $$status -eq 0 -o $$status -eq 77

proof-parent-guide-test: proof_parent_guide_test
	./proof_parent_guide_test
	./proof_parent_guidance_test.sh

ancestor-store-scale: ancestor_store_scale_test
	./ancestor_store_scale_test

tptp_test: tptp_test.o
	$(CC) $(CFLAGS) -o tptp_test tptp_test.o ../ladr/libladr.a -lm
