// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "common/container/unordered_map.h"

namespace VideoCommon {

/// Lookup with a reverse index of the views each entry uses. Retiring a view visits only its
/// dependants, rather than scanning all cached descriptors/framebuffers for each deleted image.
/// The owner serializes access, and removal callbacks must not insert into this cache.
template <typename Key, typename Value, typename Dependency, size_t InlineDependencies>
class DependencyCache {
public:
    [[nodiscard]] Value Find(const Key& key) const {
        const auto it = entries.find(key);
        return it != entries.end() ? it->second.value : Value{};
    }

    /// The cached value, or null when the key isn't cached. A cached value may itself be the
    /// null ID (the texture cache remembers descriptors whose image couldn't be found).
    [[nodiscard]] const Value* TryFind(const Key& key) const {
        const auto it = entries.find(key);
        return it != entries.end() ? &it->second.value : nullptr;
    }

    /// Insert an absent key. Repeated dependencies are indexed only once.
    void Insert(const Key& key, Value value, std::span<const Dependency> dependencies) {
        const auto [it, inserted] = entries.try_emplace(key);
        ASSERT(inserted);
        if (!inserted) {
            return;
        }
        Entry& entry = it->second;
        entry.value = value;
        for (const Dependency dependency : dependencies) {
            if (!dependency || std::ranges::find(entry.dependencies, dependency,
                                                 &Reference::dependency) !=
                                   entry.dependencies.end()) {
                continue;
            }
            auto& keys = dependants[dependency];
            entry.dependencies.push_back({dependency, keys.size()});
            keys.push_back(key);
        }
    }

    /// Erase each matching entry exactly once, including when it uses several removed views.
    template <typename OnErase>
    void EraseDependencies(std::span<const Dependency> dependencies, OnErase&& on_erase) {
        for (const Dependency dependency : dependencies) {
            for (;;) {
                const auto reverse = dependants.find(dependency);
                if (reverse == dependants.end()) {
                    break;
                }
                // Erasing the entry also changes (and may erase) this reverse list.
                const Key key = reverse->second.back();
                const auto it = entries.find(key);
                ASSERT(it != entries.end());
                const Value value = it->second.value;
                for (const Reference used : it->second.dependencies) {
                    const auto refs = dependants.find(used.dependency);
                    ASSERT(refs != dependants.end());
                    auto& keys = refs->second;
                    ASSERT(used.index < keys.size() && keys[used.index] == key);
                    // Swap-remove using the recorded position. Searching the list for every
                    // removal would be quadratic when thousands of framebuffers share a view.
                    if (used.index + 1 != keys.size()) {
                        keys[used.index] = std::move(keys.back());
                        auto& moved = entries.find(keys[used.index])->second.dependencies;
                        const auto binding = std::ranges::find(moved, used.dependency,
                                                                &Reference::dependency);
                        ASSERT(binding != moved.end());
                        binding->index = used.index;
                    }
                    keys.pop_back();
                    if (keys.empty()) {
                        dependants.erase(refs);
                    }
                }
                entries.erase(it);
                on_erase(value);
            }
        }
    }

    [[nodiscard]] size_t size() const noexcept {
        return entries.size();
    }

private:
    struct Reference {
        Dependency dependency;
        size_t index;
    };

    struct Entry {
        Value value{};
        boost::container::small_vector<Reference, InlineDependencies> dependencies;
    };

    Common::unordered_map<Key, Entry> entries;
    Common::unordered_map<Dependency, boost::container::small_vector<Key, 1>> dependants;
};

} // namespace VideoCommon
