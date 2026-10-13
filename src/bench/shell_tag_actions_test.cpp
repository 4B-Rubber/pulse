#include "../app/shell_tag_actions.h"
#include <cstdio>
namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
namespace {
int failures=0;
void Check(bool ok,const char* name) { std::printf("[%s] %s\n",ok?"PASS":"FAIL",name); failures += !ok; }
}
int main() {
    using namespace pulse::app;
    PlacesCatalog places; places.persist=false;
    const auto id=places.CreateTag(L"private in-memory tag",0x123456);
    const std::wstring a=L"C:\\shell-tag-in-memory\\A.txt",b=L"C:\\shell-tag-in-memory\\B.txt";
    ShellTagActions actions;
    auto apply=[&] {
        size_t changed=0;
        for(const auto& request:GroupShellTagActions(actions.Drain())) {
            std::vector<TagAdsUpdate> updates; std::wstring name;
            changed += ApplyShellTagBatch(places,request,updates,name);
        }
        return changed;
    };
    auto state=[&] { return places.GetSelectionState(id,{a,b}); };
    actions.Push({id,a,ShellTagAction::Add,L"seed"}); apply();
    Check(state()==TagSelectionState::Mixed,"initial mixed selection established without filesystem writes");
    actions.Push({id,a,ShellTagAction::Add,L"add-A"}); apply();
    Sleep(450);
    actions.Push({id,b,ShellTagAction::Add,L"add-B"}); apply();
    Check(state()==TagSelectionState::All,"mixed selection adds uniformly despite more than 400ms launch gap");
    actions.Push({id,a,ShellTagAction::Add,L"again-A"});
    actions.Push({id,b,ShellTagAction::Add,L"again-B"});
    Check(apply()==0 && state()==TagSelectionState::All,"all-tagged add is idempotent instead of a reverse toggle");
    actions.Push({id,a,ShellTagAction::Remove,L"remove-A"}); apply();
    actions.Push({id,b,ShellTagAction::Remove,L"remove-B"}); apply();
    Check(state()==TagSelectionState::None,"explicit remove works for the complete selection in separate drains");
    Check(!actions.Push({id,a,ShellTagAction::Add,L"add-A"}) && actions.empty(),"retry of an earlier add ID cannot undo a later removal");
    actions.Push({id,b,ShellTagAction::Add,L"order-B"});
    actions.Push({id,a,ShellTagAction::Add,L"order-A"}); apply();
    Check(state()==TagSelectionState::All,"arrival order of selected files does not change explicit add outcome");
    actions.Push({id,a,ShellTagAction::Remove,L"rapid-remove"});
    actions.Push({id,a,ShellTagAction::Add,L"rapid-add"}); apply();
    Check(state()==TagSelectionState::All,"two rapid opposite actions remain distinct and preserve accepted order");
    actions.Push({id,a,ShellTagAction::Remove,L"last-remove"}); apply();
    Check(!actions.Push({id,a,ShellTagAction::Remove,L"last-remove"}) && apply()==0 && state()==TagSelectionState::Mixed,
        "duplicate operation ID is suppressed after its original application");
    actions.Push({id,a}); apply();
    Check(state()==TagSelectionState::All,"legacy request defaults to additive assignment");
    actions.Push({L"unknown-tag",a,ShellTagAction::Remove,L"unknown"});
    Check(apply()==0 && state()==TagSelectionState::All,"unknown tag cannot change existing assignments");
    Check(!actions.Push({id,L"",ShellTagAction::Add,L"empty"}) && actions.empty(),"empty source is rejected before queueing");
    Check(places.persist==false,"test never enables preference persistence or ADS dispatch");
    const auto grouped=GroupShellTagActions({{id,a,ShellTagAction::Add,L"one"},
        {id,b,ShellTagAction::Add,L"two"},{id,a,ShellTagAction::Remove,L"three"},
        {id,a,ShellTagAction::Add,L"four"}});
    Check(grouped.size()==3 && grouped[0].paths.size()==2 && grouped[1].action==ShellTagAction::Remove &&
        grouped[2].action==ShellTagAction::Add,"adjacent same actions share one persistence batch without merging opposite operations");
    return failures?1:0;
}
