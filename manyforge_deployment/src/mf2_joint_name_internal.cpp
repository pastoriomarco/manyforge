#include "mf2_joint_name_internal.hpp"

#include <manyforge_deployment/content_identity.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::deployment::detail {
namespace {

using JointIndex =
    std::map<std::string, const UrdfJointIr*, std::less<>>;
using JointChildren =
    std::map<std::string, std::vector<std::string>, std::less<>>;
using PrecedenceEdges =
    std::map<std::string,
             std::set<std::pair<std::string, std::string>>,
             std::less<>>;

[[nodiscard]] bool utf8_bytewise_less(std::string_view left,
                                      std::string_view right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](char lhs, char rhs) {
        return static_cast<unsigned char>(lhs) <
               static_cast<unsigned char>(rhs);
      });
}

struct Utf8BytewiseLess final {
  using is_transparent = void;

  [[nodiscard]] bool operator()(std::string_view left,
                                std::string_view right) const {
    return utf8_bytewise_less(left, right);
  }
};

[[noreturn]] void fail(std::string message) {
  throw Mf2JointOrderingError(std::move(message));
}

}  // namespace

std::vector<std::pair<std::string, std::size_t>>
dependency_ordered_joint_local_ranks(const UrdfDocumentIr& urdf) {
  constexpr auto kNoJoint = std::numeric_limits<std::size_t>::max();

  JointIndex joints;
  JointIndex parent_joint_by_child_link;
  JointChildren children_by_parent_link;
  for (const auto& joint : urdf.joints) {
    if (!joints.emplace(joint.name, &joint).second) {
      fail("mf2 joint ordering received duplicate joint name '" +
           joint.name + "'");
    }
    if (!parent_joint_by_child_link
             .emplace(joint.child_link_name, &joint)
             .second) {
      fail("mf2 joint ordering received a link with multiple parent joints");
    }
    children_by_parent_link[joint.parent_link_name].push_back(joint.name);
  }
  for (auto& [parent, children] : children_by_parent_link) {
    (void)parent;
    std::sort(children.begin(), children.end(), Utf8BytewiseLess{});
  }

  std::vector<const UrdfJointIr*> indexed_joints;
  indexed_joints.reserve(joints.size());
  std::map<std::string, std::size_t, std::less<>> index_by_name;
  for (const auto& [name, joint] : joints) {
    index_by_name.emplace(name, indexed_joints.size());
    indexed_joints.push_back(joint);
  }

  const auto joint_count = indexed_joints.size();
  if (joint_count == 0U) {
    return {};
  }
  std::vector<std::size_t> parent(joint_count, kNoJoint);
  for (std::size_t index = 0U; index < joint_count; ++index) {
    const auto found = parent_joint_by_child_link.find(
        indexed_joints[index]->parent_link_name);
    if (found != parent_joint_by_child_link.end()) {
      parent[index] = index_by_name.at(found->second->name);
    }
  }

  // Build one bounded parent/depth/root index. This replaces the former
  // root-path-per-joint representation, whose reserved storage was
  // quadratic even for a wide tree without mimic dependencies.
  std::vector<unsigned char> visit_state(joint_count, 0U);
  std::vector<std::size_t> depth(joint_count, 0U);
  std::vector<std::size_t> root(joint_count, kNoJoint);
  std::vector<std::size_t> chain;
  chain.reserve(joint_count);
  for (std::size_t start = 0U; start < joint_count; ++start) {
    if (visit_state[start] == 2U) {
      continue;
    }
    chain.clear();
    auto current = start;
    while (current != kNoJoint && visit_state[current] == 0U) {
      visit_state[current] = 1U;
      chain.push_back(current);
      current = parent[current];
    }
    if (current != kNoJoint && visit_state[current] == 1U) {
      fail("mf2 joint ordering encountered a cyclic URDF tree");
    }
    for (auto row = chain.rbegin(); row != chain.rend(); ++row) {
      const auto index = *row;
      if (parent[index] == kNoJoint) {
        depth[index] = 0U;
        root[index] = index;
      } else {
        const auto parent_index = parent[index];
        if (visit_state[parent_index] != 2U ||
            root[parent_index] == kNoJoint) {
          fail("mf2 joint ordering failed to resolve a parent joint");
        }
        if (depth[parent_index] ==
            std::numeric_limits<std::size_t>::max()) {
          fail("mf2 joint ordering depth exceeds the platform limit");
        }
        depth[index] = depth[parent_index] + 1U;
        root[index] = root[parent_index];
      }
      visit_state[index] = 2U;
    }
  }

  const auto mimic_count = static_cast<std::size_t>(std::count_if(
      indexed_joints.begin(), indexed_joints.end(),
      [](const UrdfJointIr* joint) { return joint->mimic.has_value(); }));

  // Binary lifting is only allocated when a mimic dependency needs an LCA.
  // Therefore the common no-mimic case remains linear in tree size (apart
  // from deterministic sibling sorting); the general case is O(J log J)
  // storage/preparation and O(log J) per dependency.
  std::vector<std::vector<std::size_t>> ancestors;
  if (mimic_count != 0U && joint_count != 0U) {
    std::size_t level_count = 0U;
    for (auto maximum_distance = joint_count - 1U;;) {
      ++level_count;
      if (maximum_distance == 0U) {
        break;
      }
      maximum_distance >>= 1U;
    }
    ancestors.assign(level_count,
                     std::vector<std::size_t>(joint_count, kNoJoint));
    ancestors.front() = parent;
    for (std::size_t level = 1U; level < level_count; ++level) {
      for (std::size_t index = 0U; index < joint_count; ++index) {
        const auto halfway = ancestors[level - 1U][index];
        if (halfway != kNoJoint) {
          ancestors[level][index] =
              ancestors[level - 1U][halfway];
        }
      }
    }
  }

  const auto lift = [&](std::size_t index, std::size_t distance) {
    std::size_t level = 0U;
    while (distance != 0U && index != kNoJoint) {
      if ((distance & 1U) != 0U) {
        if (level >= ancestors.size()) {
          fail("mf2 joint ordering lift exceeds its ancestor index");
        }
        index = ancestors[level][index];
      }
      distance >>= 1U;
      ++level;
    }
    return index;
  };

  const auto is_ancestor = [&](std::size_t candidate,
                               std::size_t descendant) {
    return root[candidate] == root[descendant] &&
           depth[candidate] <= depth[descendant] &&
           lift(descendant, depth[descendant] - depth[candidate]) ==
               candidate;
  };

  const auto lowest_common_ancestor =
      [&](std::size_t left, std::size_t right) {
        if (root[left] != root[right]) {
          return kNoJoint;
        }
        if (depth[left] > depth[right]) {
          left = lift(left, depth[left] - depth[right]);
        } else if (depth[right] > depth[left]) {
          right = lift(right, depth[right] - depth[left]);
        }
        if (left == right) {
          return left;
        }
        for (std::size_t level = ancestors.size(); level-- > 0U;) {
          const auto left_up = ancestors[level][left];
          const auto right_up = ancestors[level][right];
          if (left_up != right_up) {
            if (left_up == kNoJoint || right_up == kNoJoint) {
              fail("mf2 joint ordering encountered an invalid "
                   "ancestor index");
            }
            left = left_up;
            right = right_up;
          }
        }
        if (parent[left] == kNoJoint ||
            parent[left] != parent[right]) {
          fail("mf2 joint ordering failed to resolve a lowest "
               "common ancestor");
        }
        return parent[left];
      };

  PrecedenceEdges precedence_by_parent;
  for (const auto& [follower_name, follower] : joints) {
    if (!follower->mimic.has_value()) {
      continue;
    }
    const auto master = joints.find(follower->mimic->joint_name);
    if (master == joints.end()) {
      fail("mf2 joint ordering encountered a mimic with an unknown master");
    }
    const auto master_index = index_by_name.at(master->first);
    const auto follower_index = index_by_name.at(follower_name);
    if (master_index == follower_index) {
      fail("mf2 joint ordering encountered a self-referential mimic");
    }
    if (is_ancestor(master_index, follower_index)) {
      // The master is an ancestor and is necessarily imported first.
      continue;
    }
    if (is_ancestor(follower_index, master_index)) {
      fail("mf2 joint ordering cannot place mimic master '" +
           master->first + "' before its ancestor follower '" +
           follower_name + "'");
    }

    std::size_t master_branch_index = root[master_index];
    std::size_t follower_branch_index = root[follower_index];
    if (root[master_index] == root[follower_index]) {
      const auto common =
          lowest_common_ancestor(master_index, follower_index);
      if (common == kNoJoint) {
        fail("mf2 joint ordering failed to locate a common ancestor");
      }
      master_branch_index =
          lift(master_index, depth[master_index] - depth[common] - 1U);
      follower_branch_index =
          lift(follower_index,
               depth[follower_index] - depth[common] - 1U);
    }
    if (master_branch_index == kNoJoint ||
        follower_branch_index == kNoJoint) {
      fail("mf2 joint ordering failed to project a mimic dependency");
    }
    const auto* master_branch_joint =
        indexed_joints[master_branch_index];
    const auto* follower_branch_joint =
        indexed_joints[follower_branch_index];
    if (master_branch_joint->parent_link_name !=
        follower_branch_joint->parent_link_name) {
      fail("mf2 joint ordering failed to project a mimic dependency "
           "to sibling branches");
    }
    precedence_by_parent[master_branch_joint->parent_link_name].insert(
        {master_branch_joint->name, follower_branch_joint->name});
  }

  std::vector<std::pair<std::string, std::size_t>> result;
  result.reserve(joints.size());
  for (const auto& [parent_link, children] : children_by_parent_link) {
    std::map<std::string, std::size_t, std::less<>> indegree;
    std::map<std::string, std::vector<std::string>, std::less<>> outgoing;
    for (const auto& child : children) {
      indegree.emplace(child, 0U);
    }
    if (const auto projected = precedence_by_parent.find(parent_link);
        projected != precedence_by_parent.end()) {
      for (const auto& [before, after] : projected->second) {
        if (!indegree.contains(before) || !indegree.contains(after)) {
          fail("mf2 joint ordering projected outside one sibling set");
        }
        outgoing[before].push_back(after);
        ++indegree.at(after);
      }
    }
    for (auto& [before, after] : outgoing) {
      (void)before;
      std::sort(after.begin(), after.end(), Utf8BytewiseLess{});
    }

    std::set<std::string, Utf8BytewiseLess> ready;
    for (const auto& [child, count] : indegree) {
      if (count == 0U) {
        ready.insert(child);
      }
    }
    std::size_t rank = 0U;
    while (!ready.empty()) {
      const std::string current = *ready.begin();
      ready.erase(ready.begin());
      if (rank > kMf2JointMaximumLocalRank) {
        fail("mf2 joint sibling rank exceeds the fixed-width grammar");
      }
      result.emplace_back(current, rank++);
      if (const auto found = outgoing.find(current);
          found != outgoing.end()) {
        for (const auto& dependent : found->second) {
          auto& count = indegree.at(dependent);
          if (count == 0U) {
            fail("mf2 joint ordering encountered an invalid precedence "
                 "indegree");
          }
          --count;
          if (count == 0U) {
            ready.insert(dependent);
          }
        }
      }
    }
    if (rank != children.size()) {
      fail("mf2 joint ordering contains cyclic projected sibling "
           "constraints at parent link '" +
           parent_link + "'");
    }
  }
  std::sort(result.begin(), result.end(),
            [](const auto& left, const auto& right) {
              return utf8_bytewise_less(left.first, right.first);
            });
  return result;
}

std::string make_mf2_generated_joint_name(
    std::size_t parent_local_rank,
    const std::vector<std::pair<std::string_view, std::string_view>>& fields) {
  if (parent_local_rank > kMf2JointMaximumLocalRank) {
    fail("mf2 joint sibling rank exceeds the fixed-width grammar");
  }
  std::string preimage{kMf2JointHashPreimageDomain};
  for (const auto& [field, value] : fields) {
    preimage.push_back('\0');
    preimage.append(field);
    preimage.push_back('=');
    preimage.append(std::to_string(value.size()));
    preimage.push_back(':');
    preimage.append(value);
  }
  const auto digest = sha256_digest(preimage);
  if (!digest.starts_with("sha256:") || digest.size() != 71U) {
    throw std::logic_error("internal mf2 joint digest is malformed");
  }
  const auto rank_text = std::to_string(parent_local_rank);
  return "mf2_joint_" +
         std::string(kMf2JointLocalRankWidth - rank_text.size(), '0') +
         rank_text + "_" + digest.substr(7U);
}

}  // namespace manyforge::deployment::detail
