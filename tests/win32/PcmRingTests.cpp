#include "win32/PcmRing.h"

#include <array>
#include <atomic>
#include <iostream>
#include <string_view>
#include <thread>

namespace {
smsgg::AudioFrame frame(unsigned n) {return {static_cast<float>(n),static_cast<float>((n*17)&0xfffff)};}
bool same(smsgg::AudioFrame a,smsgg::AudioFrame b) {return a.left==b.left && a.right==b.right;}
bool bounds() {
    smsgg::win32::PcmRing ring(8);
    std::array<smsgg::AudioFrame,12> input{},output{};
    for(unsigned i=0;i<12;++i) input[i]=frame(i);
    output.fill({-1,-2});
    if(ring.queued() || ring.pop(output)!=0 || !same(output[0],{-1,-2})) return false;
    if(ring.push(input)!=8 || ring.queued()!=8 || ring.push(input)!=0) return false;
    if(ring.pop(std::span(output).first(3))!=3) return false;
    for(unsigned i=0;i<3;++i) if(!same(output[i],frame(i))) return false;
    if(ring.push(std::span(input).subspan(8))!=3 || ring.pop(output)!=8) return false;
    for(unsigned i=0;i<8;++i) if(!same(output[i],frame(i+3))) return false;
    if(ring.queued()!=0 || ring.push({})!=0 || ring.pop({})!=0) return false;
    ring.push(input);ring.reset();
    return ring.queued()==0 && ring.pop(output)==0 && ring.push(std::span(input).subspan(10))==2
        && ring.pop(output)==2 && same(output[0],frame(10)) && same(output[1],frame(11));
}
bool threaded() {
    smsgg::win32::PcmRing ring(1024);
    constexpr unsigned count=500000;
    std::atomic<bool> failed{};
    std::thread producer([&] {
        std::array<smsgg::AudioFrame,37> batch{};
        for(unsigned sent=0;sent<count && !failed.load();) {
            const auto n=std::min<unsigned>(batch.size(),count-sent);
            for(unsigned i=0;i<n;++i) batch[i]=frame(sent+i);
            sent+=static_cast<unsigned>(ring.push(std::span(batch).first(n)));
            std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        std::array<smsgg::AudioFrame,53> batch{};
        unsigned received=0;
        while(received<count && !failed.load()) {
            const auto n=ring.pop(batch);
            for(size_t i=0;i<n;++i) if(received>=count || !same(batch[i],frame(received++))) failed=true;
            if(!n) std::this_thread::yield();
        }
    });
    producer.join();consumer.join();
    return !failed && ring.queued()==0;
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="bounds"?bounds():name=="threaded"&&threaded();
    if(!pass) std::cerr<<"PCM ring test failed: "<<name<<'\n';
    return pass?0:1;
}
