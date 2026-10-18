#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <set>
#include <stdexcept>
#include <tuple>

namespace manyforge::core {
namespace {
using V = content_identity::CanonicalValue;
constexpr std::uint64_t kSafe = 9'007'199'254'740'991ULL;
[[noreturn]] void bad(std::string_view why) {
  throw std::invalid_argument(std::string(why));
}
void require(bool value, std::string_view why) {
  if (!value) bad(why);
}
const V& get(const V& v, std::string_view name) {
  const auto* p = v.find_object_member(name);
  if (!p) bad("missing rule evidence field");
  return *p;
}
void shape(const V& v, std::initializer_list<std::string_view> required,
           std::initializer_list<std::string_view> optional = {}) {
  require(v.is_object(), "rule evidence object required");
  for (auto name : required) (void)get(v, name);
  std::set<std::string_view> seen;
  for (const auto& [name, value] : v.as_object()) {
    (void)value;
    require(seen.insert(name).second, "duplicate evidence field");
    require(std::find(required.begin(), required.end(), name) != required.end() ||
                std::find(optional.begin(), optional.end(), name) != optional.end(),
            "unknown rule evidence field");
  }
}
std::string text(const V& v, bool identity = true) {
  require(v.is_string(), "evidence string required");
  const auto& s = v.as_string();
  if (identity)
    require(!s.empty() && s.find('\0') == std::string::npos, "invalid evidence identity");
  return s;
}
template <class T>
T id(const V& v) {
  return T(text(v));
}
template <class T>
V idv(const T& v) {
  return V(text(V(v.value())));
}
std::uint64_t counter(const V& v, bool positive = true) {
  const auto s = text(v);
  std::uint64_t n{};
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  require(ec == std::errc{} && p == s.data() + s.size() && std::to_string(n) == s &&
              (!positive || n != 0U),
          "invalid canonical unsigned evidence counter");
  return n;
}
V count(std::uint64_t n, bool positive = true) {
  require(!positive || n != 0U, "zero evidence counter");
  return V(std::to_string(n));
}
std::uint64_t safe(const V& v, bool positive = true) {
  require(
      v.is_number() && std::isfinite(v.as_number()) && v.as_number() >= (positive ? 1.0 : 0.0) &&
          v.as_number() <= static_cast<double>(kSafe) && std::floor(v.as_number()) == v.as_number(),
      "invalid safe evidence number");
  return static_cast<std::uint64_t>(v.as_number());
}
V number(std::uint64_t n, bool positive = true) {
  require(n <= kSafe && (!positive || n), "unsafe evidence number");
  return V(static_cast<double>(n));
}
TimePoint stamp(const V& v) {
  const auto n = counter(v, false);
  require(n <= static_cast<std::uint64_t>(INT64_MAX), "evidence time overflow");
  return static_cast<TimePoint>(n);
}
V time(TimePoint n) {
  require(n >= 0, "negative evidence time");
  return count(static_cast<std::uint64_t>(n), false);
}
bool boolean(const V& v) {
  require(v.is_bool(), "evidence Boolean required");
  return v.as_bool();
}
const V::Array& array(const V& v) {
  require(v.is_array(), "evidence array required");
  return v.as_array();
}
template <class T, class F>
std::optional<T> optional(const V& v, F f) {
  return v.is_null() ? std::nullopt : std::optional<T>(f(v));
}
template <class T, class F>
V opt(const std::optional<T>& v, F f) {
  return v ? f(*v) : V(nullptr);
}
V error_value(const ErrorDTO& e) {
  require(!e.code.empty(), "empty evidence error code");
  V::Object details;
  for (const auto& [k, v] : e.details) details.emplace_back(k, V(v));
  return V(V::Object{
      {"code", V(e.code)}, {"message", V(e.message)}, {"details", V(std::move(details))}});
}
ErrorDTO error_from(const V& v) {
  shape(v, {"code", "message", "details"});
  ErrorDTO e{text(get(v, "code")), text(get(v, "message"), false), {}};
  const auto& d = get(v, "details");
  require(d.is_object(), "error details object required");
  for (const auto& [k, x] : d.as_object())
    require(e.details.emplace(k, text(x, false)).second, "duplicate error detail");
  return e;
}
V error(const std::optional<ErrorDTO>& e) {
  return opt(e, error_value);
}
std::optional<ErrorDTO> error(const V& e) {
  return optional<ErrorDTO>(e, error_from);
}
V cursor_value(const IoSourceCursorDTO& c) {
  return V(V::Object{{"owner_epoch", idv(c.owner_epoch)},
                     {"catalog_generation", count(c.catalog_generation, false)},
                     {"source_id", idv(c.source_id)},
                     {"record_sequence", count(c.record_sequence, false)}});
}
IoSourceCursorDTO cursor_from(const V& v) {
  shape(v, {"owner_epoch", "catalog_generation", "source_id", "record_sequence"});
  return {id<RequestOwnerEpoch>(get(v, "owner_epoch")),
          counter(get(v, "catalog_generation"), false), id<InterruptSourceId>(get(v, "source_id")),
          counter(get(v, "record_sequence"), false)};
}
V registration_value(const IoRuleRegistrationDTO& r) {
  const auto* s = std::get_if<IoOperationRuleScopeDTO>(&r.scope);
  if (!s) {
    const auto& activation = std::get<IoActivationRuleScopeDTO>(r.scope);
    return V(
        V::Object{{"owner_epoch", idv(r.owner_epoch)},
                  {"catalog_generation", count(r.catalog_generation, false)},
                  {"registration_id", count(r.registration_id)},
                  {"policy_id", idv(r.policy_id)},
                  {"scope", V(V::Object{{"kind", V("activation")},
                                        {"retained_bundle_id", idv(activation.retained_bundle_id)},
                                        {"graph_revision", idv(activation.graph_revision)}})}});
  }
  return V(V::Object{
      {"owner_epoch", idv(r.owner_epoch)},
      {"catalog_generation", count(r.catalog_generation, false)},
      {"registration_id", count(r.registration_id)},
      {"policy_id", idv(r.policy_id)},
      {"scope",
       V(V::Object{{"kind", V("operation")},
                   {"operation_id", idv(s->operation_id)},
                   {"operation_revision", number(s->operation_revision.value())},
                   {"participant_id", idv(s->participant_id)},
                   {"controllable_component_id", idv(s->controllable_component_id)},
                   {"command_target_id", idv(s->command_target_id)},
                   {"controller_goal_id", idv(s->controller_goal_id)},
                   {"controller_goal_generation", number(s->controller_goal_generation.value())},
                   {"preparation_id", count(s->preparation_id)},
                   {"prepared_contract_digest", idv(s->prepared_contract_digest)},
                   {"motion_interrupt_scope_id", idv(s->motion_interrupt_scope_id)}})}});
}
IoRuleRegistrationDTO registration_from(const V& v) {
  shape(v, {"owner_epoch", "catalog_generation", "registration_id", "policy_id", "scope"});
  const auto& s = get(v, "scope");
  if (text(get(s, "kind")) == "activation") {
    shape(s, {"kind", "retained_bundle_id", "graph_revision"});
    return {id<RequestOwnerEpoch>(get(v, "owner_epoch")),
            counter(get(v, "catalog_generation"), false), counter(get(v, "registration_id")),
            id<InterruptPolicyId>(get(v, "policy_id")),
            IoActivationRuleScopeDTO{id<RetainedBundleId>(get(s, "retained_bundle_id")),
                                     id<GraphRevision>(get(s, "graph_revision"))}};
  }
  shape(s, {"kind", "operation_id", "operation_revision", "participant_id",
            "controllable_component_id", "command_target_id", "controller_goal_id",
            "controller_goal_generation", "preparation_id", "prepared_contract_digest",
            "motion_interrupt_scope_id"});
  require(text(get(s, "kind")) == "operation", "activation scope is not an operation result");
  return {
      id<RequestOwnerEpoch>(get(v, "owner_epoch")), counter(get(v, "catalog_generation"), false),
      counter(get(v, "registration_id")), id<InterruptPolicyId>(get(v, "policy_id")),
      IoOperationRuleScopeDTO{id<OperationId>(get(s, "operation_id")),
                              OperationRevision(safe(get(s, "operation_revision"))),
                              id<OperationParticipantId>(get(s, "participant_id")),
                              id<ControllableComponentId>(get(s, "controllable_component_id")),
                              id<CommandTargetId>(get(s, "command_target_id")),
                              id<ControllerGoalId>(get(s, "controller_goal_id")),
                              ControllerGoalGeneration(safe(get(s, "controller_goal_generation"))),
                              counter(get(s, "preparation_id")),
                              id<ContentDigest>(get(s, "prepared_contract_digest")),
                              id<MotionInterruptScopeId>(get(s, "motion_interrupt_scope_id"))}};
}
V sample_value(const IoSignalSampleDTO& s) {
  const auto& p = s.publication;
  const char* validity = p.validity == IoSampleValidity::kValid         ? "valid"
                         : p.validity == IoSampleValidity::kInvalid     ? "invalid"
                         : p.validity == IoSampleValidity::kUnavailable ? "unavailable"
                                                                        : nullptr;
  require(validity, "invalid sample validity");
  require((p.validity == IoSampleValidity::kValid) == p.value.has_value(),
          "sample validity/value disagreement");
  return V(V::Object{{"controllable_component_id", idv(p.signal_id.controllable_component_id)},
                     {"signal_name", V(text(V(p.signal_id.name)))},
                     {"source_id", idv(p.source_id)},
                     {"source_epoch", idv(p.incarnation)},
                     {"source_sequence", count(p.source_sequence)},
                     {"validity", V(validity)},
                     {"value", p.value ? V(*p.value) : V(nullptr)},
                     {"error", error(p.error)},
                     {"received_at_ns", time(s.received_at_ns)}});
}
IoSignalSampleDTO sample_from(const V& v) {
  shape(v, {"controllable_component_id", "signal_name", "source_id", "source_epoch", "source_sequence",
            "validity", "value", "error", "received_at_ns"});
  const auto name = text(get(v, "validity"));
  const auto kind = name == "valid"         ? IoSampleValidity::kValid
                    : name == "invalid"     ? IoSampleValidity::kInvalid
                    : name == "unavailable" ? IoSampleValidity::kUnavailable
                                            : static_cast<IoSampleValidity>(-1);
  IoSignalSampleDTO s{
      {{id<ControllableComponentId>(get(v, "controllable_component_id")), text(get(v, "signal_name"))},
       id<InterruptSourceId>(get(v, "source_id")),
       id<IoSourceIncarnation>(get(v, "source_epoch")),
       counter(get(v, "source_sequence")),
       kind,
       optional<bool>(get(v, "value"), boolean),
       error(get(v, "error"))},
      stamp(get(v, "received_at_ns"))};
  (void)sample_value(s);
  return s;
}
V bb_scope_value(const IoProgramBlackboardScopeDTO& s) {
  require(s.claim_generation.has_value() == s.program_run_id.has_value(), "partial BB Run scope");
  return V(V::Object{
      {"owner_epoch", idv(s.request_owner_epoch)},
      {"program_runner_owner_epoch", idv(s.program_runner_owner_epoch)},
      {"program_binding_id", idv(s.program_binding_id)},
      {"active_publication_generation", number(s.active_publication_generation.value())},
      {"cycle_generation", number(s.cycle_generation)},
      {"claim_generation", opt(s.claim_generation, [](auto x) { return number(x.value()); })},
      {"program_run_id", opt(s.program_run_id, [](const auto& x) { return idv(x); })}});
}
IoProgramBlackboardScopeDTO bb_scope_from(const V& v) {
  shape(v, {"owner_epoch", "program_runner_owner_epoch", "program_binding_id",
            "active_publication_generation", "cycle_generation", "claim_generation",
            "program_run_id"});
  IoProgramBlackboardScopeDTO s{
      id<RequestOwnerEpoch>(get(v, "owner_epoch")),
      id<RequestOwnerEpoch>(get(v, "program_runner_owner_epoch")),
      safe(get(v, "cycle_generation")),
      optional<CellAdmissionClaimGeneration>(
          get(v, "claim_generation"),
          [](const V& x) { return CellAdmissionClaimGeneration(safe(x)); }),
      optional<ProgramRunId>(get(v, "program_run_id"),
                             [](const V& x) { return id<ProgramRunId>(x); }),
      id<ProgramBindingId>(get(v, "program_binding_id")),
      ActiveBundlePublicationGeneration(safe(get(v, "active_publication_generation")))};
  (void)bb_scope_value(s);
  return s;
}
V bb_value(const IoBlackboardValueDTO& v) {
  require(v.value.has_value(), "matched BB key is unavailable");
  V scalar(nullptr);
  const char* type = nullptr;
  switch (v.value_kind) {
    case IoConditionValueKind::kBoolean:
      require(std::holds_alternative<bool>(*v.value), "BB type mismatch");
      type = "bool";
      scalar = V(std::get<bool>(*v.value));
      break;
    case IoConditionValueKind::kInt64:
      require(std::holds_alternative<std::int64_t>(*v.value), "BB type mismatch");
      type = "int64";
      scalar = V(std::to_string(std::get<std::int64_t>(*v.value)));
      break;
    case IoConditionValueKind::kFloat64:
      require(std::holds_alternative<double>(*v.value) && std::isfinite(std::get<double>(*v.value)),
              "BB type mismatch");
      type = "float64";
      scalar = V(std::get<double>(*v.value));
      break;
    case IoConditionValueKind::kString:
      require(std::holds_alternative<std::string>(*v.value), "BB type mismatch");
      type = "string";
      scalar = V(std::get<std::string>(*v.value));
      break;
  }
  require(type, "unknown BB type");
  return V(V::Object{
      {"name", V(text(V(v.name)))}, {"value_type", V(type)}, {"value", std::move(scalar)}});
}
IoBlackboardValueDTO bb_value_from(const V& v) {
  shape(v, {"name", "value_type", "value"});
  auto name = text(get(v, "name"));
  const auto type = text(get(v, "value_type"));
  const auto& x = get(v, "value");
  IoBlackboardValueDTO r{std::move(name), IoConditionValueKind::kBoolean, std::nullopt};
  if (type == "bool")
    r.value = boolean(x);
  else if (type == "int64") {
    const auto s = text(x);
    std::int64_t n{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    require(ec == std::errc{} && p == s.data() + s.size() && std::to_string(n) == s,
            "invalid signed BB integer");
    r.value_kind = IoConditionValueKind::kInt64;
    r.value = n;
  } else if (type == "float64") {
    require(x.is_number() && std::isfinite(x.as_number()), "finite BB number required");
    r.value_kind = IoConditionValueKind::kFloat64;
    r.value = x.as_number();
  } else if (type == "string") {
    r.value_kind = IoConditionValueKind::kString;
    r.value = text(x, false);
  } else
    bad("unknown BB value type");
  return r;
}
V bb_trip_value(const IoBlackboardTripEvidenceDTO& b) {
  require(!b.values.empty(), "matched BB values are empty");
  V::Array values;
  std::string previous;
  for (const auto& v : b.values) {
    require(v && v->name > previous, "null, duplicate or unordered BB evidence");
    previous = v->name;
    values.push_back(bb_value(*v));
  }
  return V(V::Object{{"scope", bb_scope_value(b.scope)},
                     {"source_id", idv(b.source_id)},
                     {"source_epoch", idv(b.incarnation)},
                     {"message_sequence", count(b.message_sequence)},
                     {"received_at_ns", time(b.trusted_received_at_ns)},
                     {"values", V(std::move(values))}});
}
IoBlackboardTripEvidenceDTO bb_trip_from(const V& v) {
  shape(v, {"scope", "source_id", "source_epoch", "message_sequence", "received_at_ns", "values"});
  IoBlackboardTripEvidenceDTO b{bb_scope_from(get(v, "scope")),
                                id<InterruptSourceId>(get(v, "source_id")),
                                id<IoSourceIncarnation>(get(v, "source_epoch")),
                                counter(get(v, "message_sequence")),
                                stamp(get(v, "received_at_ns")),
                                {}};
  for (const auto& x : array(get(v, "values")))
    b.values.push_back(std::make_shared<const IoBlackboardValueDTO>(bb_value_from(x)));
  (void)bb_trip_value(b);
  return b;
}
V bb_loss_value(const IoBlackboardSourceLossEvidenceDTO& b) {
  return V(V::Object{
      {"scope", bb_scope_value(b.scope)},
      {"source_id", idv(b.source_id)},
      {"source_epoch", idv(b.incarnation)},
      {"last_accepted_message_sequence", count(b.last_accepted_message_sequence, false)},
      {"unavailable_key", opt(b.unavailable_key, [](const auto& x) { return V(text(V(x))); })}});
}
IoBlackboardSourceLossEvidenceDTO bb_loss_from(const V& v) {
  shape(v, {"scope", "source_id", "source_epoch", "last_accepted_message_sequence",
            "unavailable_key"});
  return {bb_scope_from(get(v, "scope")), id<InterruptSourceId>(get(v, "source_id")),
          id<IoSourceIncarnation>(get(v, "source_epoch")),
          counter(get(v, "last_accepted_message_sequence"), false),
          optional<std::string>(get(v, "unavailable_key"), [](const auto& x) { return text(x); })};
}
V barrier_value(const IoIngressLossBarrierDTO& b) {
  require(b.first_received_at_ns <= b.last_received_at_ns, "reversed ingress barrier times");
  return V(V::Object{
      {"source_id", idv(b.source_id)},
      {"source_epoch", idv(b.incarnation)},
      {"first_received_at_ns", time(b.first_received_at_ns)},
      {"last_received_at_ns", time(b.last_received_at_ns)},
      {"after_ingress_source_sequence", count(b.after_ingress_source_sequence, false)},
      {"middleware_message_lost", V(b.middleware_message_lost)},
      {"host_ingress_rejected", V(b.host_ingress_rejected)},
      {"sequence_exhausted", V(b.sequence_exhausted)},
      {"disconnected", V(b.disconnected)},
      {"coalesced", V(b.coalesced)},
      {"middleware_reported_lost_messages",
       opt(b.middleware_reported_lost_messages, [](auto x) { return count(x, false); })}});
}
IoIngressLossBarrierDTO barrier_from(const V& v) {
  shape(v,
        {"source_id", "source_epoch", "first_received_at_ns", "last_received_at_ns",
         "after_ingress_source_sequence", "middleware_message_lost", "host_ingress_rejected",
         "sequence_exhausted", "disconnected", "coalesced", "middleware_reported_lost_messages"});
  IoIngressLossBarrierDTO b{id<InterruptSourceId>(get(v, "source_id")),
                            id<IoSourceIncarnation>(get(v, "source_epoch")),
                            stamp(get(v, "first_received_at_ns")),
                            stamp(get(v, "last_received_at_ns")),
                            counter(get(v, "after_ingress_source_sequence"), false),
                            boolean(get(v, "middleware_message_lost")),
                            boolean(get(v, "host_ingress_rejected")),
                            boolean(get(v, "sequence_exhausted")),
                            boolean(get(v, "disconnected")),
                            boolean(get(v, "coalesced")),
                            optional<std::uint64_t>(get(v, "middleware_reported_lost_messages"),
                                                    [](const V& x) { return counter(x, false); })};
  (void)barrier_value(b);
  return b;
}
const char* loss_name(IoSourceLossReason k) {
  switch (k) {
    case IoSourceLossReason::kSequenceGap:
      return "sequence_gap";
    case IoSourceLossReason::kDisconnected:
      return "disconnected";
    case IoSourceLossReason::kIncarnationChanged:
      return "incarnation_changed";
    case IoSourceLossReason::kIngressOverflow:
      return "ingress_overflow";
    case IoSourceLossReason::kIngressLoss:
      return "ingress_loss";
  }
  bad("unknown source loss reason");
}
V source_loss_value(const IoSourceLossDTO& l) {
  require(l.first_lost_source_sequence.has_value() == l.last_lost_source_sequence.has_value(),
          "partial source loss range");
  if (l.first_lost_source_sequence)
    require(*l.first_lost_source_sequence <= *l.last_lost_source_sequence,
            "reversed source loss range");
  return V(
      V::Object{{"reason", V(loss_name(l.reason))},
                {"last_accepted_source_sequence", count(l.last_accepted_source_sequence, false)},
                {"first_lost_source_sequence",
                 opt(l.first_lost_source_sequence, [](auto n) { return count(n); })},
                {"last_lost_source_sequence",
                 opt(l.last_lost_source_sequence, [](auto n) { return count(n); })},
                {"error", error(l.error)},
                {"ingress_barrier", opt(l.ingress_barrier, barrier_value)}});
}
IoSourceLossDTO source_loss_from(const V& v) {
  shape(v, {"reason", "last_accepted_source_sequence", "first_lost_source_sequence",
            "last_lost_source_sequence", "error", "ingress_barrier"});
  std::optional<IoSourceLossReason> k;
  for (auto x : {IoSourceLossReason::kSequenceGap, IoSourceLossReason::kDisconnected,
                 IoSourceLossReason::kIncarnationChanged, IoSourceLossReason::kIngressOverflow,
                 IoSourceLossReason::kIngressLoss})
    if (text(get(v, "reason")) == loss_name(x)) k = x;
  require(k.has_value(), "unknown source loss reason");
  IoSourceLossDTO l{*k,
                    counter(get(v, "last_accepted_source_sequence"), false),
                    optional<std::uint64_t>(get(v, "first_lost_source_sequence"),
                                            [](const V& x) { return counter(x); }),
                    optional<std::uint64_t>(get(v, "last_lost_source_sequence"),
                                            [](const V& x) { return counter(x); }),
                    error(get(v, "error")),
                    optional<IoIngressLossBarrierDTO>(get(v, "ingress_barrier"), barrier_from)};
  (void)source_loss_value(l);
  return l;
}
const char* failure_name(IoRuleSourceFailureKind k) {
  switch (k) {
    case IoRuleSourceFailureKind::kContinuityLoss:
      return "continuity_loss";
    case IoRuleSourceFailureKind::kMissing:
      return "missing";
    case IoRuleSourceFailureKind::kStale:
      return "stale";
    case IoRuleSourceFailureKind::kInvalid:
      return "invalid";
    case IoRuleSourceFailureKind::kUnavailable:
      return "unavailable";
  }
  bad("unknown source failure kind");
}
void cursor_registration(const IoSourceCursorDTO& c, const IoRuleRegistrationDTO& r) {
  require(c.owner_epoch == r.owner_epoch && c.catalog_generation == r.catalog_generation,
          "cursor registration mismatch");
}
V trip_value(const IoRuleTripEvidenceDTO& t) {
  V::Array samples, cursors;
  std::set<IoSignalId> names;
  std::set<InterruptSourceId> sources;
  for (const auto& s : t.matched_samples) {
    require(s.publication.validity == IoSampleValidity::kValid && s.publication.value &&
                s.received_at_ns <= t.evaluated_at_ns &&
                names.insert(s.publication.signal_id).second,
            "invalid or duplicate matched sample");
    samples.push_back(sample_value(s));
  }
  for (const auto& c : t.source_cursors) {
    cursor_registration(c, t.registration);
    require(sources.insert(c.source_id).second, "duplicate supporting source");
    cursors.push_back(cursor_value(c));
  }
  std::map<InterruptSourceId, IoSourceIncarnation> incarnations;
  for (const auto& s : t.matched_samples) {
    require(sources.contains(s.publication.source_id), "matched sample has no supporting source");
    const auto [found, inserted] =
        incarnations.emplace(s.publication.source_id, s.publication.incarnation);
    require(inserted || found->second == s.publication.incarnation,
            "matched source contains contradictory incarnations");
  }
  V acquisition(nullptr);
  if (t.triggering_acquisition) {
    const auto& a = *t.triggering_acquisition;
    cursor_registration(a.source_cursor, t.registration);
    require(a.source_id == a.source_cursor.source_id && sources.contains(a.source_id),
            "causal source mismatch");
    const auto c = std::find(t.source_cursors.begin(), t.source_cursors.end(), a.source_cursor);
    require(c != t.source_cursors.end(), "causal cursor is not supporting snapshot");
    if (const auto found = incarnations.find(a.source_id); found != incarnations.end())
      require(found->second == a.incarnation, "causal source incarnation mismatch");
    acquisition = V(V::Object{{"source_id", idv(a.source_id)},
                              {"source_epoch", idv(a.incarnation)},
                              {"source_sequence", count(a.source_sequence)},
                              {"source_cursor", cursor_value(a.source_cursor)}});
  }
  V::Object o{{"registration", registration_value(t.registration)},
              {"matched_samples", V(std::move(samples))},
              {"source_cursors", V(std::move(cursors))},
              {"triggering_acquisition", std::move(acquisition)},
              {"evaluated_at_ns", time(t.evaluated_at_ns)}};
  if (t.blackboard) {
    const auto& b = *t.blackboard;
    require(b.scope.request_owner_epoch == t.registration.owner_epoch &&
                sources.contains(b.source_id) && b.trusted_received_at_ns <= t.evaluated_at_ns,
            "BB trip provenance mismatch");
    if (t.triggering_acquisition && t.triggering_acquisition->source_id == b.source_id)
      require(t.triggering_acquisition->incarnation == b.incarnation &&
                  t.triggering_acquisition->source_sequence == b.message_sequence,
              "BB causal frame mismatch");
    o.emplace_back("blackboard", bb_trip_value(b));
  }
  return V(std::move(o));
}
IoRuleTripEvidenceDTO trip_from(const V& v) {
  shape(v,
        {"registration", "matched_samples", "source_cursors", "triggering_acquisition",
         "evaluated_at_ns"},
        {"blackboard"});
  IoRuleTripEvidenceDTO t{
      registration_from(get(v, "registration")), {}, {}, stamp(get(v, "evaluated_at_ns"))};
  for (const auto& x : array(get(v, "matched_samples")))
    t.matched_samples.push_back(sample_from(x));
  for (const auto& x : array(get(v, "source_cursors"))) t.source_cursors.push_back(cursor_from(x));
  t.triggering_acquisition =
      optional<IoRuleAcquisitionKeyDTO>(get(v, "triggering_acquisition"), [](const V& x) {
        shape(x, {"source_id", "source_epoch", "source_sequence", "source_cursor"});
        return IoRuleAcquisitionKeyDTO{id<InterruptSourceId>(get(x, "source_id")),
                                       id<IoSourceIncarnation>(get(x, "source_epoch")),
                                       counter(get(x, "source_sequence")),
                                       cursor_from(get(x, "source_cursor"))};
      });
  if (const auto* b = v.find_object_member("blackboard")) t.blackboard = bb_trip_from(*b);
  (void)trip_value(t);
  return t;
}
V loss_value(const IoRuleSourceLossEvidenceDTO& l) {
  cursor_registration(l.cursor, l.registration);
  require(l.cursor.source_id == l.source_id, "loss cursor source mismatch");
  if (l.sample)
    require(l.sample->publication.source_id == l.source_id &&
                l.sample->publication.incarnation == l.incarnation &&
                l.sample->received_at_ns <= l.evaluated_at_ns,
            "loss sample provenance mismatch");
  require((l.kind == IoRuleSourceFailureKind::kContinuityLoss) == l.source_loss.has_value(),
          "continuity loss requires its actual source record");
  if (l.source_loss && l.source_loss->ingress_barrier)
    require(l.source_loss->ingress_barrier->source_id == l.source_id &&
                l.source_loss->ingress_barrier->incarnation == l.incarnation,
            "loss barrier provenance mismatch");
  V::Object o{{"registration", registration_value(l.registration)},
              {"source_id", idv(l.source_id)},
              {"source_epoch", idv(l.incarnation)},
              {"cursor", cursor_value(l.cursor)},
              {"kind", V(failure_name(l.kind))},
              {"source_loss", opt(l.source_loss, source_loss_value)},
              {"sample", opt(l.sample, sample_value)},
              {"evaluated_at_ns", time(l.evaluated_at_ns)}};
  if (l.blackboard) {
    const auto& b = *l.blackboard;
    require(b.scope.request_owner_epoch == l.registration.owner_epoch &&
                b.source_id == l.source_id && b.incarnation == l.incarnation,
            "BB loss provenance mismatch");
    if (l.source_loss)
      require(b.last_accepted_message_sequence == l.source_loss->last_accepted_source_sequence,
              "BB source loss last accepted frame mismatch");
    o.emplace_back("blackboard", bb_loss_value(b));
  }
  return V(std::move(o));
}
IoRuleSourceLossEvidenceDTO loss_from(const V& v) {
  shape(v,
        {"registration", "source_id", "source_epoch", "cursor", "kind", "source_loss", "sample",
         "evaluated_at_ns"},
        {"blackboard"});
  std::optional<IoRuleSourceFailureKind> k;
  for (auto x : {IoRuleSourceFailureKind::kContinuityLoss, IoRuleSourceFailureKind::kMissing,
                 IoRuleSourceFailureKind::kStale, IoRuleSourceFailureKind::kInvalid,
                 IoRuleSourceFailureKind::kUnavailable})
    if (text(get(v, "kind")) == failure_name(x)) k = x;
  require(k.has_value(), "unknown failure kind");
  IoRuleSourceLossEvidenceDTO l{registration_from(get(v, "registration")),
                                id<InterruptSourceId>(get(v, "source_id")),
                                id<IoSourceIncarnation>(get(v, "source_epoch")),
                                cursor_from(get(v, "cursor")),
                                *k,
                                optional<IoSourceLossDTO>(get(v, "source_loss"), source_loss_from),
                                optional<IoSignalSampleDTO>(get(v, "sample"), sample_from),
                                stamp(get(v, "evaluated_at_ns"))};
  if (const auto* b = v.find_object_member("blackboard")) l.blackboard = bb_loss_from(*b);
  (void)loss_value(l);
  return l;
}
V acquisition_value(const IoRuleAcquisitionKeyDTO& a) {
  return V(V::Object{{"source_id", idv(a.source_id)},
                     {"source_epoch", idv(a.incarnation)},
                     {"source_sequence", count(a.source_sequence)},
                     {"source_cursor", cursor_value(a.source_cursor)}});
}
IoRuleAcquisitionKeyDTO acquisition_from(const V& v) {
  shape(v, {"source_id", "source_epoch", "source_sequence", "source_cursor"});
  return {id<InterruptSourceId>(get(v, "source_id")),
          id<IoSourceIncarnation>(get(v, "source_epoch")), counter(get(v, "source_sequence")),
          cursor_from(get(v, "source_cursor"))};
}
V context_value(const IoRuleStateContextDTO& c) {
  require(!io_rule_state_context_field_error(c), "invalid global recovery context");
  return V(
      V::Object{{"owner_epoch", idv(c.owner_epoch)},
                {"catalog_generation", count(c.catalog_generation)},
                {"active_publication_generation", number(c.active_publication_generation.value())},
                {"retained_bundle_id", idv(c.retained_bundle_id)},
                {"graph_revision", idv(c.graph_revision)}});
}
IoRuleStateContextDTO context_from(const V& v) {
  shape(v, {"owner_epoch", "catalog_generation", "active_publication_generation",
            "retained_bundle_id", "graph_revision"});
  return {id<RequestOwnerEpoch>(get(v, "owner_epoch")), counter(get(v, "catalog_generation")),
          ActiveBundlePublicationGeneration(safe(get(v, "active_publication_generation"))),
          id<RetainedBundleId>(get(v, "retained_bundle_id")),
          id<GraphRevision>(get(v, "graph_revision"))};
}
V state_value(const IoRuleStateContextDTO& c, const IoRuleStateDTO& r) {
  require(!io_rule_state_field_error(c, r), "invalid global recovery state");
  return V(V::Object{{"rule_id", idv(r.rule_id)},
                     {"state", V(std::string(io_global_rule_state_token(r.state)))},
                     {"triggering_acquisition", opt(r.triggering_acquisition, acquisition_value)}});
}
IoRuleStateDTO state_from(const V& v) {
  shape(v, {"rule_id", "state", "triggering_acquisition"});
  const auto name = text(get(v, "state"));
  require(name == "clear" || name == "tripped" || name == "latched", "invalid global state");
  return {id<InterruptPolicyId>(get(v, "rule_id")),
          name == "clear"     ? IoGlobalRuleState::kClear
          : name == "tripped" ? IoGlobalRuleState::kTripped
                              : IoGlobalRuleState::kLatched,
          optional<IoRuleAcquisitionKeyDTO>(get(v, "triggering_acquisition"), acquisition_from)};
}
V evidence_value(const IoRuleEvidenceDTO& e, const IoRuleRegistrationDTO& r) {
  require(
      e.state == IoRuleRegistrationState::kArmed || e.state == IoRuleRegistrationState::kRetired,
      "invalid registration state");
  if (e.trip) require(e.trip->registration == r, "trip relabels registration");
  V::Array losses;
  std::set<InterruptSourceId> sources;
  for (const auto& loss : e.source_losses) {
    require(loss.registration == r && sources.insert(loss.source_id).second,
            "duplicate or relabeled source loss");
    losses.push_back(loss_value(loss));
  }
  return V(V::Object{{"state", V(e.state == IoRuleRegistrationState::kArmed ? "armed" : "retired")},
                     {"trip", opt(e.trip, trip_value)},
                     {"source_losses", V(std::move(losses))},
                     {"evaluation_error", error(e.evaluation_error)}});
}
IoRuleEvidenceDTO evidence_from(const V& v) {
  shape(v, {"state", "trip", "source_losses", "evaluation_error"});
  const auto state = text(get(v, "state"));
  require(state == "armed" || state == "retired", "invalid registration state");
  IoRuleEvidenceDTO e{
      state == "armed" ? IoRuleRegistrationState::kArmed : IoRuleRegistrationState::kRetired,
      optional<IoRuleTripEvidenceDTO>(get(v, "trip"), trip_from),
      {},
      error(get(v, "evaluation_error"))};
  for (const auto& x : array(get(v, "source_losses"))) e.source_losses.push_back(loss_from(x));
  return e;
}
V recovery_evidence_value(const IoRuleEvidenceDTO& evidence,
                          const IoRuleRegistrationDTO& registration,
                          const IoRuleStateContextDTO& context, TimePoint inactive_at) {
  auto value = evidence_value(evidence, registration);
  if (evidence.trip) {
    require(evidence.trip->evaluated_at_ns <= inactive_at,
            "recovery observation precedes retained trip");
    if (evidence.trip->blackboard)
      require(evidence.trip->blackboard->scope.active_publication_generation ==
                  context.active_publication_generation,
              "BB trip publication mismatch");
  }
  for (const auto& loss : evidence.source_losses) {
    require(loss.evaluated_at_ns <= inactive_at,
            "recovery observation precedes retained source loss");
    if (loss.blackboard)
      require(loss.blackboard->scope.active_publication_generation ==
                  context.active_publication_generation,
              "BB loss publication mismatch");
  }
  return value;
}
V recovery_value(const IoGlobalRuleRecoveryRecordDTO& r) {
  const auto& request = r.intent.request_context;
  const auto& selector = r.intent.selector;
  const auto& c = selector.context;
  const IoRuleScopeDTO scope = IoActivationRuleScopeDTO{c.retained_bundle_id, c.graph_revision};
  const IoRuleRegistrationDTO old{c.owner_epoch, c.catalog_generation,
                                  selector.expected_rule_registration_id, selector.rule_id, scope};
  require(
      request.request_owner_epoch == c.owner_epoch && request.request_id == r.result.request_id &&
          r.result.replay_status == RequestReplayStatusDTO::kAcceptedNew && r.result.accepted &&
          !r.result.error && r.result.context == c && r.result.rule.rule_id == selector.rule_id &&
          r.result.rule.state == IoGlobalRuleState::kClear &&
          r.prior_latched_state.rule_id == selector.rule_id &&
          r.prior_latched_state.state == IoGlobalRuleState::kLatched &&
          r.inactive_observation.value && !*r.inactive_observation.value &&
          r.inactive_observation.evaluated_at_ns >= 0,
      "global recovery authorization tuple mismatch");
  const auto& successor = r.successor_registration;
  require(successor.owner_epoch == c.owner_epoch &&
              successor.catalog_generation == c.catalog_generation &&
              successor.policy_id == selector.rule_id && successor.scope == scope &&
              successor.registration_id != old.registration_id,
          "global recovery successor mismatch");
  // Reuse closed trip/loss/compact-BB validation. The prior identity is the
  // selected actual activation nonce; it is never relabelled as an operation.
  (void)registration_value(old);
  require(r.prior_latched_evidence.trip || !r.prior_latched_evidence.source_losses.empty() ||
              r.prior_latched_evidence.evaluation_error,
          "global recovery latch has no retained factual evidence");
  auto prior = recovery_evidence_value(r.prior_latched_evidence, old, c,
                                       r.inactive_observation.evaluated_at_ns);
  require(r.prior_latched_evidence.trip ? r.prior_latched_state.triggering_acquisition ==
                                              r.prior_latched_evidence.trip->triggering_acquisition
                                        : !r.prior_latched_state.triggering_acquisition,
          "global recovery latch acquisition does not match retained trip");
  std::optional<V> source_loss;
  if (r.source_loss_evidence) {
    const auto& evidence = *r.source_loss_evidence;
    require(!evidence.source_losses.empty(), "recovery source-loss evidence has no actual loss");
    const auto& observer = evidence.source_losses.front().registration;
    require(observer.owner_epoch == c.owner_epoch &&
                observer.catalog_generation == c.catalog_generation &&
                observer.policy_id == selector.rule_id && observer.scope == scope &&
                observer.registration_id != successor.registration_id,
            "recovery source-loss observer mismatch");
    (void)registration_value(observer);
    source_loss =
        recovery_evidence_value(evidence, observer, c, r.inactive_observation.evaluated_at_ns);
  }
  V::Object value{
      {"api_version", V("manyforge.internal.io_global_rule_recovery_record.v2")},
      {"intent",
       V(V::Object{{"request_context",
                    V(V::Object{{"request_id", idv(request.request_id)},
                                {"request_owner_epoch", idv(request.request_owner_epoch)},
                                {"source_label", V(request.source_label)},
                                {"correlation_id", opt(request.correlation_id,
                                                       [](const auto& x) { return V(x); })}})},
                   {"selector",
                    V(V::Object{{"context", context_value(c)},
                                {"rule_id", idv(selector.rule_id)},
                                {"expected_rule_registration_id", count(old.registration_id)}})},
                   {"reason", opt(r.intent.reason, [](const auto& x) { return V(x); })}})},
      {"result", V(V::Object{{"request_id", idv(r.result.request_id)},
                             {"replay_status", V("accepted_new")},
                             {"accepted", V(true)},
                             {"context", context_value(r.result.context)},
                             {"rule", state_value(c, r.result.rule)},
                             {"error", error(r.result.error)}})},
      {"prior_latched_state", state_value(c, r.prior_latched_state)},
      {"prior_latched_evidence", std::move(prior)},
      {"inactive_observation",
       V(V::Object{{"value", V(false)},
                   {"evaluated_at_ns", time(r.inactive_observation.evaluated_at_ns)}})},
      {"successor_registration", registration_value(successor)}};
  if (source_loss) value.emplace_back("source_loss_evidence", std::move(*source_loss));
  return V(std::move(value));
}
IoGlobalRuleRecoveryRecordDTO recovery_from(const V& v) {
  shape(v,
        {"api_version", "intent", "result", "prior_latched_state", "prior_latched_evidence",
         "inactive_observation", "successor_registration"},
        {"source_loss_evidence"});
  require(text(get(v, "api_version")) == "manyforge.internal.io_global_rule_recovery_record.v2",
          "invalid global recovery record version");
  const auto& intent = get(v, "intent");
  shape(intent, {"request_context", "selector", "reason"});
  const auto& request = get(intent, "request_context");
  shape(request, {"request_id", "request_owner_epoch", "source_label", "correlation_id"});
  const auto& selector = get(intent, "selector");
  shape(selector, {"context", "rule_id", "expected_rule_registration_id"});
  const auto& result = get(v, "result");
  shape(result, {"request_id", "replay_status", "accepted", "context", "rule", "error"});
  require(text(get(result, "replay_status")) == "accepted_new", "invalid recovery replay status");
  const auto& observation = get(v, "inactive_observation");
  shape(observation, {"value", "evaluated_at_ns"});
  IoGlobalRuleRecoveryRecordDTO record{
      {{id<RequestId>(get(request, "request_id")),
        id<RequestOwnerEpoch>(get(request, "request_owner_epoch")),
        text(get(request, "source_label"), false),
        optional<std::string>(get(request, "correlation_id"),
                              [](const V& x) { return text(x, false); })},
       {context_from(get(selector, "context")), id<InterruptPolicyId>(get(selector, "rule_id")),
        counter(get(selector, "expected_rule_registration_id"))},
       optional<std::string>(get(intent, "reason"), [](const V& x) { return text(x, false); })},
      {id<RequestId>(get(result, "request_id")), RequestReplayStatusDTO::kAcceptedNew,
       boolean(get(result, "accepted")), context_from(get(result, "context")),
       state_from(get(result, "rule")), error(get(result, "error"))},
      state_from(get(v, "prior_latched_state")),
      evidence_from(get(v, "prior_latched_evidence")),
      {optional<bool>(get(observation, "value"), boolean),
       stamp(get(observation, "evaluated_at_ns"))},
      registration_from(get(v, "successor_registration"))};
  if (const auto* loss = v.find_object_member("source_loss_evidence"))
    record.source_loss_evidence = evidence_from(*loss);
  return record;
}
ErrorDTO recovery_failure(const std::exception& e) {
  return {"io_global_rule_recovery_record_invalid",
          "Global I/O recovery record is invalid",
          {{"reason", e.what()}}};
}
const char* arbitration(IoRuleArbitrationDispositionDTO v) {
  switch (v) {
    case IoRuleArbitrationDispositionDTO::kActive:
      return "active";
    case IoRuleArbitrationDispositionDTO::kQueued:
      return "queued";
    case IoRuleArbitrationDispositionDTO::kSuppressedByActiveMask:
      return "suppressed_by_active_mask";
    case IoRuleArbitrationDispositionDTO::kSuperseded:
      return "superseded";
  }
  bad("unknown arbitration disposition");
}
const char* stop_result(StopCertificateResultDTO v) {
  switch (v) {
    case StopCertificateResultDTO::kSucceededNominal:
      return "succeeded_nominal";
    case StopCertificateResultDTO::kSucceededEscalated:
      return "succeeded_escalated";
    case StopCertificateResultDTO::kPartial:
      return "partial";
    case StopCertificateResultDTO::kFailedUncontained:
      return "failed_uncontained";
  }
  bad("unknown Stop result");
}
const char* cause(MotionExecutionTerminalCause v) {
  switch (v) {
    case MotionExecutionTerminalCause::kSignalTrip:
      return "signal_trip";
    case MotionExecutionTerminalCause::kNoSignalTrip:
      return "no_signal_trip";
    case MotionExecutionTerminalCause::kStopped:
      return "stopped";
    case MotionExecutionTerminalCause::kFailed:
      return "failed";
    case MotionExecutionTerminalCause::kInterruptedFault:
      return "interrupted_fault";
    default:
      bad("unsupported rule terminal cause");
  }
}
V outcome_value(const IoMotionRuleOutcomeDTO& o) {
  require(std::holds_alternative<IoOperationRuleScopeDTO>(o.registration.scope),
          "activation scope is not an operation result");
  require(o.stop_id.has_value() == o.stop_generation.has_value(), "partial Stop identity");
  require(o.stop_certificate_id.has_value() == o.stop_certificate_revision.has_value() &&
              o.stop_certificate_id.has_value() == o.stop_result.has_value() &&
              (!o.stop_result || o.stop_id),
          "partial certificate identity");
  require(o.evidence.state == IoRuleRegistrationState::kArmed ||
              o.evidence.state == IoRuleRegistrationState::kRetired,
          "invalid registration state");
  if (o.evidence.trip)
    require(o.evidence.trip->registration == o.registration, "trip relabels registration");
  V::Array losses;
  std::set<InterruptSourceId> lost;
  for (const auto& l : o.evidence.source_losses) {
    require(l.registration == o.registration && lost.insert(l.source_id).second,
            "duplicate or relabeled source loss");
    losses.push_back(loss_value(l));
  }
  V evidence(V::Object{
      {"state", V(o.evidence.state == IoRuleRegistrationState::kArmed ? "armed" : "retired")},
      {"trip", opt(o.evidence.trip, trip_value)},
      {"source_losses", V(std::move(losses))},
      {"evaluation_error", error(o.evidence.evaluation_error)}});
  V::Object row{
      {"registration", registration_value(o.registration)},
      {"evidence", std::move(evidence)},
      {"stop_id", opt(o.stop_id, [](const auto& x) { return idv(x); })},
      {"stop_generation", opt(o.stop_generation, [](auto x) { return number(x.value()); })},
      {"stop_certificate_id", opt(o.stop_certificate_id, [](const auto& x) { return idv(x); })},
      {"stop_certificate_revision",
       opt(o.stop_certificate_revision, [](auto x) { return number(x.value()); })},
      {"stop_result", opt(o.stop_result, [](auto x) { return V(stop_result(x)); })},
      {"terminal_cause", opt(o.terminal_cause, [](auto x) { return V(cause(x)); })},
      {"error", error(o.error)}};
  if (o.arbitration_disposition)
    row.emplace_back("arbitration_disposition", V(arbitration(*o.arbitration_disposition)));
  return V(std::move(row));
}
IoMotionRuleOutcomeDTO outcome_from(const V& v) {
  shape(v,
        {"registration", "evidence", "stop_id", "stop_generation", "stop_certificate_id",
         "stop_certificate_revision", "stop_result", "terminal_cause", "error"},
        {"arbitration_disposition"});
  const auto& e = get(v, "evidence");
  shape(e, {"state", "trip", "source_losses", "evaluation_error"});
  const auto state = text(get(e, "state"));
  require(state == "armed" || state == "retired", "unknown registration state");
  IoMotionRuleOutcomeDTO o{
      registration_from(get(v, "registration")),
      {state == "armed" ? IoRuleRegistrationState::kArmed : IoRuleRegistrationState::kRetired,
       optional<IoRuleTripEvidenceDTO>(get(e, "trip"), trip_from),
       {},
       error(get(e, "evaluation_error"))}};
  for (const auto& l : array(get(e, "source_losses")))
    o.evidence.source_losses.push_back(loss_from(l));
  o.stop_id = optional<StopId>(get(v, "stop_id"), [](const V& x) { return id<StopId>(x); });
  o.stop_generation = optional<StopGeneration>(get(v, "stop_generation"),
                                               [](const V& x) { return StopGeneration(safe(x)); });
  o.stop_certificate_id = optional<StopCertificateId>(
      get(v, "stop_certificate_id"), [](const V& x) { return id<StopCertificateId>(x); });
  o.stop_certificate_revision = optional<StopCertificateRevision>(
      get(v, "stop_certificate_revision"),
      [](const V& x) { return StopCertificateRevision(safe(x)); });
  if (!get(v, "stop_result").is_null()) {
    bool found = false;
    for (auto x :
         {StopCertificateResultDTO::kSucceededNominal,
          StopCertificateResultDTO::kSucceededEscalated, StopCertificateResultDTO::kPartial,
          StopCertificateResultDTO::kFailedUncontained})
      if (text(get(v, "stop_result")) == stop_result(x)) {
        o.stop_result = x;
        found = true;
      }
    require(found, "unknown Stop result");
  }
  if (!get(v, "terminal_cause").is_null()) {
    bool found = false;
    for (auto x :
         {MotionExecutionTerminalCause::kSignalTrip, MotionExecutionTerminalCause::kNoSignalTrip,
          MotionExecutionTerminalCause::kStopped, MotionExecutionTerminalCause::kFailed,
          MotionExecutionTerminalCause::kInterruptedFault})
      if (text(get(v, "terminal_cause")) == cause(x)) {
        o.terminal_cause = x;
        found = true;
      }
    require(found, "unknown rule terminal cause");
  }
  o.error = error(get(v, "error"));
  if (const auto* a = v.find_object_member("arbitration_disposition")) {
    bool found = false;
    for (auto x :
         {IoRuleArbitrationDispositionDTO::kActive, IoRuleArbitrationDispositionDTO::kQueued,
          IoRuleArbitrationDispositionDTO::kSuppressedByActiveMask,
          IoRuleArbitrationDispositionDTO::kSuperseded})
      if (text(*a) == arbitration(x)) {
        o.arbitration_disposition = x;
        found = true;
      }
    require(found, "unknown arbitration");
  }
  (void)outcome_value(o);
  return o;
}
ErrorDTO failure(const std::exception& e) {
  return {
      "io_rule_outcome_invalid", "I/O rule outcome projection is invalid", {{"reason", e.what()}}};
}
}  // namespace
Result<std::string> EncodeIoRuleOutcomes(std::span<const IoMotionRuleOutcomeDTO> outcomes) {
  try {
    require(!outcomes.empty(), "empty rule extension");
    V::Array rows;
    std::set<std::tuple<RequestOwnerEpoch, std::uint64_t, std::uint64_t>> ids;
    for (const auto& o : outcomes) {
      require(ids.emplace(o.registration.owner_epoch, o.registration.catalog_generation,
                          o.registration.registration_id)
                  .second,
              "duplicate rule registration");
      rows.push_back(outcome_value(o));
    }
    return Result<std::string>::Ok(content_identity::canonical_json(V(std::move(rows))));
  } catch (const std::exception& e) {
    return Result<std::string>::Err(failure(e));
  }
}
Result<std::vector<IoMotionRuleOutcomeDTO>> DecodeIoRuleOutcomes(std::string_view bytes) {
  try {
    content_identity::CanonicalJsonParseLimits limits;
    auto value = content_identity::parse_canonical_json(bytes, limits);
    std::vector<IoMotionRuleOutcomeDTO> rows;
    for (const auto& v : array(value)) rows.push_back(outcome_from(v));
    const auto checked = EncodeIoRuleOutcomes(rows);
    if (!checked.ok()) return Result<std::vector<IoMotionRuleOutcomeDTO>>::Err(checked.error());
    return Result<std::vector<IoMotionRuleOutcomeDTO>>::Ok(std::move(rows));
  } catch (const std::exception& e) {
    return Result<std::vector<IoMotionRuleOutcomeDTO>>::Err(failure(e));
  }
}
Result<void> ValidateIoRuleOutcomeParticipant(
    const OperationId& operation, OperationRevision revision,
    const OperationParticipantId& participant, const ControllableComponentId& component,
    const CommandTargetId& target, const ControllerGoalId& goal,
    ControllerGoalGeneration goal_generation, std::span<const IoMotionRuleOutcomeDTO> outcomes) {
  try {
    for (const auto& o : outcomes) {
      const auto* s = std::get_if<IoOperationRuleScopeDTO>(&o.registration.scope);
      // Control ACKs advance the observation without relabeling execution history.
      // Live admission still checks the exact prepared execution revision.
      require(s && s->operation_id == operation && s->operation_revision.nonzero() &&
                  s->operation_revision <= revision &&
                  s->participant_id == participant &&
                  s->controllable_component_id == component &&
                  s->command_target_id == target &&
                  s->controller_goal_id == goal &&
                  s->controller_goal_generation == goal_generation,
              "rule registration does not match containing operation participant");
    }
    return Result<void>::Ok();
  } catch (const std::exception& e) {
    return Result<void>::Err(failure(e));
  }
}
Result<void> ValidateIoRuleOutcomeParticipant(const OperationId& operation,
                                              OperationRevision revision,
                                              const OperationParticipantStateDTO& p) {
  return ValidateIoRuleOutcomeParticipant(
      operation, revision, p.participant_id, p.controllable_component_id, p.command_target_id,
      p.command_id, p.command_generation, p.io_rule_outcomes);
}
Result<std::string> EncodeIoGlobalRuleRecoveryRecord(const IoGlobalRuleRecoveryRecordDTO& record) {
  try {
    return Result<std::string>::Ok(content_identity::canonical_json(recovery_value(record)));
  } catch (const std::exception& e) {
    return Result<std::string>::Err(recovery_failure(e));
  }
}
Result<IoGlobalRuleRecoveryRecordDTO> DecodeIoGlobalRuleRecoveryRecord(std::string_view bytes) {
  try {
    content_identity::CanonicalJsonParseLimits limits;
    auto record = recovery_from(content_identity::parse_canonical_json(bytes, limits));
    const auto checked = EncodeIoGlobalRuleRecoveryRecord(record);
    if (!checked.ok()) return Result<IoGlobalRuleRecoveryRecordDTO>::Err(checked.error());
    require(checked.value() == bytes, "noncanonical global recovery record");
    return Result<IoGlobalRuleRecoveryRecordDTO>::Ok(std::move(record));
  } catch (const std::exception& e) {
    return Result<IoGlobalRuleRecoveryRecordDTO>::Err(recovery_failure(e));
  }
}
}  // namespace manyforge::core
