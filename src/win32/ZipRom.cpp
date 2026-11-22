#include "win32/ZipRom.h"

#include <windows.h>
#include <miniz.h>
#include <cstdio>
#include <memory>

namespace smsgg::win32 {
bool isRawRomPath(const std::filesystem::path& path) {
    const auto ext=path.extension();
    return !_wcsicmp(ext.c_str(),L".sms") || !_wcsicmp(ext.c_str(),L".gg") || !_wcsicmp(ext.c_str(),L".sg");
}
bool isZipRomPath(const std::filesystem::path& path) {
    return !_wcsicmp(path.extension().c_str(),L".zip");
}
std::string zipRomError(const std::filesystem::path& path,const std::filesystem::path& entry,std::string_view reason) {
    const auto zip=path.u8string();
    std::string error(zip.begin(),zip.end());
    if(!entry.empty()) {
        const auto name=entry.u8string();
        error+=" / ";
        error.append(name.begin(),name.end());
    }
    error+=": ";
    error+=reason;
    return error;
}

std::variant<ZipRom,std::string> readZipRom(const std::filesystem::path& path) {
    constexpr size_t maxRomBytes=1048576+512; // Largest cartridge plus the optional copier header.
    FILE* stream=nullptr;
    if(_wfopen_s(&stream,path.c_str(),L"rb")!=0) return zipRomError(path,{},"Cannot open ZIP");
    const std::unique_ptr<FILE,decltype(&std::fclose)> file(stream,std::fclose);
    mz_zip_archive archive{};
    const auto failure=[&](const std::filesystem::path& entry) {
        return zipRomError(path,entry,mz_zip_get_error_string(mz_zip_get_last_error(&archive)));
    };
    if(!mz_zip_reader_init_cfile(&archive,file.get(),0,MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY))
        return failure({});
    const std::unique_ptr<mz_zip_archive,decltype(&mz_zip_reader_end)> reader(&archive,mz_zip_reader_end);
    for(mz_uint index=0;index<mz_zip_reader_get_num_files(reader.get());++index) {
        if(mz_zip_reader_is_file_a_directory(reader.get(),index)) continue;
        const auto length=mz_zip_reader_get_filename(reader.get(),index,nullptr,0);
        if(!length) return failure({});
        std::string name(length,'\0');
        mz_zip_reader_get_filename(reader.get(),index,name.data(),length);
        const auto end=name.size()-1;
        const auto isExtension=[&](std::string_view extension) {
            return end>=extension.size() && _strnicmp(name.data()+end-extension.size(),extension.data(),extension.size())==0;
        };
        if(!isExtension(".sms") && !isExtension(".gg") && !isExtension(".sg")) continue;

        mz_zip_archive_file_stat stat{};
        const bool hasStat=mz_zip_reader_file_stat(reader.get(),index,&stat);
        // ZIP language encoding flag, with the original ZIP code page for legacy names.
        const UINT codePage=stat.m_bit_flag & (1u<<11) ? CP_UTF8 : 437;
        const auto chars=static_cast<int>(end);
        const auto wideLength=::MultiByteToWideChar(codePage,0,name.data(),chars,nullptr,0);
        if(!wideLength) return zipRomError(path,{},"Cannot decode ZIP entry name");
        std::wstring wideName(wideLength,L'\0');
        ::MultiByteToWideChar(codePage,0,name.data(),chars,wideName.data(),wideLength);
        ZipRom rom{std::filesystem::path(std::move(wideName)),{}};
        if(!hasStat) return failure(rom.entry);
        if(stat.m_is_encrypted) return zipRomError(path,rom.entry,"Encrypted ZIP ROM is unsupported");
        if(!stat.m_is_supported) return zipRomError(path,rom.entry,"Unsupported ZIP compression or entry features");
        if(stat.m_uncomp_size>maxRomBytes) return zipRomError(path,rom.entry,"ROM size exceeds 1 MiB + 512 bytes");
        if(stat.m_uncomp_size==0) return zipRomError(path,rom.entry,"Empty ROM");
        rom.bytes.resize(static_cast<size_t>(stat.m_uncomp_size));
        if(!mz_zip_reader_extract_to_mem(reader.get(),index,rom.bytes.data(),rom.bytes.size(),0))
            return failure(rom.entry);
        return rom;
    }
    return zipRomError(path,{},"No supported ROM in ZIP");
}
}
