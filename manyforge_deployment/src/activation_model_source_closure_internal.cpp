#include "activation_model_source_closure_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include <zlib.h>

#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/model_source.hpp>

namespace manyforge::deployment::detail {
namespace {

using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kSchema =
    "manyforge.activation_model_source_closure.v1";
constexpr std::size_t kMaximumComponents = 128U;
constexpr std::size_t kMaximumResources = 4096U;
constexpr std::size_t kMaximumTransferChunkCharacters = 700000U;
constexpr std::size_t kMaximumTransferChunksPerArtifact = 1024U;
constexpr std::uint64_t kMaximumGzipOverheadBytes = 1024ULL * 1024ULL;

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument("model_source_closure " + std::move(message));
}

[[nodiscard]] const Object &object(const CanonicalValue &value,
                                   std::string_view label) {
  const auto *result = std::get_if<Object>(&value.storage());
  if (result == nullptr) fail(std::string(label) + " must be an object");
  return *result;
}

[[nodiscard]] const Array &array(const CanonicalValue &value,
                                 std::string_view label) {
  const auto *result = std::get_if<Array>(&value.storage());
  if (result == nullptr) fail(std::string(label) + " must be an array");
  return *result;
}

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           std::string_view name,
                                           std::string_view label) {
  const auto *result = value.find_object_member(name);
  if (result == nullptr) {
    fail(std::string(label) + " is missing " + std::string(name));
  }
  return *result;
}

void require_fields(const CanonicalValue &value,
                    std::initializer_list<std::string_view> expected,
                    std::string_view label) {
  const auto &members = object(value, label);
  std::set<std::string, std::less<>> actual;
  for (const auto &[name, unused] : members) {
    static_cast<void>(unused);
    actual.insert(name);
  }
  const std::set<std::string, std::less<>> wanted(expected.begin(),
                                                  expected.end());
  if (actual != wanted) {
    for (const auto &name : wanted) {
      if (!actual.contains(name)) {
        fail(std::string(label) + " is missing " + name);
      }
    }
    for (const auto &name : actual) {
      if (!wanted.contains(name)) {
        fail(std::string(label) + " has unexpected field " + name);
      }
    }
  }
}

[[nodiscard]] const std::string &text(const CanonicalValue &value,
                                      std::string_view label) {
  const auto *result = std::get_if<std::string>(&value.storage());
  if (result == nullptr || result->empty()) {
    fail(std::string(label) + " must be non-empty text");
  }
  return *result;
}

[[nodiscard]] std::uint64_t bounded_integer(const CanonicalValue &value,
                                            std::uint64_t maximum,
                                            std::string_view label) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
      std::floor(*number) != *number ||
      *number > static_cast<double>(maximum)) {
    fail(std::string(label) + " must be a bounded non-negative integer");
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] bool portable_id(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U || value.front() < 'a' ||
      value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-';
  });
}

[[nodiscard]] bool safe_package_uri(std::string_view value) noexcept {
  constexpr std::string_view prefix = "package://";
  if (!value.starts_with(prefix) || value.size() > 4096U ||
      value.find('\\') != std::string_view::npos) {
    return false;
  }
  const auto path = value.substr(prefix.size());
  if (path.empty() || path.front() == '/' || path.back() == '/') return false;
  std::size_t offset = 0U;
  std::size_t segment_index = 0U;
  while (offset < path.size()) {
    const auto separator = path.find('/', offset);
    const auto segment = path.substr(
        offset, separator == std::string_view::npos ? path.size() - offset
                                                    : separator - offset);
    if (segment.empty() || segment == "." || segment == ".." ||
        (segment_index == 0U && !portable_id(segment))) {
      return false;
    }
    ++segment_index;
    if (separator == std::string_view::npos) break;
    offset = separator + 1U;
  }
  return segment_index >= 2U;
}

[[nodiscard]] int base64_value(char character) noexcept {
  if (character >= 'A' && character <= 'Z') return character - 'A';
  if (character >= 'a' && character <= 'z') return character - 'a' + 26;
  if (character >= '0' && character <= '9') return character - '0' + 52;
  if (character == '+') return 62;
  if (character == '/') return 63;
  return -1;
}

void append_base64(std::string_view encoded, std::string &output,
                   std::string_view label) {
  if (encoded.empty() || encoded.size() > kMaximumTransferChunkCharacters ||
      encoded.size() % 4U != 0U) {
    fail(std::string(label) + " is not one bounded base64 chunk");
  }
  for (std::size_t offset = 0U; offset < encoded.size(); offset += 4U) {
    const bool final_group = offset + 4U == encoded.size();
    const char third_character = encoded[offset + 2U];
    const char fourth_character = encoded[offset + 3U];
    const bool third_padding = third_character == '=';
    const bool fourth_padding = fourth_character == '=';
    const int first = base64_value(encoded[offset]);
    const int second = base64_value(encoded[offset + 1U]);
    const int third = third_padding ? 0 : base64_value(third_character);
    const int fourth = fourth_padding ? 0 : base64_value(fourth_character);
    if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
        (third_padding && !fourth_padding) ||
        ((third_padding || fourth_padding) && !final_group)) {
      fail(std::string(label) + " is not canonical base64");
    }
    if ((third_padding && (second & 0x0f) != 0) ||
        (!third_padding && fourth_padding && (third & 0x03) != 0)) {
      fail(std::string(label) + " has non-zero base64 padding bits");
    }
    output.push_back(static_cast<char>((first << 2) | (second >> 4)));
    if (!third_padding) {
      output.push_back(
          static_cast<char>(((second & 0x0f) << 4) | (third >> 2)));
    }
    if (!fourth_padding) {
      output.push_back(
          static_cast<char>(((third & 0x03) << 6) | fourth));
    }
  }
}

[[nodiscard]] std::string inflate_gzip(std::string_view compressed,
                                       std::size_t expected_size,
                                       std::string_view label) {
  if (compressed.empty() || compressed.size() >
                                kMaxModelSourceOpaqueArtifactBytes +
                                    kMaximumGzipOverheadBytes) {
    fail(std::string(label) + " has an invalid compressed size");
  }
  std::string result(expected_size, '\0');
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef *>(
      const_cast<char *>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = reinterpret_cast<Bytef *>(result.data());
  stream.avail_out = static_cast<uInt>(result.size());
  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
    fail(std::string(label) + " could not initialize gzip decoding");
  }
  const int status = inflate(&stream, Z_FINISH);
  const auto consumed = stream.total_in;
  const auto produced = stream.total_out;
  const int end_status = inflateEnd(&stream);
  if (status != Z_STREAM_END || end_status != Z_OK ||
      consumed != compressed.size() || produced != expected_size) {
    fail(std::string(label) + " is not one exact gzip stream");
  }
  return result;
}

[[nodiscard]] CapturedModelSourceArtifact parse_artifact(
    const CanonicalValue &value, std::string logical_uri,
    std::uint64_t maximum_bytes, std::string_view label) {
  require_fields(value,
                 {"byte_size", "chunks", "content_digest", "encoding",
                  "media_type", "transferred_byte_size"},
                 label);
  const auto byte_size = bounded_integer(member(value, "byte_size", label),
                                         maximum_bytes,
                                         std::string(label) + ".byte_size");
  if (byte_size == 0U) fail(std::string(label) + " is empty");
  const auto &encoding = text(member(value, "encoding", label),
                              std::string(label) + ".encoding");
  if (encoding != "gzip+base64") {
    fail(std::string(label) + " has unsupported encoding");
  }
  const auto &media_type = text(member(value, "media_type", label),
                                std::string(label) + ".media_type");
  const auto &digest = text(member(value, "content_digest", label),
                            std::string(label) + ".content_digest");
  if (!is_sha256_digest(digest)) {
    fail(std::string(label) + " has an invalid content digest");
  }
  const auto transferred_size = bounded_integer(
      member(value, "transferred_byte_size", label),
      maximum_bytes + kMaximumGzipOverheadBytes,
      std::string(label) + ".transferred_byte_size");
  const auto &chunks = array(member(value, "chunks", label),
                             std::string(label) + ".chunks");
  if (chunks.empty() || chunks.size() > kMaximumTransferChunksPerArtifact) {
    fail(std::string(label) + " has an invalid chunk count");
  }
  std::string compressed;
  compressed.reserve(static_cast<std::size_t>(transferred_size));
  for (std::size_t index = 0U; index < chunks.size(); ++index) {
    append_base64(text(chunks[index], std::string(label) + ".chunk"),
                  compressed, std::string(label) + ".chunk");
    if (compressed.size() > transferred_size) {
      fail(std::string(label) + " exceeds its transferred size");
    }
  }
  if (compressed.size() != transferred_size) {
    fail(std::string(label) + " does not match its transferred size");
  }
  auto bytes = inflate_gzip(compressed, static_cast<std::size_t>(byte_size),
                            label);
  if (sha256_digest(bytes) != digest) {
    fail(std::string(label) + " does not match its content digest");
  }
  return {std::move(logical_uri), media_type, core::ContentDigest(digest),
          std::move(bytes)};
}

[[nodiscard]] CapturedModelSourceArtifact parse_entrypoint(
    const CanonicalValue &value, std::string_view kind,
    std::string_view component_id) {
  const std::string label = std::string(component_id) + ".entrypoints." +
                            std::string(kind);
  auto result = parse_artifact(value, std::string(kind),
                               kMaxModelSourceXmlEntrypointBytes, label);
  if (result.media_type != "application/xml") {
    fail(label + " must use application/xml");
  }
  return result;
}

[[nodiscard]] CapturedModelSourceArtifact parse_resource(
    const CanonicalValue &value, std::string_view component_id) {
  require_fields(value,
                 {"byte_size", "chunks", "content_digest", "encoding",
                  "logical_uri", "media_type", "transferred_byte_size"},
                 std::string(component_id) + ".resource");
  const auto &uri = text(member(value, "logical_uri", "resource"),
                         "resource.logical_uri");
  if (!safe_package_uri(uri)) {
    fail(std::string(component_id) + ".resource has an unsafe logical URI");
  }
  CanonicalValue::Object artifact_members;
  for (const auto &[name, child] : object(value, "resource")) {
    if (name != "logical_uri") artifact_members.emplace_back(name, child);
  }
  return parse_artifact(CanonicalValue(std::move(artifact_members)), uri,
                        kMaxModelSourceOpaqueArtifactBytes,
                        std::string(component_id) + ".resource");
}

} // namespace

CapturedModelSourceArtifact parse_activation_model_source_transfer_artifact(
    const CanonicalValue &value, std::string logical_uri,
    std::uint64_t maximum_bytes, std::string_view label) {
  return parse_artifact(value, std::move(logical_uri), maximum_bytes, label);
}

std::vector<CapturedComponentModelSource>
parse_activation_model_source_closure(const CanonicalValue &value) {
  require_fields(value, {"components", "schema_version"}, "root");
  if (text(member(value, "schema_version", "root"), "schema_version") !=
      kSchema) {
    fail("has an unsupported schema version");
  }
  const auto &components = array(member(value, "components", "root"),
                                 "components");
  if (components.size() > kMaximumComponents) {
    fail("has an invalid component count");
  }
  std::vector<CapturedComponentModelSource> result;
  result.reserve(components.size());
  std::set<std::string, std::less<>> component_ids;
  std::uint64_t closure_bytes = 0U;
  std::size_t resource_count = 0U;
  std::string previous_component;
  for (const auto &component : components) {
    require_fields(component, {"component_id", "entrypoints", "resources"},
                   "component");
    const auto &component_id = text(member(component, "component_id", "component"),
                                    "component.component_id");
    if (!portable_id(component_id) || !component_ids.insert(component_id).second ||
        (!previous_component.empty() && component_id < previous_component)) {
      fail("components are not uniquely canonical-sorted portable identifiers");
    }
    previous_component = component_id;
    const auto &entrypoints = member(component, "entrypoints", "component");
    const auto &entrypoint_members = object(entrypoints, "entrypoints");
    if (entrypoint_members.empty() || entrypoint_members.size() > 2U ||
        entrypoints.find_object_member("urdf") == nullptr ||
        (entrypoint_members.size() == 2U &&
         entrypoints.find_object_member("srdf") == nullptr)) {
      fail(component_id + ".entrypoints has an invalid shape");
    }
    auto urdf = parse_entrypoint(member(entrypoints, "urdf", "entrypoints"),
                                 "urdf", component_id);
    std::optional<CapturedModelSourceArtifact> srdf;
    if (const auto *candidate = entrypoints.find_object_member("srdf");
        candidate != nullptr) {
      srdf = parse_entrypoint(*candidate, "srdf", component_id);
    }
    closure_bytes += urdf.bytes.size();
    if (srdf.has_value()) closure_bytes += srdf->bytes.size();
    std::vector<CapturedModelSourceArtifact> resources;
    std::set<std::string, std::less<>> resource_uris;
    std::string previous_uri;
    for (const auto &resource :
         array(member(component, "resources", "component"), "resources")) {
      if (++resource_count > kMaximumResources) {
        fail("exceeds the resource count bound");
      }
      auto parsed = parse_resource(resource, component_id);
      if (!resource_uris.insert(parsed.logical_uri).second ||
          (!previous_uri.empty() && parsed.logical_uri < previous_uri)) {
        fail(component_id + ".resources are not uniquely canonical-sorted");
      }
      previous_uri = parsed.logical_uri;
      closure_bytes += parsed.bytes.size();
      if (closure_bytes > kMaxModelSourceClosureBytes) {
        fail("exceeds the decoded byte bound");
      }
      resources.push_back(std::move(parsed));
    }
    if (closure_bytes > kMaxModelSourceClosureBytes) {
      fail("exceeds the decoded byte bound");
    }
    result.push_back({component_id, std::move(urdf), std::move(srdf),
                      std::move(resources)});
  }
  return result;
}

} // namespace manyforge::deployment::detail
