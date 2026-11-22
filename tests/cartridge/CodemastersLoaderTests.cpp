#include "core/cartridge/RomLoader.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
    using namespace smsgg;
    if(argc!=2) return 1;
    const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
    for(const size_t size:{0,1,8192,16384,32767,32768,32769,49152,65536,131072,262144,524288,1048576,1048577}) {
        const bool geometry=size>=32768 && size<=1048576 && (size&(size-1))==0;
        for(const size_t ram:{0,8192,16384,32768}) for(const size_t prefix:{0,512}) {
            std::vector<uint8_t> bytes(size+prefix,0xa5);
            if(size) bytes.back()=0x76;
            const LoadOptions options{MapperKind::Codemasters,ram,prefix?CopierHeader::Strip512:CopierHeader::None};
            auto result=RomLoader::fromBytes(bytes,options);
            const bool accepted=geometry && ram<=8192;
            if(accepted) {
                const auto* image=std::get_if<RomImage>(&result);
                if(!image || image->bytes().size()!=size || image->bytes().back()!=0x76
                    || image->options().mapper!=MapperKind::Codemasters || image->options().sramBytes!=ram) return 1;
                const auto path=root/"codemasters.sms";
                {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!out) return 1;}
                auto loaded=RomLoader::load(path,options);
                const auto* file=std::get_if<RomImage>(&loaded);
                if(!file || file->metadata().crc32!=image->metadata().crc32 || file->bytes().size()!=size
                    || !std::equal(file->bytes().begin(),file->bytes().end(),image->bytes().begin())) return 1;
            } else {
                const auto expected=bytes.empty()?RomLoadError::EmptyInput:ram>8192?RomLoadError::IncompatibleOptions
                    :prefix && size==0?RomLoadError::InvalidCopierHeader:RomLoadError::UnsupportedRomSize;
                if(!std::holds_alternative<RomLoadError>(result) || std::get<RomLoadError>(result)!=expected) {
                    std::cerr<<"size="<<size<<" ram="<<ram<<" prefix="<<prefix<<'\n';return 1;
                }
            }
        }
    }
    for(const size_t raw:{1,511,512}) {
        auto result=RomLoader::fromBytes(std::vector<uint8_t>(raw),{MapperKind::Codemasters,0,CopierHeader::Strip512});
        if(!std::holds_alternative<RomLoadError>(result) || std::get<RomLoadError>(result)!=RomLoadError::InvalidCopierHeader) return 1;
    }
    return 0;
}
