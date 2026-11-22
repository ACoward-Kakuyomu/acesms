#include "core/cartridge/RomLoader.h"
#include "CodemastersTestSupport.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
    using namespace smsgg;
    if(argc!=2) return 1;
    const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
    auto check=[](const std::vector<uint8_t>& bytes,LoadOptions options,MapperKind mapper,MapperSelection reason) {
        auto result=RomLoader::fromBytes(bytes,options);
        const auto* image=std::get_if<RomImage>(&result);
        return image && image->options().mapper==mapper && image->metadata().mapperSelection==reason;
    };
    auto codemasters=test::codemastersHeaderFixture();
    std::copy_n("TMR SEGA",8,codemasters.begin()+0x7ff0);codemasters[0x7fff]=0x4c;
    for(const auto mapper:{MapperKind::Sega,MapperKind::RomOnly,MapperKind::Codemasters})
        if(!check(codemasters,{mapper},mapper,MapperSelection::Explicit)) return 1;
    if(!check(codemasters,{MapperKind::Auto},MapperKind::Codemasters,MapperSelection::CodemastersHeader)) return 1;
    if(!check(codemasters,{MapperKind::Sega,16384},MapperKind::Sega,MapperSelection::Explicit)) return 1;
    auto invalid=RomLoader::fromBytes(codemasters,{MapperKind::Auto,16384});
    if(!std::holds_alternative<RomLoadError>(invalid) || std::get<RomLoadError>(invalid)!=RomLoadError::IncompatibleOptions) return 1;
    ++codemasters[0];
    if(!check(codemasters,{MapperKind::Auto},MapperKind::Sega,MapperSelection::DefaultSega)) return 1;
    for(const size_t size:{8192,16384,32768,49152,65536,131072,262144,524288,1048576}) {
        std::vector<uint8_t> bytes(size,0xa5);
        const auto mapper=size==49152?MapperKind::RomOnly:MapperKind::Sega;
        const auto reason=size==49152?MapperSelection::Linear48K:MapperSelection::DefaultSega;
        if(!check(bytes,{MapperKind::Auto},mapper,reason)) return 1;
    }
    auto linear=RomLoader::fromBytes(std::vector<uint8_t>(49152),{MapperKind::Auto,8192});
    if(!std::holds_alternative<RomLoadError>(linear) || std::get<RomLoadError>(linear)!=RomLoadError::IncompatibleOptions) return 1;
    const auto plain=test::codemastersHeaderFixture(65536);
    auto raw=plain;raw.insert(raw.begin(),512,0xa5);
    const LoadOptions options{MapperKind::Auto,8192,CopierHeader::Strip512};
    auto direct=RomLoader::fromBytes(raw,options);
    const auto path=root/"auto.sms";
    {std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(raw.data()),raw.size());if(!file)return 1;}
    auto loaded=RomLoader::load(path,options);
    const auto* a=std::get_if<RomImage>(&direct);const auto* b=std::get_if<RomImage>(&loaded);
    if(!a || !b || a->options().mapper!=MapperKind::Codemasters || b->options().mapper!=MapperKind::Codemasters
        || b->options().sramBytes!=8192 || b->options().copierHeader!=CopierHeader::Strip512
        || b->metadata().mapperSelection!=MapperSelection::CodemastersHeader || a->metadata().crc32!=b->metadata().crc32
        || !std::equal(plain.begin(),plain.end(),b->bytes().begin())) return 1;
    return 0;
}
