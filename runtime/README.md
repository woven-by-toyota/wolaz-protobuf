# Wolaz protobuf runtime library

This is the library which you include in your C++ projects. It decodes the
protobuf binary wire format; it does not encode.

```c++
#include <wolaz.hpp>
```

It is the only thing generated code depends on. `wolaz.hpp` carries the
templates; `src/wolaz.cpp` carries the non-template wire-format helpers so they
are compiled once rather than in every translation unit that includes a
generated header.

## C++ standard

The runtime and generated code are C++14. `wolaz.hpp` provides its own minimal
`wolaz::optional<T>` and `wolaz::string_view` in place of the C++17 standard
types, and `wolaz::expected<T, E>` in place of the C++23 `std::expected`. They
are used under every standard, C++17 and later included, so that the
runtime's functions have the same signatures whichever standard each
translation unit is compiled with.

`wolaz::string_view` converts from anything with `char` `data()` and `size()`,
such as `std::string` or `std::string_view`, and from a `const char*`. It
converts explicitly to `std::string`, and can be written to a `std::ostream`.
It has `size()`, `length()`, `empty()`, `[]`, `substr()`, and `begin()` and
`end()`, which are `const char*`.
`wolaz::optional<T>` has `has_value()`, `operator bool`, `*`, `->`,
`value_or()`, `emplace()` and `reset()`; there is no `value()`. It holds its
value as a plain member, so `T` must be default-constructible, and an optional
of a trivially copyable type is itself trivially copyable. Constructing one
from a value of fundamental type is explicit: `optional<int> x{3}`, not
`optional<int> x = 3`.

`wolaz::expected<T, E>` holds either a `T` or an error `E`, which defaults to
`wolaz::string_view`. It has `has_value()`, `operator bool`, `*`, `->` and
`error()`, which is empty when there is a value; there is no `value()`. It
converts implicitly from a `T` or an `E`, which is how a generated `parse()`
returns either the message or an error.

`//runtime:wolaz` is built with `-std=c++14 -pedantic-errors`, and
`//test:cpp14_test` compiles the generated code the same way. The rest of the
build is C++17, because stock protobuf and abseil require it.

## What it provides

| | |
|---|---|
| `cursor` | a non-owning read head over a byte range, with strict varint/tag/length decoding; each read returns an `optional` |
| `codec::*` | one type per protobuf field type, mapping it to a C++ type and knowing how to read it |
| `subrange<Codec>` | a lazy view of one repeated field: a (buffer, field number) pair, where generated code narrows the buffer to the field's records and supplies the element count |
| `field_span` | where a field's records lie in a message, as `parse()` finds them; see DEVELOPMENT.md |
| `lazy<M>` | an undecoded submessage; constant size regardless of `M` |
| `decode<M>(bytes, depth = 0)` | the entry point: decodes one message |
| `expected<T, E>` | a C++14 stand-in for `std::expected`; `E` defaults to `string_view` |
| `err::*` | the decode errors, as `string_view` constants |
| `optional<T>`, `string_view` | minimal versions of the C++17 `std::optional` and `std::string_view` |

## The lifetime rule

Decoding never copies. Singular strings and bytes come back as
`wolaz::string_view`, repeated fields as `subrange`, and submessages as
`lazy<M>` — all of them pointing into the buffer you passed in.

**The input buffer must outlive any decoded message that borrows from it.**
That single invariant is what the zero-copy guarantee rests on.

## Allocation

Decoding never allocates. A decoded message is a fixed-size struct, and
everything variable-length in it — strings, repeated fields, submessages — is
a view into the input buffer.

Fields the message does not declare are skipped rather than stored, since
keeping a variable number of them would mean allocating. They cannot be
inspected after decoding.

## Validation

Decoding is one level at a time. `decode<M>()` validates every field of `M`
itself: varints, lengths, UTF-8 in `string` fields, and each element of every
repeated scalar field. A submessage is only checked for being a well-formed
length-delimited record or group. Its contents are validated when you decode
it with `lazy<M>::get()`, which returns the error if there is one.

So a buffer with a malformed submessage decodes successfully at the top
level, and the error appears when that submessage is decoded. A caller that
needs the whole tree validated up front has to decode every submessage.

This keeps decoding free of recursion: a generated `parse()` never calls
another one.

## Errors

A failed decode returns an `expected<M>` whose `error()` is a `string_view`
describing what went wrong. The messages are static strings, so failing does
not allocate. Compare against the constants in `wolaz::err`, such as
`wolaz::err::bad_length`, with `==`, which compares the text. They are
`constexpr`, so they can be used from other translation units' static
initializers.

## AUTOSAR C++14

The runtime and the generated code attempt to follow the AUTOSAR C++14
guidelines (AP Release 19-03). If your organization uses Wolaz then you
should audit Wolaz for compliance with your organization’s standards.

Wolaz helps comply with rule **A18-5-7** because it never performs 
any dynamic allocation.

Wolaz deviates from these rules:

| Rule | Where | Why |
|---|---|---|
| M6-6-5, single point of exit | throughout | Project-wide deviation. Functions return early on error. |
| M3-9-3, floating-point bit representation | `bits_to_float`, `bits_to_double` | Decoding `float` and `double` means reinterpreting their IEEE 754 bits (A0-4-1), which is done with `std::memcpy`. |
| A7-2-1, enum values outside the enumerators | `codec::enum_` | Protobuf enums are open: a value with no enumerator must still be returned. `<Enum>_IsValid()` tells the caller. |
| M5-0-11, M5-0-12, plain `char` for byte data | `string_view`, `cursor` | The wire format is viewed in `char` buffers, so that a `std::string` can be decoded without a copy. Bytes are converted to `std::uint8_t` in one place, `byte_at()` in `wolaz.cpp`. |
| M5-0-15, pointer arithmetic other than indexing | `string_view::end()` | `begin()` and `end()` are `const char*`, like `std::string_view`'s, so that a view works with range-for and the standard algorithms. The decoder itself only indexes. |
| A16-0-1, preprocessor only for includes | `#if __cplusplus` around the `err` definitions | C++14 needs out-of-class definitions of the `constexpr` static members, and C++17 deprecates them. |
| M11-0-1, A11-0-1, public data in a non-POD type | every generated `decode::M` | The public aggregate is the API. The structs meet A11-0-2 (public data only, no member functions), and are trivially copyable, but default member initializers make them non-POD, which keeps a default-constructed message valid. |
| A8-5-2, brace initialization | member initializers of generic `T` in `optional` and `expected` | `value_(value)` uses parentheses, because braces would pick an `std::initializer_list` constructor of `T` where one exists. |

Known gaps:

- Message and enum type names are not sanitized the way field names are, so
  a type whose name starts with an underscore, or a nested type whose parent
  ends in one, produces a reserved identifier (A17-0-1).
- A generated `parse()` names its parameters and locals `bytes`, `depth`,
  `msg`, `in` and `tag`. A message type with one of these names would be
  hidden by them (A2-10-1).
- The process rules (A0-4-3, A1-1-2, A1-2-1, A1-4-1, M0-3-1, A15-0-7, A15-0-8)
  call for a qualified toolchain, code metrics, static analysis and timing
  analysis, which this repository does not have.

## Bazel

```
//runtime:wolaz
```

## Tests

`runtime/test/runtime_test.cpp` covers the wire-format layer directly: varint
boundaries and over-long rejection, zigzag, little-endian fixed widths, tag
validation, group skipping (including nesting and the depth limit), packed
and unpacked repeated fields and their
mixture, and UTF-8 validation. Its input is written out by hand or produced
with stock protobuf's encoder.

`test/example_test.cpp`, at the top of the repository, covers the generated
API: each message is serialized by stock protobuf and decoded by wolaz.

```sh
bazel test //runtime/test:all //test:all
```
