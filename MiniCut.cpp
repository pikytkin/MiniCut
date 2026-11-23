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
#include <commctrl.h>
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
#include <cstring>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <limits>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace {
constexpr LONGLONG HNS = 10000000LL;
constexpr UINT WM_EXPORT_DONE = WM_APP + 10;
constexpr UINT WM_EXPORT_PROGRESS = WM_APP + 11;
constexpr UINT WM_PLAYER_EVENT = WM_APP + 12;
constexpr int ID_OPEN=100, ID_PLAY=101, ID_SPLIT=102, ID_REMOVE=103,
    ID_TRIM_START=104, ID_TRIM_END=105, ID_UNDO=106, ID_SAVE=107,
    ID_NO_AUDIO=109, ID_LOAD_AUDIO=110;

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

HWND g_window=nullptr,g_video=nullptr,g_status=nullptr,g_time=nullptr,g_noAudio=nullptr,g_loadAudio=nullptr;
HWND g_buttons[8]={};
UINT32 g_previewWidth=0,g_previewHeight=0;
int g_spinnerStep=0,g_exportPercent=-1;
void Relayout();
HFONT g_font=nullptr;
IMFPMediaPlayer* g_player=nullptr;
IMFPMediaPlayer* g_audioPlayer=nullptr;
std::wstring g_file,g_audioFile;
std::vector<Span> g_kept,g_audioKept;
struct EditState {
    std::vector<Span> video,audio;
    int videoSelected=-1,audioSelected=-1;
    bool focusAudio=false;
};
std::vector<EditState> g_history;
LONGLONG g_duration=0,g_audioDuration=0,g_cursor=0,g_dragStart=0,g_dragEnd=0;
int g_selected=-1,g_selectedAudio=-1,g_previewClip=-1,g_previewAudioClip=-1;
bool g_playing=false,g_dragging=false,g_exporting=false,g_muteOriginal=false;
bool g_playerReady=false,g_audioReady=false,g_userWantsPlay=false,g_previewPriming=false;
ULONGLONG g_primeSince=0;
LONGLONG g_primeSource=0;
bool g_clipMouseDown=false,g_clipDragging=false,g_dragAudio=false,g_focusAudio=false,g_rangeSelect=false;
int g_dragClipIndex=-1,g_insertSlot=0;
POINT g_mousePt={};
RECT g_montageTrack={},g_audioTrack={};
LONGLONG MontageDuration();
int MontageX(LONGLONG t);
void SeekTo(LONGLONG t,bool keepAudioContinuous=false);
void UpdatePlayButton();
void BeginStillPreview();

void Status(const std::wstring& text) { if(g_status) SetWindowTextW(g_status,text.c_str()); }
void ShowFail(const wchar_t* title, const std::wstring& detail) {
    MessageBoxW(g_window,detail.c_str(),title,MB_OK|MB_ICONERROR);
}
void RedrawTrack() {
    if(!g_window)return;
    // Invalidate the full vertical cursor: it also crosses the gap BETWEEN tracks.
    RECT dirty={g_montageTrack.left-4,g_montageTrack.top-9,
                g_audioTrack.right+4,g_audioTrack.bottom+9};
    InvalidateRect(g_window,&dirty,TRUE);
}
void SetUIBusy(bool busy) {
    g_exporting=busy;
    for(auto& b:g_buttons) if(b) EnableWindow(b,!busy);
    if(g_noAudio)EnableWindow(g_noAudio,!busy);
    if(g_loadAudio)EnableWindow(g_loadAudio,!busy);
    if(!busy)g_exportPercent=-1;
    if(g_window)InvalidateRect(g_window,nullptr,FALSE);
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
        if(FAILED(event->hrEvent)){
            PostMessageW(g_window,WM_EXPORT_PROGRESS,1001,(LPARAM)event->hrEvent);
            return;
        }
        if(event->eEventType==MFP_EVENT_TYPE_MEDIAITEM_SET ||
           event->eEventType==MFP_EVENT_TYPE_PLAYBACK_ENDED){
            // MFPlay is asynchronous. Handle readiness on the editor's UI thread.
            PostMessageW(g_window,WM_PLAYER_EVENT,(WPARAM)event->eEventType,
                         (LPARAM)event->pMediaPlayer);
        }
    }
};
PlayerCallback* g_callback=nullptr;

struct ClipPosition {
    int index=-1;
    LONGLONG source=0,offset=0;
};
LONGLONG ClipsDuration(const std::vector<Span>& clips){
    LONGLONG total=0;
    for(const Span& p:clips)total+=p.last-p.first;
    return total;
}
ClipPosition LocateClip(const std::vector<Span>& clips,LONGLONG t){
    LONGLONG offset=0;
    for(int i=0;i<(int)clips.size();++i){
        const Span& s=clips[i];
        LONGLONG length=s.last-s.first;
        if(t<offset+length)
            return {i,s.first+std::max(0LL,t-offset),offset};
        offset+=length;
    }
    return {-1,0,offset};
}
void StopAudioPlayer(){
    g_audioReady=false;
    if(g_audioPlayer){g_audioPlayer->Shutdown();g_audioPlayer->Release();g_audioPlayer=nullptr;}
    g_previewAudioClip=-1;
}
void UpdatePlayButton(){
    if(g_buttons[1])
        SetWindowTextW(g_buttons[1],g_playing?L"⏸ Пауза":L"▶ Пуск");
}
void StopPlayer() {
    g_playing=false;g_playerReady=false;g_userWantsPlay=false;g_previewPriming=false;
    StopAudioPlayer();
    if(g_player){g_player->Shutdown();g_player->Release();g_player=nullptr;}
    g_previewClip=-1;UpdatePlayButton();
}
void SetPlayerPosition(IMFPMediaPlayer* player,LONGLONG time){
    if(!player)return;
    PROPVARIANT pos;PropVariantInit(&pos);
    pos.vt=VT_I8;pos.hVal.QuadPart=std::max(0LL,time);
    player->SetPosition(MFP_POSITIONTYPE_100NS,&pos);
}
void BeginStillPreview(){
    if(!g_player||!g_playerReady||g_playing)return;
    ClipPosition clip=LocateClip(g_kept,g_cursor);
    if(clip.index<0)return;
    g_primeSource=clip.source;g_primeSince=GetTickCount64();
    if(g_previewPriming)return;
    // Render the first frame in a short muted burst. Seeking while stopped
    // cannot reliably display a video frame in the MFPlay renderer.
    g_player->SetVolume(0.0f);
    g_previewPriming=true;
    if(FAILED(g_player->Play())){
        g_previewPriming=false;
        g_player->SetVolume(g_muteOriginal?0.0f:1.0f);
        Status(L"Не удалось обновить кадр предпросмотра.");
    }
}
void StartPlayback(){
    if(!g_player){Status(L"Сначала откройте MP4.");return;}
    if(!g_playerReady){
        g_userWantsPlay=true;
        Status(L"Видео загружается. Воспроизведение начнётся автоматически.");
        return;
    }
    if(g_cursor>=MontageDuration())SeekTo(0);
    if(g_previewPriming){
        // Already rendering muted preview frames; keep the current playback
        // running instead of issuing a duplicate asynchronous Play request.
        g_previewPriming=false;
    }else if(FAILED(g_player->Play())){
        Status(L"Плеер ещё готовится к воспроизведению.");
        g_userWantsPlay=true;return;
    }
    g_userWantsPlay=false;g_playing=true;
    g_player->SetVolume(g_muteOriginal?0.0f:1.0f);
    ClipPosition audio=LocateClip(g_audioKept,g_cursor);
    if(g_audioPlayer&&g_audioReady&&audio.index>=0){
        SetPlayerPosition(g_audioPlayer,audio.source);
        g_audioPlayer->Play();
    }
    UpdatePlayButton();
}
void TogglePlayback(){
    if(!g_player){Status(L"Сначала откройте MP4.");return;}
    if(g_playing){
        g_playing=false;g_previewPriming=false;g_userWantsPlay=false;
        g_player->Pause();
        if(g_audioPlayer)g_audioPlayer->Pause();
        UpdatePlayButton();
    }else StartPlayback();
}
void SeekTo(LONGLONG t,bool keepAudioContinuous) {
    g_cursor=ClampTime(t,MontageDuration());
    ClipPosition v=LocateClip(g_kept,g_cursor);
    g_previewClip=v.index;
    if(g_player&&g_playerReady&&v.index>=0){
        SetPlayerPosition(g_player,v.source);
        if(!g_playing)BeginStillPreview();
    }
    ClipPosition a=LocateClip(g_audioKept,g_cursor);
    int oldAudioClip=g_previewAudioClip;
    g_previewAudioClip=a.index;
    if(g_audioPlayer&&g_audioReady){
        // Video edits can jump in source time while the imported audio is continuous.
        // Do not seek or restart audio unless the AUDIOTRACK itself has a cut.
        bool audioChanged=!keepAudioContinuous || oldAudioClip!=a.index;
        if(a.index>=0){
            if(audioChanged)SetPlayerPosition(g_audioPlayer,a.source);
            if(g_playing && audioChanged)g_audioPlayer->Play();
        }else if(audioChanged)g_audioPlayer->Pause();
    }
    if(g_time)SetWindowTextW(g_time,(TimeLabel(g_cursor)+L" / "+TimeLabel(MontageDuration())).c_str());
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
        UINT32 nativeWidth=0,nativeHeight=0;
        MFGetAttributeSize(videoType.get(),MF_MT_FRAME_SIZE,&nativeWidth,&nativeHeight);
        StopPlayer();
        g_previewWidth=nativeWidth;g_previewHeight=nativeHeight;
        g_file=file;g_duration=newDuration;g_cursor=0;
        g_audioFile.clear();g_audioKept.clear();g_audioDuration=0;
        g_kept={{0,g_duration}};g_history.clear();g_selected=0;g_selectedAudio=-1;
        g_clipMouseDown=false;g_clipDragging=false;g_dragging=false;g_focusAudio=false;
        if(!g_callback)g_callback=new PlayerCallback();
        g_playerReady=false;g_userWantsPlay=false;g_previewPriming=false;
        HRESULT hr=MFPCreateMediaPlayer(file.c_str(),FALSE,0,g_callback,g_video,&g_player);
        if(FAILED(hr)){g_player=nullptr;Status(L"Видео загружено, но предпросмотр недоступен");}
        else Status(L"Перетаскивайте клипы мышью. Shift+протяжка — выделить диапазон для удаления.");
        SetWindowTextW(g_window,(L"MiniCut — "+file).c_str());
        Relayout();SeekTo(0);InvalidateRect(g_window,nullptr,FALSE);
        // MEDIAITEM_SET will set the first frame once the item is ready.
    }catch(const MediaError& e){ShowFail(L"Открытие файла",ErrorText(e));}
}
void OpenAudioFile(const std::wstring& file){
    if(g_exporting||g_file.empty())return;
    try{
        ComPtr<IMFSourceReader> source;
        Check(MFCreateSourceReaderFromURL(file.c_str(),nullptr,source.put()),L"Не удалось открыть аудио");
        ComPtr<IMFMediaType> type;
        Check(source->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,type.put()),L"Аудиодорожка не найдена");
        PROPVARIANT duration;PropVariantInit(&duration);
        Check(source->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&duration),
              L"Не удалось определить длину аудио");
        LONGLONG len=(LONGLONG)duration.uhVal.QuadPart;
        PropVariantClear(&duration);
        if(len<=0)throw MediaError{E_INVALIDARG,L"Аудиофайл пуст"};
        StopAudioPlayer();
        g_audioReady=false;
        g_audioFile=file;g_audioDuration=len;
        // Separate audio timeline starts at zero; extra material beyond the video is ignored.
        g_audioKept={{0,std::min(len,MontageDuration())}};
        g_selectedAudio=0;g_focusAudio=true;g_dragging=false;g_history.clear();
        if(!g_callback)g_callback=new PlayerCallback();
        HRESULT hr=MFPCreateMediaPlayer(file.c_str(),FALSE,0,g_callback,nullptr,&g_audioPlayer);
        if(FAILED(hr)){g_audioPlayer=nullptr;Status(L"Аудио загружено для экспорта, но предпросмотр звука недоступен.");}
        else Status(L"Аудио добавлено. Его можно разрезать, удалять и переставлять отдельно.");
        SeekTo(g_cursor);
    }catch(const MediaError& e){ShowFail(L"Открытие аудио",ErrorText(e));}
}

void Remember(){
    g_history.push_back({g_kept,g_audioKept,g_selected,g_selectedAudio,g_focusAudio});
    if(g_history.size()>50)g_history.erase(g_history.begin());
}
void NormalizeSelection(){
    if(g_selected>=(int)g_kept.size())g_selected=(int)g_kept.size()-1;
    if(g_selectedAudio>=(int)g_audioKept.size())g_selectedAudio=(int)g_audioKept.size()-1;
    if(g_selected<0&&!g_kept.empty())g_selected=0;
    if(g_selectedAudio<0&&!g_audioKept.empty())g_selectedAudio=0;
}
void RemoveInterval(LONGLONG a,LONGLONG b){
    if(g_exporting)return;
    if(a>b)std::swap(a,b);
    if(b-a<HNS/100)return;
    Remember();
    auto& parts=g_focusAudio?g_audioKept:g_kept;
    std::vector<Span> rest;
    LONGLONG offset=0;
    for(const Span& part:parts){
        LONGLONG len=part.last-part.first;
        LONGLONG localA=std::max(0LL,std::min(len,a-offset));
        LONGLONG localB=std::max(0LL,std::min(len,b-offset));
        if(localA>0)rest.push_back({part.first,part.first+localA});
        if(localB<len)rest.push_back({part.first+localB,part.last});
        offset+=len;
    }
    parts=std::move(rest);NormalizeSelection();
    g_dragging=false;
    SeekTo(std::min(a,MontageDuration()));
    Status(g_focusAudio?L"Удалён аудиодиапазон. Ctrl+Z — отмена.":L"Удалён видеофрагмент. Ctrl+Z — отмена.");
}
void SplitHere(){
    if(g_file.empty()||g_exporting)return;
    auto& parts=g_focusAudio?g_audioKept:g_kept;
    ClipPosition at=LocateClip(parts,g_cursor);
    if(at.index<0||at.source<=parts[at.index].first+HNS/100||
       at.source>=parts[at.index].last-HNS/100)return;
    Remember();
    Span old=parts[at.index];parts[at.index].last=at.source;
    parts.insert(parts.begin()+at.index+1,{at.source,old.last});
    if(g_focusAudio)g_selectedAudio=at.index+1;else g_selected=at.index+1;
    Status(g_focusAudio?L"Аудиофрагмент разрезан.":L"Видео разрезано. Перетащите часть мышью.");
    RedrawTrack();
}
void RemoveSelected(){
    if(g_file.empty()||g_exporting)return;
    if(g_dragging){RemoveInterval(g_dragStart,g_dragEnd);return;}
    auto& parts=g_focusAudio?g_audioKept:g_kept;
    int selected=g_focusAudio?g_selectedAudio:g_selected;
    if(selected>=0&&selected<(int)parts.size()){
        Remember();parts.erase(parts.begin()+selected);
        NormalizeSelection();SeekTo(std::min(g_cursor,MontageDuration()));
        Status(g_focusAudio?L"Аудиофрагмент удалён. Ctrl+Z — отмена.":L"Видеофрагмент удалён. Ctrl+Z — отмена.");
    }else{
        Status(L"Сначала выделите фрагмент на видео- или аудиодорожке.");
    }
}
void Undo(){
    if(g_exporting)return;
    if(g_history.empty()){Status(L"Нет действий для отмены.");return;}
    EditState x=std::move(g_history.back());g_history.pop_back();
    g_kept=std::move(x.video);g_audioKept=std::move(x.audio);
    g_selected=x.videoSelected;g_selectedAudio=x.audioSelected;g_focusAudio=x.focusAudio;
    NormalizeSelection();g_dragging=false;
    SeekTo(std::min(g_cursor,MontageDuration()));Status(L"Действие отменено.");
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

#include "MiniCutAudio.h"
#include "MiniCutVideo.h"
// FAST remuxes H.264; EXACT decodes video to RGB32 before encoding.
// Imported audio is mixed with original audio in PCM and encoded to AAC.

struct ExportResult {bool ok=false;std::wstring message;};
void ExportVideo(HWND notify,std::wstring input,std::wstring output,std::vector<Span> spans,
                 LONGLONG duration,bool exact,bool muteAudio,
                 std::wstring externalAudio,std::vector<Span> audioSpans){
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

        bool mixAudio=!externalAudio.empty();
        bool hasAudio=!muteAudio && !mixAudio &&
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
        UINT32 frameW=0,frameH=0;
        if(exact){
            Check(MFCreateMediaType(videoInput.put()),L"Формат декодирования");
            Check(videoInput->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),L"Видео тип");
            Check(videoInput->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),L"NV12");
            Check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,videoInput.get()),L"Декодирование в NV12");
            videoInput.reset();
            Check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,videoInput.put()),L"Получение типа NV12");
            // Normalize packed stride for H264 encoder. Decoder's NV12 buffers may
            // have padded rows and must not be forwarded unchanged.
            Check(MFGetAttributeSize(videoInput.get(),MF_MT_FRAME_SIZE,&frameW,&frameH),L"Размер видеокадра");
            Check(MFCreateMediaType(videoOutput.put()),L"Формат H.264");
            Check(videoOutput->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),L"Видео тип H.264");
            Check(videoOutput->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264),L"Видеокодек H.264");
            CopyAttribute(videoInput.get(),videoOutput.get(),MF_MT_FRAME_SIZE);
            CopyAttribute(videoInput.get(),videoOutput.get(),MF_MT_FRAME_RATE);
            CopyAttribute(videoInput.get(),videoOutput.get(),MF_MT_PIXEL_ASPECT_RATIO);
            CopyAttribute(nativeVideo.get(),videoOutput.get(),MF_MT_VIDEO_PRIMARIES);
            CopyAttribute(nativeVideo.get(),videoOutput.get(),MF_MT_TRANSFER_FUNCTION);
            CopyAttribute(nativeVideo.get(),videoOutput.get(),MF_MT_YUV_MATRIX);
            CopyAttribute(nativeVideo.get(),videoOutput.get(),MF_MT_VIDEO_NOMINAL_RANGE);
            videoOutput->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
            UINT32 bitrate=4000000;
            nativeVideo->GetUINT32(MF_MT_AVG_BITRATE,&bitrate);
            bitrate=std::min(30000000u,std::max(1000000u,bitrate));
            videoOutput->SetUINT32(MF_MT_AVG_BITRATE,bitrate);
        }else{
            videoInput.p=nativeVideo.p;videoInput.p->AddRef();
            videoOutput.p=nativeVideo.p;videoOutput.p->AddRef();
        }

        ComPtr<IMFMediaType> encoderInput;
        if(exact){
            Check(MFCreateMediaType(encoderInput.put()),L"Тип входа видеокодировщика");
            Check(videoInput->CopyAllItems(encoderInput.get()),L"Параметры видеокодировщика");
            if(!frameW||!frameH||(frameW&1)||(frameH&1))
                throw MediaError{MF_E_INVALIDMEDIATYPE,L"Точный экспорт требует чётную ширину и высоту"};
            // Decoder stride may be aligned to GPU pitch. The new sample is packed.
            Check(encoderInput->SetUINT32(MF_MT_DEFAULT_STRIDE,frameW),L"Шаг строк NV12");
            Check(encoderInput->SetUINT32(MF_MT_SAMPLE_SIZE,(UINT32)((UINT64)frameW*frameH*3/2)),
                  L"Размер упакованного NV12");
        }
        ComPtr<IMFSinkWriter> writer;
        Check(MFCreateSinkWriterFromURL(output.c_str(),nullptr,nullptr,writer.put()),L"Создание MP4");
        DWORD videoStream=0,audioStream=0;
        Check(writer->AddStream(videoOutput.get(),&videoStream),L"Добавление видео дорожки");
        Check(writer->SetInputMediaType(videoStream,exact?encoderInput.get():videoInput.get(),nullptr),
              L"Настройка видеокодека");
        if(mixAudio)ConfigureMixedAAC(writer.get(),audioStream);
        else if(hasAudio){
            Check(writer->AddStream(nativeAudio.get(),&audioStream),L"Добавление аудио дорожки");
            Check(writer->SetInputMediaType(audioStream,nativeAudio.get(),nullptr),L"Настройка AAC");
        }
        LONGLONG total=0;
        for(auto s:spans)total+=s.last-s.first;
        if(total<=0)throw MediaError{E_INVALIDARG,L"Нет фрагментов для сохранения"};
        std::vector<int32_t> mixedPCM;
        if(mixAudio){
            const LONGLONG frames=AudioFrames(total);
            if(frames>(LONGLONG)MIX_RATE*60*30)
                throw MediaError{E_OUTOFMEMORY,L"Смешивание аудио ограничено 30 минутами"};
            mixedPCM.resize((size_t)frames*MIX_CHANNELS,0);
            if(!muteAudio && FileHasAudio(input))MixSourcePCM(input,spans,mixedPCM);
            MixSourcePCM(externalAudio,audioSpans,mixedPCM);
        }
        Check(writer->BeginWriting(),L"Начало записи MP4");
        LONGLONG audioWritten=0;
        if(mixAudio)WriteMixedAudioUntil(writer.get(),audioStream,mixedPCM,audioWritten,HNS/5);
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
                if(!exact && SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp,&decode))){
                    const LONGLONG adjusted=offset+((LONGLONG)decode-part.first);
                    if(adjusted>=0)sample->SetUINT64(MFSampleExtension_DecodeTimestamp,(UINT64)adjusted);
                }
                if(isVideo&&firstVideo){sample->SetUINT32(MFSampleExtension_Discontinuity,TRUE);firstVideo=false;}
                if(isAudio&&firstAudio){sample->SetUINT32(MFSampleExtension_Discontinuity,TRUE);firstAudio=false;}
                if(mixAudio && isVideo)
                    WriteMixedAudioUntil(writer.get(),audioStream,mixedPCM,audioWritten,newTime+HNS/5);
                if(exact && isVideo){
                    // Do not feed GPU-aligned / bottom-up 2D decoder surfaces directly
                    // into the H264 encoder, which assumes a packed NV12 layout.
                    ComPtr<IMFSample> packed;
                    CopyNV12ForEncoder(sample.get(),frameW,frameH,packed.put());
                    Check(writer->WriteSample(videoStream,packed.get()),L"Запись плотного NV12 кадра");
                }else{
                    Check(writer->WriteSample(isVideo?videoStream:audioStream,sample.get()),L"Запись кадра/звука");
                }
                int pct=(int)(100.0*(double)(offset+std::min(t-part.first,part.last-part.first))/(double)total);
                if(pct!=lastPct){lastPct=pct;PostMessageW(notify,WM_EXPORT_PROGRESS,pct,0);}
            }
            offset+=part.last-part.first;
        }
        if(mixAudio)WriteMixedAudioUntil(writer.get(),audioStream,mixedPCM,audioWritten,total);
        Check(writer->Finalize(),L"Завершение MP4");
        result->ok=true;
        result->message=L"Сохранение выполнено успешно.";
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
    bool muteAudio=g_muteOriginal;
    SetUIBusy(true);
    g_spinnerStep=0;g_exportPercent=0;
    Status(L"Сохранение видео…");
    std::thread([win=g_window,src=g_file,dst=output,parts=g_kept,len=g_duration,exact,muteAudio,
                 audio=g_audioFile,audioParts=g_audioKept](){
        ExportVideo(win,src,dst,parts,len,exact,muteAudio,audio,audioParts);
    }).detach();
}


LONGLONG MontageDuration(){return ClipsDuration(g_kept);}
int MontageX(LONGLONG offset){
    LONGLONG total=MontageDuration();
    if(total<=0)return g_montageTrack.left;
    return g_montageTrack.left+(int)((double)offset/total*(g_montageTrack.right-g_montageTrack.left));
}
LONGLONG PositionFromX(int x){
    int width=std::max(1,static_cast<int>(g_montageTrack.right-g_montageTrack.left));
    double fraction=std::max(0.0,std::min(1.0,(double)(x-g_montageTrack.left)/width));
    return ClampTime((LONGLONG)(fraction*MontageDuration()),MontageDuration());
}
RECT ClipRect(const std::vector<Span>& clips,size_t index,RECT row){
    LONGLONG offset=0;
    for(size_t i=0;i<index&&i<clips.size();++i)offset+=clips[i].last-clips[i].first;
    int left=MontageX(offset);
    if(index>=clips.size())return {left,row.top,row.right,row.bottom};
    int right=MontageX(offset+clips[index].last-clips[index].first);
    if(index+1==clips.size())right=MontageX(ClipsDuration(clips));
    return {left,row.top+2,right,row.bottom-2};
}
RECT MontageClipRect(size_t index){return ClipRect(g_kept,index,g_montageTrack);}
RECT AudioClipRect(size_t index){return ClipRect(g_audioKept,index,g_audioTrack);}
int ClipAtX(const std::vector<Span>& clips,int x,RECT row){
    if(x<row.left||x>=row.right)return -1;
    for(size_t i=0;i<clips.size();++i){
        RECT r=ClipRect(clips,i,row);
        if(x>=r.left&&x<r.right)return (int)i;
    }
    return -1;
}
int InsertSlot(const std::vector<Span>& clips,int x,RECT row){
    for(size_t i=0;i<clips.size();++i){
        RECT r=ClipRect(clips,i,row);
        if(x<(r.left+r.right)/2)return (int)i;
    }
    return (int)clips.size();
}
void MoveClip(int from,int slot,bool audio){
    auto& clips=audio?g_audioKept:g_kept;
    int n=(int)clips.size();
    if(from<0||from>=n||g_exporting)return;
    slot=std::max(0,std::min(slot,n));
    int destination=slot>from?slot-1:slot;
    if(destination==from){RedrawTrack();return;}
    Remember();
    Span item=clips[from];
    clips.erase(clips.begin()+from);
    clips.insert(clips.begin()+destination,item);
    if(audio)g_selectedAudio=destination;else g_selected=destination;
    g_focusAudio=audio;
    SeekTo(std::min(g_cursor,MontageDuration()));
    Status(audio?L"Аудиочасти переставлены. Ctrl+Z — отмена.":L"Видеочасти переставлены. Экспорт сохранит этот порядок.");
}
RECT MontageTrackRect(){
    RECT r;GetClientRect(g_window,&r);
    return {18,r.bottom-175,r.right-18,r.bottom-145};
}
RECT AudioTrackRect(){
    RECT r;GetClientRect(g_window,&r);
    return {18,r.bottom-110,r.right-18,r.bottom-80};
}
void DrawTrack(HDC dc,const std::vector<Span>& clips,RECT track,bool audio){
    HBRUSH bg=CreateSolidBrush(audio?RGB(48,62,79):RGB(56,60,72));
    FillRect(dc,&track,bg);DeleteObject(bg);
    for(size_t i=0;i<clips.size();++i){
        RECT r=ClipRect(clips,i,track);
        if(r.right<=r.left)continue;
        bool active=(audio?g_selectedAudio:g_selected)==(int)i && g_focusAudio==audio;
        HBRUSH fill=CreateSolidBrush(audio?(active?RGB(71,175,142):RGB(55,123,112)):
                                           (active?RGB(67,151,206):RGB(74,108,148)));
        FillRect(dc,&r,fill);DeleteObject(fill);
        HPEN sep=CreatePen(PS_SOLID,2,RGB(23,27,33));
        HPEN old=(HPEN)SelectObject(dc,sep);
        MoveToEx(dc,r.left,track.top,nullptr);LineTo(dc,r.left,track.bottom);
        SelectObject(dc,old);DeleteObject(sep);
        if(r.right-r.left>56){
            RECT label=r;label.left+=3;label.right-=3;
            std::wstring name=(audio?L"Аудио ":L"Часть ")+std::to_wstring(i+1);
            SetTextColor(dc,RGB(249,252,255));SetBkMode(dc,TRANSPARENT);
            DrawTextW(dc,name.c_str(),-1,&label,DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_END_ELLIPSIS);
        }
    }
}
void DrawTimeline(HDC dc){
    if(g_file.empty())return;
    RECT video=g_montageTrack,audio=g_audioTrack;
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(191,206,224));
    RECT label={video.left,video.top-24,video.right,video.top-3};
    DrawTextW(dc,L"ВИДЕО — тяните часть для перестановки · Shift+протяжка — выделить",-1,
              &label,DT_SINGLELINE|DT_LEFT|DT_VCENTER);
    DrawTrack(dc,g_kept,video,false);
    label={audio.left,audio.top-24,audio.right,audio.top-3};
    std::wstring labelAudio=g_audioFile.empty()?L"АУДИО — нажмите «Загрузить аудио»":
                                           L"АУДИО — тяните для перестановки · Shift+протяжка — выделить";
    DrawTextW(dc,labelAudio.c_str(),-1,&label,DT_SINGLELINE|DT_LEFT|DT_VCENTER);
    DrawTrack(dc,g_audioKept,audio,true);
    if(g_dragging){
        int a=MontageX(g_dragStart),b=MontageX(g_dragEnd);
        if(a>b)std::swap(a,b);
        RECT row=g_focusAudio?audio:video;
        RECT sel={a,row.top,b,row.bottom};
        HBRUSH overlay=CreateHatchBrush(HS_DIAGCROSS,RGB(250,180,70));
        FillRect(dc,&sel,overlay);DeleteObject(overlay);
    }
    int cursorX=MontageX(g_cursor);
    HPEN pen=CreatePen(PS_SOLID,2,RGB(255,233,105));
    HPEN old=(HPEN)SelectObject(dc,pen);
    MoveToEx(dc,cursorX,video.top-5,nullptr);LineTo(dc,cursorX,audio.bottom+4);
    SelectObject(dc,old);DeleteObject(pen);
    if(g_clipDragging){
        const auto& clips=g_dragAudio?g_audioKept:g_kept;
        RECT row=g_dragAudio?audio:video;
        int slot=std::max(0,std::min(g_insertSlot,(int)clips.size()));
        int x=slot==(int)clips.size()?MontageX(ClipsDuration(clips)):ClipRect(clips,slot,row).left;
        HPEN target=CreatePen(PS_SOLID,3,RGB(255,233,105));
        old=(HPEN)SelectObject(dc,target);
        MoveToEx(dc,x,row.top-5,nullptr);LineTo(dc,x,row.bottom+5);
        SelectObject(dc,old);DeleteObject(target);
    }
}
void Relayout(){
    if(!g_window)return;
    RECT r;GetClientRect(g_window,&r);
    int w=r.right,h=r.bottom;
    // Preserve source sharpness: never upscale low-resolution video.
    int areaW=w-36,areaH=std::max(120,h-362),videoW=areaW,videoH=areaH;
    if(g_previewWidth&&g_previewHeight){
        double scale=std::min(1.0,std::min((double)areaW/g_previewWidth,
                                          (double)areaH/g_previewHeight));
        videoW=std::max(1,(int)(g_previewWidth*scale));
        videoH=std::max(1,(int)(g_previewHeight*scale));
    }
    if(g_video)MoveWindow(g_video,18+(areaW-videoW)/2,54+(areaH-videoH)/2,
                          videoW,videoH,TRUE);
    int row=h-297,x=18;
    auto place=[&](HWND hwnd,int width){
        if(hwnd)MoveWindow(hwnd,x,row,width,37,TRUE);
        x+=width+9;
    };
    place(g_buttons[0],132);
    place(g_loadAudio,179);
    place(g_buttons[1],121);
    place(g_buttons[2],129);
    place(g_buttons[3],110);
    place(g_buttons[6],116);
    row=h-247;x=18;
    place(g_buttons[4],173);
    place(g_buttons[5],163);
    if(g_time)MoveWindow(g_time,393,h-239,195,29,TRUE);
    if(g_noAudio)MoveWindow(g_noAudio,w-319,h-246,300,37,TRUE);
    g_montageTrack=MontageTrackRect();
    g_audioTrack=AudioTrackRect();
    if(g_buttons[7])MoveWindow(g_buttons[7],w-174,h-57,155,36,TRUE);
    if(g_status)MoveWindow(g_status,18,h-53,std::max(100,w-212),31,TRUE);
    InvalidateRect(g_window,nullptr,FALSE);
}
HWND MakeButton(int idx,int id,const wchar_t* caption){
    HWND h=CreateWindowExW(0,L"BUTTON",caption,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
        0,0,100,36,g_window,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(h,WM_SETFONT,(WPARAM)g_font,TRUE);
    g_buttons[idx]=h;return h;
}
std::wstring AudioFileDialog(){
    wchar_t path[MAX_PATH]={};
    OPENFILENAMEW d={};d.lStructSize=sizeof(d);d.hwndOwner=g_window;
    d.lpstrFile=path;d.nMaxFile=MAX_PATH;
    d.lpstrFilter=L"Аудио (MP3, WAV, M4A, AAC, WMA)\0*.mp3;*.wav;*.m4a;*.aac;*.wma\0Все файлы (*.*)\0*.*\0";
    d.nFilterIndex=1;d.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
    return GetOpenFileNameW(&d)?std::wstring(path):std::wstring();
}

RECT SpinnerRect(){
    if(!g_window)return {0,0,0,0};
    RECT client;GetClientRect(g_window,&client);
    return {client.right-270,8,client.right-8,49};
}
void DrawExportSpinner(HDC dc){
    if(!g_exporting)return;
    RECT area=SpinnerRect();
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,RGB(232,239,247));
    std::wstring caption=L"Сохранение: "+(g_exportPercent>=0
        ?std::to_wstring(g_exportPercent)+L"%":std::wstring(L"подготовка"));
    RECT label={area.left,area.top,area.right-49,area.bottom};
    DrawTextW(dc,caption.c_str(),-1,&label,DT_SINGLELINE|DT_RIGHT|DT_VCENTER);
    int cx=area.right-24,cy=(area.top+area.bottom)/2;
    constexpr double PI=3.141592653589793;
    for(int i=0;i<12;++i){
        double angle=(2.0*PI*i)/12.0;
        int x=cx+(int)(12.0*std::cos(angle));
        int y=cy+(int)(12.0*std::sin(angle));
        int glow=(i+12-g_spinnerStep%12)%12;
        int v=75+(11-glow)*15;
        HBRUSH dot=CreateSolidBrush(RGB(v>240?240:v, v>240?240:v, 255));
        HGDIOBJ old=SelectObject(dc,dot);
        Ellipse(dc,x-3,y-3,x+4,y+4);
        SelectObject(dc,old);DeleteObject(dot);
    }
}
LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        g_window=hwnd;
        DragAcceptFiles(hwnd,TRUE);
        g_font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_video=CreateWindowExW(WS_EX_CLIENTEDGE,L"STATIC",L"Откройте видео",
            WS_CHILD|WS_VISIBLE|SS_BLACKRECT,0,0,100,100,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        MakeButton(0,ID_OPEN,L"Открыть MP4");MakeButton(1,ID_PLAY,L"▶ / Пауза");
        MakeButton(2,ID_SPLIT,L"✂ Разрезать");MakeButton(3,ID_REMOVE,L"Удалить");
        MakeButton(4,ID_TRIM_START,L"Убрать до курсора");
        MakeButton(5,ID_TRIM_END,L"Убрать после");MakeButton(6,ID_UNDO,L"↶ Отмена");
        MakeButton(7,ID_SAVE,L"Сохранить");
        g_loadAudio=CreateWindowW(L"BUTTON",L"Загрузить аудио",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|WS_TABSTOP,0,0,179,37,
            hwnd,(HMENU)(INT_PTR)ID_LOAD_AUDIO,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(g_loadAudio,WM_SETFONT,(WPARAM)g_font,TRUE);
        g_noAudio=CreateWindowW(L"BUTTON",L"Отключить оригинальный звук",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,300,37,
            hwnd,(HMENU)(INT_PTR)ID_NO_AUDIO,GetModuleHandleW(nullptr),nullptr);
        g_time=CreateWindowW(L"STATIC",L"00:00:00 / 00:00:00",WS_CHILD|WS_VISIBLE,0,0,100,20,hwnd,nullptr,nullptr,nullptr);
        g_status=CreateWindowW(L"STATIC",L"Откройте MP4. Можно перетащить файл в окно.",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_ENDELLIPSIS,0,0,100,20,hwnd,nullptr,nullptr,nullptr);
        SendMessageW(g_time,WM_SETFONT,(WPARAM)g_font,TRUE);
        SendMessageW(g_status,WM_SETFONT,(WPARAM)g_font,TRUE);
        Relayout();SetTimer(hwnd,1,60,nullptr);UpdatePlayButton();return 0;
    }
    case WM_GETMINMAXINFO:{
        auto p=(MINMAXINFO*)lp;p->ptMinTrackSize.x=960;p->ptMinTrackSize.y=635;return 0;
    }
    case WM_SIZE:Relayout();return 0;
    case WM_PLAYER_EVENT:{
        auto eventType=(MFP_EVENT_TYPE)wp;
        IMFPMediaPlayer* source=(IMFPMediaPlayer*)lp;
        if(source==g_player && g_player){
            if(eventType==MFP_EVENT_TYPE_MEDIAITEM_SET){
                g_playerReady=true;
                g_player->SetAspectRatioMode(MFVideoARMode_PreservePicture);
                SIZE native={},aspectCorrected={};
                if(SUCCEEDED(g_player->GetNativeVideoSize(&native,&aspectCorrected))){
                    if(aspectCorrected.cx>0&&aspectCorrected.cy>0){
                        g_previewWidth=(UINT32)aspectCorrected.cx;
                        g_previewHeight=(UINT32)aspectCorrected.cy;
                    }
                }
                Relayout();
                ClipPosition at=LocateClip(g_kept,g_cursor);
                if(at.index>=0)SetPlayerPosition(g_player,at.source);
                if(g_userWantsPlay)StartPlayback();
                else BeginStillPreview();
            }else if(eventType==MFP_EVENT_TYPE_PLAYBACK_ENDED && !g_previewPriming){
                g_playing=false;g_userWantsPlay=false;
                if(g_audioPlayer)g_audioPlayer->Pause();
                UpdatePlayButton();
            }
        }else if(source==g_audioPlayer && g_audioPlayer){
            if(eventType==MFP_EVENT_TYPE_MEDIAITEM_SET){
                g_audioReady=true;
                ClipPosition a=LocateClip(g_audioKept,g_cursor);
                if(a.index>=0){
                    SetPlayerPosition(g_audioPlayer,a.source);
                    if(g_playing)g_audioPlayer->Play();
                }
            }
        }
        return 0;
    }
    case WM_DROPFILES:{
        HDROP drop=(HDROP)wp;wchar_t p[MAX_PATH]={};
        if(DragQueryFileW(drop,0,p,MAX_PATH))OpenFile(p);
        DragFinish(drop);return 0;
    }
    case WM_LBUTTONDOWN:{
        POINT p={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(g_file.empty()||g_exporting)return 0;
        bool audio=PtInRect(&g_audioTrack,p)!=0;
        bool video=PtInRect(&g_montageTrack,p)!=0;
        if(!audio&&!video)break;
        const auto& clips=audio?g_audioKept:g_kept;
        RECT row=audio?g_audioTrack:g_montageTrack;
        int index=ClipAtX(clips,p.x,row);
        if(index<0)break;
        SetFocus(hwnd); // Follow selection: Delete and Ctrl+Z target this editor.
        g_focusAudio=audio;g_dragAudio=audio;
        if(audio)g_selectedAudio=index;else g_selected=index;
        g_dragging=false;g_clipDragging=false;
        g_clipMouseDown=true;g_dragClipIndex=index;g_insertSlot=index;
        g_mousePt=p;g_rangeSelect=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        g_dragStart=g_dragEnd=PositionFromX(p.x);
        SeekTo(g_dragStart);SetCapture(hwnd);RedrawTrack();return 0;
    }
    case WM_MOUSEMOVE:{
        if(!g_clipMouseDown)break;
        int x=GET_X_LPARAM(lp);
        if(g_rangeSelect){
            if(abs(x-g_mousePt.x)>5)g_dragging=true;
            g_dragEnd=PositionFromX(x);
            SeekTo(g_dragEnd);
        }else{
            if(abs(x-g_mousePt.x)>5)g_clipDragging=true;
            if(g_clipDragging){
                const auto& clips=g_dragAudio?g_audioKept:g_kept;
                RECT row=g_dragAudio?g_audioTrack:g_montageTrack;
                g_insertSlot=InsertSlot(clips,x,row);
            }
        }
        RedrawTrack();return 0;
    }
    case WM_LBUTTONUP:{
        if(!g_clipMouseDown)break;
        bool moved=g_clipDragging,range=g_rangeSelect&&g_dragging;
        int from=g_dragClipIndex,slot=g_insertSlot;bool audio=g_dragAudio;
        g_clipMouseDown=false;g_clipDragging=false;g_dragClipIndex=-1;
        ReleaseCapture();
        if(moved)MoveClip(from,slot,audio);
        else if(range)Status(L"Диапазон выделен. Нажмите «Удалить», чтобы его вырезать.");
        else SeekTo(g_dragStart);
        RedrawTrack();return 0;
    }
    case WM_KEYDOWN:{
        if(wp=='Z'&&(GetKeyState(VK_CONTROL)&0x8000)){Undo();return 0;}
        if((wp==VK_LEFT||wp==VK_RIGHT)&&(GetKeyState(VK_CONTROL)&0x8000)){
            int selected=g_focusAudio?g_selectedAudio:g_selected;
            if(selected>=0)MoveClip(selected,wp==VK_LEFT?selected-1:selected+2,g_focusAudio);
            return 0;
        }
        if(wp==VK_DELETE){RemoveSelected();return 0;}
        if(wp==VK_SPACE){SendMessageW(hwnd,WM_COMMAND,ID_PLAY,0);return 0;}
        break;
    }
    case WM_COMMAND:{
        switch(LOWORD(wp)){
        case ID_OPEN:{auto p=FileDialog(false);if(!p.empty())OpenFile(p);break;}
        case ID_LOAD_AUDIO:{auto p=AudioFileDialog();if(!p.empty())OpenAudioFile(p);break;}
        case ID_NO_AUDIO:
            if(HIWORD(wp)!=BN_CLICKED)break;
            g_muteOriginal=!g_muteOriginal;
            if(g_player)g_player->SetVolume(g_muteOriginal?0.0f:1.0f);
            InvalidateRect(g_noAudio,nullptr,TRUE);
            Status(g_muteOriginal?L"Оригинальный звук отключён. Загруженное аудио останется.":L"Оригинальный звук включён.");
            break;
        case ID_PLAY:TogglePlayback();break;
        case ID_SPLIT:SplitHere();break;
        case ID_REMOVE:RemoveSelected();break;
        case ID_TRIM_START:if(!g_file.empty()&&!g_exporting)RemoveInterval(0,g_cursor);break;
        case ID_TRIM_END:if(!g_file.empty()&&!g_exporting)RemoveInterval(g_cursor,
            g_focusAudio?ClipsDuration(g_audioKept):MontageDuration());break;
        case ID_UNDO:Undo();break;
        case ID_SAVE:StartExport(false);break;
        }
        return 0;
    }
    case WM_TIMER:{
        if(g_previewPriming && g_playerReady && g_player){
            ULONGLONG elapsed=GetTickCount64()-g_primeSince;
            PROPVARIANT pos;PropVariantInit(&pos);
            bool moved=false;
            if(SUCCEEDED(g_player->GetPosition(MFP_POSITIONTYPE_100NS,&pos))&&pos.vt==VT_I8){
                LONGLONG position=pos.hVal.QuadPart;
                moved=position>=g_primeSource+HNS/40 &&
                      position<=g_primeSource+2*HNS;
            }
            PropVariantClear(&pos);
            if((elapsed>=110 && moved)||elapsed>=520){
                // The first frame has reached the renderer; freeze it without
                // ever requiring the user to press Play to initialize MFPlay.
                g_previewPriming=false;
                g_player->Pause();
                g_player->SetVolume(g_muteOriginal?0.0f:1.0f);
                if(g_video)InvalidateRect(g_video,nullptr,FALSE);
                g_player->UpdateVideo();
                UpdatePlayButton();
            }
        }
        if(g_exporting){
            ++g_spinnerStep;
            RECT spinner=SpinnerRect();
            InvalidateRect(hwnd,&spinner,FALSE);
        }
        if(!g_player||!g_playing||g_previewClip<0)break;
        PROPVARIANT pos;PropVariantInit(&pos);
        if(SUCCEEDED(g_player->GetPosition(MFP_POSITIONTYPE_100NS,&pos))&&pos.vt==VT_I8){
            LONGLONG source=pos.hVal.QuadPart;
            if(g_previewClip<(int)g_kept.size()){
                Span clip=g_kept[g_previewClip];
                LONGLONG startOffset=0;
                for(int i=0;i<g_previewClip;++i)
                    startOffset+=g_kept[i].last-g_kept[i].first;
                if(source>=clip.last-HNS/100){
                    LONGLONG next=startOffset+(clip.last-clip.first);
                    if(next>=MontageDuration()){
                        g_player->Pause();
                        if(g_audioPlayer)g_audioPlayer->Pause();
                        g_playing=false;g_cursor=MontageDuration();
                        UpdatePlayButton();
                    }else SeekTo(next,true);
                }else if(source>=clip.first){
                    g_cursor=startOffset+source-clip.first;
                    ClipPosition audio=LocateClip(g_audioKept,g_cursor);
                    if(audio.index!=g_previewAudioClip){
                        g_previewAudioClip=audio.index;
                        if(g_audioPlayer){
                            if(audio.index>=0){
                                SetPlayerPosition(g_audioPlayer,audio.source);
                                g_audioPlayer->Play();
                            }else g_audioPlayer->Pause();
                        }
                    }
                }
            }
            SetWindowTextW(g_time,(TimeLabel(g_cursor)+L" / "+TimeLabel(MontageDuration())).c_str());
            RedrawTrack();
        }
        PropVariantClear(&pos);return 0;
    }
    case WM_EXPORT_PROGRESS:
        if(wp==1001){Status(L"Предпросмотр: Media Foundation сообщила об ошибке.");}
        else if(g_exporting){
            g_exportPercent=(int)wp;
            Status(L"Сохранение: "+std::to_wstring(wp)+L"% …");
            RECT spinner=SpinnerRect();InvalidateRect(hwnd,&spinner,FALSE);
        }
        return 0;
    case WM_EXPORT_DONE:{
        std::unique_ptr<ExportResult> r((ExportResult*)lp);
        SetUIBusy(false);
        Status(r->message);
        if(r->ok)MessageBoxW(hwnd,r->message.c_str(),L"MiniCut — сохранено",MB_OK|MB_ICONINFORMATION);
        else ShowFail(L"Не удалось сохранить MP4",r->message);
        return 0;
    }
    case WM_DRAWITEM:{
        DRAWITEMSTRUCT* d=(DRAWITEMSTRUCT*)lp;
        if(d && d->CtlID==ID_NO_AUDIO){
            RECT r=d->rcItem;
            HBRUSH bg=CreateSolidBrush(g_muteOriginal?RGB(32,87,70):RGB(43,54,69));
            FillRect(d->hDC,&r,bg);DeleteObject(bg);
            HPEN border=CreatePen(PS_SOLID,2,g_muteOriginal?RGB(97,224,156):RGB(94,115,143));
            HGDIOBJ oldPen=SelectObject(d->hDC,border);
            HGDIOBJ oldBrush=SelectObject(d->hDC,GetStockObject(NULL_BRUSH));
            Rectangle(d->hDC,r.left,r.top,r.right-1,r.bottom-1);
            SelectObject(d->hDC,oldBrush);SelectObject(d->hDC,oldPen);DeleteObject(border);
            RECT check={r.left+10,r.top+10,r.left+27,r.top+27};
            DrawFrameControl(d->hDC,&check,DFC_BUTTON,DFCS_BUTTONCHECK|
                             (g_muteOriginal?DFCS_CHECKED:0));
            SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,RGB(243,248,253));
            HFONT previous=(HFONT)SelectObject(d->hDC,g_font);
            RECT txt={r.left+34,r.top,r.right-6,r.bottom};
            DrawTextW(d->hDC,L"Отключить оригинальный звук",-1,&txt,
                      DT_SINGLELINE|DT_VCENTER|DT_LEFT);
            SelectObject(d->hDC,previous);
            return TRUE;
        }
        break;
    }
    case WM_PAINT:{
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);
        RECT client;GetClientRect(hwnd,&client);
        HBRUSH bg=CreateSolidBrush(RGB(29,34,43));FillRect(dc,&client,bg);DeleteObject(bg);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(219,227,238));
        HFONT previous=(HFONT)SelectObject(dc,g_font);
        RECT title={20,16,client.right-20,45};
        DrawTextW(dc,L"MiniCut",-1,&title,DT_SINGLELINE|DT_VCENTER);
        if(g_duration){DrawTimeline(dc);}
        DrawExportSpinner(dc);
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
    // Dispatch shortcuts before focused child BUTTON controls consume key messages.
    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)>0){
        if(msg.message==WM_KEYDOWN && !g_exporting){
            if(msg.wParam==VK_DELETE){
                SendMessageW(g_window,WM_COMMAND,MAKEWPARAM(ID_REMOVE,0),0);
                continue;
            }
            if(msg.wParam=='Z' && (GetKeyState(VK_CONTROL)&0x8000) &&
               !(GetKeyState(VK_MENU)&0x8000)){
                SendMessageW(g_window,WM_COMMAND,MAKEWPARAM(ID_UNDO,0),0);
                continue;
            }
        }
        TranslateMessage(&msg);DispatchMessageW(&msg);
    }
    MFShutdown();CoUninitialize();return (int)msg.wParam;
}
