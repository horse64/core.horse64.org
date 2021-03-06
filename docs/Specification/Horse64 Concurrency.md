
# Concurrency in Horse64

## Basics

For concurrency, Horse64 allows the use of async/await. **Note: these work
differently than e.g. in the Python programming language,** where they
wouldn't provide true concurrency - in Horse64, they do.

To achieve that, Horse64 uses `async my_function_call(myarg1, myarg2)` to
spawn the given function in a separate [execution context](
#execution-context). These execution contexts have semi-preemptive,
occasionally truly concurrent execution. Aside from not accessing global
variables, any sort of activity can be done inside such an `async` call
in its separate execution context and it will normally not block the
`main` execution context (that was spawned by the initial `main` program
entrypoint). The restriction on globals is enforced with the help of the
[async property](#async-property-for-functions) for functions and classes.
Please note all parameters are deep copied for `async` calls due to
[heap separation](#heap-separation).

For the exact syntax of async calls, [see the syntax specification](
Horse64.md#asyncawait) or [the grammar](Horse64%20Grammar.md).


## Execution Context

The execution contexts in Horse64 are essentially semi-preemptive green
threads, spread out on a limited number of real OS thread workers.
"Green thread" basically means that the [horsevm runtime](
../Misc%20Tooling/horsevm.md) keeps a separate stack for each, and
semi-preemptive means that with predefined yield points that are designed
to avoid infinitely blocking scenarios, the green threads/execution contexts
will be automatically cycled through to avoid any of them stalling for
too long. With multiple true OS thread workers each running one of these
execution contexts, true concurrency is achieved.

Essentially this means in practice that unlike Python/JS async/await,
in Horse64 you can just do blocking things inside an `async` call
and they will generally not block the caller even without a yield,
and you don't need to pay attention to only call non-blocking and
yielding functions. You also get true concurrency in addition. However,
the functions running on any but the `main` execution context need
to fulfill the [async property](#async-property-for-functions).


## `async` property for functions

The `async` property is auto-applied at compile time to any
function that 1. doesn't access any global variables outside of
simple consts, and 2. that doesn't have any inner calls, no matter
whether conditional or not, that can be compile-time traced to a
function that is *not* `async`.
(This is checked transitively.)

Please note Horse64's `async` property therefore says nothing about
a function's yielding behavior or non-blocking nature, or whether it
*has* to be run async. (Which is usually the case for Python/JS/C# async.)
It only says whether a function or class *may* be used asynchronosly.

Since the compiler will assume functions as `async` if in doubt
which can yield false positives, and it's also not always obvious to the
programmer due to the transitive nature whether the compiler will consider
a particular function to be `async`, you are generally advised to
heavily use the `async` and `noasync` keywords to mark functions
explicitly:

```horse64
func my_async_func async {
    print("Hello from async-compatible land!")
}

func my_incompatible_func noasync {
    print("No async fun here!")
}

func main {
    async my_async_func()  # works because it's marked as 'async'
    async my_incompatible_func()  # forbidden since this one is 'noasync'
}
```

The effect of explicitly marking a function as `async` is as follows:

- If the function is determined by `horsec` to definitely have a
  call to a `noasync` function or access a global variable,
  a compile-time error will occur. This allows you to rectify the issue
  before it would have led to a runtime error.

- Any documentation generators can include the `async` property
  into the resulting API listing, and should do so. This allows other
  programmers to see which of your functions are considered safe in
  terms of global state side effects.

- If the function calls a `noasync` func at runtime anyway while running
  in a separate execution context (which is possible if e.g. a parameter
  passed in inadvertendly refers to a `noasync` function, since `horsec`
  will not reliably detect this possibly happening) then an
  `InvalidNoasyncCallError` will be raised at runtime.
  This is considered a programming error and should not happen by a
  Horse64 program's design.

The effect of explicitly marking a function as `noasync` is as follows:

- The function is hereby declared to be considered **not** `async`.
  As a consequence, the function will now be blocked for use with
  or nested inside any `async` calls. Any attempts to do so will cause
  either a compile-time error if detected at compile time, or otherwise
  a `InvalidNoasyncCallError` at runtime, which as specified above is
  considered a programming error.

- Any documentation generators can include the `noasync` property
  into the resulting API listing, similarly as for `async` above.

Without any explicit markings, as described above `horsec` will still
apply one of these two categories for each function but might err in
favor of the `async` side. This is meant to save you from needing to
specify this property for each tiny helper function in your program,
even if the category is obvious when looking at it. **In cases where
it is not obvious to you, you should always consider applying
`async`/`noasync` explicitly.**


## Heap separation

Each execution context will not only have its own stack, but also
its own heap. All objects passed into the parameters of an `async`
call are `.clone()`d when the call happens, and since all functions
used inside an `async` call must have the `async` property they
are prevented from accessing any global state. Therefore, all global
variables transparently belong to the initial `main` execution context,
and all potentially concurrent `async` runs live in their own little
world. Communication happens via `await`ed return values (which are
`.clone()`d back into the original caller's execution context), and
via [pipes](#pipe).


## Pipe

FIXME write this


## await

FIXME write this

---
This documentation is CC-BY-SA-4.0 licensed.
( https://creativecommons.org/licenses/by-sa/4.0/ )
Copyright (C) 2020-2021 Horse64 Team (See AUTHORS.md)
