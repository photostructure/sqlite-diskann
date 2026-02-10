# C Best Practices: Robust, Portable, Maintainable Code

**Target:** Modern C (C11+) for production systems
**Philosophy:** Fail visibly, minimize coupling, no bogus guardrails

---

## 1. Memory Management

### Ownership and Lifecycle

Every allocation must have a clear owner. Document ownership when not obvious:

- `// Returns malloc'd string - caller must free`
- `// Returns borrowed pointer - do NOT free`
- `// Takes ownership - do NOT free after calling`

**Always check allocations:**

```c
void *buf = malloc(size);
if (!buf) return ERROR_NOMEM;  // Fail visibly
```

**Defensive post-free pattern:**

```c
free(ptr);
ptr = NULL;  // Prevents double-free, catches use-after-free bugs
```

### Centralized Cleanup with Goto

```c
int process(const char *path) {
    FILE *f = NULL; void *buf = NULL; int rc = -1;
    if (!(f = fopen(path, "r"))) goto cleanup;
    if (!(buf = malloc(SIZE))) goto cleanup;
    // ... work
    rc = 0;
cleanup:
    free(buf); if (f) fclose(f); return rc;
}
```

**Benefits:** Single exit, no leaks, explicit order. **Stack vs heap:** Prefer stack when possible. Never use VLAs (`char buf[n]`).

---

## 2. Error Handling

### Fail Early and Visibly

**Don't mask errors with defaults:**

```c
// Bad: int get_age(User *u) { return u ? u->age : 0; }  // 0 not valid!
// Good: int get_age(User *u, int *age) { if (!u || !age) return ERR; *age = u->age; return OK; }
```

**Key principle:** When assumptions break, fail loudly. No bogus defaults.

### Return Codes and Output Parameters

Return status codes (0=OK, negative=error), use output params for values:

```c
int parse(const char *s, long *result) {
    if (!s || !result) return ERR_NULL;
    errno = 0; char *end; long v = strtol(s, &end, 10);
    if (errno) return ERR_OVERFLOW;
    if (end == s) return ERR_INVALID;
    *result = v; return OK;
}
```

### Assert vs Runtime Checks

**Public API:** Validate all inputs with runtime checks. **Internal:** Assert preconditions (debug only).

```c
int public_api(const char *d) { if (!d) return ERR_NULL; return internal(d); }
static void internal(const char *d) { assert(d); /* work */ }
```

---

## 3. Cross-Platform Portability

### Platform Detection

```c
#if defined(_WIN32)
    #define PLATFORM_WINDOWS
#elif defined(__linux__)
    #define PLATFORM_LINUX
#elif defined(__APPLE__)
    #define PLATFORM_MACOS
#endif

#if defined(_MSC_VER)
    #define COMPILER_MSVC
#elif defined(__clang__)
    #define COMPILER_CLANG
#elif defined(__GNUC__)
    #define COMPILER_GCC
#endif
```

### Portable Types

**Use `<stdint.h>` for fixed-width types:**

```c
int32_t  count;    // Exactly 32 bits
uint64_t offset;   // Exactly 64 unsigned
size_t   length;   // Pointer-sized
```

**Never assume:** `sizeof(int)==4`, `sizeof(long)==8`, `sizeof(void*)==sizeof(size_t)`

### Printf Format Specifiers

**Use `<inttypes.h>` macros:**

```c
uint64_t big = 123456ULL;
printf("%" PRIu64 "\n", big);  // Portable

int32_t num = -42;
printf("%" PRId32 "\n", num);
```

Common: `PRId32`, `PRIu64`, `PRIx64` (hex), `PRIX64` (uppercase).

---

## 4. Modern C Standards

### Static Assertions (C11+)

```c
static_assert(sizeof(int) >= 4, "int must be 32+ bits");
static_assert(sizeof(void*) == sizeof(size_t), "size_t mismatch");
```

### Type-Generic Macros (C11+)

```c
#define max(a, b) _Generic((a), \
    int: max_int, \
    long: max_long)(a, b)
```

### What to Avoid

- **VLAs:** `char buf[n]` - stack overflow risk
- **Deprecated:** `gets()` (use `fgets()`), `strcpy()` (use `strncpy()`/`strlcpy()`)
- **Implicit declarations:** Always include headers

---

## 5. Defensive Programming

### When to Be Defensive

**Public APIs:** Validate all inputs. **Internal code:** Trust preconditions, use assert.

```c
// Public: validate everything
int vec_create(vector_t **out, size_t cap) {
    if (!out || cap == 0 || cap > MAX) return ERR_INVALID;
    if (!(*out = malloc(sizeof(vector_t)))) return ERR_NOMEM;
    return OK;
}

// Internal: assert preconditions, no defensive checks
static void resize(vector_t *v) {
    assert(v && v->data);
    void *new = realloc(v->data, v->cap * 2);
}
```

**Don't mask errors with defaults:** Return explicit errors, not magic values like 0.

---

## 6. Code Organization

### Headers and Encapsulation

**Guards:** `#pragma once` or `#ifndef HEADER_H / #define HEADER_H / #endif`

**Opaque pointers:** Forward-declare structs in headers, define in implementation:

```c
// Header: typedef struct Database Database; Database *db_open(const char *path);
// Impl: struct Database { FILE *file; void *cache; };  // Hidden internals
```

**Static functions:** Mark all internal functions `static` (file-scope only).

---

## 7. Testing & Verification

### Compiler Warnings

```bash
gcc -std=c17 -Wall -Wextra -Werror -pedantic \
    -Wconversion -Wshadow -Wstrict-prototypes
```

### Static Analysis

```bash
clang-tidy src/*.c -- -std=c17
cppcheck --enable=all src/
```

### Dynamic Analysis

```bash
# Valgrind
valgrind --leak-check=full ./program

# AddressSanitizer
gcc -fsanitize=address -g prog.c
./a.out

# UndefinedBehaviorSanitizer
gcc -fsanitize=undefined -g prog.c
```

### Fuzzing

```bash
afl-gcc program.c -o program
afl-fuzz -i inputs/ -o findings/ ./program
```

---

## Quick Reference Checklist

**Memory:**

- ✅ Every malloc has paired free
- ✅ Allocations checked for NULL
- ✅ Pointers nulled after free
- ✅ Cleanup handles partial init

**Errors:**

- ✅ Explicit error codes returned
- ✅ No silent defaults on failure
- ✅ Public APIs validate inputs
- ✅ Internal code uses assert

**Portability:**

- ✅ `<stdint.h>` types (int32_t)
- ✅ `<inttypes.h>` printf (PRId64)
- ✅ No sizeof assumptions
- ✅ Platform code in #ifdef

**Modern C:**

- ✅ static_assert for compile checks
- ✅ Avoids VLAs
- ✅ Avoids gets/strcpy
- ✅ C11+ standard

**Testing:**

- ✅ -Wall -Wextra -Werror
- ✅ ASan/valgrind clean
- ✅ Static analysis passes

---

## Sources

**Memory:** [Tutorial](https://www.tutorialspoint.com/cprogramming/c_memory_management.htm) | [Opensource.com](https://opensource.com/article/21/8/memory-programming-c) | [Surfside](https://www.surfsidemedia.in/post/memory-management-best-practices-in-c)

**Standards:** [C17 Guide](https://thelinuxcode.com/c17-standard-a-practical-production-focused-guide-2026-edition/) | [Modern C Book](https://link.springer.com/book/10.1007/978-3-031-45361-8) | [GitHub](https://github.com/AnthonyCalandra/modern-c-features)

**Defensive:** [Red Hat](https://redhat-crypto.gitlab.io/defensive-coding-guide/) | [BrainsToBytes](https://www.brainstobytes.com/defensive-programming-fundamentals) | [UC Davis](https://nob.cs.ucdavis.edu/bishop/secprog/robust.html)

**Design:** Kent Beck's "Four Rules of Simple Design" and "Tidy First"
