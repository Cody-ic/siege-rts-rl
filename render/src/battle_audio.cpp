#include "render/battle_audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace render {
namespace {
constexpr float tau=6.28318530718f;
float noise(std::uint32_t& state) {state^=state<<13;state^=state>>17;state^=state<<5;return static_cast<float>(state&65535u)/32767.5f-1;}
Sound sound_from(const std::vector<float>& samples) {
    Wave wave{static_cast<unsigned int>(samples.size()),24000,32,1,const_cast<float*>(samples.data())};
    return LoadSoundFromWave(wave);
}
}
std::vector<float> synthesize_cue(BattleCue cue,int rate) {
    const float duration=cue==BattleCue::Horn?2.6f:cue==BattleCue::Phoenix?2.2f:cue==BattleCue::Collapse?1.7f:cue==BattleCue::WingFall?1.3f:0.65f;
    std::vector<float> pcm(static_cast<std::size_t>(duration*static_cast<float>(rate)));
    std::uint32_t state=8171u+static_cast<std::uint32_t>(cue)*73u;float low=0;
    for(std::size_t i=0;i<pcm.size();++i) {
        const float t=static_cast<float>(i)/static_cast<float>(rate),n=noise(state);low+=0.09f*(n-low);
        const float attack=std::min(1.0f,t*130),tail=std::min(1.0f,(duration-t)*12);
        float value=0;
        switch(cue) {
        case BattleCue::Horn: {
            const float swell=std::min(1.0f,t*2)*std::exp(-t*0.9f);
            value=swell*(0.32f*std::sin(tau*110*t)+0.17f*std::sin(tau*165*t)+0.05f*std::sin(tau*330*t));break;
        }
        case BattleCue::Phoenix: {
            const float note=t<0.72f?440.0f:t<1.44f?523.251f:659.255f;
            const float a=std::fmod(t,0.72f);
            value=std::min(1.0f,a*25)*std::exp(-a*5)*(0.20f*std::sin(tau*note*a)+0.07f*std::sin(tau*note*2.003f*a))+low*0.3f*std::exp(-t);break;
        }
        case BattleCue::Bow: value=(n*0.28f+std::sin(tau*(720*t-280*t*t))*0.20f)*std::exp(-t*15);break;
        case BattleCue::Bolt: value=(low*0.8f+std::sin(tau*86*t)*0.25f+std::sin(tau*243*t)*0.12f)*std::exp(-t*8);break;
        case BattleCue::Stone: value=(low*1.4f+std::sin(tau*(65*t-15*t*t))*0.35f)*std::exp(-t*9);break;
        case BattleCue::Collapse: value=(low*1.8f+0.20f*std::sin(tau*42*t))*std::exp(-t*2.7f)*(0.7f+0.3f*std::sin(tau*13*t));break;
        case BattleCue::WingFall: value=(low*0.7f+0.13f*std::sin(tau*(660*t-120*t*t)))*std::exp(-t*3);break;
        case BattleCue::Work: value=(n*0.14f+0.25f*std::sin(tau*940*t)+0.09f*std::sin(tau*1511*t))*std::exp(-t*24);break;
        case BattleCue::Count: break;
        }
        pcm[i]=value*attack*tail;
        const auto delay=static_cast<std::size_t>(static_cast<float>(rate)*0.093f);
        if(i>delay) pcm[i]+=pcm[i-delay]*0.12f;
        pcm[i]=std::clamp(pcm[i],-0.85f,0.85f);
    }
    return pcm;
}
std::vector<float> synthesize_wind(int rate) {
    constexpr float duration=12;
    std::vector<float> pcm(static_cast<std::size_t>(duration*static_cast<float>(rate)));
    std::uint32_t state=47501u;float low=0;
    for(std::size_t i=0;i<pcm.size();++i) {
        const float t=static_cast<float>(i)/static_cast<float>(rate);low+=0.015f*(noise(state)-low);
        const float fade=std::min({1.0f,t,(duration-t)});
        pcm[i]=fade*(low*(0.35f+0.12f*std::sin(tau*t/7))+0.006f*std::sin(tau*55*t));
    }
    return pcm;
}
BattleAudio::BattleAudio(bool enabled) {
    if(!enabled) return;
    InitAudioDevice();ready_=IsAudioDeviceReady();
    if(!ready_) return;
    wind_=sound_from(synthesize_wind());
    for(std::size_t i=0;i<kCues;++i) sounds_[i]=sound_from(synthesize_cue(static_cast<BattleCue>(i)));
}
BattleAudio::~BattleAudio() {
    if(!ready_) return;
    for(auto sound:sounds_) UnloadSound(sound);
    UnloadSound(wind_);CloseAudioDevice();
}
void BattleAudio::toggle() {muted_=!muted_;active(active_);}
void BattleAudio::active(bool enabled) {
    active_=enabled;
    if(!ready_) return;
    if(muted_ || !enabled) {
        StopSound(wind_);for(auto sound:sounds_) StopSound(sound);return;
    }
    SetSoundVolume(wind_,0.6f);
    if(!IsSoundPlaying(wind_)) PlaySound(wind_);
}
void BattleAudio::play(const std::vector<BattleSignal>& events,const game::IsoProjection& proj,
                       const Camera2D& camera,Vector2 viewport) {
    if(!ready_||muted_||!active_) return;
    const double now=GetTime();
    for(const auto& e:events) {
        const auto k=static_cast<std::size_t>(e.cue);if(now<next_[k]) continue;
        const auto p=proj.world_to_screen(e.pos);const auto screen=GetWorldToScreen2D({p.x,p.y},camera);
        const float dx=(screen.x-viewport.x*0.5f)/std::max(1.0f,viewport.x*0.5f),dy=(screen.y-viewport.y*0.5f)/std::max(1.0f,viewport.y*0.5f);
        const bool horn=e.cue==BattleCue::Horn;
        const float gain=horn?0.40f:std::clamp(1.25f-0.65f*std::sqrt(dx*dx+dy*dy),0.0f,1.0f)*0.48f;
        if(gain<0.02f) continue;
        SetSoundVolume(sounds_[k],gain);
        // raylib pan: 0 is right, 1 is left.
        SetSoundPan(sounds_[k],horn?0.5f:std::clamp(0.5f-dx*0.30f,0.1f,0.9f));
        PlaySound(sounds_[k]);
        next_[k]=now+(e.cue==BattleCue::Phoenix?8.0:e.cue==BattleCue::Horn?3.0:0.12);
    }
}
}
