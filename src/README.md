This directory contains the source code for the VM which can be embedded in a "runtime".

See [src/main.c](main.c) for a minimal example "runtime".

All code in this directory is C11 compatible.

# Architecture

The VM is stack based with variable width instructions. A `vm_value_t` is used to represent all types. Heap allocated types store a pointer in `vm_value_t` which is managed by an RC system. Any heap allocated types (like Array/Map/Promise) which can store a reference to another heap allocated type are tracked by a GC system to prevent dependency cycles from artificially lengthening the life of an object[^1].

Native functions can be passed to the VM which make up the functionality of a given "runtime". All function calls are resolved to an index in the function tables (both for native functions and bytecode functions) by the compiler AOT.

A job queue abstraction is used to handle `Promise` based async. `Promises` are resolved _but do not_ execute their `.then()` callbacks until `bool run_promise_jobs(vm_t* vm, vm_job_queue_t* job_queue)` is called ensuring deterministic ordering [^2].

The VM is designed to be as simple as possible to ease auditing of the code for bugs and limit its effect on performance, walking a fine line between a dynamic VM (like JavaScript) and a statically compiled runtime (like C#).

There are a number of built-in types provided by the VM (Array/Map/Promise/String) [^3].

[^1]: While the allocations are tracked in a linked list, the mark-sweep GC has not been fully implemented at this time.
[^2]: This is the same approach used by JavaScript runtimes using the microtask queue.
[^3]: Eventually this should be split into a "core VM" with just the bytecode interpreter/RC&GC, the "core runtime" with support for these built-in types, and the "embedded runtime".