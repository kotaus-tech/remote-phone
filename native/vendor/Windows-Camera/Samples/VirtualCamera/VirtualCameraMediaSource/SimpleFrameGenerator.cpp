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
        (void)rgbMask;
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
        // A white counter on the dark panel remains legible in browser/meeting-app previews.
        const uint32_t foreground = 0x00FFFFFF;

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

    bool GetCounterPixelColor(
        DWORD x,
        DWORD y,
        DWORD width,
        DWORD height,
        UINT32 frameNumber,
        ULONG* color)
    {
        constexpr DWORD digitCount = 6;
        constexpr DWORD digitWidth = 36;
        constexpr DWORD digitHeight = 72;
        constexpr DWORD segmentThickness = 8;
        constexpr DWORD digitGap = 6;
        constexpr DWORD padding = 8;
        constexpr DWORD panelLeft = 12;
        constexpr DWORD panelTop = 12;
        constexpr DWORD panelWidth = padding * 2 + digitCount * digitWidth + (digitCount - 1) * digitGap;
        constexpr DWORD panelHeight = padding * 2 + digitHeight;
        constexpr BYTE top = 1 << 0;
        constexpr BYTE upperRight = 1 << 1;
        constexpr BYTE lowerRight = 1 << 2;
        constexpr BYTE bottom = 1 << 3;
        constexpr BYTE lowerLeft = 1 << 4;
        constexpr BYTE upperLeft = 1 << 5;
        constexpr BYTE middle = 1 << 6;
        constexpr BYTE digitSegments[10] =
        {
            top | upperRight | lowerRight | bottom | lowerLeft | upperLeft,
            upperRight | lowerRight,
            top | upperRight | middle | lowerLeft | bottom,
            top | upperRight | middle | lowerRight | bottom,
            upperLeft | middle | upperRight | lowerRight,
            top | upperLeft | middle | lowerRight | bottom,
            top | upperLeft | middle | lowerRight | bottom | lowerLeft,
            top | upperRight | lowerRight,
            top | upperRight | lowerRight | bottom | lowerLeft | upperLeft | middle,
            top | upperRight | lowerRight | bottom | upperLeft | middle
        };
        constexpr DWORD divisors[digitCount] = { 100000, 10000, 1000, 100, 10, 1 };

        if (width < panelLeft + panelWidth || height < panelTop + panelHeight ||
            x < panelLeft || x >= panelLeft + panelWidth ||
            y < panelTop || y >= panelTop + panelHeight)
        {
            return false;
        }

        *color = 0;
        const DWORD halfHeight = digitHeight / 2;
        const DWORD horizontalWidth = digitWidth - 2 * segmentThickness;
        const DWORD verticalHeight = halfHeight - segmentThickness;
        for (DWORD index = 0; index < digitCount; ++index)
        {
            const DWORD digit = (frameNumber % 1000000U / divisors[index]) % 10U;
            const BYTE segments = digitSegments[digit];
            const DWORD left = panelLeft + padding + index * (digitWidth + digitGap);
            const DWORD topEdge = panelTop + padding;
            const auto inside = [x, y](DWORD leftEdge, DWORD topEdge, DWORD rectangleWidth, DWORD rectangleHeight)
            {
                return x >= leftEdge && x < leftEdge + rectangleWidth &&
                    y >= topEdge && y < topEdge + rectangleHeight;
            };

            if (((segments & top) && inside(left + segmentThickness, topEdge, horizontalWidth, segmentThickness)) ||
                ((segments & upperRight) && inside(left + digitWidth - segmentThickness, topEdge + segmentThickness, segmentThickness, verticalHeight)) ||
                ((segments & lowerRight) && inside(left + digitWidth - segmentThickness, topEdge + halfHeight, segmentThickness, verticalHeight)) ||
                ((segments & bottom) && inside(left + segmentThickness, topEdge + digitHeight - segmentThickness, horizontalWidth, segmentThickness)) ||
                ((segments & lowerLeft) && inside(left, topEdge + halfHeight, segmentThickness, verticalHeight)) ||
                ((segments & upperLeft) && inside(left, topEdge + segmentThickness, segmentThickness, verticalHeight)) ||
                ((segments & middle) && inside(left + segmentThickness, topEdge + halfHeight - segmentThickness / 2, horizontalWidth, segmentThickness)))
            {
                *color = 0x00FFFFFF;
                break;
            }
        }
        return true;
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
    RETURN_IF_FAILED(MFGetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, &m_width, &m_height));
    RETURN_HR_IF(E_INVALIDARG, m_width == 0 || m_height == 0);
    if (m_subType == MFVideoFormat_NV12)
    {
        RETURN_HR_IF(E_INVALIDARG, (m_width & 1) != 0 || (m_height & 1) != 0);
        RETURN_HR_IF(E_INVALIDARG, m_width > 3840 || m_height > 2160);
    }
    else
    {
        RETURN_HR_IF(E_INVALIDARG, m_width > 640 || m_height > 480);
    }

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
    else if (m_subType == MFVideoFormat_NV12)
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, pBuf);
        RETURN_HR_IF(E_INVALIDARG, pitch <= 0 || (pitch & 1) != 0 || (m_width & 1) != 0 || (m_height & 1) != 0);
        RETURN_HR_IF(E_INVALIDARG, static_cast<UINT32>(pitch) < m_width);

        const ULONGLONG yPlaneSize = static_cast<ULONGLONG>(pitch) * m_height;
        const ULONGLONG requiredSize = yPlaneSize + yPlaneSize / 2;
        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), requiredSize > len);

        const UINT32 offset = m_frameNumber % m_height;
        for (UINT32 row = 0; row < m_height; ++row)
        {
            const BYTE gray = static_cast<BYTE>(row + offset);
            const ULONG background =
                ((static_cast<ULONG>(gray) << 16) |
                    (static_cast<ULONG>(gray) << 8) |
                    static_cast<ULONG>(gray)) & rgbMask;
            BYTE yValue = 0;
            RGB24ToY(
                static_cast<int>((background >> 16) & 0xFF),
                static_cast<int>((background >> 8) & 0xFF),
                static_cast<int>(background & 0xFF),
                &yValue);
            std::memset(pBuf + static_cast<size_t>(row) * pitch, yValue, m_width);
        }

        constexpr DWORD panelLeft = 12;
        constexpr DWORD panelTop = 12;
        constexpr DWORD panelWidth = 262;
        constexpr DWORD panelHeight = 88;
        if (m_width >= panelLeft + panelWidth && m_height >= panelTop + panelHeight)
        {
            for (DWORD row = panelTop; row < panelTop + panelHeight; ++row)
            {
                BYTE* luma = pBuf + static_cast<size_t>(row) * pitch;
                for (DWORD column = panelLeft; column < panelLeft + panelWidth; ++column)
                {
                    ULONG overlayColor = 0;
                    if (GetCounterPixelColor(column, row, m_width, m_height, m_frameNumber, &overlayColor))
                    {
                        BYTE yValue = 16;
                        if (overlayColor != 0)
                        {
                            RGB24ToY(255, 255, 255, &yValue);
                        }
                        luma[column] = yValue;
                    }
                }
            }
        }

        BYTE* chromaPlane = pBuf + static_cast<size_t>(yPlaneSize);
        for (UINT32 row = 0; row < m_height; row += 2)
        {
            BYTE* chroma = chromaPlane + static_cast<size_t>(row / 2) * pitch;
            for (UINT32 column = 0; column < m_width; column += 2)
            {
                ULONG color = 0;
                if (!GetCounterPixelColor(column, row, m_width, m_height, m_frameNumber, &color))
                {
                    const BYTE gray = static_cast<BYTE>(row + offset);
                    color =
                        ((static_cast<ULONG>(gray) << 16) |
                            (static_cast<ULONG>(gray) << 8) |
                            static_cast<ULONG>(gray)) & rgbMask;
                }

                BYTE unusedY = 0;
                BYTE u = 128;
                BYTE v = 128;
                RGB24ToYUY2(
                    static_cast<int>((color >> 16) & 0xFF),
                    static_cast<int>((color >> 8) & 0xFF),
                    static_cast<int>(color & 0xFF),
                    &unusedY,
                    &u,
                    &v);
                chroma[column] = u;
                chroma[column + 1] = v;
            }
        }
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
    RETURN_HR_IF(E_INVALIDARG, pitch <= 0);
    RETURN_HR_IF(E_INVALIDARG, static_cast<ULONGLONG>(pitch) < static_cast<ULONGLONG>(width) * sizeof(uint32_t));
    RETURN_HR_IF(
        HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
        static_cast<ULONGLONG>(pitch) * height > len);

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
