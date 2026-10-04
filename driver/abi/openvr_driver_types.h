// SPDX-License-Identifier: MPL-2.0
//
// Only the enum values and structs the driver uses from the PC VR runtime's driver interface,
// declared by hand from the interface SDK 2.15.6; sizes and offsets are asserted below.

#pragma once

#include <cstddef>
#include <cstdint>

namespace oxrvr
{

using SharedTextureHandle_t = uint64_t;
using PropertyContainerHandle_t = uint64_t;
using DriverHandle_t = uint64_t;
using TrackedDeviceIndex_t = uint32_t;
using PropertyTypeTag_t = uint32_t;
using VRInputComponentHandle_t = uint64_t;

enum EVRInitError
{
    VRInitError_None = 0,
    VRInitError_Init_InterfaceNotFound = 105,
};

enum EVREye
{
    Eye_Left = 0,
    Eye_Right = 1,
};

enum ETrackedDeviceClass
{
    TrackedDeviceClass_HMD = 1,
    TrackedDeviceClass_Controller = 2,
};

enum ETrackedControllerRole
{
    TrackedControllerRole_LeftHand = 1,
    TrackedControllerRole_RightHand = 2,
};

enum EVRInputError
{
    VRInputError_None = 0,
};

enum EVRScalarType
{
    VRScalarType_Absolute = 0,
};

enum EVRScalarUnits
{
    VRScalarUnits_NormalizedOneSided = 0,
    VRScalarUnits_NormalizedTwoSided = 1,
};

enum ETrackingResult
{
    TrackingResult_Running_OK = 200,
};

enum ETrackedPropertyError
{
    TrackedProp_Success = 0,
};

enum EVRSettingsError
{
    VRSettingsError_None = 0,
};

enum EPropertyWriteType
{
    PropertyWrite_Set = 0,
};

enum ETrackedDeviceProperty
{
    Prop_TrackingSystemName_String = 1000,
    Prop_ModelNumber_String = 1001,
    Prop_SerialNumber_String = 1002,
    Prop_RenderModelName_String = 1003,
    Prop_ManufacturerName_String = 1005,
    Prop_ControllerRoleHint_Int32 = 3007,
    Prop_SecondsFromVsyncToPhotons_Float = 2001,
    Prop_DisplayFrequency_Float = 2002,
    Prop_UserIpdMeters_Float = 2003,
    Prop_CurrentUniverseId_Uint64 = 2004,
    Prop_IsOnDesktop_Bool = 2007,
    Prop_DriverDirectModeSendsVsyncEvents_Bool = 2043,
    Prop_GraphicsAdapterLuid_Uint64 = 2045,
    Prop_DriverProvidedChaperoneJson_String = 2095,
    Prop_InputProfilePath_String = 1037,
    Prop_NamedIconPathDeviceOff_String = 5001,
    Prop_NamedIconPathDeviceSearching_String = 5002,
    Prop_NamedIconPathDeviceSearchingAlert_String = 5003,
    Prop_NamedIconPathDeviceReady_String = 5004,
    Prop_NamedIconPathDeviceReadyAlert_String = 5005,
    Prop_NamedIconPathDeviceNotReady_String = 5006,
    Prop_NamedIconPathDeviceStandby_String = 5007,
    Prop_NamedIconPathDeviceAlertLow_String = 5008,
    Prop_NamedIconPathDeviceStandbyAlert_String = 5009,
    Prop_ControllerType_String = 7000,
};

inline constexpr PropertyTypeTag_t k_unFloatPropertyTag = 1;
inline constexpr PropertyTypeTag_t k_unInt32PropertyTag = 2;
inline constexpr PropertyTypeTag_t k_unUint64PropertyTag = 3;
inline constexpr PropertyTypeTag_t k_unBoolPropertyTag = 4;
inline constexpr PropertyTypeTag_t k_unStringPropertyTag = 5;

inline constexpr uint32_t VREvent_Quit = 700;

struct HmdQuaternion_t
{
    double w, x, y, z;
};

struct HmdMatrix34_t
{
    float m[3][4];
};

struct HmdMatrix44_t
{
    float m[4][4];
};

struct HmdVector2_t
{
    float v[2];
};

struct VRTextureBounds_t
{
    float uMin, vMin;
    float uMax, vMax;
};

struct DistortionCoordinates_t
{
    float rfRed[2];
    float rfGreen[2];
    float rfBlue[2];
};

struct DriverPose_t
{
    double poseTimeOffset;
    HmdQuaternion_t qWorldFromDriverRotation;
    double vecWorldFromDriverTranslation[3];
    HmdQuaternion_t qDriverFromHeadRotation;
    double vecDriverFromHeadTranslation[3];
    double vecPosition[3];
    double vecVelocity[3];
    double vecAcceleration[3];
    HmdQuaternion_t qRotation;
    double vecAngularVelocity[3];
    double vecAngularAcceleration[3];
    ETrackingResult result;
    bool poseIsValid;
    bool willDriftInYaw;
    bool shouldApplyHeadModel;
    bool deviceIsConnected;
};

struct SwapTextureSetDesc_t
{
    uint32_t nWidth;
    uint32_t nHeight;
    uint32_t nFormat;
    uint32_t nSampleCount;
};

struct SwapTextureSet_t
{
    SharedTextureHandle_t rSharedTextureHandles[3];
    uint32_t unTextureFlags;
};

struct SubmitLayerPerEye_t
{
    SharedTextureHandle_t hTexture, hDepthTexture;
    VRTextureBounds_t bounds;
    HmdMatrix44_t mProjection;
    HmdMatrix34_t mHmdPose;
    float flHmdPosePredictionTimeInSecondsFromNow;
};

struct Throttling_t
{
    uint32_t nFramesToThrottle;
    uint32_t nAdditionalFramesToPredict;
};

struct DriverDirectMode_FrameTiming
{
    uint32_t m_nSize;
    uint32_t m_nNumFramePresents;
    uint32_t m_nNumMisPresented;
    uint32_t m_nNumDroppedFrames;
    uint32_t m_nReprojectionFlags;
};

struct PropertyWrite_t
{
    ETrackedDeviceProperty prop;
    EPropertyWriteType writeType;
    ETrackedPropertyError eSetError;
    void *pvBuffer;
    uint32_t unBufferSize;
    PropertyTypeTag_t unTag;
    ETrackedPropertyError eError;
};

// The event header; the 48-byte data union is kept opaque.
struct VREvent_t
{
    uint32_t eventType;
    TrackedDeviceIndex_t trackedDeviceIndex;
    float eventAgeSeconds;
    uint64_t data[6];
};

static_assert(sizeof(DriverPose_t) == 280);
static_assert(offsetof(DriverPose_t, qWorldFromDriverRotation) == 8);
static_assert(offsetof(DriverPose_t, vecPosition) == 120);
static_assert(offsetof(DriverPose_t, qRotation) == 192);
static_assert(offsetof(DriverPose_t, result) == 272);
static_assert(offsetof(DriverPose_t, poseIsValid) == 276);
static_assert(offsetof(DriverPose_t, deviceIsConnected) == 279);
static_assert(sizeof(DistortionCoordinates_t) == 24);
static_assert(sizeof(SwapTextureSetDesc_t) == 16);
static_assert(sizeof(SwapTextureSet_t) == 32);
static_assert(offsetof(SwapTextureSet_t, unTextureFlags) == 24);
static_assert(sizeof(SubmitLayerPerEye_t) == 152);
static_assert(offsetof(SubmitLayerPerEye_t, bounds) == 16);
static_assert(offsetof(SubmitLayerPerEye_t, mProjection) == 32);
static_assert(offsetof(SubmitLayerPerEye_t, mHmdPose) == 96);
static_assert(offsetof(SubmitLayerPerEye_t, flHmdPosePredictionTimeInSecondsFromNow) == 144);
static_assert(sizeof(Throttling_t) == 8);
static_assert(sizeof(DriverDirectMode_FrameTiming) == 20);
static_assert(sizeof(PropertyWrite_t) == 40);
static_assert(offsetof(PropertyWrite_t, pvBuffer) == 16);
static_assert(offsetof(PropertyWrite_t, unBufferSize) == 24);
static_assert(offsetof(PropertyWrite_t, unTag) == 28);
static_assert(offsetof(PropertyWrite_t, eError) == 32);
static_assert(sizeof(VREvent_t) == 64);
static_assert(offsetof(VREvent_t, data) == 16);

} // namespace oxrvr
