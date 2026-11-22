#include "win32/CheatExchange.h"
#include "win32/FileStore.h"

#define PICOJSON_USE_INT64
#include <picojson.h>

#include <algorithm>
#include <utility>

namespace smsgg::win32 {
namespace {
using picojson::array;
using picojson::object;
using picojson::value;
constexpr int64_t exchangeVersion=1;
constexpr size_t maxExchangeBytes = 1024 * 1024;
constexpr unsigned maxExchangeDepth = 16;

bool exceedsNestingLimit(std::string_view text) {
    unsigned depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto ch = text[i];
        if (quoted && ch == '\\') {
            ++i;
        } else if (ch == '"') {
            quoted = !quoted;
        } else if (!quoted) {
            if ((ch == '{' || ch == '[') && ++depth > maxExchangeDepth) return true;
            if ((ch == '}' || ch == ']') && depth != 0) --depth;
        }
    }
    return false;
}

const char* machineKey(Machine machine) {
    switch(machine) {
    case Machine::Sms: return "sms";
    case Machine::GameGear: return "gg";
    case Machine::Sg1000: return "sg1000";
    }
    return "";
}

std::string sha256Key(const RomIdentity& identity) {
    constexpr char digits[]="0123456789abcdef";
    std::string result;
    result.reserve(64);
    for(const auto byte:identity.sha256) {
        result.push_back(digits[byte>>4]);
        result.push_back(digits[byte&15]);
    }
    return result;
}

CheatExchangeError fileFailure(FileError error) {
    constexpr const wchar_t* operations[]{L"Open",L"Size check",L"Read",L"Write",L"Flush",L"Close",L"Replace"};
    return {std::wstring(operations[static_cast<size_t>(error.operation)])+
        L" failed (Windows error "+std::to_wstring(error.code)+L")."};
}

std::optional<uint32_t> unsignedInt(const value& input) {
    if(!input.is<int64_t>()) return {};
    const auto result=input.get<int64_t>();
    if(result<0 || result>UINT32_MAX) return {};
    return static_cast<uint32_t>(result);
}
}

std::variant<std::vector<Cheat>,CheatExchangeError> readCheatExchange(const std::filesystem::path& path,
    const RomIdentity& identity,Machine machine,std::span<const Cheat> draft) {
    std::error_code sizeError;
    const auto size=std::filesystem::file_size(path,sizeError);
    if(sizeError) return CheatExchangeError{L"Cannot read file size (Windows error "+std::to_wstring(sizeError.value())+L")."};
    if(size==0) return CheatExchangeError{L"Invalid JSON file size."};
    if(size>maxExchangeBytes) return CheatExchangeError{L"Cheat JSON exceeds the 1 MiB size limit."};
    auto loaded=readFileExact(path,static_cast<size_t>(size));
    if(const auto* error=std::get_if<FileError>(&loaded)) return fileFailure(*error);
    const auto& bytes=std::get<std::vector<uint8_t>>(loaded);
    std::string_view text(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    if(!::MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0))
        return CheatExchangeError{L"The file is not UTF-8."};
    if(text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    // Bound nesting before picojson recursively parses the input.
    if(exceedsNestingLimit(text)) return CheatExchangeError{L"Cheat JSON exceeds the nesting limit of 16."};
    value root;
    std::string parseError;
    const auto remaining=picojson::parse(root,text.begin(),text.end(),&parseError);
    if(!parseError.empty() || !root.is<object>() ||
        std::any_of(remaining,text.end(),[](char c){return c!=' ' && c!='\t' && c!='\r' && c!='\n';}))
        return CheatExchangeError{L"Invalid cheat JSON."};
    const auto& document=std::as_const(root);
    if(unsignedInt(document.get("version"))!=exchangeVersion)
        return CheatExchangeError{L"Unsupported cheat JSON version."};
    const auto& rom=document.get("rom");
    const auto& target=document.get("machine");
    if(!target.is<std::string>() || target.get<std::string>()!=machineKey(machine))
        return CheatExchangeError{L"The cheat file targets a different machine."};
    if(!rom.is<object>() || unsignedInt(rom.get("size"))!=identity.bytes || !rom.get("sha256").is<std::string>() ||
        !std::ranges::equal(rom.get("sha256").get<std::string>(),sha256Key(identity),[](char input,char expected){
            return (input>='A' && input<='F' ? input+('a'-'A') : input)==expected;
        })) return CheatExchangeError{L"The cheat file targets a different ROM."};

    const auto& codes=document.get("cheats");
    if(!codes.is<array>()) return CheatExchangeError{L"The cheat list is missing or invalid."};
    std::vector<Cheat> merged(draft.begin(),draft.end());
    const auto& entries=codes.get<array>();
    for(size_t index=0;index<entries.size();++index) {
        const auto& entry=entries[index];
        const CheatExchangeError invalid{L"Invalid cheat entry "+std::to_wstring(index+1)+L"."};
        if(!entry.is<object>() || !entry.get("name").is<std::string>() || !entry.get("mode").is<std::string>()) return invalid;
        const auto address=unsignedInt(entry.get("address")), width=unsignedInt(entry.get("width")), data=unsignedInt(entry.get("value"));
        const auto& mode=entry.get("mode").get<std::string>();
        if(!address || !width || *width>4 || !data || (mode!="once" && mode!="always")) return invalid;
        const auto& name=entry.get("name").get<std::string>();
        const auto count=::MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),nullptr,0);
        if(count<1 || count>64) return invalid;
        Cheat cheat{std::wstring(count,L'\0'),*address,static_cast<SearchWidth>(*width),*data,false,mode=="always"};
        if(!::MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),cheat.name.data(),count) ||
            !validCheat(machine,cheat)) return invalid;
        const auto duplicate=[&](const Cheat& other) {
            return cheat.address==other.address && cheat.width==other.width && cheat.value==other.value && cheat.always==other.always;
        };
        if(std::ranges::none_of(merged,duplicate)) merged.push_back(std::move(cheat));
    }
    return merged;
}

std::optional<CheatExchangeError> writeCheatExchange(const std::filesystem::path& path,
    const RomIdentity& identity,Machine machine,std::span<const Cheat> saved) {
    array codes;
    codes.reserve(saved.size());
    for(const auto& cheat:saved) {
        const auto chars=static_cast<int>(cheat.name.size());
        const auto count=::WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,cheat.name.data(),chars,nullptr,0,nullptr,nullptr);
        if(!count) return CheatExchangeError{L"A cheat name cannot be encoded as UTF-8."};
        std::string name(count,'\0');
        if(!::WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,cheat.name.data(),chars,name.data(),count,nullptr,nullptr))
            return CheatExchangeError{L"A cheat name cannot be encoded as UTF-8."};
        codes.emplace_back(object{{"name",value(std::move(name))},{"address",value(int64_t{cheat.address})},
            {"width",value(static_cast<int64_t>(cheat.width))},{"value",value(int64_t{cheat.value})},
            {"mode",value(cheat.always?"always":"once")}});
    }
    const auto text=value(object{{"version",value(exchangeVersion)},{"machine",value(machineKey(machine))},
        {"rom",value(object{{"sha256",value(sha256Key(identity))},{"size",value(int64_t{identity.bytes})}})},
        {"cheats",value(std::move(codes))}}).serialize()+"\n";
    if(text.size()>maxExchangeBytes) return CheatExchangeError{L"Cheat JSON exceeds the 1 MiB size limit."};
    if(auto error=writeFileAtomically(path,{reinterpret_cast<const uint8_t*>(text.data()),text.size()})) return fileFailure(*error);
    return {};
}

} // namespace smsgg::win32
