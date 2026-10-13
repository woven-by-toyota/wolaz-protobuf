// Wolaz conformance test runner (testee side).
//
// Speaks the protobuf conformance harness protocol on stdin/stdout: read a
// 4-byte little-endian length, then that many bytes of a serialised
// ConformanceRequest; reply with a length-prefixed ConformanceResponse.
//
//   conformance_test_runner --enforce_recommended wolaz_conformer
//
// What is exercised
//
// wolaz only decodes. For each binary request the payload is decoded with
// wolaz-generated code, and every decoded value is copied into the matching
// stock protobuf message. Stock protobuf then serialises that message for the
// response. The harness compares the response with what the reference
// implementation would produce, so every value wolaz decodes is checked.
//
// The copy follows the wire format's merge rules: scalars last-wins, repeated
// fields concatenate, and a singular message field that arrives more than
// once is merged occurrence by occurrence (wolaz::lazy::for_each_occurrence).
//
// The ConformanceRequest/Response envelope is handled with stock protobuf.
//
// Two stock protobuf behaviours are reproduced here, on the stock side, so
// that the output matches what the reference implementation produces:
//   - An unrecognised value of a closed (proto2) enum goes into the stock
//     message's unknown-field set, as stock parsing does. wolaz itself treats
//     every enum as open.
//   - A TestAllRequiredTypesProto2 payload with a required field missing is
//     reported as a parse error, as stock parsing does. wolaz itself does not
//     enforce `required`.
//
// What is not reproduced
//
// wolaz skips unknown fields and has no extensions, so neither appears in the
// response. The conformance tests that expect them to survive are listed in
// failure_list_wolaz.txt.
//
// JSON and text-format requests are answered with `skipped`. wolaz is a
// binary wire-format decoder; it has no JSON or text parser.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>

#include <wolaz.hpp>

#include "conformance/conformance.pb.h"
#include "google/protobuf/test_messages_proto2.pb.h"
#include "google/protobuf/test_messages_proto2.wolaz.hpp"
#include "google/protobuf/test_messages_proto3.pb.h"
#include "google/protobuf/test_messages_proto3.wolaz.hpp"

namespace {

using conformance::ConformanceRequest;
using conformance::ConformanceResponse;

namespace gp = ::google::protobuf;                        // stock
namespace wk = ::google::protobuf::decode;                // wolaz
namespace s3 = ::protobuf_test_messages::proto3;          // stock
namespace w3 = ::protobuf_test_messages::proto3::decode;  // wolaz
namespace s2 = ::protobuf_test_messages::proto2;          // stock
namespace w2 = ::protobuf_test_messages::proto2::decode;  // wolaz

// Copying wolaz-decoded messages into stock messages.
//
// Merge(in, out) applies one decoded wolaz message to a stock message,
// following the wire format's merge rules, and returns false if a submessage
// fails to decode. wolaz validates a submessage only when it is decoded, so
// this is where malformed nested input is found.

bool Merge(const wk::Any& in, gp::Any* out);
bool Merge(const wk::Duration& in, gp::Duration* out);
bool Merge(const wk::Timestamp& in, gp::Timestamp* out);
bool Merge(const wk::FieldMask& in, gp::FieldMask* out);
bool Merge(const wk::Struct& in, gp::Struct* out);
bool Merge(const wk::Value& in, gp::Value* out);
bool Merge(const wk::ListValue& in, gp::ListValue* out);
bool Merge(const wk::Empty& in, gp::Empty* out);
bool Merge(const wk::BoolValue& in, gp::BoolValue* out);
bool Merge(const wk::Int32Value& in, gp::Int32Value* out);
bool Merge(const wk::Int64Value& in, gp::Int64Value* out);
bool Merge(const wk::UInt32Value& in, gp::UInt32Value* out);
bool Merge(const wk::UInt64Value& in, gp::UInt64Value* out);
bool Merge(const wk::FloatValue& in, gp::FloatValue* out);
bool Merge(const wk::DoubleValue& in, gp::DoubleValue* out);
bool Merge(const wk::StringValue& in, gp::StringValue* out);
bool Merge(const wk::BytesValue& in, gp::BytesValue* out);

bool Merge(const w3::TestAllTypesProto3& in, s3::TestAllTypesProto3* out);
bool Merge(const w3::TestAllTypesProto3_NestedMessage& in,
           s3::TestAllTypesProto3_NestedMessage* out);
bool Merge(const w3::ForeignMessage& in, s3::ForeignMessage* out);

bool Merge(const w2::TestAllTypesProto2& in, s2::TestAllTypesProto2* out);
bool Merge(const w2::TestAllTypesProto2_NestedMessage& in,
           s2::TestAllTypesProto2_NestedMessage* out);
bool Merge(const w2::TestAllTypesProto2_Data& in,
           s2::TestAllTypesProto2_Data* out);
bool Merge(const w2::TestAllTypesProto2_MultiWordGroupField& in,
           s2::TestAllTypesProto2_MultiWordGroupField* out);
bool Merge(const w2::TestAllTypesProto2_MessageSetCorrect& in,
           s2::TestAllTypesProto2_MessageSetCorrect* out);
bool Merge(const w2::ForeignMessageProto2& in, s2::ForeignMessageProto2* out);

bool Merge(const w2::TestAllRequiredTypesProto2& in,
           s2::TestAllRequiredTypesProto2* out);
bool Merge(const w2::TestAllRequiredTypesProto2_NestedMessage& in,
           s2::TestAllRequiredTypesProto2_NestedMessage* out);
bool Merge(const w2::TestAllRequiredTypesProto2_Data& in,
           s2::TestAllRequiredTypesProto2_Data* out);

// A singular message field: merge every occurrence, in wire order.
template <class W, class S>
bool MergeLazy(const wolaz::lazy<W>& in, S* out) {
  return in.for_each_occurrence([&](wolaz::string_view body) {
    auto decoded = wolaz::decode<W>(body, in.depth());
    return decoded && Merge(*decoded, out);
  });
}

// One element of a repeated message field, or one map entry.
template <class W, class S>
bool MergeOne(const wolaz::lazy<W>& in, S* out) {
  auto decoded = in.get();
  return decoded && Merge(*decoded, out);
}

// The stock descriptor of field `number` of message `S`.
template <class S>
const gp::FieldDescriptor* FieldOf(int number) {
  const gp::FieldDescriptor* f = S::descriptor()->FindFieldByNumber(number);
  if (f == nullptr) std::abort();
  return f;
}

// True if `value` is not a member of a closed enum. Stock protobuf parks such
// values in the unknown-field set instead of in the field.
bool IsUnknownClosedEnumValue(const gp::EnumDescriptor* e, std::int32_t value) {
  return e->is_closed() && e->FindValueByNumber(value) == nullptr;
}

// Sets an enum field, or for an unknown closed-enum value adds it to the
// unknown-field set as stock parsing would.
template <class S, class Set>
void SetEnum(S* out, int number, std::int32_t value, Set&& set) {
  if (IsUnknownClosedEnumValue(FieldOf<S>(number)->enum_type(), value)) {
    out->mutable_unknown_fields()->AddVarint(
        number, static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
  } else {
    set(value);
  }
}

// Field-copy helpers. Each names a wolaz field and the stock accessor for
// the same field. They differ only where stock protobuf lower-cases a field
// name that wolaz keeps as written.

#define COPY_AS(w, s) \
  if (in.w) out->set_##s(*in.w);
#define COPY(f) COPY_AS(f, f)
#define COPY_STR(f) \
  if (in.f) out->set_##f(std::string(*in.f));
#define COPY_MSG(f) \
  if (in.f && !MergeLazy(in.f, out->mutable_##f())) return false;
#define COPY_ENUM(f, number)                                              \
  if (in.f)                                                               \
    SetEnum(out, number, static_cast<std::int32_t>(*in.f),                \
            [&](std::int32_t v) {                                         \
              out->set_##f(static_cast<decltype(out->f())>(v));           \
            });

#define COPY_REP(f) \
  for (auto v : in.f) out->add_##f(v);
#define COPY_REP_STR(f) \
  for (wolaz::string_view v : in.f) out->add_##f(std::string(v));
#define COPY_REP_MSG(f)                                    \
  for (const auto& e : in.f)                               \
    if (!MergeOne(e, out->add_##f())) return false;
#define COPY_REP_ENUM(f, number)                                          \
  for (auto v : in.f)                                                     \
    SetEnum(out, number, static_cast<std::int32_t>(v),                    \
            [&](std::int32_t x) {                                         \
              out->add_##f(static_cast<decltype(out->f(0))>(x));          \
            });

// `[ctype = CORD]` fields: stock protobuf hides their generated accessors, so
// they are set through stock reflection instead.
#define COPY_CORD(f, number)                                              \
  if (in.f)                                                               \
    out->GetReflection()->SetString(                                      \
        out, FieldOf<std::remove_pointer_t<decltype(out)>>(number),       \
        std::string(*in.f));
#define COPY_REP_CORD(f, number)                                          \
  for (wolaz::string_view v : in.f)                                         \
    out->GetReflection()->AddString(                                      \
        out, FieldOf<std::remove_pointer_t<decltype(out)>>(number),       \
        std::string(v));

// A map is a repeated entry message. A later entry for the same key replaces
// an earlier one, as stock parsing does; an absent key or value is the
// default.
#define COPY_MAP(f, key_default, value_default)                           \
  for (const auto& e : in.f) {                                            \
    auto entry = e.get();                                                 \
    if (!entry) return false;                                             \
    (*out->mutable_##f())[entry->key.value_or(key_default)] =             \
        entry->value.value_or(value_default);                             \
  }
#define COPY_MAP_STR(f)                                                   \
  for (const auto& e : in.f) {                                            \
    auto entry = e.get();                                                 \
    if (!entry) return false;                                             \
    (*out->mutable_##f())[std::string(entry->key.value_or(""))] =         \
        std::string(entry->value.value_or(""));                           \
  }
#define COPY_MAP_MSG(f, key)                                              \
  for (const auto& e : in.f) {                                            \
    auto entry = e.get();                                                 \
    if (!entry) return false;                                             \
    auto& value = (*out->mutable_##f())[key];                             \
    value.Clear();                                                        \
    if (entry->value && !MergeLazy(entry->value, &value)) return false;   \
  }
#define STR_KEY std::string(entry->key.value_or(""))
#define INT_KEY entry->key.value_or(0)
// For a closed enum, an entry whose value is not a member goes to the
// unknown-field set whole, as stock parsing does.
#define COPY_MAP_ENUM(f, number)                                          \
  for (const auto& e : in.f) {                                            \
    auto entry = e.get();                                                 \
    if (!entry) return false;                                             \
    const auto value = static_cast<std::int32_t>(                         \
        entry->value.value_or(                                            \
            static_cast<std::decay_t<decltype(*entry->value)>>(0)));          \
    const gp::EnumDescriptor* type = FieldOf<std::remove_pointer_t<       \
        decltype(out)>>(number)->message_type()->map_value()->enum_type();\
    if (IsUnknownClosedEnumValue(type, value)) {                          \
      out->mutable_unknown_fields()->AddLengthDelimited(                  \
          number, std::string(e.bytes()));                                \
    } else {                                                              \
      using V = typename std::remove_reference_t<                         \
          decltype(*out->mutable_##f())>::mapped_type;                    \
      (*out->mutable_##f())[STR_KEY] = static_cast<V>(value);             \
    }                                                                     \
  }

// Well-known types -----------------------------------------------------------

bool Merge(const wk::Any& in, gp::Any* out) {
  COPY_STR(type_url)
  COPY_STR(value)
  return true;
}

bool Merge(const wk::Duration& in, gp::Duration* out) {
  COPY(seconds)
  COPY(nanos)
  return true;
}

bool Merge(const wk::Timestamp& in, gp::Timestamp* out) {
  COPY(seconds)
  COPY(nanos)
  return true;
}

bool Merge(const wk::FieldMask& in, gp::FieldMask* out) {
  COPY_REP_STR(paths)
  return true;
}

bool Merge(const wk::Struct& in, gp::Struct* out) {
  COPY_MAP_MSG(fields, STR_KEY)
  return true;
}

bool Merge(const wk::Value& in, gp::Value* out) {
  using kind = wk::Value::kind;
  switch (in.kind_case) {
    case kind::NOT_SET:
      break;
    case kind::null_value:
      out->set_null_value(static_cast<gp::NullValue>(*in.null_value));
      break;
    case kind::number_value:
      out->set_number_value(*in.number_value);
      break;
    case kind::string_value:
      out->set_string_value(std::string(*in.string_value));
      break;
    case kind::bool_value:
      out->set_bool_value(*in.bool_value);
      break;
    case kind::struct_value:
      return MergeLazy(in.struct_value, out->mutable_struct_value());
    case kind::list_value:
      return MergeLazy(in.list_value, out->mutable_list_value());
  }
  return true;
}

bool Merge(const wk::ListValue& in, gp::ListValue* out) {
  COPY_REP_MSG(values)
  return true;
}

bool Merge(const wk::Empty&, gp::Empty*) { return true; }

bool Merge(const wk::BoolValue& in, gp::BoolValue* out) {
  COPY(value)
  return true;
}
bool Merge(const wk::Int32Value& in, gp::Int32Value* out) {
  COPY(value)
  return true;
}
bool Merge(const wk::Int64Value& in, gp::Int64Value* out) {
  COPY(value)
  return true;
}
bool Merge(const wk::UInt32Value& in, gp::UInt32Value* out) {
  COPY(value)
  return true;
}
bool Merge(const wk::UInt64Value& in, gp::UInt64Value* out) {
  COPY(value)
  return true;
}
bool Merge(const wk::FloatValue& in, gp::FloatValue* out) {
  COPY(value)
  return true;
}
bool Merge(const wk::DoubleValue& in, gp::DoubleValue* out) {
  COPY(value)
  return true;
}
bool Merge(const wk::StringValue& in, gp::StringValue* out) {
  COPY_STR(value)
  return true;
}
bool Merge(const wk::BytesValue& in, gp::BytesValue* out) {
  COPY_STR(value)
  return true;
}

// Fields TestAllTypesProto2 and TestAllTypesProto3 have in common ----------
//
// The two messages declare most fields identically, so one template copies
// them for both.

template <class W, class S>
bool MergeCommon(const W& in, S* out) {
  COPY(optional_int32)
  COPY(optional_int64)
  COPY(optional_uint32)
  COPY(optional_uint64)
  COPY(optional_sint32)
  COPY(optional_sint64)
  COPY(optional_fixed32)
  COPY(optional_fixed64)
  COPY(optional_sfixed32)
  COPY(optional_sfixed64)
  COPY(optional_float)
  COPY(optional_double)
  COPY(optional_bool)
  COPY_STR(optional_string)
  COPY_STR(optional_bytes)
  COPY_MSG(optional_nested_message)
  COPY_MSG(optional_foreign_message)
  COPY_ENUM(optional_nested_enum, 21)
  COPY_ENUM(optional_foreign_enum, 22)
  COPY_STR(optional_string_piece)
  COPY_CORD(optional_cord, 25)
  COPY_MSG(recursive_message)

  COPY_REP(repeated_int32)
  COPY_REP(repeated_int64)
  COPY_REP(repeated_uint32)
  COPY_REP(repeated_uint64)
  COPY_REP(repeated_sint32)
  COPY_REP(repeated_sint64)
  COPY_REP(repeated_fixed32)
  COPY_REP(repeated_fixed64)
  COPY_REP(repeated_sfixed32)
  COPY_REP(repeated_sfixed64)
  COPY_REP(repeated_float)
  COPY_REP(repeated_double)
  COPY_REP(repeated_bool)
  COPY_REP_STR(repeated_string)
  COPY_REP_STR(repeated_bytes)
  COPY_REP_MSG(repeated_nested_message)
  COPY_REP_MSG(repeated_foreign_message)
  COPY_REP_ENUM(repeated_nested_enum, 51)
  COPY_REP_ENUM(repeated_foreign_enum, 52)
  COPY_REP_STR(repeated_string_piece)
  COPY_REP_CORD(repeated_cord, 55)

  COPY_REP(packed_int32)
  COPY_REP(packed_int64)
  COPY_REP(packed_uint32)
  COPY_REP(packed_uint64)
  COPY_REP(packed_sint32)
  COPY_REP(packed_sint64)
  COPY_REP(packed_fixed32)
  COPY_REP(packed_fixed64)
  COPY_REP(packed_sfixed32)
  COPY_REP(packed_sfixed64)
  COPY_REP(packed_float)
  COPY_REP(packed_double)
  COPY_REP(packed_bool)
  COPY_REP_ENUM(packed_nested_enum, 88)

  COPY_REP(unpacked_int32)
  COPY_REP(unpacked_int64)
  COPY_REP(unpacked_uint32)
  COPY_REP(unpacked_uint64)
  COPY_REP(unpacked_sint32)
  COPY_REP(unpacked_sint64)
  COPY_REP(unpacked_fixed32)
  COPY_REP(unpacked_fixed64)
  COPY_REP(unpacked_sfixed32)
  COPY_REP(unpacked_sfixed64)
  COPY_REP(unpacked_float)
  COPY_REP(unpacked_double)
  COPY_REP(unpacked_bool)
  COPY_REP_ENUM(unpacked_nested_enum, 102)

  COPY_MAP(map_int32_int32, 0, 0)
  COPY_MAP(map_int64_int64, 0, 0)
  COPY_MAP(map_uint32_uint32, 0u, 0u)
  COPY_MAP(map_uint64_uint64, 0u, 0u)
  COPY_MAP(map_sint32_sint32, 0, 0)
  COPY_MAP(map_sint64_sint64, 0, 0)
  COPY_MAP(map_fixed32_fixed32, 0u, 0u)
  COPY_MAP(map_fixed64_fixed64, 0u, 0u)
  COPY_MAP(map_sfixed32_sfixed32, 0, 0)
  COPY_MAP(map_sfixed64_sfixed64, 0, 0)
  COPY_MAP(map_int32_float, 0, 0.0f)
  COPY_MAP(map_int32_double, 0, 0.0)
  COPY_MAP(map_bool_bool, false, false)
  COPY_MAP_STR(map_string_string)
  COPY_MAP_STR(map_string_bytes)
  COPY_MAP_MSG(map_string_nested_message, STR_KEY)
  COPY_MAP_MSG(map_string_foreign_message, STR_KEY)
  COPY_MAP_ENUM(map_string_nested_enum, 73)
  COPY_MAP_ENUM(map_string_foreign_enum, 74)

  // The oneof: only the member that won on the wire. The proto3 message has
  // one more member, oneof_null_value, handled in its own Merge.
  using oneof_field = typename W::oneof_field;
  switch (in.oneof_field_case) {
    case oneof_field::oneof_uint32:
      out->set_oneof_uint32(*in.oneof_uint32);
      break;
    case oneof_field::oneof_nested_message:
      if (!MergeLazy(in.oneof_nested_message,
                     out->mutable_oneof_nested_message()))
        return false;
      break;
    case oneof_field::oneof_string:
      out->set_oneof_string(std::string(*in.oneof_string));
      break;
    case oneof_field::oneof_bytes:
      out->set_oneof_bytes(std::string(*in.oneof_bytes));
      break;
    case oneof_field::oneof_bool:
      out->set_oneof_bool(*in.oneof_bool);
      break;
    case oneof_field::oneof_uint64:
      out->set_oneof_uint64(*in.oneof_uint64);
      break;
    case oneof_field::oneof_float:
      out->set_oneof_float(*in.oneof_float);
      break;
    case oneof_field::oneof_double:
      out->set_oneof_double(*in.oneof_double);
      break;
    case oneof_field::oneof_enum:
      SetEnum(out, 119, static_cast<std::int32_t>(*in.oneof_enum),
              [&](std::int32_t v) {
                out->set_oneof_enum(
                    static_cast<decltype(out->oneof_enum())>(v));
              });
      break;
    default:
      break;
  }

  // Stock protobuf lower-cases these names. wolaz keeps the case, but drops
  // leading underscores and collapses runs of them, which C++ reserves.
  COPY_AS(fieldname1, fieldname1)
  COPY_AS(field_name2, field_name2)
  COPY_AS(field_name3, _field_name3)
  COPY_AS(field_name4_, field__name4_)
  COPY_AS(field0name5, field0name5)
  COPY_AS(field_0_name6, field_0_name6)
  COPY_AS(fieldName7, fieldname7)
  COPY_AS(FieldName8, fieldname8)
  COPY_AS(field_Name9, field_name9)
  COPY_AS(Field_Name10, field_name10)
  COPY_AS(FIELD_NAME11, field_name11)
  COPY_AS(FIELD_name12, field_name12)
  COPY_AS(field_name13, __field_name13)
  COPY_AS(Field_name14, __field_name14)
  COPY_AS(field_name15, field__name15)
  COPY_AS(field_Name16, field__name16)
  COPY_AS(field_name17_, field_name17__)
  COPY_AS(Field_name18_, field_name18__)
  return true;
}

// TestAllTypesProto3 ----------------------------------------------------------

bool Merge(const w3::TestAllTypesProto3& in, s3::TestAllTypesProto3* out) {
  if (!MergeCommon(in, out)) return false;

  COPY_ENUM(optional_aliased_enum, 23)
  if (in.oneof_field_case ==
      w3::TestAllTypesProto3::oneof_field::oneof_null_value) {
    out->set_oneof_null_value(static_cast<gp::NullValue>(*in.oneof_null_value));
  }

  COPY_MSG(optional_bool_wrapper)
  COPY_MSG(optional_int32_wrapper)
  COPY_MSG(optional_int64_wrapper)
  COPY_MSG(optional_uint32_wrapper)
  COPY_MSG(optional_uint64_wrapper)
  COPY_MSG(optional_float_wrapper)
  COPY_MSG(optional_double_wrapper)
  COPY_MSG(optional_string_wrapper)
  COPY_MSG(optional_bytes_wrapper)
  COPY_REP_MSG(repeated_bool_wrapper)
  COPY_REP_MSG(repeated_int32_wrapper)
  COPY_REP_MSG(repeated_int64_wrapper)
  COPY_REP_MSG(repeated_uint32_wrapper)
  COPY_REP_MSG(repeated_uint64_wrapper)
  COPY_REP_MSG(repeated_float_wrapper)
  COPY_REP_MSG(repeated_double_wrapper)
  COPY_REP_MSG(repeated_string_wrapper)
  COPY_REP_MSG(repeated_bytes_wrapper)

  COPY_MSG(optional_duration)
  COPY_MSG(optional_timestamp)
  COPY_MSG(optional_field_mask)
  COPY_MSG(optional_struct)
  COPY_MSG(optional_any)
  COPY_MSG(optional_value)
  if (in.optional_null_value) {
    out->set_optional_null_value(
        static_cast<gp::NullValue>(*in.optional_null_value));
  }
  COPY_MSG(optional_empty)
  COPY_REP_MSG(repeated_duration)
  COPY_REP_MSG(repeated_timestamp)
  COPY_REP_MSG(repeated_fieldmask)
  COPY_REP_MSG(repeated_struct)
  COPY_REP_MSG(repeated_any)
  COPY_REP_MSG(repeated_value)
  COPY_REP_MSG(repeated_list_value)
  COPY_REP_MSG(repeated_empty)
  return true;
}

bool Merge(const w3::TestAllTypesProto3_NestedMessage& in,
           s3::TestAllTypesProto3_NestedMessage* out) {
  COPY(a)
  COPY_MSG(corecursive)
  return true;
}

bool Merge(const w3::ForeignMessage& in, s3::ForeignMessage* out) {
  COPY(c)
  return true;
}

// TestAllTypesProto2 ----------------------------------------------------------

bool Merge(const w2::TestAllTypesProto2& in, s2::TestAllTypesProto2* out) {
  if (!MergeCommon(in, out)) return false;

  COPY_MAP(map_int32_bool, 0, false)
  COPY_MAP_MSG(map_int32_nested_message, INT_KEY)

  COPY_MSG(data)
  COPY_MSG(multiwordgroupfield)

  COPY(default_int32)
  COPY(default_int64)
  COPY(default_uint32)
  COPY(default_uint64)
  COPY(default_sint32)
  COPY(default_sint64)
  COPY(default_fixed32)
  COPY(default_fixed64)
  COPY(default_sfixed32)
  COPY(default_sfixed64)
  COPY(default_float)
  COPY(default_double)
  COPY(default_bool)
  COPY_STR(default_string)
  COPY_STR(default_bytes)

  COPY_MSG(message_set_correct)
  return true;
}

bool Merge(const w2::TestAllTypesProto2_NestedMessage& in,
           s2::TestAllTypesProto2_NestedMessage* out) {
  COPY(a)
  COPY_MSG(corecursive)
  return true;
}

bool Merge(const w2::TestAllTypesProto2_Data& in,
           s2::TestAllTypesProto2_Data* out) {
  COPY(group_int32)
  COPY(group_uint32)
  return true;
}

bool Merge(const w2::TestAllTypesProto2_MultiWordGroupField& in,
           s2::TestAllTypesProto2_MultiWordGroupField* out) {
  COPY(group_int32)
  COPY(group_uint32)
  return true;
}

// MessageSetCorrect declares no fields, only an extension range, and wolaz has
// no extensions: everything inside it is skipped.
bool Merge(const w2::TestAllTypesProto2_MessageSetCorrect&,
           s2::TestAllTypesProto2_MessageSetCorrect*) {
  return true;
}

bool Merge(const w2::ForeignMessageProto2& in, s2::ForeignMessageProto2* out) {
  COPY(c)
  return true;
}

// TestAllRequiredTypesProto2 -------------------------------------------------

bool Merge(const w2::TestAllRequiredTypesProto2& in,
           s2::TestAllRequiredTypesProto2* out) {
  COPY(required_int32)
  COPY(required_int64)
  COPY(required_uint32)
  COPY(required_uint64)
  COPY(required_sint32)
  COPY(required_sint64)
  COPY(required_fixed32)
  COPY(required_fixed64)
  COPY(required_sfixed32)
  COPY(required_sfixed64)
  COPY(required_float)
  COPY(required_double)
  COPY(required_bool)
  COPY_STR(required_string)
  COPY_STR(required_bytes)
  COPY_MSG(required_nested_message)
  COPY_MSG(required_foreign_message)
  COPY_ENUM(required_nested_enum, 21)
  COPY_ENUM(required_foreign_enum, 22)
  COPY_STR(required_string_piece)
  COPY_CORD(required_cord, 25)
  COPY_MSG(recursive_message)
  COPY_MSG(optional_recursive_message)
  COPY_MSG(data)

  COPY(default_int32)
  COPY(default_int64)
  COPY(default_uint32)
  COPY(default_uint64)
  COPY(default_sint32)
  COPY(default_sint64)
  COPY(default_fixed32)
  COPY(default_fixed64)
  COPY(default_sfixed32)
  COPY(default_sfixed64)
  COPY(default_float)
  COPY(default_double)
  COPY(default_bool)
  COPY_STR(default_string)
  COPY_STR(default_bytes)
  return true;
}

bool Merge(const w2::TestAllRequiredTypesProto2_NestedMessage& in,
           s2::TestAllRequiredTypesProto2_NestedMessage* out) {
  COPY(a)
  COPY_MSG(corecursive)
  COPY_MSG(optional_corecursive)
  return true;
}

bool Merge(const w2::TestAllRequiredTypesProto2_Data& in,
           s2::TestAllRequiredTypesProto2_Data* out) {
  COPY(group_int32)
  COPY(group_uint32)
  return true;
}

// The protocol ---------------------------------------------------------------

// Reads exactly `size` bytes from stdin. Returns false on clean EOF.
bool ReadExactly(void* out, std::size_t size) {
  char* p = static_cast<char*>(out);
  std::size_t got = 0;
  while (got < size) {
    const std::size_t n = std::fread(p + got, 1, size - got, stdin);
    if (n == 0) return false;
    got += n;
  }
  return true;
}

void WriteExactly(const void* data, std::size_t size) {
  const char* p = static_cast<const char*>(data);
  std::size_t put = 0;
  while (put < size) {
    const std::size_t n = std::fwrite(p + put, 1, size - put, stdout);
    if (n == 0) {
      std::fprintf(stderr, "wolaz_conformer: short write to stdout\n");
      std::exit(1);
    }
    put += n;
  }
}

// Decodes `payload` as `W` with wolaz, copies it into a stock `S`, and
// serialises that with stock protobuf.
template <class W, class S>
void DecodeAndReserialize(const std::string& payload,
                          ConformanceResponse* response) {
  auto decoded = wolaz::decode<W>(std::string_view(payload));
  if (!decoded) {
    // A rejected payload is the expected outcome for the many malformed-input
    // cases; report why so failures are diagnosable.
    response->set_parse_error(std::string(decoded.error()));
    return;
  }

  S message;
  if (!Merge(*decoded, &message)) {
    // A submessage was malformed. wolaz finds that when the submessage is
    // decoded, which is during the copy, rather than in decode() above.
    response->set_parse_error("malformed submessage");
    return;
  }
  if (!message.IsInitialized()) {
    // Only TestAllRequiredTypesProto2 has required fields. Stock parsing
    // rejects a payload that leaves one unset.
    response->set_parse_error("required field missing");
    return;
  }
  if (!message.SerializeToString(response->mutable_protobuf_payload())) {
    response->set_serialize_error("stock protobuf could not serialize");
  }
}

void HandleRequest(const ConformanceRequest& request,
                   ConformanceResponse* response) {
  // wolaz only reads the binary wire format.
  switch (request.requested_output_format()) {
    case conformance::PROTOBUF:
      break;
    case conformance::JSON:
      response->set_skipped("wolaz has no JSON support");
      return;
    case conformance::TEXT_FORMAT:
      response->set_skipped("wolaz has no text-format support");
      return;
    default:
      response->set_runtime_error("unrecognised requested_output_format");
      return;
  }

  switch (request.payload_case()) {
    case ConformanceRequest::kProtobufPayload:
      break;
    case ConformanceRequest::kJsonPayload:
      response->set_skipped("wolaz has no JSON parser");
      return;
    case ConformanceRequest::kTextPayload:
      response->set_skipped("wolaz has no text-format parser");
      return;
    default:
      response->set_runtime_error("request has no recognised payload");
      return;
  }

  const std::string& type = request.message_type();
  const std::string& payload = request.protobuf_payload();

  if (type == "protobuf_test_messages.proto3.TestAllTypesProto3") {
    DecodeAndReserialize<w3::TestAllTypesProto3, s3::TestAllTypesProto3>(
        payload, response);
  } else if (type == "protobuf_test_messages.proto2.TestAllTypesProto2") {
    DecodeAndReserialize<w2::TestAllTypesProto2, s2::TestAllTypesProto2>(
        payload, response);
  } else if (type ==
             "protobuf_test_messages.proto2.TestAllRequiredTypesProto2") {
    DecodeAndReserialize<w2::TestAllRequiredTypesProto2,
                         s2::TestAllRequiredTypesProto2>(payload, response);
  } else {
    // Editions payload types are declared in separate .proto files that this
    // build does not generate; say so rather than silently failing.
    response->set_skipped("unsupported message type: " + type);
  }
}

}  // namespace

int main() {
  int handled = 0;
  for (;;) {
    std::uint32_t length = 0;
    if (!ReadExactly(&length, sizeof length)) break;  // harness closed stdin

    std::string serialized_request(length, '\0');
    if (length > 0 && !ReadExactly(&serialized_request[0], length)) {
      std::fprintf(stderr, "wolaz_conformer: truncated request body\n");
      return 1;
    }

    ConformanceRequest request;
    if (!request.ParseFromString(serialized_request)) {
      std::fprintf(stderr, "wolaz_conformer: unparseable ConformanceRequest\n");
      return 1;
    }

    ConformanceResponse response;
    HandleRequest(request, &response);

    std::string serialized_response;
    if (!response.SerializeToString(&serialized_response)) {
      std::fprintf(stderr, "wolaz_conformer: unserialisable response\n");
      return 1;
    }

    const std::uint32_t out_length =
        static_cast<std::uint32_t>(serialized_response.size());
    WriteExactly(&out_length, sizeof out_length);
    WriteExactly(serialized_response.data(), serialized_response.size());
    std::fflush(stdout);
    ++handled;
  }

  std::fprintf(stderr, "wolaz_conformer: %d requests handled\n", handled);
  return 0;
}
