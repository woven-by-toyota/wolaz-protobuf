# Wolaz conformance test runner

`wolaz_conformer` is a testee for Google's conformance harness. It reads
length-prefixed `ConformanceRequest` messages on stdin and replies with
length-prefixed `ConformanceResponse` messages on stdout.

```sh
bazel test //conformance:conformance_test --cache_test_results=no --test_output=all
```

The target uses protobuf's own `conformance_test` macro, whose runner script
always passes `--enforce_recommended`. See `DEVELOPMENT.md` for running the
harness by hand.

## Current result

```
CONFORMANCE SUITE PASSED: 1405 successes, 4235 skipped, 9 expected failures, 0 unexpected failures
```

Verified with protobuf v36.2, and again with the testee built under
AddressSanitizer and UndefinedBehaviorSanitizer.

## How a test is answered

wolaz only decodes, so the response is produced in two steps:

1. **wolaz decodes** the payload with wolaz-generated code.
2. **Stock protobuf encodes.** `wolaz_conformer.cpp` copies every decoded
   value into the matching stock protobuf message: `wolaz::optional` fields when
   set, repeated fields with range-for loops over the wolaz `subrange`s, maps
   entry by entry, and submessages recursively. Stock protobuf then serializes
   that message for the response.

The harness compares the response with what the reference implementation
produces, so every value wolaz decodes is checked, not just whether it accepts
the payload.

The copy follows the wire format's merge rules: scalars last-wins, repeated
fields concatenate, and a singular message field that arrives more than once
is merged occurrence by occurrence using `wolaz::lazy::for_each_occurrence`.

Two stock-protobuf behaviours are reproduced by the copy, on the stock side,
because wolaz itself does not have them:

- **Closed enums.** wolaz returns any enum value it reads. For a proto2 enum
  value that is not a declared member, stock protobuf puts it in the message's
  unknown-field set instead of the field, and the copy does the same.
- **Required fields.** wolaz does not enforce `required`. Stock parsing
  rejects a `TestAllRequiredTypesProto2` payload that leaves one unset, so the
  testee reports a parse error when the copied message is not initialized.

A payload wolaz rejects is reported as `parse_error`, which is the expected
outcome for the suite's many malformed-input cases. wolaz validates a
submessage only when it is decoded, so a malformed submessage is found during
the copy, which decodes every submessage, and is also reported as
`parse_error`.

The request/response envelope is handled with stock protobuf.

## What is skipped, and why

The 4235 skips are the JSON and text-format tests. wolaz is a binary
wire-format decoder; it has no JSON or text parser. The harness accounts for
`skipped` responses separately from failures.

Editions payload types are also skipped: they live in `.proto` files this build
does not generate.

## What fails, and why

Nine tests, listed with a full explanation in `failure_list_wolaz.txt`. All
nine fail for one reason: **wolaz does not preserve unknown fields.**

When wolaz decodes a field number that the message does not declare, it steps
over it. The field is not stored anywhere and cannot be inspected afterwards.
Keeping a variable number of unknown fields would mean allocating, and
zero-heap-allocation decoding is the point of the library. Stock protobuf
keeps them in an `UnknownFieldSet` and writes them back out, and these tests
check for that:

| Tests | What they send | Why wolaz's output differs |
|---|---|---|
| `UnknownVarint`, `UnknownOrdering` (proto2 and proto3) | field numbers `TestAllTypesProto2/3` does not declare | wolaz skips them, so the response has no unknown fields |
| `ValidMessageSetEncoding` and its three variants | extensions inside `message_set_correct` | `MessageSetCorrect` declares no fields, only an extension range, and wolaz has no extensions, so its whole content is unknown and skipped |
| `MessageSetEncoding.UnknownExtension` | an undeclared extension inside `message_set_correct` | stock protobuf keeps it as an unknown field; wolaz skips it |

Extensions declared on `TestAllTypesProto2` itself (field numbers 120–200) are
skipped the same way, but no conformance test sends one.
