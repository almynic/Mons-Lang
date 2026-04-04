# Mons Lang

A statically typed, functionally-oriented general purpose language written in C.

Mons prefers expressions over statements, immutability over mutation, and composition over inheritance. OOP is possible — it is just not the default mental model.

---

## Table of contents

- [Overview](#overview)
- [Language tour](#language-tour)
  - [Primitives and bindings](#primitives-and-bindings)
  - [Functions](#functions)
  - [Control flow](#control-flow)
  - [Structs and traits](#structs-and-traits)
  - [Error handling](#error-handling)
  - [Pattern matching](#pattern-matching)
  - [Closures](#closures)
  - [Generics](#generics)
  - [Metaprogramming](#metaprogramming)
- [Type system](#type-system)
- [Implementation roadmap](#implementation-roadmap)
- [Project structure](#project-structure)
- [Building & CLI](#building--cli)
- [Design principles](#design-principles)
- [Language reference](LANGUAGE.md) (syntax & Phase 1 examples)

---

## Overview

| Property | Value |
|---|---|
| Type system | Static, inferred (Hindley-Milner style) |
| Mutability | Immutable by default — explicit `mut` |
| Paradigm | Functional-first, imperative possible |
| OOP | Structs + traits + impl blocks — no class keyword |
| Error handling | `Result[T, E]` + `match` or `try/catch/finally` |
| Metaprogramming | AST macros, runtime reflection, compile-time codegen |
| Implementation | Written in C, zero dependencies |
| Status | **Phase 1 complete** — tree-walk interpreter (`make`, `./mons`, `./mons -i`, `make test`) |

The **language tour** below describes the *target* Mons design. The **reference implementation** implements the **Phase 1** milestone end-to-end: lex → parse → typecheck → eval for the supported subset, **`./mons path.mons`** for typechecking a file, **`./mons -i`** for an interactive REPL, and **`make test`**. Larger features (`match`, lambdas, `impl` blocks, `try`/`catch`, generics, macros, stdlib) are **Phase 2** goals unless noted otherwise.

---

## Language tour

### Primitives and bindings

```mons
let x: int = 42
let pi: double = 3.14159
let name: string = "Mons"
let active: bool = true

// type is inferred — annotation optional
let y = 100
let greeting = "hello"

// mutation requires explicit mut
let mut counter = 0
counter = counter + 1
```

Primitive types: `int`, `float`, `double`, `bool`, `string`.

---

### Functions

```mons
fn add(a: int, b: int) -> int {
    a + b   // last expression is the return value — no return keyword needed
}

// return type inferred
fn greet(name: string) {
    let msg = "Hello, " + name
    msg
}

// public function
pub fn square(n: int) -> int { n * n }
```

---

### Control flow

`if` and `match` are expressions — they produce a value.

```mons
// if as expression
let label = if score > 90 { "A" } else if score > 75 { "B" } else { "C" }

// for is iterator-based
for item in collection {
    process(item)
}

// range iteration (via stdlib)
for i in 0..10 {
    println(i)
}
```

---

### Structs and traits

```mons
pub struct Point {
    x: float,
    y: float,
}

impl Point {
    fn distance(self, other: Point) -> float {
        let dx = self.x - other.x
        let dy = self.y - other.y
        sqrt(dx * dx + dy * dy)
    }

    fn origin() -> Point {
        Point { x: 0.0, y: 0.0 }
    }
}

trait Shape {
    fn area(self) -> float
    fn perimeter(self) -> float

    // default method
    fn describe(self) -> string {
        "area=" + to_string(self.area())
    }
}

pub struct Circle {
    center: Point,
    radius: float,
}

impl Shape for Circle {
    fn area(self) -> float { 3.14159 * self.radius * self.radius }
    fn perimeter(self) -> float { 2.0 * 3.14159 * self.radius }
}
```

Structs can implement multiple traits. Inheritance is supported but discouraged — the language makes composition feel more natural.

---

### Error handling

Two styles coexist. `Result` + `match` is idiomatic; `try/catch/finally` exists for ergonomics and interop.

```mons
// Result style
fn divide(a: int, b: int) -> Result[int, string] {
    if b == 0 {
        Err("division by zero")
    } else {
        Ok(a / b)
    }
}

let result = match divide(10, 0) {
    Ok(val)  => val,
    Err(msg) => { println(msg); 0 }
}

// try / catch / finally
try {
    let data = read_file("config.json")
    parse(data)
} catch (e: IoError) {
    println("io error: " + e.message)
} catch (e: ParseError) {
    println("parse error at line " + to_string(e.line))
} finally {
    cleanup()
}
```

---

### Pattern matching

`match` works on any type. Patterns can be nested, guarded, and combined with `|`.

```mons
// matching on an Option
let value: Option[int] = Some(42)

let doubled = match value {
    Some(n) if n > 0 => n * 2,
    Some(_)          => 0,
    None             => -1,
}

// matching on a struct
match point {
    Point { x: 0.0, y } => println("on y-axis at " + to_string(y)),
    Point { x, y: 0.0 } => println("on x-axis at " + to_string(x)),
    Point { x, y }      => println(to_string(x) + ", " + to_string(y)),
}

// or-patterns
match status {
    200 | 201 | 204 => "success",
    400 | 422       => "client error",
    500 | 503       => "server error",
    _               => "unknown",
}
```

---

### Closures

Functions are first-class. Closures capture their environment.

```mons
let double = |x: int| -> int { x * 2 }
let add    = |a, b| a + b        // types inferred, body is a single expression

let numbers = [1, 2, 3, 4, 5]
let evens   = filter(numbers, |n| n % 2 == 0)
let squared = map(evens, |n| n * n)
let total   = fold(squared, 0, |acc, n| acc + n)
```

---

### Generics

Parametric generics with optional trait bounds.

```mons
fn identity[T](value: T) -> T { value }

fn max_of[T: Comparable](a: T, b: T) -> T {
    if a > b { a } else { b }
}

struct Pair[A, B] {
    first:  A,
    second: B,
}

impl[A, B] Pair[A, B] {
    fn swap(self) -> Pair[B, A] {
        Pair { first: self.second, second: self.first }
    }
}
```

---

### Metaprogramming

Three layers, applied at different stages.

**AST macros** — expand before type checking, the most powerful layer:

```mons
macro assert!(expr) {
    if !expr {
        panic("assertion failed: " + stringify!(expr))
    }
}

macro vec![...elems] {
    { let mut v = Vec::new(); push_all!(v, elems); v }
}
```

**Reflection** — runtime type introspection:

```mons
let fields = reflect::fields_of(Point)
for field in fields {
    println(field.name + ": " + field.type_name)
}
```

**Code generation** — compile-time typed templates:

```mons
codegen derive_json(T) {
    impl Json for T {
        fn to_json(self) -> string { ... }
        fn from_json(s: string) -> Result[T, string] { ... }
    }
}

#[derive_json]
struct User { name: string, age: int }
```

---

## Type system

Mons uses Hindley-Milner style inference. Type annotations are optional in most positions — the checker resolves them from usage.

Built-in generic types:

| Type | Meaning |
|---|---|
| `Option[T]` | A value that may be absent — `Some(v)` or `None` |
| `Result[T, E]` | A computation that may fail — `Ok(v)` or `Err(e)` |
| `[T]` | Array of T |
| `(A, B)` | Tuple |
| `fn(A) -> B` | Function type |
| `&T` / `&mut T` | Reference (Phase 2+) |

---

## Implementation roadmap

### Reference interpreter — Phase 1 (complete)

| Component | Notes |
|-----------|--------|
| **Arena + AST** | `arena.c`, `ast.h` — nodes allocated in an arena; `ast_print.c` for debug |
| **Lexer** | `lexer.c` — tokens including keywords, literals, operators |
| **Parser** | `parser.c` — recursive descent for decls / statements / types; **Pratt** for expressions; postfix for calls, fields, methods, indexing |
| **Type checker** | `types.c` — unification (with occurs check), struct layouts in the environment, function signatures and bodies |
| **Evaluator** | `eval.c` — tree-walk with environments, refcounted composite values (`[T]`, tuples, structs) |
| **REPL + driver** | `repl.c`, `main.c` — `-i` / `--repl` (accumulated session, re-typecheck each input); no-arg demo; file path = typecheck only |
| **Bytecode (2A/2B)** | `bytecode.c`, `compile.c`, `vm.c` — stack `Chunk`, `compile_program_bc`, `OP_CALL`, `vm_run_program`; `--vm-test` |
| **Reflection + stdlib (2C)** | `reflection.c`, `stdlib/core.mons` — **`--reflect`** public API summary; VM smoke prepends stdlib + `tests/vm_smoke.mons` |

**Language surface that typechecks and runs end-to-end** (non-exhaustive):

- Top-level **`struct`** (non-generic), **`fn`** / **`pub fn`**, **`const`** / **`pub const`** (initializer type-checked and evaluated at startup into a global value environment).
- Types: primitives, **`[T]`**, tuples **`(A, B)`**, named struct types, **`Option` / `Result`** in type syntax (limited use in expressions).
- Statements: **`let`**, **`return`**, expression statements, **`for x in arr`** (arrays only at runtime).
- Expressions: literals, **`if`**, blocks, arithmetic / comparisons / **`&&`** / **`||`**, assignment to locals, **calls**, **method calls** `r.method(args)` desugared to `method(r, args)` when the callee’s first parameter is named **`self`**.
- **Struct literals** `Type { f: e, }`, **field access**, and **struct update** `Type { f: v, ..base, }` (spread must appear after explicit fields in the current parser).
- **Arrays** and **tuples**: literals, indexing; tuple indices must be **integer literals** in the type checker.

**Deferred (bytecode still catching up to Phase 1)** in this repo: `impl` / trait parsing, **`match`**, **lambdas / closures** (grammar only; not parsed end-to-end), **`try` / `catch` / `finally`** (parser rejects `try` today), AST **macro** expansion pass, **`use` imports**, bytecode for composites / `if` / `&&`/`||` / methods, tracing **GC** — see DESIGN. **Cross-function calls** and a **stdlib prelude** for VM smoke are implemented; **`--reflect`** lists public API shapes from the AST.

---

```
Phase 1 — Tree-walk interpreter (complete)
  ✓ Grammar + AST node definitions (ast.h)
  ✓ Arena allocator
  ✓ Lexer
  ✓ Parser (recursive descent + Pratt expressions)
  ✓ Type checker + inference (types.c — unification, Phase 1 subset)
  ✓ Tree-walk evaluator (eval.c)
  ✓ Driver: embedded demo, optional source file, `make test` (tests/smoke.mons)
  ✓ Interactive REPL (`./mons -i`, `repl.c`)
  ○ AST macro expansion (Phase 2 prep)
  ○ Standard library (Phase 2)

Phase 2 — Bytecode VM
  ✓ Phase 2A: stack bytecode + `Chunk`, compiler subset, stack VM, `./mons --vm-test`
  ◐ Phase 2B: **calls** (`compile_program_bc`, `OP_CALL`, `vm_run_program`); broader types, optional register machine, tracing GC still open
  ✓ Phase 2C: **reflection** (`--reflect`), **stdlib** prelude (`stdlib/core.mons` + VM smoke); lambdas/closures deferred

Phase 3 — Native code (optional)
  ▸ C code emission or LLVM IR backend
  ▸ Self-hosting (Mons compiler written in Mons)
```

---

## Project structure

```
mons-lang/
├── README.md
├── DESIGN.md
├── LANGUAGE.md             # Syntax reference + Phase 1 examples
├── mons_grammar.ebnf       # Formal grammar (EBNF)
├── Makefile
├── stdlib/
│   └── core.mons           # Prepended for `./mons --vm-test`
├── tests/
│   ├── smoke.mons          # `make test` — parse + typecheck (`bump(K)`)
│   └── vm_smoke.mons       # With stdlib: VM smoke (`twice(K)` → 14)
│
├── include/
│   ├── ast.h               # AST nodes, lists, arena API
│   ├── lexer.h
│   ├── parser.h
│   ├── types.h             # type_check_program API
│   ├── eval.h              # Value, eval_call_by_name, value_retain
│   ├── repl.h              # repl_run()
│   ├── bytecode.h          # Chunk, opcodes
│   ├── compile.h           # compile_program_bc, bc_fn_index, BcProgram
│   ├── vm.h                # vm_run_program, vm_run_chunk
│   └── reflection.h        # reflection_fprint_program
│
└── src/
    ├── main.c              # CLI: demo | file typecheck | REPL (-i)
    ├── arena.c
    ├── lexer.c
    ├── parser.c            # Recursive descent + Pratt expressions
    ├── types.c             # Type checker + unification
    ├── eval.c              # Tree-walk interpreter
    ├── repl.c              # Interactive REPL
    ├── bytecode.c          # Phase 2A: Chunk + constant pool
    ├── compile.c           # Phase 2A/2B: AST → bytecode (subset + calls)
    ├── vm.c                # Phase 2A/2B: stack VM + call frames
    ├── reflection.c        # Phase 2C: public API dump
    └── ast_print.c         # Debug AST printer
```

*(Bytecode: `bytecode.c`, `compile.c`, `vm.c` — Phase 2A stack + subset; Phase 2B multi-chunk calls. Phase 2C: `reflection.c`, `stdlib/core.mons`. Later: `use`, lambdas, tracing GC — see DESIGN.)*

---

## Building & CLI

Requires a C11-compatible compiler. No other dependencies.

```sh
cd mons-lang
make
make test           # typecheck smoke.mons + `--reflect` + bytecode VM smoke (`--vm-test`)
```

### `mons` modes

| Invocation | Behaviour |
|------------|-----------|
| *(no arguments)* | Embedded sample: print AST, typecheck, run several **`eval_call_by_name`** smoke tests (`add`, `mid`, …). |
| **`./mons path.mons`** | Read file, lex, parse, typecheck. Prints `type check: ok` on success. No eval, no AST dump. |
| **`./mons -i`** or **`./mons --repl`** | Interactive REPL: session grows with each successful input; full program is re-parsed and re-typechecked each time. Non–top-level snippets are wrapped in `fn __monsrepl_N() { … }` and evaluated; see [LANGUAGE.md — REPL](LANGUAGE.md#repl). |
| **`./mons --vm-test`** | Concatenate **`stdlib/core.mons`** + **`tests/vm_smoke.mons`**, typecheck, bytecode-compile, run **`smoke`** via **`vm_run_program`** (call to **`twice`**); prints `bytecode smoke() = 14`. |
| **`./mons --reflect path.mons`** | Lex, parse, typecheck, then print a line-oriented summary of **`pub struct`**, **`pub fn`**, **`pub const`** (for tooling). |
| **`./mons -h`** / **`--help`** | Usage summary. |

**REPL tips:** end a line with **`\\`** to continue on the next line, or leave **`{`** unclosed until the matching **`}`** (prompt shows `...`). Commands: **`:help`**, **`:clear`**, **`:quit`** (or EOF).

**CI:** `make test` runs `./mons tests/smoke.mons`, `./mons --reflect tests/smoke.mons`, and `./mons --vm-test`; all must exit 0.

---

## Design principles

**Immutable by default.** Mutation is an explicit opt-in, not a default. This makes functions honest about side effects and makes code easier to reason about.

**Expressions over statements.** `if`, `match`, and blocks all produce values. This reduces boilerplate and enables more compositional code.

**Composition over inheritance.** Structs hold data. Traits define behaviour. `impl` blocks attach behaviour to data. This is sufficient for almost every design — inheritance exists but is not idiomatic.

**Two error models, neither forced.** `Result[T, E]` with `match` is idiomatic for expected failures. `try/catch/finally` is available for ergonomics, FFI, and cases where propagating a `Result` up a deep call stack would be noise.

**Metaprogramming at the right layer.** AST macros for syntax extension, reflection for runtime introspection, codegen for typed compile-time templates. Each layer has a different power/safety tradeoff.

**Written in C, zero dependencies.** The implementation is portable and embeddable by design.
