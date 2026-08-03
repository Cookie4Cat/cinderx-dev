# CPython 3.11.6 upstream source snapshot

The files in this directory come from the `rpmbuild` output of
`python3-3.11.6-34.oe2403sp3.src.rpm` (source RPM SHA-256:
`bf124b75613faf3b15094666fe635e5e470d3e1503c36b2dbec7b9909312b1e0`).
The source root is `BUILD/Python-3.11.6`.

| Snapshot file | Source path |
| --- | --- |
| `ceval.c` | `Python/ceval.c` |
| `specialize.c` | `Python/specialize.c` |
| `frame.c` | `Python/frame.c` |
| `ceval_gil.h` | `Python/ceval_gil.h` |
| `condvar.h` | `Python/condvar.h` |
| `opcode_targets.h` | `Python/opcode_targets.h` |
| `pydtrace.h` | `Include/pydtrace.h` |

These files are pristine upstream inputs. Do not edit them in place. Put all
CinderX adaptations in wrappers, shims, Borrow templates, or generated outputs
outside this directory.
Their digests are recorded in `SHA256SUMS`. A future CI gate will compare the
checked-in snapshot with the pinned rpmbuild source tree.

When updating the snapshot, rebuild the exact source RPM with `rpmbuild`,
replace the upstream files as one atomic change, update `SHA256SUMS`, regenerate
the interpreter-local Borrow output, and compile the standalone interpreter
target.
