# gsh

`gsh` is a generative shell. This repository currently contains
a deliberately small C MVP of its soft real-time interactive core, following
the principles in [`specs/0001.principles.md`](specs/0001.principles.md) and
the architectures in [`specs/0002.real_time.md`](specs/0002.real_time.md) and
[`specs/0008.async_repl.md`](specs/0008.async_repl.md).

## Project quality criteria

`gsh` is also a coding project whose implementation should remain remarkably
simple even as the shell becomes mature and dependable. Four criteria guide
every design and release decision:

1. **Native POSIX.1-2024 completeness.** Implement 100% of the Issue 8 shell
   language and required utilities in first-party code. Compatibility
   delegation is useful during development but never counts as conformance.
2. **Performance.** Beat clean Bash and Zsh on equivalent native workloads
   without omitted semantics, hidden delegation, or benchmark-specific paths.
   Optimize startup and steady-state latency, throughput, p99/jitter, memory,
   descriptors, and process cost; never trade correctness for a favorable
   median.
3. **Mission-grade reliability.** Use bounded state, explicit ownership,
   transactional mutation, isolation, cancellation, resource ceilings, and
   deterministic recovery. The quality claim requires conformance,
   sanitizer, fuzz, fault-injection, resource-exhaustion, soak,
   static-analysis, and supported-platform evidence on the same revision.
4. **The fewest possible lines of code.** Minimize implementation size and
   public API surface. Prefer one general primitive that closes a family of
   cases, and split files by cohesive responsibility rather than by function.
   Never reduce line count by weakening semantics, performance, diagnostics,
   testability, bounded behavior, or readability.

These are simultaneous constraints. A solution is not complete when it is
fast but incorrect, reliable but needlessly complicated, small but opaque, or
POSIX-compatible only through another shell. Until the evidence above exists,
the corresponding performance, conformance, or mission-grade statement remains
an objective rather than a product claim.

Every POSIX shell builtin is completed as a native C implementation inside gsh; delegation is never an accepted final state.

The mandatory code-writing policy for human contributors and AI agents is
[`CODE.md`](CODE.md). It adopts JPL's Power of Ten rules and gsh's Literate Code
discipline. Its permanent zero-tolerance gate covers all repository-owned
production, test, fuzz, policy, and executable support code; there is no legacy
violation baseline.

The MVP already follows the central shape described by
[`specs/0002.real_time.md`](specs/0002.real_time.md):

- a single-threaded `poll()` reactor owns the editor, shell state, and terminal;
- terminal and signal events are handled in bounded batches;
- signal handlers only set flags and notify the reactor through a nonblocking
  self-pipe;
- the default managed REPL keeps the physical terminal in the reactor, gives
  each command job its own PTY and cell, and exposes the next editor
  immediately; classic mode retains direct foreground terminal handoff;
- child transitions are collected with nonblocking `waitpid()` calls;
- classic and managed jobs share one bounded reactor-owned service; `$!`,
  `jobs`, `kill`, `fg`, `bg`, and `wait` use the process that owns the live
  PID/PGID state;
- shell output uses a fixed-size nonblocking queue;
- the base prompt is immediate and has no repository integration;
- a persistent process services eligible stateless output redirections
  through a bounded `socketpair()` protocol;
- a redirection worker blocked in the kernel remains cancellable: `Ctrl-C`
  terminates it, the reactor reaps it, and a fresh worker is started.

The `gsh` core is intentionally single-threaded and multi-process. One reactor
thread is the sole owner of mutable shell state, editing, scheduling, and
physical-terminal rendering. Concurrent commands run in isolated child
processes, which the operating system may execute in parallel across CPU cores.
This design avoids locks and data races in the trusted core while process
boundaries provide explicit communication, signals, lifecycle control, and
failure containment. Future AI providers and local model runtimes follow the
same boundary: they may use threads internally, but they do not share mutable
shell state and cannot become additional owners of the reactor.

This is an architectural MVP, not yet a complete POSIX shell. It now has a
bounded Issue 8 lexer and recursive-descent parser plus a conservative native
execution planner. Static external pipelines are launched concurrently as one
POSIX process group; quote removal and ordinary file/descriptor redirections
are native on that path. Here-documents use bounded pipes and isolated writer
processes, so a slow or non-reading command cannot block the reactor. Sequential
and AND-OR lists, brace/subshell grouping, `if`, `while`, `until`, and a bounded
`case` subset are also native when their commands belong to the implemented
subset. Basic environment parameters, `$?`, `$$`, `$!`, persistent assignment-only
commands, command-local assignments, POSIX dollar-single quotes, both command
substitution forms, bounded checked `$((...))` arithmetic including all
mandatory POSIX assignment operators, provenance-aware
field splitting, bounded pathname expansion, parameter default/alternative
and assign/error operators, parameter character length, and bounded
transactional shell variables are implemented natively. Mutating expansions in
a multi-command pipeline use one fixed-size scoped journal: effects are visible
later in the same command but neither to sibling stages nor to the parent shell.
An assignment-only simple command takes the status of its last `$()` or
backquote substitution directly from the expansion pass, including negation;
no output rescan or additional process is needed.
Command substitution and pathname enumeration execute only in isolated
evaluator processes; neither can block the interactive reactor. Nested source
evaluation uses an eight-slot LIFO arena allocated once at startup. Each slot
owns parser, planner, transactional, function, alias, and 1 MiB source storage,
so entering a command substitution, `eval`, or dot script performs no heap
allocation and depth exhaustion is deterministic. The same allocation owns the
persistent root alias and function stores plus the top-level 1 MiB
complete-command fast window,
removing their former first-use allocations without faulting in their unused
text and parser arenas. `-c` maps its command name and
arguments to `$0` and the positional parameters, with native
`$#`, numbered parameters, `$@`, `$*`, and `$-`; quoted `$@` retains its
multi-field semantics. `set --` and operand forms replace or clear positional
parameters in a bounded zero-fill arena; `shift` validates an unsigned decimal
operand and advances its live window in O(1). The native option state supports
`allexport`, `errexit`, `noclobber`, `noglob`, `nounset`, `notify`, `noexec`,
`verbose`, `xtrace`, `hashall`, `ignoreeof`, `nolog`, and `pipefail`, including
their short forms where defined, `$-`, sorted `set -o` output, and reusable
`set +o` output. Allexport marks assignments for descendant environments
without changing command-local scope. Nounset rejects unset parameter
expansion while preserving the POSIX default, alternative, assign, and error
modifier rules. Noexec parses complete commands without executing them.
Verbose emits input at command boundaries; xtrace emits bounded, shell-quoted
expanded words with the current `PS4`. Pipefail captures stage statuses and
selects the rightmost failure in O(stage count), while the default retains
last-stage status. Errexit carries explicit syntactic context across lists,
functions, substitutions, subshells, pipelines, `eval`, and dot scripts, so
conditional and negated contexts suppress termination without global
heuristics. The four POSIX parameter pattern-removal operators
`#`, `##`, `%`, and `%%` are native and retain quote provenance. Literal and
fixed-width patterns use allocation-free specialized paths; shortest fixed
multi-star patterns use directional greedy segment matching under a bounded
reactor work estimate. Larger or general matching is deferred to the
cancellable evaluator process and committed transactionally, so pathological
matching cannot monopolize the interactive reactor. Option and positional
changes form one atomic transaction. `noclobber` uses an atomic
`O_CREAT|O_EXCL` decision,
rejects existing regular files including regular-file symlink targets, permits
non-regular targets, and honors the POSIX `>|` override. Safe stateless `:`,
`true`, and `false` output redirections reuse the persistent asynchronous worker
instead of paying for a new process; all other cases retain the isolated
evaluator. Direct mutations stay in the reactor, compound mutations commit
atomically, and pipelines, subshells, command substitutions, redirection
failures, signals, and cancellation retain the required isolation. `cd` uses a
directory descriptor for rollback, so a failed compound commit can restore the
previous directory even if its pathname was renamed; directory state crosses
the evaluator boundary through a bounded `SCM_RIGHTS` transaction rather than
path reconstruction.
Asynchronous AND-OR lists, `$!`, and `wait` are native. `notify` selects
immediate versus prompt-boundary completion reports without consuming job
state. A unified bounded service tracks at most 128 user jobs across classic
and managed modes, retaining
stable job IDs, PGIDs, members, commands, running/stopped/done state,
current/previous selection, notification, and consumption state. Sequential
and `&&`/`||` continuations keep `wait` in the owning reactor, Ctrl-C cancels
the wait without killing the job, and a simple external background command
tail-`exec`s in its direct child rather than retaining an evaluator wrapper.
Explicit-list `for` and the implicit `for name` form are native. A finite
literal loop with a provably pure `:`, `true`, or `false` body can be reduced
inside the reactor under a 16-operation admission budget; loops with expansion,
I/O, nesting beyond that budget, or other bodies use the isolated native
evaluator. The `break` and `continue` special builtins are native for `for`,
`while`, and `until`, including positive decimal nesting counts, oversized
counts selecting the outermost lexical loop, assignment and redirection
semantics, conditional-list propagation, and function-local loop scope.
POSIX aliases are native: a lazy fixed-capacity hash and compact
arena provide allocation-free steady-state lookup, parser-guided recursive
substitution preserves token provenance, and `alias`/`unalias` changes become
visible in complete-command order. Subshell and asynchronous changes remain
isolated, while compound mutations commit through a bounded rollback journal.
`command -v`, `command -V`, and `type` share one bounded, allocation-free
resolver for reserved words, aliases, special builtins, functions, regular
builtins, explicit pathnames, and `PATH`; command-local `PATH`, `command -p`,
redirections, pipelines, and missing-name status are native. The execution form
of `command` is also native for the implemented target set, including nested
wrappers, function suppression, `-p`, declaration assignments, temporary
special-builtin prefixes, redirections, pipelines, and 126/127 propagation.
The `exec` special builtin performs a zero-allocation `execve` overlay for
external utilities, honors leading `PATH` and environment assignments, and
commits successful redirections even when option parsing or utility lookup
fails. Interactive compound evaluation reports overlay success through a
close-on-exec protocol; descriptor state crosses the evaluator boundary in a
validated, collision-safe `SCM_RIGHTS` snapshot. Classic and managed REPLs
therefore preserve descriptor changes, while a successful overlay terminates
the owning shell session with the utility's status. A direct interactive
`exec` preserves the shell PID; a compound interactive `exec` still overlays
the isolated evaluator child before its owner exits, so exact original-PID
identity remains continuation-engine work.
When `execve()` reports `ENOEXEC`, executable text without a recognized image
format is re-executed through gsh's initialization-time canonical pathname,
never through `/bin/sh`. The new native shell image preserves the child PID,
process group, descriptors, exported environment, resolved script `$0`,
operands, pipeline/background behavior, and `exec` overlay status.
The `exit` special builtin uses an explicit evaluator control record rather
than terminating from an inner helper. It applies assignments and redirections
first, crosses function, `eval`, and dot frames, ignores pipeline negation once
termination is requested, and is naturally confined by the process boundaries
of subshells, substitutions, pipelines, and asynchronous lists. In interactive
compound evaluation, the versioned control record commits atomically with the
state observed by `exit`; classic and managed owners terminate with that exact
status only after validating the whole transaction.
The non-interactive `trap` special builtin implements the Issue 8 `-p`,
default, ignore, action, numeric-reset, and reinput-safe query forms over a
fixed 1 MiB compact action arena. Signal handlers perform only bounded
`sig_atomic_t` reporting; the evaluator parses actions at dispatch time,
preserves `$?`, and defers foreground traps while allowing `wait` and an idle
standard-input read to return immediately. EXIT actions retain the terminating
environment and can replace the final status with `exit`. Caught actions reset
across pipeline, subshell, command-substitution, and asynchronous process
boundaries, ignored dispositions persist, and ignored signals inherited by a
non-interactive shell cannot be changed. A pristine subshell query can emit its
entry snapshot for `save=$(trap -p)` without executing the parent's EXIT action.
Interactive parent-owned trap state is not integrated into the continuation
engine yet, so the overall signals/traps gate remains partial.
The `eval` and `.` special builtins are native in both non-interactive and
interactive current-environment evaluation.
`eval` concatenates operands with one space, supports an optional `--`, parses
the result in a preallocated source slot, and executes it in the caller's
environment. Dot accepts `--`, searches `PATH` for a readable file without
requiring execute permission, and preserves variables, options, aliases,
functions, positionals, directory changes, exit status, and invocation
redirections. `return` unwinds the nearest dot boundary, including through a
nested `eval`. Eval and dot also execute as isolated pipeline stages, in
subshells, asynchronous lists, and command substitutions. Source ownership and descriptor restoration are strict LIFO on
success and every failure path; the ninth simultaneously active source is
rejected deterministically. Interactive variables, aliases, functions,
positionals, options, command cache and directory changes cross the evaluator
boundary as one validated transaction. Source-local `return` status and inner
`exit` control also reach the owning classic or managed session, while
pipeline, subshell, substitution and asynchronous source execution remains
isolated.
`hash` and ordinary external execution share a 128-entry, open-addressed
command-location cache with allocation-free steady-state lookup. Every
successful `PATH` assignment invalidates it, stale executable locations fall
back to a fresh search, and compound mutations commit atomically while
pipelines and subshells remain isolated. Command-local `PATH` and `command -p`
do not pollute the owning shell's cache. Command execution remains incomplete
for POSIX features outside the documented native subset.
Function definition, deferred expansion, positional invocation scope,
environment mutation, `return`, redefinition, and `unset -f` have native
bounded implementations. Definition/call redirections and function execution
inside pipelines, subshells, command substitutions, and asynchronous lists are
also native and bounded. The complete Issue 8 error/search semantics,
performance gate, and platform evidence remain incomplete. Interactive
invocation and monitor options, `vi` editing semantics, startup-environment
rules, monitor-mode terminal edge cases, and the remaining nested expansion
forms are still incomplete. The interactive
unsupported-syntax bridge must disappear from
normal shell-language execution before `gsh` can claim POSIX.1-2024
shell-language conformance.

The builtins implemented in `gsh` itself include `.`, `:`, `[`, `alias`, `bg`,
`break`, `cd`, `command`, `continue`, `echo`, `eval`, `exec`, `exit`, `export`,
`false`, `fc`, `fg`, `getopts`, `hash`, `help`, `jobs`, `kill`, `ll`, `ls`,
`printf`, `pwd`, `read`, `readonly`, `return`, `rt`, `set`, `shift`, `test`,
`times`, `trap`, `true`, `type`, `ulimit`, `umask`, `unalias`, `unset`, `view`,
and `wait` within their
documented native contexts. The exact standalone interactive control submission
`/async` toggles the managed REPL for the current session.

The protected POSIX tranche—`echo`, `printf`, `test`, `[`, `read`, `getopts`,
`fc`, `jobs`, and `kill`—cannot reach the compatibility bridge in any command
context. One fixed registry supplies builtin identity and execution class.
Pure callbacks are shared by direct, redirected, pipeline, subshell,
asynchronous, and compound execution. Stateful commits remain parent-only:
for example, `read value < file` updates the current shell while a pipeline
stage is isolated. `fc` launches an external editor directly when requested,
then parses and runs the edited text with gsh. `jobs`, `kill`, `fg`, `bg`, and
`wait` share the live reactor-owned job service; evaluator contexts reach it
through bounded descriptor-carrying requests rather than a shell delegation.
`ulimit` implements the POSIX.1-2024 `-H`,
`-S`, `-a`, `-c`, `-d`, `-f`, `-n`, `-s`, `-t`, and `-v` resource interface.
`umask` implements octal masks, `-S`, and POSIX symbolic masks including
permission copying and the initial-mode semantics of `X`. An unredirected
standalone invocation of either environment builtin changes the current shell,
while a pipeline stage remains isolated. `times` reports the four `times()`
counters using `_SC_CLK_TCK` precision and locale-neutral formatting; compound
evaluators rebase their counters on the owning shell, while true subshells
remain isolated. The bounded editor supports UTF-8-aware left/right cursor
movement, insertion and backspace at the cursor, multiline input with `PS2`,
and bracketed multiline paste that preserves every pasted line until an
explicit Enter submits the complete buffer. It also supports history arrows,
incremental `Ctrl-R`, `Ctrl-U`, `Ctrl-L`, `Ctrl-C`, and `Ctrl-D`. The managed
REPL retains 16 bounded cells and runs at most 8 PTY command jobs concurrently.
A job that disables terminal echo for private input is focused automatically;
the preserved editor remains intact and subsequent bytes are routed to that
job until it releases focus. A job that enters non-canonical terminal mode is
shown automatically as a contained full-screen session, covering programs such
as `htop`, editors, pagers, and terminal coding agents without command-specific
rules. `fg` focuses the newest live job when a program has no detectable
terminal transition, and `Ctrl-]` remains an explicit emergency return to the
editor.
The unified job service holds at most 128 user-visible jobs and never exposes
internal workers as jobs.

## Requirements

- macOS or Linux
- a C17 compiler with POSIX APIs (`cc`, Clang, or GCC)
- `make`

The source requests the POSIX.1-2024 feature-test baseline with
`_POSIX_C_SOURCE=202405L`. Current platform SDKs may still report an older
implemented POSIX version; `gsh` therefore uses the portable interfaces present
on both macOS and Linux and keeps platform capability differences explicit.

## Build

```sh
make
```

The shell is written to `build/gsh`. To remove it:

```sh
make clean
```

`build/` contains only local artifacts and is ignored by Git.

Run the black-box PTY smoke test with:

```sh
make check
make conformance
```

The test and its lifecycle probe are also written in C. They launch the real
executable through a pseudo-terminal and verify direct execution, shell
fallback, stop/`fg`/`Ctrl-C` job control, settled and pending prompts,
isolation of a redirection worker blocked on filesystem I/O, and exact
terminal-mode restoration. It also exercises plain-text history across shell
restarts, arrow-key recall, incremental `Ctrl-R`, and private commands.

Additional reliability gates are:

```sh
make check-fault
make check-resource
make check-file-builtins
make check-resource-actions
make check-positionals
make check-background
make check-functions
make check-traps
make analyze
make fuzz-smoke
make fuzz-sanitize
make fuzz-libfuzzer-linux FUZZ_SECONDS=60
make fuzz-pty
make fuzz-pty-sanitize
make soak SOAK_SECONDS=60
```

## Code quality utilities

The repository exposes its first-party code-quality checks through `make`, so
the same bounded tools are available to contributors and to automation:

```sh
make quality
make policy
make quality-callgraph
make quality-dependencies
make quality-includes
make quality-maps
make quality-compilers
make quality-linux
make analyze
```

`quality` is the local aggregate: it applies the permanent zero-tolerance
source-policy gate, checks
the complete source-to-target manifest, runs Clang's static analyzer over every
manifest-owned production, test, fuzz, and policy C source, and builds and
verifies a linker map for every non-sanitized executable target.
`quality-dependencies` requires every such target to have a nonempty, current
compiler-generated `.d` dependency graph, including transitive headers, and
works unchanged with an out-of-tree `BUILD_DIR`. This prevents a stale binary
from concealing a source or header that no longer compiles. The aggregate also runs
`quality-includes`, which removes each direct include from each translation
unit in isolation and requires compilation in its reachable target
configuration, with the same strict flags, to fail;
a unit that still compiles has a redundant include and fails the gate.
An unconditional include whose declarations are exposed transitively in one
configuration but required by another is marked `CANON-INCLUDE: linux`,
`macos`, `gcc`, or `clang`. The non-owning pass defers that one removal, while
the owning platform or toolchain must prove it necessary; includes in inactive
preprocessor branches are likewise left to the configuration that reaches
them. Unknown ownership tags fail the gate.
`policy` is the concise form used by the wider verification gate.
`quality-callgraph` runs the same policy but also lists every unreachable
function, direct recursive call, recursive call-cycle member, and the direct
edges that keep each recursive component connected, so a refactor has an
actionable cut list. It also lists functions above the 60-unit structural
limit; the summary reports the repository-wide assertion deficit relative to
two per function. Recoverable guards contribute one check per pure Boolean
clause; explicit recoverable `require` calls are counted once and are not
double-counted through the surrounding recovery branch. Embedded fixtures
reject effectful predicates and dead types, fields, enumerators, and macros.
`quality-maps` parses the Darwin and GNU linker formats and fails if
a first-party function is discarded in every supported target; embedded
positive and negative fixtures keep both parsers honest. The maps are written
beside their binaries as `BUILD_DIR/*.map` and are removed by `make clean`.
`quality-compilers` repeats the policy and linker-map matrix in
temporary, isolated Clang and GCC build directories, using
`QUALITY_CLANG_CC` and `QUALITY_GCC_CC` when the compiler commands need local
overrides. On macOS, `/usr/bin/gcc` is normally another Clang driver, so a
separate GNU GCC installation or the Linux matrix is required for independent
GCC evidence. `quality-linux` supplies that matrix reproducibly with Docker:
it builds the pinned Debian toolchain image in
`dev/quality-linux.Dockerfile`, verifies that the container is Linux `x86_64`,
then runs policy, all non-sanitized linker-map builds, and conformance with both
GNU GCC and Clang. Docker Desktop must be running; the source tree is mounted
read-only and all Linux build products stay in the disposable container.

`policy` has no accepted debt baseline: every violation count must remain
zero. Source ownership lives in `dev/target-manifest.tsv`: every C source must
belong to one or more of the 20
closed, supported targets, and every target must have an explicit `main` or
fuzzer root. Unknown targets, unknown roles, duplicate sources, unowned files,
and dependency-only targets fail `policy`; embedded positive and negative
fixtures protect the target parser.
The manifest is an enforcement input rather than an exception to the Code
Canon. As with the normal build, an isolated artifact directory can be selected with, for
example, `make BUILD_DIR=/tmp/gsh-quality quality`.

`check-fault` uses a separate test-only binary and exercises 89 deterministic
failure points. The production build contains neither the injection
configuration nor its fault names. `check-resource` covers runtime descriptor
and process exhaustion, data-limit capability, bounded single-line and
multiline input, signal storms, recovery after a rejected command, and failed
initialization. The conformance limit gate also exercises the last valid nested
command-substitution and `eval` source slots plus deterministic rejection of
the next level. Limits applied
after startup use gsh's native `ulimit`, so the
dynamic loader is outside the measurement; translated architectures are
reported as unsupported rather than native evidence. Native conformance also
checks the argument, expansion, pathname-result, redirection, pipeline,
here-document-count, and 64 KiB
here-document-payload budgets. `check-background` fills the 128-entry registry
and verifies identity, completion, consumption, duplicate rejection, and slot
reuse. The soak runner repeatedly edits, dispatches, mutates and shifts
positional parameters, launches and waits for background jobs, pipes, feeds
here-documents, cancels,
resizes, changes directory, and refreshes the worker while tracking RSS, open
descriptors, children, and reactor misses.

The native Issue 8 lexer, parser, and static execution planner share a
deterministic C fuzz driver. Valid function definitions additionally exercise
store construction, clone, segmented snapshot transfer, hostile headers, and
arbitrary snapshot mutations; `check-sanitize` also runs the structural
function-store test under ASan/UBSan. A separate black-box PTY profile feeds bounded
random editor/control-byte streams, resize events, invalid UTF-8 and NUL bytes,
then checks prompt recovery, child count, descriptor count, and terminal
restoration. Override its default with `PTY_FUZZ_CASES=number`. On a Clang
toolchain with the libFuzzer runtime, run coverage-guided fuzzing with:

```sh
make fuzz-libfuzzer FUZZ_SECONDS=60
```

Curated seeds and minimized, descriptively named regressions live in
`tests/corpus/lexer/` and are committed. libFuzzer loads them as read-only
inputs and writes newly discovered coverage cases to the ignored local
`dev/fuzz/lexer/` work corpus. Promote a generated hash file into the committed
corpus only after minimizing it and giving it a name that identifies the
behavior or defect it preserves.

Apple's bundled Clang may omit the libFuzzer runtime; `fuzz-sanitize` remains
available there. The reproducible Linux x86_64 container includes the Clang
runtime and exposes the same coverage-guided gate through Make:

```sh
make fuzz-libfuzzer-linux FUZZ_SECONDS=60
```

Run the reproducible clean-shell latency comparison with:

```sh
make bench
```

It alternates `gsh`, Bash without startup files, and Zsh with `-f`. The command
writes the complete result to the ignored local file
`dev/performance/current.csv` and prints only a three-line summary containing
the gate result, matrix size, and report path. Override the destination with
`BENCH_OUTPUT=/path/report.csv`; `make bench-record` remains an alias for the
same recording run. The default directory is created automatically; an
overridden destination must already have a parent directory.

The CSV has one row per test, shell, and measurement. Its leading columns hold
the revision, UTC timestamp, platform, tool versions, exact integer
p50/p95/p99/max values, the 5 ms deadline count, and paired-win gates. The
bounded `sample_001` through `sample_500` columns preserve every raw sample in
measurement order; unused cells remain empty. Latency is stored in nanoseconds
and memory in bytes so consumers never need to reverse formatted units. The
temporary report is written only after all measurements finish, synchronized,
and atomically renamed, so report I/O cannot enter a timed interval and a
failed run cannot replace the previous complete CSV.

All three shells emit the same-length base prompt.
Workloads currently cover startup, idle key echo,
`/usr/bin/true`, variable, builtin-command, and cached `PATH` lookup, assignment,
`${parameter:=word}`, arithmetic
assignment, a mutating expansion in a two-stage pipeline, `ulimit -S -n`,
`umask`, `times`, descriptor-only `exec`, `export`, `unset`, `readonly`,
finite explicit-list `for`,
`set -- a b c`, `shift`,
`set -Cf; set +Cf`, controlled `allexport` assignment and `nounset` lookup,
simple and fixed multi-star parameter pattern removal, enabled and disabled
pathname expansion, a non-regular output redirection under `noclobber`,
asynchronous builtin and external launch, `wait` for a completed job, and alias
definition, lookup/expansion, and removal.
Per-sample setup is outside the timed window, including the positional reset
before each `shift`. `command -v` selects the local comparison shells; override
them with `BASH_BIN=/path` or `ZSH_BIN=/path`.

The same run also records raw byte samples and p50/p95/p99/max memory. Startup
idle reports both the shell process and its persistent process tree. Each
command workload reports the shell lifetime peak, its delta over the
pre-command idle footprint, growth beyond the pre-command lifetime peak, and
the aggregate shell-plus-children peak and delta. Because normal commands can
finish between two kernel observations, the external-command and pipeline tree
measurements use equivalent children held for 20 ms (50 ms for the asynchronous
external workload) and sample every 1 ms.
macOS uses physical footprint/lifetime maximum and Linux uses `VmRSS`/`VmHWM`;
results are comparable between shells on the same host, not across operating
systems. Final same-revision matrices and their raw output belong in
the ignored local `dev/performance/` directory, separately from `build/`.

The `6917664` code candidate below is evidence for this machine, not a release
or cross-platform performance claim. It was measured on 2026-09-03 with
Darwin 25.6.0 arm64 (18 CPUs), Apple Clang 21.0.0, Bash 5.3.3, and Zsh 5.9.
The complete record contains 37 latency and 26 command-memory workloads; this
compact view shows startup, idle input, `exec`, alias paths, and the newly
explicit pathname-expansion cost. Cells are
p50 / p99 milliseconds over 120 startup, 500 key, or 300 command samples:

| Workload | `gsh` | Bash | Zsh |
| --- | ---: | ---: | ---: |
| startup | 5.953 / 6.729 | 4.535 / 5.357 | 4.820 / 5.449 |
| idle key | 0.011 / 0.014 | 0.011 / 0.015 | 0.010 / 0.014 |
| `exec` descriptor commit | 0.027 / 0.055 | 0.061 / 0.099 | 0.074 / 0.097 |
| alias define/update | 0.025 / 0.031 | 0.062 / 0.077 | 0.080 / 0.103 |
| alias lookup/expand | 0.020 / 0.028 | 0.052 / 0.065 | 0.059 / 0.077 |
| `unalias` | 0.023 / 0.029 | 0.059 / 0.073 | 0.074 / 0.098 |
| pathname expansion | 1.418 / 1.722 | 0.119 / 0.203 | 0.141 / 0.199 |

The direct-builtin majority gate passed at 900/900 paired wins against each
comparison shell, and the median p50 change across the 36 workloads shared with
the starting revision was 0.00%. Startup p50 was effectively unchanged from
that revision, but remains slower than Bash and Zsh in this sample. Pathname
enumeration is deliberately isolated from the reactor and is also materially
slower; a bounded persistent-service design is the next performance step.
The main process uses 56.55 MiB at idle and its persistent process tree uses
57.44 MiB, effectively unchanged from the starting revision but still a clear
footprint target. Re-run `make bench` after every affected implementation
change; never carry a result across revisions as if it were fresh evidence.
The full methodology, ordered raw values, percentiles, and first-use memory
growth are in the ignored local CSV named by the benchmark summary.

## Run

Start the interactive shell from a terminal:

```sh
./build/gsh
```

For example:

```text
gsh$ long-running-command
gsh* printf 'hello\n' | tr a-z A-Z
HELLO
gsh* cd /tmp
gsh* pwd
/tmp
gsh* sleep 1 &
[1] 12345
gsh* wait "$!" && printf 'done\n'
done
gsh$ rt
reactor cycles=... async_jobs=... focus=editor ...
```

Enter freezes the submitted prompt and command into a cell. Output adds rows
only when bytes arrive, so a silent command does not leave a blank row. The
fresh editor at the bottom accepts input immediately while independent cells
run and finish in any order. Shell-state mutations and `$?` dependencies remain
ordered. A
private-input program such as `sudo` receives focus automatically when its PTY
disables echo; the preserved editor remains unchanged and normal editing
resumes when the job settles. Non-canonical applications such as `htop`,
editors, pagers, and terminal coding agents receive an automatic contained
full-screen focus; their curses protocol is displayed live and the completed
cell keeps only `[full-screen session]`, not a raw control-sequence dump.
Line-oriented programs without a detectable transition receive input after
`fg`; `Ctrl-]` can detach any focused job without stopping it. The cell and PTY
limits are fixed, and saturation rejects new work instead of allocating
without bound.
Long loops and other compound commands do not fence unrelated external work:
a literal command such as `git status` starts immediately when the running
compound command has no pending mutation that can alter its launch state.

Programs that require traditional direct ownership of the physical terminal
can use classic mode:

```sh
GSH_REPL=classic ./build/gsh
```

The managed REPL is enabled by default and can be selected persistently with:

```text
shell.async_repl.enabled = true
```

An absent key also means `true`. `GSH_REPL=classic` forces classic mode, while
any explicit non-`classic` value forces managed mode. Enter `/async` as an
exact standalone interactive line to toggle the current session without
modifying `~/.gshrc`. If jobs, PTYs, input, state commits, or classic background
processes are still live, the requested transition remains pending; entering
`/async` again cancels it.

### Clickable files and native preview

The managed REPL recognizes file and directory references without inserting
terminal hyperlinks into command output. Native `ls` and `ll` send typed
metadata over a private close-on-exec channel; adapters recognize the output of
`/bin/ls`, `find`, `tree`, `fd`, `rg --files`, `git status`, `grep`, and `rg`;
the conservative fallback recognizes path-shaped tokens but excludes URLs and
bare words. Pathname cells are styled and clickable; in `ll`, the underline
and hitbox extend through the complete left-hand name field, including its
alignment padding. Output-carried
OSC, DCS, CSI, SOS, PM, and APC sequences cannot manufacture an action.
Because the managed REPL owns an alternate screen, mouse-wheel events scroll
its 10,240-line bounded viewport directly, including when the active prompt is
on the last terminal row. This also handles SGR wheel reports emitted by
iTerm2.

Clicking a regular file opens the native `view` builtin. At 100 columns or
more it opens beside a 45-percent REPL pane whose minimum width is 48 columns;
on narrower terminals it uses a contained full-screen panel. It uses bounded
`pread()` windows, syntax colors for Python,
shell, C/C++, JavaScript/TypeScript, Rust, Go, JSON, TOML/YAML, and Markdown,
and switches to a hex view when a NUL byte is present. `q` closes the viewer,
`/` searches, `n` and `N` move among results, `r` reloads, and `e` launches the
configured editor. A `file:line[:column]` reference opens at that location.
While the side viewer has focus, clicking another file in the left REPL pane
replaces the current preview in place. Returning from an external editor
clears its physical terminal frames before gsh redraws its own logical
scrollback, so editor contents do not reappear when scrolling upward.
Clicking a directory queues the equivalent of `cd -- PATH && ll` as a normal
ordered shell operation. Resource clicks do not enter command history, and
preview navigation does not change `$?`.

`ls` is a first-party C implementation of the POSIX Issue 8 option set:

```text
-A -C -F -H -L -R -S -a -c -d -f -g -i -k -l
-m -n -o -p -q -r -s -t -u -x -1
```

Its non-terminal output contains no colors, escape sequences, or visible
metadata. `ll [-a] [--] [PATH]` displays the filename at the left and responsive
metadata columns at the right, while `view [--] FILE` copies bytes unchanged
when its output is redirected or piped. Interactive managed `ll` listings begin
with a clickable `<-` row that navigates to the parent listing; at the
filesystem root the row is omitted. The directory change uses the same transactional path as
`cd`, so `PWD`, `OLDPWD`, and `$?` remain coherent.

The corresponding optional schema-version-1 settings are:

```text
terminal.actions = auto
terminal.actions.path_detection = safe
terminal.images = auto
shell.preview.editor = auto

# Exact override; one element must be {file}.
shell.preview.editor = ["nvim", "--", "{file}"]
```

`terminal.actions` accepts `auto`, `on`, or `off`. Path detection accepts
`off` (only native structured references), `known` (plus adapters), or `safe`
(plus conservative universal detection). Editor auto-discovery tries `nvim`,
then `vim`, then `nano`; automatic Vim/Neovim sessions show absolute line
numbers, while an explicit vector is never overridden and has no fallback.
`terminal.images` accepts `auto`, `on`, or `off`. `auto` uses only a graphics
protocol confirmed by terminal capability reporting, `on` permits active
bounded probes, and `off` skips probing and always reserves the image area with
a bordered diagonal placeholder.

Markdown files use a semantic preview for headings, lists, quotes, emphasis,
links, tables, fenced source blocks, formulas, and local images. Remote images
are never fetched. Local image paths are resolved relative to the document and
must remain regular files below that directory. `e` edits the Markdown source
at the selected source line and reloads it on return. When a confirmed image
protocol is absent, the same layout is retained as a bordered placeholder.
PDF files return to the existing generic, content-based text/hex viewer;
source, plain-text, and other binary previews are unchanged.

The future `?` steering and `??` AI queue described by specification 0008 are
not implemented yet; ordinary shell operation does not depend on an LLM.

Interactive command history retains at most 1024 accepted commands. Use the
left and right arrows to move through the current command, the up and down
arrows to navigate history, and `Ctrl-R` for incremental reverse search. A
complete command whose first and last bytes are ASCII spaces executes normally
but is not recorded. History is stored as owner-only text in
`~/.gsh_history`, using a zsh-style extended-history line with reversible
escaping for embedded newlines and backslashes. The file is loaded before the
first prompt and this session's entries are merged and atomically written on a
clean shell exit. A short sidecar lock preserves entries when multiple shells
exit concurrently; abnormal termination may lose only the unsaved session,
matching the traditional bash/zsh lifecycle.

`history status` reports the effective state and file path. Passphrases,
history agents, lock commands, and reminder prompts are not part of the
history lifecycle. A legacy `~/.gsh/history.vault` is neither read nor deleted
automatically.

`rt` exposes the bounded reactor's local service-time diagnostics. Its 5 ms
deadline applies only to work performed by the interactive core after `poll()`
wakes; it is not a guarantee about external commands or the host OS. `gsh$`
means every prior command is terminal and its output source is closed. `gsh*`
means at least one ordinary command is queued, running, stopped, has pending
input or output, or still owns a PTY. A suspended native preview is excluded:
it remains available to refocus but leaves the editable prompt as `gsh$`. The
prompt changes automatically from `gsh*` to `gsh$` when the session settles.
Progress output that uses carriage return,
including Git's compression and object-writing counters, rewrites one retained
row instead of turning every intermediate percentage into scrollback.

One command can also be executed without an interactive terminal:

```sh
./build/gsh -c 'printf "%s\n" "hello from gsh"'
```

As specified by POSIX, the first operand after the command string becomes `$0`
and the remaining operands become `$1`, `$2`, and so on:

```sh
./build/gsh --native-only -c \
  'for value; do printf "<%s>\n" "$value"; done' command-name a 'b c'
```

`-c` always uses the first-party evaluator. The historical `--native-only`
spelling remains available for focused tests, but regular non-interactive
invocation has the same no-delegation behavior:

```sh
./build/gsh --native-only -c \
  "/usr/bin/printf '%s\\n' native | /usr/bin/tr a-z A-Z"
```

An unsupported construct exits with status 2 instead of invoking another
shell. Native syntax checking without execution is available as
`./build/gsh -n -c 'command'`.

`-s`, implicit standard input, and command-file operands use that same native
evaluator and preserve `$0` plus the positional operands:

```sh
./build/gsh < script.sh
./build/gsh -s first 'second value' < script.sh
./build/gsh script.sh first 'second value'
```

Invocation accepts native `-a`, `-b`, `-C`, `-e`, `-f`, `-h`, `-n`, `-u`,
`-v`, and `-x`, their `+` forms, combined flags, and named `-o`/`+o` options.
Parsing is bounded and atomic: invalid combinations execute nothing and cannot
leave partially changed option state.

Descriptor input is streamed and committed one complete command at a time, so
the total source length is no longer capped. Command-file input uses a 16 KiB
read window. Standard input that shares its open file description with child
commands is rewound to the exact command boundary when seekable; a pipe or
other non-seekable input is read without prefetch past that boundary. Complete
commands up to 1 MiB stay in the preallocated fast window. Longer commands
spill in 16 KiB batches to one mode-0600 file that is immediately unlinked;
its descriptor is close-on-exec and private mappings exist only while parsing
and executing that command. Alias rewrites use a copy-on-write view of the same
backing file. There is therefore no shell-selected line or complete-command
input limit: address space and filesystem exhaustion are reported as system
resource failures. Null bytes remain invalid. Non-blocking standard input is
switched to blocking mode on the shared open file description before reading,
as required. Empty and comment-only complete commands preserve the prior
status. A trapped signal interrupts an otherwise idle input read, runs before
another byte is required, and resumes the same partial command without source
loss. Interactive unsupported syntax still has a compatibility fallback;
interactive `eval` and dot, non-interactive top-level input, and external
`ENOEXEC` scripts no longer use it.

## Current scope

The largest missing shell-language layer is the complete POSIX.1-2024 expansion
and evaluation runtime: interactive/monitor and `vi` option semantics, startup
environment processing, the remaining special parameters, shell-variable
attributes and scopes,
advanced operators on `$@`/`$*`, and the remaining expansion forms needed to
compose every valid word. Positional replacement and
shifting are native, but do not imply
the rest of `set` is complete. The currently supported `$(...)` and backquoted
forms capture a bounded result, reject embedded null bytes, remove trailing
newlines, use the shared initialization-time source arena, and are cancellable
with their containing job. Arithmetic implements
bounded checked signed-`long` parsing with precedence, short-circuiting, nested
parameter/command/arithmetic expansion, all eleven mandatory POSIX assignment
operators, classified diagnostics, and the required interactive and non-
interactive error behavior. Mutations are transactional and preserve pipeline,
subshell and command-substitution isolation.
Field splitting and globbing retain per-byte expansion and quote provenance,
including mixed literal/expanded words, quoted metacharacters, and the distinct
fields produced by quoted `$@`. Pathname component matching uses the active
locale for POSIX bracket classes and multibyte characters, preserves escaping
and leading-dot rules, and retains explicit directory-entry and candidate
ceilings. Nested advanced parameter operators remain delegated. Here-document
parameter, command, and arithmetic expansion are native.

Function definition redirects, invocation redirects, and execution in
pipelines, subshells, command substitutions, and asynchronous lists are native.
Non-interactive `eval` and dot scripts reuse those semantics in the current
environment, including function/alias definition and dot-local `return`.
When `command` suppresses their special-builtin properties, leading
assignments use an atomic fixed-store overlay: they are visible during the
source and exported to nested utilities, their names are restored afterward
even if made readonly, and all other source mutations still commit.
Function command-search and error semantics, attached redirections on the
remaining compound commands, required builtin utility pages outside the
protected tranche, monitor-mode terminal/notification edge cases, and dynamic
command-name or command-substitution forms of parent-owned `wait` are also
incomplete. The 30 atomic alias requirements are verified on macOS; native
Linux x86-64 release evidence is still required. Command
completion, AI requests through `?`,
journaling/rewind, and OS automation are also outside this MVP.

The core deliberately has no threads. One reactor remains the sole owner of
terminal, editor, environment, and job state, so there are no locks on the
interactive path and no multithreaded `fork()` hazards. Parallelism is supplied
by POSIX processes: all stages of a native pipeline run concurrently in one
process group; bounded here-document writers join that job and may block only
outside the reactor; persistent worker processes isolate potentially blocking
optional work. Background AND-OR lists use direct child processes, and simple
external jobs tail-`exec` without an evaluator wrapper. The current worker
allows one in-flight stateless output redirection. It cannot mutate shell state
or write to the terminal, and the reactor validates every result before a
status commit. Eligible redirections are serialized through that worker.
If one blocks on a FIFO, the reactor remains responsive and cancellation
replaces the worker.

This is soft real-time engineering, not hard real-time or mission-grade status.
The repository has executable conformance tranches, bounded fuzz/property
checks, sanitizer builds, 89 deterministic fault cases, resource-pressure
scenarios, and a configurable soak runner. The current native tranche contains
640 execution cases, 30 syntax cases, and 18 deterministic limit cases with
one explicitly unsupported case and no delegated cases. The current same-source
tranche passes the local macOS matrix and the pinned Linux x86-64 GCC/Clang
quality/conformance container. It does not gain same-revision release evidence
until the macOS arm64 and Ubuntu x86-64 GCC/Clang jobs run after publication.
Present coverage is also not yet complete.

The normative checklist and evidence-state rules live in
[`specs/0007.verification.md`](specs/0007.verification.md). CI defines macOS Clang
and Linux Clang/GCC jobs, but a matrix is considered verified only after those
jobs have actually run for the same revision; the workflow file alone is not
evidence of a passing platform. The current ignored suspension record is
`dev/status/posix-2024-notes.md` when present; its global percentage remains a
planning estimate, not a conformance or release score.

## Terminal compatibility

Graphics are an optional Markdown preview enhancement, not a prerequisite for
using gsh. The compositor emits Kitty Graphics or iTerm2 inline-image data only
after that capability has been confirmed across the active terminal, SSH, and
multiplexer chain. An unconfirmed or unavailable protocol degrades to a
bordered placeholder with diagonals; it never changes command output.

| Functionality | Classic VT/ANSI and SSH behavior |
| --- | --- |
| Shell language, builtins, pipelines, redirections and scripts | Independent from image protocols |
| `ls`, `ll`, text/source/hex `view` | Available normally; keyboard and commands remain available without mouse |
| Clickable resource actions | Enabled when mouse reporting survives the terminal or SSH/multiplexer chain |
| Markdown formatting | Textual formatting remains available |
| Markdown images | Kitty or iTerm2 when confirmed; bordered diagonal placeholder otherwise |
| Pipes, redirects and non-interactive SSH | Canonical output without graphics or terminal-control metadata |

Older terminals therefore lose only images, mouse interaction, and some visual
richness. The normal shell remains compatible over SSH. An interactive SSH
session normally needs a PTY (for example, `ssh -t host gsh`), while
`ssh host gsh -c ...` remains non-interactive and emits neither graphics nor
terminal-control metadata. gsh never sends an unconfirmed graphics protocol.
