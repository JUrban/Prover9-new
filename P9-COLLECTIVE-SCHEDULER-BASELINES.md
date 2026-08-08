# Collective scheduler frozen baselines

Date: 2026-08-08 (Europe/Berlin)

Source branch point: `b6e89bf213b4ea9d4349080f1a4130a978fcf200`

The archived outputs below are external evidence and are intentionally not
modified by development on `better-collective-scheduler`.  Their banners all
identify Prover9 2026-6A.  The exact historical executable is not available,
so no binary hash is claimed for those jobs.

## Artifact hashes

| Artifact | SHA-256 |
| --- | --- |
| `../bob/rr_osbe.out2-leg` | `6e3aec993d11d4b1b223bd2ed6677f498958678952a46702982bd2ad5c171b07` |
| `../bob/rr_osbe.out2-rad` | `17b106dd726fcbbb745a3b9ef3e0e3d05285350e3a0e9ab8d616802320fa2c82` |
| `../bob/rr_osbe.out2-rad-comp` | `93df32ec688d0085a946301e71273b9b4ad0ac6dff0e94c6fdf6c9330b47d7da` |
| `../bob/rr_osbe.out2-unl-rad-comp6h.gz` | `ec5ab892543439b5c35ce0c632335c6f2a39fdba454b45c7b80a6849e1b974de` |
| `../bob/outaH` | `c87075b7e647ea7bc867a47417013176935db93846330073865b6e35ccfb8288` |
| `../bob/outa50` | `aa861e822f9f89aeee01cfd68b816d29d2f4ff07d3ff743dfc132384779c04c6` |
| `../Osborn-instrumentation-2026-08-07/rr_osbe-hints10pct.in` | `995062826983d6ea7432d741536b7be1493f0a36e7357eaa8c3b3e1249ebd59c` |

## Archived 1,000-given controls

| Artifact | Search / store / hints / frontier | Generated | Kept | Distinct hints matched | User CPU | Peak RSS |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `rr_osbe.out2-leg` | OTTER / full / FPA / clauses | 1,832,961 | 86,890 | 508 | 113.44 s | 507,108 KiB |
| `rr_osbe.out2-rad` | DISCOUNT / dense / packed / collective | 9,126 | 1,717 | 101 | 441.39 s | 295,680 KiB |
| `rr_osbe.out2-rad-comp` | DISCOUNT / dense / compact / collective | 9,126 | 1,717 | 101 | 68.33 s | 371,904 KiB |

Both radical jobs used `collective_given_ratio=4` and cleared
`collective_hint_probes`, `collective_promising_candidates`, and
`collective_promising_scheduler`.  The packed and compact search results and
selector traces agree; the hint representation is not the source of the
collective trajectory change.

## Archived long-run points

The old OTTER proof in `outa50` ended with:

```text
Given=11242. Generated=212152996. Kept=11131760. proofs=1.
Megabytes=18810.85.
User_CPU=76217.95, System_CPU=3920.81, Wall_clock=80147.
```

The filtered `outaH` artifact contains 10,048 `Hha` and 1,000 `Hw`
hint-selected givens after the 194 initial clauses.  It is not a count of
distinct input hints.

The six-hour conservative collective report ended with:

```text
Given=178177. Generated=6512790. Kept=297330. proofs=0.
Collective_frontier: batches_created=178177, completed=6, pending=178171.
Collective_work: paramod_turns=1084, paramod_pairs_completed=1084,
                 hyper_turns=99156, hyper_sets_completed=315.
Hint match stats: matched=301.
User_CPU=21601.65, System_CPU=55.88, Wall_clock=21720.
RSS_kb: current=767284, peak=767284.
```

At that point the new run had 32.6 times fewer generated candidates despite
15.8 times more givens.  Development comparisons must therefore match CPU,
raw enumeration, or committed/generated work rather than treating given
count as equivalent progress.

## Reproducible bounded controls

`test.src/osborn_collective_controls.sh` prepares and runs four bounded
policies from an explicit Osborn input:

1. OTTER/FPA clauses frontier;
2. DISCOUNT/dense/compact clauses frontier;
3. the conservative collective policy; and
4. collective mode with its existing optional aids enabled.

The script defaults to 100 givens, 120 seconds, and 2,048 MiB per job.  It
records the exact generated input, executable/input hashes, exit status,
`/usr/bin/time -v`, and Prover9 output.  Larger limits require explicit
arguments and are outside the development default.
