/*  Copyright (C) 2006, 2007 William McCune

    This file is part of the LADR Deduction Library.

    The LADR Deduction Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License,
    version 2.

    The LADR Deduction Library is distributed in the hope that it will be
    useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with the LADR Deduction Library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef TP_SEARCH_STRUCTURES_H
#define TP_SEARCH_STRUCTURES_H

#include "../ladr/ladr.h"

// Attributes

typedef struct prover_attributes * Prover_attributes;

struct prover_attributes {
  // These are attribute IDs
  int label,
    bsub_hint_wt,
    answer,
    properties,
    action,
    action2;
};

// Options

typedef struct prover_options * Prover_options;

struct prover_options {
  // These are integer IDs assigned by init_flag(), init_parm, init_stringparm.

  // Flags (Boolean options)
  int 
    binary_resolution,       // inference rules
    neg_binary_resolution,
    hyper_resolution,
    pos_hyper_resolution,
    neg_hyper_resolution,
    ur_resolution,
    pos_ur_resolution,
    neg_ur_resolution,
    paramodulation,

    ordered_res,        // restrictions on inference rules
    ordered_para,
    check_res_instances,
    check_para_instances,
    para_units_only,
    para_from_vars,
    para_into_vars,
    para_from_small,
    basic_paramodulation,
    initial_nuclei,

    process_initial_sos,     // processing generated clauses
    back_demod,
    lex_dep_demod,
    lex_dep_demod_sane,
    safe_unit_conflict,
    reuse_denials,
    back_subsume,
    back_subsume_skip_used,
    back_subsume_skip_limbo,
    ancestor_subsume,       // block subsumption of variant with shorter proof
    proof_weight,           // anc_subsume metric: tree-leaves vs DAG-nodes
    unit_deletion,
    factor,
    cac_redundancy,
    degrade_hints,
    limit_hint_matchers,
    back_demod_hints,
    collect_hint_labels,
    hint_match_stats,        // print hint match count stats at end of search
    hint_match_once,         // unindex hint immediately after first match
    hint_trace,              // exact committed-candidate hint tuple
    search_event_trace,      // lightweight kept/given identity oracle
    compact_otter_audit,     // dual-run legacy/compact OTTER demodulation
    compact_otter_demodulation, // compact bank is authoritative in OTTER
    compact_unit_subsumption_audit, // compare stable-ID/legacy unit answers
    compact_otter_unit_index, // pointer-free unit index is authoritative
    compact_back_demod_audit, // compare stable-ID/legacy redex answers
    compact_otter_back_demod_index, // pointer-free redex index authoritative
    compact_nonunit_subsumption_audit, // compare compact/legacy feature trie
    compact_otter_nonunit_index, // stable-ID nonunit feature trie authoritative
    compact_term_sharing_stats, // opt-in exact cross-clause term profile
    collective_trace,        // one line per collective batch expansion
    collective_hint_probes,  // one bounded early expansion for hinted givens
    collective_promising_candidates, // raw-weight order within inference set
    collective_promising_scheduler, // prioritize known next raw keys
    collective_hint_discovery, // bounded dual-cursor hint lookahead
    print_matched_hints,     // print matched/unmatched hints per proof
    print_derivations,       // print derivation for clauses in hitlist file
    derivations_only,        // exit after last hitlist derivation
    print_new_hints,         // list non-hint-matcher proof clauses
    dont_flip_input,
    eval_rewrite,

    echo_input,              // output
    bell,
    quiet,
    print_initial_clauses,
    print_given,
    print_gen,
    print_kept,
    print_labeled,
    print_proofs,
    print_proof_goal,
    print_expanded_proof,   // unroll compound rewrite/resolve steps
    print_substitutions,    // append {var <- term, ...} rider after each inference
    default_output,
    print_clause_properties,

    expand_relational_defs,
    predicate_elim,
    inverse_order,
    sort_initial_sos,
    restrict_denials,

    input_sos_first,        // selecting given clause
    breadth_first,
    lightest_first,
    default_parts,
    random_given,
    breadth_first_hints,

    automatic,             // auto is a reserved symbol in C!
    auto_setup,
    auto_limits,
    auto_denials,
    auto_inference,
    auto_process,
    auto2,                 // enhanced auto mode
    raw,
    production,

    lex_order_vars,       // others
    ignore_option_dependencies,
    comma_stats,          // format statistics with commas
    report_index_stats,   // include FPA index stats in reports
    compress_disabled,    // compact bodies of clauses retained only as ancestors

    checkpoint_exit,      // exit after writing checkpoint
    checkpoint_ancestors, // include disabled list in checkpoint
    checkpoint_verify,    // verify data structure hashes on resume
    tptp_output,          // TPTP/TSTP output mode
    multi_order_trial,    // try multiple symbol orderings, pick best
    fast_pred_elim;       // 5-second timeout on predicate elimination

  // Parms (Integer options)

  int
    max_given,             // search limits
    max_kept,
    max_proofs,
    max_megs,
    max_seconds,
    max_minutes,
    max_hours,
    max_days,
    cnf_clause_limit,
    definitional_cnf,

    new_constants,        // inference
    para_lit_limit,
    ur_nucleus_limit,
    collective_given_ratio, // givens per collective descriptor expansion
    collective_candidate_chunk, // max collective conclusions per turn
    collective_candidate_cache, // max passives exposed by collective turns
    collective_raw_work_budget, // max iterator positions examined per turn
    collective_promising_fair_interval, // one FIFO turn per N expansions
    collective_descriptor_high_water, // hard balanced descriptor bound
    collective_descriptor_low_water,  // balanced drain exit threshold
    collective_oldest_lag_limit,      // activation lag forcing drain mode
    collective_balanced_fair_interval, // one oldest turn per N lane turns
    collective_paramod_share,         // weighted balanced lane share
    collective_pos_hyper_share,       // weighted balanced lane share
    collective_neg_hyper_share,       // weighted balanced lane share
    collective_candidate_window,      // target shared raw-candidate window
    collective_candidate_commit_interval, // max expansion turns before commit
    collective_candidate_fair_interval, // one oldest pool commit per N
    collective_discovery_raw_budget, // raw iterator steps per lookahead turn
    collective_discovery_distance, // max conclusions ahead of fair cursor
    collective_discovery_promotion_cap, // max ahead-consumed records per item
    collective_discovery_general_interval, // one ordinary lookahead per N
    collective_discovery_turn_interval, // mandatory fair expansion per N turns

    fold_denial_max,

    pick_given_ratio,      // select given clause
    hints_part,
    age_part,
    weight_part,
    true_part,
    false_part,
    random_part,
    random_seed,
    eval_limit,
    eval_var_limit,

    max_weight,            // processing generated clauses
    max_depth,
    lex_dep_demod_lim,
    max_literals,
    max_vars,
    demod_step_limit,
    demod_increase_limit,
    max_nohints,             // exit after N consecutive givens w/o hint match
    degrade_limit,           // hint matcher only if degradation_count <= N
    para_restr_beg,          // restricted paramod: skip if both IDs in range
    para_restr_end,
    backsub_check,

    variable_weight,        // weighting parameters
    constant_weight,
    not_weight,
    or_weight,
    sk_constant_weight,
    prop_atom_weight,
    nest_penalty,
    depth_penalty,
    var_penalty,
    default_weight,
    complexity,
    sine_weight,

    sos_limit,             // control size of SOS
    sos_keep_factor,
    min_sos_limit,
    lrs_interval,
    lrs_ticks,

    report,
    report_stderr,
    report_given,  // report every N given clauses
    report_preprocessing,  // report preprocessing progress every N seconds
    compact_passive_cache, // MiB cap for decoded compact-OTTER passive bodies
    compact_term_reclaim_kb, // minimum estimated stale token payload to compact
    compact_index_stale_pct, // inactive physical records before index rebuild
    fpa_depth,     // FPA index depth (higher = more selective, more memory)
    candidate_warn_limit,  // warn when candidates exceed this
    candidate_hard_limit,  // skip inference when candidates exceed this
    checkpoint_minutes,    // periodic checkpoint interval in minutes (-1 = off)
    checkpoint_given,      // one-shot deterministic checkpoint at given count
    checkpoint_candidate_pool, // one-shot at collective pool occupancy
    checkpoint_discovery_promotions, // one-shot after N lookahead promotions
    checkpoint_keep,       // max auto-checkpoint dirs to retain (default 3)
    sine,                  // SInE premise selection (-1=auto, 0=off, >0=tolerancex100)
    sine_depth,            // SInE BFS depth limit (0=unlimited/fixpoint)
    sine_max_axioms,       // SInE max selected axioms (0=unlimited)
    cl_to_trace,           // trace lifecycle of clause with this ID (0 = off)
    hint_derivations,      // print derivation of matchers for hints with ID < N (0=off)
    cores,                 // sliding-window scheduler: N concurrent children (0=off)
    hint_expiry,           // expire hints not matched in N given clauses (-1=off)
    hint_sweep_interval,   // sweep for expired hints every N given clauses
    hint_expiry_min,       // min match count before hint is eligible for expiry
    hints_fpa_depth,       // FPA index depth for hints (default 10)
    rewrite_refresh_hot_ratio, // hinted refresh turns per fair general turn
    rewrite_refresh_raw_budget, // dense records inspected per refresh turn
    rewrite_refresh_inference_ratio, // inference turns per background repair
    rewrite_refresh_high_water, // stale rewrite rules entering drain mode
    rewrite_refresh_low_water,  // stale rewrite rules leaving drain mode
    rewrite_refresh_drain_burst, // maximum consecutive urgent repair turns
    fpa_hash_threshold,    // FPA trie hash table threshold (default 16, 0=off)
    discrim_hash_threshold; // discrim tree hash table threshold (default 16, 0=off)

  // Stringparms (string options)

  int
    order,               // LPO, RPO, KBO
    eq_defs,             // fold, unfold, pass
    literal_selection,   // maximal, etc.
    stats,               // none, some, lots, all
    multiple_interps,    // false_in_all, false_in_some
    search_loop,         // otter, discount
    passive_store,       // full, compressed
    compact_unit_strategy, // root_scan, position, code_tree
    discount_demodulation, // selected, eager_legacy, eager_interreduced
    hint_index,          // fpa, compact, shallow, packed/hybrid, packed_legacy
    inference_frontier,  // clauses, collective
    collective_scheduler, // legacy, balanced_hint
    ancestor_store,      // off, memory, mmap, file
    passive_backing;     // auto, memory, mmap, file
};

// Clocks

struct prover_clocks {
  Clock pick_given,
    infer,
    preprocess,
    demod,
    redundancy,
    unit_del,
    conflict,
    weigh,
    hints,
    subsume,
    semantics,
    back_subsume,
    back_demod,
    back_unit_del,
    index,
    disable;
};

// Statistics

typedef struct prover_stats * Prover_stats;

struct prover_stats {
  unsigned long long  given,
    generated,
    generated_binary,
    generated_hyper,
    generated_ur,
    generated_paramod,
    generated_other,
    kept,
    proofs,
    kept_by_rule,
    deleted_by_rule,
    subsumed,
    back_subsumed,
    anc_subsume_blocked,
    sos_limit_deleted,
    sos_displaced,
    sos_removed,
    new_demodulators,
    new_lex_demods,
    back_demodulated,
    back_unit_deleted,
    demod_attempts,
    demod_rewrites,
    res_instance_prunes,
    para_instance_prunes,
    basic_para_prunes,
    nonunit_fsub,
    nonunit_bsub,
    usable_size,
    sos_size,
    demodulators_size,
    disabled_size,
    hints_size,
    denials_size,
    limbo_size,
    kbyte_usage,
    new_constants,
    active_indexed_clauses,
    passive_indexed_clauses,
    delayed_demodulators,
    passive_demodulator_candidates,
    passive_oriented_demodulator_candidates,
    passive_lex_demodulator_candidates,
    rewrite_only_demodulators_admitted,
    rewrite_only_demodulators_retired,
    rewrite_only_demodulators_selected,
    rewrite_only_demodulators_current,
    rewrite_only_demodulators_peak,
    rewrite_bank_bytes,
    rewrite_bank_peak_bytes,
    compact_rewrite_rules_current,
    compact_rewrite_rules_peak,
    compact_rewrite_rules_retired,
    compact_rewrite_rules_physical,
    compact_rewrite_compactions,
    compact_rewrite_bytes_reclaimed,
    compact_rewrite_attempts,
    compact_rewrite_rewrites,
    compact_rewrite_node_items,
    compact_rewrite_posting_items,
    compact_rewrite_node_bytes,
    compact_rewrite_posting_bytes,
    compact_rewrite_occurrence_bytes,
    compact_rewrite_occurrence_stream_used,
    compact_rewrite_occurrence_stream_bytes,
    compact_rewrite_rule_bytes,
    compact_rewrite_term_bytes,
    compact_rewrite_hash_bytes,
    compact_otter_audit_queries,
    compact_otter_audit_failures,
    rewrite_refresh_scanned,
    rewrite_refresh_materialized,
    rewrite_refresh_rewritten,
    rewrite_refresh_unchanged,
    rewrite_refresh_subsumed,
    rewrite_refresh_hot_turns,
    rewrite_refresh_general_turns,
    rewrite_interreduce_turns,
    rewrite_interreduce_changed,
    rewrite_interreduce_unchanged,
    rewrite_interreduce_collapsed,
    rewrite_overlap_visits,
    rewrite_overlap_dirty_marks,
    rewrite_cascade_suppressed,
    rewrite_debt_current,
    rewrite_debt_peak,
    rewrite_drain_entries,
    rewrite_drain_exits,
    rewrite_drain_turns,
    rewrite_drain_yields,
    rewrite_inference_turns,
    rewrite_refresh_stale_current,
    rewrite_refresh_stale_peak,
    rewrite_refresh_lag_max,
    passive_refresh_checks,
    passive_refresh_requeued,
    passive_refresh_subsumed,
    collective_batches_created,
    collective_batches_completed,
    collective_pair_expansions,
    collective_pair_turns,
    collective_hyper_expansions,
    collective_hyper_sets_completed,
    collective_candidates_emitted,
    collective_candidates_replayed,
    collective_deferred_turns,
    collective_raw_candidates_peak,
    collective_raw_candidates_seen,
    collective_candidate_cache_peak,
    collective_candidate_cache_stalls,
    collective_candidate_cache_current,
    collective_promising_scans,
    collective_promising_considered,
    collective_promising_buffer_peak,
    collective_promising_priority_turns,
    collective_promising_fair_turns,
    collective_promising_heap_peak,
    collective_partners_skipped,
    collective_parent_materializations,
    collective_snapshot_rebuilds,
    collective_snapshot_clauses,
    collective_snapshot_clauses_peak,
    collective_history_clauses,
    collective_history_indexed_clauses,
    collective_history_shared_clauses,
    collective_history_retained_clauses,
    collective_history_clause_bytes,
    collective_history_queries,
    collective_history_candidates,
    collective_history_rejected_future,
    collective_history_rejected_inactive,
    collective_hint_probes_scheduled,
    collective_hint_probes_expanded,
    collective_hint_selected_total,
    collective_hint_selected_hha,
    collective_hint_selected_hw,
    collective_hint_selected_lh,
    collective_hint_selected_other,
    collective_distinct_hints_matched,
    collective_batches_pending,
    collective_pending_paramod,
    collective_pending_pos_hyper,
    collective_pending_neg_hyper,
    collective_batches_peak,
    collective_descriptor_lag_sum,
    collective_descriptor_lag_p50,
    collective_descriptor_lag_p95,
    collective_descriptor_lag_max,
    collective_activation_entries,
    collective_deactivation_entries,
    collective_descriptor_bytes,
    collective_history_bytes,
    collective_created_paramod,
    collective_created_pos_hyper,
    collective_created_neg_hyper,
    collective_completed_paramod,
    collective_completed_pos_hyper,
    collective_completed_neg_hyper,
    collective_balanced_paramod_turns,
    collective_balanced_pos_hyper_turns,
    collective_balanced_neg_hyper_turns,
    collective_balanced_oldest_turns,
    collective_balanced_lane_turns,
    collective_drain_entries,
    collective_drain_exits,
    collective_givens_withheld,
    collective_paramod_from_turns,
    collective_paramod_into_turns,
    collective_iterator_raw_steps,
    collective_iterator_candidates,
    collective_iterator_completions,
    collective_iterator_invalidations,
    collective_iterator_raw_peak,
    collective_iterator_path_bytes,
    collective_hyper_iterator_raw_steps,
    collective_hyper_iterator_candidates,
    collective_hyper_iterator_completions,
    collective_hyper_iterator_choice_bytes,
    collective_candidate_pool_bytes,
    collective_candidate_pool_peak_bytes,
    collective_preview_calls,
    collective_preview_hint_matches,
    collective_preview_authoritative_matches,
    collective_preview_false_positives,
    collective_preview_changed_hint_ids,
    collective_preview_stale_refreshes,
    collective_candidate_pool_commits,
    collective_candidate_priority_commits,
    collective_candidate_fair_commits,
    collective_discovery_turns,
    collective_discovery_hot_turns,
    collective_discovery_general_turns,
    collective_discovery_forced_fair_turns,
    collective_discovery_raw_steps,
    collective_discovery_candidates,
    collective_discovery_promotions,
    collective_discovery_confirmed,
    collective_discovery_false_positives,
    collective_discovery_duplicate_skips,
    collective_discovery_catchups,
    collective_discovery_distance_max,
    collective_discovery_cap_stalls,
    collective_discovery_consumed_records,
    collective_discovery_consumed_bytes,
    compression_attempted,
    compression_successful,
    compression_skipped,
    compression_materialized,
    compression_recompressed,
    active_body_bytes,
    passive_body_bytes,
    passive_justification_bytes,
    passive_total_payload_bytes,
    passive_estimated_full_body_bytes,
    passive_compressed_clauses,
    dense_passive_records,
    dense_passive_record_bytes,
    dense_passive_heap_bytes,
    dense_passive_arena_records,
    dense_passive_arena_record_bytes,
    dense_passive_arena_backing_bytes,
    dense_passive_arena_physical_bytes,
    dense_passive_arena_materializations,
    dense_passive_arena_validation_failures,
    dense_passive_arena_file_reads,
    dense_passive_arena_file_read_bytes,
    dense_passive_arena_file_writes,
    dense_passive_arena_file_write_bytes,
    dense_passive_compactions,
    dense_passive_records_reclaimed,
    dense_passive_arena_bytes_reclaimed,
    hint_body_bytes,
    hint_estimated_full_body_bytes,
    hint_compressed_clauses,
    hint_index_node_bytes,
    hint_index_reference_bytes,
    hint_index_table_bytes,
    hint_candidate_checks,
    disabled_full_body_bytes,
    disabled_compressed_bytes,
    disabled_estimated_uncompressed_bytes,
    disabled_full_clauses,
    disabled_compressed_clauses,
    ancestor_records,
    ancestor_record_bytes,
    ancestor_backing_bytes,
    ancestor_handle_bytes,
    ancestor_materializations,
    ancestor_validation_failures,
    ancestor_mmap_eviction_passes,
    ancestor_mmap_eviction_bytes,
    ancestor_mmap_scan_eviction_passes,
    ancestor_mmap_scan_eviction_bytes,
    ancestor_io_buffer_bytes,
    ancestor_file_reads,
    ancestor_file_read_bytes,
    ancestor_file_writes,
    ancestor_file_write_bytes,
    disabled_store_bytes,
    disabled_legacy_clist_bytes,
    clause_id_entries,
    clause_id_pages,
    clause_id_table_capacity,
    clause_id_table_bytes,
    clause_id_legacy_bytes,
    allocator_reserved_kbytes,
    allocator_logical_live_bytes,
    allocator_logical_peak_bytes,
    allocator_reserved_bytes,
    allocator_peak_reserved_bytes,
    allocator_reusable_bytes,
    allocator_unallocated_bytes,
    allocator_metadata_bytes,
    allocator_fragmentation_bytes,
    allocator_direct_live_bytes,
    allocator_permanent_live_bytes,
    allocator_slab_count,
    allocator_peak_slab_count,
    allocator_reclaimed_slabs,
    allocator_reclaimed_bytes,
    process_smaps_supported,
    process_libc_heap_supported,
    process_compact_heap_enabled,
    process_pss_kbytes,
    process_anonymous_kbytes,
    process_shared_clean_kbytes,
    process_shared_dirty_kbytes,
    process_private_clean_kbytes,
    process_private_dirty_kbytes,
    process_swap_kbytes,
    libc_arena_bytes,
    libc_mmap_bytes,
    libc_in_use_bytes,
    libc_free_bytes,
    libc_releasable_bytes,
    current_rss_kbytes,
    peak_rss_kbytes,
    allocator_cumulative_bytes,
    allocator_allocation_calls,
    palloc_cumulative_bytes,
    fpa_live_nodes,
    fpa_peak_nodes,
    fpa_live_lists,
    fpa_peak_lists;
};

// Search input

typedef struct prover_input * Prover_input;

struct prover_input {
  // tformula lists
  Plist usable, sos, demods, goals, hints, unused;
  // term lists
  Plist actions, weights, resonators, kbo_weights, interps;
  Plist given_selection, keep_rules, delete_rules;
  // ordinary options
  Prover_options options;
  // extra options
  BOOL xproofs;  // tell search() to return xproofs as well as ordinary proofs
  BOOL has_goals; // input contained goals/conjecture (for SZS status: Theorem vs Unsatisfiable)
  BOOL has_neg_conj; // input had CNF negated_conjecture (refutation, not satisfiability)
  BOOL cnf_only;     // if TRUE, output processed clauses and exit (no search)
  char *problem_name;  // TPTP problem name for SZS lines (e.g., "PUZ001-2"), or NULL
  // checkpoint resume
  char *resume_dir;  // if non-NULL, resume from this checkpoint directory
};

// Search results

typedef struct prover_results * Prover_results;

struct prover_results {
  Plist proofs;
  Plist xproofs;
  struct prover_stats stats;
  double user_seconds, system_seconds;
  int return_code;
};

/* Exit codes */

enum {
  MAX_PROOFS_EXIT   = 0,
  FATAL_EXIT        = 1,  /* don't change this one! */
  SOS_EMPTY_EXIT    = 2,
  MAX_MEGS_EXIT     = 3,
  MAX_SECONDS_EXIT  = 4,
  MAX_GIVEN_EXIT    = 5,
  MAX_KEPT_EXIT     = 6,
  ACTION_EXIT       = 7,
  MAX_NOHINTS_EXIT  = 8,

  SIGINT_EXIT       = 101,
  SIGSEGV_EXIT      = 102,
  SIGTERM_EXIT      = 103,

  CHECKPOINT_EXIT   = 107
};

#endif  /* conditional compilation of whole file */
