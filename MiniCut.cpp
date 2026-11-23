// MiniCut: native Windows video cutter, no FFmpeg, no Electron.
// Requires Visual Studio 2022 C++ toolset + Windows 10/11 SDK to build.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfplay.h>
#include <mferror.h>
#include <propvarutil.h>
#include <strsafe.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace {
constexpr LONGLONG HNS = 10000000LL;
constexpr UINT WM_EXPORT_DONE = WM_APP + 10;
constexpr UINT WM_EXPORT_PROGRESS = WM_APP + 11;
constexpr int ID_OPEN=100, ID_PLAY=101, ID_SPLIT=102, ID_REMOVE=103,
    ID_TRIM_START=104, ID_TRIM_END=105, ID_UNDO=106, ID_FAST=107, ID_EXACT=108,
    ID_NO_AUDIO=109;

template <typename T> struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p(o.p) { o.p=nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept { if(this!=&o) {reset();p=o.p;o.p=nullptr;}return *this; }
    T** put() { reset(); return &p; }
    T* get() const { return p; }
    T* operator->() const { return p; }
    operator bool() const {return p!=nullptr;}
    void reset() { if(p){p->Release();p=nullptr;} }
};

struct Span { LONGLONG first=0,last=0; };
struct MediaError { HRESULT code; const wchar_t* where; };
void Check(HRESULT hr, const wchar_t* where) { if(FAILED(hr)) throw MediaError{hr,where}; }
std::wstring ErrorText(const MediaError& e) {
    wchar_t msg[512];
    swprintf_s(msg, L"%s (0x%08X)", e.where, (unsigned)e.code);
    return msg;
}
LONGLONG ClampTime(LONGLONG t,LONGLONG limit) {return std::max(0LL,std::min(t,limit));}
std::wstring TimeLabel(LONGLONG t) {
    unsigned sec=(unsigned)(std::max(0LL,t)/HNS);
    wchar_t b[32];
    swprintf_s(b,L"%02u:%02u:%02u",sec/3600,(sec/60)%60,sec%60);
    return b;
}

HWND g_window=nullptr,g_video=nullptr,g_status=nullptr,g_time=nullptr,g_noAudio=nullptr;
HWND g_buttons[9]={};
HFONT g_font=nullptr;
IMFPMediaPlayer* g_player=nullptr;
std::wstring g_file;
std::vector<Span> g_kept;
std::vector<std::vector<Span>> g_history;
LONGLONG g_duration=0,g_cursor=0,g_dragStart=0,g_dragEnd=0;
int g_selected=-1;
bool g_playing=false,g_mouseDown=false,g_dragging=false,g_exporting=false;
bool g_clipMouseDown=false,g_clipDragging=false;
int g_dragClipIndex=-1,g_insertSlot=0;
POINT g_mousePt={};
RECT g_track={},g_montageTrack={};

void Status(const std::wstring& text) { if(g_status) SetWindowTextW(g_status,text.c_str()); }
void ShowFail(const wchar_t* title, const std::wstring& detail) {
    MessageBoxW(g_window,detail.c_str(),title,MB_OK|MB_ICONERROR);
}
void RedrawTrack() { if(g_window){InvalidateRect(g_window,&g_track,FALSE);InvalidateRect(g_window,&g_montageTrack,FALSE);} }
void SetUIBusy(bool busy) {
    g_exporting=busy;
    for(auto& b:g_buttons) if(b) EnableWindow(b,!busy);
    if(g_noAudio)EnableWindow(g_noAudio,!busy);
}

class PlayerCallback final:public IMFPMediaPlayerCallback {
    volatile LONG count_=1;
public:
    STDMETHODIMP QueryInterface(REFIID id, void** out) override {
        if(!out)return E_POINTER;
        *out=nullptr;
        if(id==__uuidof(IUnknown)||id==__uuidof(IMFPMediaPlayerCallback)) {
            *out=static_cast<IMFPMediaPlayerCallback*>(this); AddRef();return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {return (ULONG)InterlockedIncrement(&count_);}
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n=(ULONG)InterlockedDecrement(&count_);
        if(!n)delete this;
        return n;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
        if(!event||!g_window)return;
        if(FAILED(event->hrEvent)) PostMessageW(g_window,WM_EXPORT_PROGRESS,1001,(LPARAM)event->hrEvent);
    }
};
PlayerCallback* g_callback=nullptr;

void StopPlayer() {
    g_playing=false;
    if(g_player) {
        g_player->Shutdown();g_player->Release();g_player=nullptr;
    }
}
void SeekTo(LONGLONG t) {
    g_cursor=ClampTime(t,g_duration);
    if(g_player) {
        PROPVARIANT pos;PropVariantInit(&pos);
        pos.vt=VT_I8;pos.hVal.QuadPart=g_cursor;
        g_player->SetPosition(MFP_POSITIONTYPE_100NS,&pos);
    }
    if(g_time) SetWindowTextW(g_time,(TimeLabel(g_cursor)+L" / "+TimeLabel(g_duration)).c_str());
    RedrawTrack();
}
void OpenFile(const std::wstring& file) {
    if(g_exporting)return;
    try {
        ComPtr<IMFSourceReader> source;
        Check(MFCreateSourceReaderFromURL(file.c_str(),nullptr,source.put()),L"Не удалось открыть видео");
        PROPVARIANT duration; PropVariantInit(&duration);
        Check(source->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&duration),L"Не удалось узнать длительность");
        if(duration.vt!=VT_UI8&&duration.vt!=VT_I8) {
            PropVariantClear(&duration);
            throw MediaError{MF_E_INVALIDMEDIATYPE,L"Неизвестная длительность видео"};
        }
        const LONGLONG newDuration=(LONGLONG)duration.uhVal.QuadPart;
        PropVariantClear(&duration);
        ComPtr<IMFMediaType> videoType;
        Check(source->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,videoType.put()),L"Видео не найдено");
        if(newDuration<=0)throw MediaError{E_INVALIDARG,L"Видео пустое"};
        StopPlayer();
        g_file=file;g_duration=newDuration;g_cursor=0;
        g_kept={{0,g_duration}};g_history.clear();g_selected=0;
        g_mouseDown=false;g_clipMouseDown=false;g_clipDragging=false;g_dragging=false;
        if(!g_callback)g_callback=new PlayerCallback();
        HRESULT hr=MFPCreateMediaPlayer(file.c_str(),FALSE,0,g_callback,g_video,&g_player);
        if(FAILED(hr)) {g_player=nullptr;Status(L"Видео загружено, но предпросмотр недоступен");}
        else Status(L"Шкала «Исходник» — разрез и выделение; части на дорожке «Монтаж» можно перетаскивать.");
        SetWindowTextW(g_window,(L"MiniCut — "+file).c_str());
        SeekTo(0);
        InvalidateRect(g_window,nullptr,FALSE);
    }catch(const MediaError& e){ShowFail(L"Открытие файла",ErrorText(e));}
}

int SpanAt(LONGLONG t) {
    for(int i=0;i<(int)g_kept.size();++i) {
        if(t>=g_kept[i].first&&t<g_kept[i].last)return i;
    }
    return -1;
}
void Remember() {
    g_history.push_back(g_kept);
    if(g_history.size()>40)g_history.erase(g_history.begin());
}
void NormalizeSelection(){
    if(g_selected>=(int)g_kept.size())g_selected=(int)g_kept.size()-1;
    if(g_selected<0&&!g_kept.empty())g_selected=0;
}
void RemoveInterval(LONGLONG a,LONGLONG b){
    if(a>b)std::swap(a,b);
    if(b-a<HNS/100)return;
    Remember();
    std::vector<Span> rest;
    for(const auto& part:g_kept) {
        if(part.last<=a||part.first>=b){rest.push_back(part);continue;}
        if(part.first<a)rest.push_back({part.first,a});
        if(part.last>b)rest.push_back({b,part.last});
    }
    g_kept=std::move(rest);NormalizeSelection();
    g_dragging=false;Status(L"Фрагмент удалён. Ctrl+Z — отмена.");RedrawTrack();
}
void SplitHere(){
    if(g_file.empty()||g_exporting)return;
    int idx=SpanAt(g_cursor);
    if(idx<0||g_cursor<=g_kept[idx].first+HNS/100||g_cursor>=g_kept[idx].last-HNS/100)return;
    Remember();
    Span old=g_kept[idx];g_kept[idx].last=g_cursor;
    g_kept.insert(g_kept.begin()+idx+1,{g_cursor,old.last});
    g_selected=idx+1;Status(L"Разрезано. Перетащите часть в «Монтаже» или нажмите «Удалить».");RedrawTrack();
}
void RemoveSelected(){
    if(g_file.empty()||g_exporting)return;
    if(g_dragging){RemoveInterval(g_dragStart,g_dragEnd);return;}
    if(g_selected>=0&&g_selected<(int)g_kept.size()){
        Remember();g_kept.erase(g_kept.begin()+g_selected);
        NormalizeSelection();Status(L"Фрагмент удалён. Ctrl+Z — отмена.");RedrawTrack();
    }
}
void Undo(){
    if(g_history.empty()||g_exporting)return;
    g_kept=g_history.back();g_history.pop_back();NormalizeSelection();
    g_dragging=false;Status(L"Действие отменено.");RedrawTrack();
}

// A sample's true codec keyframe (CleanPoint) is required for stream-copy cuts.
std::vector<LONGLONG> FindKeyframes(const std::wstring& path){
    ComPtr<IMFSourceReader> reader;
    Check(MFCreateSourceReaderFromURL(path.c_str(),nullptr,reader.put()),L"Открытие для поиска ключевых кадров");
    Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),L"Выбор дорожек");
    Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE),L"Выбор видео");
    std::vector<LONGLONG> keys;
    while(true){
        DWORD index=0,flags=0;LONGLONG time=0;
        ComPtr<IMFSample> s;
        Check(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&index,&flags,&time,s.put()),L"Чтение ключевых кадров");
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
        if(!s)continue;
        UINT32 isKey=0;
        if(SUCCEEDED(s->GetUINT32(MFSampleExtension_CleanPoint,&isKey))&&isKey)keys.push_back(time);
    }
    std::sort(keys.begin(),keys.end());
    keys.erase(std::unique(keys.begin(),keys.end()),keys.end());
    if(keys.empty())throw MediaError{MF_E_INVALIDMEDIATYPE,L"В этом видео не найдены ключевые кадры для быстрого экспорта"};
    return keys;
}
LONGLONG NextKey(const std::vector<LONGLONG>& keys,LONGLONG position,LONGLONG duration){
    auto it=std::lower_bound(keys.begin(),keys.end(),position);
    return it==keys.end()?duration:*it;
}

void CopyAttribute(IMFAttributes* from,IMFAttributes* to,const GUID& guid) {
    PROPVARIANT p;PropVariantInit(&p);
    if(SUCCEEDED(from->GetItem(guid,&p))) {to->SetItem(guid,p);PropVariantClear(&p);}
}

// Two distinct paths:
// FAST: source compressed H264+AAC -> sink identical compressed formats (remux).
// EXACT: source decodes video to NV12 -> Windows H264 encoder; audio is copied AAC.
// Audio start/end precision in EXACT remains limited to individual AAC packets.
struct ExportResult {bool ok=false;std::wstring message;};
void ExportVideo(HWND notify,std::wstring input,std::wstring output,std::vector<Span> spans,
                 LONGLONG duration,bool exact,bool muteAudio){
    ExportResult* result=new ExportResult();
    HRESULT co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    bool coOk=SUCCEEDED(co);
    bool mfOk=false;
    try {
        Check(MFStartup(MF_VERSION),L"Инициализация Media Foundation"); mfOk=true;
        ComPtr<IMFSourceReader> reader;
        Check(MFCreateSourceReaderFromURL(input.c_str(),nullptr,reader.put()),L"Открытие исходного файла");
        ComPtr<IMFMediaType> nativeVideo,nativeAudio;
        Check(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nativeVideo.put()),L"Видео дорожка не найдена");
        GUID videoSubtype={};
        Check(nativeVideo->GetGUID(MF_MT_SUBTYPE,&videoSubtype),L"Формат видео");
        if(!exact&&videoSubtype!=MFVideoFormat_H264)
            throw MediaError{MF_E_INVALIDMEDIATYPE,L"Быстрый экспорт поддерживает только H.264"};

        bool hasAudio=!muteAudio &&
            SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,nativeAudio.put()));
        if(hasAudio){
            GUID audioSubtype={};
            if(FAILED(nativeAudio->GetGUID(MF_MT_SUBTYPE,&audioSubtype))||audioSubtype!=MFAudioFormat_AAC)
                throw MediaError{MF_E_INVALIDMEDIATYPE,L"Для сохранения звука нужен AAC в исходном файле"};
        }

        if(!exact){
            auto keys=FindKeyframes(input);
            std::vector<Span> aligned;
            for(auto p:spans){
                LONGLONG a=NextKey(keys,p.first,duration);
                LONGLONG b=NextKey(keys,p.last,duration);
                if(a<b)aligned.push_back({a,b});
            }
            spans=std::move(aligned);
            if(spans.empty())throw MediaError{E_INVALIDARG,L"После привязки к ключевым кадрам не осталось видео"};
        }
        Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),L"Выбор дорожек");
        Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE),L"Выбор видео дорожки");
        if(hasAudio)Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM,TRUE),L"Выбор звуковой дорожки");

        ComPtr<IMFMediaType> videoInput,videoOutput;
        if(exact){
            Check(MFCreateMediaType(videoInput.put()),L"Формат декодирования");
            Check(videoInput->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),L"Видео тип");
            Check(videoInput->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),L"NV12");
            Check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,videoInput.get()),L"Декодирование видео в NV12");
            videoInput.reset();
            Check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,videoInput.put()),L"Получение параметров NV12");
            Check(MFCreateMediaType(videoOutput.put()),L"Формат H.264");
            Check(videoOutput->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),L"Видео тип H.264");
            Check(videoOutput->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264),L"Видеокодек H.264");
            CopyAttribute(nativeVideo.get(),videoOutput.get(),MF_MT_FRAME_SIZE);
            CopyAttribute(nativeVideo.get(),videoOutput.get(),MF_MT_FRAME_RATE);
            CopyAttribute(nativeVideo.get(),videoOutput.get(),MF_MT_PIXEL_ASPECT_RATIO);
            videoOutput->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
            UINT32 bitrate=4000000;
            nativeVideo->GetUINT32(MF_MT_AVG_BITRATE,&bitrate);
            bitrate=std::min(30000000u,std::max(1000000u,bitrate));
            videoOutput->SetUINT32(MF_MT_AVG_BITRATE,bitrate);
        }else{
            videoInput.p=nativeVideo.p;videoInput.p->AddRef();
            videoOutput.p=nativeVideo.p;videoOutput.p->AddRef();
        }

        ComPtr<IMFSinkWriter> writer;
        Check(MFCreateSinkWriterFromURL(output.c_str(),nullptr,nullptr,writer.put()),L"Создание MP4");
        DWORD videoStream=0,audioStream=0;
        Check(writer->AddStream(videoOutput.get(),&videoStream),L"Добавление видео дорожки");
        Check(writer->SetInputMediaType(videoStream,videoInput.get(),nullptr),L"Настройка видеокодека");
        if(hasAudio){
            Check(writer->AddStream(nativeAudio.get(),&audioStream),L"Добавление аудио дорожки");
            Check(writer->SetInputMediaType(audioStream,nativeAudio.get(),nullptr),L"Настройка AAC");
        }
        Check(writer->BeginWriting(),L"Начало записи MP4");
        LONGLONG total=0;
        for(auto s:spans)total+=s.last-s.first;
        if(total<=0)throw MediaError{E_INVALIDARG,L"Нет фрагментов для сохранения"};
        LONGLONG offset=0;
        int lastPct=-1;
        for(const Span& part:spans){
            PROPVARIANT seek;PropVariantInit(&seek);seek.vt=VT_I8;seek.hVal.QuadPart=part.first;
            Check(reader->SetCurrentPosition(GUID_NULL,seek),L"Перемещение по файлу");
            bool videoEnd=false,audioEnd=!hasAudio;
            bool firstVideo=true,firstAudio=true;
            while(!videoEnd||!audioEnd){
                DWORD stream=0,flags=0;LONGLONG t=0;
                ComPtr<IMFSample> sample;
                Check(reader->ReadSample(MF_SOURCE_READER_ANY_STREAM,0,&stream,&flags,&t,sample.put()),L"Чтение видео и звука");
                if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
                bool isVideo=(stream==MF_SOURCE_READER_FIRST_VIDEO_STREAM);
                bool isAudio=(stream==MF_SOURCE_READER_FIRST_AUDIO_STREAM);
                // Actual indexes may be 0/1 rather than FIRST_* sentinel constants.
                if(!isVideo&&!isAudio){
                    ComPtr<IMFMediaType> streamType;
                    if(SUCCEEDED(reader->GetCurrentMediaType(stream,streamType.put()))){
                        GUID major={};
                        if(SUCCEEDED(streamType->GetGUID(MF_MT_MAJOR_TYPE,&major))){
                            isVideo=(major==MFMediaType_Video);
                            isAudio=(major==MFMediaType_Audio);
                        }
                    }
                }
                if(!isVideo&&!isAudio)continue;
                if(isAudio&&!hasAudio)continue;
                if(t>=part.last){if(isVideo)videoEnd=true;else audioEnd=true;continue;}
                if(t<part.first||!sample)continue;
                if((isVideo&&videoEnd)||(isAudio&&audioEnd))continue;
                LONGLONG newTime=offset+(t-part.first);
                Check(sample->SetSampleTime(newTime),L"Установка времени кадра");
                LONGLONG sampleLength=0;
                if(SUCCEEDED(sample->GetSampleDuration(&sampleLength))&&sampleLength>0 && t+sampleLength>part.last)
                    sample->SetSampleDuration(part.last-t);
                UINT64 decode=0;
                if(SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp,&decode))){
                    const LONGLONG adjusted=offset+((LONGLONG)decode-part.first);
                    if(adjusted>=0)sample->SetUINT64(MFSampleExtension_DecodeTimestamp,(UINT64)adjusted);
                }
                if(isVideo&&firstVideo){sample->SetUINT32(MFSampleExtension_Discontinuity,TRUE);firstVideo=false;}
                if(isAudio&&firstAudio){sample->SetUINT32(MFSampleExtension_Discontinuity,TRUE);firstAudio=false;}
                Check(writer->WriteSample(isVideo?videoStream:audioStream,sample.get()),L"Запись кадра/звука");
                int pct=(int)(100.0*(double)(offset+std::min(t-part.first,part.last-part.first))/(double)total);
                if(pct!=lastPct){lastPct=pct;PostMessageW(notify,WM_EXPORT_PROGRESS,pct,0);}
            }
            offset+=part.last-part.first;
        }
        Check(writer->Finalize(),L"Завершение MP4");
        result->ok=true;
        result->message=muteAudio ?
            (exact?L"Точный экспорт завершён. Видео H.264 без звуковой дорожки.":L"Быстрый экспорт завершён. Видео без звука (по ключевым кадрам).") :
            (exact?L"Точный экспорт завершён. Видео H.264, звук AAC сохранён.":L"Быстрый экспорт завершён. Потоки сохранены без перекодирования (границы по ключевым кадрам).");
    }catch(const MediaError& e){result->message=ErrorText(e);}
    catch(...){result->message=L"Неизвестная ошибка экспорта";}
    if(!result->ok)DeleteFileW(output.c_str());
    if(mfOk)MFShutdown();
    if(coOk)CoUninitialize();
    PostMessageW(notify,WM_EXPORT_DONE,0,(LPARAM)result);
}

std::wstring FileDialog(bool save){
    wchar_t path[MAX_PATH]={};
    if(save){
        wcscpy_s(path,L"обрезанное_видео.mp4");
    }
    OPENFILENAMEW o={};o.lStructSize=sizeof(o);o.hwndOwner=g_window;
    o.lpstrFile=path;o.nMaxFile=MAX_PATH;
    o.lpstrFilter=L"Видео MP4 (*.mp4)\0*.mp4\0Все файлы (*.*)\0*.*\0";
    o.nFilterIndex=1;o.lpstrDefExt=L"mp4";
    o.Flags=OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    BOOL ok=save?GetSaveFileNameW(&o):GetOpenFileNameW(&o);
    return ok?std::wstring(path):std::wstring();
}
void StartExport(bool exact){
    if(g_exporting||g_file.empty()||g_kept.empty())return;
    std::wstring output=FileDialog(true);
    if(output.empty())return;
    if(_wcsicmp(output.c_str(),g_file.c_str())==0){
        ShowFail(L"Сохранение",L"Нельзя перезаписывать исходное видео. Выберите другой файл.");return;
    }
    bool muteAudio=g_noAudio && SendMessageW(g_noAudio,BM_GETCHECK,0,0)==BST_CHECKED;
    SetUIBusy(true);
    Status(exact?L"Точный экспорт: кодирование кадр за кадром…":L"Быстрый экспорт: поиск ключевых кадров…");
    std::thread([win=g_window,src=g_file,dst=output,parts=g_kept,len=g_duration,exact,muteAudio](){
        ExportVideo(win,src,dst,parts,len,exact,muteAudio);
    }).detach();
}


LONGLONG MontageDuration(){
    LONGLONG sum=0;
    for(const Span& span:g_kept)sum+=std::max(0LL,span.last-span.first);
    return sum;
}
int MontageX(LONGLONG offset){
    LONGLONG total=MontageDuration();
    if(total<=0)return g_montageTrack.left;
    double f=(double)offset/(double)total;
    return g_montageTrack.left+(int)(f*(g_montageTrack.right-g_montageTrack.left));
}
RECT MontageClipRect(size_t index){
    LONGLONG offset=0;
    for(size_t i=0;i<index && i<g_kept.size();++i)
        offset+=g_kept[i].last-g_kept[i].first;
    int left=MontageX(offset);
    if(index>=g_kept.size())
        return {left,g_montageTrack.top,g_montageTrack.right,g_montageTrack.bottom};
    int right=MontageX(offset+g_kept[index].last-g_kept[index].first);
    if(index+1==g_kept.size())right=g_montageTrack.right;
    return {left,g_montageTrack.top+2,right,g_montageTrack.bottom-2};
}
int MontageClipAtX(int x){
    if(x<g_montageTrack.left||x>=g_montageTrack.right)return -1;
    for(size_t i=0;i<g_kept.size();++i){
        RECT r=MontageClipRect(i);
        if(x>=r.left&&x<r.right)return (int)i;
    }
    return -1;
}
int MontageInsertSlot(int x){
    for(size_t i=0;i<g_kept.size();++i){
        RECT r=MontageClipRect(i);
        if(x<(r.left+r.right)/2)return (int)i;
    }
    return (int)g_kept.size();
}
void MoveClip(int from,int slot){
    int n=(int)g_kept.size();
    if(from<0||from>=n||g_exporting)return;
    slot=std::max(0,std::min(slot,n));
    int destination=slot>from?slot-1:slot;
    if(destination==from){RedrawTrack();return;}
    Remember();
    Span selected=g_kept[from];
    g_kept.erase(g_kept.begin()+from);
    g_kept.insert(g_kept.begin()+destination,selected);
    g_selected=destination;
    Status(L"Порядок частей изменён. Экспорт сохранит этот порядок. Ctrl+Z — отмена.");
    SeekTo(selected.first);
}
RECT TrackRect(){
    RECT r;GetClientRect(g_window,&r);
    return {20,r.bottom-156,r.right-20,r.bottom-143};
}
RECT MontageTrackRect(){
    RECT r;GetClientRect(g_window,&r);
    return {20,r.bottom-113,r.right-20,r.bottom-68};
}
LONGLONG PosFromX(int x){
    int span=std::max(1,static_cast<int>(g_track.right-g_track.left));
    double f=(double)(x-g_track.left)/span;
    return ClampTime((LONGLONG)(std::max(0.0,std::min(1.0,f))*g_duration),g_duration);
}
int XFromPos(LONGLONG t){
    if(g_duration<=0)return g_track.left;
    double frac=(double)t/(double)g_duration;
    return g_track.left+(int)(frac*(g_track.right-g_track.left));
}
void DrawTimeline(HDC dc){
    if(g_duration<=0)return;
    RECT track=g_track;
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(194,203,216));
    RECT sourceLabel={track.left,track.top-23,track.right,track.top-1};
    DrawTextW(dc,L"Исходник: щелчок — перемотка, протяжка — выделение участка",-1,
              &sourceLabel,DT_SINGLELINE|DT_LEFT|DT_VCENTER);
    HBRUSH bg=CreateSolidBrush(RGB(75,42,47));FillRect(dc,&track,bg);DeleteObject(bg);
    for(size_t i=0;i<g_kept.size();++i){
        RECT r={XFromPos(g_kept[i].first),track.top,XFromPos(g_kept[i].last),track.bottom};
        if(r.right<=r.left)continue;
        HBRUSH fill=CreateSolidBrush((int)i==g_selected?RGB(62,151,207):RGB(76,111,151));
        FillRect(dc,&r,fill);DeleteObject(fill);
    }
    if(g_dragging){
        int a=XFromPos(g_dragStart),b=XFromPos(g_dragEnd);if(a>b)std::swap(a,b);
        HBRUSH overlay=CreateHatchBrush(HS_DIAGCROSS,RGB(250,180,70));
        RECT sel={a,track.top,b,track.bottom};
        if(sel.right>sel.left){SetBkMode(dc,TRANSPARENT);FillRect(dc,&sel,overlay);}DeleteObject(overlay);
    }
    int x=XFromPos(g_cursor);
    HPEN pen=CreatePen(PS_SOLID,2,RGB(255,229,116));HPEN old=(HPEN)SelectObject(dc,pen);
    MoveToEx(dc,x,track.top-3,nullptr);LineTo(dc,x,track.bottom+3);
    SelectObject(dc,old);DeleteObject(pen);
    RECT montage=g_montageTrack;
    SetTextColor(dc,RGB(194,203,216));
    RECT montageLabel={montage.left,montage.top-24,montage.right,montage.top-2};
    DrawTextW(dc,L"Монтаж: перетащите часть мышью, чтобы поменять порядок экспорта",-1,
              &montageLabel,DT_SINGLELINE|DT_LEFT|DT_VCENTER);
    HBRUSH montageBg=CreateSolidBrush(RGB(58,64,75));
    FillRect(dc,&montage,montageBg);DeleteObject(montageBg);
    for(size_t i=0;i<g_kept.size();++i){
        RECT r=MontageClipRect(i);
        if(r.right<=r.left)continue;
        HBRUSH fill=CreateSolidBrush((int)i==g_selected?RGB(62,151,207):RGB(76,111,151));
        FillRect(dc,&r,fill);DeleteObject(fill);
        HPEN div=CreatePen(PS_SOLID,2,RGB(25,29,36));
        HPEN previous=(HPEN)SelectObject(dc,div);
        MoveToEx(dc,r.left,montage.top,nullptr);LineTo(dc,r.left,montage.bottom);
        SelectObject(dc,previous);DeleteObject(div);
        if(r.right-r.left>47){
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
            std::wstring name=L"Часть "+std::to_wstring(i+1);
            RECT txt=r;txt.left+=3;txt.right-=3;
            DrawTextW(dc,name.c_str(),-1,&txt,DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_END_ELLIPSIS);
        }
    }
    if(g_clipDragging){
        int slot=std::max(0,std::min(g_insertSlot,(int)g_kept.size()));
        int pos=slot==(int)g_kept.size()?montage.right:MontageClipRect(slot).left;
        HPEN highlight=CreatePen(PS_SOLID,3,RGB(255,229,116));
        HPEN previous=(HPEN)SelectObject(dc,highlight);
        MoveToEx(dc,pos,montage.top-3,nullptr);LineTo(dc,pos,montage.bottom+3);
        SelectObject(dc,previous);DeleteObject(highlight);
    }
}
void Relayout(){
    if(!g_window)return;
    RECT client;GetClientRect(g_window,&client);
    int w=client.right,h=client.bottom;
    if(g_video)MoveWindow(g_video,20,53,w-40,std::max(110,h-331),TRUE);
    int row1=h-252;
    int sizes[]={107,120,116,116,134,127,95};
    int x=20;
    for(int i=0;i<7;++i){if(g_buttons[i])MoveWindow(g_buttons[i],x,row1,sizes[i],32,TRUE);x+=sizes[i]+8;}
    if(g_time)MoveWindow(g_time,20,h-207,w-40,30,TRUE);
    g_track=TrackRect();
    g_montageTrack=MontageTrackRect();
    if(g_buttons[7])MoveWindow(g_buttons[7],w-302,h-51,138,34,TRUE);
    if(g_buttons[8])MoveWindow(g_buttons[8],w-154,h-51,134,34,TRUE);
    if(g_noAudio)MoveWindow(g_noAudio,20,h-51,194,34,TRUE);
    if(g_status)MoveWindow(g_status,222,h-46,std::max(60,w-555),30,TRUE);
    RedrawTrack();
}
HWND MakeButton(int idx,int id,const wchar_t* caption){
    HWND h=CreateWindowExW(0,L"BUTTON",caption,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        0,0,90,30,g_window,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(h,WM_SETFONT,(WPARAM)g_font,TRUE);
    g_buttons[idx]=h;return h;
}

LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        g_window=hwnd;
        DragAcceptFiles(hwnd,TRUE);
        g_font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_video=CreateWindowExW(WS_EX_CLIENTEDGE,L"STATIC",L"Откройте видео",
            WS_CHILD|WS_VISIBLE|SS_BLACKRECT,0,0,100,100,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        MakeButton(0,ID_OPEN,L"Открыть MP4");MakeButton(1,ID_PLAY,L"▶ / Пауза");
        MakeButton(2,ID_SPLIT,L"✂ Разрезать");MakeButton(3,ID_REMOVE,L"Удалить");
        MakeButton(4,ID_TRIM_START,L"Убрать до курсора");
        MakeButton(5,ID_TRIM_END,L"Убрать после");MakeButton(6,ID_UNDO,L"↶ Отмена");
        MakeButton(7,ID_FAST,L"⚡ Быстро");MakeButton(8,ID_EXACT,L"◎ Точно");
        g_noAudio=CreateWindowW(L"BUTTON",L"Экспорт без звука",
            WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,194,32,
            hwnd,(HMENU)(INT_PTR)ID_NO_AUDIO,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(g_noAudio,WM_SETFONT,(WPARAM)g_font,TRUE);
        g_time=CreateWindowW(L"STATIC",L"00:00:00 / 00:00:00",WS_CHILD|WS_VISIBLE,0,0,100,20,hwnd,nullptr,nullptr,nullptr);
        g_status=CreateWindowW(L"STATIC",L"Откройте MP4. Можно перетащить файл в окно.",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,100,20,hwnd,nullptr,nullptr,nullptr);
        SendMessageW(g_time,WM_SETFONT,(WPARAM)g_font,TRUE);
        SendMessageW(g_status,WM_SETFONT,(WPARAM)g_font,TRUE);
        Relayout();SetTimer(hwnd,1,110,nullptr);return 0;
    }
    case WM_GETMINMAXINFO:{
        auto p=(MINMAXINFO*)lp;p->ptMinTrackSize.x=930;p->ptMinTrackSize.y=570;return 0;
    }
    case WM_SIZE:Relayout();return 0;
    case WM_DROPFILES:{
        HDROP drop=(HDROP)wp;wchar_t p[MAX_PATH]={};
        if(DragQueryFileW(drop,0,p,MAX_PATH))OpenFile(p);
        DragFinish(drop);return 0;
    }
    case WM_LBUTTONDOWN:{
        POINT p={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(g_file.empty()||g_exporting)break;
        if(PtInRect(&g_montageTrack,p)){
            int index=MontageClipAtX(p.x);
            if(index>=0){
                g_clipMouseDown=true;g_clipDragging=false;
                g_dragClipIndex=index;g_insertSlot=index;g_mousePt=p;g_selected=index;
                SetCapture(hwnd);RedrawTrack();return 0;
            }
        }
        if(!PtInRect(&g_track,p))break;
        g_mouseDown=true;g_dragging=false;g_dragStart=g_dragEnd=PosFromX(p.x);g_mousePt=p;
        g_cursor=g_dragStart;g_selected=SpanAt(g_cursor);SetCapture(hwnd);RedrawTrack();return 0;
    }
    case WM_MOUSEMOVE:{
        int x=GET_X_LPARAM(lp);
        if(g_clipMouseDown){
            if(abs(x-g_mousePt.x)>5)g_clipDragging=true;
            if(g_clipDragging)g_insertSlot=MontageInsertSlot(x);
            RedrawTrack();return 0;
        }
        if(!g_mouseDown)break;
        if(abs(x-g_mousePt.x)>5)g_dragging=true;
        g_dragEnd=PosFromX(x);g_cursor=g_dragEnd;RedrawTrack();return 0;
    }
    case WM_LBUTTONUP:{
        if(g_clipMouseDown){
            int source=g_dragClipIndex,slot=g_insertSlot;
            bool moved=g_clipDragging;
            g_clipMouseDown=false;g_clipDragging=false;g_dragClipIndex=-1;
            ReleaseCapture();
            if(moved)MoveClip(source,slot);
            else if(source>=0&&source<(int)g_kept.size())SeekTo(g_kept[source].first);
            RedrawTrack();return 0;
        }
        if(!g_mouseDown)break;
        g_mouseDown=false;ReleaseCapture();
        if(!g_dragging){SeekTo(g_dragStart);}
        else {g_cursor=g_dragEnd;Status(L"Диапазон выделен. Нажмите «Удалить», чтобы вырезать его.");RedrawTrack();}
        return 0;
    }
    case WM_KEYDOWN:{
        if(wp=='Z'&&(GetKeyState(VK_CONTROL)&0x8000)){Undo();return 0;}
        if((wp==VK_LEFT||wp==VK_RIGHT)&&(GetKeyState(VK_CONTROL)&0x8000) && g_selected>=0){
            MoveClip(g_selected,wp==VK_LEFT?g_selected-1:g_selected+2);return 0;
        }
        if(wp==VK_DELETE){RemoveSelected();return 0;}
        if(wp==VK_SPACE){SendMessageW(hwnd,WM_COMMAND,ID_PLAY,0);return 0;}
        break;
    }
    case WM_COMMAND:{
        switch(LOWORD(wp)){
        case ID_OPEN:{auto p=FileDialog(false);if(!p.empty())OpenFile(p);break;}
        case ID_PLAY:
            if(g_player){
                if(g_playing){if(SUCCEEDED(g_player->Pause()))g_playing=false;}
                else {if(SUCCEEDED(g_player->Play()))g_playing=true;}
            }break;
        case ID_SPLIT:SplitHere();break;
        case ID_REMOVE:RemoveSelected();break;
        case ID_TRIM_START:if(!g_file.empty()&&!g_exporting)RemoveInterval(0,g_cursor);break;
        case ID_TRIM_END:if(!g_file.empty()&&!g_exporting)RemoveInterval(g_cursor,g_duration);break;
        case ID_UNDO:Undo();break;
        case ID_FAST:StartExport(false);break;
        case ID_EXACT:StartExport(true);break;
        }
        return 0;
    }
    case WM_TIMER:{
        if(!g_player||!g_playing||g_file.empty())break;
        PROPVARIANT pos;PropVariantInit(&pos);
        if(SUCCEEDED(g_player->GetPosition(MFP_POSITIONTYPE_100NS,&pos))&&pos.vt==VT_I8){
            LONGLONG t=pos.hVal.QuadPart;
            if(t<g_duration){
                int idx=SpanAt(t);
                if(idx<0){
                    auto it=std::min_element(g_kept.begin(),g_kept.end(),[t](Span a,Span b){
                        const LONGLONG nextA=a.first>t?a.first:LLONG_MAX;
                        const LONGLONG nextB=b.first>t?b.first:LLONG_MAX;
                        return nextA<nextB;
                    });
                    if(it!=g_kept.end()&&it->first>t)SeekTo(it->first);
                    else {g_player->Pause();g_playing=false;}
                }else{
                    g_cursor=t;
                    SetWindowTextW(g_time,(TimeLabel(t)+L" / "+TimeLabel(g_duration)).c_str());RedrawTrack();
                }
            }
        }
        PropVariantClear(&pos);
        return 0;
    }
    case WM_EXPORT_PROGRESS:
        if(wp==1001){Status(L"Предпросмотр: Media Foundation сообщила об ошибке.");}
        else if(g_exporting){Status(L"Экспорт: "+std::to_wstring(wp)+L"% …");}
        return 0;
    case WM_EXPORT_DONE:{
        std::unique_ptr<ExportResult> r((ExportResult*)lp);
        SetUIBusy(false);
        Status(r->message);
        if(r->ok)MessageBoxW(hwnd,r->message.c_str(),L"MiniCut — сохранено",MB_OK|MB_ICONINFORMATION);
        else ShowFail(L"Не удалось сохранить MP4",r->message);
        return 0;
    }
    case WM_PAINT:{
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);
        RECT client;GetClientRect(hwnd,&client);
        HBRUSH bg=CreateSolidBrush(RGB(29,34,43));FillRect(dc,&client,bg);DeleteObject(bg);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(219,227,238));
        HFONT previous=(HFONT)SelectObject(dc,g_font);
        RECT title={20,16,client.right-20,45};
        DrawTextW(dc,L"MiniCut   ·   видео без FFmpeg",-1,&title,DT_SINGLELINE|DT_VCENTER);
        if(g_duration){DrawTimeline(dc);}
        SelectObject(dc,previous);EndPaint(hwnd,&ps);return 0;
    }
    case WM_CLOSE:
        if(g_exporting){
            MessageBoxW(hwnd,L"Сначала дождитесь окончания экспорта, чтобы файл MP4 сохранился корректно.",
                        L"MiniCut",MB_OK|MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(hwnd);return 0;
    case WM_DESTROY:
        KillTimer(hwnd,1);StopPlayer();
        if(g_callback){g_callback->Release();g_callback=nullptr;}
        if(g_font){DeleteObject(g_font);g_font=nullptr;}
        PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
} // namespace

int APIENTRY wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
    HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(co))return 1;
    HRESULT mf=MFStartup(MF_VERSION);
    if(FAILED(mf)){CoUninitialize();return 2;}
    WNDCLASSEXW klass={};klass.cbSize=sizeof(klass);klass.hInstance=instance;
    klass.lpszClassName=L"MiniCutNativeVideoEditor";klass.lpfnWndProc=WndProc;
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    klass.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    klass.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClassExW(&klass);
    g_window=CreateWindowExW(0,klass.lpszClassName,L"MiniCut — лёгкий видеоредактор",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,1000,680,
        nullptr,nullptr,instance,nullptr);
    if(!g_window){MFShutdown();CoUninitialize();return 3;}
    ShowWindow(g_window,show);UpdateWindow(g_window);
    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    MFShutdown();CoUninitialize();return (int)msg.wParam;
}
