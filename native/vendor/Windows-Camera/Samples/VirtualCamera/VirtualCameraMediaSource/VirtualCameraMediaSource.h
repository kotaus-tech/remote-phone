//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#pragma once
#ifndef VIRTUALCAMERA_MEDIASOURCE_H
#define VIRTUALCAMERA_MEDIASOURCE_H

#include <initguid.h>

// {7B89B92E-FE71-42D0-8A41-E137D06EA184}
DEFINE_GUID(CLSID_VirtualCameraMediaSource ,
    0x7b89b92e, 0xfe71, 0x42d0, 0x8a, 0x41, 0xe1, 0x37, 0xd0, 0x6e, 0xa1, 0x84);

static LPCWSTR VIRTUALCAMERAMEDIASOURCE_CLSID = L"{7B89B92E-FE71-42D0-8A41-E137D06EA184}";
static LPCWSTR VIRTUALCAMERAMEDIASOURCE_FRIENDLYNAME = L"VirtualCameraMediaSource";
static LPCWSTR VIRTUALCAMERAMEDIASOURCE_BUILD_VERSION = L"0.1.0";

// The below 2 GUIDs are defined in Windows build 22621 and above only, 
// if targetting SDK for Windows 22621 or higher this would need to be commented out.
// GUIDs for retrieving the device source instead of recreating it from within the vcam
//DEFINE_GUID(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES,
//    0xF0273718, 0x4A4D, 0x4AC5, 0xA1, 0x5D, 0x30, 0x5E, 0xB5, 0xE9, 0x06, 0x67);
//
//DEFINE_GUID(MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES,
//    0x1BB79E7C, 0x5D83, 0x438C, 0x94, 0xD8, 0xE5, 0xF0, 0xDF, 0x6D, 0x32, 0x79);

// --> VirtualCameraMediaSource activation attributes:

// {3C31A5F8-2795-4FB9-A0A1-C733A65C0CE8}
// The value of this attribute is the physical camera symboliclink name that the VirtualCameraMediaSource
// will be using.
DEFINE_GUID(VCAM_DEVICE_INFO,
    0x3c31a5f8, 0x2795, 0x4fb9, 0xa0, 0xa1, 0xc7, 0x33, 0xa6, 0x5c, 0xc, 0xe8);

// {C7F7C57B-DF30-41D0-AFFC-15201CDF920D}
// Defines the kind of virtual camera to be instantiated
DEFINE_GUID(VCAM_KIND,
    0xc7f7c57b, 0xdf30, 0x41d0, 0xaf, 0xfc, 0x15, 0x20, 0x1c, 0xdf, 0x92, 0xd);

// {E57D6C8A-F640-4898-9E49-3558A89FEF2C}
// Optional path used by the early PoC to collect media-source diagnostics.
DEFINE_GUID(VCAM_DIAGNOSTIC_LOG_PATH,
    0xe57d6c8a, 0xf640, 0x4898, 0x9e, 0x49, 0x35, 0x58, 0xa8, 0x9f, 0xef, 0x2c);

// Per-session NV12 shared-memory channel identity. The random channel name is
// pagefile-backed and exists only while the host and media source hold handles.
// {4A9BDC62-199B-4F86-8F28-68A24B68F7C0}
DEFINE_GUID(VCAM_FRAME_CHANNEL_NAME,
    0x4a9bdc62, 0x199b, 0x4f86, 0x8f, 0x28, 0x68, 0xa2, 0x4b, 0x68, 0xf7, 0xc0);

// User SID used to restrict the channel DACL to the app owner, LocalService, and SYSTEM.
// {7AC5D973-7B7A-4D09-9159-6FC87BCD5D61}
DEFINE_GUID(VCAM_FRAME_CHANNEL_USER_SID,
    0x7ac5d973, 0x7b7a, 0x4d09, 0x91, 0x59, 0x6f, 0xc8, 0x7b, 0xcd, 0x5d, 0x61);

// <-- VirtualCameraMediaSource activation attributes

// Example Custom Property implemented by SimpleMediaSource
// 
// {0CE2EF73-4800-4F53-9B8E-8C06790FC0C7}
static const GUID PROPSETID_SIMPLEMEDIASOURCE_CUSTOMCONTROL =
{ 0xce2ef73, 0x4800, 0x4f53, { 0x9b, 0x8e, 0x8c, 0x6, 0x79, 0xf, 0xc0, 0xc7 } };

enum
{
    KSPROPERTY_SIMPLEMEDIASOURCE_CUSTOMCONTROL_COLORING = 0,
    KSPROPERTY_SIMPLEMEDIASOURCE_CUSTOMCONTROL_END  // all ids must be defined before this.
};

enum
{
    KSPROPERTY_SIMPLEMEDIASOURCE_CUSTOMCONTROL_COLORMODE_GRAYSCALE = 0x0FFFFFF,
    KSPROPERTY_SIMPLEMEDIASOURCE_CUSTOMCONTROL_COLORMODE_RED = 0x0FF0000,
    KSPROPERTY_SIMPLEMEDIASOURCE_CUSTOMCONTROL_COLORMODE_GREEN = 0x000FF00,
    KSPROPERTY_SIMPLEMEDIASOURCE_CUSTOMCONTROL_COLORMODE_BLUE = 0x00000FF
};

typedef struct {
    uint32_t      ColorMode;
} KSPROPERTY_SIMPLEMEDIASOURCE_CUSTOMCONTROL_COLORMODE_S, * PKSPROPERTY_SIMPLEMEDIASOURCE_CUSTOMCONTROL_COLORMODE_S;

// Example custom control implemented by the AugmentedMediaSource
// 
// {0C4384E1-B457-43A7-B776-6D031DD88B12}
static const GUID PROPSETID_AUGMENTEDMEDIASOURCE_CUSTOMCONTROL =
{ 0xc4384e1, 0xb457, 0x43a7, { 0xb7, 0x76, 0x6d, 0x3, 0x1d, 0xd8, 0x8b, 0x12 } };

enum
{
    KSPROPERTY_AUGMENTEDMEDIASOURCE_CUSTOMCONTROL_CUSTOMFX = 0,
    KSPROPERTY_AUGMENTEDMEDIASOURCE_CUSTOMCONTROL_END  // all ids must be defined before this.
};

enum
{
    KSPROPERTY_AUGMENTEDMEDIASOURCE_CUSTOMCONTROL_CUSTOMFX_OFF = 0,
    KSPROPERTY_AUGMENTEDMEDIASOURCE_CUSTOMCONTROL_CUSTOMFX_AUTO = 1
};

typedef struct
{
    uint64_t Capability;
    uint64_t Flags;
    uint32_t Size;
} KSPROPERTY_AUGMENTEDMEDIASOURCE_CUSTOMCONTROL_FX, * PKSPROPERTY_AUGMENTEDMEDIASOURCE_CUSTOMCONTROL_FX;

// reproduction of the enum defined in VirtualCameraManager.idl
enum class VirtualCameraKind : int32_t
{
    Synthetic = 0, // Refer to SimpleMediaSource
    BasicCameraWrapper = 1, // Refer to HWMediaSource
    AugmentedCameraWrapper = 2, // Refer to AugmentedMediaSource
};

#endif