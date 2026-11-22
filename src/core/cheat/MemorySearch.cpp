#include "core/cheat/MemorySearch.h"

#include <charconv>
#include <numeric>
#include <span>
#include <utility>

namespace smsgg {
namespace {
bool fits(uint32_t value, SearchWidth width) {
    return value < (uint64_t{1} << (8 * static_cast<unsigned>(width)));
}

uint32_t readValue(std::span<const uint8_t> ram, size_t offset, SearchWidth width) {
    uint32_t value = 0;
    for (unsigned byte = 0; byte < static_cast<unsigned>(width); ++byte) {
        value |= uint32_t{ram[offset + byte]} << (8 * byte);
    }
    return value;
}
}

std::optional<uint32_t> parseSearchValue(std::string_view text, SearchWidth width) {
    const bool hex = text.starts_with("0x") || text.starts_with("0X");
    if (hex) text.remove_prefix(2);
    if (text.empty()) return std::nullopt;
    uint32_t value{};
    const auto end = text.data() + text.size();
    const auto [parsed, error] = std::from_chars(text.data(), end, value, hex ? 16 : 10);
    if (error != std::errc{} || parsed != end || !fits(value, width)) return std::nullopt;
    return value;
}

bool validWorkRamWrite(Machine machine, unsigned address, SearchWidth width, uint32_t value) {
    if (width != SearchWidth::Byte && width != SearchWidth::Word && width != SearchWidth::Dword) return false;
    return address >= workRamBase && address - workRamBase <= workRamBytes(machine) - static_cast<unsigned>(width)
        && fits(value, width);
}

void MemorySearch::capture(std::vector<uint8_t> ram) {
    refreshed_.reset();
    if (current_.size() != ram.size()) {
        current_ = std::move(ram);
        previous_.clear();
        reset(width_);
        return;
    }
    previous_ = std::move(current_);
    current_ = std::move(ram);
}

void MemorySearch::refresh(std::vector<uint8_t> ram) {
    if(ram.size()==current_.size()) refreshed_=std::move(ram);
}

void MemorySearch::reset(SearchWidth width) {
    width_ = width;
    const auto bytes = static_cast<size_t>(width);
    candidates_.resize(current_.size() < bytes ? 0 : current_.size() - bytes + 1);
    std::iota(candidates_.begin(), candidates_.end(), uint16_t{0});
}

bool MemorySearch::filter(SearchCondition condition, uint32_t value) {
    if (condition == SearchCondition::Equal ? !fits(value, width_) : !hasPrevious()) return false;
    const auto& displayed=refreshed_?*refreshed_:current_;
    std::erase_if(candidates_, [&](uint16_t offset) {
        const auto current = readValue(displayed, offset, width_);
        const auto reference = condition == SearchCondition::Equal ? value : readValue(previous_, offset, width_);
        switch (condition) {
        case SearchCondition::Equal:
        case SearchCondition::Unchanged: return current != reference;
        case SearchCondition::Changed: return current == reference;
        case SearchCondition::Increased: return current <= reference;
        case SearchCondition::Decreased: return current >= reference;
        }
        return false;
    });
    return true;
}

SearchResult MemorySearch::result(size_t index) const {
    const auto offset = candidates_[index];
    const auto& displayed=refreshed_?*refreshed_:current_;
    return {static_cast<uint16_t>(workRamBase + offset), readValue(displayed, offset, width_),
        hasPrevious() ? std::optional{readValue(previous_, offset, width_)} : std::nullopt};
}

} // namespace smsgg
