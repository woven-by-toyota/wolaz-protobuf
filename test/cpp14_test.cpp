// Builds the runtime and generated code as C++14, and decodes with them.
//
// The other tests use stock protobuf to produce their input, and protobuf and
// abseil require C++17, so they cannot check that wolaz itself stays within
// C++14. This test does not use stock protobuf: its input is written out by
// hand. It is compiled with -std=c++14 -pedantic-errors.
//
// The conformance messages' generated headers are included as well. They are
// the largest generated code in the repository, and cover every field type.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include <wolaz.hpp>

#include "google/protobuf/test_messages_proto2.wolaz.hpp"
#include "google/protobuf/test_messages_proto3.wolaz.hpp"
#include "test/example.wolaz.hpp"
#include "test/road_testwork.wolaz.hpp"

#if __cplusplus != 201402L
#error "this test must be compiled as C++14"
#endif

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                   __LINE__, #cond);                                   \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

namespace wex = wolaz::example::decode;

// The error constants are constant-initialized, so other translation units'
// static initializers can use them.
static_assert(wolaz::err::truncated.size() == 15U, "err is constexpr");

// A decoded message is a fixed-size, trivially copyable aggregate.
static_assert(std::is_trivially_copyable<decode::RoadSection>::value,
              "decoded messages are trivially copyable");
static_assert(std::is_trivially_copyable<wolaz::optional<std::int64_t>>::value,
              "optional of a trivial type is trivially copyable");

void test_road_testwork() {
  // The same bytes road_testwork_test checks stock protobuf's encoder against.
  const char bytes[] =
      "\x0a\x02" "A1"
      "\x12\x08" "\x08\x07" "\x1a\x04" "\x0a\x02" "T1"
      "\x12\x07" "\x08\x08" "\x12\x03" "\x0a\x01\x04";
  auto section = wolaz::decode<decode::RoadSection>(
      wolaz::string_view(bytes, sizeof bytes - 1));
  CHECK(section.has_value());
  if (!section) return;
  CHECK(section->name.has_value() && *section->name == "A1");

  std::vector<std::uint64_t> ids;
  std::vector<std::string> stops;
  std::vector<std::uint64_t> lanes_ahead;
  for (const auto& e : section->elements) {
    auto element = e.get();
    CHECK(element.has_value());
    if (!element) continue;
    ids.push_back(element->id.value_or(0));
    switch (element->element_case) {
      case decode::RoadElement::element::stop: {
        auto stop = element->stop.get();
        CHECK(stop.has_value());
        if (stop) stops.emplace_back(stop->name.value_or(""));
        break;
      }
      case decode::RoadElement::element::lane: {
        auto lane = element->lane.get();
        CHECK(lane.has_value());
        if (lane)
          for (std::uint64_t a : lane->lanes_ahead) lanes_ahead.push_back(a);
        break;
      }
      case decode::RoadElement::element::NOT_SET:
        CHECK(false);
        break;
    }
  }
  CHECK((ids == std::vector<std::uint64_t>{7, 8}));
  CHECK((stops == std::vector<std::string>{"T1"}));
  CHECK((lanes_ahead == std::vector<std::uint64_t>{4}));
}

void test_example() {
  const char bytes[] =
      "\x08\x00"                                // explicit_zero: 0
      "\x1a\x07" "\x08\x2a" "\x12\x03\x01\x02\x03"  // nested {42, [1, 2, 3]}
      "\x28\x02"                                // colour: COLOUR_GREEN
      "\x4a\x05" "\x0a\x01" "a" "\x10\x01"      // counts {"a": 1}
      "\x5a\x02" "ab";                          // choice_string: "ab"
  auto msg = wolaz::decode<wex::Coverage>(
      wolaz::string_view(bytes, sizeof bytes - 1));
  CHECK(msg.has_value());
  if (!msg) return;

  // Explicit presence: a zero that was sent is distinguishable from absence.
  CHECK(msg->explicit_zero.has_value() && *msg->explicit_zero == 0);
  CHECK(!msg->implicit_zero.has_value());

  auto nested = msg->nested.get();
  CHECK(nested.has_value());
  if (nested) {
    CHECK(nested->value.value_or(-1) == 42);
    std::vector<std::int32_t> numbers;
    for (std::int32_t n : nested->numbers) numbers.push_back(n);
    CHECK((numbers == std::vector<std::int32_t>{1, 2, 3}));
  }

  CHECK(msg->colour.has_value() && *msg->colour == wex::Colour::COLOUR_GREEN);

  int entries = 0;
  for (const auto& e : msg->counts) {
    auto entry = e.get();
    CHECK(entry.has_value());
    if (!entry) continue;
    ++entries;
    CHECK(entry->key.value_or("") == "a");
    CHECK(entry->value.value_or(0) == 1);
  }
  CHECK(entries == 1);

  CHECK(msg->choice_case == wex::Coverage::choice::choice_string);
  CHECK(msg->choice_string.has_value() && *msg->choice_string == "ab");

  // Malformed input is reported, with a message.
  auto bad = wolaz::decode<wex::Coverage>(wolaz::string_view("\x1a\x05" "ab", 4));
  CHECK(!bad);
  if (!bad) CHECK(bad.error() == wolaz::err::bad_length);
}

// parse() records where each repeated field's records are, so the subrange
// holds only that span of the message and knows its size.
void test_repeated_field_span() {
  using decode::RoadSection;

  // name, then two (empty) elements: the span is just the elements.
  {
    const char bytes[] = "\x0a\x02" "A1" "\x12\x00" "\x12\x00";
    const wolaz::string_view input(bytes, 8);
    auto f = wolaz::decode<RoadSection>(input);
    CHECK(f.has_value());
    if (f) {
      CHECK(f->elements.bytes() == input.substr(4U, 4U));
      CHECK(f->elements.bytes().begin() == input.begin() + 4);
      CHECK(f->elements.size() == 2U);
      CHECK(!f->elements.empty());
      CHECK(std::distance(f->elements.begin(), f->elements.end()) == 2);
    }
  }
  // element, name, element: interleaved, so the span includes the name,
  // which the iterator skips.
  {
    const char bytes[] = "\x12\x00" "\x0a\x02" "A1" "\x12\x00";
    const wolaz::string_view input(bytes, 8);
    auto f = wolaz::decode<RoadSection>(input);
    CHECK(f.has_value());
    if (f) {
      CHECK(f->elements.bytes() == input);
      CHECK(f->elements.size() == 2U);
      CHECK(std::distance(f->elements.begin(), f->elements.end()) == 2);
      CHECK(f->name.has_value() && *f->name == "A1");
    }
  }
  // No elements: an empty span.
  {
    const char bytes[] = "\x0a\x02" "A1";
    auto f = wolaz::decode<RoadSection>(wolaz::string_view(bytes, 4));
    CHECK(f.has_value());
    if (f) {
      CHECK(f->elements.bytes().empty());
      CHECK(f->elements.size() == 0U);
      CHECK(f->elements.empty());
      CHECK(f->elements.begin() == f->elements.end());
    }
  }
}

void test_repeated_scalar_count() {
  // lanes_ahead (field 1) as packed [1, 2], unpacked 3, then a record with a
  // wire type the field cannot have (I32), an empty packed run, and packed
  // [4]. The I32 record is skipped by parse() and the iterator alike.
  const char bytes[] =
      "\x0a\x02\x01\x02"
      "\x08\x03"
      "\x0d\x00\x00\x00\x00"
      "\x0a\x00"
      "\x0a\x01\x04";
  auto lane = wolaz::decode<decode::RoadLane>(wolaz::string_view(bytes, 16));
  CHECK(lane.has_value());
  if (!lane) return;
  CHECK(lane->lanes_ahead.size() == 4U);
  std::vector<std::uint64_t> got;
  auto it = lane->lanes_ahead.begin();
  for (; it != lane->lanes_ahead.end(); ++it) got.push_back(*it);
  CHECK((got == std::vector<std::uint64_t>{1, 2, 3, 4}));
  CHECK(!it.failed());
  // The count parse() recorded is what iterating finds.
  CHECK(got.size() == lane->lanes_ahead.size());
}

// A singular message field that occurs once needs no walk to find its
// occurrences; one that occurs more than once is given their span.
void test_singular_message_occurrences() {
  using decode::RoadElement;
  using decode::RoadStop;
  const auto names_of = [](const wolaz::lazy<RoadStop>& stop) {
    std::string names;
    stop.for_each_occurrence([&](wolaz::string_view body) {
      auto t = wolaz::decode<RoadStop>(body, stop.depth());
      CHECK(t.has_value());
      if (t && t->name) names += std::string(*t->name);
      return true;
    });
    return names;
  };

  // Once.
  {
    const char bytes[] = "\x1a\x04" "\x0a\x02" "T1";
    auto e = wolaz::decode<RoadElement>(wolaz::string_view(bytes, 6));
    CHECK(e.has_value());
    if (e) {
      CHECK(e->stop.bytes() == wolaz::string_view("\x0a\x02" "T1", 4));
      CHECK(names_of(e->stop) == "T1");
    }
  }
  // Twice, with other records between: an id, and field 3 again with a
  // wire type it cannot have, which both walks skip.
  {
    const char bytes[] =
        "\x1a\x04" "\x0a\x02" "T1"
        "\x08\x07"
        "\x18\x01"
        "\x1a\x04" "\x0a\x02" "T2";
    auto e = wolaz::decode<RoadElement>(wolaz::string_view(bytes, 16));
    CHECK(e.has_value());
    if (e) {
      CHECK(e->id.value_or(0) == 7U);
      // get() is the final occurrence; for_each_occurrence sees both, in
      // wire order.
      auto last = e->stop.get();
      CHECK(last.has_value() && last->name && *last->name == "T2");
      CHECK(names_of(e->stop) == "T1T2");

      // Stopping early.
      int calls = 0;
      const bool finished = e->stop.for_each_occurrence(
          [&](wolaz::string_view) { return ++calls < 1; });
      CHECK(!finished);
      CHECK(calls == 1);
    }
  }
}

// A submessage is validated when it is decoded, not with its parent.
void test_submessage_validated_by_get() {
  // RoadSection { elements { stop { name: <claims 5 bytes, has none> } } }
  const char bytes[] = "\x12\x04" "\x1a\x02" "\x0a\x05";
  auto section =
      wolaz::decode<decode::RoadSection>(wolaz::string_view(bytes, 6));
  CHECK(section.has_value());
  if (!section) return;
  for (const wolaz::lazy<decode::RoadElement>& e : section->elements) {
    auto element = e.get();
    CHECK(element.has_value());
    if (!element) continue;
    auto stop = element->stop.get();
    CHECK(!stop.has_value());
    CHECK(stop.error() == wolaz::err::bad_length);
  }
}

void test_optional() {
  wolaz::optional<int> none;
  CHECK(!none.has_value());
  CHECK(none.value_or(7) == 7);

  wolaz::optional<int> some{3};
  CHECK(some && *some == 3);

  none = some;
  CHECK(none.has_value() && *none == 3);
  none = wolaz::nullopt;
  CHECK(!none);

  // A type with a non-trivial destructor and copy is managed correctly.
  wolaz::optional<std::vector<int>> v;
  v.emplace(3, 1);
  CHECK(v->size() == 3);
  wolaz::optional<std::vector<int>> copy = v;
  v.reset();
  CHECK(!v && copy && copy->size() == 3);
}

void test_string_view() {
  const wolaz::string_view a("abc");
  CHECK(a.size() == 3);
  CHECK(a == "abc");
  CHECK(a != "abd");
  CHECK(wolaz::string_view() == "");
  const std::string s(a);
  CHECK(s == "abc");
  CHECK(wolaz::string_view(s) == a);
}

void test_string_view_iteration() {
  const wolaz::string_view a("abc");
  CHECK(a.length() == 3U);
  CHECK(a.length() == a.size());
  CHECK(a.begin() == a.data());
  CHECK(a.end() - a.begin() == 3);
  CHECK(std::distance(a.begin(), a.end()) == 3);

  // range-for
  std::string copy;
  for (char c : a) copy += c;
  CHECK(copy == "abc");

  // The standard algorithms, and building a std::string from the range.
  CHECK(std::string(a.begin(), a.end()) == "abc");
  CHECK(std::find(a.begin(), a.end(), 'b') == a.begin() + 1);
  CHECK(std::find(a.begin(), a.end(), 'z') == a.end());
  CHECK(std::count(a.begin(), a.end(), 'c') == 1);

  // A view of part of a buffer iterates over just that part.
  const char buffer[] = "xyzabc";
  const wolaz::string_view tail(buffer + 3, 3);
  CHECK(tail.begin() == buffer + 3);
  CHECK(std::string(tail.begin(), tail.end()) == "abc");
  CHECK(tail.substr(1U, 2U).length() == 2U);
  CHECK(*tail.substr(1U, 2U).begin() == 'b');

  // Empty views, default-constructed or not, have begin() == end().
  const wolaz::string_view none;
  CHECK(none.length() == 0U);
  CHECK(none.begin() == none.end());
  int iterations = 0;
  for (char c : none) {
    static_cast<void>(c);
    ++iterations;
  }
  CHECK(iterations == 0);
  const wolaz::string_view empty_literal("");
  CHECK(empty_literal.begin() == empty_literal.end());

  // Usable in constant expressions.
  constexpr wolaz::string_view k("hello");
  static_assert(k.length() == 5U, "length() is constexpr");
  static_assert(k.end() - k.begin() == 5, "begin() and end() are constexpr");
  static_assert(*k.begin() == 'h', "begin() points at the first char");
}

}  // namespace

int main() {
  test_road_testwork();
  test_example();
  test_submessage_validated_by_get();
  test_repeated_field_span();
  test_repeated_scalar_count();
  test_singular_message_occurrences();
  test_optional();
  test_string_view();
  test_string_view_iteration();
  if (failures != 0) {
    std::fprintf(stderr, "\n%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all C++14 tests passed\n");
  return 0;
}
