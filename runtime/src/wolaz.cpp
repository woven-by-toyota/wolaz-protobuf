// Wolaz protobuf runtime: non-template wire-format helpers.
//
// These live out of line so that they are compiled once instead of in every
// translation unit that includes a generated header.

#include "wolaz.hpp"

#include <array>

namespace wolaz {

namespace {

// Varints carry seven bits per byte, low group first. The high bit of each
// byte says whether another byte follows.
constexpr std::uint64_t varint_payload_mask{0x7FU};
constexpr std::uint32_t varint_continuation_bit{0x80U};
constexpr std::uint32_t varint_bits_per_byte{7U};
// The tenth byte starts at bit 63, so it may only contribute its lowest bit.
constexpr std::uint32_t varint_tenth_byte_shift{63U};
constexpr std::uint32_t varint_tenth_byte_overflow{0x7EU};
// A 32-bit value needs at most five groups of seven bits.
constexpr std::uint32_t varint32_max_bytes{5U};

// A tag is (field number << 3) | wire type.
constexpr std::uint32_t tag_wire_type_bits{3U};
constexpr std::uint32_t tag_wire_type_mask{0x7U};

constexpr std::size_t fixed32_size{4U};
constexpr std::size_t fixed64_size{8U};
constexpr std::uint32_t bits_per_byte{8U};

// The byte at index `i` of `s`, as an unsigned value. The wire format is
// carried in `char` buffers so that it can be viewed directly from
// std::string; this is the one place that reinterprets a char as a byte.
std::uint32_t byte_at(const string_view s, const std::size_t i) noexcept {
  return static_cast<std::uint8_t>(s[i]);
}

}  // namespace

// cursor

optional<std::uint64_t> cursor::read_varint() noexcept {
  std::uint64_t result{0U};
  std::uint32_t shift{0U};
  while (pos_ < buf_.size()) {
    const std::uint32_t byte{byte_at(buf_, pos_)};
    ++pos_;
    // A tenth byte wider than one bit does not fit in 64 bits. Protobuf
    // requires over-long varints to be rejected rather than silently
    // truncated.
    if ((shift == varint_tenth_byte_shift) &&
        ((byte & varint_tenth_byte_overflow) != 0U)) {
      return nullopt;
    }
    result |= (static_cast<std::uint64_t>(byte) & varint_payload_mask) << shift;
    if ((byte & varint_continuation_bit) == 0U) {
      return optional<std::uint64_t>{result};
    }
    shift += varint_bits_per_byte;
    if (shift > varint_tenth_byte_shift) {
      return nullopt;
    }
  }
  return nullopt;  // ran off the end mid-varint
}

optional<std::uint32_t> cursor::read_varint32_strict() noexcept {
  std::uint64_t result{0U};
  // Anything that asks for a sixth byte is either out of range or an
  // overlong encoding of an in-range value; both are malformed.
  for (std::uint32_t i{0U}; i < varint32_max_bytes; ++i) {
    if (pos_ >= buf_.size()) {
      return nullopt;
    }
    const std::uint32_t byte{byte_at(buf_, pos_)};
    ++pos_;
    const std::uint32_t shift{i * varint_bits_per_byte};
    result |= (static_cast<std::uint64_t>(byte) & varint_payload_mask) << shift;
    if ((byte & varint_continuation_bit) == 0U) {
      if (result > std::numeric_limits<std::uint32_t>::max()) {
        return nullopt;
      }
      return optional<std::uint32_t>{static_cast<std::uint32_t>(result)};
    }
  }
  return nullopt;
}

optional<std::uint32_t> cursor::read_fixed32() noexcept {
  if (remaining() < fixed32_size) {
    return nullopt;
  }
  std::uint32_t v{0U};
  for (std::size_t i{0U}; i < fixed32_size; ++i) {
    const std::uint32_t byte{byte_at(buf_, pos_ + i)};
    v |= byte << (bits_per_byte * static_cast<std::uint32_t>(i));
  }
  pos_ += fixed32_size;
  return optional<std::uint32_t>{v};
}

optional<std::uint64_t> cursor::read_fixed64() noexcept {
  if (remaining() < fixed64_size) {
    return nullopt;
  }
  std::uint64_t v{0U};
  for (std::size_t i{0U}; i < fixed64_size; ++i) {
    const std::uint64_t byte{byte_at(buf_, pos_ + i)};
    v |= byte << (bits_per_byte * static_cast<std::uint32_t>(i));
  }
  pos_ += fixed64_size;
  return optional<std::uint64_t>{v};
}

optional<string_view> cursor::read_bytes(const std::size_t count) noexcept {
  if (remaining() < count) {
    return nullopt;
  }
  const string_view out{buf_.substr(pos_, count)};
  pos_ += count;
  return optional<string_view>{out};
}

optional<string_view> cursor::read_len_delim() noexcept {
  const optional<std::uint64_t> len{read_varint()};
  if ((!len.has_value()) || (*len > remaining())) {
    return nullopt;
  }
  return read_bytes(static_cast<std::size_t>(*len));
}

field_tag cursor::read_tag() noexcept {
  field_tag tag{tag_status::malformed, 0U, wire_type::varint};
  if (empty()) {
    tag.status = tag_status::end;
  } else {
    const optional<std::uint32_t> raw{read_varint32_strict()};
    if (raw.has_value()) {
      const std::uint32_t field{*raw >> tag_wire_type_bits};
      const std::uint32_t wire{*raw & tag_wire_type_mask};
      // Field 0 is never valid, and wire types 6 and 7 do not exist.
      if ((field != 0U) &&
          (wire <= static_cast<std::uint32_t>(wire_type::i32))) {
        tag.status = tag_status::ok;
        tag.field = field;
        tag.wire = static_cast<wire_type>(wire);
      }
    }
  }
  return tag;
}

bool cursor::skip_scalar(const wire_type wire) noexcept {
  bool ok{false};
  switch (wire) {
    case wire_type::varint:
      ok = read_varint().has_value();
      break;
    case wire_type::i64:
      ok = read_bytes(fixed64_size).has_value();
      break;
    case wire_type::i32:
      ok = read_bytes(fixed32_size).has_value();
      break;
    case wire_type::len:
      ok = read_len_delim().has_value();
      break;
    case wire_type::sgroup:
    case wire_type::egroup:
    default:
      ok = false;
      break;
  }
  return ok;
}

bool cursor::skip_field(const field_tag tag,
                        const std::int32_t depth) noexcept {
  bool ok{false};
  if (tag.wire == wire_type::sgroup) {
    ok = find_group_end(tag.field, depth).has_value();
  } else if (tag.wire == wire_type::egroup) {
    ok = false;  // an end tag with no matching start tag
  } else {
    ok = skip_scalar(tag.wire);
  }
  return ok;
}

// Groups nest, and each must be closed by an end tag with its own field
// number. Rather than recursing, the field numbers of the open groups are
// kept on a fixed-size stack, bounded by the recursion limit.
optional<std::size_t> cursor::find_group_end(
    const std::uint32_t field, const std::int32_t depth) noexcept {
  std::array<std::uint32_t, static_cast<std::size_t>(max_recursion_depth)>
      open{};
  std::size_t count{0U};
  optional<std::size_t> end_tag{};
  bool ok{depth < max_recursion_depth};
  if (ok) {
    open[0U] = field;
    count = 1U;
  }
  while (ok && (count > 0U)) {
    const std::size_t tag_start{pos_};
    const field_tag tag{read_tag()};
    if (tag.status != tag_status::ok) {
      ok = false;  // end of input, or a malformed tag, before the end tag
    } else if (tag.wire == wire_type::egroup) {
      // Only the matching field number closes the innermost group.
      ok = (tag.field == open[count - 1U]);
      --count;
      if (ok && (count == 0U)) {
        end_tag = optional<std::size_t>{tag_start};
      }
    } else if (tag.wire == wire_type::sgroup) {
      ok = (count < open.size()) &&
           ((depth + static_cast<std::int32_t>(count)) < max_recursion_depth);
      if (ok) {
        open[count] = tag.field;
        ++count;
      }
    } else {
      ok = skip_scalar(tag.wire);
    }
  }
  return ok ? end_tag : optional<std::size_t>{};
}

optional<string_view> cursor::read_group(const std::uint32_t field,
                                         const std::int32_t depth) noexcept {
  const std::size_t start{pos_};
  const optional<std::size_t> end{find_group_end(field, depth)};
  return end.has_value()
             ? optional<string_view>{buf_.substr(start, *end - start)}
             : optional<string_view>{};
}

// UTF-8 validation
//
// Checks each sequence against the table of well-formed UTF-8 byte sequences
// (Unicode Standard, Table 3-7), which is RFC 3629's grammar: it rejects
// over-long encodings, surrogates (U+D800..U+DFFF) and anything above
// U+10FFFF, all of which the conformance suite exercises.

namespace {

// A range of lead bytes, the length of the sequences they start, and the
// range permitted for the sequence's second byte. Every later byte is a plain
// continuation byte.
struct utf8_lead {
  std::uint32_t lead_min;
  std::uint32_t lead_max;
  std::size_t length;
  std::uint32_t second_min;
  std::uint32_t second_max;
};

constexpr std::array<utf8_lead, 8U> utf8_leads{{
    {0xC2U, 0xDFU, 2U, 0x80U, 0xBFU},
    {0xE0U, 0xE0U, 3U, 0xA0U, 0xBFU},  // excludes over-long 3-byte forms
    {0xE1U, 0xECU, 3U, 0x80U, 0xBFU},
    {0xEDU, 0xEDU, 3U, 0x80U, 0x9FU},  // excludes surrogates
    {0xEEU, 0xEFU, 3U, 0x80U, 0xBFU},
    {0xF0U, 0xF0U, 4U, 0x90U, 0xBFU},  // excludes over-long 4-byte forms
    {0xF1U, 0xF3U, 4U, 0x80U, 0xBFU},
    {0xF4U, 0xF4U, 4U, 0x80U, 0x8FU},  // caps at U+10FFFF
}};

constexpr std::uint32_t utf8_ascii_max{0x7FU};
constexpr std::uint32_t utf8_continuation_min{0x80U};
constexpr std::uint32_t utf8_continuation_max{0xBFU};

// The table row for lead byte `b`. Nothing for 0x80..0xC1 and 0xF5..0xFF,
// which never start a sequence.
optional<utf8_lead> find_utf8_lead(const std::uint32_t b) noexcept {
  optional<utf8_lead> found{};
  for (const utf8_lead& row : utf8_leads) {
    if ((b >= row.lead_min) && (b <= row.lead_max)) {
      found = optional<utf8_lead>{row};
    }
  }
  return found;
}

// The length of the multi-byte sequence starting at `s[i]`, or 0 if it is
// not well formed.
std::size_t utf8_sequence_length(const string_view s,
                                 const std::size_t i) noexcept {
  const optional<utf8_lead> lead{find_utf8_lead(byte_at(s, i))};
  bool valid{lead.has_value() && ((s.size() - i) >= lead->length)};
  if (valid) {
    const std::uint32_t second{byte_at(s, i + 1U)};
    valid = (second >= lead->second_min) && (second <= lead->second_max);
  }
  for (std::size_t k{2U}; valid && (k < lead->length); ++k) {
    const std::uint32_t next{byte_at(s, i + k)};
    valid = (next >= utf8_continuation_min) && (next <= utf8_continuation_max);
  }
  return valid ? lead->length : 0U;
}

}  // namespace

bool is_valid_utf8(const string_view s) noexcept {
  bool valid{true};
  std::size_t i{0U};
  while (valid && (i < s.size())) {
    if (byte_at(s, i) <= utf8_ascii_max) {
      ++i;
    } else {
      const std::size_t length{utf8_sequence_length(s, i)};
      valid = (length != 0U);
      i += length;
    }
  }
  return valid;
}

}  // namespace wolaz
