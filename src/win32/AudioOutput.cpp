#include "win32/AudioOutput.h"
#include "win32/AudioFormat.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <stdexcept>
#include <system_error>
#include <vector>

namespace smsgg::win32 {
namespace {
void check(HRESULT result,const char* operation) {
    if(FAILED(result)) throw std::runtime_error(std::string(operation)+" failed (HRESULT "
        +std::to_string(static_cast<unsigned long>(result))+")");
}
struct Apartment {
    Apartment() {check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Audio CoInitializeEx");}
    ~Apartment() {CoUninitialize();}
};
}
AudioOutput::AudioOutput() {
    std::promise<void> initialized;
    auto ready=initialized.get_future();
    thread_=std::thread(&AudioOutput::run,this,std::move(initialized));
    try {ready.get();}
    catch(const std::exception&) {thread_.join();throw;}
}
AudioOutput::~AudioOutput() {stop();}
void AudioOutput::stop() {
    if(!thread_.joinable()) return;
    stream_->shutdown();
    thread_.join();
}
void AudioOutput::run(std::promise<void> initialized) {
    bool ready=false;
    try {
        Apartment apartment;
        // Keep the event alive until all clients that can signal it have been released.
        std::unique_ptr<void,decltype(&::CloseHandle)> renderEvent(nullptr,&::CloseHandle);
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        Microsoft::WRL::ComPtr<IMMDevice> device;
        Microsoft::WRL::ComPtr<IAudioClient> client;
        Microsoft::WRL::ComPtr<IAudioRenderClient> render;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Create audio enumerator");
        check(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device),"Get default audio endpoint");
        check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client),"Activate IAudioClient");
        WAVEFORMATEX* raw{};
        check(client->GetMixFormat(&raw),"GetMixFormat");
        std::unique_ptr<WAVEFORMATEX,decltype(&CoTaskMemFree)> wave(raw,&CoTaskMemFree);
        const auto parsed=AudioFormat::parse({reinterpret_cast<const std::byte*>(raw),sizeof(WAVEFORMATEX)+raw->cbSize});
        if(const auto error=std::get_if<std::string>(&parsed)) throw std::runtime_error(*error);
        const auto format=std::get<AudioFormat>(parsed);
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,0,0,raw,nullptr),"Initialize shared audio");
        UINT32 buffer{};
        check(client->GetBufferSize(&buffer),"GetBufferSize");
        check(client->GetService(IID_PPV_ARGS(&render)),"Get audio render client");
        renderEvent.reset(CreateEventW(nullptr,FALSE,FALSE,nullptr));
        if(!renderEvent) throw std::system_error(GetLastError(),std::system_category(),"Create render event");
        check(client->SetEventHandle(renderEvent.get()),"SetEventHandle");
        stream_=std::make_unique<AudioStream>(format.sampleRate(),buffer);
        std::vector<AudioFrame> scratch(buffer);
        description_="WASAPI shared: rate="+std::to_string(raw->nSamplesPerSec)+" channels="+std::to_string(raw->nChannels)
            +" bits="+std::to_string(raw->wBitsPerSample)+" tag="+std::to_string(raw->wFormatTag)+" buffer="+std::to_string(buffer);
        if(raw->wFormatTag==WAVE_FORMAT_EXTENSIBLE) {
            const auto* ext=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(raw);
            description_+=" valid="+std::to_string(ext->Samples.wValidBitsPerSample)+" mask="+std::to_string(ext->dwChannelMask);
        }
        LPWSTR endpointRaw{};
        check(device->GetId(&endpointRaw),"Get audio endpoint ID");
        std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> endpoint(endpointRaw,&CoTaskMemFree);
        const auto bytes=WideCharToMultiByte(CP_UTF8,0,endpoint.get(),-1,nullptr,0,nullptr,nullptr);
        if(!bytes) throw std::system_error(GetLastError(),std::system_category(),"Size endpoint ID text");
        std::string endpointId(static_cast<size_t>(bytes),'\0');
        if(!WideCharToMultiByte(CP_UTF8,0,endpoint.get(),-1,endpointId.data(),bytes,nullptr,nullptr))
            throw std::system_error(GetLastError(),std::system_category(),"Convert endpoint ID text");
        endpointId.pop_back();description_+=" endpoint="+endpointId;
        wave.reset();
        initialized.set_value();ready=true;
        bool playing=false;
        uint64_t acknowledged=0;
        const HANDLE events[]{stream_->consumerEvent(),renderEvent.get()};
        auto refill=[&](bool muted) {
            UINT32 padding{};
            check(client->GetCurrentPadding(&padding),"GetCurrentPadding");
            const auto count=stream_->availableRenderFrames(padding);
            if(!count) return;
            BYTE* destination{};
            check(render->GetBuffer(count,&destination),"Get render buffer");
            const auto consumed=stream_->pcm.pop(std::span(scratch).first(count));
            format.write(std::span(scratch).first(consumed),
                {reinterpret_cast<std::byte*>(destination),size_t(count)*format.blockAlign()},muted);
            check(render->ReleaseBuffer(count,(muted || !consumed)?AUDCLNT_BUFFERFLAGS_SILENT:0),"Release render buffer");
            stream_->recordRead(consumed,count-consumed,padding);
        };
        for(;;) {
            const auto control=stream_->control();
            if(control.shutdown) break;
            if(control.sequence!=acknowledged) {
                if(control.command==AudioCommand::Stopped) {
                    if(playing) check(client->Stop(),"Stop audio");
                    check(client->Reset(),"Reset audio");playing=false;
                } else if(!playing) {
                    refill(control.muted);
                    check(client->Start(),"Start audio");playing=true;
                }
                acknowledged=control.sequence;
                stream_->acknowledge(acknowledged,playing);
            }
            if(playing) refill(control.muted);
            if(WaitForMultipleObjects(2,events,FALSE,INFINITE)==WAIT_FAILED)
                throw std::system_error(GetLastError(),std::system_category(),"Wait for audio events");
        }
        if(playing) check(client->Stop(),"Stop audio on exit");
        check(client->Reset(),"Reset audio on exit");
    } catch(const std::exception& error) {
        if(!ready) initialized.set_exception(std::current_exception());
        else stream_->fail(error.what());
    }
    if(stream_) stream_->markExited();
}
} // namespace smsgg::win32
