# Rust Rewrite Assessment

**Date:** 2026-02-10
**Decision:** Stay in C
**Status:** Decided

## Context

Evaluated whether to rewrite sqlite-diskann in Rust before implementing parallel graph construction and virtual table support. The project is young (one day old) so rewrite cost is low — the question is whether the Rust ecosystem supports the work ahead.

## What Rust Would Help With

- **Memory safety** — eliminates use-after-free, double-free, buffer overflows at compile time (currently caught by ASan/Valgrind)
- **Parallel graph construction** — `rayon`/`crossbeam` catch data races at compile time vs careful pthreads discipline
- **Error propagation** — `Result<T, E>` with `?` vs manual `rc` checking and `goto cleanup`
- **Node.js bindings** — `napi-rs` is production-grade (7.5k stars, used by SWC/Rolldown/Rspack)

## SQLite Virtual Table Ecosystem in Rust

| Crate                     | Stars  | Status                                          | Verdict                                                                                                                                        |
| ------------------------- | ------ | ----------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `rusqlite` (vtab feature) | ~4,000 | Active, v0.38.0                                 | Designed for _consuming_ SQLite, not building loadable extensions. vtab API is a thin `unsafe` wrapper.                                        |
| `sqlite-loadable-rs`      | ~400   | Alpha (0.0.6-alpha.6), intermittent maintenance | Purpose-built for loadable extensions, but writable vtab is explicitly unstable. Maintainer (Alex Garcia) shifted focus to C-based sqlite-vec. |
| `sqlite3_ext`             | ~16    | Abandoned (no commits since 2023)               | Had the best ergonomic design. Dead.                                                                                                           |

### The `unsafe` problem

SQLite's vtab interface is a C API: raw pointers, callback function pointers, `#[repr(C)]` struct layout. Every Rust crate requires substantial `unsafe` at the SQLite boundary. The safety benefits apply to the _internal_ graph algorithms, not the SQLite integration layer.

Both PowerSync and Turso wrote custom FFI bindings rather than using any of these crates. That's telling.

### Writable virtual tables

DiskANN needs INSERT, UPDATE, DELETE, and transaction support. Only `sqlite-loadable-rs` attempts writable vtab support, and it's marked as unstable with an API "likely to change." `rusqlite` has `UpdateVTab` and `TransactionVTab` traits, but they're `unsafe` and require deep understanding of the C vtab lifecycle.

## Trade-offs

| Factor                  | C                              | Rust                                             |
| ----------------------- | ------------------------------ | ------------------------------------------------ |
| SQLite extension API    | Native — zero friction         | FFI boundary, substantial `unsafe`               |
| vtab ecosystem          | Mature, well-documented        | Immature, no stable writable vtab crate          |
| Parallel construction   | pthreads (manual, error-prone) | rayon/crossbeam (ergonomic, compile-time safety) |
| Memory safety           | ASan + Valgrind (runtime)      | Compile-time (except at FFI boundary)            |
| Binary size             | ~17KB hello world              | ~469KB hello world (27x overhead)                |
| Build complexity        | Simple Makefile                | Cargo + build.rs + cc crate for vendored SQLite  |
| Contributor familiarity | C + SQLite is common           | Rust + SQLite internals is niche                 |
| Node.js bindings        | C addon (node-gyp)             | napi-rs (superior)                               |

## Decision Rationale

1. The virtual table interface is where we'll spend significant time, and Rust doesn't help there — it's `unsafe` C interop either way
2. No stable Rust crate supports writable virtual tables in loadable extensions
3. The parallel graph construction (where Rust shines) is a bounded subsystem manageable with pthreads
4. SQLite's extension API is native C — fighting the FFI boundary adds complexity without proportional safety gains

## If Reconsidering Later

- If pthreads proves painful for parallel construction, consider a **hybrid approach**: Rust library for the parallel build phase, called from C via FFI
- Revisit if `sqlite-loadable-rs` reaches 1.0 with stable writable vtab support
- `napi-rs` remains the superior choice for Node.js bindings regardless of core language
