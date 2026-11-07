// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>

#include "common/common_types.h"

namespace Common {

/// One bit per page of an address space, answering "may this page hold a range?" for a set of
/// address ranges kept elsewhere (RangeSet). A clear bit proves the page holds none, so a query
/// that only touches clear pages needs no lookup in the set itself.
///
/// The owner keeps one invariant: every page that holds part of a range has its bit set.
/// - MarkRange is called for every range added to the set.
/// - Removing ranges leaves the bits set (a set bit only means "look it up").
/// - ClearPage is called only after a lookup of the whole page found nothing.
/// - Reset is called only when the set is emptied.
/// Addresses at or past the covered space can't be represented; MarkRange then turns the filter
/// off (every query looks up) until the next Reset.
class PageFilter {
public:
    PageFilter(u32 address_bits, u32 page_bits)
        : page_bits_{page_bits}, num_pages_{u64{1} << (address_bits - page_bits)},
          words_{std::make_unique<std::atomic<u64>[]>(static_cast<size_t>((num_pages_ + 63) / 64))} {
        Reset();
    }

    u32 PageBits() const {
        return page_bits_;
    }

    /// Marks every page that [address, address + size) touches.
    void MarkRange(u64 address, u64 size) {
        if (size == 0) {
            return;
        }
        const u64 first = address >> page_bits_;
        const u64 last = (address + size - 1) >> page_bits_;
        if (last >= num_pages_ || last < first) {
            disabled_.store(true, std::memory_order_relaxed);
            return;
        }
        for (u64 page = first; page <= last;) {
            const u64 word = page / 64;
            const u64 bit = page % 64;
            const u64 count = (std::min)(u64{64} - bit, last - page + 1);
            const u64 mask = count == 64 ? ~u64{0} : ((u64{1} << count) - 1) << bit;
            words_[word].fetch_or(mask, std::memory_order_relaxed);
            page += count;
        }
    }

    /// True when no page that [address, address + size) touches is marked, so the set holds no range
    /// there. False means "unknown": look it up.
    bool ProvesEmpty(u64 address, u64 size) const {
        if (size == 0 || disabled_.load(std::memory_order_relaxed)) {
            return false;
        }
        const u64 first = address >> page_bits_;
        const u64 last = (address + size - 1) >> page_bits_;
        if (last >= num_pages_ || last < first || last - first >= 64) {
            return false;
        }
        for (u64 page = first; page <= last; ++page) {
            if ((words_[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1) {
                return false;
            }
        }
        return true;
    }

    /// Whether [address, address + size) lies within one page that MarkRange could have marked.
    bool IsSinglePage(u64 address, u64 size) const {
        return size != 0 && (address >> page_bits_) == ((address + size - 1) >> page_bits_) &&
               (address >> page_bits_) < num_pages_;
    }

    bool IsPageMarked(u64 address) const {
        const u64 page = address >> page_bits_;
        return page < num_pages_ && ((words_[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1);
    }

    /// Unmarks the page holding address. Only after a lookup of the whole page found no range.
    void ClearPage(u64 address) {
        const u64 page = address >> page_bits_;
        if (page < num_pages_) {
            words_[page / 64].fetch_and(~(u64{1} << (page % 64)), std::memory_order_relaxed);
        }
    }

    /// Unmarks every page and turns the filter back on. Only when the set is emptied.
    void Reset() {
        const size_t count = static_cast<size_t>((num_pages_ + 63) / 64);
        for (size_t i = 0; i < count; ++i) {
            words_[i].store(0, std::memory_order_relaxed);
        }
        disabled_.store(false, std::memory_order_relaxed);
    }

private:
    u32 page_bits_;
    u64 num_pages_;
    std::unique_ptr<std::atomic<u64>[]> words_;
    std::atomic<bool> disabled_{false};
};

} // namespace Common
