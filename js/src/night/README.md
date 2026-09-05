# NightMonkey: AOT JavaScript-to-WebAssembly compilation

**NightMonkey** is an ahead-of-time JS-to-Wasm compilation tier built
inside SpiderMonkey. NIGHT expands to *Nonlocal Inference with Guiding
Heuristics for Types*: an optimistic whole-program type analysis guides
code generation, with dynamic guards for correctness. (The night monkey,
genus *Aotus*, is the only truly nocturnal monkey: it does its work in
the night, before the program runs during the day.)

Each JS function's bytecode is compiled to a WebAssembly function that
runs alongside the runtime compiled to Wasm. There are two modes of use:

- **Snapshot** (the shipping flow): the `nightmonkey` host binary drives
  Wizer in-process to snapshot the runtime plus loaded user program (or
  processes an existing Wizer snapshot), reads out JS bytecode and heap
  objects (such as prototype objects), and rewrites that snapshot with
  compiled bodies.
- **In-process** (the testing flow): the JS shell compiled to Wasm runs
  under `wasm-jit-runner`, walks its own live heap, compiles the script
  tree, and injects the bodies into its running instance via runner
  hostcalls (`--night-inprocess`). A drop-in shell for jit-tests.

NightMonkey has a two-part structure: an *optimistic static type
analysis* and a *guard-based codegen backend*. The idea is that we:

1. "Predict" types statically, using a model of JavaScript semantics
   that is intentionally optimistic (elides corner-cases). We call this
   the "likelier-types analysis" (in a nod to the initial version of the
   analysis, the "likely-types analysis"; this one is a little better).

2. Generate an optimistic Wasm body for a given JS function bytecode
   body, using those predicted types.

3. Insert dynamic guards checking those assumptions, with fallbacks to a
   fully generic (but still compiled!) Wasm body.

The *key constraint* that NightMonkey adheres to, and attempts to solve:
we cannot derive type information, or any other profiling information,
by observing a running program. In other words, unlike the standard JIT
approach based on the "JIT hypothesis" (that a warmed-up program will
reach a steady state with stable types, which we can then specialize
for), we must decide any specialization we will do ahead-of-time, based
on whatever analysis or heuristics we can come up with. The thing we
permit ourselves in return is much more analysis time: unlike a JIT
engine, we do not need to compile in milliseconds.

NightMonkey performs its analysis using a whole-program, call-sensitive,
points-to (heap abstraction) + callgraph analysis, over a lattice that
is a hybrid of a Steensgaard (union-find-based) and capped Andersen
(points-to-set/membership-based) design.

The codegen using the types that come out of this analysis is then a
"two-track" approach: there is one optimistic track that adheres to
"type contexts" that are maximally optimal, and one fully generic track.
(Earlier experiments tried to do more multiversioning, a la Static Basic
Block Versioning, but that did not converge well.)

## Layout

| Path | Contents |
|---|---|
| `compiler/` | The compiler crate (`night-compiler`). |
| `compiler/night-compiler.h` | C ABI between SpiderMonkey and the compiler. |
| `compiler/src/source.rs`, `src/source/ffi.rs` | The `Source` object graph: the sole input to the compiler. |
| `compiler/src/bytecode.rs` | Bytecode parser and `OpcodeVisitor` (generated `JSOp` enum from Opcodes.h). |
| `compiler/src/options.rs` | `Options`/`Diagnostics`: the entire configuration surface. |
| `compiler/src/likelier/` | The speculative likely-types analysis (`scan`/`heap`/`calls`/`engine`/`emit`/`dump`). |
| `compiler/src/opsem.rs` | Interval algebra and op semantics; the vocabulary shared by analysis and codegen. |
| `compiler/src/facts.rs` | `LikelyFacts`: the analysis-to-codegen fact contract. |
| `compiler/src/wasm/bbv.rs` | The workqueue-BBV bytecode-to-Wasm codegen driver. |
| `compiler/src/wasm/translate.rs` | Shared translation substrate: `Helpers`/`AtomTable`/`Outcome`/ctx types, layout constants. |
| `compiler/src/wasm/regex.rs` | The regex AOT compiler (irregexp bytecode to Wasm matchers). |
| `compiler/src/wasm/mod.rs` | The `layout_env` / `translate_all` seams: analysis prepass, reserved linear-memory region layout, body translation, table patching. |
| `compiler/src/wasm/inprocess.rs` | In-process batch builder for the runner hostcalls. |
| `runtime/` | The night runtime: `NightRuntime.cpp` is the `night_runtime_*` C ABI generated code calls, in front of the engine halves it forwards to -- `NightOps.cpp` (bytecode ops), `NightInlineCaches.cpp` (property-cache populate and replay), `NightInlineHeap.cpp` (inline allocation, write barriers, and the baked-layout asserts), `NightGenerator.cpp`, `NightRegExp.cpp`. `NightEntry.cpp` is the other direction: entering compiled bodies. Plus the value stack and snapshot registration/activation/capture. Linked into the shell under `--enable-nightmonkey`. |
| `snapshot/` | Snapshot/live-heap reader crate (`night-snapshot`): parses the registration block and walks the script graph into a `Source`. |
| `nightmonkey/` | The `nightmonkey` binary: snapshot in, AOT-compiled module out. The optional `wizen` Cargo feature also accepts programs and drives wizer as a library. |
| `wasm-jit-runner/` | Wasmtime-based runner exposing function-injection hostcalls for the in-process flow. |
| `configs/` | The mozconfigs (see "Builds"). |
| `docs/` | `DESIGN.md`, `INTEGRATION.md`, `TODO`. |
| `tools/` | Profiling, benchmarking, and visualization helpers (`viz.py`, `opprof.py`, `pairab.sh`, ...). |
| `inproc-shell.sh` | `jit_test.py`/`jstests.py` shim running the wasm shell under the runner; the in-process build installs a copy into `dist/bin`. |

## Build flags

- `--enable-nightmonkey` (wasm32 targets only; configure errors otherwise):
  links the night runtime into the shell, enables wizer snapshot
  registration, and builds the `nightmonkey` host binary into
  `dist/host/bin`.
- `--enable-nightmonkey-inprocess` (requires `--enable-nightmonkey`): links
  the compiler crate into the shell (the `--night-inprocess` flag), adds the
  wasm-jit-runner hostcall imports to the shell module, and builds the
  `wasm-jit-runner` host binary into `dist/bin`.

## Prerequisites

- A Rust toolchain with the `wasm32-wasip1` target
  (`rustup target add wasm32-wasip1`).
- `wasmtime` on `$PATH` or at `$HOME/bin/wasmtime`, to run compiled modules.

Wizer is a library dependency of `nightmonkey`; there is nothing to install.

All `./mach build` invocations run from the repo root, against the whole
tree (never a subdirectory), and never concurrently with another build.

## Flow 1: snapshot (the shipping flow)

Step 1 — build the wizerable wasm shell (also produces the compiler):

```
MOZCONFIG=js/src/night/configs/mozconfig-nightmonkey ./mach build
# -> obj-nightmonkey/dist/bin/js               (wasm32-wasi shell)
# -> obj-nightmonkey/dist/host/bin/nightmonkey (the AOT compiler)
```

Step 2 — compile a program, in one command:

```
obj-nightmonkey/dist/host/bin/nightmonkey \
    --shell obj-nightmonkey/dist/bin/js program.js -o program-aot.wasm
wasmtime run program-aot.wasm
```

`nightmonkey` snapshots the shell with wizer in-process, then rewrites the
snapshot with compiled bodies. The program's top level runs *during*
wizening, so setup and class construction are captured in the image, and the
resumed snapshot calls the program's global `main()`.

Passing a pre-made snapshot instead of a `.js` file also works, and is the
fast inner loop for compiler work:

```
nightmonkey --shell <js.wasm> program.js --keep-snapshot snap.wasm -o out.wasm
nightmonkey snap.wasm -o out.wasm      # recompile without re-wizening
```

`nightmonkey --help` lists the diagnostics (`--stats`, `--dump-bytecode`,
`--dump-bbv`, `--dump-facts`, `--dump-graph`, `--viz`, `--viz-lower`,
`--viz-facts`) and the compilation options (`--force-interp`,
`--keep-names`). `--dump-bytecode`
takes an optional comma-separated source-id list
(`--dump-bytecode=145,153`); a whole-bundle disassembly is megabytes.
Debug sections are stripped by default; `--keep-names` retains them.

## Flow 2: in-process (drop-in shell for jit-tests)

Step 1 — build the in-process shell and the runner:

```
MOZCONFIG=js/src/night/configs/mozconfig-nightmonkey-inprocess ./mach build
# -> obj-nightmonkey-inprocess/dist/bin/js               (wasm32-wasi shell)
# -> obj-nightmonkey-inprocess/dist/bin/wasm-jit-runner  (the test host)
# -> obj-nightmonkey-inprocess/dist/host/bin/nightmonkey (also usable for flow 1)
```

Step 2 — run a program:

```
obj-nightmonkey-inprocess/dist/bin/wasm-jit-runner \
    --dir / --cache-dir ~/.cache/wjr \
    obj-nightmonkey-inprocess/dist/bin/js -- --night-inprocess /abs/path/program.js
```

The script path must be **absolute**: the guest resolves paths against the
runner's preopen root (`--dir /`). `--cache-dir` caches the compiled shell.
Everything after `--` goes to the JS shell. Omitting `--night-inprocess` runs
the same binary as a plain interpreter — the differential baseline.

Step 3 — the full jit-test suite in both lanes:

```
python3 js/src/jit-test/jit_test.py -j16 js/src/night/inproc-shell.sh
NIGHT_INPROCESS_OFF=1 python3 js/src/jit-test/jit_test.py -j16 js/src/night/inproc-shell.sh
```

The same shim serves jstests. Through mach, the installed copy stands in for
the shell (the jstests harness finds the objdir from its path):

```
MOZCONFIG=js/src/night/configs/mozconfig-nightmonkey-inprocess \
    ./mach jstests --shell obj-nightmonkey-inprocess/dist/bin/inproc-shell.sh
```

Both lanes are expected to pass completely (append a directory like `basic`
to scope). Two directive families keep it that way:

- `skip-if: nightTierEnabled()` — tests exercising designed-out capability:
  the debugger / frame-introspection / interrupt classes, plus an annotated
  artifact class (GC-introspection tests sensitive to the tier's
  literal-string and allocation profile; each carries an in-file comment).
- Platform skips in the upstream idiom, applying to BOTH lanes of the wasi
  shell: `typeof Intl === 'undefined'`, `!this.SharedArrayBuffer`,
  `!this.Atomics`, and `getBuildConfiguration("wasi")`.

## Build-system notes

- The compiler's dependencies (waffle and the wasm-tools crates) come from
  **crates.io**, not from `third_party/rust`: `.cargo/config.toml.in` does
  not apply the vendored-source replacement, so `third_party/` carries no
  NightMonkey diff. That is a deliberate development override for this fork,
  which has no offline-build constraint; see the comment in that file for why
  it cannot be scoped to NightMonkey alone.
- `night-compiler` is a workspace member because it is linked into the
  `jsrust` staticlib for the in-process lane.
- `nightmonkey` and `wasm-jit-runner` are **standalone cargo projects**,
  deliberately outside the workspace: both pull in a wasmtime-class
  dependency tree (nightmonkey via wizer) that must not enter the root
  `Cargo.lock`. Each is driven by a forced `GENERATED_FILES` step
  (`build_nightmonkey.py`, `build_wasm_jit_runner.py`); cargo owns the
  incrementality, so a no-op rebuild is subsecond.
- `cargo check` inside `compiler/` typechecks the compiler crate quickly, but
  only `./mach build` links it into the wasm shell.

For performance work, the benchmark-lane configs
(`mozconfig-native`/`-ion`/`-wasm`/`-weval`) live in `configs/` too;
benchmark A/B only with interleaved same-binary runs pinned to one core.

## Documentation

- **[`docs/DESIGN.md`](docs/DESIGN.md)** — the design of record: the
  soundness model and the object stamp, the BBV emission strategy, the
  layered lowerings for the common opcodes, the analysis (data structures,
  lattices, abstract interpretation), the runtime ABI, and the known
  limitations and rough edges.
- **[`docs/INTEGRATION.md`](docs/INTEGRATION.md)** — everything NightMonkey
  touches outside `js/src/night/`, organized by mechanism, with the
  stock-build cost of each unconditional change.
- **[`docs/TODO`](docs/TODO)** — the production TODO: holes in completeness
  and productionization.

## Performance

As of 2026-09-04, comparing to native IonMonkey and baseline tiers, and
against Wasm-hosted interpreter and weval+PBL execution:

```plain
bench            native-ion nat-baseline  wasm-interp        weval          aot   aot/wasm-int      aot/weval weval/wasm-int      ion/weval        ion/aot   baseline/aot
richards              29205         6489          377          936        11893          31.55          12.71           2.48          31.20           2.46           0.55
deltablue             28179         6870          395          978         6678          16.91           6.83           2.48          28.81           4.22           1.03
crypto                42755         5654          714          949        15696          21.98          16.54           1.33          45.05           2.72           0.36
raytrace              58549        11458         1045         1815        11964          11.45           6.59           1.74          32.26           4.89           0.96
earley-boyer          83262        21153         1510         3982        15088           9.99           3.79           2.64          20.91           5.52           1.40
navier-stokes         43926         8269         1223         2090        24980          20.43          11.95           1.71          21.02           1.76           0.33
splay                 29291        23303         5248         6853        10693           2.04           1.56           1.31           4.27           2.74           2.18
regexp                18601         7223          596          766         2484           4.17           3.24           1.29          24.28           7.49           2.91
pdfjs                 95804        40738         4116         6185        24743           6.01           4.00           1.50          15.49           3.87           1.65
mandreel              73940        11619          865         1269        19545          22.60          15.40           1.47          58.27           3.78           0.59
code-load             70224        69259        37108        37005        37271           1.00           1.01           1.00           1.90           1.88           1.86
box2d                 99321        22370         1896         4135        25999          13.71           6.29           2.18          24.02           3.82           0.86
react-bench           0.631        1.415       15.026       10.421        2.862           5.25           3.64           1.44          16.52           4.54           2.02
geomean               49233        14111         1527         2569        14247           8.93           5.37           1.66          18.94           3.53           1.05
(octane = Score higher-better; react-bench = ms/render lower-better; best-of-3, taskset -c 1)
(ratio cols = speedup of A over B, direction-corrected for react-bench;
 geomean row: lane cols over octane scores only, ratio cols over all benches)
```

We can conclude that NightMonkey is ~9x faster than the Wasm interpreter on
average, or ~5x faster than weval+PBL. It is nearly on par with the native
baseline compiler, and within ~3.5x of the IonMonkey optimized native-code
ceiling (while running within a Wasm engine). On benchmarks where type-based
specialization works especially well, NightMonkey comes within ~2.5x (e.g.
Richards) of native Ion.
