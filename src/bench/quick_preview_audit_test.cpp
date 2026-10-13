#include "../ui/quick_preview_window.h"
#include <objbase.h>
#include <commctrl.h>
#include <cstdio>
#include <thread>
namespace pulse::ui {
struct QuickPreviewAuditTest {
    static bool Check(bool ok,const char* label){printf("[%s] %s\n",ok?"PASS":"FAIL",label);fflush(stdout);return ok;}
    static void Pump(unsigned ms){const auto end=GetTickCount64()+ms;do{MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(5);}while(GetTickCount64()<end);}
    static void Store(QuickPreviewWindow& q,uint32_t frame,ThumbnailCache::Item item){
        auto& cache=q.thumbnails_;
        const auto pixels=q.RequestedPixelSize(q.ContentRect());
        const auto key=cache.Key(q.item_.path,pixels,q.item_.modified,q.item_.size,frame);
        std::lock_guard lock(cache.mutex_);
        if(auto it=cache.items_.find(key);it!=cache.items_.end()){cache.lru_.erase(it->second.lru_position);cache.items_.erase(it);}
        cache.pending_.erase(key);cache.queue_.clear();cache.lru_.push_front(key);item.lru_position=cache.lru_.begin();cache.items_[key]=std::move(item);
    }
    static bool Run(){
        OleInitialize(nullptr);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
        HWND owner=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        bool ok=owner!=nullptr;
        {
            QuickPreviewWindow q;ok &= Check(q.Initialize(owner,WM_APP+201,WM_APP+202),"initialize private Quick Look window");
            q.thumbnails_.running_=true;
            QuickPreviewItem item;item.path=L"C:\\PulseAuditFixtures\\workbook.xlsx";item.name=L"workbook.xlsx";item.modified=2;item.size=3;
            q.Show(item,false,WindowEffect::None,true);
            ThumbnailCache::Item workbook;workbook.kind=ipc::PreviewContentKind::Table;
            workbook.text=L"PULSETBL\t1\nW\t2\t1\t0\nS\txlsx\tSheet1\t1\t1\t0\t\t0\nR\tvalue\nS\txlsx\tSheet2\t0\t0\t0\tnot-loaded\t0\n";
            Store(q,0,std::move(workbook));q.Render();
            for (const auto kind : {QuickPreviewWindow::NativeKind::Table, QuickPreviewWindow::NativeKind::Tree}) {
                q.native_kind_=kind;
                const auto closed=q.ContentRect();
                q.OpenFind();
                RECT edit{};GetWindowRect(q.find_edit_,&edit);
                MapWindowPoints(nullptr,q.hwnd_,reinterpret_cast<POINT*>(&edit),2);
                ok &= Check(q.FindBarHeight()>0 && q.ContentRect().top>closed.top &&
                    edit.top>=q.FindBarRect().top && edit.bottom<=q.ContentRect().top,
                    kind==QuickPreviewWindow::NativeKind::Table ? "table search reserves space above content" :
                        "tree search reserves space above content");
                q.CloseFind();
                ok &= Check(q.ContentRect().top==closed.top && !IsWindowVisible(q.find_edit_),
                    "closing structured preview search restores content and hides edit");
            }
            q.native_kind_=QuickPreviewWindow::NativeKind::Table;
            ok &= Check(q.table_.Key(VK_NEXT,false,true)&&q.table_.IsSheetRequestPending(),"request nonfirst worksheet through table input");
            ThumbnailCache::Item failed;failed.failed=true;Store(q,1,std::move(failed));q.Render();
            ok &= Check(q.sheet_request_==1&&!q.table_.IsSheetRequestPending()&&q.table_.SelectedSheetIndex()==1,"production Render terminal failure ends correct sheet request after ResetAnimation");
            q.table_.Key(VK_PRIOR,false,true);q.Render();
            ok &= Check(q.table_.SelectedSheetIndex()==0 && !q.table_.IsSheetRequestPending(),
                "failed worksheet allows returning to the loaded first sheet");
            q.table_.Key(VK_NEXT,false,true);
            ThumbnailCache::Item recovered;recovered.kind=ipc::PreviewContentKind::Table;
            recovered.text=L"PULSETBL\t1\nW\t2\t1\t1\nS\txlsx\tSheet1\t0\t0\t0\tnot-loaded\t0\nS\txlsx\tSheet2\t1\t1\t0\t\t0\nR\trecovered\n";
            Store(q,1,std::move(recovered));q.Render();
            ok &= Check(q.table_.SelectedSheetIndex()==1 && !q.table_.IsSheetRequestPending() &&
                q.preview_text_.find(L"recovered")!=std::wstring::npos,
                "revisiting failed worksheet accepts successful retry through Render");
            item.path=L"C:\\PulseAuditFixtures\\retry.tiff";item.name=L"retry.tiff";q.Update(item);
            ThumbnailCache::Item retry;retry.failed=retry.transient=true;retry.retry_at=GetTickCount64()+150;Store(q,0,std::move(retry));q.Render();
            Pump(350);
            bool queued=false;{std::lock_guard lock(q.thumbnails_.mutex_);queued=!q.thumbnails_.queue_.empty();}
            ok &= Check(queued,"ordinary idle message loop wakes transient preview retry without manual paint");
            item.path=L"C:\\PulseAuditFixtures\\unicode.txt";item.name=L"unicode.txt";q.Update(item);
            const std::wstring unicode=L"A\U0001f600\U00020bb7e\u0301Z \u05d0\u05d1\u05d2";
            for (const float scale : {1.0f,1.5f,2.0f}) {
                q.scale_=scale;
                ThumbnailCache::Item text;text.kind=ipc::PreviewContentKind::Text;text.text=unicode;
                Store(q,0,std::move(text));q.Render();
                bool valid=q.native_kind_==QuickPreviewWindow::NativeKind::Text && q.text_layout_.get();
                bool boundaries[12]{};
                if (valid) {
                    // Exercise actual shaped glyph halves, including RTL, rather than guessed character widths.
                    q.text_layout_->SetFontSize(14.0f*scale,DWRITE_TEXT_RANGE{0,static_cast<UINT32>(unicode.size())});
                    for (UINT32 pos=0;pos<unicode.size();++pos) {
                        FLOAT x=0,y=0;DWRITE_HIT_TEST_METRICS metric{};
                        if (FAILED(q.text_layout_->HitTestTextPosition(pos,FALSE,&x,&y,&metric))) {valid=false;continue;}
                        for (const float part : {0.25f,0.75f}) {
                            uint32_t index=0;
                            const float px=q.TextOriginX()+metric.left+metric.width*part;
                            const float py=q.ContentRect().top+20.0f*scale-q.text_scroll_+metric.top+metric.height*0.5f;
                            if (!q.HitTestText(px,py,index) || index>unicode.size()) {valid=false;continue;}
                            // Forbidden boundaries split emoji, supplementary Han or the combining accent.
                            if (index==2 || index==4 || index==6) valid=false;
                            if (index<std::size(boundaries)) boundaries[index]=true;
                        }
                    }
                }
                ok &= Check(valid && boundaries[1] && boundaries[3] && boundaries[5] && boundaries[7],
                    scale==1.0f?"plain Quick Look Unicode cluster boundaries at 100% layout scale":
                    scale==1.5f?"plain Quick Look Unicode cluster boundaries at 150% layout scale":
                                "plain Quick Look Unicode cluster boundaries at 200% layout scale");
            }
            q.Close();const auto epoch=q.thumbnails_.epoch_.load();
            q.OnImagePackInstalled();q.OnExtraPackInstalled();q.OnMediaPackInstalled();
            ok &= Check(q.thumbnails_.epoch_.load()==epoch+3,"all installed-pack callbacks invalidate hidden Quick Look cache generations");
            q.thumbnails_.running_=false;
        }
        if(owner)DestroyWindow(owner);OleUninitialize();return ok;
    }
};
}
int RunQuickPreviewAuditTest(){
    const auto name=L"PulseQuickPreviewAudit-"+std::to_wstring(GetCurrentProcessId());
    HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);if(!desktop)return 1;
    bool ok=false;std::thread worker([&]{if(SetThreadDesktop(desktop))ok=pulse::ui::QuickPreviewAuditTest::Run();});worker.join();CloseDesktop(desktop);return ok?0:1;
}
