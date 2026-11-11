#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::core {

/// An insertion-ordered ledger with an index on one key.
///
/// Entries live in fixed slots reserved once (`reserve`) and never move: a
/// doubly linked list over the slots keeps the order, oldest first, and a
/// multiset of slot numbers ordered by each entry's own key is the index.
/// Freed slots and index nodes are reused, so adding, finding, evicting the
/// oldest entry and erasing one entry are O(log n) and allocate nothing while
/// the ledger holds at most the reserved count. Several entries may share a
/// key: `find` returns the oldest of them, exactly what a front-to-back scan
/// returns. `Index` provides `using Key` and
/// `static const Key& key(const Entry&)`; an entry's key never changes while
/// it is in the ledger (assign only fields outside it, or the same key); an
/// entry may be moved from right before it is erased.
template <typename Entry, typename Index>
class IndexedLedger {
 public:
  using Key = typename Index::Key;

 private:
  using Slot = std::uint32_t;
  static constexpr Slot kNone = std::numeric_limits<Slot>::max();
  // Orders slot numbers by their entries' keys. A key looked up from outside
  // is compared directly: the index never copies a key.
  struct KeyOrder {
    using is_transparent = void;
    const IndexedLedger* owner;
    const Key& key(Slot slot) const { return Index::key(*owner->slots_[slot].entry); }
    bool operator()(Slot left, Slot right) const { return key(left) < key(right); }
    bool operator()(Slot left, const Key& right) const { return key(left) < right; }
    bool operator()(const Key& left, Slot right) const { return left < key(right); }
  };
  using IndexSet = std::multiset<Slot, KeyOrder>;
  struct Row {
    std::optional<Entry> entry;
    Slot previous{kNone};
    Slot next{kNone};
    // This entry's index node, so erasing never reads the key again.
    typename IndexSet::iterator position{};
  };

 public:
  template <bool Const>
  class Iterator {
    using Owner = std::conditional_t<Const, const IndexedLedger, IndexedLedger>;

   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = Entry;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<Const, const Entry*, Entry*>;
    using reference = std::conditional_t<Const, const Entry&, Entry&>;

    Iterator() = default;
    Iterator(Owner* owner, Slot slot) : owner_(owner), slot_(slot) {}
    operator Iterator<true>() const { return {owner_, slot_}; }  // NOLINT

    reference operator*() const { return *owner_->slots_[slot_].entry; }
    pointer operator->() const { return &*owner_->slots_[slot_].entry; }
    Iterator& operator++() {
      slot_ = owner_->slots_[slot_].next;
      return *this;
    }
    Iterator operator++(int) {
      auto previous = *this;
      ++*this;
      return previous;
    }
    Iterator& operator--() {
      slot_ = slot_ == kNone ? owner_->tail_ : owner_->slots_[slot_].previous;
      return *this;
    }
    Iterator operator--(int) {
      auto previous = *this;
      --*this;
      return previous;
    }
    friend bool operator==(const Iterator& left, const Iterator& right) {
      return left.slot_ == right.slot_;
    }

   private:
    friend class IndexedLedger;
    Owner* owner_{};
    Slot slot_{kNone};
  };
  using iterator = Iterator<false>;
  using const_iterator = Iterator<true>;

  IndexedLedger() = default;
  IndexedLedger(const IndexedLedger&) = delete;
  IndexedLedger& operator=(const IndexedLedger&) = delete;

  /// Allocates the slots and index nodes for `capacity` entries, once.
  void reserve(std::size_t capacity) {
    if (capacity > slots_.size()) grow(capacity);
  }

  [[nodiscard]] iterator begin() noexcept { return {this, head_}; }
  [[nodiscard]] iterator end() noexcept { return {this, kNone}; }
  [[nodiscard]] const_iterator begin() const noexcept { return {this, head_}; }
  [[nodiscard]] const_iterator end() const noexcept { return {this, kNone}; }
  [[nodiscard]] auto rbegin() const noexcept { return std::reverse_iterator(end()); }
  [[nodiscard]] auto rend() const noexcept { return std::reverse_iterator(begin()); }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
  [[nodiscard]] Entry& back() noexcept { return *slots_[tail_].entry; }

  /// The oldest entry with `key`, or end().
  [[nodiscard]] iterator find(const Key& key) { return {this, find_slot(key)}; }
  [[nodiscard]] const_iterator find(const Key& key) const { return {this, find_slot(key)}; }
  [[nodiscard]] bool contains(const Key& key) const { return find_slot(key) != kNone; }

  /// The oldest entry with `key` that satisfies `predicate`, or end().
  template <typename Predicate>
  [[nodiscard]] iterator find_if(const Key& key, Predicate predicate) {
    return {this, find_slot_if(key, predicate)};
  }
  template <typename Predicate>
  [[nodiscard]] const_iterator find_if(const Key& key, Predicate predicate) const {
    return {this, find_slot_if(key, predicate)};
  }

  Entry& push_back(Entry entry) {
    // Beyond the reserved count (no caller reaches it) the ledger grows.
    if (free_ == kNone) grow(slots_.size() == 0U ? 1U : slots_.size() * 2U);
    const Slot slot = free_;
    auto& row = slots_[slot];
    free_ = row.next;
    row.entry.emplace(std::move(entry));
    row.previous = tail_;
    row.next = kNone;
    (tail_ == kNone ? head_ : slots_[tail_].next) = slot;
    tail_ = slot;
    ++size_;
    // A multiset inserts an equal key after its range: each range stays in
    // insertion order.
    auto node = std::move(nodes_.back());
    nodes_.pop_back();
    node.value() = slot;
    row.position = index_.insert(std::move(node));
    return *row.entry;
  }

  /// Erasing any one entry is O(1) and moves no other entry. The entry may
  /// already have been moved from.
  iterator erase(const_iterator entry) {
    const Slot slot = entry.slot_;
    auto& current = slots_[slot];
    nodes_.push_back(index_.extract(current.position));
    const Slot next = current.next;
    (current.previous == kNone ? head_ : slots_[current.previous].next) = next;
    (next == kNone ? tail_ : slots_[next].previous) = current.previous;
    current.entry.reset();
    current.next = free_;
    free_ = slot;
    --size_;
    return {this, next};
  }

  void pop_front() { erase(begin()); }

  /// Removes every entry that satisfies `predicate`, keeping the others'
  /// order: one pass, O(n) predicate calls, no entry moved.
  template <typename Predicate>
  void erase_if(Predicate predicate) {
    for (auto entry = begin(); entry != end();)
      entry = predicate(*entry) ? erase(entry) : std::next(entry);
  }

  void clear() {
    while (!empty()) pop_front();
  }

 private:
  [[nodiscard]] Slot find_slot(const Key& key) const {
    const auto found = index_.lower_bound(key);
    return found == index_.end() || key < KeyOrder{this}.key(*found) ? kNone : *found;
  }
  template <typename Predicate>
  [[nodiscard]] Slot find_slot_if(const Key& key, Predicate& predicate) const {
    for (auto [row, last] = index_.equal_range(key); row != last; ++row)
      if (predicate(*slots_[*row].entry)) return *row;
    return kNone;
  }

  void grow(std::size_t capacity) {
    // Slots are addressed by number, so growing moves no live entry's
    // identity; the index nodes are created here and then only reused.
    const auto first = static_cast<Slot>(slots_.size());
    slots_.resize(capacity);
    for (auto slot = static_cast<Slot>(capacity); slot-- > first;) {
      slots_[slot].next = free_;
      free_ = slot;
    }
    std::multiset<Slot> created;
    for (Slot slot = first; slot < capacity; ++slot) created.insert(created.end(), slot);
    nodes_.reserve(capacity);
    while (!created.empty()) nodes_.push_back(created.extract(created.begin()));
  }

  std::vector<Row> slots_;
  IndexSet index_{KeyOrder{this}};
  std::vector<typename IndexSet::node_type> nodes_;
  Slot head_{kNone};
  Slot tail_{kNone};
  Slot free_{kNone};
  std::size_t size_{};
};

}  // namespace manyforge::core
