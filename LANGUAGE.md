# Mons language reference

This file describes **syntax** and gives **examples**. For design goals and implementation notes, see [README.md](README.md) and [DESIGN.md](DESIGN.md). The canonical formal grammar is [mons_grammar.ebnf](mons_grammar.ebnf); the **reference interpreter** implements the **Phase 1** language surface (see [Implemented in Phase 1](#implemented-in-phase-1) below).

## Table of contents

- [REPL](#repl)
- [Lexical basics](#lexical-basics)
- [Program structure](#program-structure)
- [Top-level declarations](#top-level-declarations)
- [Types (Phase 1)](#types-phase-1)
- [Statements](#statements)
- [Expressions and precedence](#expressions-and-precedence)
- [Worked mini-program](#worked-mini-program)
- [Implemented in Phase 1](#implemented-in-phase-1)
- [Planned / full grammar](#planned--full-grammar-not-phase-1)

---

## REPL

Run **`./mons -i`** or **`./mons --repl`** for an interactive session. The REPL keeps a growing source buffer: each submission is appended, then the **entire program** is lexed, parsed, and typechecked. Input that is not valid as top-level declarations is wrapped in a temporary function `fn __monsrepl_N() { ... }`, which is then evaluated so the **last expression** of the block is printed (composite values use refcounting; the REPL releases the result after printing).

**Multi-line input**

- **Unbalanced braces:** keep typing until `{` and `}` match (prompt switches to `...`).
- **Line continuation:** put a **backslash** (`\`) as the last character on a line to join the next line into the same submission (useful for `let ...;` on one line and the result expression on the next).

**Commands:** **`:help`**, **`:clear`** (drop the accumulated program and reset repl counter), **`:quit`** / **`:q`**, or end-of-file to exit.

**Note:** Each logical submission should be one “paragraph” (one brace-balanced chunk or one backslash-continued block). Otherwise a `let` on one line and an expression on the next are processed as two separate inputs.

---

## Lexical basics

- **Identifiers**: letters, digits, underscore; must not start as a number (underscore allowed).
- **Comments**: `//` to end of line, or `/* ... */`.
- **Integer literals**: decimal digits, e.g. `42`.
- **Float**: digits, `.`, digits, suffix `f`, e.g. `1.5f`.
- **Double**: digits, `.`, digits (no `f`), e.g. `2.0`.
- **Bool**: `true`, `false`.
- **String**: `"..."` with escapes `\n`, `\t`, `\r`, `\\`, `\"`, `\0`.
- **`None`**: literal for optional absence (typed as `Option[...]` when used in typing contexts).

---

## Program structure

A program is a sequence of **top-level declarations** in order. There is no required `main` in the grammar: the no-argument **`mons`** binary runs a fixed embedded sample and evaluates several **`pub fn`** entries by name; the **REPL** evaluates wrapped snippets or lets you build declarations incrementally; **`./mons file.mons`** only typechecks.

```mons
struct Point {
    x: int,
    y: int,
}

const answer: int = 40 + 2;

fn area(self: Point) -> int {
    self.x * self.y
}

pub fn use_point() -> int {
    let p = Point { x: 3, y: 4, };
    p.x + p.y
}
```

---

## Top-level declarations

### `struct`

Defines a named product type. Fields end with a comma (trailing comma allowed). **Generics on structs** are not supported in Phase 1.

```mons
struct Point {
    x: int,
    y: int,
}
```

### `const` / `pub const`

Requires an **explicit type** and an initializer. The initializer is type-checked with the rest of the program; the interpreter evaluates consts into a global environment before running a function. **`pub`** is accepted for visibility metadata (same type-check rules as `const`).

```mons
const answer: int = 40 + 2;
pub const K: int = 7;
```

### `fn` / `pub fn`

Functions have parameters `(name: type, ...)` and an optional return type `-> T`. The body is a **block**. The **last expression** in the block is the function’s value unless `return` is used.

```mons
fn add(a: int, b: int) -> int {
    a + b
}

pub fn twice(n: int) -> int {
    return n * 2;
}
```

---

## Types (Phase 1)

| Form | Meaning |
|------|---------|
| `int`, `float`, `double`, `bool`, `string` | Primitives |
| `Name` | Named type (e.g. struct) |
| `[T]` | Array of `T` |
| `(A, B, ...)` | Tuple |
| `Option[T]` | Optional (syntax in types; expression support is limited) |
| `Result[T, E]` | Result (syntax in types; expression support is limited) |
| `fn(A, B) -> R` | Function type |

References `&T` / `&mut T` appear in the grammar; treat them as **not yet** fully supported in the Phase 1 checker unless you verify otherwise.

---

## Statements

Statements appear inside blocks. The block may end with a **trailing expression** (no semicolon) that becomes the block’s value.

### `let` / `let mut`

```mons
let x = 1;
let y: int = 2;
let mut s = 0;
s = s + 1;
```

### `return`

```mons
return;
return 42;
```

### Expression statement

Any expression followed by `;`.

### `for`

Iterator form: **`for` *ident* `in` *expr* *block***. *expr* may be an **array** or a **homogeneous tuple** (all elements must unify to one type); the loop variable has that element type.

```mons
let a = [1, 2, 3];
let s = 0;
for x in a {
    s = s + x;
};
s
```

---

## Expressions and precedence

From loose to tight binding (typical):

1. Assignment `=`
2. `||`
3. `&&`
4. `==`, `!=`
5. `<`, `>`, `<=`, `>=`
6. `+`, `-`
7. `*`, `/`, `%`
8. Unary `!`, `-`
9. Postfix: calls `f(a)`, indexing `a[i]`, field access `x.f`, method calls `x.m(a)`

Parentheses `( ... )` group as usual.

### Blocks as expressions

```mons
let n = {
    let a = 1;
    let b = 2;
    a + b
};
```

### `if`

`if` is an expression: each branch is a block (or another expression as parsed). `else if` chains and final `else` follow the grammar. A branch may use **`return`** to exit the enclosing function (or **`||`** lambda body); the type checker requires branch types to unify with each other and with the function’s return type when **`return`** appears.

```mons
let ok = if a + b * 2 == 0 && true || false {
    true
} else {
    false
};
```

### Calls

```mons
add(1, 2);
area(p);
```

### Method calls (inherent methods)

`receiver.method(args)` is checked and evaluated as a call to a **top-level** function named `method`, with the receiver passed as the **first argument**. That parameter must be named **`self`**.

```mons
fn area(self: Point) -> int {
    self.x * self.y
}

let p = Point { x: 3, y: 4, };
p.area()
```

### Field access and indexing

```mons
p.x
t[1]
a[0]
```

Tuple indices in types must be **integer literals** (e.g. `t[1]`, not `t[i]` for inference in Phase 1).

### Arrays and tuples

```mons
let a = [1, 2, 3];
let t = (10, 20);
```

### Struct literals and update

The parser treats **`Ident {`** as a struct literal only when **`Ident`** looks like a **type name** (PascalCase: at least one lowercase letter after the first, or a single-letter uppercase name like **`T`**). **All-uppercase** identifiers (**`FLAG`**, **`MAX`**) are not struct literals, so **`if FLAG { … }`** is a boolean condition, not `FLAG { … }` fields.

**Full init** (without base): every field must be supplied exactly once (type checker).

```mons
let p = Point { x: 3, y: 4, };
```

**Update**: copy from a base value and override selected fields. The **`..base`** spread is written **after** explicit fields in the current parser.

```mons
let p = Point { x: 1, y: 2, };
let q = Point { x: 10, ..p, };
```

---

## Worked mini-program

This shape matches what the embedded sample in `src/main.c` exercises:

```mons
struct Point {
    x: int,
    y: int,
}

fn area(self: Point) -> int {
    self.x * self.y
}

const answer: int = 40 + 2;

pub fn add(a: int, b: int) -> bool {
    let x = 1.5f;
    let y = 2.0;
    a + b * 2 == 0 && true || false
}

pub fn mid() -> int {
    let a = [1, 2, 3];
    a[1]
}

pub fn sum_arr() -> int {
    let a = [1, 2, 3];
    let s = 0;
    for x in a {
        s = s + x;
    };
    s
}

pub fn tup() -> int {
    let t = (10, 20);
    t[1]
}

pub fn rect_area() -> int {
    let p = Point { x: 3, y: 4, };
    p.area()
}

pub fn life() -> int {
    answer
}

pub fn shifted() -> int {
    let p = Point { x: 1, y: 2, };
    let q = Point { x: 10, ..p, };
    q.x + q.y
}
```

---

## Implemented in Phase 1

Rough checklist for this repository’s lexer, parser, type checker, and tree-walk evaluator:

- Top-level: **`struct`** (non-generic), **`fn`** / **`pub fn`**, **`const`** / **`pub const`** (annotated).
- Types: primitives, arrays, tuples, named structs, `Option` / `Result` in type syntax (limited in expressions).
- Statements: **`let`**, **`return`**, expression statements, **`for x in expr`** (array or homogeneous tuple).
- Expressions: literals, **`if`**, blocks, operators above, assignment to locals, calls, **method calls** as desugared top-level `fn`, struct literals, **`..base`**, arrays/tuples and indexing.
- Tooling: **`./mons -i`** interactive REPL; **`./mons path.mons`** typecheck-only; **`./mons --reflect FILE`** prints **`pub`** structs, functions, and constants from the type-checked AST; **`./mons --vm-test`** concatenates **`stdlib/core.mons`** with **`tests/vm_smoke.mons`**, then runs a fixed set of bytecode smoke entries (closures, control flow, **`for`**, arrays + tuples + **`[]`**, **`float`/`double`**, structs + **`..base`**, inherent **`impl`**, …); no-arg driver runs the embedded eval demo.
- **Lambdas:** `|x: T| expr`, `|| expr`, type inference on omitted param types; closures work in the **interpreter** and on the **bytecode** VM (after typecheck, inferred parameter types are materialized on the AST for lowering). **`OP_CLOSURE`** / upvalues implement captures on the VM.
- **Inherent `impl`:** `impl StructName { fn f(self: StructName, …) { … } }` is parsed and type-checked; methods are compiled to bytecode and callable as **`recv.method(…)`** when included in **`--vm-test`** programs. **Trait** `impl Trait for Type` is not supported yet.
- **Not** fully wired: **`match`**, **`try` / `catch`**, macro expansion, **`use`**, and much of the full EBNF surface beyond what the README lists.

When in doubt, compare with [mons_grammar.ebnf](mons_grammar.ebnf) and the “implemented today” table in [README.md](README.md).

---

## Planned / full grammar (beyond what runs everywhere today)

The EBNF also describes features intended for later stages, including:

- `use` imports, **trait** `impl` / full trait objects, `match`, **`try` / `catch` / `finally`**, generics on functions and structs, and `macro` definitions and `name!(...)` invocations. **Inherent `impl`** and a **bytecode** subset are already implemented — see **README** / **DESIGN** Phase 2.

Treat items as **design targets** unless the README explicitly lists them as implemented end-to-end on both interpreter and VM.
