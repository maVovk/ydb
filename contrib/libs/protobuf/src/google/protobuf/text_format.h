// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Author: jschorr@google.com (Joseph Schorr)
//  Based on original Protocol Buffers design by
//  Sanjay Ghemawat, Jeff Dean, and others.
//
// Utilities for printing and parsing protocol messages in a human-readable,
// text-based format.

#ifndef GOOGLE_PROTOBUF_TEXT_FORMAT_H__
#define GOOGLE_PROTOBUF_TEXT_FORMAT_H__

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "google/protobuf/port.h"
#include "y_absl/container/flat_hash_map.h"
#include "y_absl/strings/string_view.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "google/protobuf/message_lite.h"
#include "google/protobuf/port.h"


// Must be included last.
#include "google/protobuf/port_def.inc"

#ifdef SWIG
#error "You cannot SWIG proto headers"
#endif

namespace google {
namespace protobuf {

namespace internal {
PROTOBUF_EXPORT extern const char kDebugStringSilentMarker[1];
PROTOBUF_EXPORT extern const char kDebugStringSilentMarkerForDetection[3];

PROTOBUF_EXPORT extern std::atomic<bool> enable_debug_text_redaction_marker;
PROTOBUF_EXPORT extern std::atomic<bool> enable_debug_text_random_marker;
PROTOBUF_EXPORT extern std::atomic<bool> enable_debug_text_format_marker;

}  // namespace internal

namespace io {
class ErrorCollector;  // tokenizer.h
}

// This class implements protocol buffer text format, colloquially known as text
// proto.  Printing and parsing protocol messages in text format is useful for
// debugging and human editing of messages.
//
// This class is really a namespace that contains only static methods.
class PROTOBUF_EXPORT TextFormat {
 public:
  TextFormat(const TextFormat&) = delete;
  TextFormat& operator=(const TextFormat&) = delete;

  // Outputs a textual representation of the given message to the given
  // output stream. Returns false if printing fails.
  static bool Print(const Message& message, io::ZeroCopyOutputStream* output);

  // Print the fields in an UnknownFieldSet.  They are printed by tag number
  // only.  Embedded messages are heuristically identified by attempting to
  // parse them. Returns false if printing fails.
  static bool PrintUnknownFields(const UnknownFieldSet& unknown_fields,
                                 io::ZeroCopyOutputStream* output);

  // Like Print(), but outputs directly to a string.
  // Note: output will be cleared prior to printing, and will be left empty
  // even if printing fails. Returns false if printing fails.
  static bool PrintToString(const Message& message, TProtoStringType* output);

  // Like PrintUnknownFields(), but outputs directly to a string. Returns
  // false if printing fails.
  static bool PrintUnknownFieldsToString(const UnknownFieldSet& unknown_fields,
                                         TProtoStringType* output);

  // Outputs a textual representation of the value of the field supplied on
  // the message supplied. For non-repeated fields, an index of -1 must
  // be supplied. Note that this method will print the default value for a
  // field if it is not set.
  static void PrintFieldValueToString(const Message& message,
                                      const FieldDescriptor* field, int index,
                                      TProtoStringType* output);

  // Forward declare `Printer` for `BaseTextGenerator::MarkerToken` which
  // restricts some methods of `BaseTextGenerator` to the class `Printer`.
  class Printer;

  class PROTOBUF_EXPORT BaseTextGenerator {
   private:
    // Passkey (go/totw/134#what-about-stdshared-ptr) that allows `Printer`
    // (but not derived classes) to call `PrintMaybeWithMarker` and its
    // `Printer::TextGenerator` to overload it.
    // This prevents users from bypassing the marker generation.
    class MarkerToken {
     private:
      explicit MarkerToken() = default;  // 'explicit' prevents aggregate init.
      friend class Printer;
    };

   public:
    virtual ~BaseTextGenerator();

    virtual void Indent() {}
    virtual void Outdent() {}
    // Returns the current indentation size in characters.
    virtual size_t GetCurrentIndentationSize() const { return 0; }

    // Print text to the output stream.
    virtual void Print(const char* text, size_t size) = 0;

    void PrintString(const TProtoStringType& str) { Print(str.data(), str.size()); }

    template <size_t n>
    void PrintLiteral(const char (&text)[n]) {
      Print(text, n - 1);  // n includes the terminating zero character.
    }

    // Internal to Printer, access regulated by `MarkerToken`.
    virtual void PrintMaybeWithMarker(MarkerToken, y_absl::string_view text) {
      Print(text.data(), text.size());
    }

    // Internal to Printer, access regulated by `MarkerToken`.
    virtual void PrintMaybeWithMarker(MarkerToken, y_absl::string_view text_head,
                                      y_absl::string_view text_tail) {
      Print(text_head.data(), text_head.size());
      Print(text_tail.data(), text_tail.size());
    }

    friend class Printer;
  };

  // The default printer that converts scalar values from fields into their
  // string representation.
  // You can derive from this FastFieldValuePrinter if you want to have fields
  // to be printed in a different way and register it at the Printer.
  class PROTOBUF_EXPORT FastFieldValuePrinter {
   public:
    FastFieldValuePrinter();
    FastFieldValuePrinter(const FastFieldValuePrinter&) = delete;
    FastFieldValuePrinter& operator=(const FastFieldValuePrinter&) = delete;
    virtual ~FastFieldValuePrinter();
    virtual void PrintBool(bool val, BaseTextGenerator* generator) const;
    virtual void PrintInt32(arc_i32 val, BaseTextGenerator* generator) const;
    virtual void PrintUInt32(arc_ui32 val, BaseTextGenerator* generator) const;
    virtual void PrintInt64(arc_i64 val, BaseTextGenerator* generator) const;
    virtual void PrintUInt64(arc_ui64 val, BaseTextGenerator* generator) const;
    virtual void PrintFloat(float val, BaseTextGenerator* generator) const;
    virtual void PrintDouble(double val, BaseTextGenerator* generator) const;
    virtual void PrintString(const TProtoStringType& val,
                             BaseTextGenerator* generator) const;
    virtual void PrintBytes(const TProtoStringType& val,
                            BaseTextGenerator* generator) const;
    virtual void PrintEnum(arc_i32 val, const TProtoStringType& name,
                           BaseTextGenerator* generator) const;
    virtual void PrintFieldName(const Message& message, int field_index,
                                int field_count, const Reflection* reflection,
                                const FieldDescriptor* field,
                                BaseTextGenerator* generator) const;
    virtual void PrintFieldName(const Message& message,
                                const Reflection* reflection,
                                const FieldDescriptor* field,
                                BaseTextGenerator* generator) const;
    virtual void PrintMessageStart(const Message& message, int field_index,
                                   int field_count, bool single_line_mode,
                                   BaseTextGenerator* generator) const;
    // Allows to override the logic on how to print the content of a message.
    // Return false to use the default printing logic. Note that it is legal for
    // this function to print something and then return false to use the default
    // content printing (although at that point it would behave similarly to
    // PrintMessageStart).
    virtual bool PrintMessageContent(const Message& message, int field_index,
                                     int field_count, bool single_line_mode,
                                     BaseTextGenerator* generator) const;
    virtual void PrintMessageEnd(const Message& message, int field_index,
                                 int field_count, bool single_line_mode,
                                 BaseTextGenerator* generator) const;
  };

  // Deprecated: please use FastFieldValuePrinter instead.
  class PROTOBUF_EXPORT FieldValuePrinter {
   public:
    FieldValuePrinter();
    FieldValuePrinter(const FieldValuePrinter&) = delete;
    FieldValuePrinter& operator=(const FieldValuePrinter&) = delete;
    virtual ~FieldValuePrinter();
    virtual TProtoStringType PrintBool(bool val) const;
    virtual TProtoStringType PrintInt32(arc_i32 val) const;
    virtual TProtoStringType PrintUInt32(arc_ui32 val) const;
    virtual TProtoStringType PrintInt64(arc_i64 val) const;
    virtual TProtoStringType PrintUInt64(arc_ui64 val) const;
    virtual TProtoStringType PrintFloat(float val) const;
    virtual TProtoStringType PrintDouble(double val) const;
    virtual TProtoStringType PrintString(const TProtoStringType& val) const;
    virtual TProtoStringType PrintBytes(const TProtoStringType& val) const;
    virtual TProtoStringType PrintEnum(arc_i32 val, const TProtoStringType& name) const;
    virtual TProtoStringType PrintFieldName(const Message& message,
                                       const Reflection* reflection,
                                       const FieldDescriptor* field) const;
    virtual TProtoStringType PrintMessageStart(const Message& message,
                                          int field_index, int field_count,
                                          bool single_line_mode) const;
    virtual TProtoStringType PrintMessageEnd(const Message& message, int field_index,
                                        int field_count,
                                        bool single_line_mode) const;

   private:
    FastFieldValuePrinter delegate_;
  };

  class PROTOBUF_EXPORT MessagePrinter {
   public:
    MessagePrinter() {}
    MessagePrinter(const MessagePrinter&) = delete;
    MessagePrinter& operator=(const MessagePrinter&) = delete;
    virtual ~MessagePrinter() {}
    virtual void Print(const Message& message, bool single_line_mode,
                       BaseTextGenerator* generator) const = 0;
  };

  // Interface that Printers or Parsers can use to find extensions, or types
  // referenced in Any messages.
  class PROTOBUF_EXPORT Finder {
   public:
    virtual ~Finder();

    // Try to find an extension of *message by fully-qualified field
    // name.  Returns nullptr if no extension is known for this name or number.
    // The base implementation uses the extensions already known by the message.
    virtual const FieldDescriptor* FindExtension(Message* message,
                                                 const TProtoStringType& name) const;

    // Similar to FindExtension, but uses a Descriptor and the extension number
    // instead of using a Message and the name when doing the look up.
    virtual const FieldDescriptor* FindExtensionByNumber(
        const Descriptor* descriptor, int number) const;

    // Find the message type for an Any proto.
    // Returns nullptr if no message is known for this name.
    // The base implementation only accepts prefixes of type.googleprod.com/ or
    // type.googleapis.com/, and searches the DescriptorPool of the parent
    // message.
    virtual const Descriptor* FindAnyType(const Message& message,
                                          const TProtoStringType& prefix,
                                          const TProtoStringType& name) const;

    // Find the message factory for the given extension field. This can be used
    // to generalize the Parser to add extension fields to a message in the same
    // way as the "input" message for the Parser.
    virtual MessageFactory* FindExtensionFactory(
        const FieldDescriptor* field) const;
  };

  // Class for those users which require more fine-grained control over how
  // a protobuffer message is printed out.
  class PROTOBUF_EXPORT Printer {
   public:
    Printer();

    // Like TextFormat::Print
    bool Print(const Message& message, io::ZeroCopyOutputStream* output) const;
    // Like TextFormat::PrintUnknownFields
    bool PrintUnknownFields(const UnknownFieldSet& unknown_fields,
                            io::ZeroCopyOutputStream* output) const;
    // Like TextFormat::PrintToString
    bool PrintToString(const Message& message, TProtoStringType* output) const;
    // Like TextFormat::PrintUnknownFieldsToString
    bool PrintUnknownFieldsToString(const UnknownFieldSet& unknown_fields,
                                    TProtoStringType* output) const;
    // Like TextFormat::PrintFieldValueToString
    void PrintFieldValueToString(const Message& message,
                                 const FieldDescriptor* field, int index,
                                 TProtoStringType* output) const;

    // Adjust the initial indent level of all output.  Each indent level is
    // equal to two spaces.
    void SetInitialIndentLevel(int indent_level) {
      initial_indent_level_ = indent_level;
    }

    // If printing in single line mode, then the entire message will be output
    // on a single line with no line breaks.
    void SetSingleLineMode(bool single_line_mode) {
      single_line_mode_ = single_line_mode;
    }

    bool IsInSingleLineMode() const { return single_line_mode_; }

    // If use_field_number is true, uses field number instead of field name.
    void SetUseFieldNumber(bool use_field_number) {
      use_field_number_ = use_field_number;
    }

    // Set true to print repeated primitives in a format like:
    //   field_name: [1, 2, 3, 4]
    // instead of printing each value on its own line.  Short format applies
    // only to primitive values -- i.e. everything except strings and
    // sub-messages/groups.
    void SetUseShortRepeatedPrimitives(bool use_short_repeated_primitives) {
      use_short_repeated_primitives_ = use_short_repeated_primitives;
    }

    // Set true to output UTF-8 instead of ASCII.  The only difference
    // is that bytes >= 0x80 in string fields will not be escaped,
    // because they are assumed to be part of UTF-8 multi-byte
    // sequences. This will change the default FastFieldValuePrinter.
    void SetUseUtf8StringEscaping(bool as_utf8);

    // Set the default FastFieldValuePrinter that is used for all fields that
    // don't have a field-specific printer registered.
    // Takes ownership of the printer.
    void SetDefaultFieldValuePrinter(const FastFieldValuePrinter* printer);

    PROTOBUF_DEPRECATED_MSG("Please use FastFieldValuePrinter")
    void SetDefaultFieldValuePrinter(const FieldValuePrinter* printer);

    // Sets whether we want to hide unknown fields or not.
    // Usually unknown fields are printed in a generic way that includes the
    // tag number of the field instead of field name. However, sometimes it
    // is useful to be able to print the message without unknown fields (e.g.
    // for the python protobuf version to maintain consistency between its pure
    // python and c++ implementations).
    void SetHideUnknownFields(bool hide) { hide_unknown_fields_ = hide; }

    // If print_message_fields_in_index_order is true, fields of a proto message
    // will be printed using the order defined in source code instead of the
    // field number, extensions will be printed at the end of the message
    // and their relative order is determined by the extension number.
    // By default, use the field number order.
    void SetPrintMessageFieldsInIndexOrder(
        bool print_message_fields_in_index_order) {
      print_message_fields_in_index_order_ =
          print_message_fields_in_index_order;
    }

    // If expand==true, expand google.protobuf.Any payloads. The output
    // will be of form
    //    [type_url] { <value_printed_in_text> }
    //
    // If expand==false, print Any using the default printer. The output will
    // look like
    //    type_url: "<type_url>"  value: "serialized_content"
    void SetExpandAny(bool expand) { expand_any_ = expand; }

    // Set how parser finds message for Any payloads.
    void SetFinder(const Finder* finder) { finder_ = finder; }

    // If non-zero, we truncate all string fields that are  longer than
    // this threshold.  This is useful when the proto message has very long
    // strings, e.g., dump of encoded image file.
    //
    // NOTE(hfgong):  Setting a non-zero value breaks round-trip safe
    // property of TextFormat::Printer.  That is, from the printed message, we
    // cannot fully recover the original string field any more.
    void SetTruncateStringFieldLongerThan(
        const arc_i64 truncate_string_field_longer_than) {
      truncate_string_field_longer_than_ = truncate_string_field_longer_than;
    }

    // Sets whether strings will be redacted and thus unparsable.
    void SetRedactDebugString(bool redact) { redact_debug_string_ = redact; }

    // Register a custom field-specific FastFieldValuePrinter for fields
    // with a particular FieldDescriptor.
    // Returns "true" if the registration succeeded, or "false", if there is
    // already a printer for that FieldDescriptor.
    // Takes ownership of the printer on successful registration.
    bool RegisterFieldValuePrinter(const FieldDescriptor* field,
                                   const FastFieldValuePrinter* printer);

    PROTOBUF_DEPRECATED_MSG("Please use FastFieldValuePrinter")
    bool RegisterFieldValuePrinter(const FieldDescriptor* field,
                                   const FieldValuePrinter* printer);

    // Register a custom message-specific MessagePrinter for messages with a
    // particular Descriptor.
    // Returns "true" if the registration succeeded, or "false" if there is
    // already a printer for that Descriptor.
    bool RegisterMessagePrinter(const Descriptor* descriptor,
                                const MessagePrinter* printer);

    // Default printing for messages, which allows registered message printers
    // to fall back to default printing without losing the ability to control
    // sub-messages or fields.
    // NOTE: If the passed in `text_generaor` is not actually the current
    // `TextGenerator`, then no output will be produced.
    void PrintMessage(const Message& message,
                      BaseTextGenerator* generator) const;

   private:
    friend TProtoStringType Message::DebugString() const;
    friend TProtoStringType Message::ShortDebugString() const;
    friend TProtoStringType Message::Utf8DebugString() const;
    friend void internal::PerformAbslStringify(
        const Message& message,
        y_absl::FunctionRef<void(y_absl::string_view)> append);

    // Sets whether silent markers will be inserted.
    void SetInsertSilentMarker(bool v) { insert_silent_marker_ = v; }

    // Sets whether the output string should be made non-deterministic.
    // This discourages equality checks based on serialized string comparisons.
    void SetRandomizeDebugString(bool randomize) {
      randomize_debug_string_ = randomize;
    }

    // Forward declaration of an internal class used to print the text
    // output to the OutputStream (see text_format.cc for implementation).
    class TextGenerator;
    using MarkerToken = BaseTextGenerator::MarkerToken;

    // Forward declaration of an internal class used to print field values for
    // DebugString APIs (see text_format.cc for implementation).
    class DebugStringFieldValuePrinter;

    // Forward declaration of an internal class used to print UTF-8 escaped
    // strings (see text_format.cc for implementation).
    class FastFieldValuePrinterUtf8Escaping;

    // Internal Print method, used for writing to the OutputStream via
    // the TextGenerator class.
    void Print(const Message& message, BaseTextGenerator* generator) const;

    // Print a single field.
    void PrintField(const Message& message, const Reflection* reflection,
                    const FieldDescriptor* field,
                    BaseTextGenerator* generator) const;

    // Print a repeated primitive field in short form.
    void PrintShortRepeatedField(const Message& message,
                                 const Reflection* reflection,
                                 const FieldDescriptor* field,
                                 BaseTextGenerator* generator) const;

    // Print the name of a field -- i.e. everything that comes before the
    // ':' for a single name/value pair.
    void PrintFieldName(const Message& message, int field_index,
                        int field_count, const Reflection* reflection,
                        const FieldDescriptor* field,
                        BaseTextGenerator* generator) const;

    // Outputs a textual representation of the value of the field supplied on
    // the message supplied or the default value if not set.
    void PrintFieldValue(const Message& message, const Reflection* reflection,
                         const FieldDescriptor* field, int index,
                         BaseTextGenerator* generator) const;

    // Print the fields in an UnknownFieldSet.  They are printed by tag number
    // only.  Embedded messages are heuristically identified by attempting to
    // parse them (subject to the recursion budget).
    void PrintUnknownFields(const UnknownFieldSet& unknown_fields,
                            BaseTextGenerator* generator,
                            int recursion_budget) const;

    bool PrintAny(const Message& message, BaseTextGenerator* generator) const;

    // Try to redact a field value based on the annotations associated with
    // the field. This function returns true if it redacts the field value.
    bool TryRedactFieldValue(const Message& message,
                             const FieldDescriptor* field,
                             BaseTextGenerator* generator,
                             bool insert_value_separator) const;

    const FastFieldValuePrinter* GetFieldPrinter(
        const FieldDescriptor* field) const {
      auto it = custom_printers_.find(field);
      return it == custom_printers_.end() ? default_field_value_printer_.get()
                                          : it->second.get();
    }

    int initial_indent_level_;
    bool single_line_mode_;
    bool use_field_number_;
    bool use_short_repeated_primitives_;
    bool insert_silent_marker_;
    bool redact_debug_string_;
    bool randomize_debug_string_;
    bool hide_unknown_fields_;
    bool print_message_fields_in_index_order_;
    bool expand_any_;
    arc_i64 truncate_string_field_longer_than_;

    std::unique_ptr<const FastFieldValuePrinter> default_field_value_printer_;
    y_absl::flat_hash_map<const FieldDescriptor*,
                        std::unique_ptr<const FastFieldValuePrinter>>
        custom_printers_;

    y_absl::flat_hash_map<const Descriptor*,
                        std::unique_ptr<const MessagePrinter>>
        custom_message_printers_;

    const Finder* finder_;
  };

  // Parses a text-format protocol message from the given input stream to
  // the given message object. This function parses the human-readable
  // serialization format written by Print(). Returns true on success. The
  // message is cleared first, even if the function fails -- See Merge() to
  // avoid this behavior.
  //
  // Example input: "user {\n id: 123 extra { gender: MALE language: 'en' }\n}"
  //
  // One common use for this function is parsing handwritten strings in test
  // code.
  //
  // If you would like to read a protocol buffer serialized in the
  // (non-human-readable) binary wire format, see
  // google::protobuf::MessageLite::ParseFromString().
  static bool Parse(io::ZeroCopyInputStream* input, Message* output);
  // Like Parse(), but reads directly from a string.
  static bool ParseFromString(y_absl::string_view input, Message* output);

  // Like Parse(), but the data is merged into the given message, as if
  // using Message::MergeFrom().
  static bool Merge(io::ZeroCopyInputStream* input, Message* output);
  // Like Merge(), but reads directly from a string.
  static bool MergeFromString(y_absl::string_view input, Message* output);

  // Parse the given text as a single field value and store it into the
  // given field of the given message. If the field is a repeated field,
  // the new value will be added to the end
  static bool ParseFieldValueFromString(const TProtoStringType& input,
                                        const FieldDescriptor* field,
                                        Message* message);

  // A location in the parsed text.
  struct ParseLocation {
    int line;
    int column;

    ParseLocation() : line(-1), column(-1) {}
    ParseLocation(int line_param, int column_param)
        : line(line_param), column(column_param) {}
  };

  // A range of locations in the parsed text, including `start` and excluding
  // `end`.
  struct ParseLocationRange {
    ParseLocation start;
    ParseLocation end;
    ParseLocationRange() : start(), end() {}
    ParseLocationRange(ParseLocation start_param, ParseLocation end_param)
        : start(start_param), end(end_param) {}
  };

  // Data structure which is populated with the locations of each field
  // value parsed from the text.
  class PROTOBUF_EXPORT ParseInfoTree {
   public:
    ParseInfoTree() = default;
    ParseInfoTree(const ParseInfoTree&) = delete;
    ParseInfoTree& operator=(const ParseInfoTree&) = delete;

    // Returns the parse location range for index-th value of the field in
    // the parsed text. If none exists, returns a location with start and end
    // line -1. Index should be -1 for not-repeated fields.
    ParseLocationRange GetLocationRange(const FieldDescriptor* field,
                                        int index) const;

    // Returns the starting parse location for index-th value of the field in
    // the parsed text. If none exists, returns a location with line = -1. Index
    // should be -1 for not-repeated fields.
    ParseLocation GetLocation(const FieldDescriptor* field, int index) const {
      return GetLocationRange(field, index).start;
    }

    // Returns the parse info tree for the given field, which must be a message
    // type. The nested information tree is owned by the root tree and will be
    // deleted when it is deleted.
    ParseInfoTree* GetTreeForNested(const FieldDescriptor* field,
                                    int index) const;

   private:
    // Allow the text format parser to record information into the tree.
    friend class TextFormat;

    // Records the starting and ending locations of a single value for a field.
    void RecordLocation(const FieldDescriptor* field, ParseLocationRange range);

    // Create and records a nested tree for a nested message field.
    ParseInfoTree* CreateNested(const FieldDescriptor* field);

    // Defines the map from the index-th field descriptor to its parse location.
    y_absl::flat_hash_map<const FieldDescriptor*, std::vector<ParseLocationRange>>
        locations_;
    // Defines the map from the index-th field descriptor to the nested parse
    // info tree.
    y_absl::flat_hash_map<const FieldDescriptor*,
                        std::vector<std::unique_ptr<ParseInfoTree>>>
        nested_;
  };

  // For more control over parsing, use this class.
  class PROTOBUF_EXPORT Parser {
   public:
    Parser();
    ~Parser();

    // Like TextFormat::Parse().
    bool Parse(io::ZeroCopyInputStream* input, Message* output);
    // Like TextFormat::ParseFromString().
    bool ParseFromString(y_absl::string_view input, Message* output);
    // Like TextFormat::Merge().
    bool Merge(io::ZeroCopyInputStream* input, Message* output);
    // Like TextFormat::MergeFromString().
    bool MergeFromString(y_absl::string_view input, Message* output);

    // Set where to report parse errors.  If nullptr (the default), errors will
    // be printed to stderr.
    void RecordErrorsTo(io::ErrorCollector* error_collector) {
      error_collector_ = error_collector;
    }

    // Set how parser finds extensions.  If nullptr (the default), the
    // parser will use the standard Reflection object associated with
    // the message being parsed.
    void SetFinder(const Finder* finder) { finder_ = finder; }

    // Sets where location information about the parse will be written. If
    // nullptr
    // (the default), then no location will be written.
    void WriteLocationsTo(ParseInfoTree* tree) { parse_info_tree_ = tree; }

    // Normally parsing fails if, after parsing, output->IsInitialized()
    // returns false.  Call AllowPartialMessage(true) to skip this check.
    void AllowPartialMessage(bool allow) { allow_partial_ = allow; }

    // Allow field names to be matched case-insensitively.
    // This is not advisable if there are fields that only differ in case, or
    // if you want to enforce writing in the canonical form.
    // This is 'false' by default.
    void AllowCaseInsensitiveField(bool allow) {
      allow_case_insensitive_field_ = allow;
    }

    // Like TextFormat::ParseFieldValueFromString
    bool ParseFieldValueFromString(const TProtoStringType& input,
                                   const FieldDescriptor* field,
                                   Message* output);

    // When an unknown extension is met, parsing will fail if this option is
    // set to false (the default). If true, unknown extensions will be ignored
    // and a warning message will be generated.
    // Beware! Setting this option true may hide some errors (e.g. spelling
    // error on extension name).  This allows data loss; unlike binary format,
    // text format cannot preserve unknown extensions.  Avoid using this option
    // if possible.
    void AllowUnknownExtension(bool allow) { allow_unknown_extension_ = allow; }

    // When an unknown field is met, parsing will fail if this option is set
    // to false (the default). If true, unknown fields will be ignored and
    // a warning message will be generated.
    // Beware! Setting this option true may hide some errors (e.g. spelling
    // error on field name). This allows data loss; unlike binary format, text
    // format cannot preserve unknown fields.  Avoid using this option
    // if possible.
    void AllowUnknownField(bool allow) { allow_unknown_field_ = allow; }


    void AllowFieldNumber(bool allow) { allow_field_number_ = allow; }

    // Sets maximum recursion depth which parser can use. This is effectively
    // the maximum allowed nesting of proto messages.
    void SetRecursionLimit(int limit) { recursion_limit_ = limit; }

   private:
    // Forward declaration of an internal class used to parse text
    // representations (see text_format.cc for implementation).
    class ParserImpl;

    // Like TextFormat::Merge().  The provided implementation is used
    // to do the parsing.
    bool MergeUsingImpl(io::ZeroCopyInputStream* input, Message* output,
                        ParserImpl* parser_impl);

    io::ErrorCollector* error_collector_;
    const Finder* finder_;
    ParseInfoTree* parse_info_tree_;
    bool allow_partial_;
    bool allow_case_insensitive_field_;
    bool allow_unknown_field_;
    bool allow_unknown_extension_;
    bool allow_unknown_enum_;
    bool allow_field_number_;
    bool allow_relaxed_whitespace_;
    bool allow_singular_overwrites_;
    int recursion_limit_;
  };


 private:
  // Hack: ParseInfoTree declares TextFormat as a friend which should extend
  // the friendship to TextFormat::Parser::ParserImpl, but unfortunately some
  // old compilers (e.g. GCC 3.4.6) don't implement this correctly. We provide
  // helpers for ParserImpl to call methods of ParseInfoTree.
  static inline void RecordLocation(ParseInfoTree* info_tree,
                                    const FieldDescriptor* field,
                                    ParseLocationRange location);
  static inline ParseInfoTree* CreateNested(ParseInfoTree* info_tree,
                                            const FieldDescriptor* field);
};


inline void TextFormat::RecordLocation(ParseInfoTree* info_tree,
                                       const FieldDescriptor* field,
                                       ParseLocationRange location) {
  info_tree->RecordLocation(field, location);
}

inline TextFormat::ParseInfoTree* TextFormat::CreateNested(
    ParseInfoTree* info_tree, const FieldDescriptor* field) {
  return info_tree->CreateNested(field);
}

}  // namespace protobuf
}  // namespace google

#include "google/protobuf/port_undef.inc"

#endif  // GOOGLE_PROTOBUF_TEXT_FORMAT_H__
