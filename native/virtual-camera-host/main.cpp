#include <windows.h>
#include <knownfolders.h>
#include <sddl.h>
#include <aclapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfvirtualcamera.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "SharedNv12FrameBuffer.h"
#include "VirtualCameraMediaSource.h"

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kCameraFriendlyName[] = L"Видоискатель — тестовая камера";
constexpr wchar_t kDiagnosticLogRelativePath[] = L"\\Kotaus\\RemotePhone\\logs\\VirtualCameraMediaSource.log";
constexpr int kFrameCaptureCount = 5;
constexpr UINT32 kSmokeWidth = 3840;
constexpr UINT32 kSmokeHeight = 2160;
constexpr UINT32 kSmokeFrameRate = 30;
constexpr LONGLONG kSmokeSampleDuration100ns = 333333;
constexpr UINT32 kFramePipeMaxBytes =
    kSmokeWidth * kSmokeHeight * 3U / 2U;

void WriteWideText(DWORD standardHandle, const wchar_t* text);

enum class FramePipeReadStatus {
    Frame,
    EndOfStream,
    Invalid
};

enum class PipeReadExactStatus {
    Complete,
    EndOfStream,
    Failed
};

bool IsValidFramePipeHeader(const remotephone::frame::Nv12FramePipeHeader& header) {
    return header.magic == remotephone::frame::kNv12FramePipeMagic
        && header.version == 1
        && header.headerBytes == sizeof(remotephone::frame::Nv12FramePipeHeader)
        && header.flags == 0
        && remotephone::frame::IsValidNv12FrameDimensions(header.width, header.height)
        && header.payloadBytes == remotephone::frame::Nv12FrameByteLength(header.width, header.height)
        && header.payloadBytes <= kFramePipeMaxBytes;
}

PipeReadExactStatus ReadPipeExact(HANDLE pipe, BYTE* destination, DWORD byteCount, bool allowCleanEof) {
    DWORD totalRead = 0;
    while (totalRead < byteCount) {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, destination + totalRead, byteCount - totalRead, &bytesRead, nullptr)) {
            const DWORD error = GetLastError();
            if (allowCleanEof && totalRead == 0 && error == ERROR_BROKEN_PIPE) {
                return PipeReadExactStatus::EndOfStream;
            }
            return PipeReadExactStatus::Failed;
        }
        if (bytesRead == 0) {
            return totalRead == 0 && allowCleanEof
                ? PipeReadExactStatus::EndOfStream
                : PipeReadExactStatus::Failed;
        }
        totalRead += bytesRead;
    }
    return PipeReadExactStatus::Complete;
}

FramePipeReadStatus ReadFramePipePacket(
    HANDLE pipe,
    remotephone::frame::Nv12FramePipeHeader* header,
    std::vector<BYTE>* payload) {
    if (header == nullptr || payload == nullptr) {
        return FramePipeReadStatus::Invalid;
    }
    std::array<BYTE, sizeof(remotephone::frame::Nv12FramePipeHeader)> headerBytes{};
    const PipeReadExactStatus headerStatus = ReadPipeExact(
        pipe,
        headerBytes.data(),
        static_cast<DWORD>(headerBytes.size()),
        true);
    if (headerStatus == PipeReadExactStatus::EndOfStream) {
        return FramePipeReadStatus::EndOfStream;
    }
    if (headerStatus != PipeReadExactStatus::Complete) {
        return FramePipeReadStatus::Invalid;
    }
    std::memcpy(header, headerBytes.data(), sizeof(*header));
    if (!IsValidFramePipeHeader(*header)) {
        return FramePipeReadStatus::Invalid;
    }

    payload->assign(header->payloadBytes, 0);
    const PipeReadExactStatus payloadStatus = ReadPipeExact(
        pipe,
        payload->data(),
        header->payloadBytes,
        false);
    if (payloadStatus != PipeReadExactStatus::Complete) {
        if (!payload->empty()) {
            SecureZeroMemory(payload->data(), payload->size());
        }
        payload->clear();
        return FramePipeReadStatus::Invalid;
    }
    return FramePipeReadStatus::Frame;
}

HRESULT GetCurrentUserSidString(std::wstring* sidText) {
    if (sidText == nullptr) {
        return E_POINTER;
    }
    sidText->clear();

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    DWORD requiredBytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &requiredBytes);
    const DWORD queryError = GetLastError();
    if (requiredBytes == 0 || queryError != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(token);
        return HRESULT_FROM_WIN32(queryError == ERROR_SUCCESS ? ERROR_INVALID_DATA : queryError);
    }

    std::vector<BYTE> tokenBuffer(requiredBytes);
    if (!GetTokenInformation(token, TokenUser, tokenBuffer.data(), requiredBytes, &requiredBytes)) {
        const DWORD error = GetLastError();
        CloseHandle(token);
        return HRESULT_FROM_WIN32(error);
    }
    CloseHandle(token);

    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
    LPWSTR sidString = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidString) || sidString == nullptr) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    *sidText = sidString;
    LocalFree(sidString);
    return S_OK;
}

HRESULT CreateFrameChannelName(std::wstring* channelName) {
    if (channelName == nullptr) {
        return E_POINTER;
    }
    channelName->clear();
    GUID channelGuid{};
    HRESULT result = CoCreateGuid(&channelGuid);
    if (FAILED(result)) {
        return result;
    }
    wchar_t guidText[40] = {};
    if (StringFromGUID2(channelGuid, guidText, ARRAYSIZE(guidText)) != 39) {
        return E_FAIL;
    }
    *channelName = remotephone::frame::kGlobalFrameChannelPrefix;
    channelName->append(guidText);
    return S_OK;
}

HRESULT RunFramePipe(HANDLE inputPipe, const std::wstring& channelName) {
    remotephone::frame::SharedNv12FrameBuffer writer;
    std::vector<BYTE> payload;
    remotephone::frame::Nv12FramePipeHeader packet{};
    bool channelReadyReported = false;

    for (;;) {
        const FramePipeReadStatus readStatus = ReadFramePipePacket(inputPipe, &packet, &payload);
        if (readStatus == FramePipeReadStatus::EndOfStream) {
            return S_OK;
        }
        if (readStatus != FramePipeReadStatus::Frame) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        const HRESULT openResult = writer.OpenForWriter(channelName);
        if (openResult == S_OK) {
            const HRESULT publishResult = writer.PublishNv12(
                payload.data(),
                packet.width,
                packet.height,
                packet.payloadBytes,
                packet.timestampNs);
            if (FAILED(publishResult)) {
                SecureZeroMemory(payload.data(), payload.size());
                return publishResult;
            }
            if (!channelReadyReported) {
                WriteWideText(STD_OUTPUT_HANDLE, L"CPU_FRAME_CHANNEL=READY\n");
                channelReadyReported = true;
            }
        } else if (FAILED(openResult) && openResult != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            && openResult != HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)
            && openResult != HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD)) {
            SecureZeroMemory(payload.data(), payload.size());
            return openResult;
        }

        SecureZeroMemory(payload.data(), payload.size());
        payload.clear();
    }
}

struct SupportedMode {
    GUID subtype;
    UINT32 width;
    UINT32 height;
    UINT32 framesPerSecond;
    const wchar_t* name;
};

constexpr size_t kSupportedModeCount = 7;
const std::array<SupportedMode, kSupportedModeCount> kSupportedModes = {{
    { MFVideoFormat_NV12, 1280, 720, 30, L"NV12 1280x720@30" },
    { MFVideoFormat_NV12, 1280, 720, 60, L"NV12 1280x720@60" },
    { MFVideoFormat_NV12, 1920, 1080, 30, L"NV12 1920x1080@30" },
    { MFVideoFormat_NV12, 1920, 1080, 60, L"NV12 1920x1080@60" },
    { MFVideoFormat_NV12, 3840, 2160, 30, L"NV12 3840x2160@30" },
    { MFVideoFormat_NV12, 3840, 2160, 60, L"NV12 3840x2160@60" },
    { MFVideoFormat_RGB32, 640, 480, 30, L"RGB32 640x480@30" },
}};

void WriteWideText(DWORD standardHandle, const wchar_t* text) {
    HANDLE handle = GetStdHandle(standardHandle);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || text == nullptr) {
        return;
    }

    const int characterCount = static_cast<int>(wcslen(text));
    if (characterCount == 0) {
        return;
    }

    DWORD consoleMode = 0;
    if (GetConsoleMode(handle, &consoleMode)) {
        DWORD charactersWritten = 0;
        WriteConsoleW(handle, text, static_cast<DWORD>(characterCount), &charactersWritten, nullptr);
        return;
    }

    UINT outputCodePage = GetConsoleOutputCP();
    if (outputCodePage == 0) {
        outputCodePage = CP_UTF8;
    }
    const int byteCount = WideCharToMultiByte(outputCodePage, 0, text, characterCount, nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0) {
        return;
    }
    std::string encoded(static_cast<size_t>(byteCount), '\0');
    if (WideCharToMultiByte(outputCodePage, 0, text, characterCount, encoded.data(), byteCount, nullptr, nullptr) != byteCount) {
        return;
    }

    DWORD bytesWritten = 0;
    if (!WriteFile(handle, encoded.data(), static_cast<DWORD>(byteCount), &bytesWritten, nullptr) ||
        bytesWritten != static_cast<DWORD>(byteCount)) {
        return;
    }
}

void ReportFailure(const wchar_t* action, HRESULT result) {
    wchar_t message[512] = {};
    swprintf_s(
        message,
        ARRAYSIZE(message),
        L"Ошибка: %s (код 0x%08lX).\n",
        action,
        static_cast<unsigned long>(result));
    OutputDebugStringW(message);
    WriteWideText(STD_ERROR_HANDLE, message);
}

class Apartment final {
public:
    HRESULT Initialize() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_initialized = SUCCEEDED(result);
        return result;
    }

    ~Apartment() {
        if (m_initialized) {
            CoUninitialize();
        }
    }

private:
    bool m_initialized = false;
};

class MediaFoundation final {
public:
    HRESULT Initialize() {
        const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        m_initialized = SUCCEEDED(result);
        return result;
    }

    ~MediaFoundation() {
        if (m_initialized) {
            MFShutdown();
        }
    }

private:
    bool m_initialized = false;
};

struct ActivateList final {
    IMFActivate** items = nullptr;
    UINT32 count = 0;

    ~ActivateList() {
        if (items != nullptr) {
            for (UINT32 index = 0; index < count; ++index) {
                if (items[index] != nullptr) {
                    items[index]->Release();
                }
            }
            CoTaskMemFree(items);
        }
    }
};

std::wstring GetDiagnosticLogPath() {
    PWSTR programData = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &programData);
    if (FAILED(result) || programData == nullptr) {
        if (programData != nullptr) {
            CoTaskMemFree(programData);
        }
        return {};
    }

    std::wstring path(programData);
    CoTaskMemFree(programData);
    path.append(kDiagnosticLogRelativePath);
    return path;
}

HRESULT EnsureDiagnosticLogWritable(const std::wstring& path) {
    if (path.empty()) {
        return E_FAIL;
    }

    HANDLE file = CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(file);
    return S_OK;
}

HRESULT FindTestCameraSource(_Out_ IMFMediaSource** mediaSource) {
    if (mediaSource == nullptr) {
        return E_POINTER;
    }
    *mediaSource = nullptr;

    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 1);
    if (FAILED(result)) {
        return result;
    }
    result = attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(result)) {
        return result;
    }

    ActivateList devices;
    result = MFEnumDeviceSources(attributes.Get(), &devices.items, &devices.count);
    if (FAILED(result)) {
        return result;
    }

    for (UINT32 index = 0; index < devices.count; ++index) {
        if (devices.items[index] == nullptr) {
            continue;
        }

        LPWSTR friendlyName = nullptr;
        UINT32 nameLength = 0;
        const HRESULT nameResult = devices.items[index]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
            &friendlyName,
            &nameLength);
        const std::wstring reportedName = SUCCEEDED(nameResult) && friendlyName != nullptr
            ? std::wstring(friendlyName)
            : std::wstring();
        const bool isTestCamera = reportedName.rfind(kCameraFriendlyName, 0) == 0;
        (void)nameLength;
        CoTaskMemFree(friendlyName);
        if (!isTestCamera) {
            continue;
        }

        return devices.items[index]->ActivateObject(IID_PPV_ARGS(mediaSource));
    }

    return MF_E_NOT_FOUND;
}

ULONGLONG HashSample(IMFSample* sample, HRESULT* result) {
    if (sample == nullptr || result == nullptr) {
        if (result != nullptr) {
            *result = E_POINTER;
        }
        return 0;
    }

    ComPtr<IMFMediaBuffer> buffer;
    *result = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(*result)) {
        return 0;
    }

    BYTE* bytes = nullptr;
    DWORD maximumLength = 0;
    DWORD currentLength = 0;
    *result = buffer->Lock(&bytes, &maximumLength, &currentLength);
    if (FAILED(*result)) {
        return 0;
    }
    if (bytes == nullptr || currentLength == 0 || currentLength > maximumLength) {
        buffer->Unlock();
        *result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return 0;
    }

    ULONGLONG hash = 14695981039346656037ULL;
    for (DWORD index = 0; index < currentLength; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    *result = buffer->Unlock();
    return hash;
}

HRESULT RunMediaFoundationCaptureSmokeTest() {
    ComPtr<IMFMediaSource> mediaSource;
    HRESULT result = [&]() -> HRESULT {
        HRESULT current = FindTestCameraSource(&mediaSource);
        if (FAILED(current)) {
            return current;
        }

        ComPtr<IMFAttributes> readerAttributes;
        current = MFCreateAttributes(&readerAttributes, 1);
        if (FAILED(current)) {
            return current;
        }
        current = readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
        if (FAILED(current)) {
            return current;
        }

        ComPtr<IMFSourceReader> reader;
        current = MFCreateSourceReaderFromMediaSource(mediaSource.Get(), readerAttributes.Get(), &reader);
        if (FAILED(current)) {
            return current;
        }
        current = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        if (FAILED(current)) {
            return current;
        }

        std::array<bool, kSupportedModeCount> foundModes{};
        ComPtr<IMFMediaType> captureType;
        DWORD nativeTypeCount = 0;
        for (DWORD typeIndex = 0;; ++typeIndex) {
            ComPtr<IMFMediaType> mediaType;
            current = reader->GetNativeMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                typeIndex,
                &mediaType);
            if (current == MF_E_NO_MORE_TYPES) {
                break;
            }
            if (FAILED(current)) {
                return current;
            }

            GUID subtype = GUID_NULL;
            UINT32 width = 0;
            UINT32 height = 0;
            UINT32 frameRateNumerator = 0;
            UINT32 frameRateDenominator = 0;
            current = mediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (FAILED(current)) {
                return current;
            }
            current = MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &width, &height);
            if (FAILED(current)) {
                return current;
            }
            current = MFGetAttributeRatio(
                mediaType.Get(),
                MF_MT_FRAME_RATE,
                &frameRateNumerator,
                &frameRateDenominator);
            if (FAILED(current) || frameRateDenominator == 0) {
                return FAILED(current) ? current : E_INVALIDARG;
            }

            for (size_t expectedIndex = 0; expectedIndex < kSupportedModes.size(); ++expectedIndex) {
                const SupportedMode& expected = kSupportedModes[expectedIndex];
                if (IsEqualGUID(subtype, expected.subtype) &&
                    width == expected.width && height == expected.height &&
                    frameRateNumerator == expected.framesPerSecond * frameRateDenominator) {
                    foundModes[expectedIndex] = true;
                    if (width == kSmokeWidth && height == kSmokeHeight &&
                        frameRateNumerator == kSmokeFrameRate * frameRateDenominator &&
                        IsEqualGUID(subtype, MFVideoFormat_NV12)) {
                        captureType = mediaType;
                    }
                }
            }

            ++nativeTypeCount;
        }

        for (size_t index = 0; index < kSupportedModes.size(); ++index) {
            if (!foundModes[index]) {
                wchar_t message[256] = {};
                swprintf_s(message, ARRAYSIZE(message), L"CI_CAMERA_SMOKE_MISSING_MODE %s\n", kSupportedModes[index].name);
                WriteWideText(STD_ERROR_HANDLE, message);
                return MF_E_INVALIDMEDIATYPE;
            }
        }
        if (captureType == nullptr) {
            return MF_E_INVALIDMEDIATYPE;
        }

        for (const SupportedMode& mode : kSupportedModes) {
            wchar_t message[256] = {};
            swprintf_s(message, ARRAYSIZE(message), L"CI_CAMERA_SMOKE_NATIVE_MODE %s\n", mode.name);
            WriteWideText(STD_OUTPUT_HANDLE, message);
        }

        current = reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            nullptr,
            captureType.Get());
        if (FAILED(current)) {
            return current;
        }

        std::array<ULONGLONG, kFrameCaptureCount> frameHashes{};
        LONGLONG previousTimestamp = 0;
        int sampleIndex = 0;
        int attempts = 0;
        while (sampleIndex < kFrameCaptureCount && attempts < kFrameCaptureCount * 4) {
            ++attempts;
            ComPtr<IMFSample> sample;
            DWORD actualStreamIndex = 0;
            DWORD streamFlags = 0;
            LONGLONG timestamp = 0;
            current = reader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                &actualStreamIndex,
                &streamFlags,
                &timestamp,
                &sample);
            if (FAILED(current)) {
                return current;
            }
            if ((streamFlags & MF_SOURCE_READERF_ERROR) != 0 ||
                (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            if (sample == nullptr) {
                continue;
            }
            (void)actualStreamIndex;

            if (sampleIndex > 0) {
                const LONGLONG timestampInterval = timestamp - previousTimestamp;
                const LONGLONG minimumInterval = kSmokeSampleDuration100ns * 3 / 4;
                const LONGLONG maximumInterval = kSmokeSampleDuration100ns * 5 / 4;
                if (timestampInterval < minimumInterval || timestampInterval > maximumInterval) {
                    wchar_t message[256] = {};
                    swprintf_s(
                        message,
                        ARRAYSIZE(message),
                        L"CI_CAMERA_SMOKE_CADENCE_ERROR interval_100ns=%I64d expected_100ns=%I64d\\n",
                        static_cast<long long>(timestampInterval),
                        static_cast<long long>(kSmokeSampleDuration100ns));
                    WriteWideText(STD_ERROR_HANDLE, message);
                    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                }
            }
            previousTimestamp = timestamp;

            LONGLONG sampleDuration = 0;
            current = sample->GetSampleDuration(&sampleDuration);
            if (FAILED(current)) {
                return current;
            }
            if (sampleDuration != kSmokeSampleDuration100ns) {
                return MF_E_INVALIDMEDIATYPE;
            }

            frameHashes[static_cast<size_t>(sampleIndex)] = HashSample(sample.Get(), &current);
            if (FAILED(current)) {
                return current;
            }
            ++sampleIndex;
        }
        if (sampleIndex != kFrameCaptureCount) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }

        bool framesChanged = false;
        for (size_t index = 1; index < frameHashes.size(); ++index) {
            framesChanged = framesChanged || frameHashes[index] != frameHashes[0];
        }
        if (!framesChanged) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        wchar_t summary[320] = {};
        swprintf_s(
            summary,
            ARRAYSIZE(summary),
            L"CI_CAMERA_SMOKE_CAPTURE_OK native_types=%lu mode=NV12 3840x2160@30 samples=%d sample_duration_100ns=%I64d\n",
            static_cast<unsigned long>(nativeTypeCount),
            kFrameCaptureCount,
            static_cast<long long>(kSmokeSampleDuration100ns));
        WriteWideText(STD_OUTPUT_HANDLE, summary);
        return S_OK;
    }();

    if (mediaSource != nullptr) {
        const HRESULT shutdownResult = mediaSource->Shutdown();
        if (SUCCEEDED(result) && FAILED(shutdownResult)) {
            result = shutdownResult;
        }
    }
    return result;
}

HRESULT RunFrameTransportSelfTest() {
    std::wstring userSid;
    HRESULT result = GetCurrentUserSidString(&userSid);
    if (FAILED(result)) {
        return result;
    }

    GUID testGuid{};
    result = CoCreateGuid(&testGuid);
    if (FAILED(result)) {
        return result;
    }
    wchar_t guidText[40] = {};
    if (StringFromGUID2(testGuid, guidText, ARRAYSIZE(guidText)) != 39) {
        return E_FAIL;
    }
    const std::wstring channelName = L"Local\\RemotePhone.FrameTest." + std::wstring(guidText);

    remotephone::frame::SharedNv12FrameBuffer writer;
    result = writer.OpenForSelfTestWriter(channelName, userSid);
    if (result != S_OK) {
        ReportFailure(L"не удалось открыть writer для NV12 self-test", result);
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    remotephone::frame::SharedNv12FrameBuffer reader;
    result = reader.OpenForSelfTest(channelName, userSid);
    if (result != S_OK) {
        ReportFailure(L"не удалось открыть read-only reader для NV12 self-test", result);
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    const std::array<BYTE, 24> firstFrame = {{
        10, 10, 20, 20,
        10, 10, 20, 20,
        30, 30, 40, 40,
        30, 30, 40, 40,
        100, 150, 110, 160,
        120, 170, 130, 180,
    }};
    ULONGLONG firstSequence = 0;
    result = writer.PublishNv12(firstFrame.data(), 4, 4, static_cast<UINT32>(firstFrame.size()), 123456789ULL, &firstSequence);
    if (FAILED(result)) {
        return result;
    }
    if (writer.PublishNv12(firstFrame.data(), 4, 4, static_cast<UINT32>(firstFrame.size() - 1), 123456789ULL) != E_INVALIDARG
        || writer.PublishNv12(firstFrame.data(), 3, 4, static_cast<UINT32>(firstFrame.size()), 123456789ULL) != E_INVALIDARG) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    std::array<BYTE, 12> scaledFrame{};
    scaledFrame.fill(0xCD);
    UINT64 observedTimestamp = 0;
    ULONGLONG observedSequence = 0;
    result = reader.CopyLatestNv12(2, 2, scaledFrame.data(), static_cast<DWORD>(scaledFrame.size()), 4, &observedTimestamp, &observedSequence);
    if (FAILED(result) || result != S_OK
        || observedTimestamp != 123456789ULL || observedSequence != firstSequence
        || scaledFrame[0] != 10 || scaledFrame[1] != 20
        || scaledFrame[2] != 0 || scaledFrame[3] != 0
        || scaledFrame[4] != 30 || scaledFrame[5] != 40
        || scaledFrame[6] != 0 || scaledFrame[7] != 0
        || scaledFrame[8] != 115 || scaledFrame[9] != 165
        || scaledFrame[10] != 0 || scaledFrame[11] != 0) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    std::array<BYTE, 24> newestFrame{};
    newestFrame.fill(200);
    ULONGLONG secondSequence = 0;
    result = writer.PublishNv12(newestFrame.data(), 4, 4, static_cast<UINT32>(newestFrame.size()), 987654321ULL, &secondSequence);
    if (FAILED(result) || secondSequence <= firstSequence) {
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    scaledFrame.fill(0);
    result = reader.CopyLatestNv12(2, 2, scaledFrame.data(), static_cast<DWORD>(scaledFrame.size()), 4, &observedTimestamp, &observedSequence);
    if (FAILED(result) || result != S_OK
        || observedTimestamp != 987654321ULL || observedSequence != secondSequence
        || scaledFrame[0] != 200 || scaledFrame[1] != 200
        || scaledFrame[4] != 200 || scaledFrame[5] != 200
        || scaledFrame[8] != 200 || scaledFrame[9] != 200) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    result = writer.ClearPublishedFrames();
    if (FAILED(result)) {
        return result;
    }
    if (reader.CopyLatestNv12(2, 2, scaledFrame.data(), static_cast<DWORD>(scaledFrame.size()), 4) != S_FALSE) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    remotephone::frame::Nv12FramePipeHeader pipeHeader{};
    pipeHeader.magic = remotephone::frame::kNv12FramePipeMagic;
    pipeHeader.version = 1;
    pipeHeader.headerBytes = sizeof(pipeHeader);
    pipeHeader.width = 4;
    pipeHeader.height = 4;
    pipeHeader.payloadBytes = static_cast<UINT32>(firstFrame.size());
    pipeHeader.timestampNs = 42;
    if (!IsValidFramePipeHeader(pipeHeader)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    pipeHeader.flags = 1;
    if (IsValidFramePipeHeader(pipeHeader)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    wchar_t summary[256] = {};
    swprintf_s(
        summary,
        ARRAYSIZE(summary),
        L"CPU_FRAME_CHANNEL_SELFTEST_PASS version=%u slots=%u max=%ux%u payload_bytes=%u\n",
        remotephone::frame::kFrameChannelVersion,
        remotephone::frame::kFrameSlotCount,
        remotephone::frame::kMaxFrameWidth,
        remotephone::frame::kMaxFrameHeight,
        remotephone::frame::Nv12FrameByteLength(3840, 2160));
    WriteWideText(STD_OUTPUT_HANDLE, summary);
    return S_OK;
}

HRESULT StopRemoveAndShutdownCamera(ComPtr<IMFVirtualCamera>& camera) {
    if (camera == nullptr) {
        return S_OK;
    }

    const HRESULT stopResult = camera->Stop();
    const HRESULT removeResult = camera->Remove();
    const HRESULT shutdownResult = camera->Shutdown();
    camera.Reset();

    if (FAILED(stopResult)) {
        return stopResult;
    }
    if (FAILED(removeResult)) {
        return removeResult;
    }
    return shutdownResult;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    const bool applicationSession = argc == 2 && std::wcscmp(argv[1], L"--application-session") == 0;
    const bool ciSmokeTest = argc == 2 && std::wcscmp(argv[1], L"--ci-smoke") == 0;
    const bool frameTransportSelfTest = argc == 2 && std::wcscmp(argv[1], L"--cpu-frame-ring-self-test") == 0;
    if (!applicationSession && !ciSmokeTest && !frameTransportSelfTest) {
        WriteWideText(
            STD_ERROR_HANDLE,
            L"Недопустимый запуск. Камера управляется приложением «Видоискатель».\n");
        return 2;
    }

    Apartment apartment;
    HRESULT result = apartment.Initialize();
    if (FAILED(result)) {
        ReportFailure(L"не удалось подготовить COM", result);
        return 1;
    }

    if (frameTransportSelfTest) {
        result = RunFrameTransportSelfTest();
        if (FAILED(result)) {
            ReportFailure(L"не прошёл тест NV12 shared memory", result);
            return 1;
        }
        return 0;
    }

    MediaFoundation mediaFoundation;
    result = mediaFoundation.Initialize();
    if (FAILED(result)) {
        ReportFailure(L"не удалось запустить Media Foundation", result);
        return 1;
    }

    const std::wstring diagnosticLogPath = GetDiagnosticLogPath();
    result = EnsureDiagnosticLogWritable(diagnosticLogPath);
    if (FAILED(result)) {
        ReportFailure(L"не удалось открыть журнал в ProgramData", result);
        return 1;
    }

    std::wstring frameChannelName;
    result = CreateFrameChannelName(&frameChannelName);
    if (FAILED(result)) {
        ReportFailure(L"не удалось создать имя временного NV12-канала", result);
        return 1;
    }
    std::wstring frameChannelUserSid;
    result = GetCurrentUserSidString(&frameChannelUserSid);
    if (FAILED(result)) {
        ReportFailure(L"не удалось определить владельца временного NV12-канала", result);
        return 1;
    }

    ComPtr<IMFVirtualCamera> camera;
    result = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        kCameraFriendlyName,
        VIRTUALCAMERAMEDIASOURCE_CLSID,
        nullptr,
        0,
        camera.GetAddressOf());
    if (FAILED(result)) {
        ReportFailure(L"не удалось создать тестовую виртуальную камеру", result);
        return 1;
    }

    result = camera->SetUINT32(VCAM_KIND, static_cast<UINT32>(VirtualCameraKind::Synthetic));
    if (FAILED(result)) {
        ReportFailure(L"не удалось выбрать синтетический источник", result);
        camera->Shutdown();
        return 1;
    }
    result = camera->SetString(VCAM_DIAGNOSTIC_LOG_PATH, diagnosticLogPath.c_str());
    if (FAILED(result)) {
        ReportFailure(L"не удалось передать журнал службе камеры", result);
        camera->Shutdown();
        return 1;
    }
    result = camera->SetString(VCAM_FRAME_CHANNEL_NAME, frameChannelName.c_str());
    if (FAILED(result)) {
        ReportFailure(L"не удалось передать имя временного NV12-канала", result);
        camera->Shutdown();
        return 1;
    }
    result = camera->SetString(VCAM_FRAME_CHANNEL_USER_SID, frameChannelUserSid.c_str());
    if (FAILED(result)) {
        ReportFailure(L"не удалось передать SID временного NV12-канала", result);
        camera->Shutdown();
        return 1;
    }

    result = camera->Start(nullptr);
    if (FAILED(result)) {
        wchar_t smokeFailure[128] = {};
        swprintf_s(
            smokeFailure,
            ARRAYSIZE(smokeFailure),
            L"CI_CAMERA_START_FAILED hresult=0x%08lX\n",
            static_cast<unsigned long>(result));
        WriteWideText(STD_ERROR_HANDLE, smokeFailure);
        ReportFailure(L"не удалось запустить тестовую виртуальную камеру", result);
        camera->Remove();
        camera->Shutdown();
        return 1;
    }

    int exitCode = 0;
    if (ciSmokeTest) {
        result = RunMediaFoundationCaptureSmokeTest();
        if (FAILED(result)) {
            ReportFailure(L"Media Foundation не перечислил режимы или не захватил меняющиеся кадры", result);
            exitCode = 1;
        }
    } else {
        WriteWideText(STD_OUTPUT_HANDLE, L"CAMERA_STATUS=RUNNING\n");
        WriteWideText(
            STD_OUTPUT_HANDLE,
            L"Тестовая камера запущена в приложении «Видоискатель». Оставьте приложение открытым, пока проверяете браузер, OBS и Discord.\n");
        WriteWideText(STD_OUTPUT_HANDLE, L"CPU_FRAME_PIPE=READY\n");
        // Electron closes this pipe on application shutdown; EOF then releases the session camera.
        result = RunFramePipe(GetStdHandle(STD_INPUT_HANDLE), frameChannelName);
        if (FAILED(result)) {
            ReportFailure(L"ошибка входящего NV12 frame pipe", result);
            exitCode = 1;
        }
    }

    const HRESULT cleanupResult = StopRemoveAndShutdownCamera(camera);
    if (FAILED(cleanupResult)) {
        ReportFailure(L"не удалось штатно остановить и убрать тестовую камеру", cleanupResult);
        exitCode = 1;
    }

    if (ciSmokeTest && exitCode == 0) {
        WriteWideText(STD_OUTPUT_HANDLE, L"CI_CAMERA_SMOKE_PASS\n");
    } else if (applicationSession) {
        WriteWideText(STD_OUTPUT_HANDLE, L"CAMERA_STATUS=STOPPED\n");
    }
    return exitCode;
}
