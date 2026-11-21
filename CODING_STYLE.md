# C Coding Style Guide

This document defines a consistent, readable style for C source and
header files. It is intended for both human developers and automated
coding agents.

The rules below favor clarity, portability, maintainability, and
consistency. When a project already has an established convention,
follow that convention unless it conflicts with correctness or an
explicit project requirement.

## 1. General principles

-   Write code that is easy to read, review, debug, and maintain.
-   Prefer simple, explicit constructs over clever or overly compact
    code.
-   Keep formatting consistent within each file and across the project.
-   Keep lines to approximately 80 characters when practical. Wrap long
    expressions and declarations at logical boundaries.
-   Use meaningful names for functions, variables, types, and macros.
-   Avoid unnecessary abstractions, dependencies, and public interfaces.
-   Do not rely on behavior that is undefined, implementation-defined,
    or dependent on undocumented compiler behavior.
-   Follow the project's supported C standard and compiler requirements.

## 2. File organization

A typical source file should use this order:

1.  File identification or copyright notice, if required by the project.
2.  Includes.
3.  File-local type definitions and macros.
4.  File-local function declarations.
5.  File-scope data declarations.
6.  Function definitions.

Keep declarations close to where they are relevant, while ensuring
required prototypes and shared interfaces are available through
appropriate headers.

Do not add revision-control identifiers unless the project explicitly
requires them. Version-control systems already track file history.

## 3. Comments

Use comments to explain intent, constraints, non-obvious behavior, and
important design decisions. Do not use comments to restate code that is
already self-explanatory.

### 3.1 Single-line comments

Use a normal C comment for a short comment:

``` c
/* Check whether the input is valid. */
```

For a particularly important single-line comment, it may be useful to
make the emphasis visually clear:

``` c
/*
 * IMPORTANT: The buffer must remain valid until the operation completes.
 */
```

### 3.2 Multi-line comments

Write multi-line comments as complete sentences. Wrap them at a readable
width and format them as paragraphs:

``` c
/*
 * Validate the input before allocating resources. This keeps the error
 * handling straightforward and avoids partially initialized state.
 */
```

### 3.3 Function comments

Document the purpose of significant functions, especially public
functions and functions with non-obvious behavior. Explain important
preconditions, side effects, ownership rules, and failure behavior where
relevant.

The comment before the program entry point should briefly describe the
program's purpose.

Avoid comments that merely narrate individual statements.

## 4. Header files

### 4.1 Include guards

Every header should protect itself against multiple inclusion. Use a
unique, descriptive guard name based on the header's path or purpose:

``` c
#ifndef PROJECT_SOCKET_H
#define PROJECT_SOCKET_H

/* Header contents. */

#endif /* PROJECT_SOCKET_H */
```

### 4.2 Includes and dependencies

-   Include a header when its declarations, types, or macros are
    directly required.
-   Do not rely on another header to include a dependency indirectly.
-   Avoid unnecessary includes, especially when a forward declaration is
    sufficient.
-   Keep public headers self-contained: a consumer should be able to
    include the header without depending on unrelated include order.
-   Include the headers needed to define public types used by value in
    the interface.
-   For pointers to structures or unions whose definitions are not
    needed, prefer forward declarations.

Example:

``` c
#include <stddef.h>

struct directory_entry;

int process_entry(const char *, struct directory_entry *);
```

### 4.3 Public and private types

Expose structures and unions in public headers only when callers need
their definitions, such as when callers allocate them or access their
members.

Otherwise, keep implementation details in source files or private
headers.

Do not create typedef aliases for structure or union tags solely to
avoid writing `struct` or `union`:

``` c
/* Prefer this. */
struct example;
struct example *create_example(void);

/* Avoid this solely for naming convenience. */
typedef struct example example_t;
```

This keeps pointer-only users from needing unnecessary type definitions
and header dependencies.

### 4.4 Typedefs

Use typedefs selectively when they make an interface clearer or
represent a meaningful type abstraction.

Appropriate uses can include:

-   Function or function-pointer types.
-   Aliases for arithmetic types when the alias communicates a useful
    semantic distinction.
-   Types whose representation legitimately varies between supported
    targets or configurations.

Do not use `typedef void *` as an opaque handle when a forward-declared
structure pointer can provide stronger type checking:

``` c
struct connection;

/* Prefer a typed opaque pointer. */
struct connection *connection_create(void);
```

Avoid typedefs that hide pointer indirection or make declarations harder
to understand.

### 4.5 External declarations

Put declarations for externally linked functions and objects in an
appropriate header so that both defining and using source files share
the same declaration.

Do not repeat external declarations independently in source files.

The `extern` keyword is generally unnecessary on function prototypes:

``` c
int process_data(const char *, size_t);
```

Use `extern` where appropriate for external object declarations:

``` c
extern int global_status;
```

Keep declarations for file-local (`static`) functions in the source file
that defines them.

## 5. Include ordering

Group includes by purpose, with a blank line between groups. Within each
group, sort includes consistently, normally in lexicographical order.

A common order is:

1.  Project or platform interface headers, if required.
2.  Standard library headers.
3.  Third-party library headers.
4.  Project-local headers.

Use angle brackets for system and external headers, and double quotes
for project-local headers:

``` c
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "pathnames.h"
```

If a project has a required include order for a particular library or
subsystem, follow that order and document the reason.

Do not add headers that are not needed.

## 6. Declarations and definitions

### 6.1 Function prototypes

Declare file-local functions near the top of the source file. Do not
include parameter names in prototypes unless they improve readability or
are required by the project's documentation conventions:

``` c
static char *format_value(int, size_t);
static int read_record(const char *, struct record *);
static void print_usage(void);
```

For long prototypes, wrap parameters at logical boundaries. Align
continuation lines consistently:

``` c
static int read_record(const char *path, struct record *record,
                       struct options *options, size_t *records_read);
```

Consistency is more important than a particular alignment scheme.

### 6.2 Function definitions

Put the return type on its own line, followed by the function name and
parameter list:

``` c
static int
process_record(const char *path, size_t limit)
{
        /* Function body. */
}
```

Place the opening brace of the function body on the line following the
function declaration. Put the closing brace at the start of the line.

### 6.3 Variable declarations

Declare each variable with its own type and on its own line when that
improves clarity. Multiple variables of the same type may share a line
when the declaration remains easy to read.

Remember that the `*` belongs to each declarator, not to the base type:

``` c
int *first;
int second;

/* Also valid when the declarations are simple and clear. */
int count, total;
```

Avoid declarations that make pointer types ambiguous.

Prefer declaring variables near their first use when doing so keeps
scopes narrow and the code easy to follow. In functions with complex
control flow, group declarations where that improves readability.

Avoid initializing variables to arbitrary placeholder values when they
can be initialized meaningfully at their first use. Always initialize a
variable before reading it.

### 6.4 Structure layout

Group structure members by purpose. Where practical, order members to
reduce padding and alignment overhead, while keeping related fields
together and the layout understandable.

A useful general approach is to group members by decreasing alignment or
size, then by related purpose and name. Do not sacrifice clarity for
minor memory savings.

Give each member its own declaration:

``` c
struct example {
        int count;
        int status;
        char *name;
        char *description;
};
```

Use bitfields sparingly. They can introduce portability and layout
concerns. If several bitfields form one logical group, they may share a
declaration line when that makes the relationship clearer.

For major structures, place the definition near the top of the source
file or in a shared header if multiple source files require it.

Use meaningful member-name prefixes when they help distinguish fields or
clarify ownership.

Do not create typedef aliases for structure or union types solely for
convenience.

### 6.5 Fixed-width integer types

When a specific integer width is required, prefer the standard
fixed-width types from `<stdint.h>`, such as `uint32_t` and `int64_t`,
rather than implementation-specific alternatives.

Use ordinary `int`, `unsigned int`, `size_t`, or other standard types
when the value's meaning does not require a fixed width.

## 7. Macros and enumerations

### 7.1 Macros

Use uppercase names for macros. Parenthesize macro parameters and
expression expansions to avoid precedence surprises:

``` c
#define DOUBLE(value) ((value) * 2)
```

Avoid macros with side effects. Prefer an inline function when it
provides equivalent behavior with better type checking and clearer
debugging.

For multi-statement macros, use a `do { ... } while (0)` wrapper so the
macro behaves like a single statement:

``` c
#define UPDATE_VALUES(a, b, x, y)       \
        do {                            \
                (a) = (x) + (y);        \
                (b) = (y) + 2;          \
        } while (0)
```

Keep continuation backslashes aligned.

If a macro is simply an inline expansion of a function, use the same
base name in uppercase for the macro and lowercase for the function,
when that convention is useful and unambiguous.

### 7.2 Conditional macros

When a macro is enabled only in certain builds, keep the conditional
definition straightforward:

``` c
#ifdef ENABLE_DEBUG
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif
```

Use the project's established logging or diagnostic facilities where
available.

### 7.3 Enumerations

Use uppercase names for enumeration constants. Do not add a trailing
comma after the final element unless the project's formatting convention
explicitly permits it:

``` c
enum state {
        STATE_READY,
        STATE_RUNNING,
        STATE_STOPPED
};
```

Choose names that make the meaning and scope of each value clear.

## 8. Indentation and whitespace

-   Use eight spaces for each indentation level.
-   Do not mix tabs and spaces for indentation.
-   Indent the contents of blocks consistently.
-   Indent continuation lines enough to make the relationship to the
    continued expression clear.
-   Put a space after control-flow keywords such as `if`, `for`,
    `while`, and `switch`.
-   Do not put a space between a function name and its opening
    parenthesis.
-   Put spaces around binary operators.
-   Do not put spaces between unary operators and their operands.
-   Do not put spaces between a cast or `sizeof` and its parenthesized
    operand.

Examples:

``` c
if (count > 0) {
        process(count);
}

result = left + right;
flags = !is_valid;
value = (int)source;
size = sizeof(*value);
```

Use blank lines to separate logical sections, not between every
statement.

## 9. Control flow

### 9.1 Braces

Braces are required for multi-statement blocks. For single-statement
blocks, braces may be omitted when the result remains clear. Prefer
braces when they prevent ambiguity or make future changes safer.

Keep `else` and `else if` on the same line as the preceding closing
brace:

``` c
if (condition) {
        handle_first_case();
} else if (other_condition) {
        handle_second_case();
} else {
        handle_default_case();
}
```

### 9.2 Switch statements

Indent the `switch` body, but do not add an extra indentation level to
`case` labels. Indent statements within each case:

``` c
switch (option) {
case 'a':
        enable_feature();
        /* FALLTHROUGH */
case 'b':
        set_mode();
        break;
case 'q':
        quit_program();
        break;
default:
        print_usage();
        break;
}
```

Mark intentional fallthrough with a clear comment or the
compiler-supported fallthrough annotation used by the project.

Always include a `break`, `return`, or other clear control-flow
termination where needed.

### 9.3 Loops

Use `for (;;)` for an intentional infinite loop:

``` c
for (;;) {
        process_next_item();
}
```

Keep loop variables scoped to the loop when the supported C standard
allows it:

``` c
for (size_t i = 0; i < count; ++i)
        process_item(i);
```

Use empty loop clauses when appropriate, but make the intent clear.

Avoid placing declarations inside deeply nested blocks unless the
function's complexity makes local declaration necessary.

### 9.4 No-op statements

Use an explicit no-op statement when a loop body is intentionally empty:

``` c
while (check_condition())
        continue;
```

### 9.5 Unreachable code and fallthrough

Mark intentional switch fallthrough clearly. If code is genuinely
unreachable, use the project's established unreachable-code annotation
or a concise explanatory comment.

Do not add unreachable-code markers as a substitute for understanding or
fixing control flow.

## 10. Expressions and operators

-   Do not put spaces around unary operators.
-   Put spaces around binary operators.
-   Avoid excessive parentheses, but use them when they make precedence
    or intent clearer.
-   Do not use `!` as a substitute for an explicit comparison when the
    operand is not clearly boolean.

Prefer:

``` c
if (*p == '\0')
        return NULL;
```

Over:

``` c
if (!*p)
        return NULL;
```

Use parentheses to make complex expressions understandable:

``` c
result = ((left + right) > limit) || (enabled && ready);
```

Do not compress complex expressions merely to fit them on one line.

## 11. Pointers, casts, and `sizeof`

### 11.1 Pointer checks

Use `NULL` for null pointer values. Check pointers explicitly when doing
so improves readability and makes the intended type clear:

``` c
if ((entry = find_entry(name)) == NULL)
        return -1;
```

Avoid relying on implicit pointer-to-boolean conversion when an explicit
null comparison is clearer.

Do not cast `NULL` unnecessarily. In variadic calls, ensure pointer
arguments have the correct pointer type; an untyped null constant may
otherwise be interpreted as an integer argument.

### 11.2 Casts

Avoid unnecessary casts. Use a cast only when it is required by the
language or clearly communicates an intentional conversion.

Do not cast the return value of a function returning `void *` merely to
convert it to another object-pointer type in C.

### 11.3 `sizeof`

Write `sizeof` with parentheses around its operand:

``` c
size_t bytes = sizeof(*buffer);
```

Prefer `sizeof(*pointer)` over `sizeof(type)` when allocating or sizing
an object through a pointer. This keeps the allocation correct if the
pointed-to type changes.

## 12. Error handling and return values

-   Check the return value of operations that can fail.
-   Handle errors close to the operation that can produce them.
-   Use the project's established error-reporting and logging
    facilities.
-   Avoid duplicating error-formatting and cleanup logic when a clear
    helper function is appropriate.
-   Return standard success and failure values consistently with the
    function's documented contract.
-   For a program's `main` function, prefer `EXIT_SUCCESS` and
    `EXIT_FAILURE` from `<stdlib.h>` over unexplained numeric exit
    codes.
-   Avoid calling `exit()` from `main` when returning the status
    directly is clearer.
-   Do not invent arbitrary error codes when standard error values or a
    documented project-specific error scheme are available.

For system calls that document `-1` as the failure result, compare
against `-1` rather than using a broad `< 0` test unless the API
specifies otherwise.

Validate numeric input carefully. Check conversion errors, range limits,
and whether the entire input was consumed.

Example:

``` c
errno = 0;
value = strtol(text, &end, 10);

if (text == end || *end != '\0' ||
    (errno == ERANGE) || value <= 0) {
        report_error("invalid numeric value");
        return EXIT_FAILURE;
}
```

Use safe, bounded formatting functions. Prefer `snprintf` over
`sprintf`, and check whether the output was truncated when that matters.

## 13. Formatting output and integer types

Include `<inttypes.h>` when formatting fixed-width integer types with
the standard format macros.

For values that may be wider than `long`, use `intmax_t` or `uintmax_t`
with the `%j` length modifier:

``` c
printf("Size: %jd (%#ju)\n",
       (intmax_t)signed_size,
       (uintmax_t)unsigned_size);
```

For fixed-width integer types, use the corresponding format macros:

``` c
printf("Count: %" PRIu32 "\n", count);
printf("Flags: %#" PRIx64 "\n", flags);
```

Use the matching `<inttypes.h>` scan macros when reading fixed-width
integer types with the `scanf` family. Prefer parsing functions such as
`strtol` or `strtoumax` when they provide clearer error handling.

## 14. Variadic functions

Include `<stdarg.h>` when implementing a function that accepts a
variable number of arguments.

Use `va_start`, `va_arg` as needed, and `va_end`:

``` c
#include <stdarg.h>

void
log_message(const char *format, ...)
{
        va_list args;

        va_start(args, format);
        /* Process the arguments. */
        va_end(args);
}
```

Ensure every `va_start` is paired with a corresponding `va_end` before
the function returns.

## 15. Command-line interfaces

Use a consistent option-parsing approach, preferably the project's
established parser or a standard facility such as `getopt` where
available.

Keep option parsing, option handling, and usage output synchronized.

-   List options in a consistent order in both the parser and usage
    text.
-   Document every supported option.
-   Validate option arguments and numeric ranges.
-   Handle unknown or malformed options explicitly.
-   Use the program's actual invocation name in usage output rather than
    hardcoding it when a portable mechanism is available.

A usage function should print a concise synopsis and explain options in
the same style as the project's documentation.

A common synopsis order is:

1.  Options without operands.
2.  Options with operands.
3.  Required positional arguments.
4.  Optional positional arguments.

Use brackets for optional elements, braces for grouped alternatives when
appropriate, and `|` to separate alternatives.

## 16. Portability

-   Prefer standard C library facilities when they meet the requirement.
-   Isolate platform-specific behavior behind small, clearly named
    interfaces.
-   Do not assume that a function, macro, type, or header exists on
    every target unless the project's supported platforms guarantee it.
-   Do not rely on implementation-specific type sizes or structure
    layouts.
-   Use feature checks and project configuration where platform
    differences must be handled.
-   Do NOT use GCCisms or BSDisms as the only code path. No attribute keywords,
    no BSD keywords unless include guarded. 
-   Document unavoidable platform assumptions.

When portability matters, compile and test on the project's supported
compilers and targets.

## 17. Guidance for automated coding agents

Automated agents modifying C code under this guide must:

1.  Follow the formatting and organization already present in the
    surrounding code unless it clearly conflicts with this guide.
2.  Make the smallest coherent change that satisfies the task.
3.  Avoid unrelated refactoring, renaming, or formatting churn.
4.  Use existing project abstractions, error handling, logging, and
    memory-management conventions.
5.  Add required declarations to the correct header rather than
    duplicating declarations in source files.
6.  Include headers for directly used declarations; do not rely on
    transitive includes.
7.  Check allocation, I/O, parsing, and other fallible operations.
8.  Preserve API and ABI compatibility unless the task explicitly
    requests a change.
9.  Avoid introducing non-standard APIs or compiler extensions without
    confirming that the project supports them.
10. Keep comments accurate and update them when behavior changes.
11. Do not claim code was compiled or tested unless those actions were
    actually performed.
12. Report important assumptions, limitations, and tests performed in
    the final summary.

## 18. Quick reference

  -----------------------------------------------------------------------
  Topic                               Convention
  ----------------------------------- -----------------------------------
  Indentation                         Four spaces

  Line length                         Aim for 80 characters

  Function definitions                Return type on its own line;
                                      opening brace on the next line

  Control flow                        Space after `if`, `for`, `while`,
                                      and `switch`

  Function calls                      No space before `(`

  Binary operators                    Spaces around the operator

  Unary operators                     No surrounding spaces

  Macros                              Uppercase; parenthesize parameters
                                      and expressions

  Enumerators                         Uppercase

  Header guards                       Unique, descriptive names

  Includes                            Group by purpose; sort consistently

  Public structures                   Expose only when callers need the
                                      definition

  Opaque types                        Prefer forward-declared structure
                                      pointers

  Fixed-width integers                Use `<stdint.h>` types

  Object sizing                       Prefer `sizeof(*pointer)`

  Pointer checks                      Use explicit `== NULL` where
                                      clarity benefits

  Output formatting                   Prefer bounded formatting such as
                                      `snprintf`

  Exit status                         Use `EXIT_SUCCESS` and
                                      `EXIT_FAILURE`

  Comments                            Explain intent and non-obvious
                                      behavior
  -----------------------------------------------------------------------

Consistency, correctness, and clarity take precedence over mechanically
applying any single formatting rule.
