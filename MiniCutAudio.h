#pragma once
// Audio composition for MiniCut: independent original/custom sound, edited segments,
// 48 kHz stereo PCM mixing and AAC encoding through Windows Media Foundation.
constexpr UINT32 MIX_RATE=48000;
constexpr UINT32 MIX_CHANNELS=2;
constexpr UINT32 MIX_BLOCK=4; // 16-bit stereo PCM

LONGLONG AudioFrames(LONGLONG hns) {
    return hns<=0?0:(LONGLONG)((hns*(LONGLONG)MIX_RATE+HNS/2)/HNS);
}
LONGLONG AudioTime(LONGLONG frames) {
    return (frames*HNS)/MIX_RATE;
}
bool FileHasAudio(const std::wstring& path){
    if(path.empty())return false;
    ComPtr<IMFSourceReader> source;
    if(FAILED(MFCreateSourceReaderFromURL(path.c_str(),nullptr,source.put())))return false;
    ComPtr<IMFMediaType> type;
    return SUCCEEDED(source->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,type.put()));
}
void SetPCMType(IMFMediaType* t){
    Check(t->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio),L"Тип PCM");
    Check(t->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM),L"Подтип PCM");
    Check(t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,MIX_CHANNELS),L"Каналы PCM");
    Check(t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,MIX_RATE),L"Частота PCM");
    Check(t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16),L"Разрядность PCM");
    Check(t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,MIX_BLOCK),L"Выравнивание PCM");
    Check(t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,MIX_RATE*MIX_BLOCK),L"Скорость PCM");
}
void ConfigureMixedAAC(IMFSinkWriter* writer,DWORD& streamIndex){
    ComPtr<IMFMediaType> out,in;
    Check(MFCreateMediaType(out.put()),L"Тип AAC");
    Check(out->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio),L"Аудио AAC");
    Check(out->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_AAC),L"Кодек AAC");
    Check(out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,MIX_CHANNELS),L"Каналы AAC");
    Check(out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,MIX_RATE),L"Частота AAC");
    Check(out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16),L"Биты AAC");
    Check(out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,24000),L"Битрейт AAC");
    Check(writer->AddStream(out.get(),&streamIndex),L"Аудиодорожка AAC");
    Check(MFCreateMediaType(in.put()),L"Тип входа PCM");
    SetPCMType(in.get());
    Check(writer->SetInputMediaType(streamIndex,in.get(),nullptr),L"Вход PCM кодировщика AAC");
}
// Render selected source spans to output time, optionally mixing with existing samples.
// The input spans are in output order and may be reordered or shortened.
void MixSourcePCM(const std::wstring& sourcePath,const std::vector<Span>& spans,
                  std::vector<int32_t>& mix){
    if(spans.empty()||sourcePath.empty()||mix.empty())return;
    // Source Reader installs audio decoders / resamplers when PCM is requested.
    ComPtr<IMFSourceReader> reader;
    Check(MFCreateSourceReaderFromURL(sourcePath.c_str(),nullptr,reader.put()),L"Открытие аудио");
    Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),L"Аудиопотоки");
    Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM,TRUE),L"Выбор аудио");
    ComPtr<IMFMediaType> pcm;
    Check(MFCreateMediaType(pcm.put()),L"PCM для чтения");
    SetPCMType(pcm.get());
    Check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,nullptr,pcm.get()),
          L"Декодирование аудио в PCM 48 кГц");
    const LONGLONG maxFrames=(LONGLONG)mix.size()/MIX_CHANNELS;
    LONGLONG outputOffset=0;
    for(const Span& part:spans){
        const LONGLONG outputFrame=AudioFrames(outputOffset);
        const LONGLONG startFrame=AudioFrames(part.first);
        const LONGLONG endFrame=AudioFrames(part.last);
        outputOffset+=part.last-part.first;
        if(endFrame<=startFrame||outputFrame>=maxFrames)continue;
        PROPVARIANT seek;PropVariantInit(&seek);
        seek.vt=VT_I8;seek.hVal.QuadPart=part.first;
        Check(reader->SetCurrentPosition(GUID_NULL,seek),L"Перемещение по аудио");
        for(;;){
            DWORD index=0,flags=0;LONGLONG t=0;
            ComPtr<IMFSample> sample;
            Check(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,&index,&flags,&t,sample.put()),
                  L"Чтение звукового фрагмента");
            if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
            if(t>=part.last)break;
            if(!sample)continue;
            ComPtr<IMFMediaBuffer> buffer;
            Check(sample->ConvertToContiguousBuffer(buffer.put()),L"PCM буфер");
            BYTE* bytes=nullptr;DWORD maxLen=0,len=0;
            Check(buffer->Lock(&bytes,&maxLen,&len),L"Чтение PCM буфера");
            const int16_t* pcmData=reinterpret_cast<const int16_t*>(bytes);
            const LONGLONG sampleFrame=AudioFrames(t);
            const size_t count=(size_t)len/MIX_BLOCK;
            for(size_t j=0;j<count;++j){
                LONGLONG srcFrame=sampleFrame+(LONGLONG)j;
                if(srcFrame<startFrame||srcFrame>=endFrame)continue;
                LONGLONG dest=outputFrame+(srcFrame-startFrame);
                if(dest<0||dest>=maxFrames)continue;
                size_t pos=(size_t)dest*2;
                mix[pos]+=pcmData[j*2];
                mix[pos+1]+=pcmData[j*2+1];
            }
            buffer->Unlock();
        }
    }
}
void WriteMixedAudio(IMFSinkWriter* writer,DWORD stream,const std::vector<int32_t>& mix){
    const LONGLONG frameCount=(LONGLONG)mix.size()/MIX_CHANNELS;
    constexpr LONGLONG FRAMES_PER_SAMPLE=2048;
    for(LONGLONG start=0;start<frameCount;start+=FRAMES_PER_SAMPLE){
        const LONGLONG count=std::min(FRAMES_PER_SAMPLE,frameCount-start);
        const DWORD byteCount=(DWORD)(count*MIX_BLOCK);
        ComPtr<IMFMediaBuffer> buffer;
        Check(MFCreateMemoryBuffer(byteCount,buffer.put()),L"Создание PCM буфера");
        BYTE* data=nullptr;DWORD capacity=0,current=0;
        Check(buffer->Lock(&data,&capacity,&current),L"Запись PCM");
        int16_t* dst=reinterpret_cast<int16_t*>(data);
        for(LONGLONG j=0;j<count*2;++j){
            int32_t v=mix[(size_t)(start*2+j)];
            dst[j]=(int16_t)std::max(-32768,std::min(32767,v));
        }
        buffer->Unlock();
        Check(buffer->SetCurrentLength(byteCount),L"Размер PCM");
        ComPtr<IMFSample> sample;
        Check(MFCreateSample(sample.put()),L"Аудиосэмпл");
        Check(sample->AddBuffer(buffer.get()),L"Аудиоданные");
        Check(sample->SetSampleTime(AudioTime(start)),L"Время аудио");
        Check(sample->SetSampleDuration(AudioTime(start+count)-AudioTime(start)),L"Длительность аудио");
        Check(writer->WriteSample(stream,sample.get()),L"Кодирование AAC");
    }
}
