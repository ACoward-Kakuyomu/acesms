#include "win32/D3D11Presenter.h"
#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "../system/Stage5TestRom.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "PresenterTestSupport.h"

namespace {
struct Window {
    HWND handle = CreateWindowExW(0, L"STATIC", L"Stage 5 GPU test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 300, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Window() { if (!handle) throw std::runtime_error("Test window creation failed"); }
    ~Window() { DestroyWindow(handle); }
};
void saveBmp(const std::filesystem::path& path, const std::vector<smsgg::Rgba8>& pixels, unsigned width, unsigned height) {
    std::filesystem::create_directories(path.parent_path());
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + width * height * 4;
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (const auto p : pixels) {
        const uint8_t bgra[]{p.b, p.g, p.r, p.a};
        out.write(reinterpret_cast<const char*>(bgra), 4);
    }
    if (!out) throw std::runtime_error("BMP write failed");
}
bool same(smsgg::Rgba8 a, smsgg::Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
bool ggCrop(const std::filesystem::path& root) {
    Window window;
    smsgg::win32::D3D11Presenter p(window.handle);
    auto input=std::make_unique<std::array<smsgg::Rgba8,256*192>>();
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;++x)
        (*input)[y*256+x]={static_cast<uint8_t>(x),static_cast<uint8_t>(y),static_cast<uint8_t>(x^y),255};
    struct Case {unsigned w,h,x,y,vw,vh;};
    constexpr Case cases[]{ {160,144,0,0,160,144},{800,600,80,12,640,576},
        {400,300,40,6,320,288},{80,72,0,0,80,72},{1,1,0,0,0,0} };
    for(const auto c:cases) {
        p.uploadFrame(*input,{48,24,160,144});p.resize(c.w,c.h);
        const auto pixels=smsgg::win32::PresenterTestAccess::capture(p,c.w,c.h);
        for(unsigned y=0;y<c.h;++y) for(unsigned x=0;x<c.w;++x) {
            smsgg::Rgba8 expected{};
            if(x>=c.x && x<c.x+c.vw && y>=c.y && y<c.y+c.vh) {
                const auto sx=48+(2*(x-c.x)+1)*160/(2*c.vw);
                const auto sy=24+(2*(y-c.y)+1)*144/(2*c.vh);
                expected=(*input)[sy*256+sx];
            }
            if(!same(pixels[y*c.w+x],expected)) {
                const auto actual=pixels[y*c.w+x];
                std::cerr<<"GG crop mismatch "<<c.w<<'x'<<c.h<<" @"<<x<<','<<y<<" actual="
                    <<unsigned(actual.r)<<','<<unsigned(actual.g)<<','<<unsigned(actual.b)<<" expected="
                    <<unsigned(expected.r)<<','<<unsigned(expected.g)<<','<<unsigned(expected.b)<<'\n';return false;
            }
        }
        if(c.w==800) saveBmp(root/"gg-crop.bmp",pixels,c.w,c.h);
    }
    p.resize(0,0);if(p.present()!=smsgg::win32::PresentResult::Skipped) return false;
    p.uploadFrame(*input);p.resize(256,192); // Default area must restore SMS after GG.
    const auto sms=smsgg::win32::PresenterTestAccess::capture(p,256,192);
    for(size_t i=0;i<sms.size();++i) if(!same(sms[i],(*input)[i])) return false;
    return true;
}
bool pipeline(const std::filesystem::path& root) {
    Window window;
    smsgg::win32::D3D11Presenter p(window.handle);
    smsgg::win32::PresenterTestAccess::describe(p);
    auto input = std::make_unique<std::array<smsgg::Rgba8, 256 * 192>>();
    for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x)
        (*input)[y * 256 + x] = {static_cast<uint8_t>(x), static_cast<uint8_t>(y), static_cast<uint8_t>(x ^ y), 255};
    p.uploadFrame(*input);
    struct Case { unsigned w, h, x, y, vw, vh; };
    constexpr Case cases[]{ {256, 192, 0, 0, 256, 192}, {800, 600, 16, 12, 768, 576},
        {700, 500, 94, 58, 512, 384}, {128, 96, 0, 0, 128, 96}, {255, 191, 0, 0, 254, 191} };
    for (const auto c : cases) {
        p.resize(c.w, c.h);
        const auto actual = smsgg::win32::PresenterTestAccess::capture(p, c.w, c.h);
        for (unsigned y = 0; y < c.h; ++y) for (unsigned x = 0; x < c.w; ++x) {
            smsgg::Rgba8 expected{};
            bool matches = same(actual[y * c.w + x], expected);
            if (x >= c.x && x < c.x + c.vw && y >= c.y && y < c.y + c.vh) {
                const auto nx = (2 * (x - c.x) + 1) * 256, ny = (2 * (y - c.y) + 1) * 192;
                const auto sx = nx / (2 * c.vw), sy = ny / (2 * c.vh);
                expected = (*input)[sy * 256 + sx];
                const bool tieX = sx > 0 && nx % (2 * c.vw) == 0;
                const bool tieY = sy > 0 && ny % (2 * c.vh) == 0;
                matches = false;
                for (unsigned ty = sy - tieY; ty <= sy; ++ty) for (unsigned tx = sx - tieX; tx <= sx; ++tx)
                    matches |= same(actual[y * c.w + x], (*input)[ty * 256 + tx]);
            }
            if (!matches) {
                std::cerr << "GPU pixel mismatch: " << c.w << 'x' << c.h << " at " << x << ',' << y << '\n';
                const auto value = actual[y * c.w + x];
                std::cerr << "actual=" << unsigned(value.r) << ',' << unsigned(value.g) << ',' << unsigned(value.b)
                    << " expected=" << unsigned(expected.r) << ',' << unsigned(expected.g) << ',' << unsigned(expected.b) << '\n';
                return false;
            }
        }
        saveBmp(root / (std::to_string(c.w) + "x" + std::to_string(c.h) + ".bmp"), actual, c.w, c.h);
        const auto result = p.present();
        if (result == smsgg::win32::PresentResult::Occluded
            && p.present() != smsgg::win32::PresentResult::Skipped) return false;
        p.present(true);
    }
    p.resize(0, 0);
    if (p.present() != smsgg::win32::PresentResult::Skipped) return false;
    p.resize(256, 192);
    const auto restored = smsgg::win32::PresenterTestAccess::capture(p, 256, 192);
    for (size_t i = 0; i < restored.size(); ++i) if (!same(restored[i], (*input)[i])) return false;
    return true;
}
bool rom(const std::filesystem::path& root) {
    Window window;
    smsgg::win32::D3D11Presenter p(window.handle);
    auto system = std::make_unique<smsgg::EmulatorSystem>(std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(
        smsgg::test::stage5InputRom(), {smsgg::MapperKind::RomOnly})), smsgg::VideoStandard::Ntsc, 48000);
    while (system->completedFrames() < 3) system->step();
    p.resize(256, 192);
    p.uploadFrame(system->framePixels());
    const auto actual = smsgg::win32::PresenterTestAccess::capture(p, 256, 192);
    for (size_t i = 0; i < actual.size(); ++i) if (!same(actual[i], system->framePixels()[i])) return false;
    saveBmp(root / "rom.bmp", actual, 256, 192);
    return true;
}
bool aspect() {
    using namespace smsgg::win32;
    Window window;
    D3D11Presenter p(window.handle);
    auto input=std::make_unique<std::array<smsgg::Rgba8,256*192>>();
    input->fill({0,255,0,255});
    for(unsigned y=24;y<168;++y)
        std::fill_n(input->begin()+y*256+48,160,smsgg::Rgba8{255,0,0,255});
    p.uploadFrame(*input,{48,24,160,144});
    p.resize(820,600);
    p.setAspectRatio(AspectRatio::FourThree);
    const auto pixels=PresenterTestAccess::capture(p,820,600);
    for(unsigned y=0;y<600;++y) for(unsigned x=0;x<820;++x)
        if(!same(pixels[y*820+x],x>=10 && x<810?smsgg::Rgba8{255,0,0,255}:smsgg::Rgba8{0,0,0,255})) return false;
    return true;
}
bool fpsOverlay() {
    using namespace smsgg::win32;
    Window window;
    D3D11Presenter p(window.handle);
    const smsgg::Rgba8 red{255,0,0,255}, white{255,255,255,255}, black{0,0,0,255};
    auto input=std::make_unique<std::array<smsgg::Rgba8,256*192>>();
    input->fill(red);
    p.uploadFrame(*input);
    p.setFpsText("FPS 123.4");
    p.resize(256,192);
    const auto unscaled=PresenterTestAccess::capture(p,256,192);
    p.resize(640,480);
    p.setAspectRatio(AspectRatio::FourThree);
    const auto large=PresenterTestAccess::capture(p,640,480);
    unsigned letters{}, background{};
    for(unsigned y=0;y<480;++y) for(unsigned x=0;x<640;++x) {
        const auto actual=large[y*640+x];
        if(x<640-144 || y>=32) {
            if(!same(actual,red)) return false;
        } else {
            if(!same(actual,unscaled[y*256+x-(640-256)])) return false;
            letters+=same(actual,white);
            background+=same(actual,black);
        }
    }
    if(!letters || !background) return false;
    constexpr std::string_view longest="FPS 18446744073709551615.9";
    p.setFpsText(longest);
    const auto atCapacity=PresenterTestAccess::capture(p,640,480);
    for(const auto invalid:{std::string_view("FPS ?"),std::string_view("FPS 184467440737095516155.9")}) {
        try {
            p.setFpsText(invalid);
            std::cerr<<"Invalid FPS text accepted\n";
            return false;
        } catch(const std::invalid_argument&) {}
        const auto unchanged=PresenterTestAccess::capture(p,640,480);
        for(size_t i=0;i<unchanged.size();++i) if(!same(unchanged[i],atCapacity[i])) return false;
    }
    p.setFpsText("FPS 123.4");
    p.resize(64,48);
    for(const auto pixel:PresenterTestAccess::capture(p,64,48)) if(!same(pixel,red)) return false;
    p.setFpsText("");
    p.resize(256,192);
    for(const auto pixel:PresenterTestAccess::capture(p,256,192)) if(!same(pixel,red)) return false;
    return true;
}
bool fastForward() {
    using namespace smsgg::win32;
    Window window;
    D3D11Presenter p(window.handle);
    p.resize(256,192);
    p.present();
    p.present(true,true);
    if(PresenterTestAccess::finishPresent(p,DXGI_STATUS_OCCLUDED,true)!=PresentResult::Occluded ||
       PresenterTestAccess::finishPresent(p,DXGI_ERROR_WAS_STILL_DRAWING,true)!=PresentResult::Skipped ||
       PresenterTestAccess::finishPresent(p,S_OK,true)!=PresentResult::Displayed) return false;
    try {PresenterTestAccess::finishPresent(p,DXGI_ERROR_WAS_STILL_DRAWING,false);}
    catch(const std::runtime_error&) {return true;}
    return false;
}
bool errors() {
    Window window;
    smsgg::win32::D3D11Presenter p(window.handle);
    for (const auto result : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, E_FAIL}) {
        std::string reason;
        try { smsgg::win32::PresenterTestAccess::checkResult(p, result); }
        catch (const std::runtime_error& error) { reason = error.what(); }
        if (reason.find("Injected Present failed (HRESULT") == std::string::npos
            || (reason.find("device reason") != std::string::npos) != (result != E_FAIL)) return false;
    }
    smsgg::win32::PresenterTestAccess::checkResult(p, S_OK);
    return true;
}
}
int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string_view name = argv[1];
    const bool pass = name == "pipeline" ? pipeline(argv[2]) : name == "rom" ? rom(argv[2])
        : name == "gg_crop" ? ggCrop(argv[2]) : name == "fast_forward" ? fastForward()
        : name == "aspect" ? aspect() : name == "fps" ? fpsOverlay() : name == "errors" && errors();
    return pass ? 0 : 1;
}
