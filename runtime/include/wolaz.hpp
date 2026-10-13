// Wolaz protobuf runtime library.
//
// A lazy, allocation-free decoder for the protobuf binary wire format.
//
// Decoding never copies and never allocates. A decoded message is a plain
// aggregate of fixed size: singular scalars are stored by value, singular
// strings and bytes as `string_view` pointing into the caller's input
// buffer, and repeated fields as a `wolaz::subrange`, which is just a
// (buffer, field number) pair. Nothing in a repeated field is touched until
// the caller iterates it, at which point elements are produced one at a time
// with no intermediate storage.
//
// Decoding is one level at a time. Decoding a message validates its own
// fields, but a submessage is only checked for being a well-formed
// length-delimited record or group. Its contents are decoded and validated
// when the caller decodes it with `lazy::get()`, which reports any error
// found then.
//
// The caller's input buffer must outlive any decoded message that borrows from
// it. This is the one invariant the zero-copy guarantee rests on.
//
// Wolaz does not encode. To produce protobuf bytes, use stock protobuf.
//
// The code is written to the AUTOSAR C++14 guidelines. The deviations it
// relies on are listed in runtime/README.md.

#ifndef WOLAZ_HPP
#define WOLAZ_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

// The runtime and the code generated against it are C++14. The newer library
// types they need, optional and string_view from C++17 and expected from
// C++23, are provided below as minimal versions in namespace wolaz.
// They are used even when compiling as C++17 or later, so that the runtime's
// out-of-line functions have the same signatures whichever standard each
// translation unit uses.

namespace wolaz {

// C++17 type-trait helpers

template <class...>
struct make_void {
  using type = void;
};
template <class... T>
using void_t = typename make_void<T...>::type;

template <bool B>
using bool_constant = std::integral_constant<bool, B>;

// string_view
//
// A non-owning view of a run of chars, like C++17 string_view, with only
// what decoding needs. It converts from anything with char data() and size(),
// such as std::string or std::string_view, and explicitly to std::string.

namespace detail {

// The length of a NUL-terminated string. constexpr, so that a string_view
// built from a string literal is constant-initialized.
constexpr std::size_t c_string_length(const char* const s) noexcept {
  std::size_t n{0U};
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

}  // namespace detail

class string_view final {
 public:
  using value_type = char;
  using size_type = std::size_t;
  using const_iterator = const char*;
  using iterator = const_iterator;

  constexpr string_view() noexcept : data_{nullptr}, size_{0U} {}
  constexpr string_view(const char* const chars, const size_type count) noexcept
      : data_{chars}, size_{count} {}
  // Implicit, like std::string_view's, so that string literals convert.
  constexpr string_view(const char* const s) noexcept
      : data_{s}, size_{detail::c_string_length(s)} {}

  // From any other contiguous char container, such as std::string.
  template <class S,
            class = typename std::enable_if<
                (!std::is_same<typename std::decay<S>::type,
                               string_view>::value) &&
                std::is_convertible<decltype(std::declval<const S&>().data()),
                                    const char*>::value>::type,
            class = decltype(std::declval<const S&>().size())>
  string_view(const S& s) noexcept : data_{s.data()}, size_{s.size()} {}

  constexpr const char* data() const noexcept { return data_; }
  constexpr size_type size() const noexcept { return size_; }
  constexpr size_type length() const noexcept { return size_; }
  constexpr bool empty() const noexcept { return size_ == 0U; }

  // Pointers into the viewed chars, so a view works with range-for and the
  // standard algorithms. end() is the one use of pointer arithmetic (deviation
  // from M5-0-15, see runtime/README.md). For an empty default-constructed
  // view both are nullptr, and nullptr + 0 is well defined.
  constexpr const_iterator begin() const noexcept { return data_; }
  constexpr const_iterator end() const noexcept { return data_ + size_; }
  constexpr char operator[](const size_type i) const noexcept {
    return data_[i];
  }

  // The `count` chars starting at `pos`. Requires pos + count <= size().
  constexpr string_view substr(const size_type pos,
                               const size_type count) const noexcept {
    return (count == 0U) ? string_view{} : string_view{&data_[pos], count};
  }

  explicit operator std::string() const { return std::string(data_, size_); }

  friend bool operator==(const string_view a, const string_view b) noexcept {
    return (a.size_ == b.size_) &&
           ((a.size_ == 0U) || (std::memcmp(a.data_, b.data_, a.size_) == 0));
  }
  friend bool operator!=(const string_view a, const string_view b) noexcept {
    return !(a == b);
  }

 private:
  const char* data_;
  size_type size_;
};

// Writes the viewed chars. A template so that only <iosfwd> is needed here;
// the stream is complete wherever this is actually used.
template <class Ch, class Tr>
std::basic_ostream<Ch, Tr>& operator<<(std::basic_ostream<Ch, Tr>& os,
                                       const string_view s) {
  return os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// optional
//
// Like C++17 std::optional, with only what generated code and its callers
// need. The value is held as a member, so `T` must be default-constructible,
// and an optional of a trivially copyable `T` is itself trivially copyable.
// Construction from a value of fundamental type is explicit.

class nullopt_t final {
 public:
  explicit constexpr nullopt_t(std::int32_t) noexcept {}
};
constexpr nullopt_t nullopt{0};

template <class T>
class optional final {
  static_assert(std::is_default_constructible<T>::value,
                "wolaz::optional<T> requires a default-constructible T");

 public:
  using value_type = T;

  constexpr optional() noexcept(std::is_nothrow_default_constructible<T>::value)
      : value_(), has_{false} {}
  constexpr optional(nullopt_t) noexcept(
      std::is_nothrow_default_constructible<T>::value)
      : value_(), has_{false} {}

  // Explicit for fundamental `T` (AUTOSAR A12-1-4), implicit otherwise.
  template <class U = T,
            typename std::enable_if<std::is_fundamental<U>::value,
                                    std::int32_t>::type = 0>
  explicit constexpr optional(const T& value) noexcept(
      std::is_nothrow_copy_constructible<T>::value)
      : value_(value), has_{true} {}
  template <class U = T,
            typename std::enable_if<!std::is_fundamental<U>::value,
                                    std::int32_t>::type = 0>
  constexpr optional(const T& value) noexcept(
      std::is_nothrow_copy_constructible<T>::value)
      : value_(value), has_{true} {}

  optional& operator=(nullopt_t) noexcept(
      std::is_nothrow_default_constructible<T>::value &&
      std::is_nothrow_move_assignable<T>::value) {
    reset();
    return *this;
  }

  template <class... Args>
  T& emplace(Args&&... args) noexcept(
      std::is_nothrow_constructible<T, Args...>::value &&
      std::is_nothrow_move_assignable<T>::value) {
    value_ = T(std::forward<Args>(args)...);
    has_ = true;
    return value_;
  }
  void reset() noexcept(std::is_nothrow_default_constructible<T>::value &&
                        std::is_nothrow_move_assignable<T>::value) {
    value_ = T();
    has_ = false;
  }

  constexpr bool has_value() const noexcept { return has_; }
  explicit constexpr operator bool() const noexcept { return has_; }

  // Requires has_value().
  T& operator*() noexcept { return value_; }
  constexpr const T& operator*() const noexcept { return value_; }
  T* operator->() noexcept { return &value_; }
  constexpr const T* operator->() const noexcept { return &value_; }

  template <class U>
  constexpr T value_or(U&& fallback) const
      noexcept(std::is_nothrow_copy_constructible<T>::value &&
               std::is_nothrow_constructible<T, U&&>::value) {
    return has_ ? value_ : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  T value_;
  bool has_;
};

// Errors
//
// An error is a `string_view` describing what went wrong. The messages are
// static strings, so a failed decode does not allocate. They are constexpr,
// so they are usable from other translation units' static initializers.
//
// Compare an error against these with `==`, which compares the text.
//
// They are static members of a class template, so that each is a single
// object defined in this header, whichever standard each translation unit
// uses.
namespace detail {
template <class Unused = void>
struct errors {
  static constexpr string_view truncated{"truncated input"};
  static constexpr string_view bad_varint{"malformed varint"};
  static constexpr string_view bad_tag{"malformed tag"};
  static constexpr string_view bad_wire_type{"unexpected wire type for field"};
  static constexpr string_view bad_length{
      "length delimiter exceeds remaining input"};
  static constexpr string_view unbalanced_group{"unbalanced group delimiters"};
  static constexpr string_view recursion_limit{"recursion limit exceeded"};
  static constexpr string_view invalid_utf8{"string field is not valid UTF-8"};
};
// C++17 makes constexpr static members inline, and deprecates these.
#if __cplusplus < 201703L
template <class Unused>
constexpr string_view errors<Unused>::truncated;
template <class Unused>
constexpr string_view errors<Unused>::bad_varint;
template <class Unused>
constexpr string_view errors<Unused>::bad_tag;
template <class Unused>
constexpr string_view errors<Unused>::bad_wire_type;
template <class Unused>
constexpr string_view errors<Unused>::bad_length;
template <class Unused>
constexpr string_view errors<Unused>::unbalanced_group;
template <class Unused>
constexpr string_view errors<Unused>::recursion_limit;
template <class Unused>
constexpr string_view errors<Unused>::invalid_utf8;
#endif
}  // namespace detail

using err = detail::errors<>;

// A minimal stand-in for C++23 `std::expected`, sufficient for generated code.
template <class T, class E = string_view>
class expected final {
 public:
  expected(const T& value) noexcept(
      std::is_nothrow_copy_constructible<T>::value &&
      std::is_nothrow_default_constructible<E>::value)
      : value_(value), err_() {}
  expected(const E& failure) noexcept(
      std::is_nothrow_default_constructible<T>::value &&
      std::is_nothrow_copy_constructible<E>::value)
      : value_(), err_(failure) {}

  explicit operator bool() const noexcept { return value_.has_value(); }
  bool has_value() const noexcept { return value_.has_value(); }

  // Requires has_value().
  T& operator*() noexcept { return *value_; }
  const T& operator*() const noexcept { return *value_; }
  T* operator->() noexcept { return &*value_; }
  const T* operator->() const noexcept { return &*value_; }

  // Empty when has_value().
  const E& error() const noexcept { return err_; }

 private:
  optional<T> value_;
  E err_;
};

// Wire format primitives

enum class wire_type : std::uint8_t {
  varint = 0U,
  i64 = 1U,
  len = 2U,
  sgroup = 3U,
  egroup = 4U,
  i32 = 5U,
};

// Matches the stock C++ implementation's limit, which the conformance suite
// relies on for its deeply-nested-message cases.
constexpr std::int32_t max_recursion_depth{100};

constexpr std::int64_t zigzag_decode64(const std::uint64_t v) noexcept {
  const std::uint64_t u{(v >> 1U) ^ (~(v & 1U) + 1U)};
  return static_cast<std::int64_t>(u);
}
constexpr std::int32_t zigzag_decode32(const std::uint32_t v) noexcept {
  const std::uint32_t u{(v >> 1U) ^ (~(v & 1U) + 1U)};
  return static_cast<std::int32_t>(u);
}

// The IEEE 754 value with the given bit pattern (deviation from M3-9-3, see
// runtime/README.md).
inline float bits_to_float(const std::uint32_t b) noexcept {
  static_assert(sizeof(float) == sizeof(std::uint32_t), "float is not 32-bit");
  float f{0.0F};
  static_cast<void>(std::memcpy(&f, &b, sizeof(f)));
  return f;
}
inline double bits_to_double(const std::uint64_t b) noexcept {
  static_assert(sizeof(double) == sizeof(std::uint64_t),
                "double is not 64-bit");
  double d{0.0};
  static_cast<void>(std::memcpy(&d, &b, sizeof(d)));
  return d;
}

// Validates that `s` is well-formed UTF-8. Required for `string` fields: the
// conformance suite rejects invalid UTF-8 in both directions.
bool is_valid_utf8(const string_view s) noexcept;

// The result of cursor::read_tag().
enum class tag_status : std::uint8_t {
  ok,         // a tag was read
  end,        // clean end of input
  malformed,  // the input does not start with a valid tag
};

// A field tag. `field` and `wire` are meaningful only when `status` is ok.
struct field_tag {
  tag_status status;
  std::uint32_t field;
  wire_type wire;
};

// cursor: a non-owning read head over a byte range
//
// Every read either succeeds and advances past what it read, or fails and
// returns nothing. A failed read may leave the cursor anywhere in the input,
// so a caller stops reading after the first failure.

class cursor final {
 public:
  constexpr cursor() noexcept : buf_{}, pos_{0U} {}
  explicit constexpr cursor(const string_view bytes) noexcept
      : buf_{bytes}, pos_{0U} {}

  constexpr bool empty() const noexcept { return pos_ >= buf_.size(); }
  constexpr std::size_t remaining() const noexcept {
    return buf_.size() - pos_;
  }
  // The number of bytes read so far.
  constexpr std::size_t position() const noexcept { return pos_; }

  optional<std::uint64_t> read_varint() noexcept;
  // Reads a varint that must fit in 32 bits using no more than five bytes.
  // Tags get this stricter treatment: an overlong encoding whose value happens
  // to land in range is still malformed, and the conformance suite checks it.
  optional<std::uint32_t> read_varint32_strict() noexcept;
  optional<std::uint32_t> read_fixed32() noexcept;
  optional<std::uint64_t> read_fixed64() noexcept;
  optional<string_view> read_bytes(const std::size_t count) noexcept;
  optional<string_view> read_len_delim() noexcept;

  field_tag read_tag() noexcept;

  // Advances past the body of a field whose tag was just read. `depth` is the
  // nesting depth of the message the field belongs to, which limits how
  // deeply groups may nest.
  bool skip_field(const field_tag tag, const std::int32_t depth) noexcept;

  // Reads the body of group `field`, whose start tag was just read: the
  // contents up to, but not including, the matching end tag. The cursor
  // ends up after the end tag. `depth` is as for skip_field().
  optional<string_view> read_group(const std::uint32_t field,
                                   const std::int32_t depth) noexcept;

 private:
  // Skips the body of a field whose wire type is not a group delimiter.
  bool skip_scalar(const wire_type wire) noexcept;
  // Skips to the end of group `field`, whose start tag was just read, and
  // returns the position of its end tag.
  optional<std::size_t> find_group_end(const std::uint32_t field,
                                       const std::int32_t depth) noexcept;

  string_view buf_;
  std::size_t pos_;
};

// field_span: where one field's records lie within a message
//
// A generated parse() walks every record of a message once. For each
// repeated or submessage field it notes, in a field_span, where the field's
// first record starts, where its last record ends, how many records it has
// and how many elements they hold. The field's subrange or lazy then holds
// only that span of the message, so reading the field later does not walk
// the rest of the message. See DEVELOPMENT.md, "How fields are found".
class field_span final {
 public:
  constexpr field_span() noexcept
      : begin_{0U}, end_{0U}, records_{0U}, elements_{0U} {}

  // Records one record of the field, occupying [record_begin, record_end) of
  // the message and holding `elements` elements.
  void add(const std::size_t record_begin, const std::size_t record_end,
           const std::size_t elements) noexcept {
    if (records_ == 0U) {
      begin_ = record_begin;
    }
    end_ = record_end;
    ++records_;
    elements_ += elements;
  }

  // The part of `message` from the start of the first record to the end of
  // the last, which is empty if there were none.
  constexpr string_view of(const string_view message) const noexcept {
    return message.substr(begin_, end_ - begin_);
  }
  constexpr std::size_t records() const noexcept { return records_; }
  constexpr std::size_t elements() const noexcept { return elements_; }

 private:
  std::size_t begin_;
  std::size_t end_;
  std::size_t records_;
  std::size_t elements_;
};

// lazy: an undecoded submessage
//
// A message type cannot contain itself by value, and protobuf definitions are
// freely recursive (`TestAllTypesProto3.NestedMessage` has a field of type
// `TestAllTypesProto3`). Stock protobuf solves this with heap-allocated
// children, which is exactly what this library exists to avoid.
//
// Instead a submessage field holds only the view of its own encoded bytes.
// `lazy<M>` is therefore the same size regardless of `M`, which breaks the
// recursion, and it is also the laziest possible representation: a nested
// message costs nothing until the caller asks for it.
//
// Merging
//
// The wire format permits a *singular* message field to appear more than once,
// in which case the occurrences merge (scalars last-wins, repeated fields
// concatenate, submessages merge recursively). Merging in memory would mean
// allocating somewhere to hold the combined result, so instead a singular
// message field remembers where to find all of its occurrences.
//
// `get()` decodes the final occurrence, which is the one that wins for every
// scalar. If a field may legitimately arrive split across occurrences and you
// need the merged view, decode each occurrence from `for_each_occurrence` in
// turn: applying them in order gives the merged result.

// Selects a generated message type's `parse()` overload, which is found by
// argument-dependent lookup in the message's namespace.
template <class M>
class type_tag final {};

template <class M>
class lazy final {
 public:
  constexpr lazy() noexcept
      : bytes_{},
        parent_{},
        field_{0U},
        depth_{0},
        present_{false},
        group_{false} {}

  // A single, already-isolated body: one element of a repeated field, or a
  // singular message field that occurred exactly once.
  lazy(const string_view body, const std::int32_t nesting) noexcept
      : bytes_{body},
        parent_{},
        field_{0U},
        depth_{nesting},
        present_{true},
        group_{false} {}

  // All occurrences of `field` within `parent`, which is the enclosing
  // message or the part of it holding every occurrence; `body` is the final
  // occurrence.
  lazy(const string_view parent, const std::uint32_t field,
       const std::int32_t nesting, const bool group,
       const string_view body) noexcept
      : bytes_{body},
        parent_{parent},
        field_{field},
        depth_{nesting},
        present_{true},
        group_{group} {}

  explicit operator bool() const noexcept { return present_; }
  bool has_value() const noexcept { return present_; }

  // The still-encoded bytes of the final occurrence.
  string_view bytes() const noexcept { return bytes_; }
  // The nesting depth of the submessage.
  std::int32_t depth() const noexcept { return depth_; }

  // Decodes and validates the submessage. Cheap enough to call repeatedly,
  // but each call re-walks the submessage, so hold on to the result if you
  // need it twice.
  expected<M> get() const noexcept {
    return parse(type_tag<M>{}, bytes_, depth_);
  }
  expected<M> operator*() const noexcept { return get(); }

  // Calls `f(string_view body)` once per occurrence, in wire order.
  // Stops and returns false as soon as `f` does. For a field that occurred
  // once, which is the usual case, this calls `f` directly; only a field that
  // occurred more than once walks the span of the message holding them.
  template <class F>
  bool for_each_occurrence(F&& f) const {
    bool keep_going{true};
    if (!present_) {
      // No occurrences.
    } else if (parent_.empty()) {
      keep_going = f(bytes_);
    } else {
      cursor c{parent_};
      bool done{false};
      while (!done) {
        const field_tag tag{c.read_tag()};
        if (tag.status != tag_status::ok) {
          done = true;
        } else if ((tag.field != field_) ||
                   (tag.wire != (group_ ? wire_type::sgroup : wire_type::len))) {
          // Another field, or this one with a wire type it cannot have, which
          // parse() skipped too.
          done = !c.skip_field(tag, depth_);
        } else {
          const optional<string_view> body{
              group_ ? c.read_group(tag.field, depth_) : c.read_len_delim()};
          if (!body.has_value()) {
            done = true;
          } else if (!f(*body)) {
            keep_going = false;
            done = true;
          } else {
            // On to the next occurrence.
          }
        }
      }
    }
    return keep_going;
  }

 private:
  string_view bytes_;
  string_view parent_;
  std::uint32_t field_;
  std::int32_t depth_;
  bool present_;
  bool group_;
};

// Field codecs
//
// A codec maps a protobuf field type onto a C++ value type and knows how to
// read it. `subrange` is parameterised on codecs rather than C++
// types because the C++ type alone is ambiguous: int64, sint64 and sfixed64 all
// surface as std::int64_t but use three different encodings.

namespace detail {

constexpr std::int32_t varint_to_int32(const std::uint64_t v) noexcept {
  return static_cast<std::int32_t>(v);
}
constexpr std::int64_t varint_to_int64(const std::uint64_t v) noexcept {
  return static_cast<std::int64_t>(v);
}
constexpr std::uint32_t varint_to_uint32(const std::uint64_t v) noexcept {
  return static_cast<std::uint32_t>(v);
}
constexpr std::uint64_t varint_to_uint64(const std::uint64_t v) noexcept {
  return v;
}
constexpr std::int32_t varint_to_sint32(const std::uint64_t v) noexcept {
  return zigzag_decode32(static_cast<std::uint32_t>(v));
}
constexpr std::int64_t varint_to_sint64(const std::uint64_t v) noexcept {
  return zigzag_decode64(v);
}
constexpr bool varint_to_bool(const std::uint64_t v) noexcept {
  return v != 0U;
}

constexpr std::uint32_t fixed32_to_uint32(const std::uint32_t v) noexcept {
  return v;
}
constexpr std::int32_t fixed32_to_int32(const std::uint32_t v) noexcept {
  return static_cast<std::int32_t>(v);
}
constexpr std::uint64_t fixed64_to_uint64(const std::uint64_t v) noexcept {
  return v;
}
constexpr std::int64_t fixed64_to_int64(const std::uint64_t v) noexcept {
  return static_cast<std::int64_t>(v);
}

}  // namespace detail

namespace codec {

// A field type encoded as a varint, converted to `T` by `Convert`.
template <class T, T (*Convert)(std::uint64_t)>
class varint final {
 public:
  using value_type = T;
  static constexpr wire_type wire{wire_type::varint};
  static constexpr bool packable{true};
  static optional<T> read(cursor& c) noexcept {
    const optional<std::uint64_t> v{c.read_varint()};
    return v.has_value() ? optional<T>{Convert(*v)} : optional<T>{};
  }
};

// A field type encoded as 4 little-endian bytes, converted to `T`.
template <class T, T (*Convert)(std::uint32_t)>
class fixed32 final {
 public:
  using value_type = T;
  static constexpr wire_type wire{wire_type::i32};
  static constexpr bool packable{true};
  static optional<T> read(cursor& c) noexcept {
    const optional<std::uint32_t> v{c.read_fixed32()};
    return v.has_value() ? optional<T>{Convert(*v)} : optional<T>{};
  }
};

// A field type encoded as 8 little-endian bytes, converted to `T`.
template <class T, T (*Convert)(std::uint64_t)>
class fixed64 final {
 public:
  using value_type = T;
  static constexpr wire_type wire{wire_type::i64};
  static constexpr bool packable{true};
  static optional<T> read(cursor& c) noexcept {
    const optional<std::uint64_t> v{c.read_fixed64()};
    return v.has_value() ? optional<T>{Convert(*v)} : optional<T>{};
  }
};

using int32_ = varint<std::int32_t, &detail::varint_to_int32>;
using int64_ = varint<std::int64_t, &detail::varint_to_int64>;
using uint32_ = varint<std::uint32_t, &detail::varint_to_uint32>;
using uint64_ = varint<std::uint64_t, &detail::varint_to_uint64>;
using sint32_ = varint<std::int32_t, &detail::varint_to_sint32>;
using sint64_ = varint<std::int64_t, &detail::varint_to_sint64>;
using bool_ = varint<bool, &detail::varint_to_bool>;
using fixed32_ = fixed32<std::uint32_t, &detail::fixed32_to_uint32>;
using sfixed32_ = fixed32<std::int32_t, &detail::fixed32_to_int32>;
using float_ = fixed32<float, &bits_to_float>;
using fixed64_ = fixed64<std::uint64_t, &detail::fixed64_to_uint64>;
using sfixed64_ = fixed64<std::int64_t, &detail::fixed64_to_int64>;
using double_ = fixed64<double, &bits_to_double>;

// Enums travel as varints of their 32-bit signed value.
//
// Wolaz treats every enum as open, including proto2 closed enums: any value
// on the wire is returned as is, even one no enumerator has (deviation from
// A7-2-1, see runtime/README.md). Stock protobuf instead diverts an
// unrecognised value of a closed enum into its unknown-field set. Use the
// generated `<Enum>_IsValid()` to tell the two cases apart.
template <class E>
class enum_ final {
  static_assert(std::is_enum<E>::value, "enum_<E> requires an enum type");

 public:
  using value_type = E;
  static constexpr wire_type wire{wire_type::varint};
  static constexpr bool packable{true};
  static optional<E> read(cursor& c) noexcept {
    const optional<std::uint64_t> v{c.read_varint()};
    return v.has_value()
               ? optional<E>{static_cast<E>(detail::varint_to_int32(*v))}
               : optional<E>{};
  }
};

// `bytes`: an uninterpreted length-delimited payload.
class bytes_ final {
 public:
  using value_type = string_view;
  static constexpr wire_type wire{wire_type::len};
  static constexpr bool packable{false};
  static constexpr bool is_string{true};
  static optional<string_view> read(cursor& c) noexcept {
    return c.read_len_delim();
  }
};

// `string`: like bytes, but must be valid UTF-8.
class string_ final {
 public:
  using value_type = string_view;
  static constexpr wire_type wire{wire_type::len};
  static constexpr bool packable{false};
  static constexpr bool is_string{true};
  static optional<string_view> read(cursor& c) noexcept {
    const optional<string_view> s{c.read_len_delim()};
    return (s.has_value() && is_valid_utf8(*s)) ? s : optional<string_view>{};
  }
};

// A length-delimited submessage. Yields `lazy<M>`: iterating a repeated
// message field hands back undecoded views, so elements the caller never
// dereferences are never walked.
template <class M>
class message_ final {
 public:
  using value_type = lazy<M>;
  static constexpr wire_type wire{wire_type::len};
  static constexpr bool packable{false};
  static constexpr bool is_string{false};
};

// A proto2 group: the same payload as a submessage, delimited by matching
// start/end tags instead of a length prefix.
template <class M>
class group_ final {
 public:
  using value_type = lazy<M>;
  static constexpr wire_type wire{wire_type::sgroup};
  static constexpr bool packable{false};
  static constexpr bool is_string{false};
};

}  // namespace codec

// The number of elements in `run`, the payload of a packed repeated field,
// or nothing if it is not a whole number of well-formed elements of codec C.
template <class C>
optional<std::size_t> count_packed(const string_view run) noexcept {
  cursor c{run};
  std::size_t count{0U};
  bool ok{true};
  while (ok && (!c.empty())) {
    ok = C::read(c).has_value();
    ++count;
  }
  return ok ? optional<std::size_t>{count} : optional<std::size_t>{};
}

// Lets `subrange` be spelled either with an explicit codec
// (`subrange<codec::sint32_>`) or, where the C++ type is unambiguous, with the
// plain value type (`subrange<string_view>`).
template <class T, class = void>
struct default_codec;

template <>
struct default_codec<string_view> {
  using type = codec::bytes_;
};
template <>
struct default_codec<float> {
  using type = codec::float_;
};
template <>
struct default_codec<double> {
  using type = codec::double_;
};
template <>
struct default_codec<bool> {
  using type = codec::bool_;
};
template <>
struct default_codec<std::int32_t> {
  using type = codec::int32_;
};
template <>
struct default_codec<std::int64_t> {
  using type = codec::int64_;
};
template <>
struct default_codec<std::uint32_t> {
  using type = codec::uint32_;
};
template <>
struct default_codec<std::uint64_t> {
  using type = codec::uint64_;
};

namespace detail {
template <class T, class = void>
struct is_codec : std::false_type {};
template <class T>
struct is_codec<T, void_t<typename T::value_type, decltype(T::wire)>>
    : std::true_type {};

template <class T>
using resolve_codec_t =
    typename std::conditional<is_codec<T>::value, std::enable_if<true, T>,
                              default_codec<T>>::type::type;

// Per-codec properties are probed through traits that default sensibly, since
// not every codec declares every property.
template <class C, class = void>
struct is_string_codec : std::false_type {};
template <class C>
struct is_string_codec<C, void_t<decltype(C::is_string)>>
    : bool_constant<C::is_string> {};

// How a repeated element of codec C is read from the wire, as a tag type for
// overload selection: a length-delimited submessage, a group, or a scalar
// (which includes string and bytes).
struct message_element {};
struct group_element {};
struct scalar_element {};

template <class C>
using element_kind = typename std::conditional<
    (C::wire == wire_type::len) && (!is_string_codec<C>::value),
    message_element,
    typename std::conditional<C::wire == wire_type::sgroup, group_element,
                              scalar_element>::type>::type;
}  // namespace detail

// subrange: a lazy view of one repeated field
//
// Holds only the enclosing message's bytes and a field number. Protobuf allows
// the elements of a repeated field to be interleaved with other fields, and a
// repeated field of a primitive numeric type (one that uses the VARINT, I32 or
// I64 wire type) to arrive packed, unpacked, or split across several packed
// records. Parsers must accept all of these, so the iterator scans the whole
// message, skipping other fields and descending into packed runs.

template <class Codec>
class subrange_iterator final {
 public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = typename Codec::value_type;
  using difference_type = std::ptrdiff_t;
  using pointer = const value_type*;
  using reference = const value_type&;

  subrange_iterator() noexcept
      : outer_{},
        packed_{},
        field_{0U},
        depth_{0},
        value_(),
        in_packed_{false},
        done_{true},
        failed_{false} {}

  // The first element of `field` in `message`, a message at nesting depth
  // `depth`; or, if `at_end`, the end.
  subrange_iterator(const string_view message, const std::uint32_t field,
                    const std::int32_t depth, const bool at_end) noexcept
      : outer_{message},
        packed_{},
        field_{field},
        depth_{depth},
        value_(),
        in_packed_{false},
        done_{at_end},
        failed_{false} {
    if (!done_) {
      advance();
    }
  }

  reference operator*() const noexcept { return value_; }
  pointer operator->() const noexcept { return &value_; }

  subrange_iterator& operator++() noexcept {
    advance();
    return *this;
  }
  subrange_iterator operator++(int) noexcept {
    const subrange_iterator copy{*this};
    advance();
    return copy;
  }

  friend bool operator==(const subrange_iterator& a,
                         const subrange_iterator& b) noexcept {
    return (a.done_ || b.done_)
               ? (a.done_ == b.done_)
               : ((a.outer_.position() == b.outer_.position()) &&
                  (a.packed_.position() == b.packed_.position()));
  }
  friend bool operator!=(const subrange_iterator& a,
                         const subrange_iterator& b) noexcept {
    return !(a == b);
  }

  // True if the field data was malformed. Iteration stops at the first bad
  // element, so a caller that must distinguish "ended" from "gave up"
  // checks this after the loop.
  bool failed() const noexcept { return failed_; }

 private:
  using packable = bool_constant<Codec::packable>;

  void advance() noexcept;

  // Handles an occurrence of the field. Returns true once advance() is
  // finished, false to keep scanning.
  bool take(const field_tag tag) noexcept;

  // The parts of advance() that only some codecs can compile, chosen by
  // overload: packed runs exist only for packable codecs, and each element
  // kind is read differently.
  bool resume_packed(std::true_type) noexcept;
  bool resume_packed(std::false_type) noexcept { return false; }
  enum class packed_start : std::uint8_t { not_packed, started, empty_run };
  packed_start start_packed(const wire_type wire, std::true_type) noexcept;
  packed_start start_packed(const wire_type, std::false_type) noexcept {
    return packed_start::not_packed;
  }
  void read_packed() noexcept;
  void read_element(const field_tag, detail::message_element) noexcept;
  void read_element(const field_tag tag, detail::group_element) noexcept;
  void read_element(const field_tag, detail::scalar_element) noexcept;
  void fail() noexcept {
    failed_ = true;
    done_ = true;
  }

  cursor outer_;
  cursor packed_;
  std::uint32_t field_;
  std::int32_t depth_;
  value_type value_;
  bool in_packed_;
  bool done_;
  bool failed_;
};

template <class CodecOrValue>
class subrange final {
 public:
  using codec_type = detail::resolve_codec_t<CodecOrValue>;
  using value_type = typename codec_type::value_type;
  using iterator = subrange_iterator<codec_type>;

  constexpr subrange() noexcept
      : msg_{}, field_{0U}, depth_{0}, count_{} {}

  // Field `field` of `message`, a message at nesting depth `depth`. The
  // whole message is scanned for the field's records, and size() counts them.
  subrange(const string_view message, const std::uint32_t field,
           const std::int32_t depth = 0) noexcept
      : msg_{message}, field_{field}, depth_{depth}, count_{} {}

  // As above, for a field whose records parse() has already found: only
  // `span` of the message is scanned, and size() is the span's element
  // count. This is what generated code uses.
  subrange(const string_view message, const std::uint32_t field,
           const std::int32_t depth, const field_span span) noexcept
      : msg_{span.of(message)},
        field_{field},
        depth_{depth},
        count_{span.elements()} {}

  iterator begin() const noexcept {
    return iterator{msg_, field_, depth_, msg_.empty()};
  }
  iterator end() const noexcept { return iterator{msg_, field_, depth_, true}; }

  bool empty() const noexcept {
    return count_.has_value() ? (*count_ == 0U) : (begin() == end());
  }

  // O(1) for a subrange from generated code. Otherwise it walks the field,
  // which is O(n) in the size of the message.
  std::size_t size() const noexcept {
    return count_.has_value()
               ? *count_
               : static_cast<std::size_t>(std::distance(begin(), end()));
  }

  // The bytes the iterator scans: the field's span of the message, or the
  // whole message.
  string_view bytes() const noexcept { return msg_; }

 private:
  string_view msg_;
  std::uint32_t field_;
  std::int32_t depth_;
  optional<std::size_t> count_;
};

template <class Codec>
void subrange_iterator<Codec>::advance() noexcept {
  // Continue a packed run if one is in progress.
  bool finished{resume_packed(packable{})};
  while ((!finished) && (!done_)) {
    const field_tag tag{outer_.read_tag()};
    if (tag.status != tag_status::ok) {
      failed_ = (tag.status == tag_status::malformed);
      done_ = true;
    } else if (tag.field != field_) {
      if (!outer_.skip_field(tag, depth_)) {
        fail();
      }
    } else {
      finished = take(tag);
    }
  }
}

template <class Codec>
bool subrange_iterator<Codec>::take(const field_tag tag) noexcept {
  bool finished{true};
  // A packable field arriving length-delimited is a packed run.
  const packed_start start{start_packed(tag.wire, packable{})};
  if (start == packed_start::empty_run) {
    finished = false;  // an empty run contributes no elements
  } else if (start == packed_start::started) {
    // The run's first element has been read.
  } else if (tag.wire != Codec::wire) {
    // A wire type this field cannot have: skipped, as parse() skips it.
    if (!outer_.skip_field(tag, depth_)) {
      fail();
    }
    finished = false;
  } else {
    read_element(tag, detail::element_kind<Codec>{});
  }
  return finished;
}

// Returns true if advance() is finished: either the next element of the run
// was read, or the run was malformed.
template <class Codec>
bool subrange_iterator<Codec>::resume_packed(std::true_type) noexcept {
  bool finished{false};
  if (!in_packed_) {
    // No run in progress.
  } else if (!packed_.empty()) {
    read_packed();
    finished = true;
  } else {
    in_packed_ = false;
    packed_ = cursor{};
  }
  return finished;
}

template <class Codec>
typename subrange_iterator<Codec>::packed_start
subrange_iterator<Codec>::start_packed(const wire_type wire,
                                       std::true_type) noexcept {
  packed_start start{packed_start::not_packed};
  if (wire == wire_type::len) {
    const optional<string_view> payload{outer_.read_len_delim()};
    if (!payload.has_value()) {
      fail();
      start = packed_start::started;
    } else if (payload->empty()) {
      start = packed_start::empty_run;
    } else {
      packed_ = cursor{*payload};
      in_packed_ = true;
      read_packed();
      start = packed_start::started;
    }
  }
  return start;
}

template <class Codec>
void subrange_iterator<Codec>::read_packed() noexcept {
  const optional<value_type> v{Codec::read(packed_)};
  if (v.has_value()) {
    value_ = *v;
  } else {
    fail();
  }
}

// Repeated submessage: hand back the undecoded bytes.
template <class Codec>
void subrange_iterator<Codec>::read_element(
    const field_tag, detail::message_element) noexcept {
  const optional<string_view> payload{outer_.read_len_delim()};
  if (payload.has_value()) {
    value_ = value_type{*payload, depth_ + 1};
  } else {
    fail();
  }
}

// Repeated group: the body runs to the matching end tag.
template <class Codec>
void subrange_iterator<Codec>::read_element(const field_tag tag,
                                            detail::group_element) noexcept {
  const optional<string_view> payload{outer_.read_group(tag.field, depth_)};
  if (payload.has_value()) {
    value_ = value_type{*payload, depth_ + 1};
  } else {
    fail();
  }
}

template <class Codec>
void subrange_iterator<Codec>::read_element(
    const field_tag, detail::scalar_element) noexcept {
  const optional<value_type> v{Codec::read(outer_)};
  if (v.has_value()) {
    value_ = *v;
  } else {
    fail();
  }
}

// Decoding entry point
//
// Decodes and validates one message, at nesting depth `depth`. Submessages
// are not decoded: see lazy::get().
template <class M>
expected<M> decode(const string_view bytes,
                   const std::int32_t depth = 0) noexcept {
  return parse(type_tag<M>{}, bytes, depth);
}

}  // namespace wolaz

#endif  // WOLAZ_HPP
