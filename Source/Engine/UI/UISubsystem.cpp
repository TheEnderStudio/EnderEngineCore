#include <UI/UISubsystem.hpp>
#include <Core/Log.hpp>
#include <Utilities/FontLoader.hpp>
#include <cmath>

EE_NAMESPACE_UI_BEGIN

static void reverseStack(Stack<Uptr<UILayer>>& s) {
	Vector<Uptr<UILayer>> v; while(!s.empty()){v.push_back(std::move(s.top()));s.pop();}
	for(auto&p:v) s.push(std::move(p));
}

struct UISubsystem::Impl {
	Rendering::Render2DSubsystem* r2d=nullptr;
	Vector<UIControl*> controls;
	Stack<Uptr<UILayer>> layers;
	Stack<Vec4> maskStack; // x,y=pos, z,w=size (0,0,0,0 = no mask)
	bool reversed=false;
	Vector<UILayer> pendingPush;
	bool pendingPop=false;
	UITextInput* focusedTextInput=nullptr;
	F32 dt=0.016f;
	UInt32 screenW=1280,screenH=720;float mouseX=0,mouseY=0;bool mouseDown=false,mouseWasDown=false;
	Object* hovered=nullptr;bool afterPost=true;
	UICrosshair* crosshair=nullptr;

	Vec2 anchorOffset(UIAnchor a,Vec2 s)const{float sw=(float)screenW,sh=(float)screenH;switch(a){case UIAnchor::TopLeft:return Vec2(0,0);case UIAnchor::TopCenter:return Vec2(sw*0.5f-s.x*0.5f,0);case UIAnchor::TopRight:return Vec2(sw-s.x,0);case UIAnchor::MidLeft:return Vec2(0,sh*0.5f-s.y*0.5f);case UIAnchor::Center:return Vec2(sw*0.5f-s.x*0.5f,sh*0.5f-s.y*0.5f);case UIAnchor::MidRight:return Vec2(sw-s.x,sh*0.5f-s.y*0.5f);case UIAnchor::BotLeft:return Vec2(0,sh-s.y);case UIAnchor::BotCenter:return Vec2(sw*0.5f-s.x*0.5f,sh-s.y);case UIAnchor::BotRight:return Vec2(sw-s.x,sh-s.y);}return Vec2(0);}
	Vec2 resolvePos(const UIElementDesc&d)const{return d.position+anchorOffset(d.anchor,d.size);}
	bool hitTest(Vec2 p,Vec2 s)const{return mouseX>=p.x&&mouseX<=p.x+s.x&&mouseY>=p.y&&mouseY<=p.y+s.y;}

	void drawDefault(UIControl*c){if(c->desc.visible)r2d->drawRect(resolvePos(c->desc),c->desc.size,c->desc.color);}
	void drawDefaultB(UIButton*b, bool isActive=true){if(!b->desc.visible)return;Vec2 p=resolvePos(b->desc);bool h=isActive&&hitTest(p,b->desc.size);r2d->drawRect(p,b->desc.size,h?Vec4(b->desc.color.x*1.2f,b->desc.color.y*1.2f,b->desc.color.z*1.2f,b->desc.color.w):b->desc.color);if(!b->label.empty()&&b->fontSRV&&b->fontData)drawTxt(b->fontSRV,b->fontData,b->label,p,b->desc.size,Vec4(1));}
	void drawDefaultL(UILabel*l){if(!l->desc.visible||!l->fontSRV||!l->fontData)return;String t=l->text;if(t.empty())return;drawTxt(l->fontSRV,l->fontData,t,resolvePos(l->desc),l->desc.size,l->desc.color);}
	void drawDefaultP(UIPicture*p){if(!p->desc.visible||!p->textureSRV)return;Vec2 pos=resolvePos(p->desc);r2d->drawSprite({pos,p->desc.size,p->desc.color,0,p->textureSRV});}
	void drawDefaultTI(UITextInput*ti){
		if(!ti->desc.visible||!ti->fontSRV||!ti->fontData)return;
		Vec2 p=resolvePos(ti->desc);
		F32 bw=ti->borderWidth;
		r2d->drawRect(Vec2(p.x-bw,p.y-bw),Vec2(ti->desc.size.x+bw*2,ti->desc.size.y+bw*2),
			ti->focused?ti->borderFocused:ti->borderUnfocused);
		r2d->drawRect(p,ti->desc.size,ti->focused?Vec4(0.15f,0.15f,0.22f,0.92f):Vec4(0.12f,0.12f,0.16f,0.85f));
		F32 pad=ti->padding+bw;
		if(!ti->buffer.empty())drawTxt(ti->fontSRV,ti->fontData,ti->buffer,p+Vec2(pad,0),ti->desc.size-Vec2(pad*2,0),ti->textColor);
		if(ti->focused){
			F32 cw=2.0f;auto*f=static_cast<EnderEngine::Utilities::FontData*>(ti->fontData);
			F32 tw=0,curTw=0;
			for(size_t i=0;i<ti->buffer.size();i++){auto it=f->glyphs.find((unsigned char)ti->buffer[i]);F32 a=(it!=f->glyphs.end())?it->second.advance:(ti->fontSize*0.4f);tw+=a;if(i<ti->cursorPos)curTw+=a;}
			F32 areaW=ti->desc.size.x-pad*2;
			F32 ch=ti->desc.size.y*0.6f,cy=p.y+(ti->desc.size.y-ch)*0.5f,cx=p.x+pad+(areaW-tw)*0.5f+curTw;
			ti->cursorBlink=std::fmod(ti->cursorBlink+dt*3.0f,1.0f);
			if(ti->cursorBlink<0.5f)r2d->drawRect(Vec2(cx,cy),Vec2(cw,ch),Vec4(1));
		}
	}
	void drawTxt(void*srv,void*fd,const String&txt,Vec2 pos,Vec2 area,Vec4 col){
		auto*f=static_cast<EnderEngine::Utilities::FontData*>(fd);
		// Decode UTF-8 to codepoints
		Vector<UInt32> cps;
		for(size_t i=0;i<txt.size();){UInt32 cp=0;unsigned char c=(unsigned char)txt[i];
			if(c<0x80){cp=c;i+=1;}else if(c<0xE0){cp=((c&0x1F)<<6)|(txt[i+1]&0x3F);i+=2;}else if(c<0xF0){cp=((c&0x0F)<<12)|((txt[i+1]&0x3F)<<6)|(txt[i+2]&0x3F);i+=3;}else{cp=((c&0x07)<<18)|((txt[i+1]&0x3F)<<12)|((txt[i+2]&0x3F)<<6)|(txt[i+3]&0x3F);i+=4;}
			cps.push_back(cp);}
		// Calculate width
		F32 tw=0;for(auto cp:cps){auto it=f->glyphs.find(cp);tw+=(it!=f->glyphs.end())?it->second.advance:f->fontSize*0.4f;}
		F32 by=pos.y+(area.y+f->lineHeight*0.75f)*0.5f,px=pos.x+(area.x-tw)*0.5f;
		for(auto cp:cps){auto it=f->glyphs.find(cp);if(it==f->glyphs.end()){px+=f->fontSize*0.4f;continue;}auto&g=it->second;Vec2 gp(px+g.bearing.x,by-g.bearing.y);if(g.size.x>0&&g.size.y>0){Rendering::SpriteDesc sd;sd.position=gp;sd.size=g.size;sd.texture=srv;sd.color=col;sd.uvMin=g.uvMin;sd.uvMax=g.uvMax;r2d->drawSpriteUV(sd);}px+=g.advance;}
	}

	void drawControl(UIControl*c, bool isActive=true){
		if(!c->desc.visible)return;
		if(c->ctrlType==UIControl::Type::Button)drawDefaultB(static_cast<UIButton*>(c),isActive);
		else if(c->ctrlType==UIControl::Type::Label)drawDefaultL(static_cast<UILabel*>(c));
		else if(c->ctrlType==UIControl::Type::Picture)drawDefaultP(static_cast<UIPicture*>(c));
		else if(c->ctrlType==UIControl::Type::TextInput)drawDefaultTI(static_cast<UITextInput*>(c));
		else drawDefault(c);
	}
};

UISubsystem::UISubsystem():Subsystem("UI"),m_impl(std::make_unique<Impl>()){}
UISubsystem::~UISubsystem()=default;
void UISubsystem::initialize(Rendering::Render2DSubsystem*r,UInt32 w,UInt32 h){m_impl->r2d=r;m_impl->screenW=w;m_impl->screenH=h;}
void UISubsystem::setScreenSize(UInt32 w,UInt32 h){m_impl->screenW=w;m_impl->screenH=h;if(m_impl->r2d)m_impl->r2d->setScreenSize(w,h);}
void UISubsystem::setMousePos(float x,float y){m_impl->mouseX=x;m_impl->mouseY=y;}
void UISubsystem::setMouseDown(bool d){m_impl->mouseDown=d;}

void UISubsystem::inputChar(char c){
	UITextInput*ti=m_impl->focusedTextInput;
	if(!ti||c<32)return;
	ti->buffer.insert(ti->cursorPos,1,c);
	ti->cursorPos++;
}
void UISubsystem::inputBackspace(){
	UITextInput*ti=m_impl->focusedTextInput;
	if(!ti||ti->cursorPos==0)return;
	ti->buffer.erase(ti->cursorPos-1,1);
	ti->cursorPos--;
}
void UISubsystem::inputDelete(){
	UITextInput*ti=m_impl->focusedTextInput;
	if(!ti||ti->cursorPos>=ti->buffer.size())return;
	ti->buffer.erase(ti->cursorPos,1);
}
void UISubsystem::inputCursorLeft(){
	UITextInput*ti=m_impl->focusedTextInput;
	if(!ti||ti->cursorPos==0)return;
	ti->cursorPos--;
}
void UISubsystem::inputCursorRight(){
	UITextInput*ti=m_impl->focusedTextInput;
	if(!ti||ti->cursorPos>=ti->buffer.size())return;
	ti->cursorPos++;
}
void UISubsystem::inputSubmit(){
	UITextInput*ti=m_impl->focusedTextInput;
	if(!ti)return;
	UITextSubmitEvent ev;ev.control=ti;ev.text=ti->buffer;emit(ev);
}
UITextInput* UISubsystem::focusedTextInput()const{return m_impl->focusedTextInput;}
void UISubsystem::focusTextInput(UITextInput*ti){
	if(m_impl->focusedTextInput)m_impl->focusedTextInput->focused=false;
	m_impl->focusedTextInput=ti;
	if(ti){ti->focused=true;ti->cursorPos=ti->buffer.size();}
}
void UISubsystem::setDeltaTime(F32 d){m_impl->dt=d;}
void UISubsystem::setCrosshair(UICrosshair* ch){m_impl->crosshair=ch;}
void UISubsystem::setCrosshairStyle(CrosshairStyle s){if(m_impl->crosshair)m_impl->crosshair->style=s;}

void UISubsystem::pushLayer(const UILayer& L){
	if(m_impl->reversed){m_impl->pendingPush.push_back(L);return;}
	auto p=std::make_unique<UILayer>(L);for(auto*c:p->getControls()){registerObject(*c);m_impl->controls.push_back(c);}
	m_impl->layers.push(std::move(p));
}
void UISubsystem::popLayer(){
	if(m_impl->reversed){m_impl->pendingPop=true;return;}
	if(m_impl->layers.empty())return;
	auto&t=m_impl->layers.top();for(auto*c:t->getControls()){unregisterObject(*c);auto it=std::find(m_impl->controls.begin(),m_impl->controls.end(),c);if(it!=m_impl->controls.end())m_impl->controls.erase(it);}
	m_impl->layers.pop();
}

void UISubsystem::pushMask(Vec2 pos, Vec2 size) {
	m_impl->maskStack.push(Vec4(pos.x, pos.y, size.x, size.y));
	m_impl->r2d->setScissorRect(pos, size);
}
void UISubsystem::popMask() {
	auto& s = m_impl->maskStack;
	if (!s.empty()) s.pop();
	if (s.empty()) { m_impl->r2d->setScissorRect(Vec2(0), Vec2(0)); return; }
	auto& m = s.top();
	m_impl->r2d->setScissorRect(Vec2(m.x, m.y), Vec2(m.z, m.w));
}

UILayer* UISubsystem::topLayer(){
	return m_impl->layers.empty() ? nullptr : m_impl->layers.top().get();
}

Result<void,CoreError> UISubsystem::registerControl(UIControl*c){auto r=registerObject(*c);if(r.isOk())m_impl->controls.push_back(c);return r;}
void UISubsystem::unregisterControl(UIControl*c){unregisterObject(*c);auto it=std::find(m_impl->controls.begin(),m_impl->controls.end(),c);if(it!=m_impl->controls.end())m_impl->controls.erase(it);}

void UISubsystem::beginFrame(bool ap){
	auto&p=*m_impl;p.afterPost=ap;p.r2d->setAfterPostProcess(ap);p.r2d->begin();
	reverseStack(p.layers);p.reversed=true;
	auto*t=topLayer();if(!t){p.mouseWasDown=p.mouseDown;return;}
	Object*nh=nullptr;
	for(auto*c:t->getControls()){if(!c->desc.visible)continue;Vec2 pos=p.resolvePos(c->desc);if(p.hitTest(pos,c->desc.size))nh=c;if(p.mouseDown&&!p.mouseWasDown&&p.hitTest(pos,c->desc.size)){UIClickEvent ev;ev.control=c;ev.pressed=true;emit(ev);if(c->ctrlType==UIControl::Type::TextInput){if(p.focusedTextInput)p.focusedTextInput->focused=false;p.focusedTextInput=static_cast<UITextInput*>(c);p.focusedTextInput->focused=true;}else{if(p.focusedTextInput){p.focusedTextInput->focused=false;p.focusedTextInput=nullptr;}}}if(!p.mouseDown&&p.mouseWasDown&&p.hitTest(pos,c->desc.size)){UIClickEvent ev;ev.control=c;ev.pressed=false;
	emit(ev);}}
	if(nh!=p.hovered){if(p.hovered){UIHoverEvent ev;ev.control=p.hovered;ev.hovered=false;emit(ev);}if(nh){UIHoverEvent ev;ev.control=nh;ev.hovered=true;emit(ev);}p.hovered=nh;}
	if(p.mouseDown&&!p.mouseWasDown&&!nh){if(p.focusedTextInput){p.focusedTextInput->focused=false;p.focusedTextInput=nullptr;}}
	p.mouseWasDown=p.mouseDown;
}

void UISubsystem::endFrame(){
	auto&p=*m_impl;
	if(p.pendingPop){
		if(!p.layers.empty()){
			auto&t=p.layers.top();for(auto*c:t->getControls()){unregisterObject(*c);auto it=std::find(p.controls.begin(),p.controls.end(),c);if(it!=p.controls.end())p.controls.erase(it);}
			p.layers.pop();
		}
		p.pendingPop=false;
	}
	reverseStack(p.layers);
	{
		auto* active = topLayer();
		Stack<Uptr<UILayer>> tmp;
		while(!p.layers.empty()){
			auto&L=p.layers.top();
			if(L->visible && L->afterPostProcess==p.afterPost){if(L->bgColor.w>0)p.r2d->drawRect(Vec2(0,0),Vec2((float)p.screenW,(float)p.screenH),L->bgColor);
				for(auto*c:L->getControls()){if(!c->desc.visible)continue;if(c->desc.afterPostProcess!=p.afterPost)continue;UIDrawEvent ev;ev.control=c;ev.afterPostProcess=p.afterPost;ev.handled=false;emit(ev);if(!ev.handled)p.drawControl(c, L.get() == active);}
			}
			tmp.push(std::move(L));p.layers.pop();
		}
		while(!tmp.empty()){p.layers.push(std::move(tmp.top()));tmp.pop();}
	}
	// Restore: reverses back to normal
	reverseStack(p.layers);p.reversed=false;
	for(auto&L:p.pendingPush){auto pp=std::make_unique<UILayer>(std::move(L));for(auto*c:pp->getControls()){registerObject(*c);p.controls.push_back(c);}p.layers.push(std::move(pp));}
	p.pendingPush.clear();
	// Draw crosshair at screen center (always on top)
	if(p.crosshair && p.crosshair->style != CrosshairStyle::None) {
		Vec2 center((F32)p.screenW*0.5f, (F32)p.screenH*0.5f);
		if(p.crosshair->style == CrosshairStyle::Filled)
			p.r2d->drawFilledCircle(center, p.crosshair->radius, Vec4(1), 32);
		else
			p.r2d->drawOutlineCircle(center, p.crosshair->radius, p.crosshair->thickness, Vec4(1), 32);
	}
	p.r2d->end();
}

void UISubsystem::drawRect(const UIElementDesc&d){if(d.visible)m_impl->r2d->drawRect(m_impl->resolvePos(d),d.size,d.color);}
bool UISubsystem::drawButton(const UIElementDesc&d,const String&l){if(!d.visible)return false;Vec2 p=m_impl->resolvePos(d);bool h=m_impl->hitTest(p,d.size);m_impl->r2d->drawRect(p,d.size,h?Vec4(d.color.x*1.2f,d.color.y*1.2f,d.color.z*1.2f,d.color.w):d.color);(void)l;return h&&m_impl->mouseDown&&!m_impl->mouseWasDown;}

Result<void,CoreError> UISubsystem::onInitialize(){if(!m_impl->r2d){EError("UI:no R2D");return CoreError::OperationFailed;}EInfo("UI ready");return{};}
void UISubsystem::onShutdown(){m_impl->r2d=nullptr;}

EE_NAMESPACE_UI_END
