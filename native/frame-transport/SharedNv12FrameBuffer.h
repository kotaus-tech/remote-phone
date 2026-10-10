#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace remotephone {
namespace frame {

constexpr UINT32 kFrameChannelVersion = 1;
// Any orientation inside the 4K pixel budget: portrait phone screens are
// taller than 2160 scan lines (e.g. 1080×2374), so the per-side cap is 4096
// and IsValidNv12FrameDimensions enforces width*height <= kMaxFramePixels.
// The slot capacity stays 4K-sized (kMaxFrameBytes is unchanged).
constexpr UINT32 kMaxFrameWidth = 4096;
constexpr UINT32 kMaxFrameHeight = 4096;
constexpr ULONGLONG kMaxFramePixels = 3840ULL * 2160ULL;
constexpr UINT32 kFrameSlotCount = 3;
constexpr UINT32 kPixelFormatNv12 = 1;
constexpr ULONGLONG kFrameStaleAfterMs = 3000;
constexpr wchar_t kGlobalFrameChannelPrefix[] = L"Global\\Kotaus.RemotePhone.Frame.";

// Binary stdin protocol from the desktop process to RemotePhone.VirtualCameraHost.exe.
// All integer fields are little-endian (the Windows host and Electron are both little-endian).
#pragma pack(push, 1)
struct Nv12FramePipeHeader final {
    UINT32 magic;             // ASCII "RPF1" as a little-endian UINT32.
    UINT16 version;           // 1.
    UINT16 headerBytes;        // sizeof(Nv12FramePipeHeader), currently 32.
    UINT32 width;
    UINT32 height;
    UINT32 payloadBytes;       // tightly packed NV12: width * height * 3 / 2.
    UINT32 flags;              // must be zero.
    UINT64 timestampNs;        // WebCodecs VideoFrame timestamp converted to nanoseconds.
};
#pragma pack(pop)
static_assert(sizeof(Nv12FramePipeHeader) == 32, "Unexpected NV12 frame-pipe header size");
constexpr UINT32 kNv12FramePipeMagic = 0x31465052; // "RPF1".

class SharedNv12FrameBuffer final {
public:
    SharedNv12FrameBuffer() = default;
    ~SharedNv12FrameBuffer();

    SharedNv12FrameBuffer(const SharedNv12FrameBuffer&) = delete;
    SharedNv12FrameBuffer& operator=(const SharedNv12FrameBuffer&) = delete;

    // The media source lazily creates the pagefile-backed Global mapping when running in
    // Frame Server (LocalService, session 0), or opens the existing mapping read-only.
    // S_FALSE means that the channel is not available in this process yet; the camera may
    // continue serving its synthetic fallback and retry on the next sample request.
    HRESULT OpenForMediaSource(const std::wstring& channelName, const std::wstring& userSid);

    // The application-session host never creates the Global mapping; it only opens the
    // channel created by the Media Foundation source and maps it for writing.
    HRESULT OpenForWriter(const std::wstring& channelName);

    // Publish one tightly packed NV12 frame. The writer drops old frames rather than
    // blocking the capture/decode thread. Returns E_HANDLE if the writer has not connected.
    HRESULT PublishNv12(
        const BYTE* bytes,
        UINT32 width,
        UINT32 height,
        UINT32 byteLength,
        UINT64 timestampNs,
        ULONGLONG* publishedSequence = nullptr);

    // Wipe every shared slot when the producer disconnects so camera pixels do not remain
    // in the pagefile-backed mapping after the application session ends.
    HRESULT ClearPublishedFrames();

    // Copy the newest fresh NV12 frame into a Media Foundation NV12 buffer. The destination
    // may have padded rows; this method copies only visible pixels and clears row padding.
    // It area-averages when scaling down to the requested camera mode and uses nearest
    // sampling when scaling up. S_FALSE means there is no usable live frame yet.
    HRESULT CopyLatestNv12(
        UINT32 outputWidth,
        UINT32 outputHeight,
        BYTE* destination,
        DWORD destinationLength,
        LONG destinationPitch,
        UINT64* sourceTimestampNs = nullptr,
        ULONGLONG* sourceSequence = nullptr);

    void Reset() noexcept;
    bool IsOpen() const noexcept { return m_view != nullptr; }

#ifdef REMOTE_PHONE_FRAME_TRANSPORT_TESTING
    // Test-only path: exercises the same mapping layout in the caller's Local namespace,
    // where a normal test process does not need SeCreateGlobalPrivilege.
    HRESULT OpenForSelfTest(const std::wstring& localName, const std::wstring& userSid);
    HRESULT OpenForSelfTestWriter(const std::wstring& localName, const std::wstring& userSid);
#endif

private:
    enum class AccessMode : UINT32 { None, Reader, Writer };

    HRESULT TryOpenMediaSource();
    HRESULT TryOpenWriter();
    HRESULT OpenExisting(AccessMode mode);
    HRESULT CreateForMediaSource(bool allowLocalNamespace);
    HRESULT MapAndValidate(HANDLE mapping, AccessMode mode);
    HRESULT InitializeNewSection(HANDLE mapping);
    bool HasValidSection() const noexcept;

    HANDLE m_mapping = nullptr;
    void* m_view = nullptr;
    AccessMode m_accessMode = AccessMode::None;
    std::wstring m_channelName;
    std::wstring m_userSid;
    bool m_mediaSourceMode = false;
    bool m_mayCreateGlobalMapping = true;
};

bool IsValidGlobalFrameChannelName(const std::wstring& value) noexcept;
bool IsValidNv12FrameDimensions(UINT32 width, UINT32 height) noexcept;
UINT32 Nv12FrameByteLength(UINT32 width, UINT32 height) noexcept;

} // namespace frame
} // namespace remotephone
