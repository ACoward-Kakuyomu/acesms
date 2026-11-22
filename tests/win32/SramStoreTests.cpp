#include "win32/SramStore.h"
#include "core/cartridge/RomLoader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace {
using namespace smsgg;using namespace smsgg::win32;
RomImage image(size_t size,MapperKind mapper=MapperKind::Sega) {return std::get<RomImage>(RomLoader::fromBytes(std::vector<uint8_t>(32768,0x5a),{mapper,size}));}
std::vector<uint8_t> disk(const std::filesystem::path& path) {std::ifstream f(path,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
bool path(const std::filesystem::path& dir) {
    const auto rom=image(8192);SramStore store(dir/L"\u65e5\u672c\u8a9e.game.sms",rom);
    std::wostringstream suffix;suffix<<std::hex<<std::setfill(L'0')<<std::setw(8)<<rom.metadata().crc32<<L"-32768.sav";
    return store.enabled() && store.path()==dir/(L"\u65e5\u672c\u8a9e.game."+suffix.str());
}
bool load(const std::filesystem::path& dir) {
    for(size_t size:{8192u,16384u,32768u}) {
        const auto rom=image(size);const auto base=dir/std::to_string(size);std::filesystem::create_directories(base);
        SramStore store(base/L"game.sms",rom);std::filesystem::remove(store.path());
        const auto missing=store.load();if(!std::holds_alternative<std::vector<uint8_t>>(missing) || std::get<std::vector<uint8_t>>(missing)!=std::vector<uint8_t>(size)) return false;
        std::vector<uint8_t> data(size);for(size_t n=0;n<size;++n) data[n]=static_cast<uint8_t>(n*37);
        if(writeFileAtomically(store.path(),data)) return false;
        SramStore again(base/L"game.sms",rom);const auto got=again.load();
        if(!std::holds_alternative<std::vector<uint8_t>>(got) || std::get<std::vector<uint8_t>>(got)!=data) return false;
        for(size_t wrong:{size-1,size+1}) {
            if(writeFileAtomically(store.path(),std::vector<uint8_t>(wrong))) return false;
            const auto bad=again.load();if(!std::holds_alternative<FileError>(bad) || std::get<FileError>(bad).operation!=FileOperation::Size) return false;
        }
        HANDLE lock=CreateFileW(store.path().c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if(lock==INVALID_HANDLE_VALUE) return false;
        const auto blocked=store.load();CloseHandle(lock);
        if(!std::holds_alternative<FileError>(blocked) || std::get<FileError>(blocked).code!=ERROR_SHARING_VIOLATION) return false;
    }
    return true;
}
bool save(const std::filesystem::path& dir) {
    const auto rom=image(8192);SramStore store(dir/L"game.sms",rom);std::filesystem::remove(store.path());
    const auto loaded=store.load();if(!std::holds_alternative<std::vector<uint8_t>>(loaded)) return false;
    auto ram=std::get<std::vector<uint8_t>>(loaded);
    if(store.save(ram) || std::filesystem::exists(store.path())) return false;
    ram[0]=0x5a;if(store.save(ram) || disk(store.path())!=ram) return false;
    const auto tmp=std::filesystem::path(store.path().wstring()+L".tmp");
    {std::ofstream f(tmp,std::ios::binary);f<<"existing tmp";}
    if(store.save(ram)) return false;
    const auto lock=CreateFileW(store.path().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(lock==INVALID_HANDLE_VALUE) return false;
    ram[3]=0xa7;const auto failed=store.save(ram);CloseHandle(lock);
    if(!failed || failed->operation!=FileOperation::Replace || disk(store.path())==ram) return false;
    if(store.save(ram) || disk(store.path())!=ram) return false; // Failed save must not advance baseline.
    if(disk(tmp)!=std::vector<uint8_t>{'e','x','i','s','t','i','n','g',' ','t','m','p'}) return false;
    std::filesystem::remove(tmp);
    auto previous=ram;previous[3]=0; // A state restore changes RAM, not the store's disk baseline.
    if(store.save(previous) || disk(store.path())!=previous) return false;
    SramStore fresh(dir/L"game.sms",rom);const auto reopened=fresh.load();
    return std::holds_alternative<std::vector<uint8_t>>(reopened) && std::get<std::vector<uint8_t>>(reopened)==previous;
}
bool disabled(const std::filesystem::path& dir) {
    for(auto mapper:{MapperKind::RomOnly,MapperKind::Sega,MapperKind::Codemasters}) for(size_t size:{0u,8192u}) {
        if(mapper==MapperKind::RomOnly && size) continue;if(mapper==MapperKind::Sega && size) continue;
        SramStore store(dir/L"not-created"/L"game.sms",image(size,mapper));const auto got=store.load();
        if(store.enabled() || !store.path().empty() || !std::holds_alternative<std::vector<uint8_t>>(got) || !std::get<std::vector<uint8_t>>(got).empty() || store.save(std::vector<uint8_t>(size))) return false;
    }
    return !std::filesystem::exists(dir/L"not-created");
}
}
int main(int argc,char** argv) {
    if(argc!=3) return 1;const std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);
    const std::string_view op=argv[1];const bool pass=op=="path"?path(dir):op=="load"?load(dir):op=="save"?save(dir):op=="disabled"&&disabled(dir);
    if(!pass) std::cerr<<"SRAM store failed: "<<op<<'\n';return pass?0:1;
}
