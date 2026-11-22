#include "win32/AudioOutput.h"

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>

int main(int argc,char** argv) {
    try {
        const auto duration=std::chrono::seconds(argc>1?std::stoi(argv[1]):2);
        smsgg::win32::AudioOutput output;
        std::cout<<output.description()<<std::endl;
        auto& stream=output.stream();
        const auto start=std::chrono::steady_clock::now();
        uint64_t generated=0;
        std::array<smsgg::AudioFrame,256> frames{};
        bool playing=false,muted=false;
        while(std::chrono::steady_clock::now()-start<duration) {
            if(!stream.status().error.empty()) throw std::runtime_error(stream.status().error);
            const auto free=stream.targetFrames()-stream.pcm.queued();
            if(free) {
                const auto n=std::min(free,frames.size());
                for(size_t i=0;i<n;++i) {
                    const float sample=static_cast<float>(.1*std::sin(6.283185307179586*440*double(generated+i)/stream.sampleRate()));
                    frames[i]={sample,sample};
                }
                generated+=stream.pcm.push(std::span(frames).first(n));
            }
            if(stream.pcm.queued()>=stream.targetFrames()) {
                if(!playing) {stream.request(smsgg::win32::AudioCommand::Playing,false);playing=true;}
                if(!muted && std::chrono::steady_clock::now()-start>duration/2) {
                    stream.request(smsgg::win32::AudioCommand::Playing,true);muted=true;
                }
                stream.wakeConsumer();
                if(WaitForSingleObject(stream.producerEvent(),100)==WAIT_FAILED) throw std::runtime_error("Producer wait failed");
            }
        }
        const auto stopped=stream.request(smsgg::win32::AudioCommand::Stopped,muted);
        while(stream.status().acknowledged<stopped && stream.status().error.empty()) {
            if(WaitForSingleObject(stream.producerEvent(),2000)!=WAIT_OBJECT_0) throw std::runtime_error("Stop acknowledgement timeout");
        }
        const auto status=stream.status();
        std::cout<<"generated="<<generated<<" consumed="<<status.consumed<<" silence="<<status.silence
            <<" queued="<<stream.pcm.queued()<<" playing="<<status.playing<<" error="<<status.error<<std::endl;
        if(!status.error.empty() || status.playing || status.consumed<stream.sampleRate() || status.silence) return 1;
        stream.pcm.reset();
        output.stop();
        return stream.status().exited?0:1;
    } catch(const std::exception& error) {
        std::cerr<<"Audio output probe: "<<error.what()<<std::endl;
        return 2;
    }
}
