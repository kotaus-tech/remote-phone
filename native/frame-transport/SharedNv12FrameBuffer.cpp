#include "SharedNv12FrameBuffer.h"

#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <limits>

namespace remotephone {
namespace frame {
namespace {

constexpr ULONGLONG kMappingMagic = 0x3152464E565052ULL; // "RPNVFR1".
constexpr LONG kInitializationEmpty = 0;
constexpr LONG kInitializationInProgress = 1;
constexpr LONG kInitializationReady = 2;
constexpr UINT32 kSectionHeaderBytes = 64;
constexpr UINT32 kSlotHeaderBytes = 64;
// Slot capacity stays pinned to the 4K pixel budget (mapping size unchanged);
// portrait frames fit because per-side limits are independent of the budget.
constexpr ULONGLONG kMaxFrameBytes = kMaxFramePixels * 3ULL / 2ULL;

struct alignas(64) MappingHeader final {
    ULONGLONG magic;
    UINT32 version;
    UINT32 headerBytes;
    ULONGLONG sectionBytes;
    UINT32 slotCount;
    UINT32 maxWidth;
    UINT32 maxHeight;
    UINT32 maxFrameBytes;
    volatile LONG initializationState;
    volatile LONG publishedSlot;
    alignas(8) volatile LONG64 nextSequence;
    UINT8 reserved[8];
};
static_assert(sizeof(MappingHeader) == kSectionHeaderBytes, "Unexpected frame mapping header size");

struct alignas(64) FrameSlotHeader final {
    alignas(8) volatile LONG64 sequenceLock;
    ULONGLONG timestampNs;
    ULONGLONG publishedAtTickMs;
    UINT32 width;
    UINT32 height;
    UINT32 yStride;
    UINT32 uvStride;
    UINT32 byteLength;
    UINT32 pixelFormat;
    UINT8 reserved[16];
};
static_assert(sizeof(FrameSlotHeader) == kSlotHeaderBytes, "Unexpected NV12 slot header size");

constexpr ULONGLONG AlignTo64(ULONGLONG value) noexcept {
    return (value + 63ULL) & ~63ULL;
}
constexpr ULONGLONG kSlotStride = AlignTo64(kSlotHeaderBytes + kMaxFrameBytes);
constexpr ULONGLONG kMappingBytes = kSectionHeaderBytes + kFrameSlotCount * kSlotStride;
static_assert(kMappingBytes <= std::numeric_limits<DWORD>::max(), "Frame section is too large for CreateFileMappingW");

LONG LoadLong(const volatile LONG* value) noexcept {
    MemoryBarrier();
    const LONG loaded = *value;
    MemoryBarrier();
    return loaded;
}

LONG64 LoadLong64(const volatile LONG64* value) noexcept {
    MemoryBarrier();
    const LONG64 loaded = *value;
    MemoryBarrier();
    return loaded;
}

MappingHeader* Header(void* view) noexcept {
    return static_cast<MappingHeader*>(view);
}
const MappingHeader* Header(const void* view) noexcept {
    return static_cast<const MappingHeader*>(view);
}

FrameSlotHeader* SlotHeaderAt(void* view, LONG slotIndex) noexcept {
    auto* base = static_cast<BYTE*>(view);
    return reinterpret_cast<FrameSlotHeader*>(base + kSectionHeaderBytes + static_cast<ULONGLONG>(slotIndex) * kSlotStride);
}
const FrameSlotHeader* SlotHeaderAt(const void* view, LONG slotIndex) noexcept {
    const auto* base = static_cast<const BYTE*>(view);
    return reinterpret_cast<const FrameSlotHeader*>(base + kSectionHeaderBytes + static_cast<ULONGLONG>(slotIndex) * kSlotStride);
}

BYTE* SlotBytesAt(void* view, LONG slotIndex) noexcept {
    return reinterpret_cast<BYTE*>(SlotHeaderAt(view, slotIndex)) + kSlotHeaderBytes;
}
const BYTE* SlotBytesAt(const void* view, LONG slotIndex) noexcept {
    return reinterpret_cast<const BYTE*>(SlotHeaderAt(view, slotIndex)) + kSlotHeaderBytes;
}

bool IsHexDigit(wchar_t character) noexcept {
    return (character >= L'0' && character <= L'9')
        || (character >= L'a' && character <= L'f')
        || (character >= L'A' && character <= L'F');
}

bool IsGuidText(const std::wstring& value, size_t offset = 0) noexcept {
    if (offset > value.size() || value.size() - offset != 38
        || value[offset] != L'{' || value[offset + 37] != L'}') {
        return false;
    }
    for (size_t index = 1; index < 37; ++index) {
        const bool separator = index == 9 || index == 14 || index == 19 || index == 24;
        if (separator ? value[offset + index] != L'-' : !IsHexDigit(value[offset + index])) {
            return false;
        }
    }
    return true;
}

bool IsValidSidText(const std::wstring& value) noexcept {
    if (value.empty() || value.size() > 184) {
        return false;
    }
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(value.c_str(), &sid) || sid == nullptr) {
        return false;
    }
    const bool valid = IsValidSid(sid) != FALSE;
    LocalFree(sid);
    return valid;
}

bool IsUnavailableMappingError(DWORD error) noexcept {
    return error == ERROR_ACCESS_DENIED
        || error == ERROR_FILE_NOT_FOUND
        || error == ERROR_PATH_NOT_FOUND
        || error == ERROR_PRIVILEGE_NOT_HELD;
}

HRESULT MakeSecurityAttributes(const std::wstring& userSid, PSECURITY_DESCRIPTOR* descriptor, SECURITY_ATTRIBUTES* attributes) {
    if (descriptor == nullptr || attributes == nullptr || !IsValidSidText(userSid)) {
        return E_INVALIDARG;
    }
    *descriptor = nullptr;

    // The unique per-camera channel grants access only to the owning Windows user,
    // LocalService (Media Foundation Frame Server), and SYSTEM. No raw frames are written to disk.
    const std::wstring sddl =
        L"D:P(A;;GA;;;SY)(A;;GRGW;;;LS)(A;;GRGW;;;" + userSid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            descriptor,
            nullptr)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    attributes->nLength = sizeof(*attributes);
    attributes->lpSecurityDescriptor = *descriptor;
    attributes->bInheritHandle = FALSE;
    return S_OK;
}

HRESULT MapError(DWORD error) noexcept {
    return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
}

void ScalePlaneAreaAverage(
    const BYTE* source,
    UINT32 sourceWidth,
    UINT32 sourceHeight,
    UINT32 sourceStride,
    BYTE* destination,
    UINT32 destinationWidth,
    UINT32 destinationHeight,
    UINT32 destinationStride) noexcept {
    if (sourceWidth == destinationWidth && sourceHeight == destinationHeight) {
        for (UINT32 row = 0; row < destinationHeight; ++row) {
            BYTE* outputRow = destination + static_cast<size_t>(row) * destinationStride;
            std::memcpy(outputRow, source + static_cast<size_t>(row) * sourceStride, destinationWidth);
            if (destinationStride > destinationWidth) {
                std::memset(outputRow + destinationWidth, 0, destinationStride - destinationWidth);
            }
        }
        return;
    }

    for (UINT32 outputY = 0; outputY < destinationHeight; ++outputY) {
        const UINT32 sourceY0 = static_cast<UINT32>((static_cast<ULONGLONG>(outputY) * sourceHeight) / destinationHeight);
        const UINT32 sourceY1 = std::max(sourceY0 + 1,
            static_cast<UINT32>((static_cast<ULONGLONG>(outputY + 1) * sourceHeight) / destinationHeight));
        BYTE* outputRow = destination + static_cast<size_t>(outputY) * destinationStride;
        for (UINT32 outputX = 0; outputX < destinationWidth; ++outputX) {
            const UINT32 sourceX0 = static_cast<UINT32>((static_cast<ULONGLONG>(outputX) * sourceWidth) / destinationWidth);
            const UINT32 sourceX1 = std::max(sourceX0 + 1,
                static_cast<UINT32>((static_cast<ULONGLONG>(outputX + 1) * sourceWidth) / destinationWidth));
            ULONGLONG total = 0;
            UINT32 samples = 0;
            for (UINT32 sourceY = sourceY0; sourceY < sourceY1; ++sourceY) {
                const BYTE* inputRow = source + static_cast<size_t>(sourceY) * sourceStride;
                for (UINT32 sourceX = sourceX0; sourceX < sourceX1; ++sourceX) {
                    total += inputRow[sourceX];
                    ++samples;
                }
            }
            outputRow[outputX] = samples == 0 ? 0 : static_cast<BYTE>((total + samples / 2) / samples);
        }
        if (destinationStride > destinationWidth) {
            std::memset(outputRow + destinationWidth, 0, destinationStride - destinationWidth);
        }
    }
}

void ScaleNv12ChromaAreaAverage(
    const BYTE* source,
    UINT32 sourceWidth,
    UINT32 sourceHeight,
    BYTE* destination,
    UINT32 destinationWidth,
    UINT32 destinationHeight,
    UINT32 destinationStride) noexcept {
    const UINT32 sourcePairs = sourceWidth / 2;
    const UINT32 sourceRows = sourceHeight / 2;
    const UINT32 destinationPairs = destinationWidth / 2;
    const UINT32 destinationRows = destinationHeight / 2;

    for (UINT32 outputY = 0; outputY < destinationRows; ++outputY) {
        const UINT32 sourceY0 = static_cast<UINT32>((static_cast<ULONGLONG>(outputY) * sourceRows) / destinationRows);
        const UINT32 sourceY1 = std::max(sourceY0 + 1,
            static_cast<UINT32>((static_cast<ULONGLONG>(outputY + 1) * sourceRows) / destinationRows));
        BYTE* outputRow = destination + static_cast<size_t>(outputY) * destinationStride;
        for (UINT32 outputPair = 0; outputPair < destinationPairs; ++outputPair) {
            const UINT32 sourceX0 = static_cast<UINT32>((static_cast<ULONGLONG>(outputPair) * sourcePairs) / destinationPairs);
            const UINT32 sourceX1 = std::max(sourceX0 + 1,
                static_cast<UINT32>((static_cast<ULONGLONG>(outputPair + 1) * sourcePairs) / destinationPairs));
            ULONGLONG totalU = 0;
            ULONGLONG totalV = 0;
            UINT32 samples = 0;
            for (UINT32 sourceY = sourceY0; sourceY < sourceY1; ++sourceY) {
                const BYTE* inputRow = source + static_cast<size_t>(sourceY) * sourceWidth;
                for (UINT32 sourceX = sourceX0; sourceX < sourceX1; ++sourceX) {
                    totalU += inputRow[sourceX * 2];
                    totalV += inputRow[sourceX * 2 + 1];
                    ++samples;
                }
            }
            outputRow[outputPair * 2] = samples == 0 ? 128 : static_cast<BYTE>((totalU + samples / 2) / samples);
            outputRow[outputPair * 2 + 1] = samples == 0 ? 128 : static_cast<BYTE>((totalV + samples / 2) / samples);
        }
        if (destinationStride > destinationWidth) {
            std::memset(outputRow + destinationWidth, 0, destinationStride - destinationWidth);
        }
    }
}

} // namespace

bool IsValidGlobalFrameChannelName(const std::wstring& value) noexcept {
    const size_t prefixLength = std::wcslen(kGlobalFrameChannelPrefix);
    if (value.size() != prefixLength + 38 || value.compare(0, prefixLength, kGlobalFrameChannelPrefix) != 0) {
        return false;
    }
    return IsGuidText(value, prefixLength);
}

bool IsValidNv12FrameDimensions(UINT32 width, UINT32 height) noexcept {
    return width >= 2 && height >= 2
        && width <= kMaxFrameWidth && height <= kMaxFrameHeight
        && (width & 1U) == 0 && (height & 1U) == 0
        && static_cast<ULONGLONG>(width) * height <= kMaxFramePixels;
}

UINT32 Nv12FrameByteLength(UINT32 width, UINT32 height) noexcept {
    if (!IsValidNv12FrameDimensions(width, height)) {
        return 0;
    }
    const ULONGLONG bytes = static_cast<ULONGLONG>(width) * height * 3ULL / 2ULL;
    return bytes <= std::numeric_limits<UINT32>::max() ? static_cast<UINT32>(bytes) : 0;
}

SharedNv12FrameBuffer::~SharedNv12FrameBuffer() {
    if (m_accessMode == AccessMode::Writer) {
        (void)ClearPublishedFrames();
    }
    Reset();
}

HRESULT SharedNv12FrameBuffer::OpenForMediaSource(const std::wstring& channelName, const std::wstring& userSid) {
    if (!IsValidGlobalFrameChannelName(channelName) || !IsValidSidText(userSid)) {
        return E_INVALIDARG;
    }
    if (m_view != nullptr) {
        return m_accessMode == AccessMode::Reader && m_channelName == channelName ? S_OK : E_UNEXPECTED;
    }
    m_channelName = channelName;
    m_userSid = userSid;
    m_mediaSourceMode = true;
    return TryOpenMediaSource();
}

HRESULT SharedNv12FrameBuffer::OpenForWriter(const std::wstring& channelName) {
    if (!IsValidGlobalFrameChannelName(channelName)) {
        return E_INVALIDARG;
    }
    if (m_view != nullptr) {
        return m_accessMode == AccessMode::Writer && m_channelName == channelName ? S_OK : E_UNEXPECTED;
    }
    m_channelName = channelName;
    m_mediaSourceMode = false;
    return TryOpenWriter();
}

#ifdef REMOTE_PHONE_FRAME_TRANSPORT_TESTING
HRESULT SharedNv12FrameBuffer::OpenForSelfTest(const std::wstring& localName, const std::wstring& userSid) {
    constexpr wchar_t prefix[] = L"Local\\RemotePhone.FrameTest.";
    constexpr size_t prefixLength = ARRAYSIZE(prefix) - 1;
    if (localName.compare(0, prefixLength, prefix) != 0
        || !IsGuidText(localName, prefixLength)
        || !IsValidSidText(userSid)) {
        return E_INVALIDARG;
    }
    if (m_view != nullptr) {
        return m_accessMode == AccessMode::Reader && m_channelName == localName ? S_OK : E_UNEXPECTED;
    }
    m_channelName = localName;
    m_userSid = userSid;
    m_mediaSourceMode = true;
    return OpenExisting(AccessMode::Reader);
}

HRESULT SharedNv12FrameBuffer::OpenForSelfTestWriter(const std::wstring& localName, const std::wstring& userSid) {
    constexpr wchar_t prefix[] = L"Local\\RemotePhone.FrameTest.";
    constexpr size_t prefixLength = ARRAYSIZE(prefix) - 1;
    if (localName.compare(0, prefixLength, prefix) != 0
        || !IsGuidText(localName, prefixLength)
        || !IsValidSidText(userSid)) {
        return E_INVALIDARG;
    }
    if (m_view != nullptr) {
        return m_accessMode == AccessMode::Writer && m_channelName == localName ? S_OK : E_UNEXPECTED;
    }

    m_channelName = localName;
    m_userSid = userSid;
    m_mediaSourceMode = true;

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    HRESULT result = MakeSecurityAttributes(m_userSid, &descriptor, &attributes);
    if (FAILED(result)) {
        return result;
    }

    SetLastError(ERROR_SUCCESS);
    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        &attributes,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(kMappingBytes),
        m_channelName.c_str());
    const DWORD createError = GetLastError();
    LocalFree(descriptor);
    if (mapping == nullptr) {
        return HRESULT_FROM_WIN32(createError == ERROR_SUCCESS ? ERROR_INVALID_HANDLE : createError);
    }
    if (createError != ERROR_ALREADY_EXISTS) {
        result = InitializeNewSection(mapping);
        if (FAILED(result)) {
            CloseHandle(mapping);
            return result;
        }
    }
    return MapAndValidate(mapping, AccessMode::Writer);
}
#endif

HRESULT SharedNv12FrameBuffer::TryOpenMediaSource() {
    if (m_view != nullptr) {
        return m_accessMode == AccessMode::Reader ? S_OK : E_UNEXPECTED;
    }

    const HRESULT openResult = OpenExisting(AccessMode::Reader);
    if (openResult == S_OK) {
        return S_OK;
    }
    const DWORD openError = HRESULT_FACILITY(openResult) == FACILITY_WIN32
        ? HRESULT_CODE(openResult)
        : ERROR_SUCCESS;
    if (openError == ERROR_ACCESS_DENIED) {
        return S_FALSE;
    }
    if (openError != ERROR_FILE_NOT_FOUND && openResult != S_FALSE) {
        return openResult;
    }
    if (!m_mayCreateGlobalMapping) {
        return S_FALSE;
    }
    return CreateForMediaSource(false);
}

HRESULT SharedNv12FrameBuffer::TryOpenWriter() {
    if (m_view != nullptr) {
        return m_accessMode == AccessMode::Writer ? S_OK : E_UNEXPECTED;
    }
    const HRESULT openResult = OpenExisting(AccessMode::Writer);
    if (openResult == S_OK) {
        return S_OK;
    }
    const DWORD openError = HRESULT_FACILITY(openResult) == FACILITY_WIN32
        ? HRESULT_CODE(openResult)
        : ERROR_SUCCESS;
    if (IsUnavailableMappingError(openError)) {
        return S_FALSE;
    }
    return openResult;
}

HRESULT SharedNv12FrameBuffer::OpenExisting(AccessMode mode) {
    if (m_channelName.empty()) {
        return E_INVALIDARG;
    }
    const DWORD access = mode == AccessMode::Writer
        ? (FILE_MAP_READ | FILE_MAP_WRITE)
        : FILE_MAP_READ;
    HANDLE mapping = OpenFileMappingW(access, FALSE, m_channelName.c_str());
    if (mapping == nullptr) {
        return MapError(GetLastError());
    }
    return MapAndValidate(mapping, mode);
}

HRESULT SharedNv12FrameBuffer::CreateForMediaSource(bool allowLocalNamespace) {
    if (m_channelName.empty() || !IsValidSidText(m_userSid)) {
        return E_INVALIDARG;
    }
    if (!allowLocalNamespace && !IsValidGlobalFrameChannelName(m_channelName)) {
        return E_INVALIDARG;
    }

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    HRESULT result = MakeSecurityAttributes(m_userSid, &descriptor, &attributes);
    if (FAILED(result)) {
        return result;
    }

    SetLastError(ERROR_SUCCESS);
    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        &attributes,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(kMappingBytes),
        m_channelName.c_str());
    const DWORD createError = GetLastError();
    LocalFree(descriptor);

    if (mapping == nullptr) {
        if (!allowLocalNamespace
            && (createError == ERROR_ACCESS_DENIED || createError == ERROR_PRIVILEGE_NOT_HELD)) {
            m_mayCreateGlobalMapping = false;
        }
        return IsUnavailableMappingError(createError) ? S_FALSE : HRESULT_FROM_WIN32(createError);
    }
    if (createError == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        const HRESULT openResult = OpenExisting(AccessMode::Reader);
        if (openResult == S_OK) {
            return S_OK;
        }
        const DWORD openError = HRESULT_FACILITY(openResult) == FACILITY_WIN32
            ? HRESULT_CODE(openResult)
            : ERROR_SUCCESS;
        return IsUnavailableMappingError(openError) ? S_FALSE : openResult;
    }

    result = InitializeNewSection(mapping);
    if (FAILED(result)) {
        CloseHandle(mapping);
        return result;
    }

    // Reopen with read-only rights so the media source never writes camera frames or
    // per-frame metadata after creating and initializing the section.
    CloseHandle(mapping);
    return OpenExisting(AccessMode::Reader);
}

HRESULT SharedNv12FrameBuffer::InitializeNewSection(HANDLE mapping) {
    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(kMappingBytes));
    if (view == nullptr) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    auto* header = Header(view);
    if (InterlockedCompareExchange(&header->initializationState, kInitializationInProgress, kInitializationEmpty)
        == kInitializationEmpty) {
        header->magic = kMappingMagic;
        header->version = kFrameChannelVersion;
        header->headerBytes = kSectionHeaderBytes;
        header->sectionBytes = kMappingBytes;
        header->slotCount = kFrameSlotCount;
        header->maxWidth = kMaxFrameWidth;
        header->maxHeight = kMaxFrameHeight;
        header->maxFrameBytes = static_cast<UINT32>(kMaxFrameBytes);
        header->publishedSlot = -1;
        header->nextSequence = 0;
        MemoryBarrier();
        InterlockedExchange(&header->initializationState, kInitializationReady);
    } else {
        for (UINT32 attempt = 0; attempt < 100; ++attempt) {
            const LONG state = LoadLong(&header->initializationState);
            if (state == kInitializationReady) {
                break;
            }
            if (state != kInitializationInProgress) {
                UnmapViewOfFile(view);
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            Sleep(1);
        }
    }

    const bool valid = LoadLong(&Header(view)->initializationState) == kInitializationReady
        && Header(view)->magic == kMappingMagic
        && Header(view)->version == kFrameChannelVersion
        && Header(view)->headerBytes == kSectionHeaderBytes
        && Header(view)->sectionBytes == kMappingBytes
        && Header(view)->slotCount == kFrameSlotCount
        && Header(view)->maxFrameBytes == static_cast<UINT32>(kMaxFrameBytes);
    UnmapViewOfFile(view);
    return valid ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
}

HRESULT SharedNv12FrameBuffer::MapAndValidate(HANDLE mapping, AccessMode mode) {
    const DWORD access = mode == AccessMode::Writer
        ? (FILE_MAP_READ | FILE_MAP_WRITE)
        : FILE_MAP_READ;
    void* view = MapViewOfFile(mapping, access, 0, 0, static_cast<SIZE_T>(kMappingBytes));
    if (view == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(mapping);
        return IsUnavailableMappingError(error) ? S_FALSE : HRESULT_FROM_WIN32(error);
    }

    const MappingHeader* header = Header(view);
    const LONG state = LoadLong(&header->initializationState);
    if (state != kInitializationReady) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return S_FALSE;
    }
    if (header->magic != kMappingMagic
        || header->version != kFrameChannelVersion
        || header->headerBytes != kSectionHeaderBytes
        || header->sectionBytes != kMappingBytes
        || header->slotCount != kFrameSlotCount
        || header->maxWidth != kMaxFrameWidth
        || header->maxHeight != kMaxFrameHeight
        || header->maxFrameBytes != static_cast<UINT32>(kMaxFrameBytes)) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    m_mapping = mapping;
    m_view = view;
    m_accessMode = mode;
    return S_OK;
}

bool SharedNv12FrameBuffer::HasValidSection() const noexcept {
    if (m_view == nullptr) {
        return false;
    }
    const MappingHeader* header = Header(m_view);
    return header->magic == kMappingMagic
        && header->version == kFrameChannelVersion
        && header->headerBytes == kSectionHeaderBytes
        && header->sectionBytes == kMappingBytes
        && header->slotCount == kFrameSlotCount
        && header->maxWidth == kMaxFrameWidth
        && header->maxHeight == kMaxFrameHeight
        && header->maxFrameBytes == static_cast<UINT32>(kMaxFrameBytes)
        && LoadLong(&header->initializationState) == kInitializationReady;
}

HRESULT SharedNv12FrameBuffer::PublishNv12(
    const BYTE* bytes,
    UINT32 width,
    UINT32 height,
    UINT32 byteLength,
    UINT64 timestampNs,
    ULONGLONG* publishedSequence) {
    if (m_view == nullptr || m_accessMode != AccessMode::Writer || !HasValidSection()) {
        return E_HANDLE;
    }
    const UINT32 expectedBytes = Nv12FrameByteLength(width, height);
    if (bytes == nullptr || expectedBytes == 0 || byteLength != expectedBytes) {
        return E_INVALIDARG;
    }

    auto* header = Header(m_view);
    const LONG previousSlot = InterlockedCompareExchange(&header->publishedSlot, 0, 0);
    const LONG slotIndex = previousSlot < 0
        ? 0
        : static_cast<LONG>((previousSlot + 1) % static_cast<LONG>(kFrameSlotCount));
    auto* slot = SlotHeaderAt(m_view, slotIndex);

    const LONG64 sequence = InterlockedAdd64(&header->nextSequence, 2);
    if (sequence <= 0 || (sequence & 1) != 0) {
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    }

    InterlockedExchange64(&slot->sequenceLock, sequence | 1);
    slot->timestampNs = timestampNs;
    slot->publishedAtTickMs = GetTickCount64();
    slot->width = width;
    slot->height = height;
    slot->yStride = width;
    slot->uvStride = width;
    slot->byteLength = byteLength;
    slot->pixelFormat = kPixelFormatNv12;
    std::memcpy(SlotBytesAt(m_view, slotIndex), bytes, byteLength);
    MemoryBarrier();
    InterlockedExchange64(&slot->sequenceLock, sequence);
    InterlockedExchange(&header->publishedSlot, slotIndex);

    if (publishedSequence != nullptr) {
        *publishedSequence = static_cast<ULONGLONG>(sequence);
    }
    return S_OK;
}

HRESULT SharedNv12FrameBuffer::ClearPublishedFrames() {
    if (m_view == nullptr || m_accessMode != AccessMode::Writer || !HasValidSection()) {
        return E_HANDLE;
    }

    auto* header = Header(m_view);
    InterlockedExchange(&header->publishedSlot, -1);
    for (LONG slotIndex = 0; slotIndex < static_cast<LONG>(kFrameSlotCount); ++slotIndex) {
        auto* slot = SlotHeaderAt(m_view, slotIndex);
        const LONG64 currentSequence = InterlockedCompareExchange64(&slot->sequenceLock, 0, 0);
        InterlockedExchange64(&slot->sequenceLock, currentSequence | 1);
        SecureZeroMemory(SlotBytesAt(m_view, slotIndex), static_cast<SIZE_T>(kMaxFrameBytes));
        slot->timestampNs = 0;
        slot->publishedAtTickMs = 0;
        slot->width = 0;
        slot->height = 0;
        slot->yStride = 0;
        slot->uvStride = 0;
        slot->byteLength = 0;
        slot->pixelFormat = 0;
        MemoryBarrier();
        InterlockedExchange64(&slot->sequenceLock, 0);
    }
    return S_OK;
}

HRESULT SharedNv12FrameBuffer::CopyLatestNv12(
    UINT32 outputWidth,
    UINT32 outputHeight,
    BYTE* destination,
    DWORD destinationLength,
    LONG destinationPitch,
    UINT64* sourceTimestampNs,
    ULONGLONG* sourceSequence) {
    if (m_view == nullptr && m_mediaSourceMode) {
        const HRESULT openResult = TryOpenMediaSource();
        if (openResult != S_OK) {
            return openResult;
        }
    }
    if (m_view == nullptr || m_accessMode != AccessMode::Reader || !HasValidSection()) {
        return S_FALSE;
    }
    if (destination == nullptr || !IsValidNv12FrameDimensions(outputWidth, outputHeight)
        || destinationPitch <= 0 || (destinationPitch & 1) != 0
        || static_cast<UINT32>(destinationPitch) < outputWidth) {
        return E_INVALIDARG;
    }

    const ULONGLONG requiredBytes = static_cast<ULONGLONG>(destinationPitch) * outputHeight * 3ULL / 2ULL;
    if (requiredBytes > destinationLength) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    auto* mappingHeader = Header(m_view);
    for (UINT32 attempt = 0; attempt < 4; ++attempt) {
        const LONG slotIndex = LoadLong(&mappingHeader->publishedSlot);
        if (slotIndex < 0 || slotIndex >= static_cast<LONG>(kFrameSlotCount)) {
            return S_FALSE;
        }

        const FrameSlotHeader* slot = SlotHeaderAt(m_view, slotIndex);
        const LONG64 sequenceBefore = LoadLong64(&slot->sequenceLock);
        if (sequenceBefore <= 0 || (sequenceBefore & 1) != 0) {
            continue;
        }
        MemoryBarrier();

        const UINT32 sourceWidth = slot->width;
        const UINT32 sourceHeight = slot->height;
        const UINT32 sourceYStride = slot->yStride;
        const UINT32 sourceUvStride = slot->uvStride;
        const UINT32 sourceByteLength = slot->byteLength;
        const UINT32 pixelFormat = slot->pixelFormat;
        const UINT64 timestampNs = slot->timestampNs;
        const ULONGLONG publishedAtTickMs = slot->publishedAtTickMs;
        const UINT32 expectedSourceBytes = Nv12FrameByteLength(sourceWidth, sourceHeight);
        if (pixelFormat != kPixelFormatNv12 || expectedSourceBytes == 0
            || sourceYStride != sourceWidth || sourceUvStride != sourceWidth
            || sourceByteLength != expectedSourceBytes) {
            continue;
        }

        const ULONGLONG now = GetTickCount64();
        if (publishedAtTickMs == 0 || now < publishedAtTickMs
            || now - publishedAtTickMs > kFrameStaleAfterMs) {
            return S_FALSE;
        }

        const BYTE* frame = SlotBytesAt(m_view, slotIndex);
        BYTE* outputY = destination;
        BYTE* outputUv = destination + static_cast<size_t>(destinationPitch) * outputHeight;
        const BYTE* sourceUv = frame + static_cast<size_t>(sourceWidth) * sourceHeight;
        ScalePlaneAreaAverage(
            frame,
            sourceWidth,
            sourceHeight,
            sourceYStride,
            outputY,
            outputWidth,
            outputHeight,
            static_cast<UINT32>(destinationPitch));
        ScaleNv12ChromaAreaAverage(
            sourceUv,
            sourceWidth,
            sourceHeight,
            outputUv,
            outputWidth,
            outputHeight,
            static_cast<UINT32>(destinationPitch));

        MemoryBarrier();
        const LONG64 sequenceAfter = LoadLong64(&slot->sequenceLock);
        const LONG activeSlotAfter = LoadLong(&mappingHeader->publishedSlot);
        if (sequenceBefore != sequenceAfter || (sequenceAfter & 1) != 0 || activeSlotAfter != slotIndex) {
            continue;
        }
        const ULONGLONG copyFinishedTickMs = GetTickCount64();
        if (copyFinishedTickMs < publishedAtTickMs
            || copyFinishedTickMs - publishedAtTickMs > kFrameStaleAfterMs) {
            return S_FALSE;
        }

        if (sourceTimestampNs != nullptr) {
            *sourceTimestampNs = timestampNs;
        }
        if (sourceSequence != nullptr) {
            *sourceSequence = static_cast<ULONGLONG>(sequenceAfter);
        }
        return S_OK;
    }
    return S_FALSE;
}

void SharedNv12FrameBuffer::Reset() noexcept {
    if (m_view != nullptr) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    if (m_mapping != nullptr) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
    m_accessMode = AccessMode::None;
    m_channelName.clear();
    m_userSid.clear();
    m_mediaSourceMode = false;
    m_mayCreateGlobalMapping = true;
}

} // namespace frame
} // namespace remotephone
