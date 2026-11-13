#include "VideoEncoderVPL.h"
#include "alvr_server/Logger.h"
#include "alvr_server/Utils.h"
#include "alvr_server/bindings.h"

#define VPLVERSION(major, minor) (major << 16 | minor)
#define MAJOR_API_VERSION_REQUIRED 2
#define MINOR_API_VERSION_REQUIRED 9
#define WAIT_100_MILLISECONDS 100
#define ALIGN16(value) (((value + 15) >> 4) << 4)
#define ERROR_THROW(msg, ...)                                                                      \
    {                                                                                              \
        Error("VPL: " msg "\n", __VA_ARGS__);                                                      \
        throw MakeException("VPL: " msg, __VA_ARGS__);                                             \
    }

#define VPL_LOG(fn, msg, ...) fn("VPL: " msg "\n", __VA_ARGS__)
#define VPL_DEBUG(msg, ...) VPL_LOG(Debug, msg, __VA_ARGS__)
#define VPL_WARN(msg, ...) VPL_LOG(Warn, msg, __VA_ARGS__)
#define VPL_INFO(msg, ...) VPL_LOG(Info, msg, __VA_ARGS__)

#define VERIFY(expr, msg)                                                                          \
    if (!(expr))                                                                                   \
    ERROR_THROW("%s. %s", msg, #expr)
#define VPL_VERIFY(expr)                                                                           \
    {                                                                                              \
        mfxStatus res = expr;                                                                      \
        if (res != MFX_ERR_NONE)                                                                   \
            ERROR_THROW("\"%s\" failed with %d", #expr, res);                                      \
    }

VideoEncoderVPL::VideoEncoderVPL(std::shared_ptr<CD3DRender> pD3DRender, int width, int height)
    : m_pD3DRender(pD3DRender)
    , m_renderWidth(width)
    , m_renderHeight(height)
    , m_bitrateInMBits(30) {
    VPL_DEBUG("constructed");
}

VideoEncoderVPL::~VideoEncoderVPL() { VPL_DEBUG("destructed"); }

void VideoEncoderVPL::Initialize() {
    VPL_DEBUG("initialize");

    ChooseParams();
    InitTransferTex();
    InitVpl();
    InitVplEncode();

    // Prepare output bitstream
    m_vplBitstream.MaxLength = m_renderWidth * m_renderHeight * 8;
    m_vplBitstream.Data = (mfxU8*)calloc(m_vplBitstream.MaxLength, sizeof(mfxU8));
}

void VideoEncoderVPL::Shutdown() {
    VPL_DEBUG("shutdown");

    MFXVideoENCODE_Close(m_vplSession);
    MFXClose(m_vplSession);

    if (m_vplBitstream.Data)
        free(m_vplBitstream.Data);

    if (m_vplLoader)
        MFXUnload(m_vplLoader);
}

void VideoEncoderVPL::Transmit(
    ID3D11Texture2D* pTexture, uint64_t presentationTime, uint64_t targetTimestampNs, bool insertIDR
) {
    // VPL_DEBUG("transmit");

    auto dynParams = GetDynamicEncoderParams();
    if (dynParams.updated) {
        // The encoder is configured with a fixed frame rate (m_refreshRate), so scale the bitrate
        // to keep the requested bits per frame when the actual frame rate differs (same as AMF).
        uint64_t bitrateBps = dynParams.bitrate_bps;
        if (dynParams.framerate > 0.0f) {
            bitrateBps = static_cast<uint64_t>(
                static_cast<double>(dynParams.bitrate_bps) / dynParams.framerate * m_refreshRate
            );
        }
        // The frame rate jitters, so ignore small changes to avoid resetting the encoder often.
        uint64_t applied = m_appliedBitrateBps;
        bool significant = applied == 0 || bitrateBps > applied + applied / 10
            || bitrateBps < applied - applied / 10;
        mfxVideoParam previousParams = m_vplEncodeParams;
        if (significant && SetBitrate(bitrateBps)) {
            m_appliedBitrateBps = bitrateBps;

            // Keep the current sequence: a new one could change the SPS/PPS, which ALVR only sends
            // to the client at stream start or when the client asks for an IDR.
            mfxExtEncoderResetOption resetOption = {};
            resetOption.Header.BufferId = MFX_EXTBUFF_ENCODER_RESET_OPTION;
            resetOption.Header.BufferSz = sizeof(resetOption);
            resetOption.StartNewSequence = MFX_CODINGOPTION_OFF;
            mfxExtBuffer* extBuffers[] = { &resetOption.Header };
            mfxVideoParam resetParams = m_vplEncodeParams;
            resetParams.ExtParam = extBuffers;
            resetParams.NumExtParam = 1;

            mfxStatus sts = MFXVideoENCODE_Reset(m_vplSession, &resetParams);
            if (sts < MFX_ERR_NONE) {
                // The encoder keeps running with its previous parameters.
                VPL_WARN("failed to update bitrate to %llu bps (error %d)", bitrateBps, sts);
                m_vplEncodeParams = previousParams;
            }
        }
    }

    auto encSurface = VplImportTexture(pTexture);

    mfxEncodeCtrl encodeCtrl = {};
    // An IDR request must also carry the frame type it applies to (I) and mark the frame as a
    // reference. Passing MFX_FRAMETYPE_IDR on its own is rejected by the AVC encoder with
    // MFX_ERR_INVALID_VIDEO_PARAM.
    encodeCtrl.FrameType = insertIDR ? (MFX_FRAMETYPE_I | MFX_FRAMETYPE_REF | MFX_FRAMETYPE_IDR)
                                     : MFX_FRAMETYPE_UNKNOWN;

    mfxStatus sts = MFX_ERR_NONE;
    mfxSyncPoint syncp = {};
    bool isEncGoing = true;
    bool isDraining = false;

    while (isEncGoing) {
        isDraining = encSurface == nullptr;
        sts = MFXVideoENCODE_EncodeFrameAsync(
            m_vplSession, &encodeCtrl, encSurface, &m_vplBitstream, &syncp
        );

        if (encSurface) {
            VPL_VERIFY(encSurface->FrameInterface->Release(encSurface));
            encSurface = nullptr;
        }

        switch (sts) {
        case MFX_ERR_NONE:
            // MFX_ERR_NONE and syncp indicate output is available
            if (syncp) {
                // Encode output is not available on CPU until sync operation completes
                do {
                    sts = MFXVideoCORE_SyncOperation(m_vplSession, syncp, WAIT_100_MILLISECONDS);
                    if (MFX_ERR_NONE == sts) {
                        ParseFrameNals(
                            m_codec,
                            reinterpret_cast<uint8_t*>(
                                m_vplBitstream.Data + m_vplBitstream.DataOffset
                            ),
                            m_vplBitstream.DataLength,
                            targetTimestampNs,
                            insertIDR
                        );
                        m_vplBitstream.DataLength = 0;
                    }
                } while (sts == MFX_WRN_IN_EXECUTION);
            }
            break;

        case MFX_ERR_NOT_ENOUGH_BUFFER:
            ERROR_THROW("not enough buffer");

        case MFX_ERR_MORE_DATA:
            if (isDraining == true)
                isEncGoing = false;
            break;

        case MFX_ERR_DEVICE_LOST:
            ERROR_THROW("device lost");

        case MFX_WRN_DEVICE_BUSY:
            VPL_DEBUG("device busy");
            break;

        default:
            ERROR_THROW("unknown encoding status %d", sts);
        }
    }
}

void VideoEncoderVPL::InitTransferTex() {
    D3D11_TEXTURE2D_DESC transferTexDesc = { UINT(m_renderWidth),
                                             UINT(m_renderHeight),
                                             1,
                                             1,
                                             m_dxColorFormat,
                                             { 1, 0 },
                                             D3D11_USAGE_DEFAULT,
                                             D3D11_BIND_SHADER_RESOURCE,
                                             0,
                                             D3D11_RESOURCE_MISC_SHARED };

    HRESULT hr
        = m_pD3DRender->GetDevice()->CreateTexture2D(&transferTexDesc, nullptr, &m_transferTex);
    if (FAILED(hr))
        ERROR_THROW("failed to create transfer texture HR=%p %ls", hr, GetErrorStr(hr).c_str());
}

void VideoEncoderVPL::InitVpl() {
    m_vplLoader = MFXLoad();
    VERIFY(m_vplLoader != nullptr, "MFXLoad failed -- is implementation in path?");

    CheckVPLConfig();

    VPL_VERIFY(MFXCreateSession(m_vplLoader, 0, &m_vplSession));
    VPL_VERIFY(
        MFXVideoCORE_SetHandle(m_vplSession, MFX_HANDLE_D3D11_DEVICE, m_pD3DRender->GetDevice())
    );

    LogImplementationInfo();

    // Get interface for ImportFrameSurface
    VPL_VERIFY(MFXGetMemoryInterface(m_vplSession, &m_vplMemoryInterface));
    VERIFY(m_vplMemoryInterface != nullptr, "MFXGetMemoryInterface failed");
}

void VideoEncoderVPL::InitVplEncode() {
    m_vplEncodeParams.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    m_vplEncodeParams.mfx.LowPower = MFX_CODINGOPTION_ON;
    m_vplEncodeParams.AsyncDepth = 1;
    m_vplEncodeParams.mfx.CodecId = m_vplCodec;
    m_vplEncodeParams.mfx.CodecProfile = m_vplCodecProfile;
    m_vplEncodeParams.mfx.TargetUsage = m_vplQualityPreset;
    m_vplEncodeParams.mfx.RateControlMethod = m_vplRateControlMode;
    // Start at the maximum bitrate: the encoder picks its level (and tier) from the bitrate at
    // Init, and Reset can't move to a higher level later (MFX_ERR_INCOMPATIBLE_VIDEO_PARAM).
    // Lowering the bitrate with Reset always works. The first Transmit call applies the real
    // bitrate.
    SetBitrate(0, true);
    // Low latency streaming: no B-frames, so every frame can be sent as soon as it is encoded.
    m_vplEncodeParams.mfx.GopRefDist = 1;
    m_vplEncodeParams.mfx.FrameInfo.FrameRateExtN = m_refreshRate;
    m_vplEncodeParams.mfx.FrameInfo.FrameRateExtD = 1;
    m_vplEncodeParams.mfx.FrameInfo.FourCC = m_vplColorFormat;
    m_vplEncodeParams.mfx.FrameInfo.ChromaFormat = m_vplChromaFormat;
    m_vplEncodeParams.mfx.FrameInfo.CropW = m_renderWidth;
    m_vplEncodeParams.mfx.FrameInfo.CropH = m_renderHeight;
    m_vplEncodeParams.mfx.FrameInfo.Width = ALIGN16(m_renderWidth);
    m_vplEncodeParams.mfx.FrameInfo.Height = ALIGN16(m_renderHeight);

    mfxStatus sts = MFXVideoENCODE_Query(m_vplSession, &m_vplEncodeParams, &m_vplEncodeParams);
    switch (sts) {
    case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM:
        VPL_WARN("incompatible video params, auto-correcting");
        break;

    case MFX_WRN_PARTIAL_ACCELERATION:
        VPL_WARN("partial acceleration");
        break;

    case MFX_ERR_UNSUPPORTED:
        ERROR_THROW("query unsupported");
    }

    // Query may have corrected the rate control fields. Use the corrected values as the limits
    // for later Reset calls, since Reset can't grow the buffers allocated at Init.
    const mfxInfoMFX& mfx = m_vplEncodeParams.mfx;
    m_brcMultiplier = mfx.BRCParamMultiplier == 0 ? 1 : mfx.BRCParamMultiplier;
    m_initMaxKbps = mfx.MaxKbps;
    m_initTargetKbps = mfx.TargetKbps;
    m_initBufferSizeInKB = mfx.BufferSizeInKB;
    VPL_INFO(
        "rate control: target %u kbps, max %u kbps, buffer %u KB, multiplier %u, level %u",
        static_cast<unsigned>(mfx.TargetKbps) * m_brcMultiplier,
        static_cast<unsigned>(mfx.MaxKbps) * m_brcMultiplier,
        static_cast<unsigned>(mfx.BufferSizeInKB) * m_brcMultiplier,
        static_cast<unsigned>(m_brcMultiplier),
        static_cast<unsigned>(mfx.CodecLevel)
    );

    InitEncoder();
}

void VideoEncoderVPL::InitEncoder() {
    // Positive status codes are warnings (e.g. MFX_WRN_INCOMPATIBLE_VIDEO_PARAM: parameters were
    // adjusted) and still mean success.
    mfxStatus sts = MFXVideoENCODE_Init(m_vplSession, &m_vplEncodeParams);
    if (sts < MFX_ERR_NONE) {
        ERROR_THROW("\"MFXVideoENCODE_Init\" failed with %d", sts);
    } else if (sts > MFX_ERR_NONE) {
        VPL_DEBUG("MFXVideoENCODE_Init returned warning %d", sts);
    }
}

bool VideoEncoderVPL::SetBitrate(uint64_t bitrateBps, bool forInit) {
    // TargetKbps, MaxKbps, BufferSizeInKB and InitialDelayInKB are 16-bit fields, so anything above
    // ~65 Mbps silently wraps around. VPL scales all of them by BRCParamMultiplier.
    //
    // The multiplier is derived from a fixed maximum bitrate so it never changes between
    // MFXVideoENCODE_Reset calls, and the encoder is initialized with rate control buffers sized
    // for that maximum. Reset can only shrink those buffers; growing them past the size used at
    // Init fails with MFX_ERR_INCOMPATIBLE_VIDEO_PARAM.
    constexpr uint64_t MAX_U16 = 0xFFFF;
    constexpr uint64_t MAX_BITRATE_BPS = 1'000'000'000; // Same limit as AMF
    const uint64_t MULTIPLIER
        = m_brcMultiplier != 0 ? m_brcMultiplier : (MAX_BITRATE_BPS / 1000 + MAX_U16 - 1) / MAX_U16;

    if (bitrateBps > MAX_BITRATE_BPS) {
        bitrateBps = MAX_BITRATE_BPS;
    }
    if (bitrateBps < 1000 * MULTIPLIER) {
        bitrateBps = 1000 * MULTIPLIER;
    }

    if (forInit) {
        bitrateBps = MAX_BITRATE_BPS;
    }

    auto toField = [&](uint64_t kiloUnits) -> mfxU16 {
        uint64_t scaled = kiloUnits / MULTIPLIER;
        if (scaled == 0) {
            scaled = 1;
        }
        return static_cast<mfxU16>(scaled > MAX_U16 ? MAX_U16 : scaled);
    };

    // Size the buffer for about one frame, like the AMF and NVENC encoders, for low latency.
    uint64_t bufferBps = forInit ? MAX_BITRATE_BPS : bitrateBps;
    uint64_t refreshRate = m_refreshRate > 0 ? static_cast<uint64_t>(m_refreshRate) : 60;
    uint64_t bufferKB = bufferBps / 8 / refreshRate * 11 / 10 / 1000;

    mfxInfoMFX& mfx = m_vplEncodeParams.mfx;
    mfxU16 targetKbps = toField(bitrateBps / 1000);
    // CBR uses MaxKbps = TargetKbps. VBR (and the Init call, so Reset never has to grow it) uses
    // the maximum.
    bool useMax = forInit || mfx.RateControlMethod == MFX_RATECONTROL_VBR;
    mfxU16 maxKbps = useMax ? toField(MAX_BITRATE_BPS / 1000) : targetKbps;
    mfxU16 bufferSizeInKB = toField(bufferKB);
    if (!forInit) {
        // Never go above the bitrate the encoder accepted at Init, it would need a higher level.
        if (m_initTargetKbps != 0 && targetKbps > m_initTargetKbps) {
            targetKbps = m_initTargetKbps;
            if (!useMax) {
                maxKbps = targetKbps;
            }
        }
        // Never grow past what the encoder accepted at Init. (For CBR, MaxKbps follows the
        // target, so it is not limited here.)
        if (useMax && m_initMaxKbps != 0 && maxKbps > m_initMaxKbps) {
            maxKbps = m_initMaxKbps;
        }
        if (m_initBufferSizeInKB != 0 && bufferSizeInKB > m_initBufferSizeInKB) {
            bufferSizeInKB = m_initBufferSizeInKB;
        }
    }

    bool changed = mfx.BRCParamMultiplier != MULTIPLIER || mfx.TargetKbps != targetKbps
        || mfx.MaxKbps != maxKbps || mfx.BufferSizeInKB != bufferSizeInKB;

    mfx.BRCParamMultiplier = static_cast<mfxU16>(MULTIPLIER);
    mfx.TargetKbps = targetKbps;
    mfx.MaxKbps = maxKbps;
    mfx.BufferSizeInKB = bufferSizeInKB;
    mfx.InitialDelayInKB = bufferSizeInKB;

    return changed;
}

mfxFrameSurface1* VideoEncoderVPL::VplImportTexture(ID3D11Texture2D* texture) {
    m_pD3DRender->GetContext()->CopyResource(m_transferTex.p, texture);

    mfxSurfaceD3D11Tex2D extSurfD3D11 = {};
    extSurfD3D11.SurfaceInterface.Header.SurfaceType = MFX_SURFACE_TYPE_D3D11_TEX2D;
    extSurfD3D11.SurfaceInterface.Header.SurfaceFlags
        = MFX_SURFACE_FLAG_IMPORT_SHARED | MFX_SURFACE_FLAG_IMPORT_COPY;
    extSurfD3D11.SurfaceInterface.Header.StructSize = sizeof(mfxSurfaceD3D11Tex2D);
    extSurfD3D11.texture2D = m_transferTex.p;

    mfxFrameSurface1* encSurface = nullptr;
    VPL_VERIFY(m_vplMemoryInterface->ImportFrameSurface(
        m_vplMemoryInterface,
        MFX_SURFACE_COMPONENT_ENCODE,
        &extSurfD3D11.SurfaceInterface.Header,
        &encSurface
    ));

    return encSurface;
}

void VideoEncoderVPL::ChooseParams() {
    const Settings* s = Settings_Instance();

    m_refreshRate = s->m_refreshRate;
    m_codec = s->m_codec;

    if (s->m_enableHdr) {
        if (s->m_use10bitEncoder) {
            m_dxColorFormat = DXGI_FORMAT_P010;
            m_vplColorFormat = MFX_FOURCC_P010;
            m_vplChromaFormat = MFX_CHROMAFORMAT_YUV420;
        } else {
            m_dxColorFormat = DXGI_FORMAT_NV12;
            m_vplColorFormat = MFX_FOURCC_NV12;
            m_vplChromaFormat = MFX_CHROMAFORMAT_YUV420;
        }
    } else {
        m_dxColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        m_vplColorFormat = MFX_FOURCC_BGR4;
        m_vplChromaFormat = MFX_CHROMAFORMAT_YUV444;
    }

    switch (m_codec) {
    case ALVR_CODEC_H264:
        m_vplCodec = MFX_CODEC_AVC;
        break;
    case ALVR_CODEC_HEVC:
        m_vplCodec = MFX_CODEC_HEVC;
        break;
    case ALVR_CODEC_AV1:
        m_vplCodec = MFX_CODEC_AV1;
        break;
    default:
        ERROR_THROW("unsupported video encoding %d", s->m_codec);
    }

    m_vplCodecProfile = MFX_PROFILE_UNKNOWN;
    if (m_codec == ALVR_CODEC_H264) {
        switch (s->m_h264Profile) {
        case ALVR_H264_PROFILE_BASELINE:
            m_vplCodecProfile = MFX_PROFILE_AVC_BASELINE;
            break;
        case ALVR_H264_PROFILE_MAIN:
            m_vplCodecProfile = MFX_PROFILE_AVC_MAIN;
            break;
        case ALVR_H264_PROFILE_HIGH:
            m_vplCodecProfile = MFX_PROFILE_AVC_HIGH;
            break;
        default:
            ERROR_THROW("unsupported h264 profile %d", s->m_h264Profile);
        }
    }

    if (s->m_use10bitEncoder) {
        switch (m_codec) {
        case ALVR_CODEC_H264:
            m_vplCodecProfile = MFX_PROFILE_AVC_HIGH10;
            break;
        case ALVR_CODEC_HEVC:
            m_vplCodecProfile = MFX_PROFILE_HEVC_MAIN10;
            break;
        }
    }

    switch (s->m_encoderQualityPreset) {
    case ALVR_QUALITY:
        m_vplQualityPreset = MFX_TARGETUSAGE_BEST_QUALITY;
        break;
    case ALVR_BALANCED:
        m_vplQualityPreset = MFX_TARGETUSAGE_BALANCED;
        break;
    case ALVR_SPEED:
        m_vplQualityPreset = MFX_TARGETUSAGE_BEST_SPEED;
        break;
    default:
        ERROR_THROW("invalid encoder quality preset");
    }

    switch (s->m_rateControlMode) {
    case ALVR_CBR:
        m_vplRateControlMode = MFX_RATECONTROL_CBR;
        break;
    case ALVR_VBR:
        m_vplRateControlMode = MFX_RATECONTROL_VBR;
        break;
    default:
        ERROR_THROW("invalid rate control mode");
    }
}

void VideoEncoderVPL::CheckVPLConfig() {
    mfxConfig cfg[5];
    mfxVariant cfgVal[5];

    // Implementation used must be the hardware implementation
    cfg[0] = MFXCreateConfig(m_vplLoader);
    VERIFY(cfg[0] != NULL, "MFXCreateConfig failed");
    cfgVal[0].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[0].Data.U32 = MFX_IMPL_TYPE_HARDWARE;
    VPL_VERIFY(MFXSetConfigFilterProperty(cfg[0], (mfxU8*)"mfxImplDescription.Impl", cfgVal[0]));

    // Implementation used must provide API version 2.9 or newer
    cfg[1] = MFXCreateConfig(m_vplLoader);
    VERIFY(NULL != cfg[1], "MFXCreateConfig failed")
    cfgVal[1].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[1].Data.U32 = VPLVERSION(2, 9);
    VPL_VERIFY(MFXSetConfigFilterProperty(
        cfg[1], (mfxU8*)"mfxImplDescription.ApiVersion.Version", cfgVal[1]
    ));

    // Implementation used must be D3D11 acceleration mode
    cfg[2] = MFXCreateConfig(m_vplLoader);
    VERIFY(NULL != cfg[2], "MFXCreateConfig failed")
    cfgVal[2].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[2].Data.U32 = MFX_ACCEL_MODE_VIA_D3D11;
    VPL_VERIFY(
        MFXSetConfigFilterProperty(cfg[2], (mfxU8*)"mfxImplDescription.AccelerationMode", cfgVal[2])
    );

    // Implementation used must be D3D11 surface sharing mode
    // Applying the 3 associated parameters (logical AND operation) using a single mfxConfig
    cfg[3] = MFXCreateConfig(m_vplLoader);
    VERIFY(NULL != cfg[3], "MFXCreateConfig failed")
    cfgVal[3].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[3].Data.U32 = MFX_SURFACE_TYPE_D3D11_TEX2D;
    VPL_VERIFY(MFXSetConfigFilterProperty(
        cfg[3], (mfxU8*)"mfxSurfaceTypesSupported.surftype.SurfaceType", cfgVal[3]
    ));

    cfgVal[3].Data.U32 = MFX_SURFACE_COMPONENT_ENCODE;
    VPL_VERIFY(MFXSetConfigFilterProperty(
        cfg[3], (mfxU8*)"mfxSurfaceTypesSupported.surftype.surfcomp.SurfaceComponent", cfgVal[3]
    ));

    cfgVal[3].Data.U32 = MFX_SURFACE_FLAG_IMPORT_COPY;
    VPL_VERIFY(MFXSetConfigFilterProperty(
        cfg[3], (mfxU8*)"mfxSurfaceTypesSupported.surftype.surfcomp.SurfaceFlags", cfgVal[3]
    ));

    // Implementation must provide correct codec
    cfg[4] = MFXCreateConfig(m_vplLoader);
    VERIFY(NULL != cfg[4], "MFXCreateConfig failed")
    cfgVal[4].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[4].Data.U32 = m_vplCodec;
    VPL_VERIFY(MFXSetConfigFilterProperty(
        cfg[4], (mfxU8*)"mfxImplDescription.mfxEncoderDescription.encoder.CodecID", cfgVal[4]
    ));
}

void VideoEncoderVPL::LogImplementationInfo() {
    mfxImplDescription* idesc = nullptr;
    mfxStatus sts;
    // Loads info about implementation at specified list location
    sts = MFXEnumImplementations(m_vplLoader, 0, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, (mfxHDL*)&idesc);
    if (!idesc || (sts != MFX_ERR_NONE))
        return;

    const char* accel_mode;
    switch (idesc->AccelerationMode) {
    case MFX_ACCEL_MODE_NA:
        accel_mode = "na";
        break;
    case MFX_ACCEL_MODE_VIA_D3D9:
        accel_mode = "d3d9";
        break;
    case MFX_ACCEL_MODE_VIA_D3D11:
        accel_mode = "d3d11";
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI:
        accel_mode = "vaapi";
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_DRM_MODESET:
        accel_mode = "vaapi_drm_modeset";
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_GLX:
        accel_mode = "vaapi_glx";
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_X11:
        accel_mode = "vaapi_x11";
        break;
    case MFX_ACCEL_MODE_VIA_VAAPI_WAYLAND:
        accel_mode = "vaapi_wayland";
        break;
    case MFX_ACCEL_MODE_VIA_HDDLUNITE:
        accel_mode = "hddlunite";
        break;
    default:
        accel_mode = "unknown";
        break;
    }

    VPL_INFO(
        "using api version %hu.%hu on device %s",
        idesc->ApiVersion.Major,
        idesc->ApiVersion.Minor,
        idesc->Dev.DeviceID
    );
    VPL_DEBUG("api version: %hu.%hu", idesc->ApiVersion.Major, idesc->ApiVersion.Minor);
    VPL_DEBUG("impl type: hw");
    VPL_DEBUG("accel mode: %s", accel_mode);
    VPL_DEBUG("device id: %s", idesc->Dev.DeviceID);
    MFXDispReleaseImplDescription(m_vplLoader, idesc);

#if (MFX_VERSION >= 2004)
    // Show implementation path, added in 2.4 API
    mfxHDL implPath = nullptr;
    sts = MFXEnumImplementations(m_vplLoader, 0, MFX_IMPLCAPS_IMPLPATH, &implPath);
    if (!implPath || (sts != MFX_ERR_NONE))
        return;

    VPL_DEBUG("path: %s", reinterpret_cast<mfxChar*>(implPath));
    MFXDispReleaseImplDescription(m_vplLoader, implPath);
#endif
}
