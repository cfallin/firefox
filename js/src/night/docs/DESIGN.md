# NightMonkey: design of the AOT JavaScript-to-WebAssembly tier

**Status:** design of record.

**Audience:** a compiler engineer joining the project, or a reviewer
evaluating it for upstreaming. It assumes you know compilers and WebAssembly
and nothing about this codebase. Where a mechanism is intricate, the document
says so and explains it rather than summarising it away.

**Authority:** the source. Every claim here was checked against the code, and
the document cites `file:function` or `file:line` rather than other prose.
Where the source's own comments contradict the code, the code wins and the
discrepancy is called out.

---

## Table of contents

- [1. What this is, in one page](#1-what-this-is-in-one-page)
- [2. The soundness model](#2-the-soundness-model)
  - [2.1 The contract](#21-the-contract)
  - [2.2 The object stamp](#22-the-object-stamp)
  - [2.3 What each stamp bit licenses](#23-what-each-stamp-bit-licenses)
  - [2.4 Non-stamp discharge mechanisms](#24-non-stamp-discharge-mechanisms)
  - [2.5 Failure modes, ranked](#25-failure-modes-ranked)
- [3. Repository map](#3-repository-map)
- [4. The code-emission strategy: workqueue BBV](#4-the-code-emission-strategy-workqueue-bbv)
  - [4.1 What a version is](#41-what-a-version-is)
  - [4.2 The context](#42-the-context)
  - [4.3 The version table and the interning discipline](#43-the-version-table-and-the-interning-discipline)
  - [4.4 The driver](#44-the-driver)
  - [4.5 The continuation contract](#45-the-continuation-contract)
  - [4.6 theta: the single merge point](#46-theta-the-single-merge-point)
  - [4.7 Tracks: what replaced the OPT/OVF/GEN attractors](#47-tracks-what-replaced-the-optovfgen-attractors)
  - [4.8 Loops: headers, back edges, side entries, on-ramps](#48-loops-headers-back-edges-side-entries-on-ramps)
  - [4.9 Reducibility by construction](#49-reducibility-by-construction)
  - [4.10 Carriers: values that cross versions unboxed](#410-carriers-values-that-cross-versions-unboxed)
  - [4.11 Effects and LICM](#411-effects-and-licm)
  - [4.12 Capacity: the compile ladder](#412-capacity-the-compile-ladder)
  - [4.13 The program-point graph](#413-the-program-point-graph)
- [5. Lowerings](#5-lowerings)
  - [5.1 Value representation and the cost primitives](#51-value-representation-and-the-cost-primitives)
  - [5.2 GetProp](#52-getprop)
  - [5.3 SetProp](#53-setprop)
  - [5.4 GetElem and SetElem](#54-getelem-and-setelem)
  - [5.5 Call](#55-call)
  - [5.6 Construct](#56-construct)
  - [5.7 The inline splice](#57-the-inline-splice)
  - [5.8 Binary arithmetic](#58-binary-arithmetic)
  - [5.9 Comparisons](#59-comparisons)
  - [5.10 ToBoolean](#510-toboolean)
- [6. The analysis](#6-the-analysis)
- [7. opsem: the shared vocabulary](#7-opsem-the-shared-vocabulary)
- [8. The runtime and the ABI](#8-the-runtime-and-the-abi)
- [9. The two flows](#9-the-two-flows)
- [10. The regex compiler](#10-the-regex-compiler)
- [11. Limitations](#11-limitations)
- [12. Rough edges](#12-rough-edges)
- [13. Glossary](#13-glossary)

---

## 1. What this is, in one page

NightMonkey is an ahead-of-time compilation tier for JavaScript, built inside
SpiderMonkey, that emits WebAssembly. The name expands to *Nonlocal Inference
with Guiding Heuristics for Types*.

The execution model is unusual and worth internalising before anything else:

> **There is exactly one execution world and it is all-WebAssembly.**

SpiderMonkey itself is compiled to `wasm32-wasi` (the *wasm shell*). The
compiler does not emit a separate module that imports the engine; it
**appends** compiled JS function bodies into the engine's own function index
space, table and linear memory. Consequently "call a runtime helper" and
"call another JS function" are both plain in-module wasm calls, and a compiled
body can read engine data structures with ordinary `i32.load`s at
statically-known offsets. Two wasm modules sharing one linear memory would
require a PIC dynamic-linking convention the engine build does not satisfy;
merging sidesteps that entirely. The backend is
[waffle](https://crates.io/crates/waffle) 0.3.1, whose lazy function bodies
let the ~9 MB engine module pass through as raw bytes while only the appended
functions are lifted to SSA IR.

Each JS function is compiled to one wasm function that produces output
byte-identical to the interpreter, or is not compiled at all. Two flows share
one compiler (section 9): a **snapshot** flow (wizer snapshots the shell with
the program registered; the `nightmonkey` host binary rewrites the snapshot
with compiled bodies; stock `wasmtime` runs the result) and an **in-process**
flow (the shell compiles its own scripts and injects them into its running
instance through host calls; this is the jit-test vehicle).

The compiler's inputs are **the program's bytecode and the snapshot's heap
state**, and nothing else. There is a standing design constraint behind this:

> **No profiling input, ever.** The compiler may never require running the
> workload to collect a profile. Target programs may be runnable only in
> production, and there is no control plane that could feed profiles back;
> requiring one is not a requirement we can impose on a user of the compiler.
> The snapshot image is program *state*, not an execution trace, so reading it
> is allowed. Count-based or first-seen dynamic recording feeding compile-time
> decisions is not. Where static analysis and heuristics reach their limit, the
> answer is to stop, not to profile.

Two pieces do the work:

- **`compiler/src/likelier/`** — a speculative, context-sensitive,
  whole-bundle type analysis producing *likely facts*: callee sets, instance
  layouts, receiver classes, per-field numeric masks and value ranges. Nothing
  it produces is trusted (section 6).
- **`compiler/src/wasm/bbv.rs`** — workqueue **basic-block versioning**, in two
  passes over one body of code. `bbv/predict.rs` computes **one fact context
  per program point**, to a fixpoint; emission then *consults* that prediction
  and emits code enforcing it, shunting any execution that would diverge onto
  the generic track. Reducible control flow comes by construction (section 4).
  It is the only codegen path.

The compiler has no design switches. `compiler/src/options.rs` carries exactly
two things: a leave-everything-interpreted triage switch, and a set of
diagnostics that change no generated code. The crate reads no environment
variables at all outside its own unit tests. There is one lowering strategy and
one analysis and they are not configurable. Inlining in particular has no
knob: its admission caps are constants in `wasm/bbv/` (section 5.7).
---

## 2. The soundness model

This is the thing to check first, so it comes first.

### 2.1 The contract

> **Nothing the analysis produces is trusted. Every speculative fact is
> discharged at runtime by a guard, or is a sound static proof. A wrong
> prediction costs a failed guard or a generic helper call — never a
> miscompile.**

Three corollaries follow, and all three are enforced structurally rather than
by review:

1. **A script is compiled iff every reachable op in it is translatable.** There
   is no partial compilation. `Outcome` has exactly two variants: `Compiled` and
   `Skipped(reason)`. A skipped script keeps its AOT function index at 0 and
   runs interpreted — a performance loss and nothing else.
2. **There is no deoptimisation and no bailout.** A failed guard is not an exit
   from compiled code; it is a branch to a *different version of the same
   successor pc*, compiled under weaker facts. There is no frame
   reconstruction, no side-exit state map, and nothing to get wrong about them.
   The cost of a miss is code duplication at compile time and a taken branch at
   run time.
3. **A "fact" in the codegen's context is a proof, never a prediction.** The
   analysis's predictions enter the codegen only as *arm ordering* and *guard
   selection*. A guard that passes is what installs the corresponding fact into
   the context. Manufacturing a type from a prediction is the one known
   miscompile vector in this design, and the discipline against it is absolute.

Where correctness rests on something other than a guard, it rests on a stated
invariant with a mechanical basis. There are three such invariants, and each is
called out where it applies:

- **STAMP-IMPLIES-PLAIN-DATA** (section 2.2): a stamped object's guarded prefix
  slots hold plain data properties, so a passing stamp test is itself the proof
  that no accessor, proxy handler, or exotic lookup can be reached.
- **The interval algebra is a proof, not a prediction** (section 7): an interval
  attached to a value traces to canonically-boxed producers, and is the basis
  for eliding overflow checks.
- **The splice's exception rule** (section 5.7): a splice site is refused inside
  any non-loop try-note range of the caller, and a splice target is refused if
  it has non-loop try notes of its own, so "throw out of a splice" reduces
  exactly to "throw out of the whole compiled body".

### 2.2 The object stamp

The stamp is the load-bearing runtime invariant of the whole system. It is what
lets a property read cost one `i64.load`.

**Where it lives.** A single 32-bit word in the `JSObject` header, at byte
offset 4 on `wasm32` — the alignment padding SpiderMonkey already reserved
between the shape pointer and `slots_`:

```
wasm32 JSObject:   shape @0 | STAMP @4 | slots_ @8 | elements_ @12 | fixed slots @16...
```

Two consequences worth stating plainly. The mechanism is **32-bit only by
construction**: on a 64-bit build the padding word does not exist and every
maintenance call site compiles away. And the word is **zeroed by
`JSObject::initShape`**, so "unstamped" is the default state of the entire
heap and 0 can never be mistaken for a class.

**Layout.**

```
 bit  31        30      29 .......... 18   17     16    15 .............. 0
     +--------+--------+-----------------+------+------+------------------+
     |CONSTR- | RANGES |   early key     |SLOTS |TYPES |  layout key + 1  |
     | UCTING |        | (while CONSTR.) |      |      |  0 = unstamped   |
     +--------+--------+-----------------+------+------+------------------+
```

The low half is the **identity**: `layout_key + 1`, where the key numbers a
class the analysis discovered. The `+1` keeps 0 meaning "unstamped". Keys are
assigned **region-contiguously** by the analysis so that a whole family of
related classes is a single unsigned range compare in the emitted code.

The high half carries three independent **validity bits** plus a construction
sentinel. While the sentinel is set the object is mid-construction, its identity
is not yet published, and bits 18..29 hold an *early key* naming the class it
is being built as. No identity-guarded arm can hit such an object, because the
low half reads 0.

RANGES sits at bit 30, at the top of the early-key region rather than beside
SLOTS, so that the engine's unconditional clear of that bit can never corrupt a
key mid-construction. The key gave up its thirteenth bit for it.

### 2.3 What each stamp bit licenses

| bit | claims | set by | cleared by | licenses |
|---|---|---|---|---|
| **identity** (low 16) | this object is an instance of layout key *k* | the constructor-exit stamp; an init delegate's restamp (prefix key to full key); the allocation site's early-key seed while constructing | the wholesale clear (below) | every class-fact arm's key compare |
| **TYPES** | every *masked* field of this layout holds a value of its mask | seeded at allocation; carried forward by the exit stamp | the engine's store check, on any non-number value store; the wholesale clear | skipping the value tag test on a typed field load |
| **SLOTS** | the static slot predictions are valid for this object | seeded at allocation, **only from a keyed stamp** — a keyless sentinel never seeds it, because adds are uncheckable without a layout | the add check, when a property lands at an offset that contradicts the prediction; the wholesale clear | the baked `FIXED_SLOTS_BASE + 8*slot` immediate in every checkless load and store |
| **RANGES** | the predicted value *ranges* of this layout's masked fields hold | seeded at allocation; carried forward by the exit stamp | **unconditionally** by the engine's store check, number or not; by a compiled store whose value is outside the claimed window; the wholesale clear | a loaded value arriving with a proven interval, which then elides downstream overflow checks |

The engine-wide claim TYPES makes is **numberness and nothing finer**. The
per-field mask survives only because every consumer re-checks it at the load
through a number-tag dispatch. RANGES cannot ride that discipline, because it is
consumed *checklessly* — nothing re-derives a magnitude at the load — which is
exactly why it needs a bit of its own that every unchecked write drops.

Note the asymmetry between TYPES and SLOTS: **a value store can never clear
SLOTS**, because a value store cannot move a slot. That is what makes the
checkless load arm (section 5.2, L1a) safe once SLOTS has been proven in a
lineage.

#### Who clears

There are three mechanisms, at three granularities.

**1. The store check** — `nightStoreCheckOrClear`, reached from
`NativeObject::checkStoredValue`, the funnel for all thirteen engine-side slot
store flavours (and, conservatively, for dense element stores too). It
unconditionally drops RANGES, then drops TYPES if the stored value is not a
number. It is a test-then-write: an unstamped receiver, the common case, costs
one load and never dirties the header cache line.

Compiled code emits its own twin of this check inline (`emit_store_choke`,
section 5.3), specialised by what the site statically knows.

**2. The add check** — `NightAddPropCheck`, called from both branches of
`NativeObject::addProperty`, from the slot-adding path, and from the compiled
add-transition replay. It fast-outs when SLOTS is already clear (one load on
every unstamped receiver), resolves the layout from the early key or the
identity, fast-outs when the new slot is past the guarded prefix bound, and
otherwise clears SLOTS unless the assigned slot exactly matches a prediction —
either this layout's, or that of a layout which strictly *extends* it and
predicts this atom at this slot, which is what lets a two-phase constructor
fill its prefix and then its suffix without losing the bit.

**3. The wholesale clear** — `clearNightLikelyClass()`, which zeroes the word
outright. Six engine call sites, all of them structural departures from
plain-data-ness:

| path | why |
|---|---|
| `NativeObject::toDictionaryMode` | slot assignments are no longer positional |
| `NativeObject::changeProperty` | includes data becoming an accessor |
| `NativeObject::changeCustomDataPropAttributes` | custom data property |
| `NativeObject::removeProperty` | `delete` |
| `NativeObject::freezeOrSealProperties` | attributes change |
| `JSObject::swap` | swapped guts may no longer conform |

#### The STAMP-IMPLIES-PLAIN-DATA invariant

There is deliberately **no refusal allowlist** in the runtime. The invariant is
maintained structurally, by four facts that together mean a stamped object's
guarded prefix cannot hold anything but plain data:

1. **Birth.** The only paths that write a nonzero stamp are the AOT allocation
   helper, which ends in a plain-object allocation, and `create_this` for
   scripted non-derived constructors, which is also plain. Proxies, typed
   arrays and every exotic class are unreachable from both.
2. **Default.** `initShape` zeroes the word on every other allocation in the
   engine.
3. **Departure clears.** Every transition away from plain-data-ness — dictionary
   mode, data-to-accessor, delete, freeze, seal, guts swap — clears the word
   wholesale.
4. **Adds are checked.** An accessor or otherwise unpredicted property landing
   *inside* the guarded prefix clears SLOTS.

A getter or setter therefore cannot be reached by any stamp-guarded arm, and the
generic path is the only path that can reach an accessor. Two soft spots are
worth stating rather than leaving implicit: an accessor added *past* the
extension bound keeps both bits (which is fine, because the claim only ever
covers prefix slots — but that is a reasoning step, not a check); and
**prototype mutation is not hooked at all**, which is sound only because every
stamp claim is about *own* fixed slots.

#### How a stamp compare discharges a fact

One load, one mask, one compare:

```wasm
i32.load  $obj offset=4
i32.const 0xFFFF | TYPES | SLOTS      ;; the bits this consumer needs
i32.and
i32.const k      | TYPES | SLOTS
i32.eq
```

A single compare proves both "the identity is *k*" and "the required validity
bits are live". Variants pick exactly the bit set the consumer needs; a
range-valued identity fact becomes an unsigned-subtract range test instead of an
equality; and a consumer that needs only the flags loads the 16-bit half at
offset 6 so it can test them without touching the identity.

That is the whole discharge mechanism. `refine_src` then records the proven
facts against the receiver's frame slot, so the next access on the same value in
the same lineage needs no test at all.

#### The compiled-side validator

The compiler does not trust the analysis's slot predictions either. Before a
constructor's exit stamp publishes an identity, emitted code loads the shape's
small slot-span field and requires it to be at least the predicted row length.
The soundness argument is that the predicted prefix is a name-to-slot bijection
and the engine assigns added slots sequentially, so a surviving SLOTS bit plus a
count of at least *N* implies the first *N* slots hold the predicted names.

The add-check bound is a static table, stored per layout.

### 2.4 Non-stamp discharge mechanisms

Not everything is a stamp. The other discharge mechanisms, each with what makes
it sound:

| mechanism | discharges | soundness basis |
|---|---|---|
| tag tests | every primitive-type claim | direct test of the boxed representation |
| shape compares (inline caches) | a property's location on this exact shape | a shape is an immutable descriptor; the cache regions are zeroed on major GC, and cached pointers are tenured-only |
| callee identity compares (call cells, builtin cells, native pointers) | "this call site's callee is *X*" | value identity is object identity; cells cache tenured values only and are GC-zeroed; a monkeypatched builtin is a different value and self-misses |
| value fuses | "this global binding still holds the value we baked" | the fuse word *is* the guard; it is blown by any write through the engine, and is distrusted wholesale whenever interpreted code can write globals |
| the interval algebra | integer overflow, `-0`, and tag claims | a sound abstract-interpretation proof over exactly-representable integers (section 7) |
| the effect-provenance return word | "the callee did not disturb my facts" | the callee computes it; the caller tests it (section 5.5) |
| the clean-miss bit | "this cache miss ran no user code and moved nothing" | the runtime helper returns it, and returns it only from arms that provably qualify |

### 2.5 Failure modes, ranked

The design's whole cost structure is this ladder, cheapest first:

| failure | cost |
|---|---|
| a check the interval algebra elided | *does not exist* — no code is emitted |
| an overflow or `-0` exit | a convert and a reinterpret, then an edge to the `Side` version of the same pc. No spill, no call |
| a type-test arm miss | a few tag tests, then an edge. No spill, no call |
| an inline cache miss served cleanly | a spill, a helper call, a reload — but the lineage **rejoins its happy path**, facts intact |
| an inline cache miss that could have run user code | the same, and the lineage goes to the `Dirty` track: every class fact in the frame is killed and every GC-pointer carrier is swept |
| a generic helper arm | spill, call, reload, `Dirty` |
| a script the compiler declined | interpreted, at interpreter speed |

Nothing below that line exists. There is no state in which a wrong prediction
produces a wrong value.
---

## 3. Repository map

Everything lives under `js/src/night/`.

| Path | Contents |
|---|---|
| `compiler/` | The Rust compiler crate, `night-compiler`. |
| `compiler/night-compiler.h` | The C ABI between SpiderMonkey and the crate. |
| `compiler/src/lib.rs` | FFI entry points (`night_inproc_build` and friends). |
| `compiler/src/options.rs` | `Options` / `Diagnostics`: the entire configuration surface. |
| `compiler/src/source.rs`, `src/source/ffi.rs` | The `Source` object graph — the sole input to the compiler. |
| `compiler/src/bytecode.rs` | Bytecode parser and `OpcodeVisitor`; `JSOp` is generated from `Opcodes.h` by `build.rs`. |
| `compiler/src/opsem.rs` | The shared op-semantics vocabulary: primitive-bit alphabet, result-type algebra, interval algebra. |
| `compiler/src/likelier/` | The speculative whole-bundle analysis (`scan`/`heap`/`calls`/`engine`/`types`/`emit`/`dump`). |
| `compiler/src/facts.rs` | `LikelyFacts`: the analysis-to-codegen contract. |
| `compiler/src/wasm/bbv.rs` | The workqueue-BBV codegen: versioning, all op lowerings, splicing, LICM. ~27k lines. |
| `compiler/src/wasm/bbv/predict.rs` | The prediction: one fact context per program point, and the fixpoint that computes it. The only fact store emitted code reads. |
| `compiler/src/wasm/translate.rs` | Shared substrate: `Helpers`, `AtomTable`, `Outcome`, layout constants, scan utilities, the entry shim into `bbv.rs`. |
| `compiler/src/wasm/effects.rs` | The effect taxonomy (`EffectClass`, `HeapKind`) used by call handling and LICM. |
| `compiler/src/wasm/regex.rs` | The regex AOT compiler (irregexp bytecode to wasm matchers). |
| `compiler/src/wasm/mod.rs` | `layout_env` / `translate_all`: analysis prepass, reserved memory-region layout, per-body translation, table and address patching. |
| `compiler/src/wasm/inprocess.rs` | In-process batch builder: function blobs plus the environment descriptor for the runner host calls. |
| `runtime/` | The C++ night runtime: the `night_runtime_*` helper ABI shim (`NightRuntime.cpp`) over the engine halves -- bytecode ops (`NightOps.cpp`), property caches (`NightInlineCaches.cpp`), inline allocation and barriers (`NightInlineHeap.cpp`), generators (`NightGenerator.cpp`), regexes (`NightRegExp.cpp`) -- plus entry into compiled bodies (`NightEntry.cpp`), the value stack, and snapshot registration and activation. Linked into the shell. |
| `snapshot/` | The snapshot / live-heap reader crate: parses the registration block and walks the script graph into a `Source`. |
| `snapshot-dump/` | A standalone wasmtime host tool that dumps a snapshot's `Source` graph, as a cross-check. |
| `nightmonkey/` | The `nightmonkey` host binary: wizer snapshot in, AOT-compiled snapshot out. |
| `wasm-jit-runner/` | Wasmtime-based test host exposing the function-injection host calls. |
| `tools/` | Diagnostic post-processors for the compiler's dump modes (`viz.py`, `opprof.py`, ...). |

The waffle backend is an ordinary crates.io dependency (`waffle = "0.3.1"`), vendored at `third_party/rust/waffle`. It is not NightMonkey code, but two of its passes are load-bearing here and are described in [section 4.9](#49-reducibility-by-construction).
---

## 4. The code-emission strategy: workqueue BBV

`compiler/src/wasm/bbv.rs`. This is the only codegen path. The design is
basic-block versioning in the Chevalier-Boisvert sense, driven off a
workqueue rather than a plan: the compiler processes one *version* at a time,
emits exactly one bytecode op for it, and routes every successor edge through
a single merge point.

There are no ahead-of-time plans, no admission passes, no deoptimisation
landings, and no bailout mechanism. A failed speculation is an ordinary
control-flow edge to the generic version of the same successor pc.

**One distinction runs through all of section 4 and is worth fixing before
reading it.** "Version" has historically meant two things; they are separate
objects here:

- a **block** — `Ver { pc, class, track, depth }`, structural identity. The
  token class and depth exist to keep every cycle single-entry (4.9), so two
  blocks may be duplicates of one another for reducibility's sake alone;
- a **prediction** — the fact context, keyed by the program point and nothing
  else. One per pc on the Opt track; the generic track carries no facts.

Codegen never mints or moves a prediction. It reads one and enforces it.

### 4.1 What a version is

```rust
// bbv/version.rs
struct Ver {
    pc: u32,        // unified pc space: root bytecode + synthetic inline segments
    class: u32,     // interned token-vector id: the per-loop layer markers
    track: Track,   // Opt | Side | Dirty
    depth: u16,     // operand-stack depth at entry
}
struct VerId(u32);  // bbv/version.rs
```

**The version identity does not mention the abstract context.** That is the
central design decision, and it is worth dwelling on, because the natural
formulation of BBV — a version *is* a `(pc, ctx)` pair — is what this replaced.

Under `(pc, ctx)` identity, the context that a version carries is "whichever
lineage got there first", and every merge policy question ("may this edge join
that version? how many versions may a pc have? when do we widen?") becomes a
heuristic in the merge function. Under structural identity there is no merge
policy at all, because the two things have been separated:

- `Ver` names a **block**. `class` and `depth` are there to keep every cycle
  single-entry ([section 4.9](#49-reducibility-by-construction)); two `Ver`s
  may be duplicates of one another for that reason alone.
- The **fact context** is keyed by the program point and by nothing else —
  one prediction per `pc` on `Opt`, and none on `Dirty`, which carries no
  facts ([section 4.2](#42-the-context)).

Three consequences fall out:

- token vectors naming a version stay stable across prediction rounds, so
  version identity does not move under the analysis;
- **version count is bounded by construction** at two tracks per
  `(pc, token class, depth)`, which retires every minting budget; and
- **every `Opt` block at a pc emits the same body**, because it reads the
  same prediction. That is the test of the split: on an ISA that admitted
  irreducible control flow the duplicate blocks would simply disappear, and
  no dynamic path's code would change.

`pc` is capped at 24 bits (`MAX_PC`, `bbv/mod.rs`); scripts over 128 KiB of
bytecode are not compiled at all, and inline splice segments allocate synthetic
pcs above the root script's bytecode in the same space (section 5.7).

`depth` is normally a function of `pc`, since bytecode stack depth is
deterministic, and costs nothing. It is in the identity because an
**exception landing** enters a pc at the try-note's unwound depth — a second
legitimate depth for one pc (`bbv/version.rs`). It is *only* in the identity:
it keys no prediction, because a landing has no facts to misalign — every
landing edge goes to `Dirty` (`cont_stripped`), which is where a path that is
off every happy path by definition belongs, and also keeps a rare exceptional
state from pessimising the normal path it lands beside.

### 4.2 The context

```rust
// bbv/ctx.rs
struct Ctx {
    locals: Vec<SlotCtx>,
    stack:  Vec<SlotCtx>,
    args:   Vec<SlotCtx>,          // [0] = this, [1+i] = formal i
    caller_locals: Vec<SlotCtx>,   // the caller's frame facts across an inline splice
    caller_args:   Vec<SlotCtx>,
    tokens: Vec<(u32, u64)>,       // (loop index, layer marker), sorted
    carried: Vec<u32>,             // which locals arrive as block params
    track: Track,
}
```

The context is a *split* object: `tokens`, `carried` and `track` are identity
or location dimensions, not facts. `tokens` and `track` are duplicated into
`Ver`; `carried` is pure location and is excluded from the implication
relation entirely.

**Only the fact half is stored, and it is stored once per program point.**
`bbv/predict.rs` holds `Predictions`, whose whole content is:

| keyed by | holds |
|---|---|
| `pc` | the fact context on the `Opt` track (`Ctx::facts_only`) |
| `pc` | the operand depth that prediction was minted at |
| `(pc, track)` | `carried`: which locals arrive as block params |

Nothing else keys a prediction. Token class and depth name a *block*, not a
prediction. `Dirty` has no entry at all: **GEN carries no facts** — every
value is boxed at every op boundary there, and after out-of-lining
([section 5](#5-lowerings)) every GEN op body is a generic helper call that
would be handed a boxed operand regardless, so a fact there reaches no
lowering decision. `Ctx::facts_free` is its definition.

A `Ver`'s emission context is therefore *derived*, never stored: the pc's
prediction, plus that identity's own tokens, track and carried set.

`Predictions` is split down the middle on purpose. The **consult** half
(`at`, `carried`) is what codegen may use: it reads a prediction and emits
code that *enforces* it, shunting any execution that would diverge to GEN. It
never mints a fact and never moves one, and `theta` asserts it. The
**prediction** half (`join_arrival`, `set`, `widen_all`) belongs to the pass
that computes the fixpoint.

Trailing all-TOP slots are trimmed by `Ctx::canon` (`bbv/ctx.rs`) and a slot
beyond the vector reads TOP, so contexts have a canonical form.

The per-slot fact:

```rust
// bbv/ctx.rs
struct SlotCtx {
    prim_mask: u16,         // PRIM_INT32 | PRIM_DOUBLE | PRIM_STRING | ...
    outside: bool,          // may be an object
    range: RangeBucket,     // I32 < I53 < Top
    cls: Option<(u16,u16)>, // a proven stamped-class-key range
    cls_shallow: bool,      // the stamp's TYPES bit proven set
    cls_slots: bool,        // the stamp's SLOTS bit proven set
    likely_cls: Option<(u16,u16)>, // ADVISORY class hint; invisible to `implies`
    src: Option<SlotRef>,   // provenance: which frame slot this value IS
    iv: Option<(i64,i64)>,  // a proven exact-integer interval
    iv_grow: u8,            // fixpoint metadata: how often the interval widened
    prov: Prov,             // census provenance; excluded from Eq/Hash
}
```

Four fields deserve comment.

**`likely_cls` is advisory and never proven.** It is the analysis's per-site
value class for the load that produced the value ("likely this class,
unchecked"). No consumer may trust it without emitting its own guard, which is
what promotes it to a proven `cls` on the hit arm. Because every use re-guards,
it needs no kill discipline — it survives a may-GC call and a restamp, where
`cls` dies — and it is invisible to `implies` and to the proof, because an
advisory fact owes no guard.

**`prov` is census metadata, not a fact.** Its `PartialEq` is always true and
its `Hash` writes nothing, so it rides `SlotCtx`'s derived impls without
entering any comparison.

**`src` is a location, not a claim.** In per-op BBV every operand a guard sees
arrived as a block parameter, so a guard that proves something about a value
has nowhere to write the proof back to unless it knows which frame slot the
value *is*. `src` is that pointer. It is excluded from `implies`, survives a
join only when both arrivals agree, and is consumed by `refine_src`
(`bbv/frame.rs`), which writes a guard-proven class fact back into the source
slot's context cell so later ops in the same lineage need no re-test.

`refine_src` writes back **class facts only**. Numeric write-back was tried
and loses on most benchmarks, because re-typing a context slot hands out a
different carrier representation and pays a conversion on every subsequent
edge. The practical
consequence is worth stating plainly, because the surrounding code reads as if
it were otherwise: **a passed tag guard on a local does not make the next
arithmetic op on that local checkless.** Only class identity is durable across
ops in a lineage.

**`iv_grow` is fixpoint metadata**, the widening trigger, and carries no claim.

The implication relation (`SlotCtx::implies`, `bbv/ctx.rs`) is pointwise:
mask subset, objectness, range chain, class-key-range containment, and the two
stamp bits monotone, plus interval containment. `Ctx::implies` (`bbv/ctx.rs`)
is pointwise over all five slot vectors **with token vectors and track required
equal** — a token is an identity, not a fact.

`SlotCtx::join` (`bbv/ctx.rs`) is the least upper bound: masks union, class
facts survive only if both sides claim, `src` survives only on agreement, and
the interval goes through `opsem::iv_join_tolerant`, which keeps the exact
union for the first three growths per slot and then snaps up a widening ladder
(section 7).

### 4.3 The version table and the interning discipline

```rust
// bbv/version.rs
struct VerTable {
    ids:  HashMap<Ver, VerId>,
    vers: Vec<Ver>,             // structural identity: a block
    ctx:  Vec<Option<Ctx>>,     // parallel, DERIVED: pred[pc] + this block's tokens/track/carried
    pred: Predictions,          // THE fact store, keyed by program point (bbv/predict.rs)
    at_pc: HashMap<Pc, Vec<VerId>>,   // a prediction's dependents: the worklist's edge set
}
```

`VerTable::intern` is the only dedup point. The waffle side is three parallel
maps on the emitter: `blocks: HashMap<VerId, Block>`,
`block_params: HashMap<VerId, Vec<Value>>`, and the workqueue itself.

Contexts are **never hashed**. The compile-time budget for a large program
does not survive hashing free-form contexts on every edge, so the only thing
interned is the *token vector*:

```rust
// bbv/version.rs
fn tok_class(&mut self, toks: &[(u32, u64)]) -> u32
```

backed by `tok_classes: HashMap<Vec<(u32,u64)>, u32>`, which **persists across
prediction rounds** — a class id is part of a version identity, so it must name
the same thing in round N+1 that it named in round N. Both the version table
and the token-class table are moved forward between rounds rather than rebuilt.

**The splice set persists for the same reason, and it is the sharper case.** A
spliced callee body owns a range of synthetic pc space, and the prediction is
keyed by pc — so a later walk that renumbered the segments would silently
re-point every prediction past the first splice. The splice set is therefore
decided on the first walk and then **frozen** (`Splices`,
`Bbv::adopt_splices`, `bbv/mod.rs`): later walks may only look segments up,
and a site that finds none lowers generically.

Frozen means *decided*. The admission questions -- the per-script site cap,
the args-object bar, the depth cap, the target rejects, the handler ranges --
are answered on the walk that created the segments and must not be asked
again, because the freeze hands the next walk their budgets already spent.
Re-asking them declines a splice walk 1 made, and the artifact then emits real
calls through a pc range the prediction still describes as spliced. That is
not a size decision, it is the two halves of the compiler disagreeing about
the program. The splice **fuel** is the one
gate that is not admission -- it asks whether this body has grown enough to
stop expanding it, which is a property of the walk doing the emitting -- so it
alone is re-asked, and both lowerings are correct at a spliced site.

Each segment's loop intervals
are re-appended in segment order, so a loop's index — and hence every interned
token class — names the same loop in every walk. Attempting instead to gate
splices *during* emission corrupts exactly this numbering, reproducing as a
crash once splice admission depends on a changed walk.

`Ctx` derives `Hash`, but nothing uses it; that derive is vestigial.

### 4.4 The driver

`translate_script` (`bbv/mod.rs`) is the outer harness. Up-front refusals:
bytecode over 128 KiB, generator/async bodies that use `arguments`, and
unsupported environment ops (section 11). Then, per compile-ladder rung (section 4.12), a **two-phase
build**:

```
predict::run(...)                       // phase 1: the prediction (bbv/predict.rs)
t = Bbv::new(...);                      // phase 2: mode = Code, CONSULT-ONLY
t.vers = vers;
outcome = t.emit();
if t.map_changed { retry the closure, or descend the ladder }
```

and inside `predict::run`:

```
loop {
    t = Bbv::new(...); t.mode = ContextOnly;
    t.vers = vers; t.tok_classes = tok_classes; t.adopt_splices(splices);
    t.emit()?;                          // walks the program, appends NO IR
    vers = take(t.vers); ...; splices = t.take_splices();
    if !t.map_changed { break }
    rounds += 1;
    if rounds >= cap { vers.strip_all(); stripping = true }   // loud safety net
}
```

The **same `emit_op` body serves both modes** (`EmitMode`, `bbv/ops.rs`). In
`ContextOnly` the IR primitives append nothing, but a virtual value counter is
bumped so that every size-sensitive decision (splice admission, the body-size
cap) sees the same number in both modes and takes the same control decisions.
That counter is charged **per identity**, so re-running a block replaces its
contribution rather than adding a second copy. This is the maintainability
property the whole design rests on: the abstract transfer function and the
lowering cannot drift apart, because they are the same code.

**Inside one walk the fixpoint is a worklist over program points, not a
sequence of whole-program re-walks.** When `theta` moves the prediction at a
pc it re-arms exactly the blocks at that pc (`rearm_pc`), refreshing their
derived contexts as it does; those blocks re-run and propagate. There is no
dependency graph beyond that. The outer rounds remain only because a walk can
discover blocks the previous one never reached, and because the splice set is
frozen only after the first. Getting this wrong is expensive rather than
merely slow: before the worklist, keying the prediction by pc took raytrace
from ~10 rounds to 438 and regexp from 1.6 s to 132 s of compile time,
because a lattice step cost a whole-program walk.

**Why a fixpoint first, rather than emitting and regenerating?** Because the
moment a back edge weakens a header's context, the preheader's already-emitted
edge targets the wrong version, and repairing that is not a retarget: the
context transition may need conversion code the edge never had, which cascades
to *its* predecessors. That cascade is a fixpoint, and it would be paid in
re-emission rather than in a cheap analysis walk.

`emit()` (`bbv/version.rs`) sets up the frame and then drains the queue:

```rust
while let Some(key) = self.workqueue.pop() {
    if !self.processed.insert(key) { continue }
    if self.value_count() > MAX_BODY_VALUES { return Ok(()) }   // early abort
    self.run_version(key)?;
}
```

The frame prologue lives in a synthetic entry block **outside the version
table**, so a branch back to pc 0 can never re-run it.

`run_version` (`bbv/version.rs`) emits exactly one op:

1. restore the emission point, the frame view (for splice segments), the
   tokens and track, the four fact vectors, and the carrier caches;
2. rebuild the operand stack from the block's parameters, re-attaching each
   slot's class facts, provenance and interval — dropping those made every
   stack operand's proven class die at the next op, which in per-op BBV is
   immediately;
3. decode one op at `pc` and lower it;
4. if the lowering did not set a terminator, add the fall-through edge through
   `cont(next_pc)`.

So **every op ends its block.** One waffle block per version, plus whatever
intra-lowering blocks the op's own diamonds needed.

There is no scheduling pass. Blocks land in the waffle arena in creation
order, which is edge-discovery order; waffle recomputes RPO and the dominator
tree and emits in its own order (section 4.9).

### 4.5 The continuation contract

Every successor edge any lowering produces goes through one seam:

```rust
// bbv/version.rs
fn cont(&mut self, succ_pc: u32) -> BlockTarget {
    let normal = self.cont_normal(succ_pc);        // computed FIRST, unconditionally
    if let Some(t) = self.try_onramp(succ_pc, &normal) { return t; }
    normal
}
```

The ordering is load-bearing: the normal continuation is the on-ramp's own
bail target, so the merge-point call sequence and every context join are
identical whether or not the on-ramp fires. The failure arm mints nothing.

`cont_normal` (`bbv/version.rs`) builds the arrival context — the live fact
vectors, the operand stack projected slot-wise, the outgoing token vector, and
the carrier sets — canonicalises it, hands it to `theta`, and then calls
`cont_at` to materialise the edge.

`cont_at` (`bbv/version.rs`) does the mechanical half:

1. ensure the target version has a block (`ensure_version_block`);
2. build the argument vector in the exact parameter layout
   `[ stack(depth) | carried locals | carried args | flags? ]`, converting each
   operand to the target slot's representation via `convert_to_repr`;
3. in `Code` mode, assert that the argument count matches the target's
   parameter count. Only `Code` asserts: the carried half comes from the
   version context, which is frozen by the time `Code` runs but still moving
   during the fixpoint rounds.

`ensure_version_block` (`bbv/version.rs`) is where a continuation becomes a *new*
version: if the identity has no block yet, create one, type its parameters from
the version context, and push it on the workqueue. Otherwise reuse the existing
block and just supply edge arguments. So all tails are shared, and emitted code
is bounded by `O(|ops| x |versions per pc|)`.

Two wrappers exist for readability: `edge_to` is `cont`, and `dirty_edge_to`
is also `cont` (the track already carries the dirt). `cont_stripped`
(`bbv/version.rs`) is the exception/finally landing form: it flushes deferred
state, boxes the whole operand stack, zeroes every fact vector, forces the
`Dirty` track, and then routes through the same seam — which is what keeps a
landing block's parameter layout in step with `run_version`.

The invariants the contract enforces:

1. every edge's arguments match the target's parameters in count and type;
2. every arrival's context **implies** the target version's context, so each
   representation conversion on the edge is licensed;
3. stack depth agrees, because it is in the identity;
4. a version's block is created at most once.

### 4.6 theta: the single merge point

```rust
// bbv/version.rs
fn theta(&mut self, pc: u32, ctx_in: Ctx) -> VerId
```

`theta` is the only place a merge decision is made, and **it holds no policy.**
That is a change from the original design, which had three attractors and a
per-pc budget; section 4.7 explains what replaced them and why. What theta does
now, in order:

1. **Ladder collapse.** On the `gen_only` rung, `ctx_in.gen_collapsed()` drops
   every identity dimension but keeps the facts — one version per pc that still
   hands out unboxed block parameters where all arrivals agree. Under
   `stripping`, `ctx_in.stripped()` drops facts and carriers too.
2. **Peel fold.** A `Side`-track arrival at a pc inside a loop structure is
   folded to `Dirty`: one peel per loop, not per track.
3. **Facts-free GEN.** A `Dirty` arrival is reduced to `ctx_in.facts_free()`.
4. **Identity interning.** Intern the token vector to a class id, then intern
   `Ver { pc, class, track, depth }`.
5. **The prediction step, in the prediction pass only:**

```rust
if track == Opt && mode == ContextOnly {
    if vers.pred.join_arrival(pc, depth, ctx_in.facts_only()) {   // pc-keyed!
        map_changed = true;
        self.rearm_pc(pc);                                       // the worklist
    }
}
```

`join_arrival` is the same lattice step as before — discovery, or
`join_iv_only` for an arrival that disagrees only on intervals, or the full
join — but keyed by the **program point** rather than by the block identity.

6. **The derived context.** The identity's `ctx` becomes `pred[pc]` plus its
   own tokens, track and carried set (or, on `Dirty`, no facts at all).

In `Code` mode step 5 does not run at all: `theta` is a pure lookup, and a
`debug_assert` checks the contract the edge conversions rest on — that the
arrival implies its program point's prediction. Reaching a block the
prediction pass never saw is likewise a debug assertion failure; in production
both are caught by the closure check described below.

The `implies_sans_iv` special case exists because an arrival that disagrees
*only* on the interval should weaken the interval in place rather than run a
full join — a full join would drop disagreeing `src` provenance, and thereby
change the code emitted, on account of a dimension no consumer had asked about.

**Termination** rests on three independent arguments:

- *The lattice is finite and joins only descend.* Masks are 16 bits, the range
  bucket is a 3-chain, class ranges only widen, the stamp bits only clear,
  `src` only goes to `None`. Intervals are the one unbounded-looking component
  and are bounded by the widening ladder keyed on `iv_grow`.
- *Identities are finite.* `Ver` ranges over `pc x class x 3 tracks x depth`,
  and a token vector has one entry per enclosing loop drawn from three markers.
- *There is a hard escape.* If the fixpoint exceeds `max(48, versions/4)`
  rounds, `strip_all()` widens every context to its stripped form. That is
  guaranteed to close: with every context stripped, a version's context is a
  function of its own identity, so no arrival can move one and the next round
  walks exactly the same versions. It prints a warning unconditionally, because
  convergence is guaranteed and any firing is a bug to chase — the strip is a
  safety net, never policy.

The **closure check** after the `Code` pass is the containment property: if
emission discovered a version the fixpoint had not, the emitted body may have
converted an edge to a representation the value was never proven to have, so
the body is *not* emitted. It retries the fixpoint up to three times and
otherwise descends the compile ladder, whose bottom rung is facts-empty and
where the check cannot apply.

### 4.7 Tracks: what replaced the OPT/OVF/GEN attractors

The original design had three per-pc attractors — OPT (refining joins only,
per-pc budget K), OVF (numeric-degraded, absorbing), GEN (bottom). They are
gone: the code does not implement them, and there is no per-pc budget
anywhere in the source.

What replaced them is a **structural dimension** in the version identity:

```rust
// bbv/ctx.rs
enum Track { Opt, Side, Dirty }
impl Track { fn step(self, to: Track) -> Track { self.max(to) } }
```

`Opt` means exactly one thing: the execution **conforms to the prediction at
every program point it has passed**. `Dirty` is where a non-conforming
execution is shunted, and it carries no facts.

**A call does not step the track.** A may-GC call kills the class facts, and
one might expect the call-crossed lineage to be kept separate so those weak
facts cannot join into the strong pre-call version (the join law). With the
fact context keyed by the program point there is no version to poison — a pc
has one prediction, which is the join of its own arrivals and nothing else —
so that separation buys nothing, and what stands in its place is the pressure
valve the design rests on: a value the analysis cannot pin after a call is
predicted weakly and stays generic *on Opt*, rather than routing the whole
call-heavy tail of every body into fully generic code. Opt does not mean
fast; it means conforming.

**One contract follows from this, stated only in prose.** `dirty_edge_to` —
the continuation an IC-miss or slow-helper arm takes — steps the track
itself, scoped to the edge. It must: what such an arm delivers is a
*claim-free* result, and with one prediction per program point a single
untyped `Opt` arrival degrades every lineage through that point. On
navier-stokes that turns a 1-instruction `f64.mul` into a 410-instruction
ladder two bytecodes downstream of a slow element arm. This is the standing
rejoin rule applied literally: an arm that proves nothing is not conforming,
whether or not it contradicts anything.

**What not stepping the track on a call buys**: substantially cheaper code on
call-heavy benchmarks and OPT residency in the high nineties percent across
most of the suite. What remains against it is that **the
lowerings answer to the track rather than to the prediction** (see
`outline_generic`, `bbv/outline.rs`): a weakly predicted op on `Opt` gets the
full speculative bundle, 410 emitted IR instructions for that navier `Mul`
against 15 for the same op out-of-lined on GEN and 1 with its operands typed.
The pressure valve describes a generic-on-`Opt` shape the emitter does not
have, and no policy patch on the *track* supplies it — conditioning the step
on whether the call destroys a class fact the lineage holds recovers nothing.

- `Opt` — the happy path. An op's happy path needs no analysis to identify: it
  is the arm that *falls through*.
- `Side` — reached through any `side_arm`: a missed guard, an overflow exit, an
  inline slow arm.
- `Dirty` — GEN: a non-conforming execution, carrying no facts.

Steps only ever descend, so the track is a sticky property of how control got
here, and **two tracks never join**. That single fact is what the attractor
policy was trying to achieve. The failure mode it fixes is concrete: under a
first-arrival policy, `+`'s slow arm — which pushes a bottom type because `+`
may concatenate — could *define* the successor pc for the int lineage that fell
through it. On one benchmark kernel, 63 of 434 merge decisions were such
demotions and all 11 fully-generic arithmetic sites sat on OPT versions. Under
the track split that slow arm is on `Side` and cannot reach the `Opt` version
at all.

A `side_arm` (`bbv/arith.rs`) saves the entire lineage state (stack, all four
fact vectors, carrier caches, flags, track), steps the track down, emits the
arm, edges to the *same successor pc* under the arm's own weaker facts, then
restores. Every branch splits context; nothing merges back.

There is one further collapse: theta folds `Side` into `Dirty` wherever a loop
header exists to rejoin at, because `Side` is nearly zero dynamically and one
peel per loop is cheaper than one per track. Straight-line side lineages, which
have no header to rejoin at, keep their own copies.

`gen_only` — the bottom rung of the compile ladder — is the third thing the
old GEN attractor named: every version at every pc is facts-empty, giving back
exactly the generic lane's code. It is a compiler-capacity fallback, not a
runtime deoptimisation.

### 4.8 Loops: headers, back edges, side entries, on-ramps

Loop extents come from a purely syntactic scan (`translate.rs`
`scan_loop_intervals`): `LoopHead` pcs, and each negative-offset branch whose
target is a `LoopHead`. Callee loops are appended, base-offset, when a splice
segment is created.

Each context carries a **token vector**: one `(loop index, marker)` entry per
enclosing loop, with three markers (`bbv/mod.rs`):

| marker | meaning |
|---|---|
| `TOK_PEEL` | acyclic at this level — side entries, mid-body track steps, every distinct outer history, all funnelled into one class |
| `TOK_CYCLE` | steady dirty cycling; what a non-`Opt` header hands its body |
| `TOK_OPT` | the `Opt` cycle layer; what an `Opt` header hands its body |

`out_tokens_for` (`bbv/version.rs`) computes the outgoing vector. Three rules
matter:

- **The header-drop rule.** The containment test for loop `(h, e)` is
  `h + 1 <= pc < e`, i.e. *strictly interior*. An edge to the header itself is
  not "into the loop body", so the loop's own token is omitted from the vector.
  Entry edge and back edge therefore produce the same token vector, hence the
  same class, hence the same version. **This is what makes each cycle have
  exactly one entry.**
- A header hands its *own* side's marker to its body, so every body version
  carries a token naming which header it belongs to.
- A non-`Opt` lineage's marker collapses to `TOK_PEEL` unless it is the loop's
  dirty-cycle membership.

A **side entry** is an edge into a loop's strict interior from outside — it
takes `TOK_PEEL`, and the peel copy is acyclic by construction: its only way to
cycle is through a header, which re-marks it.

**Header versions are not pinned or special-cased.** A header pc gets one
version per (token class, track, depth) reaching it, like any other pc. The
loop stays typed if and only if its body preserves the header's context; that
fixpoint emerges from the driver rather than being proven ahead of time.

A loop header's *entry* and its *steady state* are one version. Splitting them
— peeling the first iteration so the entry context need not join the steady one
— was tried and loses overall: it helps the one benchmark it targets and costs
several others, because peeling every loop's entry costs a second copy of
every loop body.

#### On-ramps

An **on-ramp** is an edge-owned guard chain that lets a `Side` or `Dirty`
lineage re-enter the `Opt` version of a loop header by re-proving every fact
that header's prediction claims. This is the design's one load-bearing
asymmetry: the on-ramp edge **proves** the prediction instead of joining into
it.

**Extending it to `Opt` sources does not work.** With the fact context keyed
by the program point, an `Opt` edge whose arrival is weaker than its target's
prediction no longer lands in a version of its own — it degrades the one
prediction every lineage through that point reads, which is the join law with
nowhere left to hide. Having such an edge prove the prediction instead (same
target block, a guard chain in front, a bail to the pc's GEN version) is the
conformance-by-construction answer, and Opt residency collapses under it
because the guards fail. It is the same population lesson as the dirty
cycle's own back edge below — an edge that reaches the proof every iteration
pays every iteration, and a proof that can fail will.

`try_onramp` (`bbv/version.rs`) declines unless: the target pc is a loop header;
the current track is not already `Opt`; the source is either strictly inside
the loop (a *funnel*, requiring a `TOK_PEEL` label) or outside it (an *entry*);
every outer token label is `TOK_CYCLE`, since a `TOK_PEEL`-labelled tail would
rejoin the steady `Opt` header off its dominance region — the irreducible
shape; and **the header pc has an `Opt` prediction at all**.

That last condition is not "the target `Opt` version exists" -- the
difference is the whole of what the prediction split buys here. A proof
copy of a header and the steady `Opt` cycle are two blocks at one pc, so they
read *one* prediction: there is no per-copy context to seed, no rule for which
existing version to copy it from, and no "unseeded" case to decline. What is
left is the original question — is there anything to rejoin — asked directly.

**Header depth is not one of the conditions.** The proof target is keyed by
the arriving lineage's own outer labels with this loop's token dropped, so for
an inner header it is a *copy* of that loop rather than the steady `Opt`
version, and the copy cycles alone — reducibility by construction. The funnel
form does not additionally require the outermost level: that requirement
would be redundant, because every funnel it would turn away is already turned
away by the `TOK_CYCLE` rule above, which is where the reducibility argument
actually lives.

Feasibility is then checked over **every slot the target context claims** —
the whole stack, all locals, `this` and every formal — because a fact on a
frame-resident slot is trusted at its next read and therefore owes a guard even
when nothing is carried. `conform_gap` (`bbv/version.rs`) answers, per slot: not
re-provable / already implied / provable with a guard. There are three guard
kinds: a **tag test** on a boxed source, an **exact-i64** round-trip check, and
an **exact-f64** round-trip check.

The tag test proves membership in the target's whole admissible tag set --
one compare per tag, OR-ed, with the int32/double pair collapsed to the single
`is_number_tag` range test wherever both are present. Proving only one tag at
a time (plus the numeric pair as a special case), with `conform_gap` refusing
any target it could not express that way, would throw out ordinary unions
like `int32 | object`; those are exactly what a dirty loop body carries, so
refusing them would decline the recovery edge of box2d's hottest loop.
Several compares is the right price: the alternative to a successful on-ramp
is the
whole remainder of the loop off the Opt track, and the alternative to a failed
one is the dirty continuation that already existed.

`exact-f64` is `exact-i64`'s twin for an unboxed f64 carrier re-proving exact
int32 -- the widened-counter case. It is `f64.eq(x, convert_i32_s(trunc_sat(x)))`
**and** an explicit rejection of the `-0` bit pattern: unlike an integer
carrier, an f64 can hold a negative zero, which round-trips through 0 and
compares equal, and boxing it as int32 0 is a real change of value.

Two claims still refuse a proof outright, and both are about what the guard
chain cannot reach: an identity claim (`cls`/`cls_shallow`/`cls_slots`) on a
target that is not object-only, because phase 2's class-word read is licensed
only behind phase 1's object proof; and a `range` claim on anything but a bare
int32 target, because the range rides on that tag and nothing else.

**The caller's frame is a proof source, not a refusal.** A header inside a
spliced segment claims the caller's frame facts, because a splice carries them
into the segment ctx by construction. Refusing those claims outright, on the
reasoning that they are compile-time carried state the edge cannot re-prove,
would be a mistake — that reasoning is really an argument about facts the
*arrival* has, and an on-ramp's arrival never has any: GEN carries no facts,
so every
on-ramp source is factless by construction. That is the premise of the
mechanism, not an obstacle to it. The question is only whether the proof can
*test* for the claim, and it can: the frame is flat memory, the parent
segment's view addresses it, and `ProofSrc::CallerLocal` / `CallerArgSlot`
load the slot and guard it like any other. Soundness needs nothing new — a
splice cannot assign the caller's frame, so no write can intervene between the
guard and the use, and the kill discipline already sweeps `caller_locals_ctx`.
Measured on call-heavy code such as pdfjs, this recovers several points of
Opt residency at negligible size cost.

Two policy rules were each measured into place:

- **Entry-edge conforms must be free.** An edge coming from outside the loop
  declines if any slot owes a guard at all; only the funnel (a tail already
  inside the loop, where the guards are paid once per recovery rather than per
  entry) pays for them.
- **Entry edges decline on interval widening.** A funnel joins its delivered
  intervals into the header's prediction; an entry that would widen a stored
  interval declines instead — unless the gap is *guardable*, in which case it
  pays a bounds check against the stored interval and delivers it unchanged.

  That clause is on unconditionally: a program point has one prediction, the
  join of its own arrivals, so an admission cannot reshape the map. A proof
  edge *proves* that prediction rather than joining into it, so the cliff
  that would come from admitting a guardable edge into the wrong version is
  structurally impossible rather than empirically avoided.

Emission is two phases in fresh blocks that touch no lineage state: an
AND-chain of tag and exact-i64 guards, then a second AND-chain of class-word
tests. The phases are separate because a class-word read must only run behind
the phase-1 object proofs — a wild in-bounds read of a non-object payload could
spuriously match.

The steady dirty cycle's own back edge deliberately does **not** conform: a
population whose facts are genuinely false (an unstamped receiver, say) would
pay the guards every iteration and fail them every time. That was tried, and
measured a 91% conform-failure rate.

For a loop nest, a non-outermost header's proof target is a *new copy* of the
inner loop labelled by the arriving lineage's outer labels. The copy cycles
alone, and its outer back edge drops the outer token at the outer header and so
re-enters the outer `Opt` cycle at *its* header: one loop level recovered per
iteration, reducibility preserved.

#### The just-in-time on-ramp: conforming at a call return

The proof is not a loop mechanism. What is loop-specific is the *policy*
around it — which target, which edge classes may attempt one, what an
unprovable interval means — and that policy is now separated from the
mechanism: `conform_gaps` produces the plan (which slots owe which guard,
what interval each delivers) and `emit_conform` emits the chain, and both are
shared.

The second caller is a call return. A call site with a keep fork sends its
merge to GEN, because every arm reaching it failed the callee's runtime
intactness proof — but that proof is all-or-nothing about the whole heap, and
failing it is not the same as contradicting anything this lineage believes.
So the merge asks the narrower question instead: re-prove the successor pc's
own prediction, fact by fact, and take the `Opt` version if the guards pass.
The target is the version the lineage was continuing into anyway (same pc,
same token class, computed as if the track had not been stepped, same depth),
so nothing about the CFG changes; the bail is the GEN continuation that
already exists; the failure arm mints nothing.

Its admission rule is the pair the loop-header refutation showed was missing —
what the proof costs (the guard chain's length) against what it recovers
(the bytecode span from the successor to the end of the innermost enclosing
loop, past which the loop's own header on-ramp is the recovery mechanism).

**Measured: correct and inert.** The proof's take rate on richards is
3,878 of 3,878 — every fact held across a call return re-verifies, exactly as
the design predicts, because the write the callee reported was to some other
object. But that population is 0.05% of the bench's scripted-call departures:
the effect-flag fork's epoch keep arm already recovers the other 99.95%,
so there is no post-call dwell left at a call return on this corpus. The
mechanism is kept for the population that exists elsewhere, and must not be
cited as a measured win.

### 4.9 Reducibility by construction

The argument, in full:

> `out_tokens_for` drops a loop's own token at its header pc, so every edge to
> header `h` — entry or back — targets `(h, outer class, track)`. Every pc in
> that header version's body carries a token naming *that* identity, so a body
> version's only predecessors are that header and its own body. Each cycle
> therefore has exactly one entry, at the header. Tracks only descend, so a body
> version on a lower track back-edges to the lower track's header — again an
> entry at a header, never into the middle of a cycle. Cross-lineage edges
> (guard misses, weakened joins) land in `TOK_PEEL` copies, which are acyclic by
> construction.

`assert_reducible` (`bbv/licm.rs`) checks it after emission — a retreating
RPO edge whose target does not dominate its source. This is exactly waffle's
own test.

**The argument has a precondition, and it is now stated.** Every step above
rests on the emitter knowing where the loop headers are, and it learns that
from the `LoopHead` markers: `scan_loop_intervals` records an interval only
for a back edge whose *target* carries one. A back edge to an unmarked target
therefore gets no interval, so no token, so no re-labeling — and the cycle can
acquire a second entry, which is exactly the irreducible shape.

Measured over the whole benchmark corpus, the count is zero: SpiderMonkey
marks every loop header, so real bytecode always satisfies the precondition.
The counterexamples were four hand-written unit-test programs
whose loops had no `LoopHead` at all (they were not being compiled as loops
either, so they were not testing what their names claimed).

The precondition is a gate: the driver refuses a
script with a back edge to an unmarked target up front, alongside the other
structural refusals, and an irreducible edge surviving `assert_reducible`
after that is a compiler bug — the body is refused (`Outcome::Skipped`, which
leaves the script on the interpreter and is always sound) rather than shipped
with a loop analysis that could not run.

There is **no relooper in this tree.** Structured control flow is
reconstructed by two waffle passes:

- `reducify.rs` runs first and unconditionally. It scans for a retreating edge
  to a non-dominator in `O(E)` and, in the by-construction case, returns the
  body borrowed and untouched. When it does fire, it makes the CFG reducible by
  context-sensitive tail duplication, cloning blocks per "skipped-header
  context". That pass carries an explicit exponential-blowup warning, which is
  why the by-construction property matters: it is the difference between a
  no-op scan and a potentially multi-megabyte duplication.
- `stackify.rs` then reconstructs `block`/`loop`/`if`/`br` from the reducible
  CFG. It implements Ramsey's *Beyond Relooper* (ICFP 2022), iteratively rather
  than recursively, and hard-errors on an irreducible edge — which is what
  reducify exists to prevent.

waffle's `localify.rs` afterwards lowers SSA values and block parameters to
wasm locals with explicit moves at branch sites. Block parameters are a
waffle-IR construct; wasm has none.

### 4.10 Carriers: values that cross versions unboxed

A value crosses a version boundary **in its proven representation, never
reboxed**:

```rust
// bbv/ctx.rs
enum Repr {
    Boxed,   // i64: the NUNBOX32 JS::Value
    I32,     // raw int32, tag proven
    F64,     // raw double
    Bool,    // 0/1
    I64,     // exact integer, |v| <= 2^53, never -0
    StrPtr,  // raw JSString*
    ObjPtr,  // raw JSObject*
}
```

`slot_repr` (`bbv/version.rs`) is the entire policy — it maps a joined fact to a
representation, and `ensure_version_block` types the block parameters from it:

```
prim_mask == INT32   && !outside          -> I32
prim_mask == BOOLEAN && !outside          -> Bool
numeric && range != Top                   -> I64        (exact integer track)
numeric                                   -> F64
prim_mask == STRING  && !outside          -> StrPtr
prim_mask == 0       && outside           -> ObjPtr
otherwise                                 -> Boxed
```

The block parameter layout is fixed:
`[ stack(depth) | carried locals (sorted) | carried args (sorted) | flags? ]`.

**On GEN there are no carriers**, because there are no facts: `slot_repr` of a
facts-free slot is `Boxed` for every slot, so every crossing is boxed. Note
what that does *not* say. `Ctx::carried` — which locals ride the edge as block
params at all — is a location, not a fact, and is untouched: the JS operand
stack and the carried locals still flow through SSA block params on GEN, they
are simply all boxed `i64`. Keeping values in SSA dataflow rather than
load/storing the AOT frame at every opcode is real and free, and is not what
facts-free GEN gives up.

**Locals-into-SSA.** `Ctx::carried` names the locals arriving as block
parameters. It is a *liveness* set, not a fact set: the proposed set on an edge
is every hot local for which the emitter currently holds a live SSA value. The
alternatives were all tried and all lose:

- gating on a non-TOP *fact* — this misses the point, because the frame
  traffic that matters is the *un-carried reads*, not the typed ones, so a
  hot function ends up with far more boxed loads than carrying by liveness
  gives it;
- proposing every local the script touches;
- re-offering only what a may-GC call swept.

An edge load is paid on *every* edge, and with this many versions per pc that
beats the lazy frame read it replaces.

**Write-through discipline.** A local store still writes the boxed value to
its frame slot, so the frame is always root-complete and the GC's view is
never stale. Reads and version edges use the carrier. At a may-GC point
carriers are **dropped, not reloaded**: raw numeric representations are immune,
`StrPtr`/`ObjPtr` die (the frame slot is the copy the GC updates), and a
`Boxed` carrier survives only if its tracked fact excludes GC things. Reloading
at the sweep point would not even typecheck as SSA, since a may-GC call is
often emitted inside a diamond arm and a value defined there does not dominate
the merge that follows.

Edge conversions (`convert_to_repr`, `bbv/object.rs`) are one instruction each
in the common cases. Two of them carry measured history: a boxed-to-f64 edge
deliberately takes the *uniform* unbox rather than a fact-specialised shortcut
so that identical unboxes GVN together, and the unboxed-to-f64 path was a
box-then-immediately-unbox round trip until it was fixed (+5.0% on one
benchmark).

### 4.11 Effects and LICM

Every emitted instruction is annotated at emission time in a side table keyed
by waffle `Value`. Two vocabularies (`compiler/src/wasm/effects.rs`):

`EffectClass` — what a callee can *do*:

| class | meaning |
|---|---|
| `Pure` | wasm ops, frame traffic, validated-fence loads, class-word tests, tag tests; helpers that only read engine state |
| `Leaf` | provably runs no user code and no GC |
| `Alloc` | may GC, never runs user code |
| `Unknown` | can reach user code |

Unlisted functions — compiled bodies, stubs — are `Unknown`, because a scripted
callee runs arbitrary JS. Where a helper's contract is ambiguous the
classification is biased down (Leaf-or-Alloc picks Alloc; Alloc-or-Unknown
picks Unknown).

`HeapKind` — *where* an access lands, the abstract-location vocabulary for
LICM write summaries: `EngineTable`, `FuseCell`, `Shape`, `ClassWord`,
`ElementsHeader`, `Elements`, `Slot`, `StringData`, `AllocCursor`, `Fresh`,
`Unknown`. Two of these encode real reasoning rather than a partition:

- `EngineTable` rows (IC ways, cached call targets, guard cells) are hoistable
  even across may-GC arms, because every consumer re-verifies against live
  object state — a stale row read can only *miss*, never yield a wrong value —
  and their addresses are compile-time constants that a GC cannot relocate.
- `FuseCell` is the opposite: the cell **is** the soundness guard, so it is
  never hoistable across a call.
- `Fresh` names stores into nursery memory this body claimed from the bump
  cursor. Without a GC the cursor only moves up, so fresh bytes are disjoint
  from every address that existed at loop entry, and a `Fresh` write can never
  invalidate an invariant load.

What a **call kills** (`note_call_eff`, `bbv/facts.rs`): a may-GC call kills
every class fact everywhere in the frame (including the caller-frame facts
carried across a splice) and sweeps every GC-pointer carrier. It does *not*
kill primitive masks, ranges or intervals — those are properties of the value,
and they move with the object — and it does not step the track
([section 4.7](#47-tracks-what-replaced-the-optovfgen-attractors)). The arm
that *ran* the call steps it, at its continuation (`dirty_edge_to`), because
what that arm hands the successor is a claim-free result.

There is one important exemption, the **quiet alloc**: an `Alloc`-class helper
that may GC but provably writes no pre-existing user-visible heap (object and
array literal allocation, `create_this`, string building, closure creation).
A GC invalidates raw pointers only, so a quiet alloc sweeps carriers and
nothing else — no track step, no fact kill.

The arm-scoped alternative — "a call only dirties a lineage that has already
left the happy path" — is decisively worse: narrowing the kill that way is
about *where* the lineage is, not about how much of it a call may reach, and
it costs far more than it saves. The conclusion that stands: **keeping facts
across a call needs a proof, not a weaker dirt rule.** That proof is the
effect-provenance return word described in section 5.5.

**LICM** runs on the emitted waffle IR, after the body is built and only if
`assert_reducible` succeeded. It identifies natural loops from back edges whose
target dominates the source, processes them innermost-first so inner hoists can
cascade outward, and hoists a load when its address operands are loop-invariant
and no in-loop instruction's effect may-write the load's heap kind. The pass
early-outs in two RPO scans when there is no retreating edge at all, which is
the common straight-line case.

### 4.11a Per-binding value facts

A `GetGName` of a syntactic global whose name is an own data property of the
snapshot's global object (`EnvLayout::gcell_bids`) carries a **value fact**:
"binding B's value has tag type T", in `Ctx::gcells` beside the slot vectors
(joined pointwise, part of `implies`, in the per-pc prediction; frame-
independent, so one vector serves the root and its spliced segments). The
read's tag ladder installs it; a later read whose held fact implies the pc's
claim runs the fuse/slot diamond alone and pushes from the fact
(`SlotRef::GCell`). It is deliberately a fact about the value, not about the
cell: it holds on the fuse-hit AND the guarded-slot arm, so no fuse miss has
to leave Opt (the in-process lane distrusts every value fuse, and a `delete`
or `defineProperty` on the global blows a cell for good). For a fact-carrying
binding the read's generic-helper arm has no Opt keep continuation (it would
rejoin `next_pc` without the fact and strip it from the prediction).

What kills it: any `SetGName`-family store in the body (the inline form
re-installs the stored value's fact for its own binding and keeps the
others; the generic form, which can run a setter, kills all). Across a call
the fact survives a keep continuation when the callee's word has `FLAG_BIND`
clear (bit 3 of the sig2 word, set by every inline and generic binding
store, carried by the fold masks) OR the **binding-write epoch** is unchanged
(`gBindEpoch`, bumped by the runtime's fuse blow/value-change paths and by the
compiled inline store; its address is published in the strlit block like the
stamp epoch's). Otherwise each fact is re-proven on the spot through the leaf
`night_runtime_binding_value` (armed cell, else guarded resolve and slot,
else a magic Value) and a tag test, failing to `dirty_edge_to`; on-ramp proofs
discharge the facts the same way (`ProofSrc::GCell`). Neither the stamp word
nor the stamp epoch can stand in: a binding is a slot of the global object,
not a claimed layout.

### 4.12 Capacity: the compile ladder

Version count is bounded structurally, but *size* is not — the wasm function
size limit and the backend's tail duplication are real and independent. So
there is a cap, `MAX_BODY_VALUES = 300_000` SSA values, and a three-rung
descent when it is exceeded:

```
rung 0:  full refined
rung 1:  splice-facts off      (only taken when that dimension plausibly tipped
                                a near-cap script; huge overflows skip it)
rung 2:  gen-only              (facts-empty versions everywhere)
         -> still over cap?  Outcome::Skipped("body too large")
```

The same ladder catches a failed closure check. Its bottom rung always
terminates, because facts-empty versions cannot move under any arrival.

A generator or async body starts on rung 2 and stays there. That is not a
capacity decision: a version's identity names a *location* inside the body
(its loop-token class and its track), and a suspend leaves the body
entirely, so a resume has no arriving version to name. On the gen-only rung
there is exactly one version per pc, which the resume dispatcher can name by
pc alone. See section 5's generator ladder and `bbv/generator.rs`.

A rejected alternative is worth naming: a single global "mint fuel" counter
gating optimistic version minting, twin minting, dirty forks *and* splice
admission at once would let one cliff silently change four unrelated
decisions. The caps are per-decision instead.

### 4.13 The program-point graph

`bbv/cfg.rs` builds a CFG, dominator tree and loop nest over the **unified pc
space** — the root script's bytecode plus every frozen splice segment, in the
numbering `ensure_seg` laid out. It is the graph over *program points*, which
is the space the prediction is keyed by (4.1), and therefore the space a fact
question is asked in. It is distinct from both of the graphs that existed
before it: `likelier/` has no blocks at all, and the emitter's graph is
waffle's, which is one node per emitted **version**.

It is built lazily, once per walk, and only after the splice set is frozen
(4.3) — before that a segment the next walk creates would extend a pc space
the graph does not cover.

Three things need it. "Check here, trust below" is a dominance query, and it
is what replaces `refine_src` when the guard-derived family of the transfer
function moves out of the emitter. A redundant guard is a guard whose
condition a *dominating* guard already proved. And the loop nest is the third
derivation of a structure the extent scan (`scan_loop_intervals`) and the
token machinery each compute a weaker form of.

**The extents are not replaced, they are audited.** They key interned token
classes, so renumbering them would renumber every version; `--dump-cfg`
instead reports every extent whose header has no back edge here and every
header with no extent. The two derivations share nothing — a `LoopHead`
marker with a backwards branch to it, against a back edge to a dominating
block — so a disagreement is a bug in one of them, and finding it is the
point.

Soundness is by over-approximation in **one** direction: more edges means
weaker dominance, which is what every consumer wants. Two places take it. A
spliced call site keeps its generic fall-through edge beside the edge into the
segment, because a walk that declined the splice really does take it. And an
exception landing is made a successor of the **entry** block rather than of
every pc its try range covers, so it is dominated by the entry alone — the
weakest true answer, at a cost of one loop corpus-wide.

---

## 5. Lowerings

This section is the one a reader most needs in order to predict what code a
given bytecode op produces. Each op is described as a **ladder**: an ordered
list of layers, each with the fact that licenses it, the runtime check that
discharges that fact, the code emitted on the fast path, and where a miss goes.

Two structural notes first, because they apply to every ladder.

**Arms end in one of two ways.**

- *A continuation (side arm).* The arm's block ends in a branch to the version
  of the **successor pc** under weaker facts. It never rejoins. The
  fall-through keeps the strong fact. `side_arm` (`bbv/arms.rs`) saves and
  restores the whole lineage state around the arm (as one `ArmState`, so no
  site can save half of it) and steps the track down.
  Class-fact arms, typed-load ladders and every arithmetic slow arm work this
  way.
- *An in-version merge (diamond).* All arms branch to a merge block that
  carries the whole operand stack as block parameters, plus a result and an
  `ok` parameter. Emission then continues in the same version at the same pc.
  The inline caches and the element diamonds use this internally.

The choice is not arbitrary. Comparisons use a diamond precisely because every
arm produces the same implication (a boolean), so per-arm continuations would
be merged back together anyway; property reads use continuations because the
arms produce genuinely different facts about the loaded value.

**Every fact lookup is keyed `(source_id, evid_pc(pc))`.** Inside an inline
splice segment, `evid_pc` subtracts the segment base so analysis facts stay
keyed to the callee's own bytecode offsets.

### 5.1 Value representation and the cost primitives

The runtime is `wasm32`, so a boxed `JS::Value` is SpiderMonkey's **NUNBOX32**
layout held in a wasm `i64`:

```
value : i64  =  (tag << 32) | payload
```

Doubles are the raw IEEE-754 bits, all 64 of them, untagged; every non-double
tag lives in the high NaN space so it cannot collide.

| tag | value | payload (low 32) |
|---|---|---|
| `TAG_CLEAR` | `0xFFFF_FF80` | the double/non-double boundary |
| `TAG_INT32` | `0xFFFF_FF81` | the int32, raw |
| `TAG_BOOLEAN` | `0xFFFF_FF82` | 0 or 1 |
| `TAG_UNDEFINED` | `0xFFFF_FF83` | 0 |
| `TAG_NULL` | `0xFFFF_FF84` | 0 |
| `TAG_MAGIC` | `0xFFFF_FF85` | which magic |
| `TAG_STRING` | `0xFFFF_FF86` | `JSString*` |
| `TAG_BIGINT` | `0xFFFF_FF89` | `JS::BigInt*` |
| `TAG_OBJECT` | `0xFFFF_FF8C` | `JSObject*` directly |

Because the tags are ordered, three of the four common type tests are
**unsigned range compares, not equalities**:

```
tag_eq(v, T)       i64.shr_u v,32 ; i32.wrap_i64 ; i32.eq T      -- 3 ops
is_number_tag(v)   i64.shr_u v,32 ; i32.wrap_i64 ; i32.le_u INT32
                       -- true for every double (hi < 0xFFFFFF80) and for int32
is_double_tag(v)   i64.shr_u v,32 ; i32.wrap_i64 ; i32.le_u CLEAR
```

The shift/wrap pair GVNs across all tests on the same value, so a three-arm
diamond on one operand pays the shift once.

Boxing and unboxing costs, which set the price of every representation
decision:

| operation | emitted |
|---|---|
| box an `I32`/`Bool`/pointer | `i64.extend_i32_u ; i64.or (tag<<32)` — 2 ops |
| box an `F64` known exactly-double | `i64.reinterpret_f64` — **1 op** |
| box an `F64` otherwise (canonicalising) | ~10 ops, branchless: truncate, convert back, compare, detect `-0`, select. It re-tags an integral double as int32 — and explicitly refuses to do so for `-0` |
| unbox a number to `f64` | ~7 ops, branchless `select`, no control flow |
| `ToInt32` of an arbitrary double | 6 ops: `mul 2^-32 ; trunc ; mul 2^32 ; sub ; trunc_sat ; wrap` — NaN and infinities land on 0 correctly |

The canonicalising box is why an in-int32 *clean* interval additionally proves
an **int32 tag**: canonical boxing re-tags every integral double as int32, so a
value known to be an in-range integer and known not to be `-0` cannot be
sitting in the frame with a double tag.

There is one deliberate anti-optimisation here worth naming: a boxed-to-f64
edge conversion takes the *uniform* unbox rather than a fact-directed shortcut,
so identical unboxes at different sites collapse under GVN. The shortcut was
measured and loses.
### 5.2 GetProp

`emit_get_property` (`bbv/property.rs`). The ladder, in the order the emitter
tries it:

```
GetProp x
 |
 +-- L0  accessor arm            -- the name is a known accessor
 +-- L1  class-fact arm          -- the analysis knows the receiver's layout
 |        L1a  checkless immediate load
 |        L1b  three-bit stamp test, then the load
 |        L1c  SLOTS bit test, then the load
 |        L1d  fused identity + SLOTS test, then the load
 +-- L2  `length` arm            -- syntactic, string / Array / arguments
 +-- L3  charCodeAt/charAt fuse arm
 +-- L4  the inline property cache
 |        W0  monomorphic way, pre-decoded offset
 |        W1  monomorphic way, general tail (proto holder or dynamic slot)
 |        W2  the global megamorphic table
 |        W3  the miss helper
 |
 +-- (always) the typed-load ladder on the result
```

The order is unconditional: L1 pre-empts L2, so an `arr.length` site that also
has a class-fact row takes the class-fact arm.

#### L0 — accessor arm

- **Licensed by** `accessor_sites[(sid, pc)]` — a resolved getter script at a
  site whose receivers agree on one class — or, failing that, membership of the
  name in `accessor_names`, the set of names registered as accessors on *any*
  class. The second form arms the fully dynamic arm even where the receiver
  never classified.
- **Discharged by** three compares against a 2048-entry accessor cache keyed on
  `(receiver shape, atom<<1 | isSet)`: entry match on shape and key, plus a
  **holder liveness** check (`holder->shape == entry.holderShape`), because an
  accessor redefinition reshapes the holder.
- **Emits** roughly 18 ops and 6 loads, then falls through into an ordinary
  call to the cached callee — whose own likely-direct arm re-guards the callee
  identity.
- **Miss** side-arms to the successor pc running the ordinary property cache.

#### L1 — the class-fact arm

This is the layer that makes the tier fast, and it is the direct consumer of
the stamp (section 2.2). `emit_class_fact_get` (`bbv/property.rs`).

- **Licensed by** `prop_sites[(sid, pc)]`, which supplies a class-key range
  `[lo, hi]`, a **predicted fixed-slot index**, and the field's value mask; or,
  where no per-site row exists, by an *exact* class fact already live on the
  receiver operand plus the script's own predicted layout.
- Slot indices from the analysis are always **fixed** slots: the emitted
  address is the immediate `FIXED_SLOTS_BASE + 8 * slot`. There is no
  dynamic-slot form of this arm. That is why the construct site predicts
  `nSlots` (section 5.6) — a field that lands in dynamic slots is a field this
  arm cannot serve.

The four sub-arms differ only in how much of the claim is already proven:

**L1a — checkless.** Licensed by a live `cls_slots` fact on the receiver: some
*earlier guard in this same lineage* tested the SLOTS bit and `refine_src`
wrote the result back. Emits **one `i64.load`**. No word load, no branch, not
even an object tag test — the identity was proven upstream, and a value store
cannot clear SLOTS (the store-side chokes are TYPES-only). There is no miss
arm; re-testing here was measured to fragment a hot loop's code layout (+21%
instruction-cache misses), and block layout is not fixable downstream.

**L1b — the folded stamp test.** Licensed by a live identity fact where the
TYPES bit is not yet proven. Emits:

```wasm
i32.load  $recv offset=4        ;; the stamp word
i32.const want                  ;; TYPES|SLOTS, plus RANGES iff the site has a range
i32.and
i32.const want
i32.eq
br_if     <miss>
i64.load  $recv offset=16+8*slot
```

Five ops and one load. The RANGES bit joins the mask **only where the site
carries a range claim**, so the fold is the same `and`/compare against a wider
immediate and the range rides in free. This is the one property read that
consumes a RANGES claim: the loaded value is pushed with an interval attached,
which downstream arithmetic then elides overflow checks against.

`refine_src` then mints durable `cls_shallow` and `cls_slots` facts, so the
*next* read of the same receiver in this lineage is L1a.

There are deliberately only two arms. A third arm serving SLOTS-only receivers
with a boxed immediate load was measured to bloat hot code units; that
population is served correctly, just more slowly, by the inline cache.

**L1c — SLOTS bit test.** One load, `and`, `ne`, branch. Used when the site is
untyped or TYPES is already proven.

**L1d — fused identity plus SLOTS.** Used when a site row exists but the
receiver carries no covering class fact. One class-word load and one fused
compare:

```wasm
;; exact fact (lo == hi):
i32.load offset=4 ; i32.and 0xFFFF|SLOTS ; i32.eq (k|SLOTS)          -- 3 ops
;; range fact (lo < hi):
i32.load offset=4 ; and 0xFFFF ; sub lo ; le_u (hi-lo)
                  ; (word & SLOTS) != 0 ; i32.and                    -- 6 ops
```

Fusing means a miss says exactly "the immediate arm does not apply" — identity
misses and SLOTS-cleared receivers both belong on the cache route.

Misses from L1b/L1c/L1d all side-arm to the successor pc running the full
inline cache.

#### L2 — the `length` arm

Purely syntactic (the name is `length`). A four-way chain: string
(`i32.load str+4`), Array (walk shape to base shape to clasp, compare against
the Array class, then load `elements_` and the length word, check it fits
int32), mapped or unmapped `arguments` (packed word, overridden bit, argc), and
otherwise the cache. The Array path is 4 loads for the clasp walk plus 2 for
the length, about 12 ops.

#### L3 — the string char-op fuse arm

For the names `charCodeAt`/`charAt` on a possibly-string receiver, three
conditions are AND-ed branchlessly: the value is a string, a process-wide fuse
word is clear, and the startup-cached original native's cell is populated. On
success the **cached native's boxed bits are pushed directly** and the property
lookup is elided entirely.

#### L4 — the inline property cache

`emit_get_prop_ic_inline` (`bbv/property.rs`). Cache rows live in a reserved
linear-memory region, one row per emitted site, at an address baked as a
placeholder constant and patched once the region base is known.

The row is deliberately **monomorphic**: polymorphism is served by the
polymorphic sentinel plus the linear-memory mega tables, so a second way would
be reserved-but-never-probed space in a region whose governing constraint is
data-cache locality.

```
site row (68 bytes):
  +0   recvShape      0 = empty, 1 = polymorphic sentinel
  +4   MONO OFFSET    pre-decoded own+fixed byte offset; 0 = take the general tail
  +8   holderPtr      0 = own property
  +12  holderShape
  +16  slotEnc        bit1 = dynamic, bits[31:2] = index
  +20  transition row (48 bytes): oldShape, newShape, slotOff, absSlot,
                                  4 x (protoPtr, protoShape)
```

The probe:

```
  tag_eq(recv, OBJECT)                         -- elided if the operand is object-only
  shape   = i32.load objptr+0
  w0shape = i32.load way+0
  shape == w0shape ?
    yes: moff = i32.load way+4
         moff != 0 ? i64.load (objptr + moff)          <-- W0: 11 ops, 4 loads
                   : the general hit tail               <-- W1
    no:  w0shape == POLY ? probe the megamorphic table  <-- W2
                         : the miss helper              <-- W3
```

The **general hit tail** loads holderPtr / holderShape / slotEnc, selects the
receiver or the holder as the base, re-checks the holder's live shape, decodes
the slot encoding branchlessly (which unconditionally loads `obj->slots_` and
`select`s, so it costs one extra load even for a fixed slot), and loads. That
is about 18 more ops and 6 more loads — which is exactly what field `+4`, the
pre-decoded offset, exists to avoid for own fixed-slot data properties.

The **megamorphic table** is a direct-mapped 8192-entry side table keyed on
`(shape, atom)`; the atom is a compile-time constant so its half of the hash
folds into an `i32.const`, making the inline hash 6 ALU ops.

There is **no generation counter on the cache**. Validity comes from the GC
callbacks that zero the whole region on a major GC, plus a targeted zeroing of
transition rows at minor-GC end when any row cached a nursery prototype.

Monomorphic-to-polymorphic transition is a state machine in the miss helper:
fill way 0 when it is empty or holds the same shape; on a **second distinct
shape** write the polymorphic sentinel and stop paying write-back churn. A site
that settles monomorphic after warmup re-learns after the next major GC zeroes
the region.

**Miss cost.** The helper call spills every live operand (boxing each and
storing to the frame), calls, and reloads. But the helper returns a **clean
bit**: a miss that was served by a pure slot lookup, a fast `length` path or a
cached add replay restores the full pre-call arm state and takes a *non-dirty*
edge to the successor pc. Only a miss that could have run user code poisons the
lineage.

#### The result layer: the typed-load ladder

Every GetProp result goes through one of two forms.

`push_load_typed` (`bbv/arith.rs`) is the **checked** form:

| site mask | emitted |
|---|---|
| absent (0) | push boxed, bottom type. **No test at all** — the absence of a claim is itself a claim |
| `0x8000` (object-only) | one `tag_eq(OBJECT)`; fall-through is an object-only lineage; the other arm side-arms to the same pc with a boxed bottom |
| int32-bearing numeric | one `tag_eq(INT32)`; fall-through wraps to an `I32` carrier |
| exactly double | one `is_double_tag`; fall-through is a bare `f64.reinterpret_i64` |

Two rules are recorded as measured: a mixed `int|double` mask takes the
**int32-first** form unless the analysis explicitly marked the site's double
evidence as fractional-reachable; and the double arm must **not** be widened to
mixed masks (-29% on one benchmark at identical version counts, with a 4.6x
rise in data-cache load misses).

`push_typed_field` (`bbv/property.rs`) is the **proven-numberness** form, reachable
only behind a passed TYPES guard, so there is no other-type arm at all: an
exactly-int32 mask takes one tag test whose losing side is a bare
`f64.reinterpret_i64`, and an exactly-double mask takes a checkless unbox.

An int32 default is wrong most of the time on real code, which is why the
ladder does not default to it.

### 5.3 SetProp

```
SetProp x / StrictSetProp x
 |
 +-- L0  accessor set arm
 |
 |   compute val_is_num   -- does this store violate any TYPES claim?
 |   compute init_mask    -- is this a constructor-init store?
 |   compute range_act    -- what does this store owe the RANGES claim?
 |
 +-- L1  class-fact set arm     (checkless / SLOTS test / fused identity+SLOTS)
 +-- L2  the inline set cache
          W0  monomorphic own-slot store
          W1  megamorphic table
          W2  add-transition replay
          W3  the miss helper
```

The set side mirrors the get side, with three additions that carry the whole
maintenance burden of the stamp.

**`val_is_num` — the choke-elision predicate.** A store must clear the TYPES
bit unless it can be shown not to violate any claim. Three independent licences:

1. the stored value is statically numeric;
2. the site's own row claims mask 0 for this field and the receiver's class
   fact is inside the row's key range — the TYPES claim covers *masked* fields
   only;
3. no row is needed: for **every** layout the class fact admits, this name's
   mask is absent or 0. An *unknown* layout counts as masked — only a layout
   whose masks we hold can prove the field carries no claim.

The third licence exists because rows are present at only a minority of store
sites in real code.

**The store choke** (`emit_store_choke`, `bbv/facts.rs`) is emitted after every
inline store arm and is the compiled twin of the engine's own store check:

```
1. the RANGE obligation, ALWAYS first:
     Nothing              -> emit nothing
     Clear                -> clear the RANGES bit  (load16/and/store16)
     Check(lo,hi)         -> tag_eq(INT32) && lo <= v <= hi ? nothing : clear RANGES
2. if val_is_num           -> return.  Nothing else is cleared.
3. site mask known and 0   -> return.
4. site mask known and m   -> one tag test chosen by m; on failure clear TYPES *and* RANGES.
5. site mask unknown       -> is_number_tag(v) ? return
                              : load the class word; if TYPES set, store it back cleared.
```

Step 1 runs *before* the numeric short-circuit deliberately: a statically
numeric value settles TYPES but says nothing about magnitude.

**The add check.** The transition-replay arm compares the runtime-assigned slot
offset against the site's prediction and clears the SLOTS bit on any deviation.
Three shapes: a known layout with this name at a known position (compare
against that offset); a known layout without this name (harmful only if the
assigned offset falls *inside* the predicted prefix, which would shift it); and
an unknown receiver, where a per-key table of predicted offsets is consulted
and anything unrecognised clears conservatively.

**The set cache.** Structurally like the get cache, with three differences: the
row's offsets 8 and 12 hold `slotEnc` and `absSlot` rather than holder fields;
there is **no pre-decoded monomorphic offset shortcut**, so every store decodes
the slot encoding and pays the extra `slots_` load; and there is an
**add-transition replay** arm.

The transition arm validates `oldShape`, a nonzero slot offset, and up to two
prototype `(pointer, shape)` pairs, and **requires the third and fourth
prototype slots to be empty** — deeper rows are punted to the helper, which
replays all four. That restriction was measured: validating four hops inline is
1.2% worse geometric mean, and a depth-split arm is an exact wash. The
conclusion is that the win is not the saved shape loads; it is not replaying
deep adds inline at all.

One precision cost is worth naming honestly: emitting the set cache
**unconditionally kills every live SLOTS fact in the body**, because the
transition replay's add check may clear the bit on an *aliased* object. It does
this even when the transition arm is not emitted, which is more conservative
than the stated justification requires.

### 5.4 GetElem and SetElem

Elements have no name to key a claim on, which changes the economics: a read
site can be *folded* by a claim, but a write site owes the maintenance duty
unconditionally. The analysis gates array claims on exactly that trade
(section 6).

#### GetElem — `emit_get_element` (`bbv/element.rs`)

```
recv_obj = object-only?  i32.const 1 : tag_eq(OBJECT)      -- each independently elided
key_int  = exact int32?  i32.const 1 : tag_eq(INT32)
pre = recv_obj & key_int

pre false -> the string arm, if the receiver may be a string; else the helper
pre true  ->
    L1 predicted typed-array arm     (only if the site has a TA-kind claim)
         shape -> baseShape -> clasp, compare against the kind's class
         idx <u LENGTH_SLOT payload             -- a detached TA has length 0
         data = DATA_SLOT payload
         kind-specific load; result is UNBOXED and typed with no tag test
         -> continuation at next_pc
    L2 dense arm
         native-object bit in shape->immutableFlags
         elements = obj+12 ; initlen = elements-12
         idx <u initlen ?
             yes: elem = i64.load (elements + idx*8)
                  tag_eq(elem, MAGIC)   -- the HOLE CHECK
                       hole -> helper
                       else -> merge
             no:  L4 polymorphic-TA probe (a pure leaf call, magic = miss)
                  L5 arguments[i] arm
                  else the helper
merge: refine the receiver to object-only and the key to int32,
       then the typed-load ladder, optionally folded (below)
```

Layer by layer:

| layer | licensed by | discharged by | miss |
|---|---|---|---|
| typed-array | a per-site typed-array kind claim | a **clasp pointer compare** plus an unsigned bounds check against the length slot; detachment is covered because a detached array has length 0 | clasp mismatch falls into the dense arm; out of bounds goes to the helper |
| dense | nothing (syntactic) | the native-object bit, an **unsigned** `idx < initializedLength` (so a negative index fails too), and a magic-tag hole check | hole to the helper; out of bounds to the next arm |
| string `s[i]` | the receiver may be a string and is not proven object | string tag, linear-and-Latin1 flags, `idx <u length`; then an inline-vs-pointer `select` and a `load8_u`, resolved through the engine's static one-character string table | any failure to the helper |
| polymorphic TA | the bundle references a typed-array constructor at all | none inline: a **pure leaf** call whose magic-tagged return means "miss" | to the arguments arm or the helper |
| `arguments[i]` | the script uses an arguments object | clasp is mapped-or-unmapped arguments, the element-overridden bit is clear, `idx <u argc`, and the slot is not a magic forward value | the helper |

Note the last one's mask: the polymorphic-TA arm is **empty for
typed-array-free bundles**, deliberately, so that a program with no typed
arrays does not lengthen hot dense-loop live ranges with a cold call block. And
a *dead* string arm is not free: a proven-object receiver kills the string arm
outright, because the dead arm's exactly-string exit lineage poisoned the
consuming version's context for whole loop bodies.

**The array RANGES fold.** When the site has an array element claim, the
typed-load ladder's int32 tag test is AND-ed with a full-word stamp compare
against `key | TYPES | RANGES`, and the fall-through carries a real interval.
The compare is exact equality of the whole word, matching how array allocations
stamp (there is no SLOTS bit on an array; arrays have no slot predictions).

#### SetElem — `emit_set_element` (`bbv/element.rs`)

Same preconditions, no string arm. The dense path:

```
native-object guard ; elements ; initlen
idx <u initlen ?
  yes: flags = i32.load (elements-16)
       careful = flags & (FROZEN | NON_PACKED)
       careful == 0 -> store                            <-- the hot path
       else -> frozen ? helper
                      : load the old element; hole ? the append arm's hole entry
                                                    : store
  no:  the append arm
store: i64.store ; the RANGES store duty ; post-write barrier unless the value
       proves it holds no GC pointer
```

**The append arm** handles two entries that converge on one probe: a true
append (`idx == initializedLength`, capacity available, element flags clear)
and an **in-bounds store into a hole**, which is add-like in exactly the same
way. The probe is a 512-row cache keyed on the receiver shape alone, holding
two prototype `(pointer, shape)` pairs and an is-array flag; validating those
proves add-safety, since only a prototype indexed accessor or a non-writable
indexed property could block the store, and both force a prototype shape
change. On success it stores, bumps `initializedLength`, and extends `length`
if the receiver is an array. **There is no host call anywhere in this arm** —
the standing observation is that a call in a hot diamond costs several percent
even when it is never executed.

Sending non-packed stores to the generic helper outright is not acceptable: a
descending fill pattern marks an array non-packed permanently.

**The element store duty** (`emit_elem_store_duty`, `bbv/facts.rs`) is the
prove-or-clear discipline for RANGES on elements. If the stored value's static
interval already sits inside the claim, **nothing is emitted at all**. Otherwise
the flags half-word is loaded and the RANGES bit cleared only if it was set —
test before write, so the usual case costs one load and never dirties the cache
line holding the object header. When the receiver did not classify, the
obligation is the *intersection* of every array claim in the bundle; a bundle
that claims nothing emits nothing anywhere.

#### Worked examples

`this.x` in a method, exact class fact live, `x` at slot 2, mask int32, TYPES
not yet proven, site has a range:

```wasm
i32.load  $recv offset=4                  ;; stamp word
i32.const 0x40030000 ; i32.and            ;; TYPES|SLOTS|RANGES
i32.const 0x40030000 ; i32.eq ; br_if <ic>
i64.load  $recv offset=32                 ;; 16 + 8*2
i64.shr_u 32 ; i32.wrap ; i32.eq INT32 ; br_if <dbl>
i32.wrap_i64                              ;; exact int32, interval [lo,hi]
```

11 ops, 2 loads, 2 branches. The **next** `this.x` in the same lineage is one
`i64.load`.

`obj.foo` with no analysis fact, a numeric site claim, monomorphic cache hit:
about 15 ops, 4 loads, 4 branches, plus a merge block carrying the operand
stack.

`a[i]` in a loop, receiver proven object, key proven int32, dense packed array
with an int32 range claim: about 25 ops, 6 loads — and **no tag tests at all**
on the receiver or the key, because both preconditions constant-folded away.
### 5.5 Call

#### The calling convention

Every compiled body has one signature (`translate.rs`, `night_abi_sig2`):

```
(i32 cx, i32 sp, i32 argc, i32 retval_out, i32 script, i64 newTarget)
    -> (i32 err, i32 eff)
```

- `err` — 0 ok, 1 exception pending; the caller routes a nonzero to its handler.
- `eff` — the two-bit **effect-provenance word** (`MUT_THIS` = 1,
  `MUT_OTHER` = 2). See "the flag fork" below.
- The **return value is not a wasm result.** The callee stores it through
  `retval_out`; the caller reads it back out of the frame.
- `script` is **ignored by every body**, and stays only because the
  specialized-call `call_indirect` and the adapter share the signature. A
  `JSScript*` held in a wasm local does not survive a compacting GC (`SCRIPT`
  is a compacting alloc kind), so `cur_script_value` re-derives it from
  `vp[0]` at each use instead — a slot the AOT value stack traces. A function
  body reads its callee's script pointer; a global body finds the script
  staged in that slot directly, as a private-GC-thing Value.

The engine-visible funcref table holds a per-script *adapter* with the older
single-result signature, so runtime entries and indirect dispatch never see
multivalue. Compiled bodies precede the contiguous adapter block in the table,
so a compiled-to-compiled indirect call can subtract a patched offset and reach
the body directly, skipping the adapter hop.

The frame, measured from `vp`:

```
vp[0]                        callee (the JSFunction value; a GLOBAL body has
                             no callee and carries its JSScript here instead,
                             as a private-GC-thing Value)
vp[8]                        this
vp[16 + 8*i]                 formal i
local_base = 16 + 8*nargs
vp[local_base + 8*j]         local j
[env slot] [args-object slot] [new.target slot] [rval slot]
[hoist region]               LICM re-derive anchors, rooted
operand_base ...             operand-stack spill slots
```

The prologue pads missing formals with `undefined` using a branchless `select`
per formal, undef-initialises every local and the rval slot, and — critically —
**lives in a synthetic entry block outside the version table**, so a branch back
to pc 0 cannot re-run it.

**Argument passing costs nothing.** Before a call the emitter spills every live
operand into the caller's operand region; the top `argc + 2` of those spilled
slots *are* the callee's frame prefix. The callee's `sp` is a pointer into the
caller's own operand area. There is no argument copy.

Two other pointers matter: `top` is the GC scan limit and doubles as the boxed
out-slot for the return value, and a stack-fits check (`top + frame + 64 KiB
<= limit`) is folded into every arm that enters a compiled body directly.

**The typed entry.** The top bit of `argc` is a selector. Every body strips it
first thing, so a stale caller-side claim is always safe. If the analysis
claims types for a body's formals, pc 0 has three predecessors: a *proven* edge
(the caller statically established every claim; no tests at all), a
*validation* edge (one tag test per claimed formal, seeding the same facts), and
a *failure* edge that steps to the `Side` track. A validation failure is not a
bailout — the invocation simply rides a weaker lineage through the same body.
On the caller side, a claim counts as proven only if the argument operand's
type implies the claim's *pass-arm fact*. A `Double` claim can never be proven
by a caller, because canonical boxing re-tags integral doubles as int32 in the
frame, so only the callee's own tag test can establish it.

#### The ladder

`emit_call_generic` (`bbv/call.rs`). Emitted control flow, in order:

```
;; --- pre-dispatch, branch-free ---
recv_bit  = classify the receiver for effect accounting
sel_argc  = argc | (caller proved the callee's entry claims ? SEL_BIT : 0)
spill_all ; compute frame_base, top ; fits = stack check

;; --- L0: fuse-guarded static call (only for a global-binding callee) ---
armed = globalVals[bid].fuse == 1
same  = callee_bits == globalVals[bid].value
if (armed & same & fits & <patched-enabled const>) {
    call <patched direct callee>(cx, frame_base, sel_argc, top, script, undef)
}

;; --- classify: the per-site call cell, then the chain ---
(funcidx, script, is_native) = classify(callee)

;; --- L1/L2: direct AOT entry ---
if (funcidx != 0 & fits) {
    funcidx == <patched expected> ? call <patched direct callee>(..., sel_argc)   ;; L1
                                  : call_indirect[funcidx - BODY_OFF](..., argc)  ;; L2
} else {
    ;; --- L3: builtin arms, only when no scripted callee was predicted ---
    [Array push / pop] [Math unary set] [clz32] [parseInt] [min/max/pow] [imul]
    [String charCodeAt / charAt / fromCharCode]
    ;; --- L4 ---
    if (is_native) native_dispatch(cx, top, frame_base, argc)
    ;; --- L5 ---
    else          night_runtime_call(cx, top, frame_base, argc)
}

;; --- merge ---
reload ; result = frame[top] ; route err ; pop operands
push the result, through the typed-load ladder if the site has a result claim
```

**L0 — the fuse-guarded static call.** Licensed when the callee operand was
read from a global binding that resolved, bundle-wide, to exactly one compiled
callee. Discharged by two loads and two compares: the binding's fuse word is
armed and the live binding value equals this callee. That **replaces the entire
classify** — five validating loads and their branch chain — with four
instructions, and the call is a *static* `call` that wasmtime can inline.
Bindings with conflicting callees across sites, or whose callee did not
compile, never enter the table and their arms are left as dead code.

**L1 — the per-site call cell plus the likely-callee direct arm.** Two stacked
mechanisms.

The **call cell** is a three-word row per site: cached callee bits, funcidx,
script pointer. It is probed with three unconditional loads and one `i64`
compare; a hit skips the whole classify chain, which is three dependent loads
(shape, base shape, clasp) plus flag and kind tests. Its soundness rests on
value identity being object identity, plus two rules: only **tenured** callees
are cached (a nursery address can be reused by the next minor GC), and the
region is zeroed by a major-GC callback. A zeroed row cannot false-hit, because
raw bits 0 is the double `+0.0`, whose cached funcidx of 0 routes to the generic
arm anyway. On a *second distinct* callee the row is stamped with a sentinel
value that no boxed object can equal, so a polymorphic site stops paying
write-back churn.

The **likely-callee arm** then compares the classified funcidx against a
constant patched at link time to the predicted callee's table index, and on
equality makes a static call. If the predicted callee never compiled, the
constant stays at `u32::MAX` and the arm is simply unreachable.

**L2 — generic AOT entry.** `funcidx != 0` proves a script-backed,
non-class-constructor JSFunction with a compiled body. `call_indirect` to the
body. This arm unconditionally flushes deferred state, records a may-GC effect,
steps the track and kills class facts and carriers.

**L3 — builtin arms.** Gated on the site having *no* predicted scripted callee,
since a site that predicts a script would only carry dead diamonds. Each arm is
call-free on its hit path and reads arguments straight out of the spilled frame.
Two identity idioms are used:

- comparing the callee's **`JSNative` pointer** against a pristine slot. This is
  necessary because self-hosted code calls intrinsic *clones* of the Math
  natives — distinct JSFunction objects wrapping the same native.
- comparing the callee **value** against a cached pristine builtin cell.

A monkeypatched `Math.sqrt` or `Array.prototype.push` is a different native or
a different value and self-misses. **No fuse hooks are needed in either case.**

**L4 — the native route.** The classify already established "function class, no
BaseScript". One branch on a value in hand reaches `native_dispatch`, which
calls the `JSNative` with the frame as its `vp`, skipping the generic helper's
arm chain. `Function.prototype.call/apply`, the `defineProperty` intercept and
rope flattening punt back out from *inside* that helper, so correctness never
depends on predicting which native this is.

**L5 — fully generic.** The complete engine call path: bound functions,
proxies, non-AOT callees, everything.

`Function.prototype.call/apply` has **no dedicated lowering**; such sites arrive
as ordinary calls with a native callee. The one exception is a recognised
`T.apply(this, arguments)` forward, proved per-script, where the `arguments`
object is elided entirely and a helper forwards the caller's live actuals into
the target.

Spread calls have no ladder at all: box five operands and call the helper.

#### The flag fork: keeping facts across a call

Because a may-GC call kills every class fact in the frame, a hot loop
containing even a trivial call runs permanently on the `Dirty` track. The
answer is not a weaker dirt rule (section 4.11 explains why a weaker dirt
rule costs more than it saves) but a **proof carried in the callee's
return**.

The effect word is an SSA value threaded through the version graph as a
trailing `i32` block parameter — not a frame slot, so the frame layout is
untouched, the GC never sees it, and there is no per-invocation initialisation.
Only bodies that some caller *demands* thread it; demand is computed by walking
the call closure from read-only-scanned monomorphic callees.

A body's static scan classifies it as `ReadOnly` (heap-read-only on its inline
paths; calls and constructs are allowed because their effects arrive
dynamically), `StoreOnly` (also property and element stores, plus literal
construction, whose receiver classification is emission's job), or `Fail`.
Return shapes follow: a threaded body returns the live accumulator on every
lineage — because the bytecode scan cannot see a *spliced* callee's inline
stores, so emission is the accounting of record — and a non-threaded body
returns a provisional zero on a clean lineage that the pass end **revokes** to
the body's classified write word if any version turned out to write.

That revocation carries more than it looks like. A non-threaded body's word is
a compile-time constant, so everything it claims must be provable statically or
revoked, and "no callee of mine wrote anything" is not statically provable —
`ReadOnly` permits calls precisely because their effects were expected to
arrive dynamically. A call does not step the track, so the revocation carries
this instead: every `or_flags_*` that finds no accumulator records its bits
body-globally (`untracked_flags`) and the pass end ORs them in, which revokes
the constant in any body that calls anything at all. Without that, a
scan-passing body nobody's demand closure reached could return a zero word
after calling something that wrote heap, and its caller's fork would believe
it — a miscompile this mechanism prevents.

At a static-target call arm the emitter then forks:

```
clean = ok & (flags == 0)
clean -> restore the pre-call arm state wholesale, re-derive the addresses of
         possibly-moved operands from their frame slots (a clean word no longer
         implies no-GC, since quiet allocs do not saturate it), and continue at
         next_pc as its own lineage -- facts, track and carriers intact
dirty -> OR the folded word into the accumulator, take the ordinary merge,
         and LEAVE THE OPT TRACK there
```

The track step on the merge is what makes the fork mean anything. Both sides
continue at the same program point, and a program point has exactly one
prediction; if the merge stayed on `Opt` the join would erase precisely the
facts the clean arm restored, and the mechanism would be dead. It is stepped
only where a keep continuation actually exists — a site with no fork has no
`Opt` lineage to protect, and stepping there is the old unconditional
`call_stepped_track`, which costs a weakly predicted tail its `Opt` code for
nothing. The merge, having failed the callee's runtime proof, may still
re-prove the successor's prediction fact by fact: the just-in-time on-ramp of
section 4.8.

Folding is a perspective translation and is subtle: the callee's `MUT_THIS`
names *the callee's* `this`, which is this frame's `this` only when the call
receiver provably is it. For a **fresh** receiver, the callee's own-this writes
hit an object no caller fact can reference, so `MUT_THIS` is dropped entirely.
Otherwise it widens to `MUT_OTHER`. Crucially the fold maps zero to zero, so
clean tests are unaffected.

The population gates are measured, not guessed: read-only-callee forks land
clean essentially always (one benchmark: 30,544,418 clean against 10 dirty),
store-only-callee forks took the dirty arm **100% of the time** corpus-wide, and
a site on an already-dirty lineage has nothing to save.

There is a second, cheaper form: a call site whose call-free numeric builtin arm
hits executes no helper and touches no heap, so **arm selection is the proof**
and its exit is a clean continuation with no flag test at all. Without it, a
`parseInt` coercion at a hot function entry left every downstream loop header
dirty.

### 5.6 Construct

`emit_construct_classify` (`bbv/call.rs`). The operand frame is
`[callee, this-placeholder, args..., newTarget]`.

```
funcidx = classify(callee)                 (or reuse one a splice already ran)

funcidx != 0 ->
    is_ctor & is_ordinary_kind & (newTarget == callee) & fits ?
        allocate `this`  (below)
        splice it into frame[1]
        RE-DERIVE the callee, its script and the real newTarget from the frame
             -- create_this may have moved them
        static call to the predicted ctor, or call_indirect
    : the generic helper
funcidx == 0 ->
    [new Array() arm]  or  the generic helper

merge: result = is_object(ret) ? ret : the reloaded `this`
       mark the result FRESH iff the ctor provably returns `this`
then:  emit_construct_class_guard -- one word compare on the result's stamp
```

**Allocating `this`** goes through a per-site cell holding a shape, slot and
element words, a nursery header, the constructor's cached shape, an IC
generation, and the cached prototype pointer and slot encoding. The fast arm
validates the cell, the constructor's live shape, the generation, and then
**re-reads `C.prototype` live** and compares it to the cached pointer — which is
what catches `C.prototype = ...` reassignment. Then it bumps the nursery cursor
and writes the header, shape, stamp word, slots and elements. That path
**cannot GC**, so it contributes zero effect; the reactor fallback saturates.
The per-path effect delta rides a block parameter precisely so the bump path
does not inherit the fallback's saturation — which is what lets a construct
site take the clean fork.

**How much is known statically.** Three tiers:

| tier | nSlots | stamp word written at allocation |
|---|---|---|
| the site resolves monomorphically to a constructor with a predicted layout | that layout's full row length | the early-key form of its layout key |
| the site has a shared-constructor record (the `Class.create()` idiom, where many classes share one constructor script) | the resolved class's row length | that class's key |
| neither | read from a per-funcidx region indexed by the *classified* funcidx at runtime | a keyless seed with all three validity bits set |

Predicting `nSlots` matters because the engine's own constructor-body property
count estimate can land predicted fields in *dynamic* slots, which the
fixed-slot arms cannot serve.

The keyless seed still sets SLOTS, because the delegate flows' *static* add
checks maintain it: positions are absolute, so an inconsistent flow self-detects
by position mismatch, and every unchecked add path clears conservatively.

**The `new Array()` arm** is licensed by an unresolved callee, zero arguments,
and the script naming `Array`; discharged by an identity compare against the
pristine Array constructor cell; and emits exactly what `[]` builds — a quiet
allocation, so the arm keeps its facts and carriers and marks the result fresh.

**Constructor-init stores.** Two mechanisms keep a freshly built object's
stamp claims alive through its own constructor:

- the *init mask* discipline: `this.f = v` inside a stamping constructor or an
  init delegate keeps the constructing sentinel and does a conform-check-or-clear
  on the masked field rather than a blanket TYPES clear. The receiver scope is
  own-`this` in the root body (with a prologue guard covering the foreign-`this`
  corner reached through `.call`), or own-`this` inside a **construct splice**,
  where the receiver is the freshly created object by construction.
- the *add check* described in section 5.3, which maintains SLOTS across adds.

**Exit stamping.** At every return of a registered stamping constructor the
body writes the class word, carrying forward whichever of the three validity
bits survived construction. A two-phase constructor's init delegate *restamps*
at its own returns, advancing a prefix key to the full key so full-only field
guards start hitting. A first stamp is classified `MUT_THIS` — nothing a caller
holds can be falsified, since the constructing sentinel fails every identity
guard on a not-yet-stamped object — while a restamp advances a *guardable*
prefix key, so pre-existing alias facts are real and it must widen to
`MUT_OTHER`.

Finally, `emit_construct_class_guard` runs immediately after every non-spliced
construct site: one word compare on the result's stamp against
`key | TYPES | SLOTS`, turning "the constructor's exit stamp usually set this"
into a fact that rides the value into its local. Every later field store on it
then elides the choke and every later load is checkless. This is the
one-guard-buys-the-lineage discipline applied where the lineage is *born*.

### 5.7 The inline splice

This is the most intricate machinery in the compiler, and it is worth being
precise about what it is *not*. There is no separate inliner IR, no recursive
translator descent, and no callee CFG import. A splice **maps the callee's
bytecode into a synthetic pc space above the root script's** and lets the
ordinary BBV workqueue emit it, with a *frame view* swapped in whenever the
emitter's current pc lands in that space.

#### Admission

`inline_candidates` (`bbv/inline.rs`) returns the predicted callees that pass, in
evidence order:

```
no analysis evidence at this site            -> decline    (never speculate blind)
depth >= 8 at a loop-interior site, 4 else   -> decline
caller needs an arguments object             -> decline
8 splice sites already in this body          -> decline
body already over 100k SSA values            -> decline unless every target
                                                is <= 160 bytecode bytes, and
                                                then only to 250k
site inside a non-Loop try-note range        -> decline
more than 4 targets                          -> decline
per-target size cap: 200 bytes in a loop, 150 outside (monomorphic);
                     500 bytes (polymorphic)
constructs: monomorphic only, plus a transitive closure budget of 300
```

Per target: non-empty bytecode within the cap, not generator or async, not
mapped-arguments, no non-`Loop` try notes, no loops if the *site* is inside a
loop, no environment ops, no `arguments`, no actual-args access, no
`new.target`, and **every op on an explicit allowlist**. The allowlist is much
narrower than the compiler's overall op coverage, because an unsupported op
inside a splice would sink the entire caller.

Note what is **not** required: no purity, no no-throw, and **no arity match**.
Over-application is sound precisely because the surplus actuals land on the
callee's locals region and the prologue's undef-init clobbers them — which is
observable only through `arguments`, a rest parameter, or actual-args access,
every one of which admission already rejects.

`splice_closure_cost` prices what a construct splice transitively pulls in,
because every other rule prices it in isolation. Bytecode bytes are the wrong
currency: a nested call lowers to a whole classify diamond (once per *version*
of a segment) while an add lowers to a handful of values, and a loop inside a
spliced callee is emitted once per version *and* duplicated per skipped-header
context by the backend on top of that.

The budget is scoped to constructs, and the reason is sharper than "size": the
*winning* call splices in one benchmark are strictly larger than the *losing*
construct splice in another on every currency. Size is not what separates them,
**benefit** is. A constructor splice earns exactly one thing — the field-init
stores running in the caller against a provably fresh `this` — so its closure
has a small fixed payoff and deserves a small fixed budget. A call splice's
payoff scales with what it removes, so it keeps the generous per-target caps.

#### The frame layout of a spliced body

At the call site the caller has `len` operands, the top `need` of which are the
call operands. The hit arm:

```
popped   = the top `need` operands
n_parent = the remaining caller operands
for i in 0..n_parent:  store box(stack[i]) at caller.operand_base + 8*i   ;; ROOT them
child_base = caller.operand_base + 8*n_parent
for (i, o) in popped:  store box(o) at child_base + 8*i   ;; the frame PREFIX
;; then, per target, inside its own hit arm:
pad missing formals with undefined      (compile-time-known argc, so no select loop)
undef-init every local and the rval slot
```

So the child frame is laid down **in place, at the top of the caller's operand
region** — exactly the trick a non-spliced direct call uses, minus the call
instruction. Entering the frame view then computes the callee's `local_base`,
rval slot and a fresh operand base above them, so a nested splice recurses the
same way. That block of stores *is* the callee's frame prologue, hand-inlined
and stripped of everything admission made impossible.

Inside a segment, `GetArg`, `FunctionThis` and the constructor exit stamp all
read from the child frame's offsets; locals are addressed off the view's
`local_base`, so no code needed changing.

Locals and arguments are real frame slots in the child region with the same
carrier layer on top, but **cross-frame seams carry nothing**: the callee's
locals were just undef-stored, and after a return the caller's carriers are
stale because the callee may have triggered a GC. The frame is the truth on
both sides of the seam.

#### The guard chain

```
funcidx = classify(callee)                 ;; the SAME cell and chain a real call uses
generic_blk:                               ;; the miss arm, emitted first
    emit_call_generic(...)                 ;; the classify repeats -- one dead compare,
    br dirty_edge_to(next_pc)              ;;   because the cell hits
chain_blk:
    <the shared frame build above>
    for each target:
        funcidx == <patched expected> ? hit : next target (or generic)
        hit: <pad, undef-init>
             swap in the callee's entry facts:
                 args_ctx[0]   = the call site's `this` operand fact
                 args_ctx[1+i] = argument i's fact
                 caller_locals / caller_args = the caller's own vectors
             br cont(segment base)          ;; an ordinary BBV edge
```

The entry-fact inheritance is the point: the callee's entry version inherits
the *call site's* proven types directly, which is what makes a splice more than
a copy of the callee body.

Polymorphic splices are load-bearing — dropping them measured -19% on one
benchmark. The per-caller cap of 8 sites is an instruction-cache tuning with
counters to prove it: at cap 8 one benchmark executes 5% *more* instructions
than at cap 64 and runs 5.5% *faster* (IPC 4.04 to 4.49, i-cache misses per 1k
instructions 7.81 to 5.60). Below 8 it inverts. Cap 4 puts the hot 90%
footprint under L1i and still scores worse than cap 8's larger footprint, so
**footprint is not the binding cost** — call overhead is, until code layout
takes over.

#### Construct splices

Monomorphic only, and three deltas: the miss arm hands the already-computed
funcidx down to the generic construct so its ladder is one compare rather than
a repeat; after the funcidx hit there is still a constructor-ness check, because
a funcidx hit proves a *script-backed JSFunction* and constructor-ness is a
JSFunction flag; and `create_this`'s out-slot sits past the child frame, in
dead space the locals init subsequently owns.

Two subtleties: the `newTarget` operand slot is dead once `this` exists (new
target users are rejected by admission) and is deliberately clobbered by the
argument padding; and the entry argument facts have **class facts stripped**,
because `create_this` may GC, which kills every durable class fact, and the
operand snapshot predates that call.

#### Returning

There is **no return instruction**. A return inside a segment is an *edge*:

```
if the segment is a construct: emit the ctor-exit stamp
emit any delegate restamp
rb = box(retval); constructs substitute is_object(rb) ? rb : frame[this]
restore the caller's fact vectors from caller_locals / caller_args
stack = []                                     ;; rebuild the CALLER's operand stack
for i in 0..caller_depth:
    stack.push(load(caller.operand_base + 8*i))   ;; RELOAD -- the GC updated in place
stack.push(retval, carrying the callee's PROVEN type and range)
br edge_to(ret_pc)
```

Two things make this work. The caller's operands were rooted at the call site
by the shared frame build, and the GC tracer updates those slots in place, so
reloading them is exact. And the caller's *frame facts* rode the segment
context and are restored here — sound because caller frame slots are private:
no environments in inline callees, no debugger, no generator callees (a
splice cannot cross a suspend), so no callee can reassign them. Class facts among them were already killed by any
may-GC call inside the segment.

The retval carries the callee's proven type and range across the seam. That is
a genuine cross-frame fact transfer, and it is what a splice buys beyond
removing the call.

#### Exceptions inside a spliced body

There is no deoptimisation and no bailout anywhere in this tier, so the only
question is a genuine JS exception. A helper's error return routes to the
enclosing handler, which is found by walking the *current script's* try notes —
under a frame view, the **callee's**. Admission guarantees the callee has no
non-`Loop` try notes, so there is never a handler, and the error block returns
from the whole compiled body with the exception pending.

That is only correct because the caller cannot have a handler over the call
site either — which is exactly why admission rejects a site inside any
non-`Loop` try-note range of the *caller*. An exception at a synthetic segment
pc could not match the caller's note ranges, so the site must have nothing to
match. Together the two rules make "throw out of a splice" equal to "throw out
of the whole compiled body", a state the engine already handles.

#### Splices and the version graph

Segment pcs are just pcs, so the version machinery works unchanged, with one
mapping: a synthetic pc's loop membership is tested against **every enclosing
call site, innermost first, each in its own pc space**. Splice code is
loop-*interior* code of the caller; without that mapping the whole splice tail
reads as a side entry and its back edges erode to the generic class. Collapsing
to the root-space site alone made every nested splice inside a callee loop read
as a side entry, whose return edge then gave that loop's cycle a second entry —
producing the only irreducible edges observed in one benchmark.

Recursion is not specially detected. It is bounded structurally: segments are
memoised on `(call pc, callee)`, so a self-recursive callee spliced into itself
needs a fresh call pc at each level and the depth cap terminates it.

A loop-bearing callee spliced at a loop-interior site is rejected outright: the
two-level version-loop nest is what the backend's duplication amplifies, and one
such pair lowered to a 44 MB function from a 13k-block body, past the engine's
function size limit.
### 5.8 Binary arithmetic

`emit_addsub_op` (`bbv/arith.rs`) is the template; multiply, divide, modulo,
the bit operations, increment/decrement and negate follow the same shape. Every
emitter first computes the **result interval** from the operands' intervals
using the algebra of section 7, and that interval is what selects the rung.

#### Add and Sub

**Rung A — both statically exact int32.**

- *Fact:* both operand masks are exactly `PRIM_INT32`, not object, no
  abstractions.
- *Check:* none for the type. Overflow is delegated, and is itself elidable.
- *Emits:* two sign-extensions (free if the operands are already on the
  exact-integer track) and one `i64.add`, then the overflow split.

**Rung B — the exact-integer track.**

- *Fact:* the *result* interval is **clean** (provably not `-0`) and inside
  `+/-2^53`, and both operands materialise as exact `i64` without an unbox
  diamond.
- *Check:* none. The interval algebra is the proof.
- *Emits:* **one `i64.add`.** No overflow test, no conversion, no branch. The
  result is carried as `I64` and its low 32 bits *are* its `ToInt32`, so
  consumers can wrap for free.
- *Miss:* there is no failure mode. This rung has none.

**Rung C — both proven numeric.** One `f64.add` after conversions that are free
for operands already carried as `F64`. The result mask follows one rule: only
when **both** operands are proven exactly-double does the result claim exactly
double (which makes its later boxing a single reinterpret); anything with an
int32-bearing operand keeps the wider numeric mask so the chain stays
canonicalised and visible to the int32 arms downstream.

**Rung D — nothing proven: the three-arm tag diamond.**

```
ab = box(a) ; bb = box(b)
br_if both_int32(ab,bb)        -> int_blk        ;; 7 ops
br_if both_number_tags(ab,bb)  -> f64_blk        ;; 7 ops, shift/wrap GVN'd with above
                               -> slow_blk

int_blk  (FALL-THROUGH, stays on the current track):
    wrap, extend, i64.add, then the overflow split
f64_blk  (side arm -> Side track, continues at succ_pc):
    two branchless unboxes, f64.add, canonicalising box       ~35 ops
slow_blk (side arm -> Side track, continues at succ_pc):
    spill, call the helper, reload, route errors               -> Dirty track
```

Every arm carries the result interval forward. That is not optional: the
successor join takes the meet across arms, so an arm that drops the interval
kills the slot's fact for *every* lineage arriving at that pc.

`+`'s slow arm pushes a **bottom** type, because `+` may concatenate; `-`'s
pushes numeric-or-bigint. Even so, the presence of a `Some` interval proves both
operands were numbers, so where the interval exists the concatenation case is
statically unreachable and the fact survives the join.

**The BigInt bit and the dynamic-code fuse.** Dropping the BigInt bit from a
helper result matters far past the one op: `is_numeric` is what admits the
*next* op's unboxed f64 path, and the BigInt bit alone disqualifies it. Whether
the bit is needed is only partly a static question — the module's own text is
scanned (`module_is_bigint_free`), but source compiled at runtime is not
scannable in principle. So when the text is clean the slow arm **splits on the
dynamic-code fuse** (`Helpers::dyncode_fuse_word`, one load and one `i32.eqz`
of a night-owned word the C++ runtime blows from `ScriptSource::assignSource`):
the intact edge claims `Int32|Double` and the blown edge continues at the same
successor pc, in its own version, carrying the BigInt bit — which is simply the
lowering a module with static BigInt evidence gets everywhere.

A static type claim has no branchless runtime guard. This tier has no deopt
landings, so the *only* sound recovery from a claim that may be false is a
second typed continuation, which is exactly what versioning is for. Measured
across the benchmark corpus, the second lineage costs well under 2% of module
bytes even on the heaviest case.

**The overflow split** (`int_result_or_ovf`, `bbv/arith.rs`):

```
w = i32.wrap_i64 sum
if the result interval is CLEAN and inside int32:
      push w as I32 with the clamped interval.   ZERO CHECK CODE.
else:
      fits = (i64.extend_i32_s w) == sum
      fits ? push w as I32 (clamped interval)
           : side arm -> f64.convert_i64_s, reinterpret; pushed as a boxed
                         integral double, PRIM_DOUBLE, continuing at succ_pc
```

The overflow exit is an **integral double boxed as raw f64 bits**. The
cleanliness requirement is real: int32-tagged operands are never `-0`, so a
flagged interval can never soundly elide the check.

#### Mul

The governing rule is **disregard overflow**: an int32 product is *typed* exact
int32, with the rare overflow and the rarer `-0` taken by side arms, rather than
typing every downstream use "might be a double" and poisoning the whole
consuming chain off the exact-int32 track.

The int32 rung has a **three-way interval split**:

```
prod = i64.mul (exact, at most 62 bits)
result interval inside int32 and CLEAN   -> checkless: wrap and push.
                                            Both the overflow half AND the -0
                                            test are dead.
result interval inside int32 but flagged -> only the slim -0 test survives
otherwise                                -> the full overflow + -0 ladder
```

The `-0` test is `product == 0 && (a ^ b) < 0`. The exit arm redoes the multiply
in `f64`, the only form that yields both the correctly-rounded wide product and
`-0` itself. Without the interval elision, this ladder can cost a quarter of a
multiply-heavy kernel's cycles.

The exact-integer rung applies to multiply as well, and there the clean-interval
test does double duty: it proves the product is an in-domain integer **and**
that it is never `-0`, which is the one case the f64 form would have had to
reproduce.

#### Div, Mod, Pow

**Div has no integer rung at all** — division is never exact-int in any
rendition — so it is either `f64.div` under a proven-numeric fact or a two-arm
diamond. NaN, infinities and `-0` all come out of `f64.div` correctly, and the
canonicalising box explicitly refuses to re-tag `-0` as int32.

**Mod does** have an exact `i64.rem_s` rung, and its admission *is* the interval
rule: the dividend must be provably non-negative and unflagged (otherwise the
result could be `-0`) and the divisor range must exclude zero (otherwise NaN).
Under those two conditions `i64.rem_s` neither traps nor disagrees with JS's
dividend-signed rule. Otherwise a leaf helper computes the correct `fmod`.

**Pow** has no ladder: box both operands and call the helper.

#### Bitwise operations

JS semantics are `ToInt32` on both operands, and wasm shifts already mask the
shift count mod 32 exactly as JS does. Three rungs: both operands wrappable to
int32 (one wasm instruction, no check); both numeric (six-op `ToInt32` each,
then one instruction); or a three-arm diamond. `>>>` produces a `u32`, which
does not fit int32, so it rides the exact-integer track with an interval of
`[0, 2^32)`.

Bit operations **cleanse the `-0` flag** — `ToInt32(-0)` is `0` — which is how a
masked or shifted chain recovers a clean interval after an operation that
flagged one.

#### The four epistemic states, side by side

For `a + b` at one pc:

| what is known | emitted |
|---|---|
| int32 with intervals proving no overflow | **1 instruction** (`i64.add`), or 0 extra if both are already carried as `I64`. No tag test, no overflow test, no branch, no block |
| int32, overflow unproven | ~5 extra instructions and one never-taken branch to a cold block |
| numeric but not int32 | no test at all, but the result is an `F64` carrier that costs a ~10-op canonicalising box at every boxed boundary unless the exactly-double fact holds |
| unknown | ~11 test instructions and 4 blocks on the fast path, plus two cold arms (~35 ops for the f64 arm; spill/call/reload for the helper arm) |

#### String concatenation

There is **no inline concatenation arm**. `+`'s non-numeric arm is a full helper
call whose result is bottom-typed on a `Side` lineage. The specialisation lives
in the runtime helper, which checks for string-string first and calls the
engine's string concatenation directly, skipping the generic `+`'s
`ToPrimitive`/`ToNumeric` dispatch on both operands.

### 5.9 Comparisons

`emit_compare_op` (`bbv/compare.rs`). Unlike arithmetic, comparisons use an
in-version **diamond** rather than per-arm continuations, because every arm
produces the same implication — a boolean — so continuations would be merged
back immediately anyway. The diamond is deferred-spill: the live operand stack
is snapshotted at entry and re-bound at the merge, so only the arm that actually
calls a helper pays a spill.

The ladder:

1. **Both exact int32** — one `i32` compare. Under proven int32 tags, loose and
   strict equality coincide, so both map to the same instruction.
2. **The exact-integer track** — one `i64` compare, admitted when at least one
   operand is already carried as `I64` (two `I32` operands take the cheaper
   int32 form instead).
3. **Both numeric** — one `f64` compare. **This is where NaN is handled, and it
   is free**: the wasm float comparisons are all false for NaN and `f64.ne` is
   true, which is exactly JS. IEEE serves both loose and strict number compares.
4. **Strict equality by raw boxed bits** — licensed when neither side may be a
   double, not both may be strings, and not both may be BigInts. One `i64.eq`.
   Object identity, nullish comparisons and int/bool all reduce to this.
5. **`== null` / `== undefined`** — an OR of two tag compares. The
   `document.all` "emulates undefined" check is elided entirely when the other
   side is a proven non-GC primitive, and is otherwise behind a runtime fuse
   word.
6. **The tag-guarded diamond** — int32 arm, f64 arm, then an equality ladder
   (for equality kinds) or the helper.

The **equality ladder** is worth spelling out, because it is where the
representation pays off. Both operands have already been proven non-number by
the diamond's tests, so for `===`:

```
both strings  -> the string equality arm
both BigInts  -> the helper
otherwise     -> i64.eq on raw bits.  Done.
```

No type peel at all: raw bits are exact for *every* remaining pair — object and
symbol identity, canonical booleans, null, undefined, and every mixed-tag pair
— except the two content-equality types. If the module's own text is provably
BigInt-free the both-BigInt tag test is dropped for one load and one test of
the dynamic-code fuse instead (section 5.8): while it is intact the bits are
exact unconditionally, and once it is blown every pair that reaches here takes
the helper, which decides content equality for real. Loose `==` keeps a peel
(both-boolean, both-object, both-string) because `1 == true` is true with
different bits.

Without this ladder every object `===` calls the compare helper, which on
object-heavy code is among the hottest generic-helper sites.

The **string equality arm** is a chain of cheap disproofs: pointer identity
(equal), length inequality (unequal), both-atom (unequal, since atoms are
deduplicated), and only then the helper. A linear character comparison leaf is
deliberately *not* emitted, because it is only reachable for a *proven* string
operand, which a bottom-typed operand never is.

The helper arm leaves the version entirely and continues at the successor pc
rather than joining the diamond — keeping the call out of the loop body is what
admits LICM on compare-conditioned loops.

### 5.10 ToBoolean

`to_bool_i32` (`bbv/frame.rs`), used by `Not`, `JumpIfFalse`, `JumpIfTrue`,
`Case`, `And` and `Or`. (`Coalesce` does not use it; it tests null and undefined
tags directly.)

```
Repr::Bool | Repr::I32   ->  the value itself, ZERO INSTRUCTIONS
                             (wasm br_if truthiness IS ToBoolean here)
Repr::I64                ->  i64.eqz ; i32.eqz                      -- 2 ops
Repr::Boxed, exact int32
        or exact boolean ->  i32.wrap_i64                           -- 1 op
everything else          ->  the full five-way tag ladder
```

The full ladder is a real diamond with a merge parameter:

```
hi = high word of the boxed value
hi <u TAG_CLEAR   -> double: (d != 0) && (d == d)      ;; nonzero AND ordered
hi == TAG_STRING  -> length != 0
hi == TAG_BIGINT  -> a helper call
otherwise         -> low32 != 0, AND NOT emulates-undefined
```

The last arm covers undefined, null, boolean and object in one shot: undefined
and null have payload 0, a boolean's payload is 0 or 1, and an object pointer is
never null. The `document.all` check is **triple-gated**: only for object tags,
then behind a process-wide fuse word (if the fuse is clear the whole check
yields 0 with no memory touched beyond the fuse), and only then a walk of
object to shape to base shape to class to flags.

One gap worth recording: there is **no `Repr::F64` fast case**, so an f64
carrier at a branch pays a box plus the entire tag ladder including a
statically-true branch. And the routine never consults the operand's interval,
so a value with a proven non-zero interval still emits the full test.
---

## 6. The analysis

`compiler/src/likelier/`. A whole-bundle, context-sensitive, inclusion-style
type analysis producing *likely facts*. It is optimistic by design, and that is
only sound because nothing it says is trusted: an unresolved flow contributes
*nothing* to a fact join rather than poisoning it to "any", and where evidence
runs out the analysis emits no claim rather than a weak one.

`likelier::analyze` runs five phases:

1. **scan** — one bytecode pass per script, producing a context-free constraint
   graph plus side tables;
2. **seed** — the snapshot image is loaded as the *initial state* of the shared
   cells;
3. **resolve shared constructor sites** — a syntactic and snapshot-concrete
   pre-pass for the "one constructor script, many classes" idiom;
4. **solve** — instantiate every script at the generic context, then drain one
   worklist to a fixpoint;
5. **emit** — project the fixpoint into `facts::LikelyFacts`.

### 6.1 The cell value: `TypeSet`

```rust
// likelier/types.rs
struct TypeSet {
    prims: u16,     // primitive tag bits + the unresolved-evidence bit
    fns:   FnSet,   // a bounded set of callable identities
    obj:   ObjPart, // object / class identity
    range: Range,   // numeric magnitude class (opsem::Range)
    hiv:   HeapIv,  // numeric value interval
}
```

Bits 0-5 of `prims` alias the codegen's primitive encoding exactly — one
alphabet across the whole compiler. Bit 14 is `P_UNKNOWN`, the distinguished
**evidence** marker: "an executed-but-unresolved flow produced this value" (a
megamorphic call result, an unmodeled builtin, a read off a lost receiver). It
is emphatically *not* the same as "nothing ever flowed": it propagates like a
primitive bit, poisons value claims, and is invisible to callee and object
resolution. It must never appear in an emitted mask.

Nullability *is* representable — null and undefined are ordinary bits — but is
deliberately **not exportable as a fact**. A `null|object` read stays bottom,
because a type in the codegen is a proof and the analysis only has a
prediction.

`FnSet` is a sorted vector with a saturation flag, capped at 8 identities; past
the cap the identities are dropped and the set becomes "megamorphic", which no
longer drives call resolution. Three identity spaces share the integer:
ordinary script ids, named natives, and the **allocating builtins** (`Array`,
the typed-array constructors) — so `var Vector = Array` flows array-ness through
cells like any other callable value.

`ObjPart` is the class-identity lattice:

```
                AnyObject                    top
                    |
                AnyOf(region root)           bounded polymorphism, flow-scoped
                    |
                ClassAny(class)              some instance of one class
                    |
                One(abstraction)             exactly one heap abstraction
                    |
                  Empty                      bottom
```

An `AnyOf` names a **region** — a union-find class over classes that actually
met in a join — and is consulted *only* by call-target resolution. Field masks
never consult it: a read off a precise `One` receiver must not see sibling
instances' values, and a read off a region must not see the merged set's cells.
That rule has a name in the source ("the mega-cell lesson") and it is the single
most important structural constraint in `heap.rs`.

### 6.2 The lattices

| lattice | elements | order | join | where |
|---|---|---|---|---|
| primitive mask | subsets of 7 tag bits plus the unknown bit | subset | union | `types.rs` |
| callable set | up to 8 ids, or saturated | subset, saturated is top | union with saturation | `types.rs` |
| object identity | the five points above | as drawn | table below | `types.rs` |
| magnitude | `I32 < I53 < Top` | chain | max | `opsem.rs` |
| heap interval | `Empty < In(lo,hi) < Num < Any` | as drawn | hull, quantized | `types.rs` |

The object join table:

| a \\ b | Empty | One(y) | ClassAny(d) | AnyOf(s) | AnyObject |
|---|---|---|---|---|---|
| **Empty** | Empty | One(y) | ClassAny(d) | AnyOf(s) | AnyObject |
| **One(x)** | One(x) | equal? One(x); snapshot-preferred; same class? ClassAny; else AnyObject | class(x)==d? ClassAny(d) : AnyObject | AnyObject | AnyObject |
| **ClassAny(c)** | ClassAny(c) | (symmetric) | c==d? ClassAny(c) : AnyObject | AnyObject | AnyObject |
| **AnyOf(r)** | AnyOf(r) | AnyObject | AnyObject | r==s? AnyOf(r) : AnyObject | AnyObject |
| **AnyObject** | AnyObject | AnyObject | AnyObject | AnyObject | AnyObject |

The engine then applies two **rescues** on top of this table: two array-classed
parts meet as one class (unioning their site classes), and two otherwise-classed
parts whose pure join would be `AnyObject` meet as an `AnyOf` region — again
unioning.

`TypeSet::join_from` (`types.rs`) is the raise operator, and it is
**directional by construction**: it mutates the receiver in place and returns
*whether the receiver grew*. That boolean is the fixpoint's only change signal.

Two honest departures from a textbook framework, both deliberate:

- **The object join is non-associative in one corner.** When two distinct `One`
  abstractions meet, a snapshot abstraction is preferred over an allocation-site
  one. In a three-way snapshot-plus-two-allocations meet the answer depends on
  worklist order; the code notes it is deterministic under the FIFO worklist.
- **The join has a global side effect.** The union-find rescues permanently
  merge classes. Cells already holding a region label are not re-raised; their
  label's meaning changes under them, and emission resolves labels to current
  roots at emission time so later unions never poison earlier evidence. It is
  monotone in region size, but it is not a pure lattice operation.

**Finite height** is argued explicitly: 7 primitive bits, 9 callable-set
growths, 3 object raises plus one snapshot absorption, array relabels, and the
interval ladder's roughly 36 rungs per bound. The product bounds a cell at 768
changes, which is asserted in debug builds.

### 6.3 The heap

There is **no oracle and no consultation point.** The snapshot is loaded as the
initial state of the same cells the program's own writes join into.

**Cell identity.** Per-context rows for `Var`, `Arg`, `This` and `Ret`;
context-free cells for everything shared:

| cell | keyed on | role |
|---|---|---|
| `GName` | a name | a global binding |
| `Aliased` | (scope, slot) | a closure environment slot |
| `Field` | (abstraction, name) | the per-abstraction field cell |
| `ClassField` | (class, name) | writes through a class-typed receiver |
| `ClassView` | (class, name) | **the read view**, fed by standing links from every `Field` of that class and from `ClassField` |
| `ThisField` | (script, name) | a script's accumulated `this.name = v` evidence |
| `ProtoSentinel` | abstraction | raised when a prototype link is installed, so chain reads that dead-ended re-fire |
| `ArrayElemsUnion` | — | the bundle-wide union of every array's element cell |
| `TableArgJoin` | (abstraction, index) | function-table dispatch, per argument index |

**Elements are a field named `[]`.** There is no separate element dimension;
element reads and writes are ordinary reads and writes on that pseudo-name.
Typed-array *kind* is carried on the abstraction and the class instead.

Abstractions are per-`(allocation site, context)`, plus snapshot objects,
function-object statics spaces, synthetic prototype abstractions (whose field
space *is* the class's method table), and synthesised native namespaces. A
class's identity is preferentially its concrete `.prototype` object, falling
back to its constructor script, falling back to a per-allocation-site
pseudo-class for classless literals and arrays.

**The reader** (`read_into`) dispatches on the receiver's object part: a `One`
receiver reads its own field cell plus the class field cell plus the accessor
getter plus the prototype chain — *not* the union root; a `ClassAny` reads the
class view plus the prototype abstraction and its chain; an `AnyOf` yields
`P_UNKNOWN` except for elements, and — **at callee position only** — the
region's method-table union, capped. That last exception is why call resolution
survives polymorphism while field typing does not. Prototype chain lookup is a
monotone join up to depth 8, never an if-else.

**The writer inventory** is the mechanism behind every field claim, and it has
three routes plus a fourth attribution channel:

| receiver | destination |
|---|---|
| `One(a)` | that abstraction's field cell |
| `ClassAny(c)` | that class's field cell |
| `AnyOf` / `AnyObject` | **dropped** (and the value escapes), except elements, which go to the bundle-wide union |
| a function | its statics cell; `.prototype` installs a prototype source, never value flow |

The fourth channel is what makes the inventory survive receiver saturation:
every `this.name = v` also raises into a per-script `ThisField` cell, which is
linked into the class field cell of every **home class** of that script. Homes
are installed for a constructor over its own class, for a method installed on
exactly one class, and transitively along `this`-forwarding call edges. Homes
are capped at 8 — a script homed everywhere is a shared helper, and more links
is the mega-cell again.

Everything then converges on the class view cell, which holds the join of every
writer that reached the class by any route. That cell is the emission oracle.

### 6.4 The engine

Nine constraint kinds (`Move`, `Const`, `Read`, `Write`, `Call`, `Apply`,
`ElemBuiltin`, `Alloc`, `Arith`), generated once per script and evaluated per
`(constraint, context)`. Constraint identity is context-free; operands resolve
to cells at evaluation time.

Two propagation mechanisms coexist:

- **Subscriptions** (lazy, per reader). Reading a cell records the reader and
  returns a snapshot clone. When the cell grows, every subscriber is
  re-enqueued. Installed on first read, permanent. This is what makes
  late-arriving evidence re-fire readers that already ran — the answer to the
  one-way-edge problem that directed constraint graphs otherwise have.
- **Standing links** (eager, cell to cell). A permanent edge that immediately
  propagates the current value. All the structural plumbing uses these: field
  to class view, class field to class view, prototype source feeds, `this`-field
  to class field, element cells to the bundle union. Link cycles terminate
  because a raise stops at no-change.

The worklist is FIFO with a membership set for deduplication. Bootstrap
instantiates every script at the generic context in sorted id order.

**Contexts.** A context is one interned id standing for a bounded call string —
not a graph copy; nothing is cloned. `push(ctx, sid, pc, callee)` applies four
rules in order:

1. **recursion collapse** — if the callee already appears in the parent chain,
   reuse the context it was entered at, so a strongly connected component is
   context-insensitive internally;
2. **depth cap** — beyond 8, fall back to the generic context;
3. **budget** — beyond 50,000 contexts, fall back to the generic context;
4. otherwise intern on `(caller context, call site, callee)`.

A fifth degradation lives at the call site: a site with more than four
non-builtin targets binds every target at the generic context. All five are
counted and reported.

Regions get their own context discipline: a callee resolved through a region's
method tables binds at a **depth-1 context parented at the generic context**,
one per (site, target) — never at the generic context itself (measured as
shared-row pollution, -18% on one benchmark) and never chained off the caller's
context (region fan-out through recursive callees exhausted the budget and
degraded the whole pipeline).

**Termination** rests on: every cell only rises, through joins monotone in each
component; each component has constant height; a constraint is re-enqueued only
when a cell it read grows; contexts are finite (interned, depth-capped,
budget-capped, recursion-collapsed); the union-find only merges; and link cycles
stop at no-change.

**Determinism** comes from dense allocation-ordered ids, sorted iteration at
bootstrap and in every emission loop, a min-root union-find, and immutable
per-abstraction join metadata. Order-independence is asserted by a shuffle test
over small graphs rather than by construction; the whole-bundle claim rests on
that diagnostic, not on a proof.

**One narrowing is deliberately unsound as dataflow.** A polymorphic dispatch
site whose receiver was lost does *not* propagate that lost receiver into a
method with a pinned `this` class. As dataflow that is a refusal of evidence; as
a prediction it is exactly the point — the dispatch site's lost receiver must
not destroy the body's asserted precision, because the body will guard anyway.

### 6.5 Calls

There is no separate call-graph structure: the callee is a cell, and the
dispatch ladder reads its callable set. Callable identity enters cells from
lambda opcodes, global-name seeds, the live global object's properties,
snapshot object properties, prototype-abstraction reads, and synthesised
natives.

At a call, arguments are read (which **subscribes**, so a later widening of a
caller operand re-fires the whole call constraint and re-binds) and raised into
the callee's per-context argument cells; the return flows back the same way. A
`this`-forwarding edge additionally records a delegation edge, which is how home
classes propagate.

Construct is richer: the site allocates an abstraction, raises it into the
callee's `this`, and — for a constructor with an explicit return — yields the
return where it is an object, joining the allocation in only where an *observed
primitive* return path exists. `P_UNKNOWN` explicitly does not count as one,
because construct semantics box a primitive return to the object either way.

**The function-table channel** solves a specific and common shape: a table of
functions saturates its element cell past the 8-identity cap, so dispatch
through it resolves nothing. Dispatch stays opaque — the return is unknown and
the arguments escape — but the site's *argument profiles* are raised into
per-index join rows on the table abstraction and fanned into every known
member's formals. Membership is collected where identities are still singular:
snapshot elements, per-context element writes, and per-site argument values
recorded before the row saturates. The members learn what the table is called
with even though no call site knows which member it reached.

**Escape.** A function value reaching an untracked sink gets a distinguished
*unresolved* type joined into its generic-context formals and `this`, once per
script. The value used is deliberately `unresolved` rather than "top": a
fabricated definite primitive mask poisoned every field cell an escaped method
wrote through `this`, and was indistinguishable from real evidence.

**Natives** are modeled from a spec table. Two refinements carry weight: an
*integral* native (`floor`, `round`, `parseInt`, ...) raises the magnitude
claim, and an *integrality-preserving* native (`pow`, `abs`, `min`, `max`)
does so only when its arguments are integral at that site — without which one
cold `Math.pow` call widened every arbitrary-precision digit cell in a
benchmark to unbounded.

`Object.defineProperty` is the one native with modeled *heap* semantics: it
reads the descriptor's getter and setter fields, registers the accessor pair on
the target's class, seeds the accessor bodies' `this`, and re-fires same-name
heap constraints.

### 6.6 What comes out: `LikelyFacts`

`compiler/src/facts.rs`. Every field is a prediction that codegen re-checks. The
contract is stated at the top of the file and holds for all of them: *a wrong
fact costs a failed guard, never correctness.*

`LikelyFacts` also carries the compilation's **one string table** (`Names`,
`ids.rs`). Every name in the tables below — property names, global bindings,
layout field names — is a `NameId` into it, and the table itself is handed on
to the translator, which takes ownership and adds the emitted module's dense
`atomId` numbering on top rather than keeping a second copy of the strings. So
a name is interned once, at the syntactic scan that precedes the analysis, and
crosses every phase boundary as an integer.

Facts are grouped here by what they license. The recurring emission discipline
is: **a per-site fact is the join over live contexts, and a genuinely
polymorphic site emits no fact at all.**

#### Class layout and stamping

| fact | says | licenses |
|---|---|---|
| `class_layouts` | per class, the ordered first-write field names; **position is the predicted fixed-slot index** | every checkless fixed-slot address; the serialized layout table the C++ validator checks against the live shape |
| `class_layout_masks` | parallel per-position value masks (numeric fields only) | the constructor-init conform check, and a load that skips the value tag test on a TYPES-and-SLOTS receiver |
| `class_layout_ranges` | parallel per-position intervals, only where a mask is also claimed | the store-side prove-or-clear duty; the three-bit stamp fold; a loaded value that arrives *with an interval* |
| `class_layout_typed_masks` | the name-keyed claim filling positions the layout tier left blank | takes priority when building the per-class mask row |
| `ctor_stamps` | constructor script to its class key | **the exit stamp itself.** Without it nothing is ever stamped and every class-fact guard misses |
| `ctor_nslots` | the constructor's full row length | the `new` site's allocation size, so every predicted field lands in a fixed slot |
| `deleg_restamps` | an init delegate to the full class key it completes | the restamp at the delegate's returns, advancing a prefix key to the full key |
| `deleg_inits` | scripts homed as `this`-forwarded delegates | keeping the add-transition arm on the set-cache tail in those bodies only, where it is worth over a million fixed-slot nursery adds per run, and nowhere else, where it is bloat |
| `construct_site_keys` | for the shared-constructor idiom, the per-site class | the allocation size and stamp word where the script-keyed tables cannot key |
| `group_tables` | per predictor group, the universal prefix names and the masks every member claims | makes a *range* class fact consumable: the guard becomes a key-range compare against a shared prefix table |
| `this_layouts` | a method script's predicted `this` class, exact or a range | serving `this.f` at sites with no per-site row |

#### Per-site property and element facts

| fact | says | licenses |
|---|---|---|
| `prop_sites` | per site: class-key range, predicted slot, value mask | **the class-fact arm** (section 5.2 L1) on both the get and set sides |
| `typed_sites` | the name-keyed type dimension, independent of the slot dimension — including names absent from every layout, and classes whose slot prediction never validates | fills the mask where the slot tier left it blank; the difference between a two-bit guard and a tag-free typed load |
| `field_sites` | per-GetProp value claim, retained only where no fenced table already covers the site | orders the typed-load ladder on an inline-cache result |
| `elem_sites` | per-GetElem value claim | orders the typed-load ladder on a dense read |
| `array_elem_claims` | per array region root: mask, low, high | array stamp keys, and the bundle-wide intersection an unclassified element store must honour |
| `array_alloc_sites` | which allocation sites stamp a fresh array | the stamp word written at allocation (the claim holds vacuously on an empty array) |
| `array_elem_recv` | the region root at each element site | the read-side fold and the write-side duty |
| `ta_elem_sites` | a settled typed-array kind at an element site | the guarded monomorphic typed-array arm |
| `elem_poly_sites` | every element site in a bundle that mentions a typed-array constructor; **empty otherwise** | the shared polymorphic typed-array probe — and its *absence* keeps typed-array-free programs free of a cold call in hot loops |

#### Calls, arguments, results

| fact | says | licenses |
|---|---|---|
| `calls` | per site, up to 4 resolved callee scripts | **all splicing** (no entry means no splice); the likely-direct call arm; and it *suppresses* the builtin arm bundle, since a site predicting a script would only carry dead diamonds |
| `native_calls` | that a site settled on *some* one modeled native — not which | the flags-fork gate around a builtin arm |
| `apply_sites` | every syntactic `.apply`/`.call`-shaped site | the apply-forward lowering, which elides the `arguments` object entirely |
| `accessor_sites`, `accessor_names` | resolved accessors, and names that are accessors on any class | the accessor arm, with and without a static target |
| `arg_types` | per script, `this` and formal claims | the guard-at-definition ladder at `GetArg`, and the body's typed-entry contract |
| `call_types` | per site, the result claim | one tag test on the generic call continuation. Object claims are emitted **only under receiver demand** — the result must feed an element access, because the element lowering's receiver tag test elides against a bare object proof while a property lowering's class-word guard does not. Claiming them blanket-wide is a substantial loss |

Every field here has an emission consumer. Nothing in `LikelyFacts` exists
purely to be printed: the un-projected lattice point, the class-region
union-find grouping and the layout-to-ctor mapping were all carried this far
for the viz layout panel alone, and were deleted with it. The panel now shows
only what the backend actually has — the stamped id, and per field its slot,
mask and range claim — which is the whole point of a panel that claims to
show what the compiler is using.

#### The fenced hierarchy

Property facts form a rung ladder, all speaking one guard form — a key-range
compare against the stamped class word — at four widths:

| rung | shape |
|---|---|
| exact constructor class | `lo == hi`, per-class row and masks |
| predictor group | `lo < hi` over a group's universal prefix, masks every member claims |
| region | `lo < hi` spanning several groups that met in a union-find region |
| per-name sub-range | the longest run of contiguous keys agreeing on a slot |

Class keys are assigned **region-contiguously** precisely so a region fact is a
plain range test in the same key space. A region range is minted only when the
region has at least two keyed groups *and* contains at least one constructor
key — a range holding only object-literal keys can never hit, and each read
pays the miss.

The hierarchy is closed by a **subsumption rule**: the unfenced per-read value
claim is dropped at any site already served by a masked class fact, because the
overlap is pure double-guarding.
---

## 7. opsem: the shared vocabulary

`compiler/src/opsem.rs`. One module holds the result-type algebra, the magnitude
rules and the exact-integer interval algebra of the JS numeric and string
operators, **written once and shared by the analysis and the codegen**.

Everything in it is stated at one epistemic level: what the whole program
suggests, shaped as the optimistic ladder the codegen committed to. `int32 op
int32` claims int32 with overflow and `-0` as side-arm territory; integral sums
and products claim the exact-integer domain the same way one level up. Nothing
here is a proof, and every result is consumed behind a guard or a fence.

That sharing is the point. The analysis's optimistic ladder and the codegen's
arm structure are the *same* rules, which is why an analysis claim and the arm
that guards it cannot disagree about what "int32 plus int32" means.

### 7.1 The alphabet and the magnitude lattice

Seven primitive bits (`INT32`, `DOUBLE`, `STRING`, `UNDEFINED`, `NULL`,
`BOOLEAN`, `BIGINT`), re-exported by both the analysis's type sets and the
codegen's contexts. Bit 14 (the analysis's unresolved-evidence marker) and bit
15 (the object-only claim) are deliberately outside the alphabet.

`Range` is a three-point chain `I32 < I53 < Top` describing the value's
magnitude *when it is a number*: whether it is known integral and exactly
representable in 53 bits. `I32` is the bottom (no evidence of anything wider);
joins take the max.

An operand is projected into a single view — its possible primitive classes, a
`wild` bit meaning "may be an object, a function, or unresolved evidence, so
`ToPrimitive` could surface anything", and its magnitude. Both type
representations project onto this losslessly for the modeled operators.

### 7.2 The interval algebra

This is where the compiler's arithmetic proofs come from.

```rust
type Iv = Option<(i64, i64, bool)>;   // lo, hi, may-be-negative-zero
```

A `Some` value asserts: the value is a finite integer in `[lo, hi]`, never `-0`,
with bounds within `+/-2^53`. Every such value — and every in-bounds
intermediate of the modeled operators — is an **exactly representable double**,
so `f64` evaluation of those operators is bit-exact integer arithmetic. `None`
means no proof.

Two properties make this a *proof* rather than a prediction:

- Every value carrying an interval traces to canonically-boxed producers:
  constants, bit-operation results, int32-tag-guarded seeds, and the compiler's
  own arithmetic. So an interval within int32 range additionally proves an
  **int32 tag**, under the engine's canonical-boxing invariant.
- The third component tracks `-0` precisely rather than conservatively: a
  product is `-0` only when one factor is 0 and the other negative; a sum is
  `-0` only when *both* addends may be; bit operations and shifts **cleanse**
  it, since `ToInt32(-0)` is `0`. Only *clean* intervals are recorded as facts,
  but a flagged intermediate may still ride an operand so that a downstream
  operation can cleanse it.

The transfer rules:

| operator | rule |
|---|---|
| `+` | `[al+bl, ah+bh]`; flagged only if both addends may be `-0` |
| `-` | `[al-bh, ah-bl]`; flag from the left operand only |
| `*` | min/max over the four corner products with checked multiplication; flagged when one factor straddles 0 and the other is negative |
| `%` | defined only for a provably non-negative, unflagged dividend and a divisor range excluding zero; result `[0, min(|b|max - 1, ah)]` |
| unary `-` | `[-ah, -al]`, flagged when 0 is in range |
| `&` | an int32 AND-ed with a provably non-negative side is `[0, that side's hi]` |
| `\|`, `^` | two non-negative int32s set no bit above the higher operand's leading bit |
| `<<`, `>>` | exact for a *constant* shift; otherwise full int32 |
| `>>>` | always `[0, 2^32)`; exact for a non-negative int32 left operand and a constant shift |
| `~` | `[-ah-1, -al-1]` |
| `/`, `**` | no rule — division is never exact-integer in any rendition |

Two **finite-height devices** sit on top, and it is worth keeping them apart
because they solve different problems:

- **Context widening** (used by the codegen's fixpoint). A slot's interval keeps
  the *exact* union for its first three growths — which is what lets a
  self-bounded loop accumulator converge to its true range instead of snapping
  past it — and only then climbs a rung ladder `0 / +/-2^31 / +/-2^36 /
  +/-2^48 / give up`. The intermediate rungs are load-bearing: real
  arbitrary-precision carry chains stabilise near `2^35`, and a ladder that
  jumps straight to `2^53` pushes the dependent sums out of the domain before
  the fixpoint can settle.
- **Heap quantization** (used by the analysis). Bounds are rounded outward to
  the next power of two (magnitudes up to 8 stay exact) at *construction*, so
  the heap interval join is a plain hull — exactly commutative, associative and
  order-independent. The ladder is clipped to the int32 domain and collapses in
  one step beyond it: a wider bound serves no consumer, wide intervals breed
  unboundedness through products anyway, and climbing twenty more octaves of
  feedback measured 2-3x the solve time on one benchmark. Masked and shifted
  chains recover a bound from the unbounded operand through the bit-operation
  rules, so nothing real is lost.

The distinction to keep: **quantization exists for finite height, not for
order-independence** (the hull join is order-independent for any fixed inputs),
which is why a runtime check may contribute an *exact* non-ladder bound.

### 7.3 Who uses it

**The analysis** projects each type set into the operand view and calls the
transfer function at the `Likely` stance for masks and magnitudes, and the
interval algebra (quantized) for values. Constant folding at scan time runs the
same algebra *unquantized*, so that a literal mask like `(1 << 28) - 1` reaches
a later `&` site with its exact bound.

**The codegen** uses the interval algebra directly as its proof engine — there
is no separate range analysis. Intervals are one dimension of the per-op BBV
context walk: they are minted by literals, by an operand's own representation,
by a passed fits-int32 check, by a passed tag guard, and by a typed-array
element kind (a proof, since the class guard pins the kind); they are propagated
by the transfer rules at every arithmetic emitter; they are joined with widening
at every merge; and they are cashed in at three places — overflow and `-0` check
elision, the narrowing of a mixed numeric mask to exactly int32 (the interval
proves the *tag*), and the prove-or-clear obligations that maintain heap range
claims.

Predictions are promoted to proofs in exactly one place: the int32 arm of a tag
dispatch seeds the analysis's predicted range as an interval, licensed by the
tag test that was just passed.

A second, *proven* stance beside this one -- for consumers that need a claim
true on every non-throwing path -- does not exist: the codegen derives its
proofs from the interval algebra instead. A future consumer needing that
stronger guarantee would add it as a second instantiation of these same
rules.
---

## 8. The runtime and the ABI

`js/src/night/runtime/`, C++, linked into `libjs` so that the wasm shell carries
it. It is the only SpiderMonkey surface generated code touches directly.

### 8.1 The helper ABI

`NightRuntime.h` is the single ABI header: flat, POD-only C, `extern "C"`, no
C++ types crossing the boundary. Each entry point is preceded by an export
macro that expands on wasm to `__attribute__((export_name(...), used))` and to
nothing natively — so the same runtime builds natively for unit tests.

Calling conventions:

- Every **may-GC** helper takes `top` as its *second* parameter: the GC scan
  limit, installed on entry. `top` doubles as the scratch out-slot — a helper
  with a boxed result writes through it, into a slot that sits exactly at the
  scan boundary and is therefore outside the rooted region.
- **Leaf** helpers, which may neither GC nor throw, omit `top`. Each such
  declaration carries an explicit justification (for example, the global-slot
  resolver uses a pure, non-allocating lookup).
- Helpers that can throw return a boolean with the result in an out-parameter.
  The two cache-miss helpers instead return a two-bit code, described below.

**The helper list is an X-macro**, and its signature strings are *derived from
the real C++ types* by a constexpr template that maps each parameter and result
type to a wasm letter. That is the load-bearing design property of the ABI:
signature drift between the C++ declaration and the wasm import is made
structurally impossible rather than asserted. There are 133 helpers.

| family | ~count | representative |
|---|---|---|
| calls and construction | 8 | `call`, `native_dispatch`, `apply_fwd`, `construct`, `create_this`, `callee_night_target` |
| property access and cache misses | 8 | `get_property`, `get_prop_ic_miss`, `set_prop_ic_miss`, `get_element`, `set_element` |
| object and array literal init | 7 | `new_object`, `new_array`, `init_prop`, `init_elem` |
| globals, names, bindings | 17 | `get_gname`, `resolve_global_slot_guarded`, `bind_name`, `get_intrinsic_cell` |
| environments and closures | 12 | `env_setup`, `get_aliased`, `push_lexical_env`, `enter_with`, `lambda` |
| arithmetic, compare, string, conversion | 19 | `add`, `binop`, `compare`, `math_unary`, `fmod`, `str_chars_eq` |
| exceptions and spec checks | 13 | `throw`, `exception`, and nine `check_*` helpers |
| generators and async | 12 | `create_generator`, `gen_suspend`, `async_await` |
| iteration | 8 | `iter`, `more_iter`, `close_iter_for_exception` |
| `super`, home object, prototype | 7 | `super_base`, `get_prop_super`, `mutate_proto` |
| `arguments` | 5 | `arguments`, `get_mapped_arg` |
| GC write barriers | 3 | `post_write_barrier`, `post_write_barrier_elem`, `pre_write_barrier` |
| misc, builtins, diagnostics | 8 | `instanceof`, `regexp`, `builtin_object` |

The generator and async family is live: the suspend/resume state machine is
lowered in `bbv/generator.rs`, and the two leaf closing checks
(`gen_closing`, `gen_is_closing`) are what the error epilogue and the
catch-pad split call.

**How helpers are bound** differs by flow and is the one real asymmetry between
them. In-process, they are wasm **imports** bound to host function pointers
(and on `wasm32` a C function pointer *is* an indirect-table index, which is
exactly what import resolution needs). In the snapshot flow, `resolve_helpers`
looks each one up as an **export of the module being rewritten** — the runtime
is already inside the image. Both produce the same handle struct for the
translator.

### 8.2 The value stack

`NightStack` is one contiguous, upward-growing array of boxed `JS::Value`, and
it is **the sole GC root for object references held by compiled code**. AOT
frames — callee, `this`, formals, locals, spilled operands, the LICM hoist
region — live here; not in the GC heap, and not in wasm locals.

It is a 2 MiB allocation owned by the runtime, present from runtime
construction with no registration step. Its size is fixed: a frame that would
not fit causes the entry point to decline and the interpreter to run the script
instead.

`[base, top)` is live and rooted. Its tracer walks exactly that range calling
the root tracer per slot, which forwards moved pointers **in place** and
no-ops on non-GC values. It is called from the context's trace hook on **every**
GC, minor and major — deliberately, because an embedding's extra-roots tracer is
major-GC-only and would leave nursery pointers in AOT frames stale.

Native code re-entering a compiled body wraps the entry in a scope guard that
saves and restores `top`. Direct compiled-to-compiled wasm calls pass `sp`
explicitly and skip the guard; `top` self-corrects at the callee's next may-GC
point.

### 8.3 Dispatch

Three entry points, all consulted from exactly three places in the interpreter
and **always after the JIT has declined**: `RunScript`, the interpreter's inline
call fast path, and generator resume.

Two runtime gates decide compiled versus interpreted: the script's AOT function
index is nonzero, and the tier has been activated. A third gate is compile-time
— the script was compiled at all.

The entry ABI is the same signature described in section 5.5, and the AOT
function index **is** the C function pointer: LLVM lowers it to an indirect
function table index, so calling it becomes a `call_indirect` of exactly that
signature. The caller stages `[callee, this, formal0..N-1]` on the value stack,
padding missing actuals with `undefined` because the body reads formals
positionally. A global body gets `[undefined, globalThis]` and zero arguments.

Return is an `i32` error code with the boxed result written through the
out-parameter. Construct semantics — substituting `this` for a non-object return
— are applied by the *caller*, because a compiled body has no interpreter frame
epilogue.

**There is no on-stack replacement and no loop side entry from the
interpreter.** Entry is only ever at a function's first op. The one thing that
resembles a side entry is generator resume, which is not OSR: it stages a
sentinel `this` and a resume descriptor above the published stack top
(unscanned, and consumed by the body before any GC can happen), and the body's
own entry dispatcher restores state. Entry is still at the physical entry
block; the dispatcher is the first thing it forks to. Because a suspended
compiled generator's saved storage layout is the AOT tier's own, such a
generator cannot fall back to the interpreter — which is why
`IsNightResumable` gates the interpreter's `JSOp::Resume` into
`EnterNightResume`.

Going the other way, compiled code calls the flat helper ABI. Generic
call-out re-enters the engine's call path; specialized sites first call a leaf
classifier that returns the callee's script and function index packed in a
64-bit word, then `call_indirect` straight into the callee, bypassing the engine
entirely.

### 8.4 GC and rooting

**Rule zero: nothing raw survives a may-GC point.** A raw JS object pointer is
never held across a call in a wasm local or in linear memory. The only durable
representation is a boxed value in a stack slot below `top`, which the tracer
forwards in place.

The **spill/reload handshake** is mechanical. Before a may-GC call: box every
live operand and store it into the frame's operand region, set `top` past them,
call. After: reload every spilled slot (the objects may have moved) and reset
each operand's representation to boxed. An error return routes to the enclosing
exception handler.

Locals are always boxed in the frame, always below `top`, hence always scanned;
the SSA carrier on top of them is a *cache*, and stores are write-through so
the frame stays the rooted truth. At every may-GC point the carrier sweep drops
exactly what a GC could invalidate: raw pointer representations always die, a
boxed carrier survives only if its fact proves it can never hold a GC thing, and
raw numeric representations are immune.

**Nothing in the reserved memory regions is traced.** The cache regions hold raw
shape, holder, prototype and callee pointers with no generation field, and are
instead **zeroed wholesale on major GC** by a registered callback. The reason is
compaction plus address reuse: a freed-then-reused shape address would
*false-hit* an otherwise sound guard. The callback runs at **both** GC begin and
GC end, and the two-sided rationale is worth keeping: at begin, so that no
pre-GC entry survives into an inter-slice mutator window of an incremental GC
(a sweep slice can free a cached-but-dead shape whose address the mutator then
reuses); at end, so that entries refilled mid-GC and then moved by a compacting
phase are also dropped.

Two regions are deliberately *not* zeroed, because they carry a per-cell
generation stamp and their hit paths re-read live state rather than trusting a
cached pointer; two more hold static tables that must survive. A second callback
handles minor-GC end: re-arm identity cells whose value was nursery-young when
resolved, and — only if some row actually cached a nursery prototype — zero the
add-transition rows.

The **helper-author contract**, as documented in the ABI header: every may-GC
helper takes and installs `top`, and writes any boxed result through it; only
helpers explicitly identified as leaves may omit it, and a leaf must neither GC
nor throw.

**Write barriers are inlined with their gate; the helper is only on the slow
edge.** The post-write (generational) barrier emits the raw store plus an
owner-tenured test, an is-GC-thing tag test, and a value-in-nursery test, all
inline, calling a leaf helper only when all three pass. It is elided entirely
when the stored value's type proves it holds no GC pointer. The pre-write
(incremental) barrier reads the zone's needs-barrier flag inline and marks the
old value only during active marking. Barrier leaves never move anything, so
they need no rooting handshake.

The engine offsets those inline barriers and the inline element access bake in
are **release-asserted at startup**: the zone and realm offsets, the elements
header offsets, the frozen flag, the function's script slot, the AOT index
field, and the object header offsets the stamp mechanism depends on. That is
the right discipline, and section 8.5 explains why its absence elsewhere is a
hazard.

### 8.5 The reserved linear-memory regions

`layout_env` (`compiler/src/wasm/mod.rs`) computes one `EnvLayout` whose bases
are **baked as absolute `i32` addresses** into every compiled body. There are
two allocation phases: a fixed-size block laid out before translation (sized
from analysis outputs the translator needs to bake addresses for), and a
post-translation block whose sizes are translation outputs — for those, bodies
bake placeholder constants that a patch pass rewrites once the sizes are known.

The base address differs by flow: the snapshot image's current memory end, or a
zero-filled arena allocated at startup.

| region | purpose | sized by | zeroed on major GC |
|---|---|---|---|
| global-binding slot rows | per binding: resolved slot entry and shape | binding count | yes |
| global value-fuse cells | baked-constant global reads: value bits plus a fuse word | binding count | yes |
| cache generation word | bumped every major GC; stamps generation-guarded cells | 4 B | (bumped) |
| AOT stack limit slot | the value the call-entry guard compares against | 4 B | no (static) |
| host-constant slots | function class pointers, the unit-string table, nursery cursor and end, boxed originals of the string char methods, fuse word addresses, the Array class pointer | fixed | partly re-armed |
| builtin identity cells | boxed bits of each pristine builtin | 24 cells | re-armed |
| typed-array class table | the fixed-length typed-array class pointers by kind | 9 + pad | no (static) |
| arguments metadata | mapped/unmapped class pointers and the data offset | 16 B | no (static) |
| string-literal block | the empty string plus replay triples | 32 B | partly |
| per-layout add-check bounds | the static bound the add check consults | layout count | **no, deliberately** |
| gname fuse words | one per fused constant global | fused count | no |
| megamorphic get table | 8192 x 24 B, direct-mapped | fixed | yes |
| megamorphic set table | 8192 x 16 B | fixed | yes |
| Math native slots | `JSNative` addresses, clone-proof callee matching | 16 | no |
| dense-append cache | 512 x 32 B, shape-hashed | fixed | yes |
| accessor-call cache | 2048 x 32 B | fixed | yes |
| per-site property cache | one 20 B way plus a 48 B add-transition row, 68 B stride | site count | yes |
| callee value cells | 16 B per call site, plus a shared trash row | site count | yes |
| inline-allocation cells | 32 B per literal site | site count | yes |
| `instanceof` cells | 16 B per site | site count | no (generation-stamped) |
| construct-`this` cells | 40 B per specialized `new` site | site count | no (generation-stamped) |
| intrinsic value cells | 8 B per distinct intrinsic name | name count | yes |
| constructor slot-count table | per function index: `this` slot count and stamp key | table size | no (static) |
| serialized side tables | atom, binding, layout, fuse and regex tables | content | no (static) |

#### The mirroring hazard, stated plainly

**The region layout is mirrored by hand across Rust and C++, and the
synchronisation mechanism covers only part of it.**

What *is* synchronised: the roughly 28 region **base addresses and lengths**
travel as a serialized descriptor — a word array written by the compiler and
decoded on the C++ side. In the snapshot flow that array is `memcpy`'d into the
descriptor struct behind a `static_assert` that the two are the same *size*; in
the in-process flow it is decoded positionally, field by field, with only a
length floor as a guard.

That mechanism catches a size change. It does **not** catch a reordering, and
one field in the descriptor is explicitly a dead slot kept only so the `memcpy`
stays aligned — which is the clearest available evidence that this is a
positional wire format, not a struct.

Every *intra-region* offset (the host-constant slots, the typed-array class
table's position, the arguments metadata block, the string-literal slot; two
of the region bases are not even in the descriptor, and C++ recomputes them
from another base) and every *entry shape and table size* (the cache way
count and stride, both megamorphic table sizes, the append and accessor cache
row counts, the builtin cell count) must agree between the compiler and the
runtime. A mismatch there is caught by nothing at the point of use: the
compiled body would index with one stride and the runtime populate with
another. That is a silent miscompile class, and it is the one place in this
system where the "guards make it safe" argument does not apply, because the
guard itself would be reading the wrong address.

`runtime/NightRegionShape.h` holds `NIGHT_REGION_SHAPE`, an X-macro of every
one of those constants. C++ takes `Night_*` constants from it and
`static_assert`s its derived forms against them (the IC stride is ways ×
way-bytes plus the transition row; `sizeof(MegaGetEntry)` is the entry size);
the three block bases C++ recomputes off `propicGenPtr` are `constexpr`
functions in that header, so the arithmetic exists once. `compiler/build.rs`
parses the same macro into `crate::region_shape`, and `translate.rs`,
`wasm/mod.rs` and `bbv/abi.rs` read the generated constants instead of their
own literals.

What remains: the region **descriptor** is still a positional wire format
(`NIGHT_ENV_REGIONS` keeps the two sides' field lists in step by generation,
but the wire is ordered words), so a change to the shape list is an ABI
change and wants a `NightAotAbiVersion` bump. The header says so.

### 8.6 Registration and snapshot capture

`NightRegistration.cpp` owns the in-memory contract between an engine embedded
in a wasm module and the external transform tool. It defines the singleton
registration block, which the tool locates through an exported
constant-returning function; builds the **layout descriptor** (every engine
field offset and flag constant the external reader will dereference) by
expanding the X-macro; eagerly delazifies a registered root's whole function
tree; and builds a **digest** of facts a raw cell read cannot soundly derive —
per-script gcthing trace kinds and per-scope binding lists. Compacting GC
stays on: `NightSealSnapshotAddresses` re-derives every recorded address from
the rooted copies once, after the last GC before the snapshot is sealed, so
the addresses it records are already post-compaction. At run time raw GC
addresses live only in slots the AOT stack traces, and the linear-memory
caches that hold raw cell addresses are purged around compacting slices
(`NightPurgeMovableCaches`).

Its resume-side counterpart installs the environment and then applies fuse
policy: if *any* script stayed interpreted, all global value fuses are
distrusted permanently, because interpreted global writes bypass the compiled
fuse hooks.

`NightSnapshotExtras.cpp` captures what the analysis needs but the reader cannot
recover from a raw image: the irregexp **bytecode of every regex literal**,
force-compiled in both subject encodings; a **self-hosted allowlist** (22 named
builtins) resolved against the live global and delazified; and the **heap
oracle** — a full GC to tenure everything, then a transcription of the live
post-setup object graph reachable from the global and from script gcthings (own
data properties, dense elements, prototype links) plus per-scope environment
slot values read out of live call objects. That last part is what lets the
analysis see closure-captured state at all. Every entry records the object's
class pointer, so a freed-and-reused address re-reads as opaque.
---

## 9. The two flows

One compiler crate consumes one input — a `Source` object graph — and emits
waffle function bodies. Two drivers wrap it. The divergence is entirely in *how
the `Source` is obtained* and *how the emitted functions become callable code*.

### 9.1 The snapshot flow

This is the shipping flow.

```
program.js
   |  the wasm shell reads it on stdin during wizer's init phase
   v
wizer --init-func wizer.initialize -r _start=wizer.resume
   |  instantiates the shell, runs init, snapshots linear memory and globals
   |  back into the module's data segments
   v
snap.wasm            (a SpiderMonkey-in-wasm image with the program's heap in it)
   |
   |  nightmonkey snap.wasm -o out.wasm      (a host-native binary)
   v
out.wasm             (the SAME module, plus compiled bodies, plus a rewritten
   |                  memory image)
   |  optionally: wasmtime compile
   v
wasmtime run
```

During wizer's init phase the shell forces **full parse** (no lazy functions)
and registers the top-level script. Registration delazifies the whole reachable
function tree, records the layout descriptor and digest, captures the regex
programs and the self-hosted allowlist, and — after the top level executes —
transcribes the live heap (section 8.6).

The transform tool then, in order: parses the module; flattens all active data
segments into one memory image; locates the registration block through an
exported address function; reads it (checking the ABI version); **walks** the
image into a `Source`; lays out the reserved regions starting at the current
image end; resolves the ~140 helpers as exports *of this same module*; compiles
every script; serializes the side tables and appends them to the image; stamps
each compiled script's AOT function index **into the memory image**; writes the
region table and sets the compiled flag; re-derives data segments from the
mutated image; and serializes.

The produced artifact is **a plain `.wasm` module** — not a `.cwasm`. Running it
under `wasmtime` is a separate step, and precompiling it with `wasmtime compile`
is an optional one.

Two mechanics are worth pinning down because they are easy to get wrong from the
outside:

- **Nothing is "linked" in the ordinary sense.** Calls into the runtime are
  direct calls to the module's own exports; calls into compiled JS go through
  the C function-pointer table, and the AOT function index *is* a table index.
- **The walker reads raw linear memory**, reconstructing objects, scripts,
  strings and scopes from cell layouts described by the generated layout
  descriptor. Trace kinds come from the digest rather than from memory, because
  they cannot be soundly derived from a cell read. Objects the heap oracle does
  not cover stay opaque.

### 9.2 The in-process flow

This is the test vehicle and the debugging lane. The compiler crate is
additionally compiled for `wasm32-wasi` and **linked into the shell itself**, so
one `js` invocation compiles and runs its own script — no external tool, no
wizer.

The host is `wasm-jit-runner`, a small wasmtime CLI with one trick: a host
import that lets the running guest **add new wasm functions to itself and call
them**. At startup it rewrites the guest module so every memory, table and
global is exported under a synthetic name, and strips every table maximum so the
function-pointer table can grow. Compiled modules are content-addressed and
cached.

Three host calls: query the current table size, and two forms of "add these
function blobs". Appended functions are contiguous, so the guest can *predict*
that blob *i* lands at `size + i` — an explicit API guarantee, and the driver
verifies it after the fact.

The guest side registers its root exactly as the snapshot flow does, walks its
**own live heap** with the same walker (over raw pointers instead of image
bytes), builds a batch, injects the blobs, copies the string-literal blob into
its reserved region, installs the environment, and stamps each compiled script's
AOT index on the live script object.

On the compiler side the differences are contained: the module is built fresh
rather than mutated, helpers are **imports** rather than exports, the funcref
table is pre-padded so blob indices are predictable, region memory comes from a
caller-supplied allocator called exactly twice, and after translation the module
is serialized and **carved** into per-function blobs in the runner's format.
Structural index assertions guard the whole scheme.

Two policy differences are worth knowing:

- Global value fuses are **always** distrusted in this lane, because there is
  always interpreter coverage.
- Every gate is per-script, as in the snapshot lane: a generator or async
  body does not decline the whole batch. Frame-introspection tests that need
  to skip AOT compilation opt out with `skip-if: nightTierEnabled()` instead.

Compile time is inside the measured run in this lane, so it is not
perf-comparable with the snapshot lane on short workloads.

### 9.3 What is shared

Byte-for-byte the same code in both flows: the `Source` graph and its FFI
builders; the bytecode model and the generated opcode enum; the whole `likelier`
analysis; `layout_env`; the helper-resolution shape; **`bbv::translate_script`,
the codegen**; the whole-tree compile loop; the regex compiler; every table
serializer; **the heap walker** (only its memory accessor differs); the C++
runtime; and registration.

Divergent: where the compiler runs, how memory is read, whether the module is
mutated or built, how helpers are bound, where region memory comes from, how
bodies become callable, where the AOT index is stamped, the environment
descriptor's shape, the fuse policy, the generator/async granularity, and
whether anything is persisted.

### 9.4 Build integration

The runtime and the host binary are ordinary build directories; the host binary
is a workspace Rust program built for the *build host*, while the shell targets
`wasm32-wasi`. The in-process lane additionally adds the compiler crate as a
feature of the shell's Rust library, which is what forces its FFI symbols to be
linked. The test runner is a deliberately out-of-workspace cargo project — its
wasmtime-class dependency tree stays out of the main workspace — driven by a
forced build step, with cargo owning incrementality.

Two build scripts generate code, and they are the model for cross-language
safety in this project:

- the compiler's `build.rs` parses SpiderMonkey's `Opcodes.h` directly and emits
  the `JSOp` enum with per-op lengths and stack effects;
- the snapshot crate's `build.rs` parses the layout X-macro out of the runtime
  header and emits the Rust field enum and ABI version, with a rerun trigger, so
  the Rust mirror cannot drift from the C++ header.

---

## 10. The regex compiler

`compiler/src/wasm/regex.rs`. It translates **irregexp bytecode** — the
interpreter ISA, force-compiled by the engine at AOT time — into wasm matchers.
One bytecode program becomes one standalone wasm function equivalent to a single
`RawMatch` activation: one match attempt, on a flat subject already in linear
memory. The engine keeps the global-match loop, string flattening and interrupt
handling.

Everything is compiled **twice**, once per subject encoding, so character-load
width is a compile-time constant.

**The signature** is six `i32` parameters (subject pointer, length, start
position, output register pointer, backtrack stack base and capacity) returning
a status: failure, success, or **retry**. Retry means "I gave up; run the
interpreter", and it is the fallback channel for everything the matcher will not
do.

**There is no register array in memory.** The whole machine state — current
position, current character, stack pointer, backtrack count, and every irregexp
register — is an SSA vector threaded as block parameters, which waffle's
localifier turns into wasm locals. That is why the register count is capped:
every leader block carries one parameter per register.

**It is not a dispatch loop.** A first pass linearly decodes the bytecode with a
fixed length table, collects *leaders* (offset 0 and every branch target),
validates every label, and assigns dense ids to backtrack targets. A second pass
walks the instructions in order emitting one block per leader, with every
conditional check lowered to a `CondBr` whose taken edge targets the named
leader carrying the whole state vector.

**Backtracking** is the interesting part. The backtrack stack is a
caller-provided `i32` buffer; the stack pointer is an SSA state slot. irregexp
pushes *code offsets*, which wasm cannot branch to. The solution:

1. a backtrack push pushes a **dense label id**, not a byte offset;
2. a pop branches unconditionally to a lazily created **dispatcher block**,
   passing the id and the state;
3. the dispatcher is terminated with a `Select`, which waffle lowers to a
   `br_table`, each target re-supplying the whole state vector;
4. the "push current position" and "push register" opcodes push values
   *untouched*, so the save/restore of the stack pointer into a register is
   oblivious to the id substitution.

**Character classes** are baked into the code: a bit table becomes two `i64`
constants tested branch-free with a shift and a mask-select. No memory table, no
branch.

**Fallbacks** are of two kinds. *Compile-time* bails drop that encoding's
variant (bytecode too large or misaligned, too many registers, an invalid
opcode, an out-of-range or misaligned label, a jump to a non-leader, too many
backtrack labels, a four-character load in a wide program, a case-insensitive
backreference on a two-byte subject with no comparison helper available).
*Run-time* bails return retry: backtrack stack overflow, a backtrack budget of
2^27 exceeded, the explicit break opcode, an unmatched dispatcher id, or running
off the end.

Two deliberate divergences from the interpreter: the packed return-code operand
of the pop opcode is ignored (safe, because the engine hard-codes no backtrack
limit), and **there is no interrupt check** — the interpreter polls at every
backtrack pop; here only the budget bounds a runaway match.

**Calling in.** The engine resolves a matcher **once per compiled regex**,
caching the result on the shared regex object: a linear scan comparing flags and
pattern text against the published descriptor table, with a sticky negative.
Then the table index is cast straight to a function pointer and called under a
no-GC guard, with the flat characters and the match-pairs array passed directly
— so **captures come back with zero marshalling**, written straight into the
engine's own pairs vector. A retry return makes the caller rerun normally.

The highest-risk surface in the file is the hand-inlined Latin-1 case folding
used by case-insensitive backreferences: it reimplements the engine's table
rather than calling into it, and the file has no tests.

---

## 11. Limitations

Some of these are deliberate design rulings; some are gaps. They are separated
below, because a reviewer should not have to guess which is which.

### 11.1 By design

**No profiling input, ever.** Stated in full in section 1. The compiler works
from bytecode plus snapshot state — program *state*, not an execution trace. No
count-based or first-seen dynamic recording may feed a compile-time decision.
Where static analysis and heuristics reach their limit, the answer is to stop.
The practical cost is real: several places in this document describe a heuristic
that a profile would answer directly. That is accepted.

**No debugger.** There is no decline for debugger presence anywhere, and the
consequence is that a `debugger;` statement in compiled code **compiles to
nothing** — it is in the no-op group with `Nop` and `JumpTarget`. That is a
silent semantic loss, not a decline. Debug-specific opcodes are declined or
folded into their non-debug equivalents. Observability is preserved only by
the coarse whole-batch decline in the test lane.

**No frame introspection.** AOT frames are not interpreter frames or JIT frames.
There is no per-frame script/pc/callee descriptor, nothing registers with the
frame iterators, and no frame iterator is referenced anywhere in the runtime.
Compiled frames are invisible to `Error.stack`, the profiler, and the debugger,
by design.

**No interrupt checks.** `LoopHead` compiles to nothing, so **back edges never
poll**. There is no interrupt helper in the ABI at all, and no interrupt check
anywhere in the tier. An infinite loop in compiled code cannot be interrupted;
there is no slow-script or watchdog path through this tier. What does exist is
an AOT-stack-overflow guard on arms that enter another compiled body directly.

These three are ratified together: the test suites carry roughly 170 skip
directives keyed on the tier being enabled, dominated by the debugger,
saved-stacks and profiler families.

**A closed world.** The compiled set is fixed at registration: only what is
syntactically reachable from a registered root (at most 8 roots) is delazified
and compiled. Therefore **`eval`, `new Function`, dynamically loaded scripts and
other realms are never compiled**. Self-hosted builtins are compiled only from a
22-entry allowlist; everything else in the self-hosted realm runs interpreted.

**No partial compilation.** A script is compiled iff every *reachable* op in it
is translatable. There is no per-pc bail, no mixed frame, and no state in which
half a body is compiled.

### 11.2 Scripts that stay interpreted

Every production skip site, exhaustively:

| reason | granularity |
|---|---|
| bytecode over 128 KiB | per script |
| generator or async body that uses `arguments` | per script |
| an unsupported environment shape | per script |
| an unsupported opcode anywhere reachable | per script |
| the emitted body exceeds 300k SSA values after the full compile-ladder descent | per script |

The environment gate declines aliased variables under a body scope that is not a
function or global scope — so a closure-using module, eval, non-syntactic or
lexical body scope is refused — and named-lambda environments.

The unsupported-opcode set is 17 opcodes:

- **`Resume`** (1) — the `yield*` driver's half of the resume protocol, which
  a compiled body would have to run on the *caller* side; delegating
  generators therefore decline;
- **`eval`** (4 forms);
- **ES modules** (3): dynamic import, `import.meta`, module imports;
- **explicit resource management / `using`** (3);
- **self-hosting internals** (3);
- **miscellaneous** (3): BigInt literals, non-syntactic global `this`,
  environment callee.

Note what *is* supported, since the list is shorter than one might guess: `with`,
full try/catch/finally with try-note-driven handler routing, for-in and the
iterator protocol, classes, accessors, private fields, and mapped `arguments`.

One nuance: the driver is a reachability workqueue seeded from pc 0, so an
unsupported opcode in **statically unreachable** bytecode does not decline the
script. The structural pre-gates (environment ops, `arguments`, `new.target`) are
linear scans over all bytecode and therefore do fire on unreachable code.

### 11.3 Quality cliffs that degrade silently

These compile, but worse:

- the context fixpoint exceeding its round cap **discards every context fact**
  in the body (with a loud warning; convergence is guaranteed, so a firing is a
  bug to chase);
- the compile ladder's middle and bottom rungs produce progressively less
  specialised code;
- the inline splice allowlist is much narrower than the compiler's op coverage,
  so many callees decline;
- whole-module BigInt-freedom — which enables tighter numeric masks everywhere
  — is lost if *any* script in the bundle mentions `BigInt`, a BigInt typed
  array, or `eval` (runtime-compiled source does not cost the claim; it is
  handled by the inline fuse test instead, and only degrades a run in which it
  actually happens);
- an irreducible residual edge keeps the body compiled but **disables LICM** for
  it.

### 11.4 Constraints the tier imposes on the engine

- **A compiled frame may hold raw GC addresses only in AOT-traced stack
  slots**; every other cache of a raw cell address (property ICs,
  megamorphic rows, resolved gname rows, and the rest) is purged around a
  compacting GC's slices (`NightPurgeMovableCaches`), and the snapshot's own
  recorded addresses are re-derived from rooted copies once the last
  shrinking GC before sealing has run.
- **32-bit address space only.** Every helper takes a `uint32_t` linear-memory
  offset identity-mapped to a host pointer, and the stamp word exists only
  because the 32-bit object header has padding to spare.
- **Global value fuses are dead** whenever any script stayed interpreted, and
  always in the test lane.
- A compiled non-syntactic-scope script would crash rather than degrade; the
  entry point refuses such scripts, and three helpers assert on their absence.
---

## 12. Rough edges

This is an engineering document, so this section exists. Everything here was
found by reading the current source. Items are grouped by what a reviewer should
do about them.

### 12.1 Correctness risks

**A suspected off-by-one in the early-key mask.** The stamp's early class key is
12 bits on the Rust side (bits 18..29), but the C++ add check extracts 13 bits
(18..30), which overlaps the RANGES bit. On a constructing object that still
carries RANGES — which is every object the allocation site seeds — the extracted
key reads high, fails the layout bound check, and **clears SLOTS spuriously**.
Exposure is narrowed by call ordering (two of the three call sites run after a
store that has already dropped RANGES), but the property-add path runs before
any store. The direction of the error is safe — it over-clears, never
over-claims — so this is a coverage and performance bug rather than a soundness
bug. It looks like a leftover from the pre-RANGES 13-bit key design. Found by
reading; not reproduced.

**The regex compiler's Latin-1 case folding is hand-inlined.** Case-insensitive
backreferences on a Latin-1 subject reimplement the engine's folding table
rather than calling into it, and `regex.rs` has no tests. The opcode length
table is likewise hand-copied from the engine header with no cross-check.

**Prototype mutation does not clear the stamp.** This is sound only because
every stamp claim is about *own* fixed slots. It is an invariant with no check
behind it, and it deserves either a comment at the mutation sites or a hook.

**A residual irreducibility of about 1%** of scripts in one large benchmark: a
retreating edge to a non-dominating target, root cause unknown. Such bodies are
still compiled (the backend handles them) but **LICM is disabled** for them.
This is tracked in the source and is an open item.

### 12.2 Stale source comments that will mislead a reader

The code moved faster than its own prose in several places. Anyone reading these
files should know:

- **`translate.rs`'s module header describes the deleted per-op translator.**
  The file is now a substrate of shared types, constants and scans, plus a shim
  into `bbv.rs`; from a certain point on it is tests.
- `lib.rs` and `view.rs` refer to an intraprocedural type prepass. That module
  was removed; the codegen derives its proofs from the interval algebra.
- The `README` refers to `wasm/intra.rs`, `wasm/relooper.rs`, a vendored
  `waffle/` directory, a `tests/` corpus and two smoke scripts. None of those
  paths exist any more; waffle is a normal crates.io dependency, and structured
  control flow is reconstructed by waffle's own passes.
- `bytecode.rs` claims mapped-`arguments` scripts stay interpreted. They do not;
  the flag only blocks inlining and some optimisations.
- The ABI header says the pre-write barrier is unnecessary because incremental
  GC is disabled. The emitter emits the gate anyway. One of the two is wrong and
  it is worth resolving which.
- One store-side discipline is documented as defaulting off; it defaults on.

### 12.3 Dead or unconsumed machinery

Not harmful, but it is weight, and a reader will otherwise waste time on it.

- The runtime layout validator survives only as a diagnostic hook; the
  production validator is emitted inline.
- **The prediction's transfer function has not been extracted from the
  emitter.** The prediction is now a declared pass with a declared output
  (`bbv/predict.rs`), it runs one fixpoint, and `Code` is a consult-only
  consumer of it — but the transfer is still *computed by* running `emit_op`
  with the IR primitives suppressed (`EmitMode::ContextOnly`). Writing an
  independent abstract semantics for the opcode set, family by family, is the
  remaining work. Until it exists, `EmitMode` names two ways to run one body
  of code rather than two bodies that can disagree; and `strip_all` plus the
  closure check stay as the release-mode net for exactly that.
- `iv_grow` is fixpoint metadata riding in the prediction. It carries no claim
  and should not survive the transfer function's extraction.

### 12.4 Known imprecision, deliberate or otherwise

- **Numeric write-back from guards is inert.** The provenance mechanism that
  makes a proven fact durable in a lineage writes back **class facts only**;
  every numeric write-back call returns immediately. This was measured
  (re-typing a context slot hands out a different carrier representation and
  pays a conversion on every edge), but the surrounding comments read as though
  a passed tag guard makes the next arithmetic op on that slot checkless. It
  does not.
- **`ToBoolean` has no `F64` fast case**, so an unboxed double at a branch pays
  a canonicalising box plus the entire five-way tag ladder including a
  statically-true branch. It also never consults the operand's interval.
- **The `length` arm never types its int32 result**, even though the string,
  array and arguments paths all just produced a proven int32, so every consumer
  of `.length` re-tests the tag. There is no comment defending this.
- **Emitting the set cache kills every live SLOTS fact in the body**, including
  when the add-transition arm is not emitted and no add can occur — more
  conservative than the stated justification requires.
- **Dense element stores clear the object's TYPES and RANGES bits** through the
  shared engine store check, even though those bits describe named slot fields.
  Sound, but array-heavy code drops the header claim on every non-number element
  write.
- The bit-operation slow arm is the only arithmetic slow arm that does not carry
  the result interval forward, which drops the slot's fact at the successor join
  for every lineage. No comment justifies the asymmetry.
- The analysis's callable-set cap silently drops identities past 8; the record of
  what was dropped is kept and never consumed.
- The prototype chain walk stops at depth 8 without recording that it truncated.

### 12.5 Where the complexity genuinely lives

Three mechanisms are intricate enough that a change to them needs real care, and
the document has tried to explain rather than summarise them:

- **`theta` and the token discipline** (sections 4.6-4.9). The reducibility
  argument is a chain of four structural properties, and the residual 1% shows
  the chain is not yet airtight.
- **The inline splice frame layout** (section 5.7). A child frame laid down
  inside the caller's operand region, with a hand-inlined prologue, a fact
  transfer in both directions, and an exception rule that depends on two
  admission refusals holding simultaneously.
- **The stamp bits** (section 2.2). Three independent validity claims on one
  word, maintained by two engine hooks and one compiled twin, with a
  construction sentinel sharing the same bits.

Of these, the stamp is the one whose invariant is most load-bearing and least
locally checkable, which is why it is documented first in this document rather
than last.

---

## 13. Glossary

| term | meaning |
|---|---|
| **arm** | one branch of a lowering's dispatch. Ends either in a continuation to the successor pc under its own facts, or in a merge inside the same version |
| **BBV** | basic-block versioning: compiling one code version per (pc, abstract state). Here the two halves are separated: a *block* is `Ver { pc, class, track, depth }`, and the abstract state is a *prediction* keyed by pc alone |
| **carrier** | an unboxed SSA value that crosses version boundaries as a block parameter, instead of being reloaded from the frame. Not the same as `Ctx::carried`, which is *which* locals ride the edge, in whatever representation |
| **prediction** | the fact context at a program point: what the analysis says holds there, which codegen enforces (section 4.2). One per pc on Opt; GEN has none |
| **class key** | the analysis's numbering of a discovered class; `key + 1` is what a stamp's low half holds |
| **clean miss** | a cache miss the runtime served without running user code, allocating or reshaping, so the caller rejoins its happy-path lineage |
| **context (`Ctx`)** | the abstract state a version is compiled under: per-slot facts, tokens, carrier sets and track |
| **continuation** | an edge from a lowering to a successor pc, routed through the merge point |
| **effect word** | the two-bit provenance value a compiled body returns describing what it disturbed |
| **fenced hierarchy** | the four-width ladder of property facts, all discharged by one guard form |
| **fuse** | a memory word that *is* a soundness guard: baked-constant reads test it, and any write through the engine blows it |
| **likely fact** | anything the analysis produces; always a prediction, never trusted |
| **on-ramp** | a guard chain letting a degraded lineage re-enter a loop header's optimised version by re-proving its context |
| **quiet alloc** | a helper that may GC but writes no pre-existing user-visible heap, so it sweeps raw pointers only |
| **segment** | a spliced callee's synthetic pc range above the root script's bytecode |
| **side entry** | an edge into a loop's interior that bypasses its header |
| **splice** | inlining, done by mapping the callee's bytecode into the caller's pc space rather than by importing a CFG |
| **stamp** | the 32-bit class-and-validity word in the object header (section 2.2) |
| **token** | a per-enclosing-loop layer marker in a context; part of version identity, never a fact |
| **track** | `Opt`, `Side` or `Dirty`: how control reached this version. Part of version identity; only ever descends |
| **version** | one compiled copy of one pc, identified by `(pc, token class, track, depth)` |
