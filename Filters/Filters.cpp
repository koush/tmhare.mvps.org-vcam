#pragma warning(disable:4244)
#pragma warning(disable:4711)

#include <streams.h>
#include <stdio.h>
#include <olectl.h>
#include <dvdmedia.h>
#include "filters.h"

static const GUID MEDIASUBTYPE_AVC1 =
{ 0x31435641, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };


//////////////////////////////////////////////////////////////////////////
//  CVCam is the source filter which masquerades as a capture device
//////////////////////////////////////////////////////////////////////////
CUnknown * WINAPI CVCam::CreateInstance(LPUNKNOWN lpunk, HRESULT *phr)
{
    ASSERT(phr);
    CUnknown *punk = new CVCam(lpunk, phr);
    return punk;
}

CVCam::CVCam(LPUNKNOWN lpunk, HRESULT *phr) : 
    CSource(NAME("Virtual Cam"), lpunk, CLSID_VirtualCam)
{
    ASSERT(phr);
    CAutoLock cAutoLock(&m_cStateLock);
    // Create the one and only output pin
    m_paStreams = (CSourceStream **) new CVCamStream*[1];
    m_paStreams[0] = new CVCamStream(phr, this, L"Virtual Cam");
}

HRESULT CVCam::QueryInterface(REFIID riid, void **ppv)
{
    //Forward request for IAMStreamConfig & IKsPropertySet to the pin
    if(riid == _uuidof(IAMStreamConfig) || riid == _uuidof(IKsPropertySet))
        return m_paStreams[0]->QueryInterface(riid, ppv);
    else
        return CSource::QueryInterface(riid, ppv);
}

//////////////////////////////////////////////////////////////////////////
// CVCamStream is the one and only output pin of CVCam which handles 
// all the stuff.
//////////////////////////////////////////////////////////////////////////
CVCamStream::CVCamStream(HRESULT *phr, CVCam *pParent, LPCWSTR pPinName) :
    CSourceStream(NAME("Virtual Cam"),phr, pParent, pPinName), m_pParent(pParent),
    m_pipe(INVALID_HANDLE_VALUE), m_first(true)
{
    // Set the default media type as 320x240x24@15
    GetMediaType(&m_mt);
}

CVCamStream::~CVCamStream()
{
} 

HRESULT CVCamStream::QueryInterface(REFIID riid, void **ppv)
{   
    // Standard OLE stuff
    if(riid == _uuidof(IAMStreamConfig))
        *ppv = (IAMStreamConfig*)this;
    else if(riid == _uuidof(IKsPropertySet))
        *ppv = (IKsPropertySet*)this;
    else
        return CSourceStream::QueryInterface(riid, ppv);

    AddRef();
    return S_OK;
}


static BOOLEAN ReadFully(HANDLE handle, void* buffer, int length) {
    BOOL fSuccess;
    DWORD amountRead;
    int offset = 0;
    while (offset != length) {
        fSuccess = ReadFile(handle, (byte*)buffer + offset, length - offset, &amountRead, NULL);
        if (!fSuccess && GetLastError() != ERROR_MORE_DATA) {
            return false;
        }
        if (fSuccess) {
            offset += amountRead;
        }
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////
//  This is the routine where we create the data being output by the Virtual
//  Camera device.
//////////////////////////////////////////////////////////////////////////

HRESULT CVCamStream::FillBuffer(IMediaSample *pms)
{
    if (m_pipe == INVALID_HANDLE_VALUE) {
        m_pipe = CreateFileA("\\\\.\\pipe\\vysor-camera", GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, 0, NULL);
        if (m_pipe == INVALID_HANDLE_VALUE) {
            return ERROR_RETRY;
        }
        DWORD dwMode = PIPE_READMODE_MESSAGE;

        SetNamedPipeHandleState(
            m_pipe,    // pipe handle 
            &dwMode,  // new pipe mode 
            NULL,     // don't set maximum bytes 
            NULL);    // don't set maximum time
    }

    BYTE* pData;
    pms->GetPointer(&pData);

    //while (true) {
        DWORD length;
        if (!ReadFully(m_pipe, (byte*)&length, 4)) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
            return ERROR;
        }

        length = htonl(length);
        int header[2];
        if (!ReadFully(m_pipe, header, 8)) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
            return ERROR;
        }
        int pts = htonl(header[0]);
        bool syncFrame = header[1];

        if (!ReadFully(m_pipe, pData, length - 8)) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
            return ERROR;
        }

        DWORD videoLength = length - 8;
        //if (syncFrame) {
        //    // search for 2 startcodes
        //    int i = 1;
        //    bool found = false;
        //    while (i < length - 4 && i < 100) {
        //        DWORD startCode = *(DWORD*)(pData + i);
        //        if (ntohl(startCode) == 1) {
        //            found = true;
        //            break;
        //        }
        //        i++;
        //    }

        //    if (found) {
        //        found = false;
        //        i++;
        //        while (i < length - 4 && i < 100) {
        //            DWORD startCode = *(DWORD*)(pData + i);
        //            if (ntohl(startCode) == 1) {
        //                found = true;
        //                break;
        //            }
        //            i++;
        //        }
        //    }

        //    if (found) {
        //        videoLength -= i;
        //        memcpy(pData, pData + i, videoLength);
        //    }
        //}

        DWORD* startcode = (DWORD*)pData;
        *startcode = htonl(videoLength - 4);

        REFERENCE_TIME rtNow;

        REFERENCE_TIME avgFrameTime = ((VIDEOINFOHEADER*)m_mt.pbFormat)->AvgTimePerFrame;

        rtNow = m_rtLastTime;
        m_rtLastTime += avgFrameTime;
        pms->SetTime(NULL, NULL);
        pms->SetSyncPoint(syncFrame);
        pms->SetActualDataLength(videoLength);


    //    if (m_first) {
    //        m_first = false;
    //    }
    //    else {
    //        break;
    //    }
    //}

    //BYTE *pData;
    //long lDataLen;
    //pms->GetPointer(&pData);
    //lDataLen = pms->GetSize();
    //for(int i = 0; i < lDataLen; ++i)
    //    pData[i] = rand();

    return NOERROR;
} // FillBuffer


// See Directshow help topic for IAMStreamConfig for details on this method
HRESULT CVCamStream::GetMediaType(CMediaType* pMediaType)
{
    pMediaType->InitMediaType();
    pMediaType->SetType(&MEDIATYPE_Video);
    pMediaType->SetSubtype(&MEDIASUBTYPE_AVC1);
    pMediaType->SetFormatType(&FORMAT_MPEG2_VIDEO);

    pMediaType->bFixedSizeSamples = FALSE;
    pMediaType->bTemporalCompression = TRUE;

    // Our H.264 codec only supports baseline, we can't extract 
    // width and height for other .264 sources. In this case
    // the value will still be 0. Hence just set any value.
    unsigned uiWidth = 1080;
    unsigned uiHeight = 2280;

    MPEG2VIDEOINFO* mpeg2 = (MPEG2VIDEOINFO*)pMediaType->AllocFormatBuffer(sizeof(MPEG2VIDEOINFO) + 80);
    ZeroMemory(mpeg2, sizeof(MPEG2VIDEOINFO) + 80);

    mpeg2->dwProfile = 66;
    mpeg2->dwLevel = 51;
    mpeg2->dwFlags = 4;

    byte sps[] = {
            103, 66, 128, 10, 218, 1, 12, 4, 118, 128, 109, 10, 19, 80
    };
    byte pps[] = { 104, 206, 6, 242 };

    byte parameters[] = {
        0, 14,
        103, 66, 128, 10, 218, 1, 12, 4, 118, 128, 109, 10, 19, 80,
        0, 4,
        104, 206, 6, 242
    };

    mpeg2->cbSequenceHeader = sizeof(parameters);
    memcpy(mpeg2->dwSequenceHeader, parameters, sizeof(parameters));

    VIDEOINFOHEADER2* pvi2 = &mpeg2->hdr;
    pvi2->bmiHeader.biBitCount = 24;
    pvi2->bmiHeader.biSize = 40;
    pvi2->bmiHeader.biPlanes = 1;
    pvi2->bmiHeader.biWidth = uiWidth;
    pvi2->bmiHeader.biHeight = uiHeight;
    pvi2->bmiHeader.biSize = 40;
    pvi2->bmiHeader.biSizeImage = DIBSIZE(pvi2->bmiHeader);
    pvi2->bmiHeader.biCompression = DWORD('1CVA');
    //pvi2->AvgTimePerFrame = m_tFrame;
    //pvi2->AvgTimePerFrame = 1000000;
    const REFERENCE_TIME FPS_25 = UNITS / 25;
    pvi2->AvgTimePerFrame = FPS_25;
    //SetRect(&pvi2->rcSource, 0, 0, m_cx, m_cy);
    SetRect(&pvi2->rcSource, 0, 0, uiWidth, uiHeight);
    pvi2->rcTarget = pvi2->rcSource;
    pvi2->dwPictAspectRatioX = uiWidth;
    pvi2->dwPictAspectRatioY = uiHeight;
    pvi2->dwBitRate = 1500000;


    return NOERROR;

} // GetMediaType




//
// Notify
// Ignore quality management messages sent from the downstream filter
STDMETHODIMP CVCamStream::Notify(IBaseFilter * pSender, Quality q)
{
    return E_NOTIMPL;
} // Notify

// This method is called after the pins are connected to allocate buffers to stream data
HRESULT CVCamStream::DecideBufferSize(IMemAllocator *pAlloc, ALLOCATOR_PROPERTIES *pProperties)
{
    CAutoLock cAutoLock(m_pFilter->pStateLock());
    HRESULT hr = NOERROR;

    MPEG2VIDEOINFO*mpeg2 = (MPEG2VIDEOINFO*) m_mt.Format();
    VIDEOINFOHEADER2* pvi = &mpeg2->hdr;
    pProperties->cBuffers = 1;
    pProperties->cbBuffer = pvi->bmiHeader.biSizeImage;

    ALLOCATOR_PROPERTIES Actual;
    hr = pAlloc->SetProperties(pProperties,&Actual);

    if(FAILED(hr)) return hr;
    if(Actual.cbBuffer < pProperties->cbBuffer) return E_FAIL;

    return NOERROR;
} // DecideBufferSize

// Called when graph is run
HRESULT CVCamStream::OnThreadCreate()
{
    m_rtLastTime = 0;
    return NOERROR;
} // OnThreadCreate


//////////////////////////////////////////////////////////////////////////
//  IAMStreamConfig
//////////////////////////////////////////////////////////////////////////

HRESULT STDMETHODCALLTYPE CVCamStream::SetFormat(AM_MEDIA_TYPE *pmt)
{
    DECLARE_PTR(VIDEOINFOHEADER, pvi, m_mt.pbFormat);
    m_mt = *pmt;
    IPin* pin; 
    ConnectedTo(&pin);
    if(pin)
    {
        IFilterGraph *pGraph = m_pParent->GetGraph();
        pGraph->Reconnect(this);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CVCamStream::GetFormat(AM_MEDIA_TYPE **ppmt)
{
    *ppmt = CreateMediaType(&m_mt);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CVCamStream::GetNumberOfCapabilities(int *piCount, int *piSize)
{
    *piCount = 8;
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CVCamStream::GetStreamCaps(int iIndex, AM_MEDIA_TYPE **pmt, BYTE *pSCC)
{
    *pmt = CreateMediaType(&m_mt);
    DECLARE_PTR(MPEG2VIDEOINFO, mpeg2, (*pmt)->pbFormat);

    ZeroMemory(mpeg2, sizeof(MPEG2VIDEOINFO));

    mpeg2->dwProfile = 66;
    mpeg2->dwLevel = 31;
    mpeg2->dwFlags = 4;

    VIDEOINFOHEADER2* pvi = &mpeg2->hdr;
    pvi->bmiHeader.biCompression = DWORD('1CVA');
    pvi->bmiHeader.biBitCount = 24;
    pvi->bmiHeader.biSize = 1080 * 2280 * 3;
    pvi->bmiHeader.biSize = 40;
    pvi->bmiHeader.biWidth = 1080;
    pvi->bmiHeader.biHeight = 2280;
    pvi->bmiHeader.biPlanes = 1;
    pvi->bmiHeader.biSizeImage = DIBSIZE(pvi->bmiHeader);
    pvi->bmiHeader.biClrImportant = 0;

    SetRectEmpty(&(pvi->rcSource)); // we want the whole image area rendered.
    SetRectEmpty(&(pvi->rcTarget)); // no particular destination rectangle

    (*pmt)->majortype = MEDIATYPE_Video;
    (*pmt)->subtype = MEDIASUBTYPE_AVC1;
    (*pmt)->formattype = FORMAT_MPEG2_VIDEO;
    (*pmt)->bTemporalCompression = FALSE;
    (*pmt)->bFixedSizeSamples= FALSE;
    (*pmt)->lSampleSize = pvi->bmiHeader.biSizeImage;
    (*pmt)->cbFormat = sizeof(MPEG2VIDEOINFO);

    DECLARE_PTR(VIDEO_STREAM_CONFIG_CAPS, pvscc, pSCC);
    
    pvscc->guid = FORMAT_VideoInfo;
    pvscc->VideoStandard = AnalogVideo_None;
    pvscc->InputSize.cx = 4096;
    pvscc->InputSize.cy = 4096;
    pvscc->MinCroppingSize.cx = 80;
    pvscc->MinCroppingSize.cy = 60;
    pvscc->MaxCroppingSize.cx = 4096;
    pvscc->MaxCroppingSize.cy = 4096;
    pvscc->CropGranularityX = 80;
    pvscc->CropGranularityY = 60;
    pvscc->CropAlignX = 0;
    pvscc->CropAlignY = 0;

    pvscc->MinOutputSize.cx = 80;
    pvscc->MinOutputSize.cy = 60;
    pvscc->MaxOutputSize.cx = 4096;
    pvscc->MaxOutputSize.cy = 4096;
    pvscc->OutputGranularityX = 0;
    pvscc->OutputGranularityY = 0;
    pvscc->StretchTapsX = 0;
    pvscc->StretchTapsY = 0;
    pvscc->ShrinkTapsX = 0;
    pvscc->ShrinkTapsY = 0;
    pvscc->MinFrameInterval = 100000;   //50 fps
    pvscc->MaxFrameInterval = 50000000; // 0.2 fps
    pvscc->MinBitsPerSecond = (80 * 60 * 3 * 8) / 5;
    pvscc->MaxBitsPerSecond = 640 * 480 * 3 * 8 * 50;

    return S_OK;
}

//////////////////////////////////////////////////////////////////////////
// IKsPropertySet
//////////////////////////////////////////////////////////////////////////


HRESULT CVCamStream::Set(REFGUID guidPropSet, DWORD dwID, void *pInstanceData, 
                        DWORD cbInstanceData, void *pPropData, DWORD cbPropData)
{// Set: Cannot set any properties.
    return E_NOTIMPL;
}

// Get: Return the pin category (our only property). 
HRESULT CVCamStream::Get(
    REFGUID guidPropSet,   // Which property set.
    DWORD dwPropID,        // Which property in that set.
    void *pInstanceData,   // Instance data (ignore).
    DWORD cbInstanceData,  // Size of the instance data (ignore).
    void *pPropData,       // Buffer to receive the property data.
    DWORD cbPropData,      // Size of the buffer.
    DWORD *pcbReturned     // Return the size of the property.
)
{
    if (guidPropSet != AMPROPSETID_Pin)             return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY)        return E_PROP_ID_UNSUPPORTED;
    if (pPropData == NULL && pcbReturned == NULL)   return E_POINTER;
    
    if (pcbReturned) *pcbReturned = sizeof(GUID);
    if (pPropData == NULL)          return S_OK; // Caller just wants to know the size. 
    if (cbPropData < sizeof(GUID))  return E_UNEXPECTED;// The buffer is too small.
        
    *(GUID *)pPropData = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

// QuerySupported: Query whether the pin supports the specified property.
HRESULT CVCamStream::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD *pTypeSupport)
{
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    // We support getting this property, but not setting it.
    if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET; 
    return S_OK;
}
