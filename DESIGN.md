# Mons Lang — Design Plan

A detailed specification of the language design, architecture, and implementation strategy.

---

## Table of contents

- [Identity and goals](#identity-and-goals)
- [Type system](#type-system)
- [Mutability model](#mutability-model)
- [Data model — structs and traits](#data-model--structs-and-traits)
- [Functional features](#functional-features)
- [Error handling](#error-handling)
- [Metaprogramming](#metaprogramming)
- [Keyword reference](#keyword-reference)
- [Implementation pipeline](#implementation-pipeline)
- [Implementation phases](#implementation-phases)
- [Phase 2 completion checklist](#phase-2-completion-checklist)
- [Phase 2 closed scope (complete)](#phase-2-closed-scope-complete)
- [File structure](#file-structure)
- [Design decisions log](#design-decisions-log)

---

## Identity and goals

Mons is a statically typed, functionally-oriented general purpose language written in C. It is designed around a small set of firm convictions:

- **Functional over OOP** — the default mental model is functions transforming data, not objects sending messages. OOP constructs exist but are not the idiomatic path.
- **Expressions over statements** — `if`, `match`, and blocks all return values. This enables composition without intermediate variables.
- **Immutability over mutation** — all bindings are immutable by default. Mutation is an explicit, local opt-in.
- **Composition over inheritance** — structs hold data, traits define behaviour, `impl` attaches behaviour to data. This is the complete picture. Inheritance is available but discouraged.
- **Metaprogramming at the right layer** — three distinct levels of metaprogramming with different power/safety tradeoffs: AST macros, runtime reflection, compile-time codegen.
- **Zero dependencies** — the implementation is written in C with no external libraries. Portable and embeddable by design.

Mons is not trying to be Rust, Haskell, or Go. It is its own thing: approachable enough to get productive quickly, principled enough to reward good design, powerful enough for real systems work.

---

## Type system

### Style

Hindley-Milner style inference. Types are always fully known at type-check time, but annotations are optional in most positions. The checker resolves types from usage context — you annotate where you want to be explicit, not where the compiler can't figure it out.

```mons
let x = 42          // int, inferred
let y: double = 1.5 // explicit
fn add(a: int, b: int) -> int { a + b }  // params annotated, return explicit
fn double(n: int) { n * 2 }              // return type inferred as int
```

### Primitives

| Keyword | Description |
|---|---|
| `int` | 64-bit signed integer |
| `float` | 32-bit IEEE 754 floating point |
| `double` | 64-bit IEEE 754 floating point |
| `bool` | `true` or `false` |
| `string` | UTF-8 string, immutable value type |

### Built-in generic types

| Type | Meaning |
|---|---|
| `Option[T]` | A value that may be absent — `Some(v)` or `None` |
| `Result[T, E]` | A computation that may fail — `Ok(v)` or `Err(e)` |
| `[T]` | Array of T |
| `(A, B, ...)` | Tuple — fixed-length, heterogeneous |
| `fn(A, B) -> C` | First-class function type |
| `&T` / `&mut T` | Reference types (Phase 2+) |

### Generics

Parametric generics with optional trait bounds. Bounds use `:` and can be combined with `+`.

```mons
fn identity[T](value: T) -> T { value }

fn max_of[T: Comparable](a: T, b: T) -> T {
    if a > b { a } else { b }
}

fn zip[A: Clone, B: Clone](a: [A], b: [B]) -> [(A, B)] { ... }
```

Generics on structs and impl blocks follow the same syntax:

```mons
struct Pair[A, B] { first: A, second: B }

impl[A, B] Pair[A, B] {
    fn swap(self) -> Pair[B, A] {
        Pair { first: self.second, second: self.first }
    }
}
```

### Algebraic data types

Mons does not have a separate `enum` keyword in Phase 1. ADTs are expressed through `Option` and `Result` plus `match`. A dedicated `enum`/`variant` keyword is a candidate for Phase 2.

---

## Mutability model

All bindings are immutable by default. `mut` is the explicit opt-in and applies only to local bindings — never to function parameters. This keeps function signatures honest about side effects.

```mons
let x = 10          // immutable — cannot be reassigned
let mut y = 10      // mutable — can be reassigned
y = 20              // ok

fn increment(n: int) -> int { n + 1 }   // n cannot be mut — enforced
```

Struct fields default to immutable. A field marked `mut` can be updated on a `mut` binding of that struct:

```mons
struct Counter {
    mut count: int,
    label: string,      // immutable field
}

let mut c = Counter { count: 0, label: "hits" }
c.count = c.count + 1  // ok — field is mut and binding is mut
// c.label = "x"       // error — field is not mut
```

---

## Data model — structs and traits

### Structs

Structs are the primary data container. They hold named, typed fields. No methods live directly on the struct definition — methods are attached via `impl` blocks.

```mons
pub struct Point {
    x: float,
    y: float,
}
```

### Impl blocks

`impl` blocks attach methods to a struct. Methods receive `self` as their first parameter (by convention — `self` is not a keyword but a strong convention).

```mons
impl Point {
    fn distance(self, other: Point) -> float {
        let dx = self.x - other.x
        let dy = self.y - other.y
        sqrt(dx * dx + dy * dy)
    }

    // associated function (no self) — called as Point::origin()
    fn origin() -> Point {
        Point { x: 0.0, y: 0.0 }
    }
}
```

### Traits

Traits define a set of method signatures a type must implement. They can include default implementations.

```mons
trait Shape {
    fn area(self) -> float          // required
    fn perimeter(self) -> float     // required

    fn describe(self) -> string {   // default implementation
        "area=" + to_string(self.area())
    }
}
```

A struct implements a trait via an `impl ... for ...` block:

```mons
pub struct Circle {
    center: Point,
    radius: float,
}

impl Shape for Circle {
    fn area(self) -> float      { 3.14159 * self.radius * self.radius }
    fn perimeter(self) -> float { 2.0 * 3.14159 * self.radius }
}
```

A struct can implement any number of traits. Trait bounds on generics ensure a type implements the required interface.

### Inheritance

Struct-to-struct inheritance is supported via an `extends` clause. It is available for cases where it genuinely fits, but the language actively makes composition feel more natural — the idiomatic path is to compose structs and delegate via trait implementations.

```mons
struct Animal {
    name: string,
}

struct Dog extends Animal {
    breed: string,
}
```

---

## Functional features

### First-class functions and closures

Functions are values. They can be passed, returned, and stored. Closures capture their enclosing environment by value.

```mons
let double = |x: int| -> int { x * 2 }
let add    = |a, b| a + b              // types and return inferred

fn apply(f: fn(int) -> int, x: int) -> int { f(x) }

apply(double, 5)   // 10
apply(|n| n * n, 4) // 16
```

### Higher-order functions

The standard library provides the usual functional toolkit over arrays:

```mons
let numbers = [1, 2, 3, 4, 5]
let evens   = filter(numbers, |n| n % 2 == 0)
let squared = map(evens, |n| n * n)
let total   = fold(squared, 0, |acc, n| acc + n)
```

### Pattern matching

`match` is exhaustive — the compiler ensures every case is covered. Patterns can be nested, guarded, and combined with `|`.

```mons
let value: Option[int] = Some(42)

let result = match value {
    Some(n) if n > 0 => n * 2,
    Some(_)          => 0,
    None             => -1,
}
```

Patterns supported:

| Pattern | Example |
|---|---|
| Wildcard | `_` |
| Literal | `42`, `true`, `"hello"` |
| Binding | `x`, `mut x` |
| Enum variant | `Some(x)`, `None`, `Ok(v)`, `Err(e)` |
| Struct | `Point { x, y }` |
| Tuple | `(a, b)` |
| Array | `[head, tail]` |
| Or | `200 \| 201 \| 204` |
| Guard | `n if n > 0` |

### Block as expression

Any block returns the value of its last expression. This means `let`, `if`, `match`, and function bodies all compose uniformly.

```mons
let label = {
    let base = compute_score()
    if base > 90 { "A" } else { "B" }
}
```

---

## Error handling

Mons supports two error handling styles. They are not in conflict — the language simply provides both.

### Result + match (idiomatic)

`Result[T, E]` is the functional-idiomatic approach. Errors are values, propagation is explicit, and `match` makes handling exhaustive.

```mons
fn divide(a: int, b: int) -> Result[int, string] {
    if b == 0 { Err("division by zero") } else { Ok(a / b) }
}

let answer = match divide(10, 2) {
    Ok(val)  => val,
    Err(msg) => { println("error: " + msg); 0 }
}
```

### try / catch / finally

Available for ergonomics, interop with native code, and situations where propagating `Result` through a deep call stack would be noisy.

```mons
try {
    let data = read_file("config.json")
    let cfg  = parse_json(data)
    apply_config(cfg)
} catch (e: IoError) {
    println("io error: " + e.message)
} catch (e: ParseError) {
    println("parse error at line " + to_string(e.line))
} finally {
    cleanup()
}
```

`finally` always runs — whether the `try` block succeeded, threw, or was caught.

---

## Metaprogramming

Three layers, applied at different stages of the pipeline. Each has a different power/safety tradeoff.

### Layer 1 — AST macros

Macros operate on the AST before type checking. They are the most powerful layer: they can generate new syntax, build DSLs, unroll patterns, and produce code that could not be written by hand without repetition. Macro expansion is a distinct phase in the pipeline.

```mons
macro assert!(expr) {
    if !expr {
        panic("assertion failed: " + stringify!(expr))
    }
}

macro vec![...elems] {
    { let mut v = Vec::new(); push_all!(v, elems); v }
}

// usage
assert!(x > 0)
let v = vec![1, 2, 3]
```

Macros are hygienic — names introduced inside a macro do not leak into the caller's scope.

### Layer 2 — Runtime reflection

Reflection happens at runtime. A program can inspect its own types, struct field names and types, and trait implementations. This enables serialization, dependency injection, ORMs, and similar patterns without code generation.

```mons
let fields = reflect::fields_of(Point)
for field in fields {
    println(field.name + ": " + field.type_name)
}

let type_name = reflect::type_name_of(some_value)
let implements_shape = reflect::implements[Shape](some_value)
```

### Layer 3 — Compile-time code generation

Codegen is the constrained, typed layer of metaprogramming. It produces typed, checked code at compile time from a template — similar to Rust's derive macros but with an explicit syntax.

```mons
codegen derive_json(T) {
    impl Json for T {
        fn to_json(self) -> string { ... }
        fn from_json(s: string) -> Result[T, string] { ... }
    }
}

#[derive_json]
struct User {
    name: string,
    age:  int,
}

// generated automatically:
// impl Json for User { ... }
```

---

## Keyword reference

| Keyword | Category | Description |
|---|---|---|
| `fn` | Declaration | Define a function or method |
| `struct` | Declaration | Define a named data structure |
| `trait` | Declaration | Define an interface / behaviour contract |
| `impl` | Declaration | Attach methods to a struct or implement a trait |
| `let` | Binding | Declare an immutable local binding |
| `mut` | Modifier | Mark a binding or field as mutable |
| `pub` | Modifier | Make a declaration public |
| `if` | Control flow | Conditional expression |
| `else` | Control flow | Alternative branch of an `if` |
| `for` | Control flow | Iterator-based loop |
| `match` | Control flow | Exhaustive pattern matching expression |
| `return` | Control flow | Early return from a function |
| `try` | Error handling | Begin a guarded block |
| `catch` | Error handling | Handle a thrown error by type |
| `finally` | Error handling | Always-run cleanup block |
| `int` | Type | 64-bit signed integer |
| `float` | Type | 32-bit floating point |
| `double` | Type | 64-bit floating point |
| `bool` | Type | Boolean — `true` or `false` |
| `string` | Type | UTF-8 string value |
| `use` | Module | Import from a module |
| `const` | Declaration | Compile-time constant |
| `macro` | Metaprogramming | Define an AST macro |
| `codegen` | Metaprogramming | Define a compile-time code generator |

---

## Implementation pipeline

Every Mons program passes through the following stages:

```
Source (.mons)
     │
     ▼
┌─────────────┐
│    Lexer    │  Hand-written. Produces a flat token stream.
└─────────────┘
     │
     ▼
┌─────────────┐
│   Parser    │  Hand-written: recursive descent for declarations,
│             │  statements, and types; Pratt parsing for expressions.
│             │  Produces an AST using the node types in ast.h.
└─────────────┘
     │
     ▼
┌──────────────────────┐
│  Macro expansion     │  AST macros expand here before type checking
│                      │  (planned; not wired in the Phase 1 driver yet).
└──────────────────────┘
     │
     ▼
┌──────────────────────┐
│  Type checker +      │  HM-style inference. Resolves all types,
│  inference           │  checks trait bounds, validates patterns.
└──────────────────────┘
     │
     ├──────────────────────────────────────┐
     │                                      │
     ▼                                      ▼
┌─────────────────┐               ┌──────────────────┐
│  Tree-walk      │               │  Bytecode VM     │
│  interpreter    │               │  (Phase 2)       │
│  (Phase 1)      │               └──────────────────┘
└─────────────────┘                        │
                                           ▼
                                 ┌──────────────────┐
                                 │  Native code     │
                                 │  (Phase 3)       │
                                 └──────────────────┘
```

The Phase 1 **`mons`** binary also provides an **interactive REPL** (`./mons -i`, implemented in `repl.c`): it grows a source buffer, re-runs lex/parse/typecheck on the full program each turn, and evaluates expression-level input via a temporary wrapper function (see README and LANGUAGE).

### Key design choices in the pipeline

**Macro expansion before type checking.** Macros operate on raw AST nodes and can produce arbitrary new nodes. Type checking runs on the fully expanded tree, so the type system sees the generated code the same way it sees hand-written code.

**Hand-written parser (recursive descent + Pratt).** Declarations, blocks, statements, and types are parsed with recursive descent. **Expressions use Pratt (operator-precedence) parsing** so precedence and associativity stay explicit in one place; postfix chains (calls, fields, indexing, method calls) are handled after atoms. No parser generator — same goals as before: control over errors, recovery, and AST shape.

**Arena allocation for AST nodes.** All AST nodes are allocated from a single arena. The arena is freed in one call after interpretation or compilation is complete. No per-node `free` calls, no reference counting.

---

## Implementation phases

### Phase 1 — Tree-walk interpreter

The first working implementation. The AST is walked directly to evaluate expressions and execute statements. This is the fastest path to a usable language and the best environment for iterating on semantics.

Build order:

1. `arena.c` — slab allocator for AST nodes and strings *(done)*
2. `lexer.c` — hand-written lexer producing a `Token[]` *(done)*
3. `parser.c` — hand-written parser (recursive descent + Pratt expressions), building `AstNode*` trees via `ast.h` *(done — top-level `struct`, `fn`, `const`; no `impl` / `match` / `try` yet)*
4. `types.c` — type checker and HM-style unification *(done for Phase 1 subset: primitives, arrays, tuples, structs with fields, calls, methods via `self`, indexing, `for`-in over arrays, const init)*
5. `eval.c` — tree-walk evaluator with environment stack and refcounted composites *(done for the same subset)*
6. `repl.c` + `main.c` CLI — embedded demo, **`./mons file.mons`** typecheck-only, **`./mons -i`** REPL *(done)*
7. `macro.c` — AST macro expansion pass *(deferred — start of macro / pipeline work is Phase 2 prep)*
8. `stdlib/core.mons` — built-in functions (I/O, math, collections) *(deferred to Phase 2)*

**Phase 1 milestone (reference implementation) — done:** the driver (`main.c`) lexes, parses, typechecks, and evaluates programs using the subset above — including **struct literals**, **field access**, **struct update** (`..base` after explicit fields), **array/tuple** literals and indexing, **`for`** over arrays, **top-level `const`** / **`pub const`** (values live in a global eval environment before any `fn` runs), **inherent-style methods** where `recv.m(args)` calls a top-level function whose first parameter is named `self`, **`./mons file.mons`** for typecheck-only runs, **`make test`** (`tests/smoke.mons`), and an **interactive REPL** (`./mons -i`, **`repl.c`**) that wraps non-declaration input in temporary functions for evaluation.

**Beyond Phase 1 (still evolving):** the parser and type checker accept **inherent `impl`** and **trait `impl Trait for Type`** (MVP, non-generic). **Bytecode** compiles **structs**, **method calls** (including static trait dispatch via **`resolved_fn`**), **`use`**, and a **`stdlib/core.mons`** slice loaded like any other module. Still largely future work: **`try`/`catch`/`throw` on bytecode**, **generics**, AST **macros**, a full **standard library**, and bytecode gaps for some **`match`** pattern forms — see README.

### Phase 2 — Bytecode VM *(complete for [closed scope](#phase-2-closed-scope-complete))*

Phase 2 adds a compact bytecode format and a dedicated execution loop alongside the tree-walk interpreter. Work was split into mergeable milestones (**2A–2C**); the **normative** “done” boundary is the [closed scope](#phase-2-closed-scope-complete) section.

#### Phase 2A — Stack bytecode + compiler subset *(in tree)*

- **Bytecode** (`bytecode.h` / `bytecode.c`): opcodes, `Chunk` (code bytes + constant pool of `Value`).
- **Compiler** (`compile.h` / `compile.c`): AST → bytecode for a **limited** surface: integer/bool/**float**/**double** literals (constant pool), **`{ ... }` block expressions** (value position → **`compile_block_as_value`**), `+ - * / %` (numeric kinds per type-checker tags on **`AstNode.bc_ty`**), comparisons (ints plus **`OP_*_FLOAT`** / **`OP_*_DOUBLE`** for ordered compares; **`OP_EQ`** / **`OP_NE`** use scalar equality for floats/doubles), unary `-` / `!`, short-circuit **`&&`** / **`||`**, **`if` / `else` / `else if`** value expressions (requires **`else`**; branch blocks use a **tail expression** and/or **`return`** for early exit from the enclosing function chunk), locals (parameters + `let`), **assignment** to locals and **upvalue cells** (`=` expression value; **`OP_DUP`** + **`OP_STORE_LOCAL`** / **`OP_SET_UPVALUE`**), top-level **`pub const`** for **int** (pool) and **bool** (`true`/`false` opcodes), blocks with tail expression, `return`, expression statements, **lambdas** (including inferred parameter types after typecheck; `OP_CLOSURE`, upvalues, `OP_CALL_CLOSURE`), **`OP_PUSH_FN`**, **struct literals** (**`OP_STRUCT_NEW`** + **`BcStructLayout`**), **field access** (**`OP_GET_FIELD`** when the receiver is a tracked local struct, else **`OP_GET_FIELD_NAMED`** via a **`BcProgram.symbol_pool`** of interned field names), **method calls** (**`OP_CALL`** with receiver first). Jumps: **`OP_JUMP`**, **`OP_POP_JUMP_IF_FALSE`**, **`OP_POP_JUMP_IF_TRUE`**, **`OP_NOT`**. **Not** compiled yet (non-exhaustive): **`try`/`catch`/`finally`/`throw`**, some indirect calls beyond closures / named fns. **Array** / **tuple** literals use **`OP_ARRAY_NEW`** / **`OP_TUPLE_NEW`**; **`OP_ARRAY_LEN`** accepts both (**`VAL_ARRAY`** / **`VAL_TUPLE`**); **`OP_INDEX_INT`** loads from either sequence type. **`for v in …`** allows **arrays** or **homogeneous tuples** in the type checker (iterator value must be **`[T]`** or **`(T, T, …)`** with a single element type). **Struct update** (`Type { f: v, ..base, }`) **is** compiled: base is stored in a temp local, missing fields are copied with **`OP_GET_FIELD`** in declaration order, then **`OP_STRUCT_NEW`**.
- **VM** (`vm.h` / `vm.c`): **stack** machine. Refcounting aligned with `eval.h` (`value_retain` / `value_release`). **`vm_run_program(chunks, nchunks, entry, args, nargs, structs, nstructs, symbol_pool, nsymbols)`** executes **`OP_CALL`** / **`OP_CALL_CLOSURE`** across multiple chunks and needs **`structs`/`symbol_pool`** when the program uses struct opcodes; **`vm_run_chunk`** passes **`NULL`/`0`** for those tables.
- **Driver**: `./mons --vm-test` resolves `use` imports starting from **`tests/vm_smoke.mons`** (which imports **`stdlib/core.mons`**), typechecks, compiles, then runs a **fixed table** of bytecode entry checks (closures including inferred params and empty-param lambdas, structs, spread, **`for`** over arrays and tuples, **`OP_INDEX_INT`** on **arrays and tuples**, **`OP_TUPLE_NEW`**, **float/double** smokes, …). Relative jumps (**`OP_JUMP`**, **`OP_POP_JUMP_IF_*`**) use **signed int16** displacements so loops can branch backward. **`smoke_block_expr`** uses a **nested block** initializer and **`if FLAG { … }`** with a **`pub const`** **`bool`** (**`FLAG`** is all-caps so the parser does not treat **`FLAG {`** as a struct literal). **`smoke_assign`** / **`smoke_assign_capture`** exercise **`=`**. `make test` also typechecks **`tests/stdlib_core.mons`** (stdlib via **`use`**) alongside **`tests/smoke.mons`** and other fixtures.

Rationale: a stack VM is quicker to land than a register allocator; the opcode layout can be retargeted to registers later without changing the language semantics.

#### Phase 2B — Calls, richer types, register machine *(bytecode subset in tree)*

- **Done:** **`compile_program_bc`**: every top-level **`fn`** and each **`impl`** method in **source order** → `BcProgram` (**`Chunk *chunks`**, **`BcStructLayout *structs`**, **`symbol_pool`** for **`OP_GET_FIELD_NAMED`**); **`bc_fn_index(program, name)`** includes **impl** methods. **`OP_CALL`** / **`OP_PUSH_FN`** / **`OP_CLOSURE`** / **`OP_CALL_CLOSURE`** as before. **`vm_run_program`** takes **struct layouts** and **symbol pool** for struct opcodes. **`tests/smoke.mons`** exercises a cross-function call (`smoke` → `bump`); **`--vm-test`** now uses the `use` resolver from **`vm_smoke.mons`** into stdlib modules. **Scalars:** **`float`** / **`double`** literals, arithmetic, ordered compares, and **`OP_EQ`** / **`OP_NE`** on VM values. **`for`** over **homogeneous tuples** (type checker + interpreter + bytecode).
- **Still open:** optional **register-based** frame layout and future GC tuning (generational/incremental policies) on top of the current hybrid collector.

#### Phase 2C — Reflection, stdlib prelude, tooling *(in tree)*

- **Reflection** (`reflection.h` / `reflection.c`): **`reflection_fprint_program`** prints a line-oriented summary of **`pub struct`**, **`pub fn`** (params + return types from the AST), and **`pub const`** for downstream tooling. CLI: **`./mons --reflect file.mons`** (parse → typecheck → summary on stdout).
- **Stdlib** (`stdlib/core.mons`): small **bytecode-safe** int helpers (`twice`, `add`, `sub`, `mul`, `square`, `abs_i`, `min_int` / `max_int`, `clamp_i`, …). **`tests/vm_smoke.mons`** and **`tests/stdlib_core.mons`** import it via `use stdlib::core;` so **`make test`** and **`--vm-test`** exercise stdlib through the same resolver as normal runs (no source concatenation).
- **Lambdas / closures (tree-walk):** Parser accepts `|params| body` and `|| body` (Rust-style empty params); parameters may omit types for inference. The type checker builds **`TY_FN`** with lexical capture via the environment chain. **`eval`** represents closures as **`VAL_CLOSURE`** (captured names + values at creation time; **`fv_`** walk finds free variables). **Calls** dispatch on **`VAL_CLOSURE`** or top-level **`NODE_FN_DECL`**. Nested lambdas and higher-order calls (`mk(5)(7)`) work.
- **Lambdas / closures (bytecode):** **`compile_program_bc`** compiles **`NODE_LAMBDA`** (including inferred parameter types — the type checker **materializes** `NODE_PARAM.type` and optional **`lambda.ret_type`** on the AST after inference). Body is a separate chunk with **`OP_GET_UPVALUE`** for captures. **`OP_CLOSURE`** records which enclosing locals/upvalues fill the closure cells. Interpreter-only closures use **`is_bytecode == false`** and **`lambda != NULL`**; VM closures use **`is_bytecode == true`**, **`lambda == NULL`**, and **`bc_chunk_idx`**. **`eval_invoke_closure`** refuses bytecode closures (VM-only).
- **Done:** hybrid memory management for runtime composites: refcount fast-path + tracing sweep to reclaim unreachable cycles.
- **Deferred:** package boundaries and any remaining **lambda body** gaps on bytecode versus the interpreter (only if new surface appears).

**Milestone (Phase 2 in this repo):** the staged **2A–2C** bytecode path, **`use`**, trait dispatch, hybrid GC, bytecode **`try/catch/finally/throw`** (with documented limits), a **stdlib** slice, and **`--vm-test`** are complete for the [closed scope](#phase-2-closed-scope-complete) below. **Long-term:** register VM tuning and a full standard library remain **Phase 2+** / **Phase 3** prep.

### Phase 2 completion checklist

Delivered in **mergeable slices**. **Normative “Phase 2 complete”** for this repo: [Phase 2 closed scope](#phase-2-closed-scope-complete) (checklist row 9).

| # | Workstream | Dependencies | Definition of done |
|---|------------|--------------|-------------------|
| 1 | **Bytecode closure parity** | None (may parallel minor VM fixes) | **Done** — VM lowers inferred params and empty-parameter lambdas; checker materializes param / return type nodes on the AST; `smoke_infer_unary`, `smoke_infer_pair`, `smoke_pipe_closure` in `--vm-test` plus extra cases in `tests/closure.mons`; README / LANGUAGE / this doc updated. |
| 2 | **Bytecode `if` + `return` correctness** | 1 optional | **Done** — `compile_block_as_value` emits **`OP_RETURN`** for branch `return`; **`infer_block`** types `if` branches that mix `return` and tail values; `smoke_if_return_*` in `--vm-test`; `if_return_ok` in `tests/closure.mons`. |
| 3 | **`match` (parser → types → eval + bytecode)** | 2 recommended | **Done** — `match` parses (including `Option::None` as `TOK_NONE` after `::`); exhaustiveness for bool / `Option` / scalars / struct (`_` required); tree-walk + bytecode for literals, `_`, binds, `Option::None` / `Option::Some` (1-tuple runtime), struct fields; `|` without bindings; bytecode skips `|` patterns, `Option::Some` inner literals, and defers some enum/struct edges — see README; `smoke_match_*` in `--vm-test`; `tests/closure.mons` match cases; README / LANGUAGE updated. |
| 4 | **`try` / `catch` / `finally`** | 3 optional (orthogonal) | **Done (MVP on both backends)** — parse, typecheck, tree-walk eval, and bytecode VM support `try/catch/finally/throw`; `finally` runs on success and throw paths; VM smoke covers basic/cross-call throw + finally. **Known VM gap:** `return` inside `try`/`catch`/`finally` regions is not lowered yet (compile-time rejection). |
| 5 | **Trait `impl Trait for Type` + dispatch** | 3 recommended (stable calls) | **Done (MVP)** — parse `trait { fn …; }`, `impl Trait for Type { … }`; checker registers traits, checks impl signatures vs trait (structural AST match on params/returns), rejects generics/supertraits; **no vtable**: `r.m()` resolves by **static receiver type** to a single `NODE_FN_DECL` stored as **`method_call.resolved_fn`** (same index model as inherent `impl`); interpreter + bytecode use it; `tests/trait_impl.mons` + `smoke_trait_bump` in `--vm-test`; default trait methods / `Self` / trait objects remain Phase 2+. |
| 6 | **`use` / modules** | 5 recommended | **Done** — top-level `use` supports plain (`use a::b;`), selective (`use a::{b,c};`), and glob (`use a::*;`) forms; loader resolves imports recursively before lex/parse, de-duplicates modules, and detects cycles; unresolved imports and cycles are tested (`tests/use_missing.mons`, `tests/use_tree_missing.mons`, `tests/use_cycle_a.mons`, `tests/use_cycle_tree_a.mons`); `--vm-test` relies on `use stdlib::core;` from `tests/vm_smoke.mons`. |
| 7 | **Tracing GC (or hybrid)** | Stable object graph: at minimum 1–3 | **Done (hybrid)** — runtime keeps refcount semantics and tracks heap composites globally; `value_gc_collect(...)` marks from roots and sweeps unreachable objects (including cycles). VM and interpreter invoke sweep at call boundaries preserving returned roots. `run_gc_stress_test()` in `--vm-test` builds synthetic cycles and verifies live-object count returns to baseline. |
| 8 | **Stdlib “real” prelude** | 6 strongly preferred | **Done** — `stdlib/core.mons` holds documented int helpers; **`tests/stdlib_core.mons`** + **`tests/vm_smoke.mons`** load it with **`use stdlib::core;`** (`make test` and **`--vm-test`**); README tour calls out builtins not in-tree yet vs `stdlib/core.mons`. |
| 9 | **Phase 2 “closed” documentation** | 1–8 per scope | **Done** — [Phase 2 closed scope](#phase-2-closed-scope-complete) below; `make test` green; README milestone matches this section. |

### Phase 2 closed scope (complete)

The **Phase 2** milestone in this repository means: **Phase 1** tree-walk (`eval.c`) plus the **staged bytecode VM** (**2A–2C**) for the surface exercised by **`make test`** and **`./mons --vm-test`**, with the exclusions listed here. This is a **closed** slice of the full EBNF, not “every grammar form on both backends.”

**In scope (implemented and covered by tests / smokes):**

- Stack **bytecode** (`bytecode.c`, `compile.c`, `vm.c`): chunks, calls, **`OP_CLOSURE`** / upvalues, structs + inherent **`impl`** + **`impl Trait for Type`** (static dispatch via **`resolved_fn`**), **`match`** for the supported pattern subset, **`for`** over arrays and homogeneous tuples, **`[]`**, **`float`/`double`**, hybrid **refcount + tracing GC** (`--vm-test` includes a synthetic cycle stress).
- **Modules:** top-level **`use a::b::c;`**, recursive load, de-duplication, cycle detection (`tests/use_*`).
- **Stdlib:** **`stdlib/core.mons`** loaded only through **`use stdlib::core;`** (`tests/stdlib_core.mons`, **`tests/vm_smoke.mons`**).
- **Tooling:** **`./mons --reflect`**, **`./mons --vm-test`**, REPL and file typecheck unchanged.

**Explicitly out of scope for this “Phase 2 complete” label (Phase 2+ or later):**

- **Full parity for `try` with `return`-interaction on bytecode** (current VM lowers try/catch/finally/throw but rejects `return` inside those regions).
- **AST macro expansion** before typecheck (`macro.c` pipeline).
- **Package layout / boundaries**, richer module system semantics beyond file-based loading.
- **Generics**, **trait objects**, default trait methods, **`Self`** in traits (non-generic trait **`impl`** only).
- **Register-based** VM frames, generational GC, and other performance engineering.
- **Native code** backends (**Phase 3**).
- **`match`** / **`enum`** forms the bytecode compiler still rejects (or-patterns with bindings, some **`Option::Some`** inner tests, etc. — see README).

**Dependency sketch** (compact):

```
1 closure VM parity
2 if/return VM ──► 3 match
3 match ──► 5 traits (recommended)
5 traits ──► 6 use (recommended)
1–3 stable ──► 7 GC
6 use ──► 8 stdlib
9 docs last
4 try/catch ── may proceed in parallel unless tied to stack/unwind cleanup
```

### Phase 3 — Native code (optional)

Emit native code from the bytecode representation. Two candidate approaches:

- **C emission** — generate valid C source and compile with `cc`. Simple, portable, no new dependencies.
- **LLVM IR** — emit LLVM IR for full optimisation pipeline. Powerful, but adds a significant dependency.

Milestone: self-hosting — the Mons compiler is written in Mons and compiles itself.

---

## File structure

```
mons-lang/
├── README.md               # Project overview and language tour
├── DESIGN.md               # This document
├── LANGUAGE.md             # Syntax reference (Phase 1)
├── mons_grammar.ebnf       # Formal EBNF grammar
├── Makefile
│
├── include/
│   ├── ast.h               # AstNode definitions, NodeKind enum, AS_* macros
│   ├── lexer.h
│   ├── parser.h
│   ├── types.h             # type_check_program
│   ├── eval.h              # Value, eval_call_by_name, value_retain, value_release
│   ├── repl.h              # repl_run()
│   ├── bytecode.h          # Chunk, opcodes (Phase 2A)
│   ├── compile.h           # compile_program_bc, BcProgram (chunks, layouts, symbol pool)
│   ├── vm.h                # vm_run_program(chunks, …, structs, symbol_pool), vm_run_chunk
│   └── reflection.h        # reflection_fprint_program (Phase 2C)
│
├── stdlib/
│   └── core.mons           # Imported via `use stdlib::core;` (bytecode-safe subset)
├── tests/
│   ├── smoke.mons          # `make test` typecheck; local `bump(K)`
│   ├── closure.mons        # `make test` — lambdas / captures (typecheck)
│   ├── stdlib_core.mons    # `make test` — typecheck `use stdlib::core`
│   └── vm_smoke.mons       # `--vm-test` — `use stdlib::core` + VM smoke table
└── src/
    ├── arena.c             # Slab arena allocator
    ├── lexer.c             # Hand-written lexer
    ├── parser.c            # Recursive descent + Pratt expression parser
    ├── types.c             # Type checker + HM-style unification (Phase 1 subset)
    ├── eval.c              # Tree-walk interpreter (Phase 1)
    ├── repl.c              # Interactive REPL (-i / --repl)
    ├── bytecode.c          # Phase 2A: bytecode chunks
    ├── compile.c           # Phase 2A/2B: AST → bytecode (subset + calls)
    ├── vm.c                # Phase 2A/2B: stack VM + call frames
    ├── reflection.c        # Phase 2C: public API summary for tooling
    ├── ast_print.c         # Debug AST printer
    └── main.c              # Pipeline entry (embedded demo, file path, REPL, --vm-test, --reflect)

# Phase 2 (closed scope): bytecode 2A–2C, reflection, stdlib/core.mons via use, hybrid GC, trait impl + match subset + try/catch/finally/throw on both backends (VM gap: return inside try regions) — see “Phase 2 closed scope (complete)” above
# Phase 2+: full try/return parity on VM, macro pass, full stdlib, register VM, remaining match/forms
```

---

## Design decisions log

A running record of non-obvious choices and the reasoning behind them.

---

**No `class` keyword.**
Structs + traits + impl blocks cover every case a class would handle, with clearer separation of data and behaviour. The absence of `class` signals the intent: Mons is not an OOP language that also has functional features, it is a functional language that supports OOP-adjacent patterns when needed.

---

**`for` is iterator-based only.**
There is no C-style `for(init; cond; step)`. The iterator protocol is the single abstraction for all sequence traversal. This keeps the language surface smaller and encourages working with higher-order functions (`map`, `filter`, `fold`) for transformations.

---

**`try/catch/finally` coexists with `Result`.**
The idiomatic path for error handling is `Result[T, E]` + `match`. But `try/catch/finally` is available for two real reasons: ergonomics when propagating errors through many layers, and interop with native code that throws. Neither style is removed — they serve different contexts.

---

**Immutable by default, `mut` local only.**
`mut` cannot appear on function parameters. This makes function signatures honest: a function that mutates something must either return the new value or take a mutable reference (Phase 2). In Phase 1 this means pure functions are the natural default.

---

**Macros before type checking.**
AST macros expand before the type checker runs. This means macros can produce any valid AST — including type annotations, struct fields, impl blocks. The tradeoff is that macro errors can produce confusing type errors. This is the same tradeoff Rust makes and is considered acceptable given the power it enables.

---

**Hand-written lexer and parser.**
No parser generator (no yacc, bison, ANTLR). The parser is mostly **recursive descent** for the grammar structure, with **Pratt parsing for expressions** so operator precedence stays table-driven and easy to adjust. That combination keeps full control over error messages, recovery, and AST shape, and remains straightforward to extend with new parsing functions rather than regenerating from a grammar file.

---

**Pratt parsing for expressions.**
A layered recursive-descent expression parser (one function per precedence level) is correct but verbose and easy to get wrong when adding operators. **Pratt parsing** centralizes precedence and associativity (including right-associative `=`) in one loop plus a small infix table, while atoms and postfix chains stay in familiar recursive-descent style.

---

**Struct literal vs block after `in` (`for x in a { … }`).**
Postfix parsing treats `ident {` as a struct initializer only when the identifier starts with an **uppercase** letter (Rust-style type name). That way `for x in a { … }` is not parsed as `a { … }` (struct literal). Field names and variables remain lowercase.

---

**Arena allocation for AST nodes.**
The parser allocates all AST nodes from a single arena. At the end of the pipeline (after interpretation or compilation), the entire arena is freed in one call. This avoids per-node `free` bookkeeping and makes allocation fast — the arena just bumps a pointer. The tradeoff is that AST nodes cannot be individually freed, which is fine because we never need to free a subset of them.

---

**Hindley-Milner inference, not bidirectional.**
HM inference propagates type information through the whole program rather than requiring the programmer to annotate in specific positions. The tradeoff is that error messages can point to the wrong location when inference fails. This is a known weakness of HM and is mitigated by careful error reporting in `types.c`.

---

**Phase 1 inherent methods without `impl` blocks.**
Until the parser accepts `impl`, method call syntax `receiver.method(args)` is lowered to a call of the **top-level** function `method`, requiring its **first parameter to be named `self`** and its type to match the receiver. This is intentionally minimal: there is no vtable or trait dispatch, and method names share the global function namespace.

---

**Top-level `const` in the interpreter.**
Constants are type-checked with the same `infer_expr` pass as the rest of the program (in source order alongside function signature registration). At evaluation time, the driver evaluates each const initializer into a **global** environment before invoking any function, so function bodies see const bindings. Initializers may be arbitrary expressions (including calls to functions declared earlier); compile-time-only enforcement is a later refinement.

---

**REPL semantics.**
The REPL appends each submission to a session string and re-parses the **entire** program. If parsing as top-level declarations fails, the submission is wrapped in `fn __monsrepl_N() { … }` and re-parsed so statements and tail expressions behave like inside a normal function body. This avoids a second parser for “expression-only” input while keeping the type checker unchanged.

---

**Stack VM before registers.**
The bytecode backend uses a **stack machine** (`vm.c`) so the compiler (`compile.c`) does not need a register allocator. **Phase 2B** adds **framed execution** and **`OP_CALL`** across chunks; a register-based VM remains an optional Phase 2B/C target for hot paths. Opcodes and `Chunk` layout are internal to the repo.

---

**Composite values and refcounting in `eval.c`.**
Arrays, tuples, and structs are heap-allocated with explicit reference counts. `value_release` should be used on `EvalResult.result` when the caller is done with a composite value. Local scopes release bindings when blocks and frames pop.
