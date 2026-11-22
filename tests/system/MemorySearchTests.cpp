#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "core/cheat/MemorySearch.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string_view>

using namespace smsgg;

namespace {
bool captureWorkRam() {
    for (const auto machine : {Machine::Sms, Machine::GameGear, Machine::Sg1000}) {
        const bool sg1000 = machine == Machine::Sg1000;
        const uint8_t program[]{0x3e,0x3c, 0x32,0xff,0xff, // LD ($FFFF),$3C via the mirror.
                                0x3e,0xa7, 0x32,0x01,0xc0, // LD ($C001),$A7.
                                0x3e,0x10, 0xd3,0x3e};     // Disable CPU access to work RAM on SMS/GG.
        std::vector<uint8_t> bytes(8192);
        std::ranges::copy(program, bytes.begin());
        const LoadOptions options{sg1000 ? MapperKind::RomOnly : MapperKind::Sega,
                                  sg1000 ? size_t{0} : size_t{8192}, CopierHeader::None, machine};
        auto image = std::get<RomImage>(RomLoader::fromBytes(std::move(bytes), options));
        auto system = std::make_unique<EmulatorSystem>(std::move(image), VideoStandard::Ntsc, 48000, machine);
        if (!sg1000) system->cartridgeRam().front() = 0xa7;
        for (int instruction = 0; instruction < 2; ++instruction) {
            if (!std::holds_alternative<uint32_t>(system->step())) return false;
        }
        const auto first = system->captureWorkRam();
        for (int instruction = 0; instruction < 4; ++instruction) {
            if (!std::holds_alternative<uint32_t>(system->step())) return false;
        }
        if (system->readMemory(0xc001) != (sg1000 ? 0xa7 : 0xff)) return false;
        const auto before = system->tstates();
        auto second = system->captureWorkRam();
        if (second.size() != (sg1000 ? 1024 : 8192) || first.size() != second.size() ||
            first[1] != 0 || second[1] != 0xa7 || second.back() != 0x3c || system->tstates() != before) return false;
        const auto size = second.size();
        MemorySearch search;
        search.capture(std::move(second));
        if (search.size() != size || search.result(size - 1).address != 0xc000 + size - 1 ||
            !search.filter(SearchCondition::Equal, 0xa7) || search.size() != 1 ||
            search.result(0).address != 0xc001) return false;
        const auto end = static_cast<unsigned>(0xc000 + size - 1);
        if (system->writeWorkRam(end, SearchWidth::Word, 0) ||
            system->writeWorkRam(end + 1, SearchWidth::Byte, 0) ||
            system->writeWorkRam(0xbfff, SearchWidth::Byte, 0) ||
            system->writeWorkRam(0xc000, SearchWidth::Byte, 256) ||
            !system->writeWorkRam(end - 3, SearchWidth::Dword, 0x12345678)) return false;
        const auto patched = system->captureWorkRam();
        const uint8_t expected[]{0x78, 0x56, 0x34, 0x12};
        if (!std::equal(patched.end() - 4, patched.end(), std::begin(expected)) ||
            system->tstates() != before || (!sg1000 && system->cartridgeRam().front() != 0xa7)) return false;
    }
    return true;
}

bool values() {
    if (parseSearchValue("255", SearchWidth::Byte) != 255 || parseSearchValue("0Xff", SearchWidth::Byte) != 255 ||
        parseSearchValue("65535", SearchWidth::Word) != 65535 ||
        parseSearchValue("0xffffffff", SearchWidth::Dword) != 0xffffffffu ||
        parseSearchValue("4294967295", SearchWidth::Dword) != 0xffffffffu ||
        parseSearchValue("256", SearchWidth::Byte) || parseSearchValue("0x10000", SearchWidth::Word)) return false;
    for (const auto invalid : {"", "-1", "+1", "0x", "0xfg", "1a", "1 ", " 1", "4294967296"}) {
        if (parseSearchValue(invalid, SearchWidth::Dword)) return false;
    }

    MemorySearch search;
    std::vector<uint8_t> ram(1024);
    ram[1] = 0x45;
    ram[2] = 0x23;
    ram[3] = 0x01;
    ram[4] = 0x80;
    ram.back() = 0x5a;
    search.reset(SearchWidth::Dword);
    search.capture(std::move(ram));
    if (search.size() != 1021 || search.result(1020).address != 0xc3fc ||
        search.result(1020).current != 0x5a000000u || !search.filter(SearchCondition::Equal, 0x80012345u) ||
        search.size() != 1 || search.result(0).address != 0xc001 || search.result(0).previous) return false;
    search.reset(SearchWidth::Word);
    if (search.size() != 1023 || search.result(1022).address != 0xc3fe ||
        search.result(1022).current != 0x5a00 || search.filter(SearchCondition::Equal, 0x10000) ||
        search.size() != 1023 || !search.filter(SearchCondition::Equal, 0x2345) ||
        search.size() != 1 || search.result(0).address != 0xc001) return false;
    search.reset(SearchWidth::Byte);
    return search.size() == 1024 && search.result(1023).current == 0x5a;
}

bool comparisons() {
    MemorySearch search;
    std::vector<uint8_t> ram(1024);
    ram[1] = 4;
    ram[2] = 8;
    ram[3] = 20;
    search.capture(ram);
    if (search.hasPrevious() || search.filter(SearchCondition::Changed) || search.size() != 1024) return false;
    ram[1] = 5;
    ram[2] = 7;
    search.capture(ram);
    if (!search.hasPrevious() || !search.filter(SearchCondition::Changed) || search.size() != 2 ||
        search.result(0).address != 0xc001 || search.result(0).current != 5 || search.result(0).previous != 4 ||
        !search.filter(SearchCondition::Increased) || search.size() != 1) return false;

    ram[1] = 4;
    ram[2] = 12;
    search.capture(ram);
    if (search.size() != 1 || search.result(0).previous != 5 || search.result(0).current != 4 ||
        !search.filter(SearchCondition::Decreased) || search.size() != 1 ||
        !search.filter(SearchCondition::Increased) || search.size() != 0) return false;
    search.capture(ram);
    if (search.size() != 0) return false;
    search.reset(SearchWidth::Byte);
    if (!search.filter(SearchCondition::Unchanged) || search.size() != 1024 ||
        !search.filter(SearchCondition::Equal, 20) || search.size() != 1 || search.result(0).address != 0xc003) return false;

    search = MemorySearch{};
    search.capture(std::move(ram));
    if(search.hasPrevious() || search.size()!=1024 || !search.filter(SearchCondition::Equal,20)) return false;
    std::vector<uint8_t> applied(1024);
    applied[3]=21;
    search.refresh(applied);
    if(search.size()!=1 || search.result(0).current!=21 || search.hasPrevious()) return false;
    search.capture(std::move(applied));
    return search.filter(SearchCondition::Increased) && search.size()==1 && search.result(0).previous==20;
}
}

int main(int argc, char** argv) {
    const std::string_view operation = argc == 2 ? argv[1] : "";
    const bool pass = operation == "capture" ? captureWorkRam() : operation == "values" ? values()
        : operation == "comparisons" && comparisons();
    if (!pass) std::cerr << "Memory search test failed: " << operation << '\n';
    return pass ? 0 : 1;
}
