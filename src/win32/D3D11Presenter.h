#pragma once

#include "core/video/Vdp.h"
#include "win32/Viewport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <string_view>

namespace smsgg::win32 {
enum class PresentResult { Displayed, Occluded, Skipped };
class D3D11Presenter final {
public:
    explicit D3D11Presenter(HWND window);
    void resize(unsigned width, unsigned height);
    void uploadFrame(std::span<const Rgba8, 256 * 192> pixels, DisplayArea area = {0,0,256,192});
    void setAspectRatio(AspectRatio value) { aspectRatio_ = value; }
    AspectRatio aspectRatio() const { return aspectRatio_; }
    void setFpsText(std::string_view text); // Empty or "FPS <number>" with one decimal place.
    PresentResult present(bool retryOccluded = false, bool fastForward = false);
private:
    friend struct PresenterTestAccess;
    void createTarget();
    void draw();
    PresentResult finishPresent(HRESULT result, bool fastForward);
    void check(HRESULT result, const char* operation) const;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> fpsTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fpsView_;
    std::string fpsText_;
    unsigned fpsWidth_{};
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> crop_;
    DisplayArea area_{0,0,256,192};
    AspectRatio aspectRatio_{AspectRatio::SquarePixels};
    unsigned width_{}, height_{};
    bool occluded_{};
};
} // namespace smsgg::win32
