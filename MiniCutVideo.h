#pragma once
// The sink writer expects a tightly packed NV12 frame. Decoder surfaces can have
// row padding / GPU pitch, so never pass the decoder's IMF2DBuffer as-is.
void CopyNV12ForEncoder(IMFSample* decoded,UINT32 width,UINT32 height,
                        IMFSample** packedOut){
    if(!packedOut)throw MediaError{E_POINTER,L"Память кадра NV12"};
    *packedOut=nullptr;
    if(!width||!height||(width&1)||(height&1))
        throw MediaError{MF_E_INVALIDMEDIATYPE,L"Точный экспорт требует чётную ширину и высоту кадра"};
    const UINT64 required64=(UINT64)width*height*3/2;
    if(required64>0xffffffffull)
        throw MediaError{E_OUTOFMEMORY,L"Слишком большой видеокадр"};
    const DWORD required=(DWORD)required64;
    ComPtr<IMFMediaBuffer> source;
    Check(decoded->GetBufferByIndex(0,source.put()),L"Буфер видео");
    ComPtr<IMFMediaBuffer> target;
    Check(MFCreateMemoryBuffer(required,target.put()),L"Подготовка кадра NV12");
    BYTE* output=nullptr;DWORD capacity=0,used=0;
    Check(target->Lock(&output,&capacity,&used),L"Запись кадра NV12");
    HRESULT copyStatus=E_FAIL;
    IMF2DBuffer* twoD=nullptr;
    if(SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&twoD)))){
        DWORD contiguous=0;
        copyStatus=twoD->GetContiguousLength(&contiguous);
        if(SUCCEEDED(copyStatus) && contiguous!=required)
            copyStatus=MF_E_INVALIDMEDIATYPE;
        if(SUCCEEDED(copyStatus))
            copyStatus=twoD->ContiguousCopyTo(output,required);
        twoD->Release();
    }else{
        // Plain memory buffers are safe only if already tightly packed.
        BYTE* input=nullptr;DWORD maxLength=0,length=0;
        copyStatus=source->Lock(&input,&maxLength,&length);
        if(SUCCEEDED(copyStatus)){
            if(length==required)memcpy(output,input,required);
            else copyStatus=MF_E_INVALIDMEDIATYPE;
            source->Unlock();
        }
    }
    target->Unlock();
    Check(copyStatus,L"Неверный шаг строк NV12 — кадр не записан во избежание артефактов");
    Check(target->SetCurrentLength(required),L"Размер видеокадра");
    ComPtr<IMFSample> result;
    Check(MFCreateSample(result.put()),L"Выходной видеосэмпл");
    Check(result->AddBuffer(target.get()),L"Данные видеосэмпла");
    LONGLONG time=0,span=0;
    if(SUCCEEDED(decoded->GetSampleTime(&time)))
        Check(result->SetSampleTime(time),L"Время кадра");
    if(SUCCEEDED(decoded->GetSampleDuration(&span)))
        Check(result->SetSampleDuration(span),L"Длительность кадра");
    // Copy only clean-point/discontinuity flags; decoded samples can carry
    // compressed-stream DTS metadata that is invalid for freshly encoded H.264.
    UINT32 flag=0;
    if(SUCCEEDED(decoded->GetUINT32(MFSampleExtension_Discontinuity,&flag)) && flag)
        result->SetUINT32(MFSampleExtension_Discontinuity,TRUE);
    *packedOut=result.p;result.p=nullptr;
}
