#include "render/chronicle_view.hpp"
#include "game/chronicle.hpp"
#include <algorithm>
#include <string>
#include <vector>
namespace render {
namespace {
struct Layout { Rectangle panel, close, body; float rail, row; };
Layout layout(Vector2 vp) {
    const float w=std::min(1220.0f,vp.x-40), h=std::min(760.0f,vp.y-40);
    const float x=(vp.x-w)/2, y=(vp.y-h)/2;
    const float rail=std::min(250.0f,w*0.27f);
    return {{x,y,w,h},{x+w-105,y+20,80,32},{x+rail+30,y+165,w-rail-60,h-255},rail,
            std::min(48.0f,(h-140)/8)};
}
std::vector<std::string> wrap(const FontSet& font, std::string_view text, float width) {
    std::vector<std::string> lines;
    std::string line;
    for(std::size_t i=0;i<text.size();) {
        if(text[i]=='\n') {lines.push_back(line);line.clear();++i;continue;}
        int bytes=0;
        const int codepoint=GetCodepointNext(text.data()+i,&bytes);
        const auto n=static_cast<std::size_t>(std::max(1,bytes));
        const std::string glyph(text.substr(i,n));
        if(!line.empty() && font.measure(line+glyph,22).x>width) {
            // 行首禁则：将前一个字和句末标点一起换行，避免单独一行的句号。
            const bool closing=codepoint==0x3002 || codepoint==0xff0c || codepoint==0xff1b ||
                codepoint==0xff1a || codepoint==0xff1f || codepoint==0xff01 ||
                codepoint==0x300d || codepoint==0x201d || codepoint==0xff09;
            if(closing) {
                auto last=line.size()-1;
                while(last>0 && (static_cast<unsigned char>(line[last])&0xc0)==0x80) --last;
                const auto carry=line.substr(last);
                lines.push_back(line.substr(0,last));line=carry;
            } else {lines.push_back(line);line.clear();}
        }
        line+=glyph;i+=n;
    }
    if(!line.empty()) lines.push_back(line);
    return lines;
}
}
void ChronicleView::preview(int chapter,int ending,bool bottom) noexcept {
    open=true;chapter_=std::clamp(chapter,0,7);scroll_=bottom?1000000.0f:0.0f;
    max_scroll_=0;ending_=std::clamp(ending,0,2);
}
std::vector<std::string_view> ChronicleView::strings() {
    std::vector<std::string_view> out{"圣城档案指挥官日记第任守城指挥官篇按波次解锁尚未解锁返回滚轮翻阅记录节选改编本次远征已解锁继续守护放下武器返回日记结局一二回顾选择将决定本局走向无尽模式伪通关达成"};
    const auto add = [&](std::string_view text) {
        while(!text.empty()) {
            const auto end=text.find('\n');
            out.push_back(text.substr(0,end));
            if(end==std::string_view::npos) break;
            text.remove_prefix(end+1);
        }
    };
    add(game::kChronicleGuard); add(game::kChronicleRelease);
    for(const auto& e:game::kChronicle) {out.push_back(e.title);add(e.text);}
    return out;
}
void ChronicleView::update(Vector2 vp,int reached_wave) {
    const auto l=layout(vp);
    const auto mouse=GetMousePosition();
    if(IsKeyPressed(KEY_ESCAPE)||IsKeyPressed(KEY_J)) {open=false;return;}
    scroll_=std::clamp(scroll_-GetMouseWheelMove()*64.0f,0.0f,max_scroll_);
    if(IsKeyPressed(KEY_DOWN)||IsKeyPressed(KEY_PAGE_DOWN)) scroll_=std::min(max_scroll_,scroll_+l.body.height*0.7f);
    if(IsKeyPressed(KEY_UP)||IsKeyPressed(KEY_PAGE_UP)) scroll_=std::max(0.0f,scroll_-l.body.height*0.7f);
    if(!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    if(CheckCollisionPointRec(mouse,l.close)) {open=false;return;}
    for(int i=0;i<8;++i) {
        const Rectangle r{l.panel.x+14,l.panel.y+112+static_cast<float>(i)*l.row,l.rail-28,l.row-4};
        if(CheckCollisionPointRec(mouse,r) && game::kChronicle[static_cast<std::size_t>(i)].wave<=reached_wave) {
            chapter_=i;ending_=0;scroll_=0;max_scroll_=0;
        }
    }
    if(chapter_==7 && reached_wave>=70) {
        const float y=l.panel.y+l.panel.height-64;
        if(CheckCollisionPointRec(mouse,{l.body.x,y,150,36})) {
            ending_=1;scroll_=0;
            if(decision_enabled) requested_=game::ChronicleChoice::Guard;
        }
        if(CheckCollisionPointRec(mouse,{l.body.x+165,y,150,36})) {
            ending_=2;scroll_=0;
            if(decision_enabled) requested_=game::ChronicleChoice::Release;
        }
        if(CheckCollisionPointRec(mouse,{l.body.x+330,y,130,36})) {ending_=0;scroll_=0;}
    }
}
void ChronicleView::draw(const FontSet& font,Vector2 vp,int reached_wave) {
    const auto l=layout(vp);
    DrawRectangle(0,0,static_cast<int>(vp.x),static_cast<int>(vp.y),Color{8,12,17,230});
    DrawRectangleRec(l.panel,Color{28,34,37,255});
    DrawRectangleLinesEx(l.panel,1,Color{157,134,88,255});
    DrawRectangleRec({l.panel.x+8,l.panel.y+8,3,l.panel.height-16},Color{191,156,95,255});
    font.draw("圣城档案 / 指挥官日记",{l.panel.x+30,l.panel.y+23},30,Color{234,218,178,255});
    font.draw("第 47 任守城指挥官 · 按波次解锁",{l.panel.x+30,l.panel.y+68},18,Color{157,170,172,255});
    DrawRectangleLinesEx(l.close,1,Color{157,134,88,255});
    font.draw("返回",{l.close.x+18,l.close.y+5},20,Color{234,218,178,255});
    DrawLineEx({l.panel.x+l.rail,l.panel.y+110},{l.panel.x+l.rail,l.panel.y+l.panel.height-28},1,Color{74,78,71,255});
    for(int i=0;i<8;++i) {
        const auto& e=game::kChronicle[static_cast<std::size_t>(i)];
        const bool unlocked=e.wave<=reached_wave;
        const float y=l.panel.y+112+static_cast<float>(i)*l.row;
        if(i==chapter_) DrawRectangleRec({l.panel.x+14,y,l.rail-28,l.row-4},Color{68,66,51,255});
        const std::string label=std::to_string(e.wave)+" / "+(unlocked?std::string(e.title):"尚未解锁");
        font.draw(label,{l.panel.x+26,y+8},19,unlocked?Color{224,215,191,255}:Color{115,124,128,255});
    }
    const auto& entry=game::kChronicle[static_cast<std::size_t>(chapter_)];
    const bool unlocked=entry.wave<=reached_wave;
    const std::string title=unlocked?(ending_==1?"继续守护 / 无尽模式":ending_==2?"放下武器 / 伪通关达成":std::string(entry.title)):"尚未解锁";
    font.draw(title,{l.body.x,l.panel.y+118},28,Color{232,211,168,255});
    std::string body=unlocked?std::string(ending_==1?game::kChronicleGuard:ending_==2?game::kChronicleRelease:entry.text):
        "第 "+std::to_string(entry.wave)+" 波解锁";
    const auto lines=wrap(font,body,l.body.width-20);
    max_scroll_=std::max(0.0f,static_cast<float>(lines.size())*33-l.body.height);
    scroll_=std::clamp(scroll_,0.0f,max_scroll_);
    BeginScissorMode(static_cast<int>(l.body.x),static_cast<int>(l.body.y),static_cast<int>(l.body.width),static_cast<int>(l.body.height));
    for(std::size_t i=0;i<lines.size();++i)
        font.draw(lines[i],{l.body.x,l.body.y+static_cast<float>(i)*33-scroll_},22,Color{216,217,203,255});
    EndScissorMode();
    if(max_scroll_>0) {
        const float thumb=std::max(28.0f,l.body.height*l.body.height/(max_scroll_+l.body.height));
        DrawRectangleRec({l.body.x+l.body.width-5,l.body.y+(l.body.height-thumb)*scroll_/max_scroll_,3,thumb},Color{181,153,99,255});
    }
    // 白羽只作为档案页边的低对比装饰，不进入战场实体。
    const Vector2 mark{l.panel.x+l.panel.width-65,l.panel.y+82};
    const Color ink{149,145,116,100};
    DrawLineEx({mark.x-14,mark.y+20},{mark.x+12,mark.y-20},1,ink);
    for(int i=0;i<6;++i) {
        const float t=static_cast<float>(i)*5;
        DrawLineEx({mark.x-10+t*0.55f,mark.y+14-t},{mark.x-24+t*0.55f,mark.y+7-t},1,ink);
        DrawLineEx({mark.x-10+t*0.55f,mark.y+14-t},{mark.x+3+t*0.55f,mark.y+17-t},1,ink);
    }
    const float bottom=l.panel.y+l.panel.height-64;
    if(chapter_==7 && unlocked) {
        const std::array<std::string,3> labels{decision_enabled?"继续守护":"结局一回顾",decision_enabled?"放下武器":"结局二回顾","返回日记"};
        for(int i=0;i<3;++i) {
            const float x=l.body.x+static_cast<float>(i)*165;
            DrawRectangleLinesEx({x,bottom,i==2?130.0f:150.0f,36},1,Color{157,134,88,255});
            font.draw(labels[static_cast<std::size_t>(i)],{x+12,bottom+7},20,Color{225,211,177,255});
        }
    } else font.draw("滚轮翻阅 · J / Esc 返回",{l.body.x,bottom+5},18,Color{162,169,164,255});
    if(chapter_==7 && unlocked) font.draw(decision_enabled?"选择将决定本局走向":"结局回顾",{l.body.x,bottom-27},18,Color{162,169,164,255});
}
}
