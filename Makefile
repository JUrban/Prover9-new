help:
	@echo See README.make

all:
	cd ladr         && $(MAKE) lib
	cd mace4.src    && $(MAKE) all
	cd provers.src  && $(MAKE) all
	cd apps.src     && $(MAKE) all
	/bin/cp -p utilities/* bin
	@echo ""
	@echo "**** Now try 'make test1'. ****"
	@echo ""

.PHONY: lto-check prover9-lto

# GNU make supplies `cc` as a built-in default even though the component
# Makefiles default to gcc.  Use the same compiler for the probe and build,
# while preserving an explicit `make ... CC=clang` override.
LTO_CC = $(if $(filter default,$(origin CC)),gcc,$(CC))

# Fail before rebuilding libraries if the selected compiler/linker does not
# support portable link-time optimization.  Normal release builds are still
# available on older toolchains.
lto-check:
	@tmp=$$(mktemp /tmp/prover9-lto-check.XXXXXX); \
	trap '/bin/rm -f "$$tmp"' EXIT; \
	if ! printf '%s\n' 'int main(void) { return 0; }' | \
	     $(LTO_CC) -O2 -flto -x c - -o "$$tmp" >/dev/null 2>&1; then \
	  echo "Compiler/linker does not support -flto; use the normal release build." >&2; \
	  exit 1; \
	fi

# Build only the CPU-critical prover and LADR library at portable -O2 -flto,
# then install the resulting executable in bin/.
prover9-lto: lto-check
	$(MAKE) -C ladr libladr.a LTO=1 CC="$(LTO_CC)"
	$(MAKE) -C provers.src prover9 LTO=1 CC="$(LTO_CC)"
	/bin/cp -p provers.src/prover9 bin/prover9

ladr lib:
	cd ladr         && $(MAKE) lib

test1:
	bin/prover9 -f prover9.examples/x2.in < /dev/null | bin/prooftrans parents_only
	@echo ""
	@echo "**** If you see a proof, prover9 is probably okay. ****"
	@echo "**** Next try 'make test2'. ****"
	@echo ""

test2:
	bin/mace4 -v0 -f mace4.examples/group2.in | bin/interpformat tabular
	@echo ""
	@echo "**** If you see a group table, mace4 is probably okay. ****"
	@echo "**** Next try 'make test3'. ****"
	@echo ""

test3:
	bin/mace4 -n3 -m -1 < apps.examples/qg.in | bin/interpformat | bin/isofilter
	@echo ""
	@echo "*** If you see 5 interpretations, the apps are probably okay. ***"
	@echo "**** Next try 'make test4'. ****"
	@echo ""

test4:
	@echo "---- PUZ001-1 (CNF) ----"
	@bin/prover9 -f tptp.examples/PUZ001-1.p < /dev/null 2>&1 | grep "SZS status"
	@echo "---- PUZ001+1 (FOF) ----"
	@bin/prover9 -f tptp.examples/PUZ001+1.p < /dev/null 2>&1 | grep "SZS status"
	@echo ""
	@echo "**** Expected: Unsatisfiable, Theorem ****"
	@echo "**** If both match, TPTP mode is okay. ****"
	@echo "**** Next try 'make test5'. ****"
	@echo ""

test5:
	@bin/prover9 -f prover9.examples/sine_test.in < /dev/null 2>&1 | sed -n '/SInE:/p; /PROOF/,/end of proof/p'
	@echo ""
	@echo "**** If you see SInE stats and a proof, SInE in LADR mode is okay. ****"
	@echo "**** Next try 'make test6'. ****"
	@echo ""

test6:
	@echo "---- mace4 TPTP: PUZ001+1 (should GaveUp - no countermodel) ----"
	@bin/mace4 -tptp -t 5 -N 10 -f tptp.examples/PUZ001+1.p < /dev/null 2>&1 | grep "SZS status"
	@echo "---- mace4 TPTP: GRP001+1 (should CounterSatisfiable) ----"
	@bin/mace4 -tptp -t 5 -N 10 -f tptp.examples/GRP001+1.p < /dev/null 2>&1 | grep "SZS status"
	@echo ""
	@echo "**** Expected: GaveUp, CounterSatisfiable ****"
	@echo "**** If both match, mace4 TPTP mode is okay. ****"
	@echo ""
	@echo "*** All of the programs are in ./bin, and they can be copied anywhere you like. ***"
	@echo ""

memory-tests:
	cd test.src && $(MAKE) memory-tests

bookkeeping-tests:
	cd test.src && $(MAKE) bookkeeping-tests

proof-parent-guide-test: lib
	$(MAKE) -C provers.src prover9
	$(MAKE) -C test.src proof-parent-guide-test


hint-postings-test: lib
	cd test.src && $(MAKE) hint-postings-test

discount-tests: all
	./test.src/discount_loop_test.sh
	./test.src/eager_demod_test.sh
	./test.src/collective_frontier_test.sh
	./test.src/hint_index_trace_test.sh
	./test.src/hint_checkpoint_test.sh
	./test.src/dense_passive_test.sh
	cd test.src && $(MAKE) checkpoint-tests

compact-frontier-tests: all
	./test.src/compact_otter_audit_test.sh
	./test.src/compact_otter_checkpoint_test.sh
	cd test.src && $(MAKE) cold-passive-store-test compact-id-map-test compact-rewrite-test compact-unit-index-test compact-back-demod-test compact-feature-index-test

compact-generalization-validate:
	./test.src/validate_compact_generalization_manifest.sh

compact-generalization-smoke: all
	./test.src/compact_generalization_smoke_test.sh

# Exact supplied Osborn-prefix replay.  This is intentionally separate from
# the fast default suites; it runs when rr_osbe.in.gz is available beside the
# repository or through OSBORN_INPUT=/path/to/rr_osbe.in.gz.
osborn-trajectory-test: all
	./test.src/osborn_compact_trajectory_test.sh

long-run-scalability-tests: all
	cd test.src && $(MAKE) compact_long_run_test ancestor_store_scale_test
	./test.src/compact_long_run_test
	cd test.src && $(MAKE) compact-long-run-report-test

clean:
	cd ladr             && $(MAKE) realclean
	cd apps.src         && $(MAKE) realclean
	cd mace4.src        && $(MAKE) realclean
	cd provers.src      && $(MAKE) realclean

realclean:
	$(MAKE) clean
	/bin/rm -f bin/*

pgo-merge:
	@if $(CC) --version 2>/dev/null | head -1 | grep -qi clang; then \
	  xcrun llvm-profdata merge -output=pgo_data/default.profdata pgo_data/*.profraw 2>/dev/null || \
	  llvm-profdata merge -output=pgo_data/default.profdata pgo_data/*.profraw; \
	else \
	  echo "GCC uses accumulated .gcda profiles directly; no merge is needed."; \
	fi

pgo-clean:
	/bin/rm -rf pgo_data


# The following cleans up, then makes a .tar.gz file of the current
# directory, leaving it in the parent directory.  (Gnu make only.)

DIR = $(shell basename $(PWD))

dist:
	$(MAKE) realclean
	cd .. && tar cvf $(DIR).tar $(DIR)
	gzip -f ../$(DIR).tar
	ls -l ../$(DIR).tar.gz
