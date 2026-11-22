#include "win32/AudioStream.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using smsgg::win32::AudioCommand;
using smsgg::win32::AudioStream;
bool control() {
    AudioStream stream(48000,480);
    if(stream.sampleRate()!=48000 || stream.bufferFrames()!=480 || stream.targetFrames()!=1920
        || WaitForSingleObject(stream.consumerEvent(),0)!=WAIT_TIMEOUT) return false;
    const auto play=stream.request(AudioCommand::Playing,false);
    if(play!=1 || WaitForSingleObject(stream.consumerEvent(),0)!=WAIT_OBJECT_0) return false;
    auto command=stream.control();
    if(command.sequence!=play || command.command!=AudioCommand::Playing || command.muted || command.shutdown) return false;
    stream.acknowledge(play,true);
    if(WaitForSingleObject(stream.producerEvent(),0)!=WAIT_OBJECT_0 || !stream.status().playing) return false;
    if(stream.request(AudioCommand::Playing,false)!=play || WaitForSingleObject(stream.consumerEvent(),0)!=WAIT_TIMEOUT) return false;
    std::array<smsgg::AudioFrame,8> frames{};
    stream.pcm.push(frames);
    const auto stop=stream.request(AudioCommand::Stopped,false);
    if(stream.status().acknowledged>=stop) return false;
    stream.acknowledge(stop,false);
    if(stream.status().acknowledged!=stop || stream.status().playing || stream.pcm.queued()!=8) return false;
    stream.pcm.reset(); // Only the producer, after the consumer's stopped acknowledgement.
    if(stream.pcm.queued()) return false;
    const auto muted=stream.request(AudioCommand::Playing,true);
    if(!stream.control().muted || muted!=stop+1) return false;
    stream.recordRead(3,5,22);
    if(stream.status().consumed!=3 || stream.status().silence!=5 || stream.status().padding!=22) return false;
    stream.shutdown();
    if(!stream.control().shutdown) return false;
    stream.markExited();
    AudioStream largeBuffer(44100,4096);
    return stream.status().exited && !stream.status().playing && largeBuffer.targetFrames()==4096;
}
bool failure() {
    AudioStream stream(48000,480);
    if(stream.availableRenderFrames(0)!=480 || stream.availableRenderFrames(47)!=433 ||
        stream.availableRenderFrames(480)!=0) return false;
    try {
        (void)stream.availableRenderFrames(481);
        return false;
    } catch(const std::runtime_error&) {}
    const auto play=stream.request(AudioCommand::Playing,false);
    stream.acknowledge(play,true);
    WaitForSingleObject(stream.producerEvent(),0);
    stream.fail("Injected device invalidated");
    const auto status=stream.status();
    if(status.error!="Injected device invalidated" || status.playing
        || WaitForSingleObject(stream.producerEvent(),0)!=WAIT_OBJECT_0) return false;
    stream.markExited();
    return stream.status().exited && stream.status().error==status.error;
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="control"?control():name=="failure"&&failure();
    if(!pass) std::cerr<<"Audio stream test failed: "<<name<<'\n';
    return pass?0:1;
}
