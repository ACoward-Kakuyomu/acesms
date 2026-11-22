#include "win32/CheatExchange.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace smsgg;
using namespace smsgg::win32;

namespace {
void require(bool result,const char* reason) {
    if(!result) throw std::runtime_error(reason);
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    require(bool(file),"read fixture");
    return {std::istreambuf_iterator<char>(file),{}};
}
void write(const std::filesystem::path& path,std::string_view text) {
    std::ofstream file(path,std::ios::binary);
    file.write(text.data(),text.size());
    require(bool(file),"write fixture");
}
void exchangeLimits(const std::filesystem::path& file, const RomIdentity& identity,
    std::string_view valid, const std::vector<Cheat>& expected) {
    constexpr size_t oneMiB = 1024 * 1024;
    auto padded = std::string(valid);
    padded.resize(oneMiB, ' ');
    write(file, padded);
    require(std::get<std::vector<Cheat>>(readCheatExchange(file, identity, Machine::Sg1000, {})) == expected,
        "import rejected 1 MiB at the boundary");
    padded.push_back(' ');
    write(file, padded);
    auto import = [&] { return readCheatExchange(file, identity, Machine::Sg1000, {}); };
    auto error = import();
    const auto* reason = std::get_if<CheatExchangeError>(&error);
    require(reason && reason->message.find(L"size") != std::wstring::npos, "import did not enforce the size limit");

    for (const bool objects : {false, true}) {
        const auto nested = [&](unsigned levels) {
            std::string field = "\"optional\":";
            for (unsigned i = 0; i < levels; ++i) field += objects ? "{\"child\":" : "[";
            field += R"("[{\"}]")";
            field.append(levels, objects ? '}' : ']');
            return "{" + field + "," + std::string(valid.substr(1));
        };
        write(file, nested(15));
        require(std::get<std::vector<Cheat>>(import()) == expected, "import rejected supported nesting");
        write(file, nested(16));
        error = import();
        reason = std::get_if<CheatExchangeError>(&error);
        require(reason && reason->message.find(L"nesting") != std::wstring::npos, "import did not enforce the depth limit");
    }

    write(file, valid);
    const Cheat longName{std::wstring(64, L'\u6b8b'), 0xc000, SearchWidth::Byte, 1, false, false};
    std::vector<Cheat> many(5000, longName);
    for (unsigned i = 0; i < many.size(); ++i) {
        many[i].address += i % 1024;
        many[i].value = i / 1024;
    }
    const auto exportError = writeCheatExchange(file, identity, Machine::Sg1000, many);
    require(exportError && exportError->message.find(L"size") != std::wstring::npos && read(file) == valid,
        "oversized export changed the existing file");
}

void exchange(const std::filesystem::path& root) {
    const auto file=root/L"codes.json", temporary=root/L"codes.json.tmp";
    std::filesystem::remove(temporary);
    RomIdentity identity{};
    identity.bytes=8192;
    identity.sha256.front()=0xab;

    const Cheat original{L"\u6b8b\u6a5f",0xc001,SearchWidth::Word,0x1234,true,true};
    const Cheat another{L"Counter",0xc3ff,SearchWidth::Byte,0xff,true,false};
    const std::vector saved{original,another};
    require(!writeCheatExchange(file,identity,Machine::Sg1000,saved),"export");
    const auto payload=read(file);
    require(payload.find("\"enabled\"")==std::string::npos && payload.find("\"sg1000\"")!=std::string::npos,"exchange contains local state or wrong machine");
    auto expected=saved;expected[0].enabled=expected[1].enabled=false;
    auto result=readCheatExchange(file,identity,Machine::Sg1000,{});
    require(std::get<std::vector<Cheat>>(result)==expected,"round-trip definitions");
    exchangeLimits(file, identity, payload, expected);

    auto local=original;local.name=L"Keep my name";
    result=readCheatExchange(file,identity,Machine::Sg1000,{&local,1});
    require(std::get<std::vector<Cheat>>(result)==std::vector{local,expected[1]},"duplicate was replaced or activated");
    auto different=identity;different.sha256[4]=1;
    require(std::holds_alternative<CheatExchangeError>(readCheatExchange(file,different,Machine::Sg1000,{})),"wrong SHA accepted");
    different=identity;++different.bytes;
    require(std::holds_alternative<CheatExchangeError>(readCheatExchange(file,different,Machine::Sg1000,{})),"wrong size accepted");
    require(std::holds_alternative<CheatExchangeError>(readCheatExchange(file,identity,Machine::Sms,{})),"wrong machine accepted");

    write(temporary,"busy");
    require(!writeCheatExchange(file,identity,Machine::Sg1000,{}) && read(file)!=payload && read(temporary)=="busy",
        "leftover temporary prevented export");
    write(file,payload);
    const auto lock=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    require(lock!=INVALID_HANDLE_VALUE,"lock export destination");
    const auto failed=writeCheatExchange(file,identity,Machine::Sg1000,{});
    CloseHandle(lock);
    require(failed && failed->message.starts_with(L"Replace failed") && read(file)==payload && read(temporary)=="busy",
        "failed replacement changed the original");
    require(!writeCheatExchange(file,identity,Machine::Sg1000,saved) && read(file)==payload,"export retry");
    std::filesystem::remove(temporary);

    const auto invalid=[&](std::string_view token,std::string_view replacement) {
        auto changed=payload;
        const auto at=changed.find(token);
        require(at!=changed.npos,"missing malformed-input test token");
        changed.replace(at,token.size(),replacement);
        write(file,changed);
        require(std::holds_alternative<CheatExchangeError>(readCheatExchange(file,identity,Machine::Sg1000,{&local,1})),
            "malformed import accepted");
    };
    invalid("\"version\":1","\"version\":2");
    invalid("\"width\":2","\"width\":3");
    invalid("\"address\":50175","\"address\":50176");
    invalid("\"value\":255","\"value\":256");
    invalid("\"value\":255","\"value\":2.5");
    invalid("\"mode\":\"once\"","\"mode\":\"other\"");
    invalid("\"Counter\"","\"\"");
    invalid("\"Counter\"","\"\xC2\"");
    write(file,payload+"{}");
    require(std::holds_alternative<CheatExchangeError>(readCheatExchange(file,identity,Machine::Sg1000,{})),"trailing JSON accepted");

    write(file,"{\"cheats\":[]}");
    require(std::holds_alternative<CheatExchangeError>(readCheatExchange(file,identity,Machine::Sg1000,{})),"missing metadata accepted");
    std::filesystem::remove(file);
    require(std::holds_alternative<CheatExchangeError>(readCheatExchange(file,identity,Machine::Sg1000,{})),"missing file accepted");
}
}

int main(int argc,char** argv) {
    try {
        require(argc==2,"arguments");
        const std::filesystem::path root=argv[1];
        std::filesystem::create_directories(root);
        exchange(root);
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
