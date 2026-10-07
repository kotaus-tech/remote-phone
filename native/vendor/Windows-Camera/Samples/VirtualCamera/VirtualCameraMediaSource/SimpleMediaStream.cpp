//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#include "pch.h"

namespace
{
    struct SupportedMode
    {
        GUID subtype;
        UINT32 width;
        UINT32 height;
        UINT32 framesPerSecond;
    };

    HRESULT CreateVideoMediaType(
        _In_ const SupportedMode& mode,
        _Out_ wil::com_ptr_nothrow<IMFMediaType>& mediaType)
    {
        RETURN_IF_FAILED(MFCreateMediaType(&mediaType));
        RETURN_IF_FAILED(mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        RETURN_IF_FAILED(mediaType->SetGUID(MF_MT_SUBTYPE, mode.subtype));
        RETURN_IF_FAILED(mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        RETURN_IF_FAILED(mediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
        RETURN_IF_FAILED(mediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
        RETURN_IF_FAILED(MFSetAttributeSize(mediaType.get(), MF_MT_FRAME_SIZE, mode.width, mode.height));
        RETURN_IF_FAILED(MFSetAttributeRatio(mediaType.get(), MF_MT_FRAME_RATE, mode.framesPerSecond, 1));
        RETURN_IF_FAILED(MFSetAttributeRatio(mediaType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

        const ULONGLONG bitsPerPixel = IsEqualGUID(mode.subtype, MFVideoFormat_NV12) ? 12ULL : 32ULL;
        const ULONGLONG bitRate =
            static_cast<ULONGLONG>(mode.width) * mode.height * bitsPerPixel * mode.framesPerSecond;
        // MF_MT_AVG_BITRATE is UINT32; saturate the descriptive value rather than
        // rejecting valid uncompressed NV12 4K60 (whose mathematical rate exceeds it).
        const UINT32 averageBitRate = bitRate > MAXDWORD ? MAXDWORD : static_cast<UINT32>(bitRate);
        RETURN_IF_FAILED(mediaType->SetUINT32(MF_MT_AVG_BITRATE, averageBitRate));
        return S_OK;
    }
}

namespace winrt::WindowsSample::implementation
{
    HRESULT SimpleMediaStream::Initialize(
            _In_ SimpleMediaSource* pSource,
            _In_ DWORD dwStreamId,
            _In_ MFSampleAllocatorUsage allocatorUsage,
            _In_opt_z_ PCWSTR diagnosticLogPath
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        wil::com_ptr_nothrow<IMFMediaTypeHandler> spTypeHandler;
        wil::com_ptr_nothrow<IMFAttributes> attrs;

        RETURN_HR_IF_NULL(E_INVALIDARG, pSource);
        m_parent = pSource;

        m_dwStreamId = dwStreamId;
        m_allocatorUsage = allocatorUsage;
        m_diagnosticLogPath = diagnosticLogPath == nullptr ? L"" : diagnosticLogPath;

        const SupportedMode supportedModes[] =
        {
            { MFVideoFormat_NV12, 1280, 720, 30 },
            { MFVideoFormat_NV12, 1280, 720, 60 },
            { MFVideoFormat_NV12, 1920, 1080, 30 },
            { MFVideoFormat_NV12, 1920, 1080, 60 },
            { MFVideoFormat_NV12, 3840, 2160, 30 },
            { MFVideoFormat_NV12, 3840, 2160, 60 },
            { MFVideoFormat_RGB32, 640, 480, 30 },
        };
        constexpr DWORD mediaTypeCount = static_cast<DWORD>(ARRAYSIZE(supportedModes));
        auto mediaTypeList = wilEx::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFMediaType>>(mediaTypeCount);
        RETURN_IF_NULL_ALLOC(mediaTypeList.get());

        for (DWORD index = 0; index < mediaTypeCount; ++index)
        {
            wil::com_ptr_nothrow<IMFMediaType> mediaType;
            RETURN_IF_FAILED(CreateVideoMediaType(supportedModes[index], mediaType));
            mediaTypeList[index] = mediaType.detach();
        }

        RETURN_IF_FAILED(MFCreateAttributes(&m_spAttributes, 10));
        RETURN_IF_FAILED(_SetStreamAttributes(m_spAttributes.get()));

        RETURN_IF_FAILED(MFCreateEventQueue(&m_spEventQueue));

        // Initialize stream descriptors
        RETURN_IF_FAILED(MFCreateStreamDescriptor(
            m_dwStreamId /*StreamId*/,
            mediaTypeCount /*MT count*/,
            mediaTypeList.get(),
            &m_spStreamDesc));

        RETURN_IF_FAILED(m_spStreamDesc->GetMediaTypeHandler(&spTypeHandler));
        RETURN_IF_FAILED(spTypeHandler->SetCurrentMediaType(mediaTypeList[0]));
        RETURN_IF_FAILED(_SetStreamDescriptorAttributes(m_spStreamDesc.get()));

        return S_OK;
    }

    // IMFMediaEventGenerator
    IFACEMETHODIMP SimpleMediaStream::BeginGetEvent(
            _In_ IMFAsyncCallback* pCallback,
            _In_ IUnknown* punkState
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_spEventQueue->BeginGetEvent(pCallback, punkState));

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::EndGetEvent(
            _In_ IMFAsyncResult* pResult,
            _COM_Outptr_ IMFMediaEvent** ppEvent
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_spEventQueue->EndGetEvent(pResult, ppEvent));

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::GetEvent(
            _In_ DWORD dwFlags,
            _COM_Outptr_ IMFMediaEvent** ppEvent
        )
    {
        // NOTE:
        // GetEvent can block indefinitely, so we don't hold the lock.
        // This requires some juggling with the event queue pointer.

        wil::com_ptr_nothrow<IMFMediaEventQueue> spQueue;

        {
            winrt::slim_lock_guard lock(m_Lock);

            RETURN_IF_FAILED(_CheckShutdownRequiresLock());
            spQueue = m_spEventQueue;
        }

        // Now get the event.
        RETURN_IF_FAILED(spQueue->GetEvent(dwFlags, ppEvent));

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::QueueEvent(
            _In_ MediaEventType eventType,
            _In_ REFGUID guidExtendedType,
            _In_ HRESULT hrStatus,
            _In_opt_ PROPVARIANT const* pvValue
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_spEventQueue->QueueEventParamVar(eventType, guidExtendedType, hrStatus, pvValue));

        return S_OK;
    }

    // IMFMediaStream
    IFACEMETHODIMP SimpleMediaStream::GetMediaSource(
            _COM_Outptr_ IMFMediaSource** ppMediaSource
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
        *ppMediaSource = nullptr;

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());
        RETURN_IF_FAILED(m_parent.copy_to(ppMediaSource));

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::GetStreamDescriptor(
            _COM_Outptr_ IMFStreamDescriptor** ppStreamDescriptor
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
        *ppStreamDescriptor = nullptr;

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        if (m_spStreamDesc != nullptr)
        {
            RETURN_IF_FAILED(m_spStreamDesc.copy_to(ppStreamDescriptor));
        }
        else
        {
            return E_UNEXPECTED;
        }

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::RequestSample(
            _In_ IUnknown* pToken
        )
    {
        winrt::slim_lock_guard lock(m_Lock);
        wil::com_ptr_nothrow<IMFSample> sample;
        wil::com_ptr_nothrow<IMFMediaBuffer> outputBuffer;
        LONG pitch = 0;
        BYTE* bufferStart = nullptr; // not used
        DWORD bufferLength = 0;
        BYTE* pbuf = nullptr;
        wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;

        RETURN_IF_FAILED(LogMediaSourceHRESULT(
            m_diagnosticLogPath.c_str(),
            L"RequestSample shutdown check",
            _CheckShutdownRequiresLock()));

        if (m_streamState != MF_STREAM_STATE_RUNNING)
        {
            WriteMediaSourceDiagnostic(
                m_diagnosticLogPath.c_str(),
                L"media_source_error stage=RequestSample state=%u selected=%u hresult=0x%08X",
                static_cast<unsigned>(m_streamState),
                static_cast<unsigned>(m_bSelected),
                static_cast<unsigned>(MF_E_INVALIDREQUEST));
            return MF_E_INVALIDREQUEST;
        }

        const ULONGLONG requestTime = static_cast<ULONGLONG>(MFGetSystemTime());
        if (m_requestWindowSampleCount == 0)
        {
            m_requestWindowStart = requestTime;
            m_requestWindowSampleCount = 1;
            m_minRequestInterval = 0;
            m_maxRequestInterval = 0;
        }
        else
        {
            if (requestTime > m_lastRequestTime)
            {
                const ULONGLONG interval = requestTime - m_lastRequestTime;
                if (m_minRequestInterval == 0 || interval < m_minRequestInterval)
                {
                    m_minRequestInterval = interval;
                }
                if (interval > m_maxRequestInterval)
                {
                    m_maxRequestInterval = interval;
                }
            }
            ++m_requestWindowSampleCount;

            const ULONGLONG elapsed = requestTime - m_requestWindowStart;
            if (elapsed >= 10000000ULL && m_requestWindowSampleCount > 1)
            {
                const ULONGLONG intervals = m_requestWindowSampleCount - 1;
                const ULONGLONG rateTenths = (intervals * 100000000ULL) / elapsed;
                const ULONGLONG averageIntervalUs = (elapsed / intervals) / 10ULL;
                WriteMediaSourceDiagnostic(
                    m_diagnosticLogPath.c_str(),
                    L"request_sample_cadence request_rate_fps=%I64u.%I64u intervals=%I64u average_interval_us=%I64u min_interval_us=%I64u max_interval_us=%I64u",
                    static_cast<unsigned long long>(rateTenths / 10ULL),
                    static_cast<unsigned long long>(rateTenths % 10ULL),
                    static_cast<unsigned long long>(intervals),
                    static_cast<unsigned long long>(averageIntervalUs),
                    static_cast<unsigned long long>(m_minRequestInterval / 10ULL),
                    static_cast<unsigned long long>(m_maxRequestInterval / 10ULL));

                m_requestWindowStart = requestTime;
                m_requestWindowSampleCount = 1;
                m_minRequestInterval = 0;
                m_maxRequestInterval = 0;
            }
        }
        m_lastRequestTime = requestTime;

        const auto logFailure = [this](PCWSTR stage, HRESULT result)
        {
            return LogMediaSourceHRESULT(m_diagnosticLogPath.c_str(), stage, result);
        };
        RETURN_IF_FAILED(logFailure(L"AllocateSample", m_spSampleAllocator->AllocateSample(&sample)));
        RETURN_IF_FAILED(logFailure(L"GetBufferByIndex", sample->GetBufferByIndex(0, &outputBuffer)));
        RETURN_IF_FAILED(logFailure(L"QueryInterface(IMF2DBuffer2)", outputBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D))));
        RETURN_IF_FAILED(logFailure(L"Lock2DSize", buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write,
            &pbuf,
            &pitch,
            &bufferStart,
            &bufferLength)));

        RETURN_IF_FAILED(logFailure(L"CreateFrame", m_spFrameGenerator->CreateFrame(pbuf, bufferLength, pitch, m_rgbMask)));
        //RETURN_IF_FAILED(WriteSampleData(pbuf, bufferLength, pitch, NUM_IMAGE_COLS, NUM_IMAGE_ROWS));
        RETURN_IF_FAILED(logFailure(L"Unlock2D", buffer2D->Unlock2D()));

        RETURN_IF_FAILED(logFailure(L"SetSampleTime", sample->SetSampleTime(MFGetSystemTime())));
        RETURN_IF_FAILED(logFailure(
            L"SetSampleDuration",
            sample->SetSampleDuration(static_cast<LONGLONG>(m_sampleDuration100ns))));
        if (pToken != nullptr)
        {
            RETURN_IF_FAILED(logFailure(L"SetSampleToken", sample->SetUnknown(MFSampleExtension_Token, pToken)));
        }
        RETURN_IF_FAILED(logFailure(L"QueueSample", m_spEventQueue->QueueEventParamUnk(MEMediaSample,
            GUID_NULL,
            S_OK,
            sample.get())));

        return S_OK;
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // IMFMediaStream2
    IFACEMETHODIMP SimpleMediaStream::SetStreamState(MF_STREAM_STATE state)
    {
        winrt::slim_lock_guard lock(m_Lock);
        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        if (m_streamState == state)
        {
            return S_OK;
        }

        switch (state)
        {
        case MF_STREAM_STATE_PAUSED:
            if (m_streamState != MF_STREAM_STATE_RUNNING)
            {
                return MF_E_INVALID_STATE_TRANSITION;
            }
            m_streamState = MF_STREAM_STATE_PAUSED;
            break;

        case MF_STREAM_STATE_RUNNING:
            RETURN_IF_FAILED(StartInternal(false, nullptr));
            break;

        case MF_STREAM_STATE_STOPPED:
            RETURN_IF_FAILED(StopInternal(false));

            break;

        default:
            return MF_E_INVALID_STATE_TRANSITION;
            break;
        }

        return S_OK;
    }

    IFACEMETHODIMP SimpleMediaStream::GetStreamState(
            _Out_ MF_STREAM_STATE* pState
        )
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        RETURN_HR_IF_NULL(E_INVALIDARG, pState);
        *pState = m_streamState;

        return S_OK;
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // Public methods
    HRESULT SimpleMediaStream::Start(_In_ IMFMediaType* pMediaType)
    {
        // Set stream seleted state to true, and update current mediatype.
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_HR_IF_NULL(E_INVALIDARG, pMediaType);
        if (m_spMediaType == nullptr)
        {
            m_spMediaType = pMediaType;
        }
        m_bSelected = true;

        // Change Stream state to running.
        const HRESULT startResult = StartInternal(true, pMediaType);
        if (FAILED(startResult))
        {
            WriteMediaSourceDiagnostic(
                m_diagnosticLogPath.c_str(),
                L"media_source_error stage=StartInternal hresult=0x%08X",
                static_cast<unsigned>(startResult));
            return startResult;
        }

        m_requestWindowStart = 0;
        m_lastRequestTime = 0;
        m_requestWindowSampleCount = 0;
        m_minRequestInterval = 0;
        m_maxRequestInterval = 0;

        GUID subtype = GUID_NULL;
        UINT32 width = 0;
        UINT32 height = 0;
        UINT32 frameRateNumerator = 0;
        UINT32 frameRateDenominator = 0;
        (void)m_spMediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
        (void)MFGetAttributeSize(m_spMediaType.get(), MF_MT_FRAME_SIZE, &width, &height);
        (void)MFGetAttributeRatio(
            m_spMediaType.get(),
            MF_MT_FRAME_RATE,
            &frameRateNumerator,
            &frameRateDenominator);
        WCHAR subtypeGuid[64] = {};
        StringFromGUID2(subtype, subtypeGuid, ARRAYSIZE(subtypeGuid));
        PCWSTR subtypeName = IsEqualGUID(subtype, MFVideoFormat_NV12)
            ? L"NV12"
            : (IsEqualGUID(subtype, MFVideoFormat_RGB32) ? L"RGB32" : L"other");
        WriteMediaSourceDiagnostic(
            m_diagnosticLogPath.c_str(),
            L"selected_media_type stream_id=%u subtype=%s subtype_guid=%s frame_size=%ux%u frame_rate=%u/%u sample_duration_100ns=%I64u",
            static_cast<unsigned>(m_dwStreamId),
            subtypeName,
            subtypeGuid,
            static_cast<unsigned>(width),
            static_cast<unsigned>(height),
            static_cast<unsigned>(frameRateNumerator),
            static_cast<unsigned>(frameRateDenominator),
            static_cast<unsigned long long>(m_sampleDuration100ns));

        return S_OK;
    }

    _Requires_lock_held_(m_Lock)
    HRESULT SimpleMediaStream::Stop(_In_ bool bSendEvent)
    {
        winrt::slim_lock_guard lock(m_Lock);

        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        m_bSelected = false;

        RETURN_IF_FAILED(StopInternal(bSendEvent));
        return S_OK;
    }

    HRESULT SimpleMediaStream::Shutdown()
    {
        winrt::slim_lock_guard lock(m_Lock);

        m_bIsShutdown = true;
        m_parent.reset();

        if (m_spEventQueue != nullptr)
        {
            m_spEventQueue->Shutdown();
            m_spEventQueue.reset();
        }

        m_spAttributes.reset();
        m_spStreamDesc.reset();

        m_streamState = MF_STREAM_STATE_STOPPED;

        return S_OK;
    }

    HRESULT SimpleMediaStream::SetSampleAllocator(IMFVideoSampleAllocator* pAllocator)
    {
        winrt::slim_lock_guard lock(m_Lock);
        RETURN_IF_FAILED(_CheckShutdownRequiresLock());

        if (m_streamState == MF_STREAM_STATE_RUNNING)
        {
            RETURN_HR_MSG(MF_E_INVALIDREQUEST, "Cannot update allocator when the stream is streaming");
        }
        m_spSampleAllocator.reset();
        m_spSampleAllocator = pAllocator;

        return S_OK;
    }


    //////////////////////////////////////////////////////////////////////////////////////////
    // Private methods

    HRESULT SimpleMediaStream::_CheckShutdownRequiresLock()
    {
        if (m_bIsShutdown)
        {
            return MF_E_SHUTDOWN;
        }

        if (m_spEventQueue == nullptr)
        {
            return E_UNEXPECTED;

        }
        return S_OK;
    }

    HRESULT SimpleMediaStream::_SetStreamAttributes(
            _In_ IMFAttributes* pAttributeStore
        )
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, pAttributeStore);

        RETURN_IF_FAILED(pAttributeStore->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_STREAM_ID, m_dwStreamId));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

        return S_OK;
    }

    HRESULT SimpleMediaStream::_SetStreamDescriptorAttributes(
            _In_ IMFAttributes* pAttributeStore
        )
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, pAttributeStore);

        RETURN_IF_FAILED(pAttributeStore->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_STREAM_ID, m_dwStreamId));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
        RETURN_IF_FAILED(pAttributeStore->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

        return S_OK;
    }

    _Requires_lock_held_(m_Lock)
    HRESULT SimpleMediaStream::StartInternal(bool bSendEvent, IMFMediaType* pNewMediaType)
    {
        BOOL bMatch = FALSE;
        if (m_spMediaType && pNewMediaType)
        {
            (void)m_spMediaType->Compare(pNewMediaType, MF_ATTRIBUTES_MATCH_ALL_ITEMS, &bMatch);

            if (!bMatch)
            {
                // update media type
                m_spMediaType = pNewMediaType;
            }
        }

        UINT32 durationFrameRateNumerator = 0;
        UINT32 durationFrameRateDenominator = 0;
        RETURN_IF_FAILED(MFGetAttributeRatio(
            m_spMediaType.get(),
            MF_MT_FRAME_RATE,
            &durationFrameRateNumerator,
            &durationFrameRateDenominator));
        RETURN_HR_IF(E_INVALIDARG, durationFrameRateNumerator == 0 || durationFrameRateDenominator == 0);
        m_sampleDuration100ns =
            (10000000ULL * durationFrameRateDenominator + durationFrameRateNumerator / 2) /
            durationFrameRateNumerator;

        if ((m_streamState != MF_STREAM_STATE_RUNNING) || !bMatch)
        {
            // Create the allocator if one doesn't exist
            if (m_allocatorUsage == MFSampleAllocatorUsage_UsesProvidedAllocator)
            {
                RETURN_HR_IF_NULL_MSG(E_POINTER, m_spSampleAllocator, "Sample allocator is not set");
            }
            else
            {
                RETURN_IF_FAILED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&m_spSampleAllocator)));
            }

            UINT32 width, height;
            GUID subType;
            RETURN_IF_FAILED(m_spMediaType->GetGUID(MF_MT_SUBTYPE, &subType));
            MFGetAttributeSize(m_spMediaType.get(), MF_MT_FRAME_SIZE, &width, &height);

            DEBUG_MSG(L"Initialize sample allocator for mediatype: %s, %dx%d ", winrt::to_hstring(subType).data(), width, height);
            RETURN_IF_FAILED(m_spSampleAllocator->InitializeSampleAllocator(10, m_spMediaType.get()));
            if (m_spFrameGenerator == nullptr)
            {
                m_spFrameGenerator = wil::make_unique_nothrow<SimpleFrameGenerator>();
                RETURN_IF_NULL_ALLOC_MSG(m_spFrameGenerator, "Fail to create SimpleFrameGenerator");
            }
            RETURN_IF_FAILED(m_spFrameGenerator->Initialize(m_spMediaType.get()));
        }

        if (bSendEvent)
        {
            // Post MEStreamStarted event to signal stream has started
            RETURN_IF_FAILED(m_spEventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
        }

        // Set stream state
        m_streamState = MF_STREAM_STATE_RUNNING;

        return S_OK;
    }

    _Requires_lock_held_(m_Lock)
    HRESULT SimpleMediaStream::StopInternal(bool bSendEvent)
    {
        if (m_requestWindowSampleCount > 1 && m_lastRequestTime > m_requestWindowStart)
        {
            const ULONGLONG intervals = m_requestWindowSampleCount - 1;
            const ULONGLONG elapsed = m_lastRequestTime - m_requestWindowStart;
            const ULONGLONG rateTenths = (intervals * 100000000ULL) / elapsed;
            const ULONGLONG averageIntervalUs = (elapsed / intervals) / 10ULL;
            WriteMediaSourceDiagnostic(
                m_diagnosticLogPath.c_str(),
                L"request_sample_cadence final=1 request_rate_fps=%I64u.%I64u intervals=%I64u average_interval_us=%I64u min_interval_us=%I64u max_interval_us=%I64u",
                static_cast<unsigned long long>(rateTenths / 10ULL),
                static_cast<unsigned long long>(rateTenths % 10ULL),
                static_cast<unsigned long long>(intervals),
                static_cast<unsigned long long>(averageIntervalUs),
                static_cast<unsigned long long>(m_minRequestInterval / 10ULL),
                static_cast<unsigned long long>(m_maxRequestInterval / 10ULL));
        }
        WriteMediaSourceDiagnostic(
            m_diagnosticLogPath.c_str(),
            L"stream_stopped stream_id=%u window_samples=%I64u",
            static_cast<unsigned>(m_dwStreamId),
            static_cast<unsigned long long>(m_requestWindowSampleCount));

        // Set stream state
        m_streamState = MF_STREAM_STATE_STOPPED;

        // NOTE: if implementation has sampleRequestQueue or sampleQueue, it must flush the queue on stopped.
        if (bSendEvent)
        {
            // Post MEStreamStopped event to signal stream has stopped
            RETURN_IF_FAILED(m_spEventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
        }

        return S_OK;
    }
}
