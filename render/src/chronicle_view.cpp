#include "render/chronicle_view.hpp"
#include "game/chronicle.hpp"
#include "game/chronicle_reading.hpp"
#include "game/chronicle_appendix_text.hpp"
#include "rlgl.h"
#include <algorithm>
#include <string>
#include <vector>
namespace render {
namespace {
constexpr float kBodySize=18.0f;
constexpr float kLineHeight=27.0f;
constexpr float kParagraphGap=10.0f;
struct Layout { Rectangle panel, close, body; float rail, row; };
Layout layout(Vector2 vp) {
    const float w=std::min(1400.0f,vp.x-24), h=vp.y-24;
    const float x=(vp.x-w)/2, y=(vp.y-h)/2;
    const float rail=std::clamp(w*0.23f,180.0f,220.0f);
    const float body_w=std::min(780.0f,w-rail-48);
    const float body_x=x+rail+24+(w-rail-48-body_w)*0.5f;
    return {{x,y,w,h},{x+w-105,y+20,80,32},{body_x,y+142,body_w,h-232},rail,
            std::min(48.0f,(h-140)/11)};
}
std::vector<std::string> wrap_plain(const FontSet& font, std::string_view text, float width) {
    std::vector<std::string> lines;
    std::string line;
    for(std::size_t i=0;i<text.size();) {
        if(text[i]=='\n') {lines.push_back(line);line.clear();++i;continue;}
        int bytes=0;
        const int codepoint=GetCodepointNext(text.data()+i,&bytes);
        const auto n=static_cast<std::size_t>(std::max(1,bytes));
        const std::string glyph(text.substr(i,n));
        if(!line.empty() && font.measure(line+glyph,kBodySize).x>width) {
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
struct TextLine { std::string text; bool emphasis=false; bool heading=false; };
std::vector<TextLine> wrap(const FontSet& font,std::string_view text,float width) {
    std::vector<TextLine> lines;
    while(!text.empty()) {
        const auto end=text.find('\n');
        auto paragraph=text.substr(0,end);
        const bool heading=paragraph.starts_with("#");
        if(heading) { const auto space=paragraph.find(' '); if(space!=std::string_view::npos) paragraph.remove_prefix(space+1); }
        const bool emphasis=paragraph.size()>=6 && paragraph.starts_with("***") && paragraph.ends_with("***");
        if(emphasis) {paragraph.remove_prefix(3);paragraph.remove_suffix(3);}
        if(paragraph.empty()) lines.push_back({{},false});
        else for(auto& line:wrap_plain(font,paragraph,width-(emphasis?7.0f:0.0f)))
            lines.push_back({std::move(line),emphasis,heading});
        if(end==std::string_view::npos) break;
        text.remove_prefix(end+1);
    }
    return lines;
}
void draw_emphasis(const FontSet& font,const std::string& text,Vector2 pos) {
    // Synthetic oblique keeps the existing CJK font coverage; a second pass adds weight.
    const float matrix[16]={1,0,0,0, -0.18f,1,0,0, 0,0,1,0, pos.x+4,pos.y,0,1};
    rlPushMatrix();rlMultMatrixf(matrix);
    font.draw(text,{0,0},kBodySize,Color{216,217,203,255});
    font.draw(text,{0.7f,0},kBodySize,Color{216,217,203,255});
    rlPopMatrix();
}
}
void ChronicleView::preview(int chapter,int ending,bool bottom) noexcept {
    open=true;chapter_=std::clamp(chapter,0,7);scroll_=bottom?1000000.0f:0.0f;
    max_scroll_=0;ending_=game::can_read_ending(choice,ending)?std::clamp(ending,0,2):0;appendix_=0;preview_emphasis_=false;pending_=game::ChronicleChoice::None;transition_=-1;
}
void ChronicleView::preview_appendix(int appendix,bool bottom,bool emphasis) noexcept {
    preview(0,0,bottom);appendix_=std::clamp(appendix,1,2);preview_emphasis_=emphasis;
}
void ChronicleView::present_choice(game::ChronicleChoice value) noexcept {
    choice=value;decision_enabled=false;pending_=game::ChronicleChoice::None;
    ending_=static_cast<int>(value);scroll_=0;transition_=0;open=true;
}
std::vector<std::string_view> ChronicleView::strings() {
    std::vector<std::string_view> out{"圣城档案指挥官日记第任守城指挥官篇按波次解锁尚未解锁返回滚轮翻阅记录节选改编本次远征已解锁继续守护放下武器返回日记结局一二回顾选择将决定本局走向无尽模式伪通关达成"};
    out.push_back("第70波你的命令决定这座城的命运本局继续无尽守城本局战役至此结束确认下令再想一想本局的选择已记录按回车继续本局结局回顾尚未作出选择");
    const auto add = [&](std::string_view text) {
        while(!text.empty()) {
            const auto end=text.find('\n');
            out.push_back(text.substr(0,end));
            if(end==std::string_view::npos) break;
            text.remove_prefix(end+1);
        }
    };
    add(game::kChronicleGuard); add(game::kChronicleRelease);
    add(game::kChronicleLeap);add(game::kChronicleSmiler);
    for(const auto text:game::kAppendixNotices) add(text);
    add("附录一根白羽损失清单一跃菲尼克斯小传微笑者副官陆衡小传尚无附录记录这根羽毛来自反复归来的白鸟。第60波的日记将说明它的来历。尚未获得这份记录。");
    for(const auto& e:game::kChronicle) {out.push_back(e.title);add(e.text);}
    return out;
}
void ChronicleView::update(Vector2 vp,int reached_wave) {
    auto l=layout(vp);
    if(!appendix_ && chapter_==7) l.body.height-=28;
    const auto mouse=GetMousePosition();
    if(transition_>=0) {
        transition_+=std::min(GetFrameTime(),0.1f);
        if(transition_>=4.5f || (transition_>=1.0f && IsKeyPressed(KEY_ENTER))) transition_=-1;
        return;
    }
    if(pending_!=game::ChronicleChoice::None) {
        const Rectangle confirm{vp.x/2-175,vp.y/2+70,165,42};
        const Rectangle back{vp.x/2+10,vp.y/2+70,165,42};
        if(IsKeyPressed(KEY_ESCAPE) || (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse,back))) pending_=game::ChronicleChoice::None;
        else if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse,confirm)) {
            requested_=pending_;pending_=game::ChronicleChoice::None;
        }
        return;
    }
    if(IsKeyPressed(KEY_ESCAPE)||IsKeyPressed(KEY_J)) {open=false;return;}
    scroll_=std::clamp(scroll_-GetMouseWheelMove()*64.0f,0.0f,max_scroll_);
    if(IsKeyPressed(KEY_DOWN)||IsKeyPressed(KEY_PAGE_DOWN)) scroll_=std::min(max_scroll_,scroll_+l.body.height*0.7f);
    if(IsKeyPressed(KEY_UP)||IsKeyPressed(KEY_PAGE_UP)) scroll_=std::max(0.0f,scroll_-l.body.height*0.7f);
    if(!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    if(CheckCollisionPointRec(mouse,l.close)) {open=false;return;}
    for(int i=0;i<8;++i) {
        const Rectangle r{l.panel.x+14,l.panel.y+112+static_cast<float>(i)*l.row,l.rail-28,l.row-4};
        if(CheckCollisionPointRec(mouse,r) && game::kChronicle[static_cast<std::size_t>(i)].wave<=reached_wave) {
            chapter_=i;ending_=0;appendix_=0;scroll_=0;max_scroll_=0;
        }
    }
    for(int i=1;i<=2;++i) {
        const bool visible=i==1?appendices.white_feather:appendices.loss_list;
        const Rectangle r{l.panel.x+14,l.panel.y+112+static_cast<float>(8+i)*l.row,l.rail-28,l.row-4};
        if(visible && CheckCollisionPointRec(mouse,r)) {appendix_=i;ending_=0;scroll_=0;max_scroll_=0;}
    }
    if(!appendix_ && chapter_==7 && reached_wave>=70) {
        const float y=l.panel.y+l.panel.height-64;
        const float button=std::min(165.0f,(l.body.width-12)/2);
        if(CheckCollisionPointRec(mouse,{l.body.x,y,button,36})) {
            if(decision_enabled) pending_=game::ChronicleChoice::Guard;
            else if(choice!=game::ChronicleChoice::None) {ending_=static_cast<int>(choice);scroll_=0;}
        }
        if(CheckCollisionPointRec(mouse,{l.body.x+button+12,y,button,36})) {
            if(decision_enabled) pending_=game::ChronicleChoice::Release;
            else {ending_=0;scroll_=0;}
        }
    }
}
void ChronicleView::draw(const FontSet& font,Vector2 vp,int reached_wave) {
    auto l=layout(vp);
    if(!appendix_ && chapter_==7) l.body.height-=28;
    if(!game::can_read_ending(choice,ending_)) ending_=0;
    if(pending_!=game::ChronicleChoice::None || transition_>=0) {
        const bool deciding=pending_!=game::ChronicleChoice::None;
        const auto selected=deciding?pending_:choice;
        const bool guard=selected==game::ChronicleChoice::Guard;
        DrawRectangle(0,0,static_cast<int>(vp.x),static_cast<int>(vp.y),Color{10,14,18,245});
        const Color accent=guard?Color{205,169,99,255}:Color{190,218,220,255};
        const auto centered=[&](const std::string& text,float y,float size,Color color) {
            font.draw(text,{(vp.x-font.measure(text,size).x)/2,y},size,color);
        };
        const float y=vp.y*0.5f;
        DrawLineEx({vp.x/2-150,y-115},{vp.x/2+150,y-115},1,accent);
        centered("第 70 波",y-160,20,accent);
        centered(deciding?"你的命令，决定这座城的命运": "本局的选择已记录",y-95,18,Color{161,169,171,255});
        centered(guard?"继续守护":"放下武器",y-40,34,accent);
        if(deciding) {
            centered(guard?"本局继续无尽守城":"本局战役至此结束",y+20,18,Color{219,218,208,255});
            for(int i=0;i<2;++i) {
                const Rectangle r{vp.x/2-175+185*static_cast<float>(i),y+70,165,42};
                DrawRectangleLinesEx(r,1,accent);
                font.draw(i==0?"确认下令":"再想一想",{r.x+40,r.y+11},18,accent);
            }
        } else {
            const float alpha=std::clamp(transition_/1.2f,0.0f,1.0f);
            centered(guard?"你下令迎击。":"你下令停止抵抗。",y+24,22,Fade(accent,alpha));
            if(transition_>1.8f) centered(guard?"循环继续。":"弓手放下弓，枪卫让开道路。",y+72,18,Fade(accent,std::clamp((transition_-1.8f)/1.2f,0.0f,1.0f)));
            centered("按回车继续",vp.y-55,16,Color{116,129,131,255});
        }
        return;
    }
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
        if(!appendix_ && i==chapter_) DrawRectangleRec({l.panel.x+14,y,l.rail-28,l.row-4},Color{68,66,51,255});
        const std::string label=std::to_string(e.wave)+" / "+(unlocked?std::string(e.title):"尚未解锁");
        font.draw(label,{l.panel.x+26,y+8},19,unlocked?Color{224,215,191,255}:Color{115,124,128,255});
    }
    const float section_y=l.panel.y+112+8*l.row;
    font.draw("附录",{l.panel.x+26,section_y+5},18,Color{157,170,172,255});
    if(!appendices.white_feather && !appendices.loss_list)
        font.draw("尚无附录记录",{l.panel.x+26,section_y+l.row+5},16,Color{115,124,128,255});
    for(int i=1;i<=2;++i) {
        if(!(i==1?appendices.white_feather:appendices.loss_list)) continue;
        const float y=section_y+static_cast<float>(i)*l.row;
        if(appendix_==i) DrawRectangleRec({l.panel.x+14,y,l.rail-28,l.row-4},Color{68,66,51,255});
        font.draw(i==1?"一根白羽":"损失清单",{l.panel.x+26,y+5},18,Color{224,215,191,255});
    }
    const auto& entry=game::kChronicle[static_cast<std::size_t>(chapter_)];
    const bool unlocked=entry.wave<=reached_wave;
    const bool appendix_readable=appendices.readable(appendix_,reached_wave);
    const std::string title=appendix_?(appendix_readable?(appendix_==1?"一跃 / 菲尼克斯小传":"微笑者 / 副官陆衡小传"):
        (appendix_==1 && appendices.white_feather?"一根白羽":"尚未解锁")):
        unlocked?(ending_==1?"继续守护 / 无尽模式":ending_==2?"放下武器":std::string(entry.title)):"尚未解锁";
    font.draw(title,{l.body.x,l.panel.y+98},26,Color{232,211,168,255});
    std::string body=unlocked?std::string(ending_==1?game::kChronicleGuard:ending_==2?game::kChronicleRelease:entry.text):
        "第 "+std::to_string(entry.wave)+" 波解锁";
    if(appendix_) body=appendix_readable?(appendix_==1?std::string(game::kChronicleLeap):game::smiler_for_choice(choice)):
        appendix_==1 && appendices.white_feather?"这根羽毛来自反复归来的白鸟。第 60 波的日记将说明它的来历。":"尚未获得这份记录。";
    const auto lines=wrap(font,body,l.body.width-20);
    std::vector<float> offsets;
    offsets.reserve(lines.size());
    float content_height=0;
    for(const auto& line:lines) {
        offsets.push_back(content_height);
        content_height+=line.text.empty()?kParagraphGap:line.heading?44.0f:kLineHeight;
    }
    max_scroll_=std::max(0.0f,content_height-l.body.height);
    if(preview_emphasis_) {
        for(std::size_t i=0;i<lines.size();++i) if(lines[i].emphasis) {
            scroll_=std::max(0.0f,offsets[i]-l.body.height*0.4f);break;
        }
        preview_emphasis_=false;
    }
    scroll_=std::clamp(scroll_,0.0f,max_scroll_);
    BeginScissorMode(static_cast<int>(l.body.x),static_cast<int>(l.body.y),static_cast<int>(l.body.width),static_cast<int>(l.body.height));
    for(std::size_t i=0;i<lines.size();++i) {
        const Vector2 pos{l.body.x,l.body.y+offsets[i]-scroll_};
        if(pos.y+kLineHeight<l.body.y || pos.y>l.body.y+l.body.height) continue;
        if(lines[i].heading) font.draw(lines[i].text,{pos.x,pos.y+8},20,Color{232,211,168,255});
        else if(lines[i].emphasis) draw_emphasis(font,lines[i].text,pos);
        else font.draw(lines[i].text,{pos.x,pos.y},kBodySize,Color{216,217,203,255});
    }
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
    if(!appendix_ && chapter_==7 && unlocked) {
        const std::array<std::string,2> labels{decision_enabled?"继续守护":"本局结局回顾",decision_enabled?"放下武器":"返回日记"};
        const float button=std::min(165.0f,(l.body.width-12)/2);
        for(int i=0;i<2;++i) {
            if(i==0 && !decision_enabled && choice==game::ChronicleChoice::None) continue;
            const float x=l.body.x+static_cast<float>(i)*(button+12);
            DrawRectangleLinesEx({x,bottom,button,36},1,Color{157,134,88,255});
            font.draw(labels[static_cast<std::size_t>(i)],{x+10,bottom+8},18,Color{225,211,177,255});
        }
    } else font.draw("滚轮翻阅 · J / Esc 返回",{l.body.x,bottom+5},18,Color{162,169,164,255});
    if(!appendix_ && chapter_==7 && unlocked) font.draw(decision_enabled?"选择将决定本局走向":"本局的选择已记录",{l.body.x,bottom-27},18,Color{162,169,164,255});
}
}
