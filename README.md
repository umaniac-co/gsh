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

The mandatory code-writing policy for human contributors and AI agents is
[`CODE.md`](CODE.md). It adopts JPL's Power of Ten rules and gsh's Literate Code
discipline; new and materially changed code must follow it, while legacy
violations remain explicit migration work rather than precedent.

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
- asynchronous AND-OR lists use a bounded direct-child registry; `$!` and
  reactor-driven `wait` stay with the process that actually owns those PIDs;
- shell output uses a fixed-size nonblocking queue;
- the base prompt is immediate and independent from optional integrations;
- a persistent process computes Git prompt enrichment and services eligible
  stateless output redirections concurrently through a bounded `socketpair()`
  protocol;
- prompt requests have identity, generation, and a 100 ms monotonic deadline;
  stale results are discarded and a stalled or malformed worker is disabled;
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
Command substitution and pathname enumeration execute only in isolated
evaluator processes; neither can block the interactive reactor. `-c` maps its
command name and arguments to `$0` and the positional parameters, with native
`$#`, numbered parameters, `$@`, `$*`, and `$-`; quoted `$@` retains its
multi-field semantics. `set --` and operand forms replace or clear positional
parameters in a bounded zero-fill arena; `shift` validates an unsigned decimal
operand and advances its live window in O(1). `set -a`/`+a` (`allexport`),
`set -C`/`+C` (`noclobber`), `set -f`/`+f` (`noglob`), and `set -u`/`+u`
(`nounset`), their `-o` forms, `$-`, sorted variable output, and reusable
`set +o` output are native. Allexport marks assignments for descendant
environments without changing command-local scope. Nounset rejects unset
parameter expansion while preserving the POSIX default, alternative, assign,
and error modifier rules. The four POSIX parameter pattern-removal operators
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
Asynchronous AND-OR lists, `$!`, and `wait` are native. The interactive shell
tracks at most 128 direct background children without allocation. Sequential
and `&&`/`||` continuations keep `wait` in the owning reactor, Ctrl-C cancels
the wait without killing the job, and a simple external background command
tail-`exec`s in its direct child rather than retaining an evaluator wrapper.
Explicit-list `for` and the implicit `for name` form are native. A finite
literal loop with a provably pure `:`, `true`, or `false` body can be reduced
inside the reactor under a 16-operation admission budget; loops with expansion,
I/O, nesting beyond that budget, or other bodies use the isolated native
evaluator. POSIX aliases are native: a lazy fixed-capacity hash and compact
arena provide allocation-free steady-state lookup, parser-guided recursive
substitution preserves token provenance, and `alias`/`unalias` changes become
visible in complete-command order. Subshell and asynchronous changes remain
isolated, while compound mutations commit through a bounded rollback journal.
Function definition, deferred expansion, positional invocation scope,
environment mutation, `return`, redefinition, and `unset -f` have native
bounded implementations. Definition/call redirections and function execution
inside pipelines, subshells, command substitutions, and asynchronous lists are
also native and bounded. The complete Issue 8 error/search semantics,
performance gate, and platform evidence remain incomplete. The remaining
`set` options beyond
`a`/`C`/`f`/`u`, full monitor-mode job control, and the remaining nested
expansion forms are still
incomplete. The compatibility bridge must disappear
before `gsh` can claim POSIX.1-2024 shell-language conformance.

The builtins implemented in `gsh` itself include `cd`, `exit`, `pwd`, `export`,
`readonly`, `unset`, `ulimit`, `umask`, `:`, `true`, `false`, `fg`, `bg`,
`set`, `shift`, `wait`, `alias`, `unalias`, `help`, and `rt` within their
currently documented contexts.
`ulimit` implements the POSIX.1-2024 `-H`,
`-S`, `-a`, `-c`, `-d`, `-f`, `-n`, `-s`, `-t`, and `-v` resource interface.
`umask` implements octal masks, `-S`, and POSIX symbolic masks including
permission copying and the initial-mode semantics of `X`. An unredirected
standalone invocation of either environment builtin changes the current shell,
while a pipeline stage remains isolated. The editor is intentionally limited
to insertion at the end of the line, bounded multiline input with `PS2`,
UTF-8-aware backspace, `Ctrl-U`, `Ctrl-L`, `Ctrl-C`, and `Ctrl-D`. The managed
REPL retains 16 bounded cells and runs at most 8 PTY command jobs concurrently;
`fg` focuses the newest live job, and `Ctrl-]` returns keyboard ownership to
the editor. The separate POSIX asynchronous-list registry holds 128 direct
children.

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

The executable is written to `build/gsh`. To remove it:

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
fallback, stop/`fg`/`Ctrl-C` job control, asynchronous Git prompt enrichment,
isolation of a worker blocked on filesystem I/O, and exact terminal-mode
restoration.

Additional reliability gates are:

```sh
make check-fault
make check-resource
make check-positionals
make check-background
make check-functions
make analyze
make fuzz-smoke
make fuzz-sanitize
make fuzz-pty
make fuzz-pty-sanitize
make soak SOAK_SECONDS=60
```

`check-fault` uses a separate test-only binary and exercises 65 deterministic
failure points. The production build contains neither the injection
configuration nor its fault names. `check-resource` covers runtime descriptor
and process exhaustion, data-limit capability, bounded single-line and
multiline input, signal storms, recovery after a rejected command, and failed
initialization. Limits applied after startup use gsh's native `ulimit`, so the
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
available there, while the Linux Clang CI job runs the coverage-guided gate.

Run the reproducible clean-shell latency comparison with:

```sh
make bench
```

It alternates `gsh`, Bash without startup files, and Zsh with `-f`, reporting
the tool versions, every ordered raw nanosecond sample, p50, p95, p99, maximum,
and samples over 5 ms. All three shells emit the same-length base prompt.
Workloads currently cover startup, idle key echo,
`/usr/bin/true`, variable lookup, assignment, `${parameter:=word}`, arithmetic
assignment, a mutating expansion in a two-stage pipeline, `ulimit -S -n`,
`umask`, `export`, `unset`, `readonly`, finite explicit-list `for`,
`set -- a b c`, `shift`,
`set -Cf; set +Cf`, controlled `allexport` assignment and `nounset` lookup,
simple and fixed multi-star parameter pattern removal, disabled pathname
expansion, a non-regular output redirection under `noclobber`, asynchronous
builtin and external launch, `wait` for a completed job, and alias definition,
lookup/expansion, and removal.
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

The current local worktree snapshot below is evidence for this machine, not a
release or cross-platform performance claim. It was measured on 2026-08-27 with
Darwin 25.5.0 arm64 (18 CPUs), Apple Clang 21.0.0, Bash 5.3.3, and Zsh 5.9.
The complete record contains 29 latency and 26 command-memory workloads; this
compact view shows startup, idle input, and the new alias paths. Cells are
p50 / p99 milliseconds over 120 startup, 500 key, or 300 command samples:

| Workload | `gsh` | Bash | Zsh |
| --- | ---: | ---: | ---: |
| startup | 2.646 / 3.210 | 4.403 / 5.281 | 4.917 / 6.060 |
| idle key | 0.011 / 0.015 | 0.011 / 0.015 | 0.011 / 0.014 |
| alias define/update | 0.063 / 0.079 | 0.072 / 0.083 | 0.086 / 0.105 |
| alias lookup/expand | 0.039 / 0.048 | 0.059 / 0.070 | 0.064 / 0.076 |
| `unalias` | 0.056 / 0.067 | 0.069 / 0.080 | 0.080 / 0.091 |

The independent alias-only repeat confirms that gsh is lower than both
comparison shells at p50, p95, p99, and maximum for all three alias workloads.
Its main process uses 1.672 MiB at idle, versus 2.297 MiB for Bash and 1.828 MiB
for Zsh in that repeat. Its complete 2.579 MiB process tree is still 0.282 MiB
and 0.751 MiB larger respectively because of the persistent worker; that fixed
cost remains an explicit optimization target. Re-run `make bench` after every
affected implementation change; never carry a result across revisions as if it
were fresh evidence. The full comparison, direct differences, methodology,
ordered raw values, and first-use memory growth are in the ignored local file
`dev/performance/2026-08-27-macos-arm64-alias.md` when that evidence is present.

## Run

Start the interactive shell from a terminal:

```sh
./build/gsh
```

For example:

```text
$gsh> long-running-command

$gsh> printf 'hello\n' | tr a-z A-Z
HELLO
$gsh> cd /tmp
$gsh> pwd
/tmp
$gsh> sleep 1 &
[1] 12345
$gsh> wait "$!" && printf 'done\n'
done
$gsh> rt
reactor cycles=... async_jobs=... focus=editor ...
```

Enter freezes the submitted prompt and command into a cell with one initial
output row. The cell grows when additional output rows arrive. The fresh editor
at the bottom accepts input immediately while independent cells run and finish
in any order. Shell-state mutations and `$?` dependencies remain ordered. A
terminal program receives input only after
`fg`; `Ctrl-]` detaches it without stopping it. The cell and PTY limits are
fixed, and saturation rejects new work instead of allocating without bound.
Long loops and other compound commands do not fence unrelated external work:
a literal command such as `git status` starts immediately when the running
compound command has no pending mutation that can alter its launch state.

Programs that require traditional direct ownership of the physical terminal
can use classic mode:

```sh
GSH_REPL=classic ./build/gsh
```

The future `?` steering and `??` AI queue described by specification 0008 are
not implemented yet; ordinary shell operation does not depend on an LLM.

`rt` exposes the bounded reactor's local service-time diagnostics. Its 5 ms
deadline applies only to work performed by the interactive core after `poll()`
wakes; it is not a guarantee about external commands or the host OS. In a Git
working tree, the optional branch segment appears asynchronously, for example
`[main] $gsh>`. Typing and the base prompt never wait for it.

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

`-c` chooses the native path when the implemented subset is sufficient and
otherwise uses the compatibility bridge. Tests can prohibit delegation with:

```sh
./build/gsh --native-only -c \
  "/usr/bin/printf '%s\\n' native | /usr/bin/tr a-z A-Z"
```

An unsupported construct then exits with status 2 instead of invoking another
shell. Native syntax checking without execution is available as
`./build/gsh -n -c 'command'`.

Input redirected from a file is passed to `/bin/sh`, which makes this work as a
minimal script mode:

```sh
./build/gsh < script.sh
```

## Current scope

The largest missing shell-language layer is the complete POSIX.1-2024 expansion
and evaluation runtime: the remaining `set` options beyond `a`/`C`/`f`/`u`,
the remaining special parameters, shell-variable attributes and scopes,
advanced operators on `$@`/`$*`, and the remaining expansion forms needed to
compose every valid word. Positional replacement and
shifting are native, but do not imply
the rest of `set` is complete. The currently supported `$(...)` and backquoted
forms capture a bounded result, reject embedded null bytes, remove trailing
newlines, and are cancellable with their containing job. Arithmetic implements
bounded checked signed-`long` parsing with precedence, short-circuiting, nested
parameter/command/arithmetic expansion, all eleven mandatory POSIX assignment
operators, classified diagnostics, and the required interactive and non-
interactive error behavior. Mutations are transactional and preserve pipeline,
subshell and command-substitution isolation.
Field splitting and globbing retain per-byte expansion and quote provenance,
including mixed literal/expanded words, quoted metacharacters, and the distinct
fields produced by quoted `$@`. Nested advanced parameter operators remain
delegated. Here-document parameter, command, and arithmetic expansion are
native.

Function definition redirects, invocation redirects, and execution in
pipelines, subshells, command substitutions, and asynchronous lists are native.
Function command-search and error semantics, attached redirections on the
remaining compound commands, the remaining required builtins, monitor-mode
job selection/notification, and dynamic
command-name or command-substitution forms of parent-owned `wait` are also
incomplete. The 30 atomic alias requirements are verified on macOS; native
Linux x86-64 release evidence is still required. Command
history and completion, AI requests through `?`,
journaling/rewind, and OS automation are also outside this MVP.

The core deliberately has no threads. One reactor remains the sole owner of
terminal, editor, environment, and job state, so there are no locks on the
interactive path and no multithreaded `fork()` hazards. Parallelism is supplied
by POSIX processes: all stages of a native pipeline run concurrently in one
process group; bounded here-document writers join that job and may block only
outside the reactor; persistent worker processes isolate potentially blocking
optional work. Background AND-OR lists use direct child processes, and simple
external jobs tail-`exec` without an evaluator wrapper. The current worker
allows one in-flight request plus one
coalesced Git refresh; it cannot mutate shell state or write to the terminal,
and the reactor validates every result before a redisplay or status commit.
Eligible stateless output redirections are serialized through the same worker.
If one blocks on a FIFO, the reactor remains responsive and cancellation
replaces the worker.

This is soft real-time engineering, not hard real-time or mission-grade status.
The repository has executable conformance tranches, bounded fuzz/property
checks, sanitizer builds, 65 deterministic fault cases, resource-pressure
scenarios, and a configurable soak runner. The current same-source tranche
passes the local macOS matrix and a native-ISA Ubuntu ARM64 GCC/Clang matrix.
That Linux execution is not amd64 translation, but it still does not satisfy
the Linux x86-64 release matrix required by principle 007; present coverage is
also not yet complete.

The normative checklist and evidence-state rules live in
[`specs/0007.verification.md`](specs/0007.verification.md). CI defines macOS Clang
and Linux Clang/GCC jobs, but a matrix is considered verified only after those
jobs have actually run for the same revision; the workflow file alone is not
evidence of a passing platform. The latest same-source local gate record is
`dev/status/2026-08-27-alias-matrix.md` when that ignored evidence is present;
its ARM64 Linux rows are portability evidence, not `verified-linux` release
evidence.
