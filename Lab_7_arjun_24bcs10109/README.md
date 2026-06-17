# Lab 7 — SQL `WHERE` Parsing & Evaluation

**Name:** Arjun
**Roll No.:** 24BCS10109

## Goal

Take one SQL query —

```sql
SELECT name FROM students WHERE marks >= 80 AND (age < 20 OR id = 5)
```

— and build **two completely different parsers** for its `WHERE` clause,
then run both against the same in-memory table and check that they
return the same rows. Doing it twice forces you to confront the
question "where does operator precedence actually live?" — once as a
**data structure** (a precedence table), and once as **code structure**
(the call graph of the parser itself).

## Layout

```
Lab_7_arjun_24bcs10109/
├── README.md                  ← this file
├── shunting_yard/
│   ├── main.cpp               ← infix → RPN + stack evaluator
│   └── Makefile               ← `make`, `make run`, `make clean`
└── recursive_descent/
    ├── main.cpp               ← lexer, AST parser, tree-walk evaluator
    └── Makefile               ← `make`, `make run`, `make clean`
```

## The two implementations

### 1. Shunting-Yard — `shunting_yard/main.cpp`

Dijkstra's classic algorithm. Tokens flow from left to right through
an operator stack governed by a precedence table; operands stream into
an output list in **Reverse Polish Notation**. A second one-pass walk
over the RPN with an int stack evaluates it for one row.

```text
Infix WHERE : marks >= 80 AND (age < 20 OR id = 5)
Postfix RPN : marks 80 >= age 20 < id 5 = OR AND
```

Precedence is a small lookup:

| Operator             | Precedence |
|----------------------|------------|
| `OR`                 | 1 |
| `AND`                | 2 |
| `=`, `<`, `>`, `<=`, `>=` | 3 |

The evaluator never sees parentheses — they've already done their job
during the infix-to-RPN conversion. Tight, fast, no recursion, no heap.

### 2. Recursive-Descent — `recursive_descent/main.cpp`

A handwritten lexer plus three mutually-recursive parsing functions:

```text
parseOr     → parseAnd ( OR  parseAnd )*
parseAnd    → parseCmp ( AND parseCmp )*
parseCmp    → '(' parseOr ')'   |   Ident CmpOp Number
```

Because `parseOr` calls `parseAnd` calls `parseCmp`, the call graph
itself enforces precedence. There is **no precedence table** anywhere
in the file. The output is a small AST that gets walked recursively
during evaluation:

```text
AND
  >=
    marks
    80
  OR
    <
      age
      20
    =
      id
      5
```

Each interior node is a binary operator; leaves are either column
references or integer literals.

## Build & run

Each subfolder ships its own `Makefile` with three targets — `make`
(build), `make run` (build + execute), `make clean` (remove the
binary):

```bash
cd shunting_yard     && make run
cd recursive_descent && make run
```

Both compile with `g++ -std=c++17 -Wall -Wextra -O2`, no warnings.

## The test table

| id | name  | age | marks |
|----|-------|-----|-------|
| 1  | Priya | 19  | 88    |
| 2  | Rohan | 22  | 67    |
| 3  | Sneha | 20  | 91    |
| 4  | Arjun | 23  | 74    |
| 5  | Meera | 21  | 95    |
| 6  | Karan | 18  | 59    |

Working through the `WHERE` clause by hand:

- `marks >= 80` is true for **Priya, Sneha, Meera**.
- `age < 20 OR id = 5` is true for **Priya** (age 19), **Karan** (age 18),
  and **Meera** (id 5).
- The intersection: **Priya** and **Meera**.

## Captured output

### Shunting-yard

```text
Lab 7 — shunting-yard (Arjun, 24BCS10109)

Infix WHERE : marks >= 80 AND (age < 20 OR id = 5)
Postfix RPN : marks 80 >= age 20 < id 5 = OR AND

Matching rows (SELECT name ...):
  Priya  (id=1, age=19, marks=88)
  Meera  (id=5, age=21, marks=95)
```

### Recursive-descent

```text
Lab 7 — recursive-descent parser (Arjun, 24BCS10109)

Query: SELECT name FROM students WHERE marks >= 80 AND (age < 20 OR id = 5)

WHERE as an AST (precedence encoded in tree shape):
AND
  >=
    marks
    80
  OR
    <
      age
      20
    =
      id
      5

Matching rows (SELECT name FROM students):
  Priya
  Meera
```

Both return the same two rows — so the two parsers agree, which is
exactly the sanity check we wanted.

## Side-by-side comparison

| Aspect                  | Shunting-yard            | Recursive-descent |
|-------------------------|--------------------------|-------------------|
| Output shape            | Flat token list (RPN)    | Tree (AST) |
| Precedence lives in     | Lookup table             | Grammar / call graph |
| Memory                  | Two `std::vector`s, no heap nodes | One `unique_ptr<Node>` per AST node |
| Code size               | ~150 lines               | ~250 lines |
| Recursion               | None                     | Yes, one frame per nesting level |
| Easy to extend with…    | New operators (add to precedence table) | New node types, optimization passes, type checks |
| Real-world analogue     | Pocket calculators, formula bars | Postgres, MySQL, every modern compiler |

Neither is "better" — they're tools for different jobs. Shunting-yard
is unbeatable when you only need to **evaluate** an expression and
don't care about its structure afterwards. Recursive-descent wins the
moment you want to **do something with the parse tree** — print
`EXPLAIN`, push predicates into an index scan, run an optimizer, swap
`AND a b` for `AND b a` because `b` is cheaper to evaluate, etc. Real
DBMSes parse into an AST because every step after parsing — query
rewriting, planning, costing, code-gen — wants tree access.

## What I took away

- **Precedence is the whole problem.** Both implementations are tiny
  except for the piece that decides "does `AND` bind tighter than `OR`?".
  In shunting-yard that's a table; in recursive-descent it's the order
  in which parsing functions call each other. Same idea, different
  expression.
- **RPN is precedence-free by construction.** Once a clause is in RPN,
  the evaluator has zero decisions to make — just push or apply. That's
  why HP calculators used RPN: no parens, no `(`/`)` keys.
- **An AST is what you build when you want to keep transforming the
  query later.** Optimisations like predicate pushdown, common-subexpression
  elimination, and constant folding all want a tree. RPN throws that
  structure away.
- **Short-circuit evaluation falls out naturally** in the AST walker
  (`a && b`, `a || b` in C++) but not in the RPN walker — RPN
  evaluates every leaf because the stack must be balanced. For boolean
  predicates in a real query engine, the short-circuit is a real win
  when one branch is expensive (e.g., a subquery).
- **Both finished agreeing on `{Priya, Meera}`**, which is the only way
  to know either parser is correct. Cross-checking two completely
  independent implementations of the same spec is a cheap, very strong
  test.
