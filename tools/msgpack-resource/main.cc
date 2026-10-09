// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#else
#include <sys/stat.h>
#endif

#include "include/libplatform/libplatform.h"
#include "include/v8.h"
#include "src/api/api-inl.h"
#include "src/base/platform/platform.h"
#include "src/base/platform/wrappers.h"
#include "src/msgpack/messagepack-resource-format.h"
#include "src/msgpack/messagepack-string.h"
#include "src/msgpack/messagepack.h"

namespace {
namespace i = v8::internal;
constexpr size_t kMaxBytes = 256 * 1024 * 1024;
constexpr char kVersion[] = "0.1.0";

struct Options {
  std::string input;
  std::string output;
  bool standard = false;
  bool stats = false;
  bool force = false;
};

void Usage(std::ostream& out) {
  out << "Usage: msgpack-resource [options] INPUT.json -o OUTPUT.v8mr\n"
         "\n"
         "Convert one UTF-8 JSON value using the engine's C++ resource encoder.\n"
         "Compact V8MR v1 is selected only when the complete file is smaller\n"
         "than standard MessagePack. Both outputs work with decodeResource.\n"
         "\n"
         "  -o, --output FILE  Required output path; '-' writes binary stdout\n"
         "  --standard         Produce standard MessagePack instead\n"
         "  --stats            Print JSON size statistics to stderr\n"
         "  --force            Allow replacing an existing output file\n"
         "  --                 End option parsing (for filenames starting '-')\n"
         "  -h, --help         Show this help\n"
         "  --version          Show tool, resource format and V8 versions\n"
         "\n"
         "INPUT '-' reads stdin. Existing files are protected by default.\n";
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  bool positional = false;
  for (int index = 1; index < argc; ++index) {
    std::string arg = argv[index];
    if (!positional && arg == "--") {
      positional = true;
    } else if (!positional && (arg == "-o" || arg == "--output")) {
      if (++index == argc || !options.output.empty())
        throw std::invalid_argument("Specify exactly one output path with -o");
      options.output = argv[index];
    } else if (!positional && arg == "--standard") {
      options.standard = true;
    } else if (!positional && arg == "--stats") {
      options.stats = true;
    } else if (!positional && arg == "--force") {
      options.force = true;
    } else if (!positional && arg.size() > 1 && arg[0] == '-') {
      throw std::invalid_argument("Unknown option: " + arg);
    } else if (options.input.empty()) {
      options.input = std::move(arg);
    } else {
      throw std::invalid_argument("Expected exactly one JSON input file");
    }
  }
  if (options.input.empty() || options.output.empty())
    throw std::invalid_argument("An input and an explicit -o output are required");
  if (options.output != "-") {
#ifdef _WIN32
    const auto output = std::filesystem::u8path(options.output);
    const bool output_exists = std::filesystem::exists(output);
    const bool same_file =
        output_exists && options.input != "-" &&
        std::filesystem::equivalent(std::filesystem::u8path(options.input),
                                    output);
#else
    // stat also detects hardlink/symlink aliases and supports the existing
    // macOS 10.13 and iOS 11 deployment targets (unlike std::filesystem).
    struct stat output_stat;
    const bool output_exists = stat(options.output.c_str(), &output_stat) == 0;
    if (!output_exists && errno != ENOENT && errno != ENOTDIR) {
      throw std::runtime_error("Cannot inspect output: " + options.output +
                               ": " + std::strerror(errno));
    }
    struct stat input_stat;
    const bool same_file =
        output_exists && options.input != "-" &&
        stat(options.input.c_str(), &input_stat) == 0 &&
        input_stat.st_dev == output_stat.st_dev &&
        input_stat.st_ino == output_stat.st_ino;
#endif
    if (output_exists) {
      if (same_file) {
        throw std::invalid_argument("Input and output must be different files");
      }
      if (!options.force)
        throw std::invalid_argument("Output already exists; use --force to replace it");
    }
  }
  return options;
}

// The only new code is CLI/I/O handling. UTF-8, JSON, supported-value checks,
// compact selection and all wire writing are shared with the engine.
std::string ReadInput(const std::string& filename) {
  FILE* file = filename == "-" ? stdin : v8::base::Fopen(filename.c_str(), "rb");
  if (!file) throw std::runtime_error("Cannot open input: " + filename);
#ifdef _WIN32
  if (file == stdin) _setmode(_fileno(stdin), _O_BINARY);
#endif
  std::string bytes;
  std::array<char, 64 * 1024> block;
  try {
    while (size_t count = std::fread(block.data(), 1, block.size(), file)) {
      if (count > kMaxBytes - bytes.size())
        throw std::runtime_error("JSON input exceeds 256 MiB");
      bytes.append(block.data(), count);
    }
    if (std::ferror(file)) throw std::runtime_error("Failed reading JSON input");
  } catch (...) {
    if (file != stdin) std::fclose(file);
    throw;
  }
  if (file != stdin) std::fclose(file);
  i::messagepack_strings::Utf8Info info;
  if (!i::messagepack_strings::ScanUtf8(
          reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &info)) {
    throw std::runtime_error("JSON input is not valid UTF-8");
  }
  return bytes;
}

void WriteOutput(const Options& options, const i::MessagePackBuffer& bytes) {
#ifdef _WIN32
  if (options.output == "-") _setmode(_fileno(stdout), _O_BINARY);
#endif
  // Exclusive creation prevents a default invocation from truncating a file
  // that appeared after argument validation. Open only after encoding succeeds.
  FILE* file = options.output == "-"
                   ? stdout
                   : v8::base::Fopen(options.output.c_str(),
                                     options.force ? "wb" : "wbx");
  if (!file) {
    throw std::runtime_error("Cannot create output: " + options.output +
                             ": " + std::strerror(errno));
  }
  bool success = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  if (file == stdout) {
    success = std::fflush(file) == 0 && success;
  } else {
    success = std::fclose(file) == 0 && success;
    if (!success && !options.force) {
#ifdef _WIN32
      std::filesystem::remove(std::filesystem::u8path(options.output));
#else
      std::remove(options.output.c_str());
#endif
    }
  }
  if (!success) throw std::runtime_error("Failed writing output");
}

class Runtime {
 public:
  explicit Runtime(const char* executable)
      : platform_(v8::platform::NewDefaultPlatform()),
        allocator_(v8::ArrayBuffer::Allocator::NewDefaultAllocator()) {
    if (!v8::V8::InitializeICUDefaultLocation(executable))
      throw std::runtime_error("Cannot initialize ICU data");
    v8::V8::InitializePlatform(platform_.get());
    if (!v8::V8::Initialize()) throw std::runtime_error("Cannot initialize V8");
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator_.get();
    isolate_ = v8::Isolate::New(params);
  }
  ~Runtime() {
    isolate_->Dispose();
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
  }
  v8::Isolate* isolate() { return isolate_; }

 private:
  std::unique_ptr<v8::Platform> platform_;
  std::unique_ptr<v8::ArrayBuffer::Allocator> allocator_;
  v8::Isolate* isolate_ = nullptr;
};

std::string ExceptionText(v8::Isolate* isolate, v8::TryCatch& caught) {
  if (!caught.HasCaught()) return "V8 operation failed";
  v8::String::Utf8Value text(isolate, caught.Exception());
  return *text ? std::string(*text, text.length()) : "V8 operation failed";
}

void Convert(const Options& options, const char* executable) {
  std::string source = ReadInput(options.input);
  const size_t input_bytes = source.size();
  // Accept a UTF-8 BOM at the start of a file, but never replace invalid bytes.
  if (source.compare(0, 3, "\xef\xbb\xbf") == 0) source.erase(0, 3);
  Runtime runtime(executable);
  v8::Isolate* isolate = runtime.isolate();
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handles(isolate);
  auto context = v8::Context::New(isolate);
  v8::Context::Scope context_scope(context);
  v8::TryCatch caught(isolate);
  v8::Local<v8::String> text;
  v8::Local<v8::Value> value;
  if (!v8::String::NewFromUtf8(isolate, source.data(), v8::NewStringType::kNormal,
                              static_cast<int>(source.size()))
           .ToLocal(&text) ||
      !v8::JSON::Parse(context, text).ToLocal(&value)) {
    throw std::runtime_error("Invalid JSON: " + ExceptionText(isolate, caught));
  }
  i::MessagePackBuffer bytes;
  std::string error;
  auto* internal_isolate = reinterpret_cast<i::Isolate*>(isolate);
  auto input = v8::Utils::OpenHandle(*value);
  const bool encoded = options.standard
                           ? i::EncodeMessagePack(internal_isolate, input, &bytes,
                                                  &error, true)
                           : i::EncodeMessagePackResource(internal_isolate, input,
                                                          &bytes, &error);
  if (!encoded) {
    if (caught.HasCaught()) error = ExceptionText(isolate, caught);
    throw std::runtime_error("Encoding failed: " + error);
  }
  const bool compact = bytes.size() >= 5 &&
                       std::memcmp(bytes.data(), messagepack_resources::kMagic,
                                   5) == 0;
  size_t standard_bytes = bytes.size();
  if (options.stats && compact) {
    i::MessagePackBuffer standard;
    if (!i::EncodeMessagePack(internal_isolate, input, &standard, &error, true))
      throw std::runtime_error("Cannot compute standard size: " + error);
    standard_bytes = standard.size();
  }
  WriteOutput(options, bytes);
  if (options.stats) {
    std::cerr << "{\"format\":\"" << (compact ? "v8mr-v1" : "msgpack")
              << "\",\"inputBytes\":" << input_bytes
              << ",\"standardBytes\":" << standard_bytes
              << ",\"outputBytes\":" << bytes.size()
              << ",\"bytesSaved\":" << standard_bytes - bytes.size() << "}\n";
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && (std::string(argv[1]) == "--help" ||
                    std::string(argv[1]) == "-h")) {
    Usage(std::cout);
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "msgpack-resource " << kVersion << " (V8MR v1; V8 "
              << v8::V8::GetVersion() << ")\n";
    return 0;
  }
  try {
    Convert(ParseOptions(argc, argv), argv[0]);
    return 0;
  } catch (const std::invalid_argument& error) {
    std::cerr << "msgpack-resource: " << error.what() << '\n';
    Usage(std::cerr);
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "msgpack-resource: " << error.what() << '\n';
    return 1;
  }
}
