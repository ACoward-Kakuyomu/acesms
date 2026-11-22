#pragma once

#include "win32/D3D11Presenter.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace smsgg::win32 {
struct PresenterTestAccess {
    static const auto& fpsText(const D3D11Presenter& p) { return p.fpsText_; }
    static PresentResult finishPresent(D3D11Presenter& p,HRESULT result,bool fastForward) {
        return p.finishPresent(result,fastForward);
    }
    static void checkResult(D3D11Presenter& p, HRESULT result) { p.check(result, "Injected Present"); }
    static std::vector<Rgba8> capture(D3D11Presenter& p, unsigned width, unsigned height) {
        p.draw();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back, staging;
        if (FAILED(p.chain_->GetBuffer(0, IID_PPV_ARGS(&back)))) throw std::runtime_error("GetBuffer failed");
        D3D11_TEXTURE2D_DESC desc{};
        back->GetDesc(&desc);
        if (desc.Width != width || desc.Height != height || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM
            || desc.SampleDesc.Count != 1) throw std::runtime_error("Unexpected back buffer");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(p.device_->CreateTexture2D(&desc, nullptr, &staging))) throw std::runtime_error("Create staging failed");
        p.context_->CopyResource(staging.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(p.context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) throw std::runtime_error("Readback map failed");
        std::vector<Rgba8> pixels(width * height);
        for (unsigned y = 0; y < height; ++y)
            std::memcpy(pixels.data() + y * width, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, width * 4);
        p.context_->Unmap(staging.Get(), 0);
        return pixels;
    }
    static void describe(D3D11Presenter& p) {
        Microsoft::WRL::ComPtr<IDXGIDevice> device;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(p.device_.As(&device)) || FAILED(device->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))
            throw std::runtime_error("Adapter description failed");
        std::wcout << L"Hardware adapter: " << desc.Description << L"\n";
    }
};
}
