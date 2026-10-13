// Decode tests for test/road_testwork.proto.
//
// Stock protobuf encodes; the result is checked against hand-assembled bytes,
// then decoded by both wolaz and stock protobuf. wolaz does not encode.

#include <cstdio>
#include <string>
#include <string_view>

#include <wolaz.hpp>

#include "test/road_testwork.pb.h"     // stock protobuf: ::RoadSection, ...
#include "test/road_testwork.wolaz.hpp"  // wolaz: ::decode::RoadSection, ...

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

// The message every test builds or expects:
//
//   RoadSection {
//     name: "A1"
//     elements { id: 7  stop { name: "T1" } }
//     elements { id: 8  lane { lanes_ahead: [4] } }
//   }
//
// Hand-encoded. proto3 packs repeated uint64, so `lanes_ahead` is a single
// length-delimited record. The length comes from sizeof (minus the
// terminating NUL), so there is no byte count to keep in sync by hand.
constexpr char kExpectedBytes[] =
    "\x0a\x02" "A1"                    // 1: name "A1"
    "\x12\x08"                         // 2: elements, 8 bytes
      "\x08\x07"                       //   1: id 7
      "\x1a\x04"                       //   3: stop, 4 bytes
        "\x0a\x02" "T1"                //     1: name "T1"
    "\x12\x07"                         // 2: elements, 7 bytes
      "\x08\x08"                       //   1: id 8
      "\x12\x03"                       //   2: lane, 3 bytes
        "\x0a\x01\x04";                //     1: lanes_ahead packed [4]

const std::string_view kExpected(kExpectedBytes, sizeof(kExpectedBytes) - 1);

std::string Hex(std::string_view s) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    out += digits[c >> 4];
    out += digits[c & 0xf];
  }
  return out;
}

void CheckBytes(std::string_view got) {
  if (got != kExpected) {
    std::fprintf(stderr, "  expected %s\n  got      %s\n",
                 Hex(kExpected).c_str(), Hex(got).c_str());
  }
  CHECK(got == kExpected);
}

// 1. google protobuf encode ------------------------------------------------

std::string ProtobufEncode() {
  RoadSection section;
  section.set_name("A1");

  RoadElement* first = section.add_elements();
  first->set_id(7);
  first->mutable_stop()->set_name("T1");

  RoadElement* second = section.add_elements();
  second->set_id(8);
  second->mutable_lane()->add_lanes_ahead(4);

  std::string bytes;
  CHECK(section.SerializeToString(&bytes));
  return bytes;
}

void test_protobuf_encode(const std::string& bytes) { CheckBytes(bytes); }

// 2. wolaz decode, from the google protobuf encode output -----------------

void test_wolaz_decode(const std::string& bytes) {
  using namespace wolaz;
  using RoadSection = decode::RoadSection;
  using RoadElement = decode::RoadElement;
  using RoadStop = decode::RoadStop;
  using RoadLane = decode::RoadLane;

  expected<RoadSection> const section = wolaz::decode<RoadSection>(std::string_view(bytes));
  // At this point, the RoadSection outer message has been decoded and allocated
  // on the stack, but the inner messages are still lazy handles that need
  // to be explicitly decoded.
  CHECK(section.has_value());
  if (!section) return;

  CHECK(section->name.has_value() && *section->name == "A1");

  CHECK(std::distance(section->elements.begin(), section->elements.end()) == 2);
  if (std::distance(section->elements.begin(), section->elements.end()) != 2) return;

  for (lazy<RoadElement> const e : section->elements) {
    expected<RoadElement> const element = e.get();
    CHECK(element.has_value());
    if (!element) return;
  
    // A RoadElement may be either a RoadStop or a RoadLane.
    switch(element->element_case) {
      case RoadElement::element::stop:
        // First element: a RoadStop named "T1".
        {
          CHECK(element->id.value_or(0) == 7U);
          expected<RoadStop> const stop = element->stop.get();
          CHECK(stop.has_value());
          if (stop) {
            CHECK(stop->name.has_value() && *stop->name == "T1");
          }
        }
        break;
      case RoadElement::element::lane:
        // Second element: a RoadLane with lanes_ahead [4].
        {
          CHECK(element->id.value_or(0) == 8U);
          expected<RoadLane> const lane = element->lane.get();
          CHECK(lane.has_value());
          if (lane) {
            for (uint64_t const lane_ahead : lane->lanes_ahead) {
              CHECK(lane_ahead == 4);
            }
          }
        }
        break;
      default:
        CHECK(false);  // Unexpected element case
    }
  }
}

// 3. google protobuf decode, from the google protobuf encode output --------

void test_protobuf_decode(const std::string& bytes) {
  RoadSection section;
  CHECK(section.ParseFromString(bytes));
  // At this point, the entire RoadSection message has been decoded into the
  // RoadSection object data structure, with all inner messages fully populated
  // by allocation on the free-store heap.

  CHECK(section.name() == "A1");
  CHECK(section.elements_size() == 2);
  if (section.elements_size() != 2) return;

  for (RoadElement const& road_element : section.elements()) {
    switch (road_element.element_case()) {
      case RoadElement::kStop:
        {
          CHECK(road_element.id() == 7U);
          const RoadStop& stop = road_element.stop();
          CHECK(stop.name() == "T1");
        }
        break;
      case RoadElement::kLane:
        {
          CHECK(road_element.id() == 8U);
          const RoadLane& lane = road_element.lane();
          CHECK(lane.lanes_ahead_size() == 1);
          for (const uint64_t ahead : lane.lanes_ahead()) {
            CHECK(ahead == 4U);
          }
        }
        break;
      default:
        CHECK(false);  // Unexpected element case
    }
  }
}

template <class F>
void Run(const char* name, F&& test) {
  const int before = failures;
  test();
  std::printf("[%s] %s\n", failures == before ? "  OK  " : "FAILED", name);
}

}  // namespace

int main() {
  const std::string bytes = ProtobufEncode();
  Run("protobuf encode", [&] { test_protobuf_encode(bytes); });
  Run("wolaz decode", [&] { test_wolaz_decode(bytes); });
  Run("protobuf decode", [&] { test_protobuf_decode(bytes); });

  if (failures != 0) {
    std::fprintf(stderr, "\n%d check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
