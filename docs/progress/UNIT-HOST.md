# make unit-host: the clean-checkout unit gate on canon

`make unit-host` is canon's unit gate. It must pass from a clean checkout. It ports `dev/unit-host-restore`
(f89baa89, f178f3c9, e767adc1, 2026-10-04/05) to canon (lead and review, 2026-10-06 10:49). On canon,
legacy `make unit` cannot run to the end: some of its steps test RTL or tools that this line does not have.

## Run it (SLOT-FORM only)

The target refuses to start outside the lab-pc slot form (`exit 2`). It refuses in the host pid namespace, in a pid
namespace without its own `/proc`, when the host `/tmp/misterplexd.lock` is visible, and as uid 0 (a mapped root).

```sh
U=$(id -u); G=$(id -g)
heavy-sim-slot -- unshare --user --map-root-user --pid --fork --mount-proc --net \
  bash -c 'mount -t tmpfs -o size=4g,mode=1777 tmpfs /tmp || exit 99; ip link set lo up;
           exec unshare --user --map-user='"$U"' --map-group='"$G"' make -C <clean-checkout> unit-host'
```

The inner namespace runs the tests as the real uid. Permission tests such as `test_lock_paths.sh` T1e
(a read-only lock dir) fail under a mapped root.

## What it runs

- The prerequisites of `unit-unlocked`: its recipe rule's list is `$(UNIT_PREREQS)`, shared by both recipe rules,
  and each of the 28 other rules that adds a `unit-unlocked` prerequisite (the mutant checks, `.ok` stamps, the
  c1 check, the target checker) is written `unit-unlocked unit-host-unlocked: ...`, so both targets resolve to the same
  prerequisite set (106 at the port, by `make -pnq`). A new prerequisite rule must name both; the coverage test
  refuses one that names only one.
- The recipe of `unit-unlocked`, verbatim and in order. The commands listed in `tests/unit/unit_host_exclusions.txt`
  are left out; each exclusion has its class and reason.
- `tests/unit/test_unit_host_coverage.py` runs first in both recipes; legacy `make unit` (`unit-unlocked`) runs it
  first too. It fails when the two drift:
  - a new `unit-unlocked` command must be added to `unit-host-unlocked` or excluded with a reason;
  - every exclusion must name a `unit-unlocked` command;
  - the order must match, and every prerequisite rule must name both targets;
  - the SLOT-FORM guard must stay in place.

At the port: `unit-unlocked` has 158 commands (157 plus the coverage test). `unit-host` runs 119 of them and
excludes 39 (the coverage test prints the current counts).

## Exclusion classes

| Class | Meaning | Lines today |
| --- | --- | --- |
| `rtl-sim`, `rtl-tooling` | Verilator simulations and the bench-list scan: the rtl-sim lane, not a host step | 29 |
| `a` | the source or feature is absent on canon: Verilator benches whose RTL or TB is not on this line, a probe mode only 3115dbdc has, lint stubs from another lineage | 7 |
| `c` | it failed identically on the batch tip ac7f4027 (same bytes) and is owned elsewhere: rtl_lint's ddr_frame_store WIDTHTRUNC baseline, the warm-reset sim, and the rtl_invariants ticket list | 3 |

`tests/unit/test_rtl_invariants.sh` is excluded whole, so the layout contract that landed in 5ebf6db8 does not run
here. Its re-inclusion is tracked on T-RTL-INVARIANTS-CANON: add it back when its FAIL list is empty, or reduced to
a known list.

`tests/unit/test_no_private_data.sh` runs (review 11:57). It had failed on the batch tip over one synthetic
fixture: the private-LAN row at `tests/unit/test_resolve.cpp:162`, whose RFC1918 base on the PMS port reaches the
192.168 branch of `plexLocationForBase`. The scanner now allows that exact line in that one file. The allowance
must match exactly one hit, so a moved, copied, edited or removed literal fails the scan again, and so does any
other copy of it (this document deliberately does not repeat the address).

The evidence is the `make -k -i unit-unlocked` run of 2026-10-06 on a clean 59ddfa41 tree, with the batch-tip
comparisons from slot job cls-rerun 1368917, as classified by review at 11:43.

## Compared with the 10-05 unit-host

- Canon runs 119 host commands; the 10-05 target ran 65. Canon's own `test_resolve`, `test_p720_transcode_vf`,
  `test_candidate_deploy.py` and `test_audio_session_host.sh` pass whole, so the 10-05 skips and stale-test
  exclusions are gone.
- The guard checks the SLOT-FORM (pid ns and its own `/proc`, no host lock), not `/proc/1/comm`.
- The four define-parity names this line added are declared in `tests/fixtures/define_parity_allowlist.json`
  (AU_MODE, REAL_ALSA_TEST, TEST_MAX_AU_BYTES, MUT_PQ_), so `scripts/check_define_parity.py` runs in unit-host.
