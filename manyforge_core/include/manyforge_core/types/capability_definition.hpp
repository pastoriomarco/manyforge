#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_core/types/value_schema_ref.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::core {

inline constexpr std::string_view kCapabilityDefinitionApiVersion =
    "manyforge.capability_definition.v1";
inline constexpr std::string_view kBuiltinCapabilityDefinitionRegistryDigest =
    "sha256:713a06c46c5f5100fd4ee1a729ae06be5db753ed84cdff5bdf4d4ac5a77565e0";

enum class CapabilityScope : std::uint8_t {
  kCellGlobal = 0,
  kControlDomain,
  kTargetBinding,
};

enum class CapabilityProviderBindingKind : std::uint8_t {
  kCapabilityProviderBinding = 0,
  kControlBinding,
};

[[nodiscard]] constexpr std::string_view to_string(CapabilityScope value) noexcept {
  switch (value) {
    case CapabilityScope::kCellGlobal:
      return "cell_global";
    case CapabilityScope::kControlDomain:
      return "control_domain";
    case CapabilityScope::kTargetBinding:
      return "target_binding";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(CapabilityProviderBindingKind value) noexcept {
  switch (value) {
    case CapabilityProviderBindingKind::kCapabilityProviderBinding:
      return "capability_provider_binding";
    case CapabilityProviderBindingKind::kControlBinding:
      return "control_binding";
  }
  return {};
}

/// Exact normalized payload for `manyforge.capability_definition.v1`.
///
/// The API version is fixed by the C++ type and `kCapabilityDefinitionApiVersion`.
/// Absence of `requirement_parameters_schema_ref` means that the capability
/// accepts only an exact empty requirement-parameters object.
struct CapabilityDefinitionDTO {
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  CapabilityScope scope;
  CapabilityProviderBindingKind provider_binding_kind;
  std::optional<ValueSchemaRefDTO> requirement_parameters_schema_ref;

  friend bool operator==(const CapabilityDefinitionDTO&, const CapabilityDefinitionDTO&) = default;
};

/// Allocation-free build-owned projection of one normative capability row.
struct CapabilityDefinitionDescriptor {
  std::string_view capability_id;
  std::string_view capability_definition_revision;
  CapabilityScope scope;
  CapabilityProviderBindingKind provider_binding_kind;
  std::optional<ValueSchemaRefDescriptor> requirement_parameters_schema_ref;

  friend bool operator==(const CapabilityDefinitionDescriptor&,
                         const CapabilityDefinitionDescriptor&) = default;
};

/// Exact spec-240 active registry. Frozen legacy replacement rows are not
/// capability definitions.
inline constexpr auto kBuiltinCapabilityDefinitionRegistry =
    std::to_array<CapabilityDefinitionDescriptor>({
        {"planning.motion.joint_goal",
         "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"planning.motion.pose_goal",
         "sha256:422c7d9025d46e0fc660705a4f05171a235279872f91c820e952cf16792b8148",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"planning.motion.cartesian_path",
         "sha256:be53c54ff0aa662db108113e9273e847e790d1b6f0adbba591d50f2237134a00",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"planning.motion.named_target",
         "sha256:ed009e67ef1c678b31faab8e6e6313ac513d93117b21aa7a499f734823d53130",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"planning.profile.first_feasible",
         "sha256:e0f578ef9af205b3c7c184fb1124ffb4fe3c35874b5e993358b43a2dc43d2674",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"planning.profile.balanced",
         "sha256:14c53dad299a42d6604eac14240d015e22a4bc1adb363fb252111e62a2d2ad90",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"planning.profile.quality_bounded",
         "sha256:7f570672270455207d5fb8eb210cd1dd22a99d7909421cf0dc8036c32e4beffb",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"planning.trajectory.time_parameterized",
         "sha256:1fde55ff59f977844d1e42a115df0e66305953803e7f2830fec4821e3cd36385",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"model.kinematics.fk",
         "sha256:c99e2dc6d89129ca1d1d4a44d561d878b786df610d1ff241f6e09b5b100edb28",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"model.limits.joint",
         "sha256:4212f32bfc7e9b04cb11db023da86b9a8f75531b5d775674d559631d93b65053",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"state.observed.current",
         "sha256:c1ec4dacead10e33176830d634e9b2287e4b96322a54700c1c3910bcba461c02",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"validation.trajectory.non_collision",
         "sha256:3cd04895527038906ddcb9c6ca9fd87f4535eba41e5014b04917c66603f26653",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.reference.resolve",
         "sha256:4f66fab4656567598a11844b3b2572112b5239d9eaca4a538f90e082db7154d9",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.object.query",
         "sha256:59bad10b1c6eea73c493dbba88cf15befe3345d32bbebd0b7c7fce4947531109",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.projection.revision_bound",
         "sha256:a50e902c8b37cdcb4121f4a4d2cd7fdd975e60559d32daab2d1ba3c43094ccc0",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.shape.box",
         "sha256:76e9a3ca351e30e271ec75e3b6de1ea42a718f81cca35cda6bab4ee642e1e12d",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.shape.sphere",
         "sha256:d84d92a0e0d9592ead27340278315b3a691c18a902fec44bc99626f36d9bc8f2",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.shape.cylinder",
         "sha256:4ffc860d1851db9b3a7fd82f5e9bd88eb1eca624dba372852fb2912fab9e34d5",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.shape.mesh",
         "sha256:5ea0a60f5d788c2133419b84f110f0edc033405c7bfd7385086888f09bd2779e",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.edit.add",
         "sha256:9f7fbd7203abe565edfb6468d9e52fa15c028f3636615eb62a8d5e8e0c84fea5",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.edit.upsert",
         "sha256:01668536a12a75af7024ce6dd1248ce5fd18f25f75d1fadfb953b4b7da0ed1f2",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.edit.remove",
         "sha256:c19f72293c0b9f54b24e8f165b27112e3ad8496e1d319f0d20d74b7eee6951bc",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.edit.update_pose",
         "sha256:2289472ab0392c94e512d8bfe65406ef7a7ffafb73b40381bd36f627248de4f3",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.edit.attach_detach",
         "sha256:444c80303025e78b49df614d71c7df35d0f40867cb0901a8bd0117e65d50eed2",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"perception.pose_updates",
         "sha256:5256349a9ce767297f6d74b0fc290ba11f48afd42c7443599d5f623545575a57",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"scene.pose.observe",
         "sha256:7ce48309e3a8f884850dd1285cbd66e19da631fbbfd27d269828fb7048832a57",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"collision.query.pre_execution",
         "sha256:c45cdcea2f79fbef5d1bc95db064078560c7d5d161b72667a5ce2c72a7432eca",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"collision.query.runtime",
         "sha256:3d8a3c9cda7b1a17704dd4a97323304c47bdb8098ac73621278abaecaeb667e7",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"collision.query.detailed",
         "sha256:59cfd352c4a08e315f95c2cb93ada8d2a16a6d2155f83b1c7603e9643088f73b",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"execution.trajectory.execute",
         "sha256:7e5958db0906b84cb4e5c68ab0ef896f74dc0ba49a9ceb4d918b386abaf9f28c",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.feedback.phase_events",
         "sha256:37a4b6d145a96b0336ad03680c3222a63da28168764663cee7f5185fcbd467da",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.feedback.state_snapshots",
         "sha256:bdcd8c70d34d34bba3735e6e1b191cade422d9b50223afed895465e0062be85a",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.stop.soft",
         "sha256:3ed811803c31726c758e227fc52bd57854cc6e7fa09dff0fd117d41800d4f8b9",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.stop.motion.spring_back_hold",
         "sha256:f073dd8dfadd5b5bc1e0725557a45da3cdb4cec657e3b25ac92666c94f29e6f5",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.stop.motion.soft_on_path",
         "sha256:216fe0dbb61a7fb76e702901453f66fa965498813755de7d0512d9c84d57119d",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.stop.motion.hard_deceleration",
         "sha256:6ba753aeeb103943923c7964cba8239661d462498c35a437c1682d0dc1f90e70",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.reset",
         "sha256:24fa527d5e81814f8a395d1d3eeae3c3236c1249b05d84d2cd48abf3f2a27ebb",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.contact.geometric",
         "sha256:63a3d877d78d71a5932cce5c92d2932b740fbba6e88d00ca3b5dd94429e3eb3d",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.contact.geometric_signed_penetration",
         "sha256:b2f0e892d401460c6b43b1c1b8b39a0cad5fcc130ba694c2c5e32d6cc2d8c323",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.contact.force_torque",
         "sha256:7424c3d486546ac3d1b40b9a1bc30d8f3762c19aa6ad603fe5fa1c26f71e3a06",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.contact.controller_reported",
         "sha256:f02bd703eef327d86c671bc3721ca5bf9ac4a1c3416791eff84e16ec8568e108",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.control.admittance",
         "sha256:53267e14deed19c4888e454fdcf56fc9057b21119a3b04f26dfaeb074d8bb5aa",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.runtime_scale.operation_scoped",
         "sha256:4ee00df7b3510c1490c83e247d12b73553ab53e4e6673da53ebea475fb756fad",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"execution.interrupt.resource_scoped",
         "sha256:ea0a625d949b1287de5b1eb0ed1e9d400d927f86c62c00b63c69898bdc58da36",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"execution.interrupt.contact_policy",
         "sha256:91fa92a90ddf807623ae9a42a97c37cd4d9628b1961e572b28a928e0da4dd98d",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"io.gpio.set_output",
         "sha256:b7f9e3ae6ed12dd59fd78193db902c6ad09528372ad0f5e2423f936833cb49f8",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"io.gpio.get_input",
         "sha256:abe7c9eed8b4b76ada84aa770ae3111691dfdd07001f4abd68a38bed56ed3d84",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"io.gpio.wait_for_input",
         "sha256:f5520f864f38d6f658039b8a338da9ef1fec014d70e9e354c5e40935717b4747",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"tool.gripper.command",
         "sha256:a24b841e7c0323815a3576869d5819340bc6a98f1c2ffd5b47131e26698e9392",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"tool.gripper.feedback",
         "sha256:53cebcf32900bf3fab41aeb490a9176d3130ae53c1a289c954f1eb8d07738e23",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kControlBinding,
         std::nullopt},
        {"component.lifecycle.observe",
         "sha256:deb6815e681e8879c172e1a7dcd03a55154bc599efaa35167c9d23f54d6b9104",
         CapabilityScope::kControlDomain, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"component.lifecycle.prepare",
         "sha256:c61d01109243632bd3a4184061863cee480a35f253aabac7719fea634e2defd3",
         CapabilityScope::kControlDomain, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"component.lifecycle.enable",
         "sha256:c0d8704375f520e69f04dccbdbe1f8856bab2c0c185ea59cddc4f34f50cbc1f7",
         CapabilityScope::kControlDomain, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"component.lifecycle.disable",
         "sha256:578cd871bc7248d18f1ccafe2659acb9d9ad35570c17b30758312d65b37fb4d1",
         CapabilityScope::kControlDomain, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"component.lifecycle.recover",
         "sha256:7303e9db6f3c6d94b00ed8c448bb98abe156775f562e122cb8046799a7d41e9a",
         CapabilityScope::kControlDomain, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"control_domain.controller.observe",
         "sha256:8425d09a2a710d802ac39de0690016ab59a12123cb46e7a4301fb55e5a6791de",
         CapabilityScope::kControlDomain, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"control_domain.controller.switch",
         "sha256:4ec9450a9b7c4a4d1900176ae07c8e505f48b02f4ba45ccbd517af367b78fc8e",
         CapabilityScope::kControlDomain, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"control_domain.controller.load_unload",
         "sha256:f705df5d2b421921eb9635c8536b52e587c7eec0f5c0bdcb857689a0b9b6311a",
         CapabilityScope::kControlDomain, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"simulator.control.reset",
         "sha256:7ed06295945087622e96dcd91e390a402898be91e1f94233f5c93daa35dfe23a",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"simulator.control.pause",
         "sha256:155d3628cea7b682bd95c1126776aa7314e751fadf9b383e3e3730894fe7c9d3",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"simulator.control.play",
         "sha256:345e9790ff50444eb6c26ff685093fccd17fb98c9e631b6a4fc7e70c9ea16405",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"simulator.control.step",
         "sha256:9fd7cae735d502dc7876c5e176f8dc134691476940c18a487ba04e27cf23cd33",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"simulator.entity_state.set",
         "sha256:cd72b14a50066a268c347d524169ef26d675bf9ec425e6130b84a841b0c6ac79",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"observability.trace.kernel",
         "sha256:1d198675488dbe64bd222695cb546bc1220b1be1b5304f5bea8a9673fbad38e3",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"observability.trace.scene",
         "sha256:aa079df5821ad6ad0fdfc15f15368ca6682cd17dd1c804998e4e954fe1979180",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"observability.logging.operational",
         "sha256:c7060b1666aad447ee2c64e8cf1d3cfc4f4ca95c7be1214069a12e573ac41879",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"assistant.model.invoke",
         "sha256:f900f43ad7bbc61f49c09295a2a63e5fabe93eb57bc9c5f44c26e3c4eeeac158",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        {"assistant.bridge.request",
         "sha256:221f74e397dcae43f8122e30f78cd1e11923b755abf51e70085bc30257f3def9",
         CapabilityScope::kCellGlobal, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
        // The `endpoint` target of a model-free AI model component.
        {"ai.model.invoke",
         "sha256:c458ea512ad5b71be1a3a9f92908d8a36b2a9a8f5cb3b31556c42e0d468c9270",
         CapabilityScope::kTargetBinding, CapabilityProviderBindingKind::kCapabilityProviderBinding,
         std::nullopt},
    });

[[nodiscard]] constexpr const CapabilityDefinitionDescriptor* find_builtin_capability_definition(
    std::string_view capability_id) noexcept {
  for (const auto& descriptor : kBuiltinCapabilityDefinitionRegistry) {
    if (descriptor.capability_id == capability_id) {
      return &descriptor;
    }
  }
  return nullptr;
}

[[nodiscard]] inline const CapabilityDefinitionDescriptor* find_builtin_capability_definition(
    const CapabilityId& capability_id) noexcept {
  return find_builtin_capability_definition(capability_id.value());
}

[[nodiscard]] inline CapabilityDefinitionDTO materialize_capability_definition(
    const CapabilityDefinitionDescriptor& descriptor) {
  std::optional<ValueSchemaRefDTO> schema_ref;
  if (descriptor.requirement_parameters_schema_ref.has_value()) {
    schema_ref.emplace(
        SchemaId(std::string(descriptor.requirement_parameters_schema_ref->schema_id)),
        SchemaRevision(std::string(descriptor.requirement_parameters_schema_ref->schema_revision)));
  }
  return {
      CapabilityId(std::string(descriptor.capability_id)),
      CapabilityDefinitionRevision(std::string(descriptor.capability_definition_revision)),
      descriptor.scope,
      descriptor.provider_binding_kind,
      std::move(schema_ref),
  };
}

[[nodiscard]] consteval bool capability_definition_ids_are_unique() {
  for (std::size_t left = 0U; left < kBuiltinCapabilityDefinitionRegistry.size(); ++left) {
    for (std::size_t right = left + 1U; right < kBuiltinCapabilityDefinitionRegistry.size();
         ++right) {
      if (kBuiltinCapabilityDefinitionRegistry[left].capability_id ==
          kBuiltinCapabilityDefinitionRegistry[right].capability_id) {
        return false;
      }
    }
  }
  return true;
}

static_assert(capability_definition_ids_are_unique());

}  // namespace manyforge::core
