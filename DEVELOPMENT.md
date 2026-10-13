# Development

Wolaz builds with [Bazel](https://bazel.build/), using the same layout and
conventions as Google's protobuf repository.

## Requirements

- **Bazel**, via the host's `bazel` (bazelisk). Bazelisk reads `.bazelversion`
  and fetches that exact release, currently **8.8.1**. Bazel 9 does not work:
  the `compile_commands.json` extractor still calls `native.py_binary`, which
  Bazel 9 removed.
- **A host C++ compiler.** Bazel auto-detects it; GCC (`/usr/bin/gcc`) is what
  is tested.

Everything else — protobuf 36.2, `protoc`, abseil, the conformance test
runner — comes from the Bazel Central Registry and is built from source by
Bazel. Nothing needs to be installed system-wide.

```sh
bazel --version        # bazel 8.8.1
```

The first build compiles protobuf and abseil and takes a few minutes. Later
builds are incremental.

## Build

Everything:

```sh
bazel build //...
```

Individual pieces:

The runtime library:

```sh
bazel build //runtime:wolaz
```

The protoc plugin:

```sh
bazel build //plugin:protoc-gen-wolaz
```

The conformance testee:

```sh
bazel build //conformance:wolaz_conformer
```

Outputs land under `bazel-bin/`:

| Target | Output |
|---|---|
| `//runtime:wolaz` | `bazel-bin/runtime/libwolaz.a` |
| `//plugin:protoc-gen-wolaz` | `bazel-bin/plugin/protoc-gen-wolaz` |
| `//conformance:wolaz_conformer` | `bazel-bin/conformance/wolaz_conformer` |

Optimised or debug builds:

```sh
bazel build --config=opt //...
```

```sh
bazel build --config=dbg //...
```

## Test

All tests, including the conformance suite:

```sh
bazel test //...
```

| Test | What it covers |
|---|---|
| `//runtime/test:runtime_test` | the wire-format layer on its own |
| `//test:example_test` | the generated decode API: messages serialized by stock protobuf, decoded by wolaz |
| `//test:road_testwork_test` | `test/road_testwork.proto`: encoded by stock protobuf, decoded by wolaz and by stock protobuf |
| `//test:cpp14_test` | the runtime and generated code compiled as C++14 with `-pedantic-errors`, decoding hand-written bytes |
| `//conformance:conformance_test` | Google's protobuf conformance suite |

The unit tests only:

```sh
bazel test //runtime/test:all //test:all
```

Everything except the conformance suite, which is the slowest target on a cold
build because it compiles the conformance runner from source:

```sh
bazel test --config=noconformance //...
```

Test output is shown automatically when a test fails (`--test_output=errors`
is set in `.bazelrc`). To see it for passing tests too:

```sh
bazel test --test_output=all //runtime/test:all //test:all
```

## Conformance

### Through Bazel

```sh
bazel test //conformance:conformance_test
```

This uses protobuf's own `conformance_test` macro. Its runner script always
passes `--enforce_recommended`, and the target adds
`--maximum_edition 2023` and `--failure_list conformance/failure_list_wolaz.txt`.

Bazel only prints PASSED or FAILED. The suite's own summary is in the test log:

```sh
grep "CONFORMANCE SUITE" bazel-testlogs/conformance/conformance_test/test.log
```

Expected:

```
CONFORMANCE SUITE PASSED: 1405 successes, 4235 skipped, 9 expected failures, 0 unexpected failures.
CONFORMANCE SUITE PASSED: 0 successes, 909 skipped, 0 expected failures, 0 unexpected failures.
```

The first line is the binary and JSON suite; the second is the text-format
suite, which wolaz skips entirely. See `conformance/README.md` for how the
testee works (wolaz decodes, stock protobuf encodes the response), what is
skipped, and why the nine expected failures fail: wolaz does not preserve
unknown fields.

Bazel caches test results. To force the suite to run again with nothing
changed:

```sh
bazel test --cache_test_results=no //conformance:conformance_test
```

### By hand

Build the runner and the testee, then run the runner directly. This prints the
per-test failure details that Bazel keeps in the log.

```sh
bazel build @com_google_protobuf//conformance:conformance_test_runner //conformance:wolaz_conformer
```

```sh
bazel-bin/external/protobuf+/conformance/conformance_test_runner \
    --enforce_recommended \
    --maximum_edition 2023 \
    --failure_list conformance/failure_list_wolaz.txt \
    bazel-bin/conformance/wolaz_conformer
```

Drop `--failure_list` to see the nine known failures reported as unexpected.

Other useful runner flags:

| Flag | Effect |
|---|---|
| `--test <name>` | run a single test, e.g. `--test Required.Proto3.ProtobufInput.ValidDataOneof.MESSAGE.Merge.ProtobufOutput` |
| `--debug` | print each request and response for the tests that run |
| `--output_dir <dir>` | where to write `failing_tests.txt`, the list of failing test names |

Without `--output_dir`, the runner tries to write `/failing_tests.txt` and
reports `Failed to open file`. That message is harmless, but pass
`--output_dir .` if you want the list.

For comparison, protobuf's own C++ implementation running the same suite, with
protobuf's own failure lists:

```sh
bazel build @com_google_protobuf//conformance:conformance_test_runner @com_google_protobuf//conformance:conformance_cpp
```

```sh
bazel-bin/external/protobuf+/conformance/conformance_test_runner \
    --enforce_recommended --maximum_edition 2023 \
    --failure_list "$(bazel info output_base)/external/protobuf+/conformance/failure_list_cpp.txt" \
    --text_format_failure_list "$(bazel info output_base)/external/protobuf+/conformance/text_format_failure_list_cpp.txt" \
    bazel-bin/external/protobuf+/conformance/conformance_cpp
```

That reports 5545 successes and 104 expected failures. It runs the JSON and
text-format tests that wolaz skips.

## Sanitizers

Same configuration names as protobuf's `.bazelrc`:

AddressSanitizer:

```sh
bazel test --config=asan //...
```

UndefinedBehaviorSanitizer:

```sh
bazel test --config=ubsan //...
```

ThreadSanitizer:

```sh
bazel test --config=tsan //...
```

All three run the full conformance suite with the testee instrumented.

Limitations with the GCC toolchain:

- **ubsan instruments wolaz's own sources only** (`runtime/` and
  `conformance/`). Under GCC, `-fsanitize=undefined` breaks the compile of
  abseil's hash containers, so protobuf and abseil are built without it. The
  runtime library is linked into every binary, so it is still checked.
- **tsan runs tests with ASLR disabled** (`setarch -R`). On recent kernels,
  TSan otherwise aborts at startup with `unexpected memory mapping`.
- **There is no msan config.** MemorySanitizer only exists in Clang.

## compile_commands.json

For clangd and any other tool that reads a compilation database:

```sh
bazel run //:refresh_compile_commands
```

This writes `compile_commands.json` at the workspace root. It covers the
runtime, both tests, the plugin and the conformer. The target list is in the
root `BUILD.bazel`; it is explicit rather than `//...` so that refreshing does
not also index the conformance runner.

Run `bazel build //...` first so that the generated `.wolaz.hpp` headers exist
on disk. clangd then resolves includes such as
`#include "google/protobuf/test_messages_proto3.wolaz.hpp"` the same way the
compiler does.

Re-run the refresh whenever you add files or change `deps`.

`.bazelrc` disables Bazel's `parse_headers` feature, because the extractor
cannot handle the header-only syntax-check actions it creates. This only skips
those extra checks; it does not change what is compiled.

## Generating code

### In a Bazel build

```python
load("@com_google_protobuf//bazel:proto_library.bzl", "proto_library")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("//bazel:wolaz_proto_library.bzl", "wolaz_proto_library")

proto_library(
    name = "msample_proto",
    srcs = ["msample.proto"],
)

wolaz_proto_library(
    name = "msample_wolaz_proto",
    deps = [":msample_proto"],
)

cc_binary(
    name = "app",
    srcs = ["app.cc"],
    deps = [":msample_wolaz_proto"],  # generated headers and the runtime
)
```

`wolaz_proto_library` provides a `CcInfo`, so a `cc_library` or `cc_binary`
can depend on it directly and gets both the generated headers and
`//runtime:wolaz`. Headers are generated for every proto in the transitive
closure of `deps`, because each generated header includes the generated headers
of its imports.

The rule wraps an aspect, the same structure protobuf uses for
`upb_proto_library`; see `bazel/private/wolaz_proto_library_internal.bzl`.

To see the headers generated for a target:

```sh
bazel build //test:example_wolaz_proto
```

```sh
cat bazel-bin/test/example.wolaz.hpp
```

### With protoc by hand

```sh
bazel build @com_google_protobuf//:protoc //plugin:protoc-gen-wolaz
```

```sh
bazel-bin/external/protobuf+/protoc \
    --plugin=protoc-gen-wolaz=bazel-bin/plugin/protoc-gen-wolaz \
    -I"$(bazel info output_base)/external/protobuf+/src" \
    --wolaz_out=/tmp/out \
    google/protobuf/timestamp.proto
```

The `-I` path is where Bazel keeps protobuf's sources, including the
well-known types. `protoc` only generates code for the files named on the
command line, so list every transitive import as well. The Bazel rule handles
this for you.

## Inspecting the build

All targets:

```sh
bazel query //...
```

What the conformer pulls in:

```sh
bazel query 'deps(//conformance:wolaz_conformer)'
```

The protoc-gen-wolaz invocations:

```sh
bazel aquery 'mnemonic(GenWolazProto, deps(//conformance:wolaz_conformer))'
```

## Cleaning

Remove build outputs:

```sh
bazel clean
```

Also remove fetched dependencies:

```sh
bazel clean --expunge
```

## How fields are found

wolaz doesn't copy repeated fields or submessages out of the input. A
`subrange` (repeated field) or `lazy` (submessage field) holds a view of the
input and reads the field from it when asked. This section explains how much
of the input that view covers, and why.

### The problem

The [encoding guide](https://protobuf.dev/programming-guides/encoding/)
lets a field's records appear anywhere in its message:

- **Repeated fields.** "Records for `e` do not need to appear consecutively,
  and can be interleaved with other fields". A packable field may also arrive
  packed, unpacked, or as several packed records, and parsers must accept
  every mix.
- **Singular message fields** ("Last One Wins"). The field may appear more
  than once, and the occurrences merge. That merge is recursive, and for a
  repeated field inside it, it means concatenating.

Stock encoders write each field's records together, but concatenating two
encoded messages, which the spec defines as merging them, interleaves them.
So a reader that has only the message bytes has to scan the whole message to
be sure it has found every record of a field.

### What parse() records

A generated `parse()` already walks every record of its message once, to
validate it. As it goes, it fills in a `wolaz::field_span` for each repeated
or submessage field, which records:

- where the field's first record starts (the position of its tag),
- where its last record ends,
- how many records it had,
- how many elements they held: one per unpacked record, and the count of
  each packed run, which `wolaz::count_packed` finds while validating the run.

These are four `std::size_t` values on the stack of `parse()`, so nothing is
allocated. After the walk, `parse()` builds each field from its span:

- **A repeated field** gets a `subrange` of just the span,
  `[first record start, last record end)`, and the span's element count.
  Iterating reads only that part of the message, and `size()` returns the
  count without reading anything.
- **A singular message field that occurred once**, which is nearly always,
  gets a `lazy` holding only that occurrence's body.
  `for_each_occurrence` calls its function on that body directly, without
  walking anything.
- **A singular message field that occurred more than once** gets a `lazy`
  holding the span of all its occurrences, plus the final one for `get()`.
  `for_each_occurrence` walks only that span.

### Why this is correct

The span starts at a record's tag and ends at the end of a record, so it is a
well-formed run of records that can be read like a message. Every record of
the field lies inside it. Any records of other fields inside it are skipped,
exactly as when scanning the whole message. So the iterator and
`for_each_occurrence` see the same records, in the same order.

`parse()` has already validated every record in the span, so reading a
parsed field cannot fail partway through.

For the element count to equal what iteration yields, `parse()` and the
readers must agree on which records count. A record with the field's number
but a wire type the field cannot have, such as `I32` for an `int32` field, is
treated by `parse()` like an undeclared field: it is skipped and not counted.
The subrange iterator and `for_each_occurrence` skip it too.

## Layout

| Path | Contents |
|---|---|
| `MODULE.bazel` | module dependencies (protobuf, abseil, rules_cc, …) |
| `.bazelrc` | C++17 default, sanitizer and `noconformance` configs |
| `.bazelversion` | the Bazel release bazelisk uses |
| `BUILD.bazel` | `refresh_compile_commands` |
| `bazel/` | `wolaz_proto_library`, the aspect behind it, shared copts |
| `runtime/` | the library users include; see `runtime/README.md` |
| `runtime/test/` | `runtime_test` |
| `test/` | `example_test` and `road_testwork_test`, with the `.proto` files they use |
| `plugin/` | `protoc-gen-wolaz` and its `proto_lang_toolchain`; see `plugin/README.md` |
| `conformance/` | the conformance testee and failure list; see `conformance/README.md` |

## Future Work Serialization

For non-dynamic-allocation serialization, we would want a lazy API which is nice to work with, but that will be tricky to write with only C++14.

From the README example `road_testwork.proto`, 

1. We would want to declaratively specify the data.

```c++
wolaz::Encode(RoadSection{
    .name = "name"
    .elements = [
        RoadElement{
            .element = RoadStop {
                .id = 1 
            }
        },
        RoadElement{
            .element = RoadLane {
                .lanes_ahead = [1,2,3];
            }
        }
    ]
});
```

2. We want to be able to specify the data with begin-end iterator pair from a transformed list.
   This style is not great for one-of sumtype fields.

```c++
wolaz::Encode(RoadSection{
    .name = "name"
    .elements = std::ranges::views::transform({{1,2,3},{4,5,6}), [](int lanes) {
        return RoadElement {
            .element = RoadLane{
                .lanes_ahead = lanes;
            }
        };
    }
});
```

3. We want to be able to specify the data with HALO generators.
   Nice for one-of sumtype fields.


```c++
wolaz::Encode(RoadSection{
    .name = "name"
    .elements = []() -> std::generator<RoadElement> {
        co_yield RoadElement {
            .element = RoadStop {
                .id = 1;
            }
        };
        co_yield RoadElement {
            .element = RoadLane {
                .lanes_ahead = [1,2,3];
            }
        };
    }();
});
```

