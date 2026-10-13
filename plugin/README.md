# Wolaz protoc plugin

`protoc-gen-wolaz` is written in terms of the Google stock C++ protoc plugin,
but the code it generates is written in terms of the `runtime/wolaz` library.

## Use from Bazel

```python
load("//bazel:wolaz_proto_library.bzl", "wolaz_proto_library")

wolaz_proto_library(
    name = "msample_wolaz_proto",
    deps = [":msample_proto"],
)
```

The rule returns a `CcInfo`, so a `cc_library` or `cc_binary` can depend on it
directly and gets both the generated headers and the runtime. Code is generated
for the whole transitive closure of `deps`, because a generated header includes
the generated headers of its imports.

Targets: `//plugin:protoc-gen-wolaz` (the binary) and `//plugin:wolaz_toolchain`
(the `proto_lang_toolchain` that tells `proto_common.compile()` how to invoke
it).

## Use from the command line

```sh
protoc --wolaz_out=. msample.proto
```

`protoc` discovers the plugin by looking for `protoc-gen-wolaz` on `PATH`, or
you can point at it explicitly. Bazel builds both:

```sh
bazel build @com_google_protobuf//:protoc //plugin:protoc-gen-wolaz
```

```sh
bazel-bin/external/protobuf+/protoc \
    --plugin=protoc-gen-wolaz=bazel-bin/plugin/protoc-gen-wolaz \
    --wolaz_out=. msample.proto
```

For each `foo.proto` it writes `foo.wolaz.hpp`. `protoc` only generates the
files named on the command line, so every transitive import has to be listed
too.

## What it generates

One struct per message, `decode::M`, in a namespace `decode` nested inside the
namespace named by the proto package. It is a constant-size aggregate of
lazily-borrowed field views, with only public data members. A free function
`parse(wolaz::type_tag<M>, bytes, depth)` in the same namespace fills it from
a buffer; call it through `wolaz::decode<M>()` or `wolaz::lazy<M>::get()`,
which find it by argument-dependent lookup. Nothing is generated for encoding:
wolaz only decodes.

Each field's number is a named constant, `decode::field_number::M::field`,
which `parse()` uses in place of literal numbers. Because of these, a message
or enum may not be named `parse` or `field_number`; the plugin reports an
error if one is.

Each generated field has a comment giving its `.proto` declaration. The
generated code follows the AUTOSAR C++14 guidelines; see
[runtime/README.md](../runtime/README.md#autosar-c14) for the deviations.

Enums are generated into the same `decode` namespace. Stock protobuf generates
into the package namespace itself, so a wolaz header and a stock protobuf
header for the same `.proto` can be included in one translation unit.

| proto | `decode::M` member |
|---|---|
| singular scalar | `wolaz::optional<T>` |
| `string` / `bytes` | `wolaz::optional<wolaz::string_view>` |
| singular message | `wolaz::lazy<decode::Sub>` |
| repeated scalar | `wolaz::subrange<codec>` |
| repeated message | `wolaz::subrange<codec::message_<...>>` |
| `map<K, V>` | `subrange` of the entry type |
| enum | `decode::E`, an `enum class : std::int32_t` |
| oneof | one member per alternative, plus `<oneof> <oneof>_case`, where `<oneof>` is a nested `enum class` |

Nested types are flattened with `_` (`Outer.Inner` becomes `Outer_Inner`) so
that every generated struct is forward-declarable, which recursive definitions
require. A singular message field is a `wolaz::lazy<M>`, which is the same size
whatever `M` is — that is what lets `TestAllTypesProto3.NestedMessage` hold a
`TestAllTypesProto3` without anything being allocated.

## Decisions worth knowing about

**Presence.** Every singular field is a `wolaz::optional` (or a `lazy`, which
also knows whether it is set), so you can tell "absent" from "present with the
default value" for fields with explicit presence. For an implicit-presence
field (proto3 singular), the encoder omits the default value, so it decodes as
absent.

**Oneofs.** Protobuf resolves a oneof by *wire* order: the last member on the
wire wins, which is not necessarily the one with the highest field number. So
`parse()` records the winner in `<oneof>_case`. Use that, not which members
happen to be set, to tell which one applies.

The type of `<oneof>_case` is an `enum class` nested in the message struct and
named after the oneof. It has one enumerator per member, named like the member
field and valued by its field number, plus `NOT_SET = 0`:

```c++
struct RoadElement {
  // uint64 id = 1;
  ::wolaz::optional<std::uint64_t> id{};

  // RoadLane lane = 2;
  ::wolaz::lazy<::decode::RoadLane> lane{};

  // RoadStop stop = 3;
  ::wolaz::lazy<::decode::RoadStop> stop{};

  // Which member of oneof `element` was set: the last one on the wire.
  enum class element : std::uint32_t {
    NOT_SET = 0U,
    lane = 2U,
    stop = 3U,
  };
  element element_case{element::NOT_SET};
};
```

protoc rejects a oneof that shares a name with a field, so the enum's name
never collides with a member. If a oneof member is itself called `NOT_SET`, the
"none" enumerator becomes `NOT_SET1`.

**Names.** Field, oneof and enum value names are kept as written, except
where C++ would not allow them. C++ reserves identifiers that begin with an
underscore or contain two in a row, so leading underscores are dropped and
runs of underscores collapsed: `_field_name3` becomes `field_name3` and
`field__name4_` becomes `field_name4_`. A C++ keyword gets a trailing
underscore. If a changed name collides with another name in the same scope,
it gets a numeric suffix; names that needed no change never do.

**Enums are treated as open**, including proto2 closed enums: any value on the
wire is returned as is. Stock protobuf diverts an unrecognised value of a
closed enum into its unknown-field set instead. Use the generated
`<Enum>_IsValid()` to tell the cases apart.

**Unknown fields are skipped.** A field number the message does not declare is
stepped over during decoding and not stored, because keeping a variable number
of them would mean allocating. It cannot be inspected afterwards.

**Validation is per level, materialisation is lazy.** `parse()` makes a
single pass that fills the singular fields and validates everything else in
the message, including each element of every repeated field, so malformed
input at that level is reported by `parse()` rather than in the middle of an
iteration. A submessage is only checked for being a well-formed record: its
contents are validated when `lazy::get()` decodes it. So `parse()` never calls
another `parse()`, and decoding is free of recursion. Nothing is stored for
repeated or nested fields; the `subrange` and `lazy` handles re-read them on
demand. While walking, `parse()` notes where each such field's records are,
so a handle reads only that span of the message, and `subrange::size()` is
O(1). See [DEVELOPMENT.md](../DEVELOPMENT.md#how-fields-are-found).

## Limitations

- No encoding. Use stock protobuf to serialize.
- No extensions. Extension fields are skipped like any other unknown field.
