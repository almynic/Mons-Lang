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

**Beyond Phase 1 (design-doc parity, later phases):** closures/lambdas, `match`, `try`/`catch`, `impl` blocks and traits in the parser, generics on functions/structs, AST macros, and a standard library.

### Phase 2 — Register-based bytecode VM

The bytecode phase introduces a proper execution engine. The AST is compiled to a compact instruction set; the VM executes bytecode rather than walking the tree.

Components:

- Bytecode format and instruction set definition
- `compiler.c` — AST → bytecode compiler
- `vm.c` — register-based VM execution loop
- Garbage collector (tri-color mark-and-sweep)
- Closure and upvalue representation
- Reflection system (runtime type metadata)

Milestone: Mons programs run significantly faster. The GC handles heap-allocated values correctly. Closures, higher-order functions, and the full stdlib work end-to-end.

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
│   ├── eval.h              # Value, eval_call_by_name, value_release
│   └── repl.h              # repl_run()
│
├── tests/
│   └── smoke.mons          # `make test`
└── src/
    ├── arena.c             # Slab arena allocator
    ├── lexer.c             # Hand-written lexer
    ├── parser.c            # Recursive descent + Pratt expression parser
    ├── types.c             # Type checker + HM-style unification (Phase 1 subset)
    ├── eval.c              # Tree-walk interpreter (Phase 1)
    ├── repl.c              # Interactive REPL (-i / --repl)
    ├── ast_print.c         # Debug AST printer
    └── main.c              # Pipeline entry (embedded demo, file path, or REPL)

# Phase 2+ (not in tree yet):
#   macro.c, compiler.c, vm.c, stdlib/
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

**Composite values and refcounting in `eval.c`.**
Arrays, tuples, and structs are heap-allocated with explicit reference counts. `value_release` should be used on `EvalResult.result` when the caller is done with a composite value. Local scopes release bindings when blocks and frames pop.
