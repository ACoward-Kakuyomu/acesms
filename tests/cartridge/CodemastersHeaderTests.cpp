#include "core/cartridge/RomLoader.h"
#include "CodemastersTestSupport.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace smsgg;
std::vector<uint8_t> fixture(size_t size=32768) {
    return test::codemastersHeaderFixture(size);
}
auto header(std::vector<uint8_t> bytes,MapperKind mapper=MapperKind::Sega) {
    auto result=RomLoader::fromBytes(std::move(bytes),{mapper});
    if(!std::holds_alternative<RomImage>(result)) throw std::runtime_error("Invalid test fixture");
    return std::get<RomImage>(result).metadata().codemasters;
}
bool run(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    for(const size_t size:{32768,65536,131072,262144,524288,1048576}) {
        auto bytes=fixture(size);
        // The excluded Sega header has arbitrary bytes, independently of its magic.
        std::fill(bytes.begin()+0x7ff0,bytes.begin()+0x8000,0xff);
        const auto h=header(bytes);
        if(!h || h->pages!=size/16384 || h->checksum!=0x9501+size/16384
            || h->computedChecksum!=h->checksum || h->status!=ChecksumStatus::Match) return false;
        for(const size_t prefix:{0,512}) {
            auto raw=bytes;raw.insert(raw.begin(),prefix,0xa5);
            const LoadOptions options{MapperKind::Sega,0,prefix?CopierHeader::Strip512:CopierHeader::None};
            auto direct=RomLoader::fromBytes(raw,options);
            const auto path=root/"header.sms";
            {std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(raw.data()),raw.size());if(!file)return false;}
            auto loaded=RomLoader::load(path,options);
            const auto* a=std::get_if<RomImage>(&direct);const auto* b=std::get_if<RomImage>(&loaded);
            if(!a || !b || !a->metadata().codemasters || !b->metadata().codemasters
                || b->metadata().codemasters->status!=ChecksumStatus::Match
                || b->metadata().codemasters->computedChecksum!=h->checksum || a->metadata().crc32!=b->metadata().crc32) return false;
        }
    }
    for(const size_t offset:{0,0x7fe6,0x7fe7,0x7fe8,0x7fe9}) {
        auto bytes=fixture();++bytes[offset];const auto h=header(bytes);
        if(!h || h->status!=ChecksumStatus::Mismatch) return false;
    }
    for(const auto [offset,value]:{std::pair{0x7fe0,0},std::pair{0x7fe0,3},std::pair{0x7fe1,0},
        std::pair{0x7fe1,0x32},std::pair{0x7fe2,0x13},std::pair{0x7fe3,0xaa},std::pair{0x7fe4,0x24},
        std::pair{0x7fe5,0x60},std::pair{0x7fea,1},std::pair{0x7fef,1}}) {
        auto bytes=fixture();bytes[offset]=static_cast<uint8_t>(value);
        if(header(bytes)) return false;
    }
    for(const size_t size:{8192,16384,32768}) for(const uint8_t value:{0,0xff})
        if(header(std::vector<uint8_t>(size,value))) return false;
    auto sdsc=fixture();std::copy_n("SDSC",4,sdsc.begin()+0x7fe0);if(header(sdsc)) return false;
    auto nonPower=fixture(49152);if(header(nonPower,MapperKind::RomOnly)) return false;
    auto endian=fixture();std::swap(endian[0x7fe6],endian[0x7fe7]);
    if(header(endian)->status!=ChecksumStatus::Mismatch) return false;
    auto sum=fixture();sum[0]=0x34;sum[1]=0x12;
    const auto h=header(sum);
    return h && h->computedChecksum==0xa737 && h->status==ChecksumStatus::Mismatch;
}
}
int main(int argc,char** argv) {
    if(argc!=2 || !run(argv[1])) {std::cerr<<"Codemasters header test failed\n";return 1;}
    return 0;
}
