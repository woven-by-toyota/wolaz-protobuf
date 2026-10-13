// Protoc plugin for Wolaz protobuf.
//
// This plugin is written in terms of the Google stock C++ protoc plugin, but
// the code it generates is written in terms of the runtime/wolaz library.
//
// For each input `foo.proto` it writes `foo.wolaz.hpp` containing, for every
// message, a `decode::M` struct: a constant-size aggregate of lazily-borrowed
// field views, and a `parse()` overload to fill it from a buffer. Wolaz only
// decodes; nothing is generated for encoding.
//
// The generated code is written to the AUTOSAR C++14 guidelines.
//
// Nested and recursive message types are represented by `wolaz::lazy<M>`,
// which holds only the submessage's bytes, so no generated struct ever
// contains itself and no indirection has to be allocated.

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "google/protobuf/compiler/code_generator.h"
#include "google/protobuf/compiler/plugin.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/io/printer.h"
#include "google/protobuf/io/zero_copy_stream.h"

namespace {

using google::protobuf::Descriptor;
using google::protobuf::EnumDescriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::FileDescriptor;
using google::protobuf::OneofDescriptor;
using google::protobuf::compiler::CodeGenerator;
using google::protobuf::compiler::GeneratorContext;
using google::protobuf::io::Printer;

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

// C++ keywords, which are legal identifiers in .proto.
//
// Descriptor accessors return absl::string_view in protobuf >= 22, so the
// naming helpers take string_view and materialise std::string only where one
// is actually needed.
bool IsCppKeyword(absl::string_view s) {
  static const std::set<std::string>* kKeywords = new std::set<std::string>{
      "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
      "bitor", "bool", "break", "case", "catch", "char", "char16_t",
      "char32_t", "char8_t", "class", "co_await", "co_return", "co_yield",
      "compl", "concept", "const", "const_cast", "consteval", "constexpr",
      "constinit", "continue", "decltype", "default", "delete", "do",
      "double", "dynamic_cast", "else", "enum", "explicit", "export",
      "extern", "false", "float", "for", "friend", "goto", "if", "inline",
      "int", "long", "mutable", "namespace", "new", "noexcept", "not",
      "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
      "public", "register", "reinterpret_cast", "requires", "return",
      "short", "signed", "sizeof", "static", "static_assert", "static_cast",
      "struct", "switch", "template", "this", "thread_local", "throw",
      "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
      "using", "virtual", "void", "volatile", "wchar_t", "while", "xor",
      "xor_eq",
  };
  return kKeywords->count(std::string(s)) != 0;
}

// A valid, unreserved C++ identifier for the proto identifier `name`.
//
// C++ reserves identifiers that begin with an underscore or contain two in a
// row (AUTOSAR A17-0-1), and proto allows both, so leading underscores are
// dropped and runs of underscores collapsed to one. A keyword gets a trailing
// underscore.
std::string CppIdentifier(absl::string_view name) {
  std::string out;
  for (const char c : name) {
    if (c != '_' || (!out.empty() && out.back() != '_')) out += c;
  }
  if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) {
    out.insert(0, "x");
  }
  if (IsCppKeyword(out)) out += "_";
  return out;
}

// The names already taken in one C++ scope.
class Scope {
 public:
  // Takes `name`, or if it is taken, `name` with the first free numeric
  // suffix.
  std::string Claim(const std::string& name) {
    std::string candidate = name;
    for (int i = 1; used_.count(candidate) != 0; ++i) {
      candidate = absl::StrCat(name, i);
    }
    used_.insert(candidate);
    return candidate;
  }

  // Assigns C++ names to a list of distinct proto names, in order. A proto
  // name that is already a usable C++ identifier keeps it; only names that
  // had to change can pick up a suffix.
  std::vector<std::string> ClaimAll(const std::vector<std::string>& names) {
    std::vector<std::string> out(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
      if (CppIdentifier(names[i]) == names[i]) out[i] = Claim(names[i]);
    }
    for (size_t i = 0; i < names.size(); ++i) {
      if (out[i].empty()) out[i] = Claim(CppIdentifier(names[i]));
    }
    return out;
  }

 private:
  std::set<std::string> used_;
};

// The C++ names of a message's fields and oneofs, which share the scope of
// the generated struct. Each oneof has an enum type named after it and a
// `<oneof>_case` member.
struct MessageNames {
  std::map<const FieldDescriptor*, std::string> field;
  std::map<const OneofDescriptor*, std::string> oneof;
  std::map<const OneofDescriptor*, std::string> oneof_case;
  // The enumerator of each oneof's enum that means "no member set".
  std::map<const OneofDescriptor*, std::string> not_set;
};

MessageNames NamesOf(const Descriptor* d) {
  std::vector<std::string> proto;
  for (int i = 0; i < d->field_count(); ++i) {
    proto.emplace_back(d->field(i)->name());
  }
  for (int i = 0; i < d->real_oneof_decl_count(); ++i) {
    proto.emplace_back(d->oneof_decl(i)->name());
  }
  Scope scope;
  const std::vector<std::string> cpp = scope.ClaimAll(proto);

  MessageNames names;
  for (int i = 0; i < d->field_count(); ++i) {
    names.field[d->field(i)] = cpp[static_cast<size_t>(i)];
  }
  for (int i = 0; i < d->real_oneof_decl_count(); ++i) {
    const OneofDescriptor* oneof = d->oneof_decl(i);
    const std::string& name = cpp[static_cast<size_t>(d->field_count() + i)];
    names.oneof[oneof] = name;
    names.oneof_case[oneof] = scope.Claim(absl::StrCat(name, "_case"));

    // The oneof's enumerators: its members, and NOT_SET unless a member has
    // that name already.
    Scope enumerators;
    for (int j = 0; j < oneof->field_count(); ++j) {
      enumerators.Claim(names.field[oneof->field(j)]);
    }
    names.not_set[oneof] = enumerators.Claim("NOT_SET");
  }
  return names;
}

// The C++ names of an enum's values, in declaration order.
std::vector<std::string> ValueNamesOf(const EnumDescriptor* e) {
  std::vector<std::string> proto;
  for (int i = 0; i < e->value_count(); ++i) {
    proto.emplace_back(e->value(i)->name());
  }
  return Scope().ClaimAll(proto);
}

// Nested types are flattened into the enclosing namespace with `_` joins, so
// that `Outer.Inner` becomes `Outer_Inner`. Flattening (rather than real C++
// nesting) keeps every generated struct forward-declarable, which recursive
// definitions require.
std::string FlatName(const Descriptor* d) {
  std::string name(d->name());
  for (const Descriptor* p = d->containing_type(); p != nullptr;
       p = p->containing_type()) {
    name = absl::StrCat(p->name(), "_", name);
  }
  return name;
}

std::string FlatName(const EnumDescriptor* d) {
  std::string name(d->name());
  for (const Descriptor* p = d->containing_type(); p != nullptr;
       p = p->containing_type()) {
    name = absl::StrCat(p->name(), "_", name);
  }
  return name;
}

// The C++ namespace corresponding to a proto package.
std::string NamespaceOf(const FileDescriptor* f) {
  if (f->package().empty()) return "";
  return absl::StrReplaceAll(f->package(), {{".", "::"}});
}

std::vector<std::string> NamespaceParts(const FileDescriptor* f) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : f->package()) {
    if (c == '.') {
      if (!cur.empty()) parts.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  return parts;
}

// Fully qualified name of a generated type, as referenced from any scope.
std::string QualifiedDecode(const Descriptor* d) {
  const std::string ns = NamespaceOf(d->file());
  return ns.empty() ? absl::StrCat("::decode::", FlatName(d))
                    : absl::StrCat("::", ns, "::decode::", FlatName(d));
}
std::string QualifiedEnum(const EnumDescriptor* d) {
  const std::string ns = NamespaceOf(d->file());
  return ns.empty() ? absl::StrCat("::decode::", FlatName(d))
                    : absl::StrCat("::", ns, "::decode::", FlatName(d));
}

std::string IncludeGuard(const FileDescriptor* f) {
  std::string g = absl::StrCat("WOLAZ_GENERATED_", f->name());
  for (char& c : g) {
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return g;
}

std::string HeaderName(const FileDescriptor* f) {
  std::string base(f->name());
  const std::string suffix = ".proto";
  if (base.size() > suffix.size() &&
      base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
    base = base.substr(0, base.size() - suffix.size());
  }
  return absl::StrCat(base, ".wolaz.hpp");
}

// ---------------------------------------------------------------------------
// Type mapping
// ---------------------------------------------------------------------------

// The wolaz codec that decodes this field's type.
std::string CodecOf(const FieldDescriptor* f) {
  switch (f->type()) {
    case FieldDescriptor::TYPE_DOUBLE:   return "::wolaz::codec::double_";
    case FieldDescriptor::TYPE_FLOAT:    return "::wolaz::codec::float_";
    case FieldDescriptor::TYPE_INT64:    return "::wolaz::codec::int64_";
    case FieldDescriptor::TYPE_UINT64:   return "::wolaz::codec::uint64_";
    case FieldDescriptor::TYPE_INT32:    return "::wolaz::codec::int32_";
    case FieldDescriptor::TYPE_FIXED64:  return "::wolaz::codec::fixed64_";
    case FieldDescriptor::TYPE_FIXED32:  return "::wolaz::codec::fixed32_";
    case FieldDescriptor::TYPE_BOOL:     return "::wolaz::codec::bool_";
    case FieldDescriptor::TYPE_STRING:   return "::wolaz::codec::string_";
    case FieldDescriptor::TYPE_BYTES:    return "::wolaz::codec::bytes_";
    case FieldDescriptor::TYPE_UINT32:   return "::wolaz::codec::uint32_";
    case FieldDescriptor::TYPE_SFIXED32: return "::wolaz::codec::sfixed32_";
    case FieldDescriptor::TYPE_SFIXED64: return "::wolaz::codec::sfixed64_";
    case FieldDescriptor::TYPE_SINT32:   return "::wolaz::codec::sint32_";
    case FieldDescriptor::TYPE_SINT64:   return "::wolaz::codec::sint64_";
    case FieldDescriptor::TYPE_ENUM:
      return absl::StrCat("::wolaz::codec::enum_<", QualifiedEnum(f->enum_type()),
                          ">");
    case FieldDescriptor::TYPE_MESSAGE:
      return absl::StrCat("::wolaz::codec::message_<",
                          QualifiedDecode(f->message_type()), ">");
    case FieldDescriptor::TYPE_GROUP:
      return absl::StrCat("::wolaz::codec::group_<",
                          QualifiedDecode(f->message_type()), ">");
  }
  return "";
}

// The C++ type a singular field of this type decodes to.
std::string DecodeValueType(const FieldDescriptor* f) {
  switch (f->type()) {
    case FieldDescriptor::TYPE_DOUBLE:   return "double";
    case FieldDescriptor::TYPE_FLOAT:    return "float";
    case FieldDescriptor::TYPE_INT64:
    case FieldDescriptor::TYPE_SFIXED64:
    case FieldDescriptor::TYPE_SINT64:   return "std::int64_t";
    case FieldDescriptor::TYPE_UINT64:
    case FieldDescriptor::TYPE_FIXED64:  return "std::uint64_t";
    case FieldDescriptor::TYPE_INT32:
    case FieldDescriptor::TYPE_SFIXED32:
    case FieldDescriptor::TYPE_SINT32:   return "std::int32_t";
    case FieldDescriptor::TYPE_UINT32:
    case FieldDescriptor::TYPE_FIXED32:  return "std::uint32_t";
    case FieldDescriptor::TYPE_BOOL:     return "bool";
    case FieldDescriptor::TYPE_STRING:
    case FieldDescriptor::TYPE_BYTES:    return "::wolaz::string_view";
    case FieldDescriptor::TYPE_ENUM:     return QualifiedEnum(f->enum_type());
    case FieldDescriptor::TYPE_MESSAGE:
    case FieldDescriptor::TYPE_GROUP:
      return absl::StrCat("::wolaz::lazy<", QualifiedDecode(f->message_type()),
                          ">");
  }
  return "";
}

bool IsMessageLike(const FieldDescriptor* f) {
  return f->type() == FieldDescriptor::TYPE_MESSAGE ||
         f->type() == FieldDescriptor::TYPE_GROUP;
}

// The wire type a singular field of this type uses, as a wolaz enumerator.
std::string WireTypeOf(const FieldDescriptor* f) {
  switch (f->type()) {
    case FieldDescriptor::TYPE_DOUBLE:
    case FieldDescriptor::TYPE_FIXED64:
    case FieldDescriptor::TYPE_SFIXED64:
      return "::wolaz::wire_type::i64";
    case FieldDescriptor::TYPE_FLOAT:
    case FieldDescriptor::TYPE_FIXED32:
    case FieldDescriptor::TYPE_SFIXED32:
      return "::wolaz::wire_type::i32";
    case FieldDescriptor::TYPE_STRING:
    case FieldDescriptor::TYPE_BYTES:
    case FieldDescriptor::TYPE_MESSAGE:
      return "::wolaz::wire_type::len";
    case FieldDescriptor::TYPE_GROUP:
      return "::wolaz::wire_type::sgroup";
    default:
      return "::wolaz::wire_type::varint";
  }
}

bool IsPackable(const FieldDescriptor* f) {
  switch (f->type()) {
    case FieldDescriptor::TYPE_STRING:
    case FieldDescriptor::TYPE_BYTES:
    case FieldDescriptor::TYPE_MESSAGE:
    case FieldDescriptor::TYPE_GROUP:
      return false;
    default:
      return true;
  }
}

// ---------------------------------------------------------------------------
// Collecting the types in a file, nested types flattened out
// ---------------------------------------------------------------------------

void CollectMessages(const Descriptor* d, std::vector<const Descriptor*>* out) {
  out->push_back(d);
  for (int i = 0; i < d->nested_type_count(); ++i) {
    CollectMessages(d->nested_type(i), out);
  }
}

void CollectEnums(const Descriptor* d, std::vector<const EnumDescriptor*>* out) {
  for (int i = 0; i < d->enum_type_count(); ++i) out->push_back(d->enum_type(i));
  for (int i = 0; i < d->nested_type_count(); ++i) {
    CollectEnums(d->nested_type(i), out);
  }
}

std::vector<const Descriptor*> AllMessages(const FileDescriptor* f) {
  std::vector<const Descriptor*> out;
  for (int i = 0; i < f->message_type_count(); ++i) {
    CollectMessages(f->message_type(i), &out);
  }
  return out;
}

std::vector<const EnumDescriptor*> AllEnums(const FileDescriptor* f) {
  std::vector<const EnumDescriptor*> out;
  for (int i = 0; i < f->enum_type_count(); ++i) out.push_back(f->enum_type(i));
  for (int i = 0; i < f->message_type_count(); ++i) {
    CollectEnums(f->message_type(i), &out);
  }
  return out;
}

// Fields in field-number order, which is the order we serialise in.
std::vector<const FieldDescriptor*> FieldsInNumberOrder(const Descriptor* d) {
  std::vector<const FieldDescriptor*> fields;
  for (int i = 0; i < d->field_count(); ++i) fields.push_back(d->field(i));
  std::sort(fields.begin(), fields.end(),
            [](const FieldDescriptor* a, const FieldDescriptor* b) {
              return a->number() < b->number();
            });
  return fields;
}

// The field's declaration as written in .proto, for the generated comments.
std::string ProtoTypeName(const FieldDescriptor* f) {
  switch (f->type()) {
    case FieldDescriptor::TYPE_MESSAGE:
    case FieldDescriptor::TYPE_GROUP:
      return std::string(f->message_type()->full_name());
    case FieldDescriptor::TYPE_ENUM:
      return std::string(f->enum_type()->full_name());
    default:
      return std::string(f->type_name());
  }
}

std::string ProtoDeclaration(const FieldDescriptor* f) {
  std::string type;
  if (f->is_map()) {
    type = absl::StrCat("map<", ProtoTypeName(f->message_type()->map_key()),
                        ", ", ProtoTypeName(f->message_type()->map_value()),
                        ">");
  } else {
    type = absl::StrCat(f->is_repeated() ? "repeated " : "", ProtoTypeName(f));
  }
  return absl::StrCat(type, " ", f->name(), " = ", f->number(), ";");
}

// Names that generated code declares in each `decode` namespace, which a
// message or enum must therefore not have.
bool IsGeneratedNamespaceMember(absl::string_view name) {
  return name == "parse" || name == "field_number";
}

// Whether parse() keeps a field_span for field `f`: where its records are, so
// that its subrange or lazy need not walk the rest of the message later.
bool HasSpan(const FieldDescriptor* f) {
  return f->is_repeated() || IsMessageLike(f);
}

// The name of the local holding field `f`'s field_span in parse(). parse()'s
// other locals and parameters have no `_span` suffix, and field names are
// distinct, so this collides with nothing.
std::string SpanName(const MessageNames& names, const FieldDescriptor* f) {
  return absl::StrCat(names.field.at(f), "_span");
}

// The statements that decode one occurrence of field `f` of message `d`. The
// occurrence's tag is in `tag` and started at `tag_start`, and the cursor `in`
// is just past it.
std::vector<std::string> FieldBody(const Descriptor* d,
                                   const FieldDescriptor* f,
                                   const MessageNames& names) {
  const std::string& fname = names.field.at(f);
  const std::string number =
      absl::StrCat("field_number::", FlatName(d), "::", fname);
  const std::string codec = CodecOf(f);
  const bool group = f->type() == FieldDescriptor::TYPE_GROUP;

  std::vector<std::string> b;
  // Records this occurrence, holding `elements` elements, in the field's span.
  const auto add_to_span = [&b, &names, f](absl::string_view elements) {
    b.push_back(absl::StrCat("  ", SpanName(names, f),
                             ".add(tag_start, in.position(), ", elements,
                             ");"));
  };
  // `if (failed) { return ::wolaz::err::<error>; }`, indented one level.
  const auto fail_if = [&b](absl::string_view failed, absl::string_view error) {
    b.push_back(absl::StrCat("  if (", failed, ") {"));
    b.push_back(absl::StrCat("    return ::wolaz::err::", error, ";"));
    b.push_back("  }");
  };
  const auto read_len_delim = [&b, &fail_if](absl::string_view var) {
    b.push_back(absl::StrCat("  const ::wolaz::optional<::wolaz::string_view> ",
                             var, "{in.read_len_delim()};"));
    fail_if(absl::StrCat("!", var, ".has_value()"), "bad_length");
  };

  if (f->is_repeated()) {
    // Validate and count, but do not store: the subrange reads these again
    // lazily, from the span recorded here.
    if (IsPackable(f)) {
      b.push_back("if (tag.wire == ::wolaz::wire_type::len) {");
      read_len_delim("run");
      b.push_back(absl::StrCat(
          "  const ::wolaz::optional<std::size_t> count{::wolaz::count_packed<",
          codec, ">(*run)};"));
      fail_if("!count.has_value()", "truncated");
      add_to_span("*count");
      b.push_back(
          absl::StrCat("} else if (tag.wire == ", WireTypeOf(f), ") {"));
      fail_if(absl::StrCat("!", codec, "::read(in).has_value()"), "truncated");
      add_to_span("1U");
    } else {
      b.push_back(absl::StrCat("if (tag.wire == ", WireTypeOf(f), ") {"));
      if (group) {
        fail_if("!in.read_group(tag.field, depth).has_value()",
                "unbalanced_group");
      } else if (f->type() == FieldDescriptor::TYPE_STRING) {
        read_len_delim("value");
        fail_if("!::wolaz::is_valid_utf8(*value)", "invalid_utf8");
      } else {
        fail_if("!in.read_len_delim().has_value()", "bad_length");
      }
      add_to_span("1U");
    }
  } else {
    b.push_back(absl::StrCat("if (tag.wire == ", WireTypeOf(f), ") {"));
    if (IsMessageLike(f)) {
      if (group) {
        b.push_back("  const ::wolaz::optional<::wolaz::string_view> body{"
                    "in.read_group(tag.field, depth)};");
        fail_if("!body.has_value()", "unbalanced_group");
      } else {
        read_len_delim("body");
      }
      // The final occurrence is what get() hands back. A singular message
      // field may legally appear more than once, in which case the
      // occurrences merge; if this one does, parse() gives the lazy the span
      // holding all of them after the loop.
      b.push_back(absl::StrCat("  msg.", fname, " = ", DecodeValueType(f),
                               "{*body, depth + 1};"));
      add_to_span("1U");
    } else {
      if (f->type() == FieldDescriptor::TYPE_STRING) {
        read_len_delim("value");
        fail_if("!::wolaz::is_valid_utf8(*value)", "invalid_utf8");
      } else if (f->type() == FieldDescriptor::TYPE_BYTES) {
        read_len_delim("value");
      } else {
        b.push_back(absl::StrCat("  const ::wolaz::optional<",
                                 DecodeValueType(f), "> value{", codec,
                                 "::read(in)};"));
        fail_if("!value.has_value()", "truncated");
      }
      b.push_back(absl::StrCat("  msg.", fname, " = value;"));
    }
    if (const OneofDescriptor* oneof = f->real_containing_oneof()) {
      b.push_back(absl::StrCat("  msg.", names.oneof_case.at(oneof), " = ",
                               FlatName(d), "::", names.oneof.at(oneof),
                               "::", fname, ";"));
    }
  }
  b.push_back("} else if (!in.skip_field(tag, depth)) {");
  b.push_back("  return ::wolaz::err::bad_wire_type;");
  b.push_back("} else {");
  b.push_back(
      "  // A wire type this field cannot have: skipped as undeclared.");
  b.push_back("}");
  return b;
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

class WolazGenerator : public CodeGenerator {
 public:
  bool Generate(const FileDescriptor* file, const std::string& /*parameter*/,
                GeneratorContext* context, std::string* error) const override {
    // Services describe RPC, not wire encoding; nothing is emitted for them.
    for (const Descriptor* d : AllMessages(file)) {
      if (IsGeneratedNamespaceMember(FlatName(d))) {
        *error = absl::StrCat("message ", d->full_name(),
                              ": wolaz generates a member of the same name");
        return false;
      }
    }
    for (const EnumDescriptor* e : AllEnums(file)) {
      if (IsGeneratedNamespaceMember(FlatName(e))) {
        *error = absl::StrCat("enum ", e->full_name(),
                              ": wolaz generates a member of the same name");
        return false;
      }
    }

    std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> out(
        context->Open(HeaderName(file)));
    Printer p(out.get(), '$');
    WriteHeader(file, &p);
    if (p.failed()) {
      *error = "failed writing generated header";
      return false;
    }
    return true;
  }

  uint64_t GetSupportedFeatures() const override {
    return FEATURE_PROTO3_OPTIONAL | FEATURE_SUPPORTS_EDITIONS;
  }
  google::protobuf::Edition GetMinimumEdition() const override {
    return google::protobuf::Edition::EDITION_PROTO2;
  }
  google::protobuf::Edition GetMaximumEdition() const override {
    return google::protobuf::Edition::EDITION_2023;
  }

 private:
  void WriteHeader(const FileDescriptor* file, Printer* p) const;
  void WriteEnum(const EnumDescriptor* e, Printer* p) const;
  void WriteDecodeStruct(const Descriptor* d, Printer* p) const;
  void WriteFieldNumbers(const Descriptor* d, Printer* p) const;
  void WriteDecodeParse(const Descriptor* d, Printer* p) const;
};

void WolazGenerator::WriteHeader(const FileDescriptor* file, Printer* p) const {
  const std::string guard = IncludeGuard(file);

  p->Print(
      "// Generated by protoc-gen-wolaz. DO NOT EDIT.\n"
      "// source: $file$\n"
      "\n"
      "#ifndef $guard$\n"
      "#define $guard$\n"
      "\n"
      "// C++14.\n"
      "\n"
      "#include <wolaz.hpp>\n"
      "\n"
      "#include <cstddef>\n"
      "#include <cstdint>\n"
      "\n",
      "file", file->name(), "guard", guard);

  for (int i = 0; i < file->dependency_count(); ++i) {
    p->Print("#include \"$dep$\"\n", "dep", HeaderName(file->dependency(i)));
  }
  if (file->dependency_count() > 0) p->Print("\n");

  const std::vector<std::string> ns = NamespaceParts(file);
  for (const std::string& part : ns) {
    p->Print("namespace $ns$ {\n", "ns", part);
  }
  if (!ns.empty()) p->Print("\n");

  // Everything wolaz generates lives in a nested `decode` namespace, enums
  // included. Stock protobuf generates into the package namespace itself, so
  // the two never define the same name and both can be included together.
  //
  // Enums come first: decode structs refer to them by value.
  const std::vector<const EnumDescriptor*> enums = AllEnums(file);
  if (!enums.empty()) {
    p->Print("namespace decode {\n\n");
    for (const EnumDescriptor* e : enums) WriteEnum(e, p);
    p->Print("}  // namespace decode\n\n");
  }

  const std::vector<const Descriptor*> messages = AllMessages(file);

  // Forward declarations, so that mutually recursive messages can name each
  // other regardless of declaration order. Map entries are included: they are
  // synthetic in the schema but real on the wire, and a map field refers to
  // its entry type from inside the enclosing message.
  if (!messages.empty()) {
    p->Print("namespace decode {\n");
    for (const Descriptor* d : messages) {
      p->Print("struct $name$;\n", "name", FlatName(d));
    }
    p->Print("}  // namespace decode\n\n");
  }

  p->Print("namespace decode {\n\n");
  for (const Descriptor* d : messages) WriteDecodeStruct(d, p);
  p->Print("}  // namespace decode\n\n");

  // Every field's number as a named constant, one namespace per message.
  const bool any_fields =
      std::any_of(messages.begin(), messages.end(),
                  [](const Descriptor* d) { return d->field_count() > 0; });
  if (any_fields) {
    p->Print("namespace decode {\nnamespace field_number {\n\n");
    for (const Descriptor* d : messages) WriteFieldNumbers(d, p);
    p->Print("}  // namespace field_number\n}  // namespace decode\n\n");
  }

  p->Print("namespace decode {\n\n");
  for (const Descriptor* d : messages) WriteDecodeParse(d, p);
  p->Print("}  // namespace decode\n\n");

  for (auto it = ns.rbegin(); it != ns.rend(); ++it) {
    p->Print("}  // namespace $ns$\n", "ns", *it);
  }

  p->Print("\n#endif  // $guard$\n", "guard", guard);
}

void WolazGenerator::WriteEnum(const EnumDescriptor* e, Printer* p) const {
  const std::string name = FlatName(e);
  const std::vector<std::string> values = ValueNamesOf(e);
  p->Print("// $full$\nenum class $name$ : std::int32_t {\n", "full",
           e->full_name(), "name", name);
  // Aliases are permitted in proto, so the same number can appear twice; C++
  // enumerators tolerate that, but duplicate *names* cannot occur.
  for (int i = 0; i < e->value_count(); ++i) {
    p->Print("  $vname$ = $num$,\n", "vname", values[static_cast<size_t>(i)],
             "num", std::to_string(e->value(i)->number()));
  }
  p->Print("};\n\n");

  // Closed-enum validity, exposed for callers that want to check it. The codec
  // itself treats all enums as open and returns whatever value is on the wire,
  // where stock protobuf would divert an unknown proto2 value to its
  // unknown-field set.
  std::vector<std::string> distinct;  // one name per distinct number
  std::set<int> seen;
  for (int i = 0; i < e->value_count(); ++i) {
    if (seen.insert(e->value(i)->number()).second) {
      distinct.push_back(values[static_cast<size_t>(i)]);
    }
  }
  p->Print(
      "// True if `value` is the number of one of $name$'s values.\n"
      "inline bool $name$_IsValid(const std::int32_t value) noexcept {\n",
      "name", name);
  if (distinct.size() == 1) {
    p->Print("  return value == static_cast<std::int32_t>($name$::$v$);\n",
             "name", name, "v", distinct[0]);
  } else {
    p->Print("  bool valid{false};\n  switch (value) {\n");
    for (const std::string& v : distinct) {
      p->Print("    case static_cast<std::int32_t>($name$::$v$):\n", "name",
               name, "v", v);
    }
    p->Print(
        "      valid = true;\n"
        "      break;\n"
        "    default:\n"
        "      break;\n"
        "  }\n"
        "  return valid;\n");
  }
  p->Print("}\n\n");
}

void WolazGenerator::WriteDecodeStruct(const Descriptor* d, Printer* p) const {
  const std::string name = FlatName(d);
  const MessageNames names = NamesOf(d);
  p->Print("// $full$\nstruct $name$ {\n", "full", d->full_name(), "name", name);

  bool first = true;
  for (const FieldDescriptor* f : FieldsInNumberOrder(d)) {
    if (!first) p->Print("\n");
    first = false;
    const std::string& fname = names.field.at(f);
    p->Print("  // $decl$\n", "decl", ProtoDeclaration(f));
    if (f->is_repeated()) {
      p->Print("  ::wolaz::subrange<$codec$> $fname${};\n", "codec", CodecOf(f),
               "fname", fname);
    } else if (IsMessageLike(f)) {
      // Already an optional-shaped handle; no extra optional needed.
      p->Print("  $type$ $fname${};\n", "type", DecodeValueType(f), "fname",
               fname);
    } else {
      p->Print("  ::wolaz::optional<$type$> $fname${};\n", "type",
               DecodeValueType(f), "fname", fname);
    }
  }

  // One discriminator per oneof. Protobuf resolves a oneof by wire order: the
  // last member on the wire wins, and that is not necessarily the one with the
  // highest field number, so the winner has to be recorded during the parse.
  //
  // The discriminator's type is an enum class named after the oneof, with one
  // enumerator per member, valued by field number.
  for (int i = 0; i < d->real_oneof_decl_count(); ++i) {
    const OneofDescriptor* oneof = d->oneof_decl(i);
    const std::string& oname = names.oneof.at(oneof);
    p->Print(
        "\n"
        "  // Which member of oneof `$proto$` was set: the last one on the wire.\n"
        "  enum class $oneof$ : std::uint32_t {\n"
        "    $not_set$ = 0U,\n",
        "proto", oneof->name(), "oneof", oname, "not_set",
        names.not_set.at(oneof));
    for (int j = 0; j < oneof->field_count(); ++j) {
      p->Print("    $fname$ = $num$U,\n", "fname",
               names.field.at(oneof->field(j)), "num",
               std::to_string(oneof->field(j)->number()));
    }
    p->Print(
        "  };\n"
        "  $oneof$ $case${$oneof$::$not_set$};\n",
        "oneof", oname, "case", names.oneof_case.at(oneof), "not_set",
        names.not_set.at(oneof));
  }

  p->Print("};\n\n");
}

void WolazGenerator::WriteFieldNumbers(const Descriptor* d, Printer* p) const {
  if (d->field_count() == 0) return;
  const MessageNames names = NamesOf(d);
  p->Print("namespace $name$ {\n", "name", FlatName(d));
  for (const FieldDescriptor* f : FieldsInNumberOrder(d)) {
    p->Print("constexpr std::uint32_t $fname${$num$U};\n", "fname",
             names.field.at(f), "num", std::to_string(f->number()));
  }
  p->Print("}  // namespace $name$\n\n", "name", FlatName(d));
}

void WolazGenerator::WriteDecodeParse(const Descriptor* d, Printer* p) const {
  const std::string name = FlatName(d);
  const MessageNames names = NamesOf(d);
  const std::vector<const FieldDescriptor*> fields = FieldsInNumberOrder(d);

  // parse
  //
  // A single pass fills the singular fields and validates everything else at
  // this level. Repeated fields are not materialised -- the subrange re-reads
  // them on demand -- but each element is still read and discarded here so
  // that malformed data is reported by parse() rather than surfacing much
  // later in the middle of an iteration. Submessages are only delimited;
  // lazy::get() decodes them, so parse() never calls another parse().
  //
  // While walking, parse() notes each repeated and submessage field's span:
  // where its first record starts and its last record ends, and how many
  // records and elements it has. The subrange or lazy is built from that, so
  // it reads only its own span later. See DEVELOPMENT.md.
  p->Print(
      "// Decodes a $full$ from `bytes`, the body of a message at nesting\n"
      "// depth `depth`. Fields the message does not declare are skipped.\n"
      "// Submessages are only delimited, not decoded: decode each with its\n"
      "// lazy::get(). Called by wolaz::decode() and lazy::get().\n"
      "inline ::wolaz::expected<$name$> parse(\n"
      "    const ::wolaz::type_tag<$name$>, const ::wolaz::string_view bytes,\n"
      "    const std::int32_t depth) noexcept {\n"
      "  if (depth > ::wolaz::max_recursion_depth) {\n"
      "    return ::wolaz::err::recursion_limit;\n"
      "  }\n"
      "  $name$ msg{};\n",
      "full", d->full_name(), "name", name);

  const bool any_span = std::any_of(fields.begin(), fields.end(), HasSpan);
  for (const FieldDescriptor* f : fields) {
    if (HasSpan(f)) {
      p->Print("  ::wolaz::field_span $span${};\n", "span", SpanName(names, f));
    }
  }

  p->Print("  ::wolaz::cursor in{bytes};\n");
  if (any_span) p->Print("  std::size_t tag_start{in.position()};\n");
  p->Print(
      "  ::wolaz::field_tag tag{in.read_tag()};\n"
      "  while (tag.status == ::wolaz::tag_status::ok) {\n");

  const auto print_lines = [p](const std::vector<std::string>& lines,
                               absl::string_view indent) {
    for (const std::string& line : lines) {
      p->PrintRaw(absl::StrCat(indent, line, "\n"));
    }
  };
  const std::vector<std::string> skip = {
      "if (!in.skip_field(tag, depth)) {",
      "  return ::wolaz::err::bad_wire_type;",
      "}",
  };

  if (fields.empty()) {
    print_lines(skip, "    ");
  } else if (fields.size() == 1) {
    // A switch needs at least two cases besides the default (AUTOSAR A6-4-1).
    const FieldDescriptor* f = fields[0];
    p->Print("    if (tag.field == field_number::$name$::$fname$) {\n", "name",
             name, "fname", names.field.at(f));
    print_lines(FieldBody(d, f, names), "      ");
    p->Print(
        "    } else if (!in.skip_field(tag, depth)) {\n"
        "      return ::wolaz::err::bad_wire_type;\n"
        "    } else {\n"
        "      // Skipped an undeclared field.\n"
        "    }\n");
  } else {
    p->Print("    switch (tag.field) {\n");
    for (const FieldDescriptor* f : fields) {
      p->Print("      case field_number::$name$::$fname$: {\n", "name", name,
               "fname", names.field.at(f));
      print_lines(FieldBody(d, f, names), "        ");
      p->Print(
          "        break;\n"
          "      }\n");
    }
    p->Print("      default: {\n");
    print_lines(skip, "        ");
    p->Print(
        "        break;\n"
        "      }\n"
        "    }\n");
  }

  if (any_span) p->Print("    tag_start = in.position();\n");
  p->Print(
      "    tag = in.read_tag();\n"
      "  }\n"
      "  if (tag.status == ::wolaz::tag_status::malformed) {\n"
      "    return ::wolaz::err::bad_tag;\n"
      "  }\n");

  // Each repeated field reads only its span, and knows its size. A singular
  // message field that occurred more than once is given the span holding all
  // its occurrences, for lazy::for_each_occurrence().
  for (const FieldDescriptor* f : fields) {
    const std::string& fname = names.field.at(f);
    if (f->is_repeated()) {
      p->Print(
          "  msg.$fname$ = ::wolaz::subrange<$codec$>{\n"
          "      bytes, field_number::$name$::$fname$, depth, $span$};\n",
          "fname", fname, "codec", CodecOf(f), "name", name, "span",
          SpanName(names, f));
    } else if (IsMessageLike(f)) {
      p->Print(
          "  if ($span$.records() > 1U) {\n"
          "    msg.$fname$ = $type${$span$.of(bytes),\n"
          "        field_number::$name$::$fname$, depth + 1, $group$,\n"
          "        msg.$fname$.bytes()};\n"
          "  }\n",
          "span", SpanName(names, f), "fname", fname, "type",
          DecodeValueType(f), "name", name, "group",
          f->type() == FieldDescriptor::TYPE_GROUP ? "true" : "false");
    }
  }
  p->Print(
      "  return msg;\n"
      "}\n\n");
}

}  // namespace

int main(int argc, char* argv[]) {
  WolazGenerator generator;
  return google::protobuf::compiler::PluginMain(argc, argv, &generator);
}
