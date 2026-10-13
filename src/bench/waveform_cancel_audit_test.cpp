#include "../ui/audio_waveform.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
int wmain(int argc,wchar_t** argv) {
    if(argc>1 && std::wstring_view(argv[1])==L"-hide_banner") {
        std::wstring args=GetCommandLineW();
        if(args.find(L"probe-stall")!=args.npos || args.find(L"pipe:1")!=args.npos) {Sleep(30000);return 0;}
        const char description[]="  Duration: 00:00:10.00, start: 0.000000, bitrate: 128 kb/s\n  Stream #0:0: Audio: pcm_s16le, 8000 Hz, mono, s16, 128 kb/s\n";
        DWORD written=0;WriteFile(GetStdHandle(STD_ERROR_HANDLE),description,sizeof(description)-1,&written,nullptr);return 1;
    }
    wchar_t self[32768]{};GetModuleFileNameW(nullptr,self,32768);
    int failures=0;
    for(const wchar_t* path: {L"probe-stall.wav",L"decode-stall.wav"}) {
        std::atomic<bool> stop{false}; int published=0;
        std::jthread cancel([&]{std::this_thread::sleep_for(std::chrono::milliseconds(250));stop=true;});
        const auto started=GetTickCount64();
        pulse::ui::FfmpegWaveform(self,path,stop,[&](const auto&,float){++published;});
        const auto elapsed=GetTickCount64()-started;
        const bool ok=elapsed<2500 && !published;
        printf("[%s] cancelled %ls returns in %llu ms without publishing\n",ok?"PASS":"FAIL",path,elapsed);failures+=!ok;
    }
    return failures?1:0;
}
