#pragma once

#include "core/Machine.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace smsgg {

enum class SearchWidth { Byte = 1, Word = 2, Dword = 4 };
enum class SearchCondition { Equal, Unchanged, Changed, Increased, Decreased };

inline constexpr unsigned workRamBase = 0xc000;
std::optional<uint32_t> parseSearchValue(std::string_view text, SearchWidth width);
bool validWorkRamWrite(Machine machine, unsigned address, SearchWidth width, uint32_t value);

struct SearchResult {
    uint16_t address;
    uint32_t current;
    std::optional<uint32_t> previous;
};

class MemorySearch {
public:
    // Capture work RAM from the same session. Start a new search object when switching sessions.
    void capture(std::vector<uint8_t> ram);
    void refresh(std::vector<uint8_t> ram);
    void reset(SearchWidth width);
    bool filter(SearchCondition condition, uint32_t value = 0);

    bool hasPrevious() const { return !previous_.empty(); }
    SearchWidth width() const { return width_; }
    size_t size() const { return candidates_.size(); }
    // index must identify an existing candidate.
    SearchResult result(size_t index) const;

private:
    SearchWidth width_{SearchWidth::Byte};
    std::vector<uint8_t> current_;
    std::vector<uint8_t> previous_;
    std::optional<std::vector<uint8_t>> refreshed_;
    std::vector<uint16_t> candidates_;
};

} // namespace smsgg
