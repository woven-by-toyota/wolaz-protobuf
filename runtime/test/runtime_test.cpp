// Runtime unit tests.
//
// These cover the wire-format layer directly, independent of the code
// generator, so that a conformance failure later can be localised. Input is
// written out by hand, or produced with stock protobuf's encoder.

#include <wolaz.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// Stock protobuf, to produce encoded test input.
#include "google/protobuf/io/coded_stream.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,        \
                   __LINE__, #cond);                                     \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

#define CHECK_EQ(a, b)                                                   \
  do {                                                                   \
    auto va = (a);                                                       \
    auto vb = (b);                                                       \
    if (!(va == vb)) {                                                   \
      std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s != %s\n",         \
                   __FILE__, __LINE__, #a, #b);                          \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

// Turns "0aff" into the two bytes 0x0a 0xff.
std::string unhex(std::string_view h) {
  auto nibble = [](char c) {
    return c <= '9' ? c - '0' : c - 'a' + 10;
  };
  std::string out;
  for (std::size_t i = 0; i + 1 < h.size(); i += 2)
    out += static_cast<char>(nibble(h[i]) << 4 | nibble(h[i + 1]));
  return out;
}

// -- varints ----------------------------------------------------------------

void test_varint_roundtrip() {
  // Bytes produced by stock protobuf's encoder, decoded by wolaz.
  const std::uint64_t values[] = {
      0, 1, 127, 128, 300, 16383, 16384, 0x7fffffff, 0x80000000ull,
      0xffffffffull, 0xffffffffffffffffull};
  for (std::uint64_t v : values) {
    std::uint8_t buf[10];  // a 64-bit varint is at most ten bytes
    const std::uint8_t* end =
        google::protobuf::io::CodedOutputStream::WriteVarint64ToArray(v, buf);
    wolaz::cursor c(wolaz::string_view(reinterpret_cast<const char*>(buf),
                                       static_cast<std::size_t>(end - buf)));
    const wolaz::optional<std::uint64_t> got = c.read_varint();
    CHECK(got.has_value() && *got == v);
    CHECK(c.empty());
  }
}

void test_varint_known_encodings() {
  // The spec's worked examples.
  struct Case {
    const char* hex;
    std::uint64_t value;
  } cases[] = {
      {"00", 0},
      {"01", 1},
      {"7f", 127},
      {"8001", 128},
      {"ac02", 300},
      {"ffffffffffffffffff01", 0xffffffffffffffffull},
  };
  for (const auto& c : cases) {
    const std::string bytes = unhex(c.hex);
    wolaz::cursor cur(bytes);
    const wolaz::optional<std::uint64_t> got = cur.read_varint();
    CHECK(got.has_value() && *got == c.value);
    CHECK(cur.empty());
  }
}

void test_varint_rejects_overlong() {
  // 11 bytes, and a 10th byte contributing more than one bit: both invalid.
  const char too_long[] = "\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x01";
  wolaz::cursor c1(std::string_view(too_long, 11));
  CHECK(!c1.read_varint().has_value());

  const char wide_tenth[] = "\xff\xff\xff\xff\xff\xff\xff\xff\xff\x7f";
  wolaz::cursor c2(std::string_view(wide_tenth, 10));
  CHECK(!c2.read_varint().has_value());

  // Truncated mid-varint.
  const char truncated[] = "\x80\x80";
  wolaz::cursor c3(std::string_view(truncated, 2));
  CHECK(!c3.read_varint().has_value());
}

// -- zigzag -----------------------------------------------------------------

void test_zigzag() {
  // The spec's table of encoded values.
  CHECK_EQ(wolaz::zigzag_decode32(0u), 0);
  CHECK_EQ(wolaz::zigzag_decode32(1u), -1);
  CHECK_EQ(wolaz::zigzag_decode32(2u), 1);
  CHECK_EQ(wolaz::zigzag_decode32(3u), -2);
  CHECK_EQ(wolaz::zigzag_decode32(4294967294u), 2147483647);
  CHECK_EQ(wolaz::zigzag_decode32(4294967295u), -2147483647 - 1);

  CHECK_EQ(wolaz::zigzag_decode64(0u), std::int64_t{0});
  CHECK_EQ(wolaz::zigzag_decode64(1u), std::int64_t{-1});
  CHECK_EQ(wolaz::zigzag_decode64(2u), std::int64_t{1});
  CHECK_EQ(wolaz::zigzag_decode64(0xfffffffffffffffeull), INT64_MAX);
  CHECK_EQ(wolaz::zigzag_decode64(0xffffffffffffffffull), INT64_MIN);
}

// -- fixed width ------------------------------------------------------------

void test_fixed() {
  // Little-endian.
  {
    const std::string bytes = unhex("78563412");
    wolaz::cursor c(bytes);
    const wolaz::optional<std::uint32_t> v = c.read_fixed32();
    CHECK(v.has_value() && *v == 0x12345678u);
  }
  {
    const std::string bytes = unhex("efcdab8967452301");
    wolaz::cursor c(bytes);
    const wolaz::optional<std::uint64_t> v = c.read_fixed64();
    CHECK(v.has_value() && *v == 0x0123456789abcdefull);
  }
  // IEEE 754 bit patterns.
  {
    const std::string bytes = unhex("0000803f");
    wolaz::cursor c(bytes);
    const wolaz::optional<float> f = wolaz::codec::float_::read(c);
    CHECK(f.has_value() && *f == 1.0f);
  }
  {
    const std::string bytes = unhex("000000000000f03f");
    wolaz::cursor c(bytes);
    const wolaz::optional<double> d = wolaz::codec::double_::read(c);
    CHECK(d.has_value() && *d == 1.0);
  }
  // Too short.
  {
    const std::string bytes = unhex("785634");
    wolaz::cursor c(bytes);
    CHECK(!c.read_fixed32().has_value());
  }
}

// -- tags and skipping ------------------------------------------------------

void test_tag_validation() {
  // field 1, wire type 2 -> 0x0a
  const char good[] = "\x0a";
  wolaz::cursor c(std::string_view(good, 1));
  const wolaz::field_tag t = c.read_tag();
  CHECK(t.status == wolaz::tag_status::ok);
  CHECK_EQ(t.field, 1u);
  CHECK(t.wire == wolaz::wire_type::len);

  // field number 0 is never valid
  const char zero_field[] = "\x00";
  wolaz::cursor c2(std::string_view(zero_field, 1));
  CHECK(c2.read_tag().status == wolaz::tag_status::malformed);

  // wire type 6 does not exist
  const char bad_wt[] = "\x0e";
  wolaz::cursor c3(std::string_view(bad_wt, 1));
  CHECK(c3.read_tag().status == wolaz::tag_status::malformed);

  // clean end of input is not an error
  wolaz::cursor c4(std::string_view("", 0));
  CHECK(c4.read_tag().status == wolaz::tag_status::end);
}

void test_skip_group() {
  // field 1 sgroup { field 2 varint 1 } egroup, then field 3 varint 7
  // 0b = tag(1, sgroup), 10 01 = tag(2,varint) 1, 0c = tag(1, egroup)
  // 18 07 = tag(3,varint) 7
  const char data[] = "\x0b\x10\x01\x0c\x18\x07";
  wolaz::cursor c(std::string_view(data, 6));

  const wolaz::field_tag t = c.read_tag();
  CHECK_EQ(t.field, 1u);
  CHECK(t.wire == wolaz::wire_type::sgroup);
  CHECK(c.skip_field(t, 0));

  const wolaz::field_tag t2 = c.read_tag();
  CHECK_EQ(t2.field, 3u);
  const wolaz::optional<std::uint64_t> v = c.read_varint();
  CHECK(v.has_value() && *v == 7u);
  CHECK(c.empty());
}

void test_skip_nested_groups() {
  // field 1 sgroup { field 2 sgroup { field 3 varint 5 } egroup 2 } egroup 1
  const char data[] = "\x0b\x13\x18\x05\x14\x0c";
  wolaz::cursor c(std::string_view(data, 6));
  CHECK(c.skip_field(c.read_tag(), 0));
  CHECK(c.empty());

  // The inner group closed with the outer group's end tag.
  const char crossed[] = "\x0b\x13\x0c\x14";
  wolaz::cursor c2(std::string_view(crossed, 4));
  CHECK(!c2.skip_field(c2.read_tag(), 0));
}

void test_skip_group_depth_limit() {
  // max_recursion_depth groups, each nested in the last, all closed.
  std::string nested;
  for (int i = 0; i < wolaz::max_recursion_depth; ++i) nested += '\x0b';
  for (int i = 0; i < wolaz::max_recursion_depth; ++i) nested += '\x0c';
  wolaz::cursor c(nested);
  CHECK(c.skip_field(c.read_tag(), 0));
  CHECK(c.empty());

  // One level deeper than that is refused.
  const std::string deeper = "\x0b" + nested + "\x0c";
  wolaz::cursor c2(deeper);
  CHECK(!c2.skip_field(c2.read_tag(), 0));
}

void test_skip_group_mismatched_end_tag() {
  // Group opened on field 1 but closed with an end tag for field 2.
  const char data[] = "\x0b\x14";
  wolaz::cursor c(std::string_view(data, 2));
  CHECK(!c.skip_field(c.read_tag(), 0));
}

void test_read_group() {
  // field 1 sgroup { field 2 varint 1 } egroup, then field 3 varint 7. The
  // body excludes the end tag.
  const char data[] = "\x0b\x10\x01\x0c\x18\x07";
  wolaz::cursor c(std::string_view(data, 6));
  const wolaz::field_tag t = c.read_tag();
  const wolaz::optional<wolaz::string_view> body = c.read_group(t.field, 0);
  CHECK(body.has_value() && *body == wolaz::string_view("\x10\x01", 2));
  CHECK_EQ(c.read_tag().field, 3u);
}

// -- subrange ---------------------------------------------------------------

void test_subrange_unpacked() {
  // field 1 varint 1, 2, 3 unpacked
  const char data[] = "\x08\x01\x08\x02\x08\x03";
  wolaz::subrange<wolaz::codec::int32_> r(std::string_view(data, 6), 1);

  std::vector<std::int32_t> got;
  for (std::int32_t v : r) got.push_back(v);
  CHECK_EQ(got.size(), std::size_t{3});
  CHECK_EQ(got[0], 1);
  CHECK_EQ(got[1], 2);
  CHECK_EQ(got[2], 3);
  CHECK_EQ(r.size(), std::size_t{3});
}

void test_subrange_packed() {
  // field 1 len [1,2,3]
  const char data[] = "\x0a\x03\x01\x02\x03";
  wolaz::subrange<wolaz::codec::int32_> r(std::string_view(data, 5), 1);
  std::vector<std::int32_t> got;
  for (std::int32_t v : r) got.push_back(v);
  CHECK_EQ(got.size(), std::size_t{3});
  CHECK_EQ(got[0], 1);
  CHECK_EQ(got[2], 3);
}

void test_subrange_mixed_packed_and_unpacked() {
  // Protobuf permits both forms for the same field in one message, and the
  // concatenation must be read as a single sequence.
  // packed [1,2], then unpacked 3, then packed [4]
  const char data[] = "\x0a\x02\x01\x02\x08\x03\x0a\x01\x04";
  wolaz::subrange<wolaz::codec::int32_> r(std::string_view(data, 9), 1);
  std::vector<std::int32_t> got;
  for (std::int32_t v : r) got.push_back(v);
  CHECK_EQ(got.size(), std::size_t{4});
  CHECK_EQ(got[0], 1);
  CHECK_EQ(got[1], 2);
  CHECK_EQ(got[2], 3);
  CHECK_EQ(got[3], 4);
}

void test_subrange_skips_other_fields() {
  // field 2 varint 9, field 1 varint 1, field 3 len "ab", field 1 varint 2
  const char data[] = "\x10\x09\x08\x01\x1a\x02\x61\x62\x08\x02";
  wolaz::subrange<wolaz::codec::int32_> r(std::string_view(data, 10), 1);
  std::vector<std::int32_t> got;
  for (std::int32_t v : r) got.push_back(v);
  CHECK_EQ(got.size(), std::size_t{2});
  CHECK_EQ(got[0], 1);
  CHECK_EQ(got[1], 2);
}

void test_subrange_empty_packed_run() {
  // An empty packed run yields no elements at all.
  const char data[] = "\x0a\x00";
  wolaz::subrange<wolaz::codec::int32_> r(std::string_view(data, 2), 1);
  CHECK(r.empty());
  CHECK_EQ(r.size(), std::size_t{0});
}

void test_subrange_strings() {
  // field 1 len "ab", field 1 len "cde"
  const char data[] = "\x0a\x02\x61\x62\x0a\x03\x63\x64\x65";
  wolaz::subrange<wolaz::codec::string_> r(std::string_view(data, 9), 1);
  std::vector<std::string> got;
  for (wolaz::string_view v : r) got.emplace_back(v);
  CHECK_EQ(got.size(), std::size_t{2});
  CHECK_EQ(got[0], std::string("ab"));
  CHECK_EQ(got[1], std::string("cde"));
}

void test_subrange_is_lazy() {
  // An empty-but-present repeated field must not require touching any payload:
  // constructing the subrange does no work, and begin() stops at the first
  // matching tag only.
  const char data[] = "\x10\x09";  // only field 2 present
  wolaz::subrange<wolaz::codec::int32_> r(std::string_view(data, 2), 1);
  CHECK(r.empty());
}

void test_subrange_reports_malformed() {
  // field 1 varint, then truncated mid-varint
  const char data[] = "\x08\x01\x08\x80";
  wolaz::subrange<wolaz::codec::int32_> r(std::string_view(data, 4), 1);
  auto it = r.begin();
  CHECK(it != r.end());
  CHECK_EQ(*it, 1);
  ++it;
  CHECK(it == r.end());
  CHECK(it.failed());
}

// -- UTF-8 ------------------------------------------------------------------

void test_string_view_points_into_input() {
  // A length-delimited payload is a view of the input, not a copy: its
  // begin() and end() are positions in the caller's buffer.
  const std::string input("\x03" "abc" "\x02" "de", 7);
  wolaz::cursor c(input);
  const wolaz::optional<wolaz::string_view> first = c.read_len_delim();
  const wolaz::optional<wolaz::string_view> second = c.read_len_delim();
  CHECK(first.has_value() && second.has_value());
  if (!first || !second) return;
  CHECK(first->begin() == input.data() + 1);
  CHECK(first->end() == input.data() + 4);
  CHECK(second->begin() == input.data() + 5);
  CHECK(second->end() == input.data() + input.size());
  CHECK_EQ(second->length(), std::size_t{2});

  std::string joined;
  for (char ch : *first) joined += ch;
  for (char ch : *second) joined += ch;
  CHECK_EQ(joined, std::string("abcde"));

  // Interoperates with std::string_view through the iterators.
  const std::string_view std_view(first->begin(),
                                  static_cast<std::size_t>(first->end() -
                                                           first->begin()));
  CHECK(std_view == "abc");
}

void test_field_span() {
  // A message of five 2-byte records; the field's records are the 2nd and 4th.
  const char data[] = "\x10\x09\x08\x01\x18\x05\x08\x02\x18\x06";
  const wolaz::string_view message(data, 10);

  wolaz::field_span none;
  CHECK(none.records() == 0U);
  CHECK(none.of(message).empty());

  wolaz::field_span span;
  span.add(2U, 4U, 1U);
  span.add(6U, 8U, 1U);
  CHECK(span.records() == 2U);
  CHECK(span.elements() == 2U);
  CHECK(span.of(message) == message.substr(2U, 6U));

  // A subrange given the span scans only it, and takes its size from it.
  wolaz::subrange<wolaz::codec::int32_> r(message, 1, 0, span);
  CHECK(r.bytes() == message.substr(2U, 6U));
  CHECK_EQ(r.size(), std::size_t{2});
  std::vector<std::int32_t> got;
  for (std::int32_t v : r) got.push_back(v);
  CHECK((got == std::vector<std::int32_t>{1, 2}));

  // Without one, it scans the whole message and counts by iterating.
  wolaz::subrange<wolaz::codec::int32_> whole(message, 1);
  CHECK(whole.bytes() == message);
  CHECK_EQ(whole.size(), std::size_t{2});
}

void test_subrange_skips_wrong_wire_type() {
  // field 1 varint 1, field 1 as I32 (a wire type an int32 cannot have),
  // field 1 varint 2. The I32 record is skipped, as parse() skips it.
  const char data[] = "\x08\x01\x0d\x00\x00\x00\x00\x08\x02";
  wolaz::subrange<wolaz::codec::int32_> r(std::string_view(data, 9), 1);
  std::vector<std::int32_t> got;
  auto it = r.begin();
  for (; it != r.end(); ++it) got.push_back(*it);
  CHECK((got == std::vector<std::int32_t>{1, 2}));
  CHECK(!it.failed());
}

void test_utf8() {
  CHECK(wolaz::is_valid_utf8(""));
  CHECK(wolaz::is_valid_utf8("hello"));
  CHECK(wolaz::is_valid_utf8("\x24"));                  // U+0024
  CHECK(wolaz::is_valid_utf8("\xc2\xa2"));              // U+00A2
  CHECK(wolaz::is_valid_utf8("\xe0\xa4\xb9"));          // U+0939
  CHECK(wolaz::is_valid_utf8("\xf0\x90\x8d\x88"));      // U+10348

  CHECK(!wolaz::is_valid_utf8("\x80"));                 // lone continuation
  CHECK(!wolaz::is_valid_utf8("\xc0\x80"));             // over-long NUL
  CHECK(!wolaz::is_valid_utf8("\xc1\xbf"));             // over-long
  CHECK(!wolaz::is_valid_utf8("\xe0\x80\xaf"));         // over-long 3-byte
  CHECK(!wolaz::is_valid_utf8("\xf0\x80\x80\xaf"));     // over-long 4-byte
  CHECK(!wolaz::is_valid_utf8("\xed\xa0\x80"));         // surrogate U+D800
  CHECK(!wolaz::is_valid_utf8("\xed\xbf\xbf"));         // surrogate U+DFFF
  CHECK(!wolaz::is_valid_utf8("\xf4\x90\x80\x80"));     // > U+10FFFF
  CHECK(!wolaz::is_valid_utf8("\xf5\x80\x80\x80"));     // invalid lead
  CHECK(!wolaz::is_valid_utf8("\xc2"));                 // truncated
  CHECK(!wolaz::is_valid_utf8("\xe0\xa4"));             // truncated
}

// -- scalar codecs ----------------------------------------------------------

void test_negative_int32_is_ten_bytes() {
  // int32 -1 arrives sign-extended to a full 64-bit varint; sint32 -1 is
  // zigzagged into a single byte.
  {
    const std::string bytes = unhex("ffffffffffffffffff01");
    wolaz::cursor c(bytes);
    const wolaz::optional<std::int32_t> v = wolaz::codec::int32_::read(c);
    CHECK(v.has_value() && *v == -1);
    CHECK(c.empty());
  }
  {
    const std::string bytes = unhex("01");
    wolaz::cursor c(bytes);
    const wolaz::optional<std::int32_t> v = wolaz::codec::sint32_::read(c);
    CHECK(v.has_value() && *v == -1);
  }
}

void test_string_codec_rejects_bad_utf8() {
  // tag(1,len), len 1, 0x80
  const char data[] = "\x0a\x01\x80";
  wolaz::cursor c(std::string_view(data, 3));
  CHECK(c.read_tag().status == wolaz::tag_status::ok);
  CHECK(!wolaz::codec::string_::read(c).has_value());

  // The same bytes are fine for a `bytes` field.
  wolaz::cursor c2(std::string_view(data, 3));
  CHECK(c2.read_tag().status == wolaz::tag_status::ok);
  const wolaz::optional<wolaz::string_view> out = wolaz::codec::bytes_::read(c2);
  CHECK(out.has_value() && out->size() == 1u);
}

}  // namespace

int main() {
  test_varint_roundtrip();
  test_varint_known_encodings();
  test_varint_rejects_overlong();
  test_zigzag();
  test_fixed();
  test_tag_validation();
  test_skip_group();
  test_skip_nested_groups();
  test_skip_group_depth_limit();
  test_skip_group_mismatched_end_tag();
  test_read_group();
  test_subrange_unpacked();
  test_subrange_packed();
  test_subrange_mixed_packed_and_unpacked();
  test_subrange_skips_other_fields();
  test_subrange_empty_packed_run();
  test_subrange_strings();
  test_subrange_is_lazy();
  test_subrange_reports_malformed();
  test_string_view_points_into_input();
  test_field_span();
  test_subrange_skips_wrong_wire_type();
  test_utf8();
  test_negative_int32_is_ten_bytes();
  test_string_codec_rejects_bad_utf8();

  if (failures != 0) {
    std::fprintf(stderr, "\n%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all runtime tests passed\n");
  return 0;
}
