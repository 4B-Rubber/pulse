#include "../preview_host/psd_raster.h"
#include "../preview_host/office_doc_model.h"
#include "../preview_host/table_document.h"
#include "../preview_host/notebook_document.h"
#include "../preview_host/archive_listing.h"
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<unsigned char>;
int failures = 0;
std::filesystem::path root;
std::vector<std::filesystem::path> fixtures;
void Check(bool ok, const char* name) { printf("[%s] %s\n", ok?"PASS":"FAIL", name); failures += !ok; }
void Put(Bytes& b, uint64_t value, int n, bool big = false) {
    for(int i=0;i<n;++i) b.push_back(static_cast<unsigned char>(value >> ((big?n-1-i:i)*8)));
}
std::wstring Write(const wchar_t* name, const Bytes& bytes) {
    const auto path = root / name;
    std::ofstream f(path,std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    Check(f.good(),"write isolated format fixture");
    fixtures.push_back(path);
    return path.wstring();
}
std::wstring Write(const wchar_t* name, const std::string& text) { return Write(name,Bytes(text.begin(),text.end())); }
uint32_t Crc(const std::string& s) {
    uint32_t crc=0xffffffff;
    for(unsigned char ch:s) { crc ^= ch; for(int i=0;i<8;++i) crc=(crc>>1)^((crc&1)?0xedb88320:0); }
    return ~crc;
}
Bytes Zip(const std::vector<std::pair<std::string,std::string>>& entries,const Bytes& comment={}) {
    Bytes b,cd;
    for(const auto& [name,data]:entries) {
        const auto offset=b.size(); const uint32_t crc=Crc(data);
        Put(b,0x04034b50,4); Put(b,20,2); Put(b,0,2); Put(b,0,2); Put(b,0,4);
        Put(b,crc,4); Put(b,data.size(),4); Put(b,data.size(),4); Put(b,name.size(),2); Put(b,0,2);
        b.insert(b.end(),name.begin(),name.end()); b.insert(b.end(),data.begin(),data.end());
        Put(cd,0x02014b50,4); Put(cd,20,2); Put(cd,20,2); Put(cd,0,2); Put(cd,0,2); Put(cd,0,4);
        Put(cd,crc,4); Put(cd,data.size(),4); Put(cd,data.size(),4); Put(cd,name.size(),2);
        Put(cd,0,2); Put(cd,0,2); Put(cd,0,2); Put(cd,0,2); Put(cd,0,4); Put(cd,offset,4);
        cd.insert(cd.end(),name.begin(),name.end());
    }
    const auto offset=b.size(); b.insert(b.end(),cd.begin(),cd.end());
    Put(b,0x06054b50,4); Put(b,0,2); Put(b,0,2); Put(b,entries.size(),2); Put(b,entries.size(),2);
    Put(b,cd.size(),4); Put(b,offset,4); Put(b,comment.size(),2); b.insert(b.end(),comment.begin(),comment.end());
    return b;
}
void Psd() {
    auto header=[](bool psb,bool indexed) {
        Bytes b{'8','B','P','S'}; Put(b,psb?2:1,2,true); Put(b,0,6,true); Put(b,1,2,true);
        Put(b,1,4,true); Put(b,32,4,true); Put(b,8,2,true); Put(b,indexed?2:1,2,true);
        Put(b,indexed?768:0,4,true); return b;
    };
    std::vector<Bytes> decoded;
    for(int permutation=0;permutation<2;++permutation) {
        auto b=header(false,true); Bytes palette(768);
        const int red=permutation?2:0,blue=permutation?0:2;
        palette[red]=255; palette[256+1]=255; palette[512+blue]=255;
        b.insert(b.end(),palette.begin(),palette.end()); Put(b,0,4,true); Put(b,0,4,true); Put(b,0,2,true);
        for(int x=0;x<32;++x) b.push_back(static_cast<unsigned char>(x%2?blue:red));
        const auto path=Write(permutation?L"palette-swapped.psd":L"palette.psd",b);
        UINT w=0,h=0,stride=0,sw=0,sh=0; Bytes pixels; std::wstring error;
        Check(pulse::preview::RasterizePsdFile(path,16,false,pixels,w,h,stride,sw,sh,&error) &&
            w==16 && h==1 && pixels[0]==128 && pixels[1]==0 && pixels[2]==128,
            "indexed PSD averages colors rather than palette indexes");
        decoded.push_back(pixels);
        Check(pulse::preview::RasterizePsdFile(path,32,false,pixels,w,h,stride,sw,sh,&error) &&
            w==32 && pixels[2]==255 && pixels[4]==255,"indexed PSD preserves native-size colors");
    }
    Check(decoded[0]==decoded[1],"equivalent palette permutations produce identical downscales");
    for(int scenario=0;scenario<3;++scenario) {
        auto b=header(true,false); Put(b,0,4,true); Put(b,0,8,true); Put(b,1,2,true);
        if(scenario!=0) Put(b,scenario==1?0xffffffffu:33u,4,true);
        const auto path=Write(L"invalid-rle.psb",b); Bytes pixels; UINT w=0,h=0,stride=0,sw=0,sh=0; std::wstring error;
        Check(!pulse::preview::RasterizePsdFile(path,16,false,pixels,w,h,stride,sw,sh,&error) && !error.empty(),
            "PSB rejects missing RLE table, oversized row and truncated row before allocation");
    }
}
void Rtf() {
    for(const auto& body:{std::string("first\\page second\\sect third"),std::string("\\page first\\sect second"),
        std::string("first{\\*\\unknown \\page ignored}second")}) {
        const auto path=Write(L"pages.rtf","{\\rtf1\\ansi "+body+"}");
        std::wstring text,error; bool cut=false;
        Check(pulse::preview::ReadRtfText(path,text,&cut,&error) && !cut && text.find(L"second")!=std::wstring::npos,
            "RTF body continues across pages/sections and skips hidden destinations");
    }
    const auto path=Write(L"long.rtf","{\\rtf1 "+std::string(13000,'x')+"}");
    std::wstring text,error; bool cut=false;
    Check(pulse::preview::ReadRtfText(path,text,&cut,&error) && cut && text.size()==12000,
        "RTF body reports text cap");
}
void Tables() {
    using namespace pulse::preview;
    for(const std::string epoch:{"0","1","true"}) {
        const std::string workbook="<workbook><workbookPr date1904=\""+epoch+"\"/><sheets><sheet name=\"Dates\" r:id=\"r1\"/></sheets></workbook>";
        const std::string sheet="<worksheet><sheetData><row><c s=\"0\"><v>45000.5</v></c><c s=\"0\"><v>60</v></c><c s=\"0\"><v>1</v></c><c s=\"0\"><v>59</v></c><c s=\"0\"><v>61</v></c></row></sheetData></worksheet>";
        const auto path=Write(L"dates.xlsx",Zip({{"xl/workbook.xml",workbook},
            {"xl/_rels/workbook.xml.rels","<Relationships><Relationship Id=\"r1\" Target=\"worksheets/sheet1.xml\"/></Relationships>"},
            {"xl/styles.xml","<styleSheet><numFmts><numFmt numFmtId=\"164\" formatCode=\"yyyy-mm-dd hh:mm\"/></numFmts><cellXfs><xf numFmtId=\"164\"/></cellXfs></styleSheet>"},
            {"xl/worksheets/sheet1.xml",sheet}}));
        std::wstring payload; uint32_t bytes=0;
        Check(MakeXlsxTable(path,payload,bytes) && payload.find(epoch=="0"?L"2023-03-15 12:00":L"2027-03-16 12:00")!=std::wstring::npos,
            "XLSX honors default/1904 boolean epochs and fractional time");
        if(epoch=="0") Check(payload.find(L"1900-02-29")!=std::wstring::npos &&
            payload.find(L"1900-01-01")!=std::wstring::npos && payload.find(L"1900-02-28")!=std::wstring::npos &&
            payload.find(L"1900-03-01")!=std::wstring::npos,"Excel early date and fictional leap-day boundaries");
    }
    std::wstring payload; uint32_t bytes=0; bool cut=false; pulse::ipc::PreviewTextEncoding encoding{};
    const auto csv=Write(L"clipped.csv",std::string(2001,'a')+",tail\n");
    Check(MakeCsvTable(csv,L".csv",payload,bytes,cut,encoding) && cut,"CSV field cap marks incomplete content");
    const auto path=Write(L"clipped.xlsx",Zip({{"xl/workbook.xml","<workbook><sheets><sheet name=\"S\" r:id=\"r1\"/></sheets></workbook>"},
        {"xl/_rels/workbook.xml.rels","<Relationships><Relationship Id=\"r1\" Target=\"worksheets/s.xml\"/></Relationships>"},
        {"xl/worksheets/s.xml","<worksheet><sheetData><row><c t=\"inlineStr\"><is><t>"+std::string(2001,'a')+"</t></is></c></row></sheetData></worksheet>"}}));
    Check(MakeXlsxTable(path,payload,bytes) && payload.find(L"S\txlsx\tS\t1\t1\t1\t")!=std::wstring::npos,
        "XLSX field cap marks sheet incomplete");
}
void Notebook() {
    for(int scenario=0;scenario<3;++scenario) {
        std::string output=scenario==0?"short":std::string(6001,'x');
        if(scenario==2) { output.clear(); for(int i=0;i<122;++i) output+="line\\n"; }
        const auto path=Write(L"output.ipynb","{\"cells\":[{\"cell_type\":\"code\",\"source\":[\"print(1)\"],\"outputs\":[{\"output_type\":\"stream\",\"text\":[\""+output+"\"]}]}],\"nbformat\":4}");
        std::wstring payload; uint32_t bytes=0; bool cut=false; pulse::ipc::PreviewTextEncoding encoding{};
        Check(pulse::preview::MakeNotebookDocument(path,payload,bytes,cut,encoding) && cut==(scenario!=0),
            "notebook marks character/line clipping incomplete while short output stays complete");
    }
}
void Archives() {
    Bytes fake; Put(fake,0x06054b50,4); fake.resize(22);
    for(int scenario=0;scenario<4;++scenario) {
        Bytes b=Zip(scenario==0?std::vector<std::pair<std::string,std::string>>{}:
            std::vector<std::pair<std::string,std::string>>{{"real.txt","content"}},scenario==2?fake:Bytes{'o','k'});
        if(scenario==3) b.insert(b.begin(),{'M','Z',0,0,0,0});
        const auto path=Write(L"comment.zip",b); std::wstring listing,error; uint32_t bytes=0;
        Check(pulse::preview::MakeArchiveListing(path,listing,bytes,&error) &&
            (scenario==0 || listing.find(L"real.txt")!=std::wstring::npos),
            "ZIP empty/comment/fake EOCD/SFX candidates preserve real directory");
    }
    auto b=Zip({{"zip64.txt","content"}});
    const size_t end=b.size()-22;
    auto u32=[&](size_t at) { return uint32_t(b[at]) | uint32_t(b[at+1])<<8 | uint32_t(b[at+2])<<16 | uint32_t(b[at+3])<<24; };
    const uint32_t size=u32(end+12),offset=u32(end+16);
    b.resize(end);
    Put(b,0x06064b50,4); Put(b,44,8); Put(b,45,2); Put(b,45,2); Put(b,0,4); Put(b,0,4);
    Put(b,1,8); Put(b,1,8); Put(b,size,8); Put(b,offset,8);
    Put(b,0x07064b50,4); Put(b,0,4); Put(b,end,8); Put(b,1,4);
    Put(b,0x06054b50,4); Put(b,0,2); Put(b,0,2); Put(b,0xffff,2); Put(b,0xffff,2);
    Put(b,0xffffffff,4); Put(b,0xffffffff,4); Put(b,0,2);
    std::wstring listing,error; uint32_t bytes=0;
    Check(pulse::preview::MakeArchiveListing(Write(L"zip64.zip",b),listing,bytes,&error) &&
        listing.find(L"zip64.txt")!=std::wstring::npos,"ZIP64 locator and full record boundaries remain supported");
}
}
int wmain(int argc,wchar_t** argv) {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    root=std::filesystem::absolute(std::filesystem::path(L"bench_data")/(L"format-audit-"+std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    const std::wstring filter=argc>1?argv[1]:L"--all";
    if(filter==L"--all"||filter==L"--psd") Psd();
    if(filter==L"--all"||filter==L"--rtf") Rtf();
    if(filter==L"--all"||filter==L"--tables") Tables();
    if(filter==L"--all"||filter==L"--notebook") Notebook();
    if(filter==L"--all"||filter==L"--zip") Archives();
    for(const auto& path:fixtures) std::filesystem::remove(path);
    std::filesystem::remove(root);
    CoUninitialize();
    return failures?1:0;
}
