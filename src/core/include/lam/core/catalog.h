#pragma once
// Game data table keyed by StrId, resolved once at load.
// Indices are stable; find() pointers are valid until the next add().
//   std::uint32_t goblin = catalog.index_of("goblin"_id);   // at load
//   const EnemyDef &def = catalog.at(goblin);               // per tick
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include <lam/core/assert.h>
#include <lam/core/id.h>
namespace lam {
template <class T> class Catalog {
public:
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    // LAM_ASSERT unique id; returns the new index (insertion order).
    std::uint32_t add(StrId id, T value) {
        LAM_ASSERT(index_.find(id.value) == index_.end(), "Catalog::add: duplicate id");
        const auto i = static_cast<std::uint32_t>(items_.size());
        items_.push_back(std::move(value));
        ids_.push_back(id);
        index_.emplace(id.value, i);
        return i;
    }
    // kNone if unknown.
    std::uint32_t index_of(StrId id) const {
        const auto it = index_.find(id.value);
        return it == index_.end() ? kNone : it->second;
    }
    const T &at(std::uint32_t index) const {
        LAM_ASSERT(index < items_.size(), "Catalog::at out of range");
        return items_[index];
    }
    T &at(std::uint32_t index) {
        LAM_ASSERT(index < items_.size(), "Catalog::at out of range");
        return items_[index];
    }
    const T *find(StrId id) const {
        const std::uint32_t i = index_of(id);
        return i == kNone ? nullptr : &items_[i];
    }
    StrId id_at(std::uint32_t index) const { return index < ids_.size() ? ids_[index] : StrId{}; }
    std::size_t size() const { return items_.size(); }
    std::span<const T> items() const { return items_; }
    void clear() {
        items_.clear();
        ids_.clear();
        index_.clear();
    }
private:
    std::vector<T> items_;
    std::vector<StrId> ids_;
    std::unordered_map<std::uint64_t, std::uint32_t> index_;
};
}
