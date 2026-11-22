#pragma once

#include <miniz.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace smsgg::test {
struct ZipEntry {
    const char* name;
    std::string_view bytes;
    unsigned level{MZ_DEFAULT_LEVEL};
};

inline void writeZip(const std::filesystem::path& path,std::initializer_list<ZipEntry> entries) {
    mz_zip_archive archive{};
    if(!mz_zip_writer_init_heap(&archive,0,0)) throw std::runtime_error("ZIP fixture initialization");
    const std::unique_ptr<mz_zip_archive,decltype(&mz_zip_writer_end)> writer(&archive,mz_zip_writer_end);
    for(const auto& entry:entries)
        if(!mz_zip_writer_add_mem(writer.get(),entry.name,entry.bytes.data(),entry.bytes.size(),entry.level))
            throw std::runtime_error("ZIP fixture entry");
    void* buffer=nullptr;
    size_t size{};
    if(!mz_zip_writer_finalize_heap_archive(writer.get(),&buffer,&size))
        throw std::runtime_error("ZIP fixture finalization");
    const std::unique_ptr<void,decltype(&mz_free)> data(buffer,mz_free);
    std::ofstream output(path,std::ios::binary);
    if(!output.write(static_cast<const char*>(data.get()),size)) throw std::runtime_error("ZIP fixture write");
}
}
