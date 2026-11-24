#include "win32/D3D11Presenter.h"
#include "win32/Viewport.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace smsgg::win32 {
namespace {
constexpr unsigned fpsPadding = 4, fpsMargin = 8, fpsAdvance = 12, fpsHeight = 2 * fpsPadding + 14;
// "FPS " plus up to 20 integral digits, a point and one decimal digit.
constexpr unsigned fpsTextureWidth = 26 * fpsAdvance + 2 * fpsPadding;
constexpr unsigned slotTextureWidth=640, slotHeight=24, slotPadding=6;
constexpr std::string_view fpsCharacters = "0123456789.FPS ";
constexpr uint8_t fpsGlyphs[][7]{
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}, {0,0,0,0,0,0,4},
    {31,16,16,30,16,16,16}, {30,17,17,30,16,16,16}, {15,16,16,14,1,1,30},
    {},
};
}

void D3D11Presenter::check(HRESULT result, const char* operation) const {
    if (SUCCEEDED(result)) return;
    std::ostringstream message;
    message << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ')';
    if (device_ && (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET))
        message << "; device reason 0x" << static_cast<unsigned long>(device_->GetDeviceRemovedReason());
    throw std::runtime_error(message.str());
}

D3D11Presenter::D3D11Presenter(HWND window) {
    constexpr D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
        D3D11_SDK_VERSION, &device_, nullptr, &context_), "D3D11CreateDevice(hardware 11_0)");
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    check(device_.As(&dxgiDevice), "Query DXGI device");
    check(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
    check(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get DXGI factory");
    RECT client{};
    if (!GetClientRect(window, &client)) check(HRESULT_FROM_WIN32(GetLastError()), "GetClientRect");
    width_ = client.right;
    height_ = client.bottom;
    DXGI_SWAP_CHAIN_DESC1 chain{};
    chain.Width = std::max(1u, width_);
    chain.Height = std::max(1u, height_);
    chain.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    chain.SampleDesc.Count = 1;
    chain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    chain.BufferCount = 2;
    chain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    check(factory->CreateSwapChainForHwnd(device_.Get(), window, &chain, nullptr, nullptr, &chain_), "CreateSwapChainForHwnd");
    check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "Disable automatic fullscreen");
    createTarget();

    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = 256;
    texture.Height = 192;
    texture.MipLevels = texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DYNAMIC;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device_->CreateTexture2D(&texture, nullptr, &texture_), "Create frame texture");
    check(device_->CreateShaderResourceView(texture_.Get(), nullptr, &view_), "Create frame view");
    texture.Width = fpsTextureWidth;
    texture.Height = fpsHeight;
    check(device_->CreateTexture2D(&texture, nullptr, &fpsTexture_), "Create FPS texture");
    check(device_->CreateShaderResourceView(fpsTexture_.Get(), nullptr, &fpsView_), "Create FPS view");

    constexpr char shader[] = R"(
struct Vertex { float4 position : SV_Position; };
Vertex vs(uint id : SV_VertexID) {
    Vertex v;
    float2 uv = float2((id << 1) & 2, id & 2);
    v.position = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    return v;
}
Texture2D image : register(t0);
cbuffer Crop : register(b0) { uint4 source; uint4 destination; };
float4 ps(Vertex v) : SV_Target {
    uint2 pixel = uint2(v.position.xy) - destination.xy;
    uint2 samplePosition = source.xy + ((2 * pixel + 1) * source.zw) / (2 * destination.zw);
    return image.Load(int3(samplePosition, 0));
}
)";
    auto compile = [&](const char* entry, const char* profile) {
        Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
        const auto result = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr,
            entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        if (FAILED(result) && errors)
            throw std::runtime_error(std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
        check(result, "D3DCompile");
        return code;
    };
    const auto vs = compile("vs", "vs_4_0");
    const auto ps = compile("ps", "ps_4_0");
    check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_), "CreateVertexShader");
    check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_), "CreatePixelShader");
    D3D11_BUFFER_DESC constant{};
    constant.ByteWidth = 32;
    constant.Usage = D3D11_USAGE_DEFAULT;
    constant.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(device_->CreateBuffer(&constant, nullptr, &crop_), "Create crop constants");
}

void D3D11Presenter::createTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    check(chain_->GetBuffer(0, IID_PPV_ARGS(&back)), "Get back buffer");
    check(device_->CreateRenderTargetView(back.Get(), nullptr, &target_), "CreateRenderTargetView");
}

void D3D11Presenter::resize(unsigned width, unsigned height) {
    occluded_ = false;
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    if (width == 0 || height == 0) return;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    target_.Reset();
    check(chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
    createTarget();
}

void D3D11Presenter::uploadFrame(std::span<const Rgba8, 256 * 192> pixels, DisplayArea area) {
    static_assert(sizeof(Rgba8) == 4);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map frame texture");
    for (unsigned y = 0; y < 192; ++y)
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch, pixels.data() + y * 256, 256 * 4);
    context_->Unmap(texture_.Get(), 0);
    area_ = area;
}

void D3D11Presenter::setFpsText(std::string_view text) {
    if (fpsText_ == text) return;
    if (text.empty()) {
        fpsText_.clear();
        fpsWidth_ = 0;
        return;
    }
    if (text.size() > (fpsTextureWidth - 2 * fpsPadding) / fpsAdvance ||
        text.find_first_not_of(fpsCharacters) != std::string_view::npos)
        throw std::invalid_argument("Invalid FPS overlay text");
    std::string nextText(text);
    const auto nextWidth = static_cast<unsigned>(text.size()) * fpsAdvance + 2 * fpsPadding;
    std::array<Rgba8, fpsTextureWidth * fpsHeight> pixels{};
    constexpr Rgba8 white{255,255,255,255};
    for (unsigned i = 0; i < text.size(); ++i) {
        const auto& glyph = fpsGlyphs[fpsCharacters.find(text[i])];
        for (unsigned y = 0; y < 7; ++y) for (unsigned x = 0; x < 5; ++x) {
            if (!(glyph[y] & (16 >> x))) continue;
            const auto offset = (fpsPadding + y * 2) * fpsTextureWidth + fpsPadding + i * fpsAdvance + x * 2;
            std::fill_n(pixels.data() + offset, 2, white);
            std::fill_n(pixels.data() + offset + fpsTextureWidth, 2, white);
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(fpsTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map FPS texture");
    for (unsigned y = 0; y < fpsHeight; ++y)
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
            pixels.data() + y * fpsTextureWidth, nextWidth * sizeof(Rgba8));
    context_->Unmap(fpsTexture_.Get(), 0);
    fpsText_.swap(nextText);
    fpsWidth_ = nextWidth;
}

void D3D11Presenter::setSlotNotice(std::wstring_view text) {
    if(slotNotice_==text) return;
    slotNotice_=text;
    slotAvailableWidth_=0;
}

void D3D11Presenter::uploadSlotNotice(unsigned availableWidth) {
    if(!slotTexture_) {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width=slotTextureWidth;
        texture.Height=slotHeight;
        texture.MipLevels=texture.ArraySize=1;
        texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        texture.SampleDesc.Count=1;
        texture.Usage=D3D11_USAGE_DYNAMIC;
        texture.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        texture.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        check(device_->CreateTexture2D(&texture,nullptr,&slotTexture_),"Create slot notice texture");
        check(device_->CreateShaderResourceView(slotTexture_.Get(),nullptr,&slotView_),"Create slot notice view");
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=slotTextureWidth;
    info.bmiHeader.biHeight=-static_cast<LONG>(slotHeight);
    info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;
    std::unique_ptr<std::remove_pointer_t<HBITMAP>,decltype(&DeleteObject)> bitmap(
        CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0),DeleteObject);
    std::unique_ptr<std::remove_pointer_t<HDC>,decltype(&DeleteDC)> dc(CreateCompatibleDC(nullptr),DeleteDC);
    if(!bitmap || !dc || !SelectObject(dc.get(),bitmap.get()))
        throw std::runtime_error("Create slot notice bitmap failed");
    SelectObject(dc.get(),GetStockObject(DEFAULT_GUI_FONT));
    constexpr unsigned style=DT_SINGLELINE|DT_NOPREFIX;
    RECT measured{};
    const auto length=static_cast<int>(slotNotice_.size());
    if(!DrawTextW(dc.get(),slotNotice_.c_str(),length,&measured,style|DT_CALCRECT))
        throw std::runtime_error("Measure slot notice failed");
    const auto width=std::min(availableWidth,static_cast<unsigned>(measured.right)+2*slotPadding);
    std::memset(pixels,0,slotTextureWidth*slotHeight*4);
    SetBkMode(dc.get(),TRANSPARENT);
    SetTextColor(dc.get(),RGB(255,255,255));
    RECT label{slotPadding,0,static_cast<LONG>(width-slotPadding),slotHeight};
    if(!DrawTextW(dc.get(),slotNotice_.c_str(),length,&label,style|DT_VCENTER|DT_END_ELLIPSIS))
        throw std::runtime_error("Draw slot notice failed");
    GdiFlush();
    auto* colors=static_cast<uint32_t*>(pixels);
    for(unsigned y=0;y<slotHeight;++y)
        for(unsigned x=0;x<width;++x) colors[y*slotTextureWidth+x]|=0xff000000;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(slotTexture_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map slot notice texture");
    for(unsigned y=0;y<slotHeight;++y)
        std::memcpy(static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,
            static_cast<const uint8_t*>(pixels)+y*slotTextureWidth*4,width*4);
    context_->Unmap(slotTexture_.Get(),0);
    slotWidth_=width;
    slotAvailableWidth_=availableWidth;
}

void D3D11Presenter::draw() {
    if (width_ == 0 || height_ == 0) return;
    context_->OMSetRenderTargets(1, target_.GetAddressOf(), nullptr);
    constexpr float black[]{0, 0, 0, 1};
    context_->ClearRenderTargetView(target_.Get(), black);
    const auto area = calculateViewport(width_, height_, area_.width, area_.height, aspectRatio_);
    if (area.width == 0 || area.height == 0) return;
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, crop_.GetAddressOf());
    const auto drawTexture = [&](DisplayArea source, Viewport destination, ID3D11ShaderResourceView* view) {
        const uint32_t crop[]{source.x,source.y,source.width,source.height,
            destination.x,destination.y,destination.width,destination.height};
        context_->UpdateSubresource(crop_.Get(),0,nullptr,crop,0,0);
        const D3D11_VIEWPORT viewport{static_cast<float>(destination.x), static_cast<float>(destination.y),
            static_cast<float>(destination.width), static_cast<float>(destination.height), 0, 1};
        context_->RSSetViewports(1, &viewport);
        context_->PSSetShaderResources(0, 1, &view);
        context_->Draw(3, 0);
    };
    drawTexture(area_, area, view_.Get());
    const bool drawFps=fpsWidth_ && area.width>=fpsWidth_+2*fpsMargin && area.height>=fpsHeight+2*fpsMargin;
    if (drawFps) {
        drawTexture({0,0,fpsWidth_,fpsHeight},
            {area.x + area.width - fpsMargin - fpsWidth_, area.y + fpsMargin, fpsWidth_, fpsHeight}, fpsView_.Get());
    }
    const auto reserved=2*fpsMargin+(drawFps?fpsWidth_+fpsMargin:0);
    if(!slotNotice_.empty() && area.width>reserved+2*slotPadding && area.height>=slotHeight+2*fpsMargin) {
        const auto available=std::min(slotTextureWidth,area.width-reserved);
        if(slotAvailableWidth_!=available) uploadSlotNotice(available);
        drawTexture({0,0,slotWidth_,slotHeight},{area.x+fpsMargin,area.y+fpsMargin,slotWidth_,slotHeight},slotView_.Get());
    }
}

PresentResult D3D11Presenter::present(bool retryOccluded, bool fastForward) {
    if (width_ == 0 || height_ == 0 || (occluded_ && !retryOccluded)) return PresentResult::Skipped;
    draw();
    return finishPresent(chain_->Present(fastForward ? 0 : 1, fastForward ? DXGI_PRESENT_DO_NOT_WAIT : 0), fastForward);
}

PresentResult D3D11Presenter::finishPresent(HRESULT result, bool fastForward) {
    if (result == DXGI_STATUS_OCCLUDED) { occluded_ = true; return PresentResult::Occluded; }
    if (fastForward && result == DXGI_ERROR_WAS_STILL_DRAWING) { occluded_ = false; return PresentResult::Skipped; }
    check(result, "Present");
    occluded_ = false;
    return PresentResult::Displayed;
}
} // namespace smsgg::win32
