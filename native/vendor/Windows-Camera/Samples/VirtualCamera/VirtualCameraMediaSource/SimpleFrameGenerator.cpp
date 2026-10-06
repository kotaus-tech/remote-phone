//
// Copyright (C) Microsoft Corporation. All rights reserved.
//
#include "pch.h"

namespace
{
    void FillRectangle(
        BYTE* pixels,
        LONG pitch,
        DWORD width,
        DWORD height,
        DWORD left,
        DWORD top,
        DWORD rectangleWidth,
        DWORD rectangleHeight,
        uint32_t color)
    {
        if (left >= width || top >= height)
        {
            return;
        }
        if (rectangleWidth > width - left)
        {
            rectangleWidth = width - left;
        }
        if (rectangleHeight > height - top)
        {
            rectangleHeight = height - top;
        }

        for (DWORD row = top; row < top + rectangleHeight; ++row)
        {
            auto line = reinterpret_cast<uint32_t*>(pixels + row * pitch) + left;
            for (DWORD column = 0; column < rectangleWidth; ++column)
            {
                line[column] = color;
            }
        }
    }

    void DrawFrameCounter(
        BYTE* pixels,
        LONG pitch,
        DWORD width,
        DWORD height,
        ULONG rgbMask,
        UINT32 frameNumber)
    {
        constexpr DWORD kDigitCount = 6;
        constexpr DWORD kDigitWidth = 36;
        constexpr DWORD kDigitHeight = 72;
        constexpr DWORD kSegmentThickness = 8;
        constexpr DWORD kDigitGap = 6;
        constexpr DWORD kPadding = 8;
        constexpr DWORD kPanelLeft = 12;
        constexpr DWORD kPanelTop = 12;
        constexpr DWORD kPanelWidth = kPadding * 2 + kDigitCount * kDigitWidth + (kDigitCount - 1) * kDigitGap;
        constexpr DWORD kPanelHeight = kPadding * 2 + kDigitHeight;

        if (width < kPanelLeft + kPanelWidth || height < kPanelTop + kPanelHeight)
        {
            return;
        }

        constexpr BYTE kTop = 1 << 0;
        constexpr BYTE kUpperRight = 1 << 1;
        constexpr BYTE kLowerRight = 1 << 2;
        constexpr BYTE kBottom = 1 << 3;
        constexpr BYTE kLowerLeft = 1 << 4;
        constexpr BYTE kUpperLeft = 1 << 5;
        constexpr BYTE kMiddle = 1 << 6;
        constexpr BYTE kDigitSegments[10] =
        {
            kTop | kUpperRight | kLowerRight | kBottom | kLowerLeft | kUpperLeft,
            kUpperRight | kLowerRight,
            kTop | kUpperRight | kMiddle | kLowerLeft | kBottom,
            kTop | kUpperRight | kMiddle | kLowerRight | kBottom,
            kUpperLeft | kMiddle | kUpperRight | kLowerRight,
            kTop | kUpperLeft | kMiddle | kLowerRight | kBottom,
            kTop | kUpperLeft | kMiddle | kLowerRight | kBottom | kLowerLeft,
            kTop | kUpperRight | kLowerRight,
            kTop | kUpperRight | kLowerRight | kBottom | kLowerLeft | kUpperLeft | kMiddle,
            kTop | kUpperRight | kLowerRight | kBottom | kUpperLeft | kMiddle
        };
        constexpr DWORD kDivisors[kDigitCount] = { 100000, 10000, 1000, 100, 10, 1 };

        FillRectangle(pixels, pitch, width, height, kPanelLeft, kPanelTop, kPanelWidth, kPanelHeight, 0);
        const uint32_t foreground = static_cast<uint32_t>(rgbMask) & 0x00FFFFFF;

        for (DWORD index = 0; index < kDigitCount; ++index)
        {
            const DWORD digit = (frameNumber % 1000000U / kDivisors[index]) % 10U;
            const BYTE segments = kDigitSegments[digit];
            const DWORD left = kPanelLeft + kPadding + index * (kDigitWidth + kDigitGap);
            const DWORD top = kPanelTop + kPadding;
            const DWORD halfHeight = kDigitHeight / 2;
            const DWORD horizontalWidth = kDigitWidth - 2 * kSegmentThickness;
            const DWORD verticalHeight = halfHeight - kSegmentThickness;

            if (segments & kTop)
            {
                FillRectangle(pixels, pitch, width, height, left + kSegmentThickness, top, horizontalWidth, kSegmentThickness, foreground);
            }
            if (segments & kUpperRight)
            {
                FillRectangle(pixels, pitch, width, height, left + kDigitWidth - kSegmentThickness, top + kSegmentThickness, kSegmentThickness, verticalHeight, foreground);
            }
            if (segments & kLowerRight)
            {
                FillRectangle(pixels, pitch, width, height, left + kDigitWidth - kSegmentThickness, top + halfHeight, kSegmentThickness, verticalHeight, foreground);
            }
            if (segments & kBottom)
            {
                FillRectangle(pixels, pitch, width, height, left + kSegmentThickness, top + kDigitHeight - kSegmentThickness, horizontalWidth, kSegmentThickness, foreground);
            }
            if (segments & kLowerLeft)
            {
                FillRectangle(pixels, pitch, width, height, left, top + halfHeight, kSegmentThickness, verticalHeight, foreground);
            }
            if (segments & kUpperLeft)
            {
                FillRectangle(pixels, pitch, width, height, left, top + kSegmentThickness, kSegmentThickness, verticalHeight, foreground);
            }
            if (segments & kMiddle)
            {
                FillRectangle(pixels, pitch, width, height, left + kSegmentThickness, top + halfHeight - kSegmentThickness / 2, horizontalWidth, kSegmentThickness, foreground);
            }
        }
    }
}

HRESULT SimpleFrameGenerator::Initialize(_In_ IMFMediaType* pMediaType)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, pMediaType);

    RETURN_IF_FAILED(pMediaType->GetGUID(MF_MT_SUBTYPE, &m_subType));
    if (m_subType != MFVideoFormat_RGB32 && m_subType != MFVideoFormat_NV12)
    {
        RETURN_HR_MSG(MF_E_UNSUPPORTED_FORMAT, "Unsupported format: %s", winrt::to_hstring(m_subType).data());
    }
    MFGetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, &m_width, &m_height);

    return S_OK;
}

/*:
   Writes to a buffer representing a 2D image.
   Writes a per-frame moving pattern and six-digit frame counter.
   Assumes top down image, no negative stride and pBuf points to the begnning of the buffer of length len.
   Param:
   pBuf - pointer to beginning of buffer
   pitch - line length in bytes
   len - length of buffer in bytes
*/
HRESULT SimpleFrameGenerator::CreateFrame(
    _Inout_updates_bytes_(len) BYTE* pBuf,
    _In_ DWORD len,
    _In_ LONG pitch,
    _In_ ULONG rgbMask)
{
    ++m_frameNumber;
    if (m_subType == MFVideoFormat_RGB32)
    {
        DEBUG_MSG(L"RGB32 frames %s\n", winrt::to_hstring(MFVideoFormat_RGB32).data());

        RETURN_IF_FAILED(_CreateRGB32Frame(pBuf, len, pitch, m_width, m_height, rgbMask, m_frameNumber));
    }
    else if(m_subType == MFVideoFormat_NV12)
    {
        DEBUG_MSG(L"NV12 frames %s \n", winrt::to_hstring(MFVideoFormat_NV12).data());

        DWORD frameBuffLen = m_width * m_height * 4;
        wil::unique_cotaskmem_ptr<BYTE[]> spBuff = wil::make_unique_cotaskmem_nothrow<BYTE[]>(frameBuffLen);
        RETURN_IF_NULL_ALLOC(spBuff.get());

        RETURN_IF_FAILED(_CreateRGB32Frame(spBuff.get(), frameBuffLen, m_width * 4, m_width, m_height, rgbMask, m_frameNumber));
        RETURN_IF_FAILED(RGB32ToNV12Frame(spBuff.get(), frameBuffLen, m_width * 4, m_width, m_height, pBuf, len, pitch));
    }
    else
    {
        return MF_E_UNSUPPORTED_FORMAT;
    }

    return S_OK;
}

//////////////////////////////////////////////////
// private

HRESULT SimpleFrameGenerator::_CreateRGB32Frame(
    _Inout_updates_bytes_(len) BYTE* pBuf,
    _In_ DWORD len,
    _In_ LONG pitch,
    _In_ DWORD width,
    _In_ DWORD height,
    _In_ ULONG rgbMask,
    _In_ UINT32 frameNumber )
{
    RETURN_HR_IF_NULL(E_INVALIDARG, pBuf);
    if (len < (abs(pitch) * height ))
    {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    const int offset = static_cast<int>(frameNumber % height);

    for (unsigned int r = 0; r < height; r++)
    {
        uint32_t* p = (uint32_t*)(pBuf + (r * pitch));
        for (unsigned int c = 0; c < width; c++)
        {
            BYTE gray = (BYTE)(r + offset);
            *p = ((uint32_t)gray << 16 | (uint32_t)gray << 8 | (uint32_t)gray) & rgbMask;
            p++;
        }
    }

    DrawFrameCounter(pBuf, pitch, width, height, rgbMask, frameNumber);
    return S_OK;
}

//////////////////////////////////////////////////
// pixelFormatConverter

void SimpleFrameGenerator::RGB24ToYUY2(int R, int G, int B, BYTE* pY, BYTE* pU, BYTE* pV)
{
    *pY = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
    *pU = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
    *pV = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
}

void SimpleFrameGenerator::RGB24ToY(int R, int G, int B, BYTE* pY)
{
    *pY = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
}

void SimpleFrameGenerator::RGB32ToNV12(BYTE RGB1[8], BYTE RGB2[8], BYTE* pY1, BYTE* pY2, BYTE* pUV)
{
    RGB24ToYUY2(RGB1[2], RGB1[1], RGB1[0], pY1, pUV, pUV + 1);
    RGB24ToY(RGB1[6], RGB1[5], RGB1[4], pY1 + 1);
    RGB24ToYUY2(RGB2[2], RGB2[1], RGB2[0], pY2, pUV, pUV + 1);
    RGB24ToY(RGB2[6], RGB2[5], RGB2[4], pY2 + 1);
};

//////////////////////////////////////////////////
// FrameFormatConverter

HRESULT SimpleFrameGenerator::RGB32ToNV12Frame(_Inout_updates_bytes_(len) BYTE* pbBuff, ULONG cbBuff, long stride, UINT width, UINT height, BYTE* pbBuffOut, ULONG cbBuffOut, long strideOut)
{
    do
    {
        RETURN_HR_IF(E_UNEXPECTED, width * 4 * height > cbBuff);
        RETURN_HR_IF(E_UNEXPECTED, width * 1.5 * height > cbBuffOut);
        RETURN_HR_IF_NULL(E_INVALIDARG, pbBuff);

        RETURN_HR_IF_NULL(E_INVALIDARG, pbBuffOut);
        for (DWORD h = 0; h < height - 1; h += 2)
        {
            BYTE* pRGB1 = h * stride + pbBuff;
            BYTE* pRGB2 = (h + 1) * stride + pbBuff;
            BYTE* pY1 = h * strideOut + pbBuffOut;
            BYTE* pY2 = (h + 1) * strideOut + pbBuffOut;
            BYTE* pUV = (h / 2 + height) * strideOut + pbBuffOut;

            for (DWORD w = 0; w < width; w += 2)
            {
                RGB32ToNV12(pRGB1, pRGB2, pY1, pY2, pUV);
                pRGB1 += 8;
                pRGB2 += 8;
                pY1 += 2;
                pY2 += 2;
                pUV += 2;
            }
        }
    } while (FALSE);

    return S_OK;
}
