﻿// -----------------------------------------------------------------------------------------
//     VCEEnc by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2014-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// IABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#include <cmath>
#include <tchar.h>
#include <process.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib") 

#include <iostream>

#include "VCECore.h"
#include "VCEParam.h"
#include "VCEVersion.h"
#include "VCEInput.h"
#include "VCEInputRaw.h"
#include "VCEInputAvs.h"
#include "VCEInputVpy.h"
#include "avcodec_reader.h"
#include "EncoderParams.h"
#include "chapter_rw.h"

#include "VideoConverter.h"

const wchar_t* VCECore::PARAM_NAME_INPUT = L"INPUT";
const wchar_t* VCECore::PARAM_NAME_INPUT_WIDTH = L"WIDTH";
const wchar_t* VCECore::PARAM_NAME_INPUT_HEIGHT = L"HEIGHT";

const wchar_t* VCECore::PARAM_NAME_OUTPUT = L"OUTPUT";
const wchar_t* VCECore::PARAM_NAME_OUTPUT_WIDTH = L"OUTPUT_WIDTH";
const wchar_t* VCECore::PARAM_NAME_OUTPUT_HEIGHT = L"OUTPUT_HEIGHT";

const wchar_t* VCECore::PARAM_NAME_ENGINE = L"ENGINE";

const wchar_t* VCECore::PARAM_NAME_ADAPTERID = L"ADAPTERID";
const wchar_t* VCECore::PARAM_NAME_CAPABILITY = L"DISPLAYCAPABILITY";

#define ENCODER_SUBMIT_TIME     L"EncoderSubmitTime"  // private property to track submit tyme

static const amf::AMF_SURFACE_FORMAT formatOut = amf::AMF_SURFACE_NV12;

class VCECore::PipelineElementAMFComponent : public PipelineElement {
public:
    PipelineElementAMFComponent(amf::AMFComponentPtr pComponent) :
        m_pComponent(pComponent) {

    }

    virtual ~PipelineElementAMFComponent() {
    }

    virtual AMF_RESULT SubmitInput(amf::AMFData* pData) {
        AMF_RESULT res = AMF_OK;
        if (pData == NULL) // EOF
        {
            res = m_pComponent->Drain();
        } else {
            res = m_pComponent->SubmitInput(pData);
            if (res == AMF_DECODER_NO_FREE_SURFACES || res == AMF_INPUT_FULL) {
                return AMF_INPUT_FULL;
            }
        }
        return res;
    }

    virtual AMF_RESULT QueryOutput(amf::AMFData** ppData) {
        AMF_RESULT res = AMF_OK;
        amf::AMFDataPtr data;
        res = m_pComponent->QueryOutput(&data);
        if (res == AMF_REPEAT) {
            res = AMF_OK;
        }
        if (res == AMF_EOF && data == NULL) {
        }
        if (data != NULL) {
            *ppData = data.Detach();
        }
        return res;
    }
    virtual AMF_RESULT Drain() {
        return m_pComponent->Drain();
    }
protected:
    amf::AMFComponentPtr m_pComponent;
};

class VCECore::PipelineElementEncoder : public PipelineElementAMFComponent {
public:
    PipelineElementEncoder(amf::AMFComponentPtr pComponent,
        ParametersStorage* pParams, amf_int64 frameParameterFreq,
        amf_int64 dynamicParameterFreq) :
        PipelineElementAMFComponent(pComponent), m_pParams(pParams),
        m_framesSubmitted(0), m_framesQueried(0),
        m_frameParameterFreq(frameParameterFreq),
        m_dynamicParameterFreq(dynamicParameterFreq),
        m_maxLatencyTime(0), m_TotalLatencyTime(0),
        m_maxLatencyFrame(0), m_LastReadyFrameTime(0) {

    }

    virtual ~PipelineElementEncoder() {
    }

    virtual AMF_RESULT SubmitInput(amf::AMFData* pData) {
        AMF_RESULT res = AMF_OK;
        if (pData == NULL) // EOF
        {
            res = m_pComponent->Drain();
        } else {
            amf_int64 submitTime = 0;
            amf_int64 currentTime = amf_high_precision_clock();
            if (pData->GetProperty(ENCODER_SUBMIT_TIME, &submitTime) != AMF_OK) {
                pData->SetProperty(ENCODER_SUBMIT_TIME, currentTime);
            }
            if (m_frameParameterFreq != 0 && m_framesSubmitted != 0
                && (m_framesSubmitted % m_frameParameterFreq) == 0) { // apply frame-specific properties to the current frame
                PushParamsToPropertyStorage(m_pParams, ParamEncoderFrame, pData);
            }
            if (m_dynamicParameterFreq != 0 && m_framesSubmitted != 0
                && (m_framesSubmitted % m_dynamicParameterFreq)
                == 0) { // apply dynamic properties to the encoder
                PushParamsToPropertyStorage(m_pParams, ParamEncoderDynamic,
                    m_pComponent);
            }
            res = m_pComponent->SubmitInput(pData);
            if (res == AMF_DECODER_NO_FREE_SURFACES || res == AMF_INPUT_FULL) {
                return AMF_INPUT_FULL;
            }
            m_framesSubmitted++;
        }
        return res;
    }

    virtual AMF_RESULT QueryOutput(amf::AMFData** ppData) {
        AMF_RESULT ret = PipelineElementAMFComponent::QueryOutput(ppData);
        if (ret == AMF_OK && *ppData != NULL) {
            amf_int64 currentTime = amf_high_precision_clock();
            amf_int64 submitTime = 0;
            if ((*ppData)->GetProperty(ENCODER_SUBMIT_TIME, &submitTime)
                == AMF_OK) {
                amf_int64 latencyTime = currentTime - AMF_MAX(submitTime,
                    m_LastReadyFrameTime);
                if (m_maxLatencyTime < latencyTime) {
                    m_maxLatencyTime = latencyTime;
                    m_maxLatencyFrame = m_framesQueried;
                }
                m_TotalLatencyTime += latencyTime;
            }
            m_framesQueried++;
            m_LastReadyFrameTime = currentTime;
        }
        return ret;
    }
    virtual std::wstring GetDisplayResult() {
        std::wstring ret;
        if (m_framesSubmitted > 0) {
            std::wstringstream messageStream;
            messageStream.precision(1);
            messageStream.setf(std::ios::fixed, std::ios::floatfield);
            double averageLatency = double(m_TotalLatencyTime) / 10000.
                / m_framesQueried;
            double maxLatency = double(m_maxLatencyTime) / 10000.;
            messageStream << L" Average (Max, fr#) Encode Latency: "
                << averageLatency << L" ms (" << maxLatency
                << " ms frame# " << m_maxLatencyFrame << L")";
            ret = messageStream.str();
        }
        return ret;
    }
protected:
    ParametersStorage* m_pParams;
    amf_int m_framesSubmitted;
    amf_int m_framesQueried;
    amf_int64 m_frameParameterFreq;
    amf_int64 m_dynamicParameterFreq;
    amf_int64 m_maxLatencyTime;
    amf_int64 m_TotalLatencyTime;
    amf_int64 m_LastReadyFrameTime;
    amf_int m_maxLatencyFrame;
};

std::wstring VCECore::AccelTypeToString(amf::AMF_ACCELERATION_TYPE accelType) {
    std::wstring strValue;
    switch (accelType) {
    case amf::AMF_ACCEL_NOT_SUPPORTED:
        strValue = L"Not supported";
        break;
    case amf::AMF_ACCEL_HARDWARE:
        strValue = L"Hardware-accelerated";
        break;
    case amf::AMF_ACCEL_GPU:
        strValue = L"GPU-accelerated";
        break;
    case amf::AMF_ACCEL_SOFTWARE:
        strValue = L"Not accelerated (software)";
        break;
    }
    return strValue;
}

bool VCECore::QueryIOCaps(amf::AMFIOCapsPtr& ioCaps) {
    bool result = true;
    if (ioCaps != NULL) {
        amf_int32 minWidth, maxWidth;
        ioCaps->GetWidthRange(&minWidth, &maxWidth);
        std::wcout << L"\t\t\tWidth: [" << minWidth << L"-" << maxWidth << L"]\n";

        amf_int32 minHeight, maxHeight;
        ioCaps->GetHeightRange(&minHeight, &maxHeight);
        std::wcout << L"\t\t\tHeight: [" << minHeight << L"-" << maxHeight << L"]\n";

        amf_int32 vertAlign = ioCaps->GetVertAlign();
        std::wcout << L"\t\t\tVertical alignment: " << vertAlign << L" lines.\n";

        amf_bool interlacedSupport = ioCaps->IsInterlacedSupported();
        std::wcout << L"\t\t\tInterlaced support: " << (interlacedSupport ? L"YES" : L"NO") << L"\n";

        amf_int32 numOfFormats = ioCaps->GetNumOfFormats();
        std::wcout << L"\t\t\tTotal of " << numOfFormats << L" pixel format(s) supported:\n";
        for (amf_int32 i = 0; i < numOfFormats; i++) {
            amf::AMF_SURFACE_FORMAT format;
            amf_bool native = false;
            if (ioCaps->GetFormatAt(i, &format, &native) == AMF_OK) {
                std::wcout << L"\t\t\t\t" << i << L": " << amf::AMFSurfaceGetFormatName(format) << L" " << (native ? L"(native)" : L"") << L"\n";
            } else {
                result = false;
                break;
            }
        }

        if (result == true) {
            amf_int32 numOfMemTypes = ioCaps->GetNumOfMemoryTypes();
            std::wcout << L"\t\t\tTotal of " << numOfMemTypes << L" memory type(s) supported:\n";
            for (amf_int32 i = 0; i < numOfMemTypes; i++) {
                amf::AMF_MEMORY_TYPE memType;
                amf_bool native = false;
                if (ioCaps->GetMemoryTypeAt(i, &memType, &native) == AMF_OK) {
                    std::wcout << L"\t\t\t\t" << i << L": " << amf::AMFGetMemoryTypeName(memType) << L" " << (native ? L"(native)" : L"") << L"\n";
                }
            }
        }
    } else {
        std::wcerr << L"ERROR: ioCaps == NULL\n";
        result = false;
    }
    return result;
}

bool VCECore::QueryEncoderForCodec(const wchar_t *componentID, amf::AMFCapabilityManagerPtr& capsManager) {
    std::wcout << L"\tCodec " << componentID << L"\n";
    amf::AMFEncoderCapsPtr encoderCaps;
    bool result = false;
    if (capsManager->GetEncoderCaps(componentID, &encoderCaps) == AMF_OK) {
        amf::AMF_ACCELERATION_TYPE accelType = encoderCaps->GetAccelerationType();
        std::wcout << L"\t\tAcceleration Type:" << AccelTypeToString(accelType) << L"\n";

        amf::H264EncoderCapsPtr encoderH264Caps = (amf::H264EncoderCapsPtr)encoderCaps;

        amf_uint32 numProfiles = encoderH264Caps->GetNumOfSupportedProfiles();
        amf_uint32 numLevels = encoderH264Caps->GetNumOfSupportedLevels();
        std::wcout << L"\t\tnumber of supported profiles:" <<numProfiles << L"\n";

        for (amf_uint32 i = 0; i < numProfiles; i++) {
            std::wcout << L"\t\t\t" << encoderH264Caps->GetProfile(i) << L"\n";

        }
        std::wcout << L"\t\tnumber of supported levels:" << numLevels << L"\n";

        for (amf_uint32 i = 0; i < numLevels; i++) {
            std::wcout << L"\t\t\t" << encoderH264Caps->GetLevel(i) << L"\n";

        }

        std::wcout << L"\t\tnumber of supported Rate Control Metheds:" << encoderH264Caps->GetNumOfRateControlMethods() << L"\n";

        for (amf_int32 i = 0; i < encoderH264Caps->GetNumOfRateControlMethods(); i++) {
            std::wcout << L"\t\t\t" << encoderH264Caps->GetRateControlMethod(i) << L"\n";

        }

        std::wcout << L"\t\tNumber of temporal Layers:" << encoderH264Caps->GetMaxNumOfTemporalLayers() << L"\n";
        std::wcout << L"\t\tMax Supported Job Priority:" << encoderH264Caps->GetMaxSupportedJobPriority() << L"\n";
        std::wcout << L"\t\tIsBPictureSupported:" << encoderH264Caps->IsBPictureSupported() << L"\n\n";
        std::wcout << L"\t\tMax Number of streams supported:" << encoderH264Caps->GetMaxNumOfStreams() << L"\n";
        std::wcout << L"\t\tEncoder input:\n";
        amf::AMFIOCapsPtr inputCaps;
        if (encoderCaps->GetInputCaps(&inputCaps) == AMF_OK) {
            result = QueryIOCaps(inputCaps);
        }

        std::wcout << L"\t\tEncoder output:\n";
        amf::AMFIOCapsPtr outputCaps;
        if (encoderCaps->GetOutputCaps(&outputCaps) == AMF_OK) {
            result = QueryIOCaps(outputCaps);
        }
        return true;
    } else {
        std::wcout << AccelTypeToString(amf::AMF_ACCEL_NOT_SUPPORTED) << L"\n";
        return false;
    }
}

bool VCECore::QueryEncoderCaps(amf::AMFCapabilityManagerPtr& capsManager) {
    std::wcout << L"Querying video encoder capabilities...\n";

    return  QueryEncoderForCodec(AMFVideoEncoderVCE_AVC, capsManager);
}

void VCECore::PrintMes(int log_level, const TCHAR *format, ...) {
    if (m_pVCELog.get() == nullptr || log_level < m_pVCELog->getLogLevel()) {
        return;
    }

    va_list args;
    va_start(args, format);

    int len = _vsctprintf(format, args) + 1; // _vscprintf doesn't count terminating '\0'
    vector<TCHAR> buffer(len, 0);
    _vstprintf_s(buffer.data(), len, format, args);
    va_end(args);

    m_pVCELog->write(log_level, buffer.data());
}





VCECore::VCECore() :
    m_pVCELog(),
    m_bTimerPeriodTuning(true),
    m_pFileReader(),
    m_pOutput(),
    m_pStatus(),
    m_inputInfo(),
    m_pContext(),
    m_pStreamOut(),
    m_pTrimParam(nullptr),
    m_pDecoder(),
    m_pEncoder(),
    m_pConverter(),
    m_deviceDX9(),
    m_deviceDX11(),
    m_Params() {
}

VCECore::~VCECore() {
    Terminate();
}

void VCECore::Terminate() {
    if (m_bTimerPeriodTuning) {
        timeEndPeriod(1);
        PrintMes(VCE_LOG_DEBUG, _T("timeEndPeriod(1)\n"));
        m_bTimerPeriodTuning = false;
    }
    PrintMes(VCE_LOG_DEBUG, _T("Stopping pipeline...\n"));
    Pipeline::Stop();
    PrintMes(VCE_LOG_DEBUG, _T("Pipeline Stopped.\n"));

    m_pStreamOut = NULL;

    m_pTrimParam = nullptr;

    if (m_pEncoder != nullptr) {
        m_pEncoder->Terminate();
        m_pEncoder = nullptr;
    }

    if (m_pConverter != nullptr) {
        m_pConverter->Terminate();
        m_pConverter = nullptr;
    }

    if (m_pDecoder != nullptr) {
        m_pDecoder->Terminate();
        m_pDecoder = nullptr;
    }

    if (m_pContext != nullptr) {
        m_pContext->Terminate();
        m_pContext = nullptr;
    }

    m_deviceDX9.Terminate();
    m_deviceDX11.Terminate();

    m_pFileReader.reset();
    m_pOutput.reset();
    m_pStatus.reset();
    m_pVCELog.reset();
}

AMF_RESULT VCECore::readChapterFile(tstring chapfile) {
#if ENABLE_AVCODEC_VCE_READER
    ChapterRW chapter;
    auto err = chapter.read_file(chapfile.c_str(), CODE_PAGE_UNSET, 0.0);
    if (err != AUO_CHAP_ERR_NONE) {
        PrintMes(VCE_LOG_ERROR, _T("failed to %s chapter file: \"%s\".\n"), (err == AUO_CHAP_ERR_FILE_OPEN) ? _T("open") : _T("read"), chapfile.c_str());
        return AMF_FAIL;
    }
    if (chapter.chapterlist().size() == 0) {
        PrintMes(VCE_LOG_ERROR, _T("no chapter found from chapter file: \"%s\".\n"), chapfile.c_str());
        return AMF_FAIL;
    }
    m_AVChapterFromFile.clear();
    const auto& chapter_list = chapter.chapterlist();
    tstring chap_log;
    for (size_t i = 0; i < chapter_list.size(); i++) {
        unique_ptr<AVChapter> avchap(new AVChapter);
        avchap->time_base = av_make_q(1, 1000);
        avchap->start = chapter_list[i]->get_ms();
        avchap->end = (i < chapter_list.size()-1) ? chapter_list[i+1]->get_ms() : avchap->start + 1;
        avchap->id = (int)m_AVChapterFromFile.size();
        avchap->metadata = nullptr;
        av_dict_set(&avchap->metadata, "title", wstring_to_string(chapter_list[i]->name, CP_UTF8).c_str(), 0);
        chap_log += strsprintf(_T("chapter #%02d [%d.%02d.%02d.%03d]: %s.\n"),
            avchap->id, chapter_list[i]->h, chapter_list[i]->m, chapter_list[i]->s, chapter_list[i]->ms,
            wstring_to_tstring(chapter_list[i]->name).c_str());
        m_AVChapterFromFile.push_back(std::move(avchap));
    }
    PrintMes(VCE_LOG_DEBUG, _T("%s"), chap_log.c_str());
    return AMF_OK;
#else
    PrintMes(QSV_LOG_ERROR, _T("chater reading unsupportted in this build"));
    return AMF_NOT_SUPPORTED;
#endif //#if ENABLE_AVCODEC_VCE_READER
}

AMF_RESULT VCECore::initInput(VCEParam *pParams, const VCEInputInfo *pInputInfo) {
#if !VCE_AUO
    m_pVCELog.reset(new VCELog(pParams->pStrLog, pParams->nLogLevel));
    if (!m_pStatus) {
        m_pStatus = std::make_shared<VCEStatus>();
    }

    int sourceAudioTrackIdStart = 1;    //トラック番号は1スタート
    int sourceSubtitleTrackIdStart = 1; //トラック番号は1スタート
    if (pParams->nInputType == VCE_INPUT_NONE) {
        if (check_ext(pParams->pInputFile, { ".y4m" })) {
            pParams->nInputType = VCE_INPUT_Y4M;
#if ENABLE_AVISYNTH_READER
        } else if (check_ext(pParams->pInputFile, { ".avs" })) {
            pParams->nInputType = VCE_INPUT_AVS;
#endif
#if ENABLE_VAPOURSYNTH_READER
        } else if (check_ext(pParams->pInputFile, { ".vpy" })) {
            pParams->nInputType = VCE_INPUT_VPY;
#endif
#if ENABLE_AVCODEC_VCE_READER
        } else if (usingAVProtocols(tchar_to_string(pParams->pInputFile, CP_UTF8), 0)
            || check_ext(pParams->pInputFile, { ".mp4", ".m4v", ".mkv", ".mov",
                    ".mts", ".m2ts", ".ts", ".264", ".h264", ".x264", ".avc", ".avc1",
                    ".265", ".h265", ".hevc",
                    ".mpg", ".mpeg", "m2v", ".vob", ".vro", ".flv", ".ogm",
                    ".webm", ".vp8", ".vp9",
                    ".wmv" })) {
            pParams->nInputType = VCE_INPUT_AVCODEC_VCE;
#endif //ENABLE_AVCODEC_VCE_READER
        } else {
            pParams->nInputType = VCE_INPUT_RAW;
        }
    }

    VCEInputRawParam rawParam = { 0 };
#if ENABLE_AVISYNTH_READER
    VCEInputAvsParam avsParam = { 0 };
#endif
#if ENABLE_VAPOURSYNTH_READER
    VCEInputVpyParam vpyParam = { 0 };
#endif
#if ENABLE_AVCODEC_VCE_READER
    AvcodecReaderPrm avcodecReaderPrm = { 0 };
#endif
    if (pParams->nInputType == VCE_INPUT_Y4M || pParams->nInputType == VCE_INPUT_RAW) {
        rawParam.y4m = pParams->nInputType == VCE_INPUT_Y4M;
        rawParam.srcFile = pParams->pInputFile;
        m_inputInfo.pPrivateParam = &rawParam;
        m_pFileReader.reset(new VCEInputRaw());
#if ENABLE_AVISYNTH_READER
    } else if (pParams->nInputType == VCE_INPUT_AVS) {
        avsParam.srcFile = pParams->pInputFile;
        m_inputInfo.pPrivateParam = &avsParam;
        m_pFileReader.reset(new VCEInputAvs());
#endif
#if ENABLE_VAPOURSYNTH_READER
    } else if (pParams->nInputType == VCE_INPUT_VPY || pParams->nInputType == VCE_INPUT_VPY_MT) {
        vpyParam.srcFile = pParams->pInputFile;
        vpyParam.bVpyMt = pParams->nInputType == VCE_INPUT_VPY_MT;
        m_inputInfo.pPrivateParam = &vpyParam;
        m_pFileReader.reset(new VCEInputVpy());
#endif
#if ENABLE_AVCODEC_VCE_READER
    } else if (pParams->nInputType == VCE_INPUT_AVCODEC_VCE) {
        avcodecReaderPrm.srcFile = pParams->pInputFile;
        avcodecReaderPrm.bReadVideo = true;
        avcodecReaderPrm.nVideoTrack = (int8_t)pParams->nVideoTrack;
        avcodecReaderPrm.nVideoStreamId = pParams->nVideoStreamId;
        avcodecReaderPrm.bReadChapter = !!pParams->bCopyChapter;
        avcodecReaderPrm.bReadSubtitle = !!pParams->nSubtitleSelectCount;
        avcodecReaderPrm.pTrimList = pParams->pTrimList;
        avcodecReaderPrm.nTrimCount = (uint16_t)pParams->nTrimCount;
        avcodecReaderPrm.nReadAudio |= pParams->nAudioSelectCount > 0; 
        avcodecReaderPrm.nAnalyzeSec = (uint16_t)pParams->nAVDemuxAnalyzeSec;
        avcodecReaderPrm.nVideoAvgFramerate = std::make_pair(pInputInfo->fps.num, pInputInfo->fps.den);
        avcodecReaderPrm.nAudioTrackStart = (int)sourceAudioTrackIdStart;
        avcodecReaderPrm.ppAudioSelect = pParams->ppAudioSelectList;
        avcodecReaderPrm.nAudioSelectCount = pParams->nAudioSelectCount;
        //avcodecReaderPrm.bReadSubtitle = prm->nSubtitleSelectCount + prm->vpp.subburn.nTrack > 0;
        //avcodecReaderPrm.pSubtitleSelect = (prm->vpp.subburn.nTrack) ? &prm->vpp.subburn.nTrack : prm->pSubtitleSelect;
        //avcodecReaderPrm.nSubtitleSelectCount = (prm->vpp.subburn.nTrack) ? 1 : prm->nSubtitleSelectCount;
        avcodecReaderPrm.pSubtitleSelect = pParams->pSubtitleSelect;
        avcodecReaderPrm.nSubtitleSelectCount = pParams->nSubtitleSelectCount;
        avcodecReaderPrm.nProcSpeedLimit = pParams->nProcSpeedLimit;
        avcodecReaderPrm.fSeekSec = pParams->fSeekSec;
        avcodecReaderPrm.pFramePosListLog = pParams->pFramePosListLog;
        avcodecReaderPrm.nInputThread = (int8_t)pParams->nInputThread;
        avcodecReaderPrm.bAudioIgnoreNoTrackError = (int8_t)pParams->bAudioIgnoreNoTrackError;
        avcodecReaderPrm.pQueueInfo = nullptr;
        m_inputInfo.pPrivateParam = &avcodecReaderPrm;
        m_pFileReader.reset(new CAvcodecReader());
        PrintMes(VCE_LOG_DEBUG, _T("Input: avqsv reader selected.\n"));
#endif
    } else {
        PrintMes(VCE_LOG_ERROR, _T("Unknown reader selected\n"));
        return AMF_NOT_SUPPORTED;
    }
    auto ret = m_pFileReader->init(m_pVCELog, m_pStatus, &m_inputInfo, m_pContext);
    if (ret != AMF_OK) {
        PrintMes(VCE_LOG_ERROR, _T("Error: %s\n"), m_pFileReader->getMessage().c_str());
        return ret;
    }
    PrintMes(VCE_LOG_DEBUG, _T("Input: reader initialization successful.\n"));
    sourceAudioTrackIdStart    += m_pFileReader->GetAudioTrackCount();
    sourceSubtitleTrackIdStart += m_pFileReader->GetSubtitleTrackCount();

#if 0
    if (pParams->nAudioSourceCount && pParams->ppAudioSourceList) {
        mfxFrameInfo videoInfo = { 0 };
        m_pFileReader->GetInputFrameInfo(&videoInfo);

        for (int i = 0; i < pParams->nAudioSourceCount; i++) {
            AvcodecReaderPrm avcodecReaderPrm = { 0 };
            avcodecReaderPrm.memType = pParams->memType;
            avcodecReaderPrm.bReadVideo = false;
            avcodecReaderPrm.nReadAudio |= pParams->nAudioSelectCount > 0;
            avcodecReaderPrm.nAnalyzeSec = pParams->nAVDemuxAnalyzeSec;
            avcodecReaderPrm.pTrimList = pParams->pTrimList;
            avcodecReaderPrm.nTrimCount = pParams->nTrimCount;
            avcodecReaderPrm.nVideoAvgFramerate = std::make_pair(videoInfo.FrameRateExtN, videoInfo.FrameRateExtD);
            avcodecReaderPrm.nAudioTrackStart = sourceAudioTrackIdStart;
            avcodecReaderPrm.nSubtitleTrackStart = sourceSubtitleTrackIdStart;
            avcodecReaderPrm.ppAudioSelect = pParams->ppAudioSelectList;
            avcodecReaderPrm.nAudioSelectCount = pParams->nAudioSelectCount;
            avcodecReaderPrm.nProcSpeedLimit = pParams->nProcSpeedLimit;
            avcodecReaderPrm.fSeekSec = pParams->fSeekSec;
            avcodecReaderPrm.bAudioIgnoreNoTrackError = pParams->bAudioIgnoreNoTrackError;
            avcodecReaderPrm.nInputThread = 0;
            avcodecReaderPrm.pQueueInfo = nullptr;

            unique_ptr<CQSVInput> audioReader(new CAvcodecReader());
            audioReader->SetQSVLogPtr(m_pQSVLog);
            sts = audioReader->Init(pParams->ppAudioSourceList[i], 0, &avcodecReaderPrm, nullptr, nullptr, nullptr);
            if (sts < MFX_ERR_NONE) {
                PrintMes(QSV_LOG_ERROR, audioReader->GetInputMessage());
                return sts;
            }
            sourceAudioTrackIdStart += audioReader->GetAudioTrackCount();
            sourceSubtitleTrackIdStart += audioReader->GetSubtitleTrackCount();
            m_AudioReaders.push_back(std::move(audioReader));
        }
    }
#endif

    if (!m_pFileReader->getInputCodec()
        && pParams->pTrimList && pParams->nTrimCount > 0) {
        //avqsvリーダー以外は、trimは自分ではセットされないので、ここでセットする
        sTrimParam trimParam;
        trimParam.list = make_vector(pParams->pTrimList, pParams->nTrimCount);
        trimParam.offset = 0;
        m_pFileReader->SetTrimParam(trimParam);
    }
    //trim情報をリーダーから取得する
    auto trimParam = m_pFileReader->GetTrimParam();
    m_pTrimParam = (trimParam->list.size()) ? trimParam : nullptr;
    if (m_pTrimParam) {
        PrintMes(VCE_LOG_DEBUG, _T("Input: trim options\n"));
        for (int i = 0; i < (int)m_pTrimParam->list.size(); i++) {
            PrintMes(VCE_LOG_DEBUG, _T("%d-%d "), m_pTrimParam->list[i].start, m_pTrimParam->list[i].fin);
        }
        PrintMes(VCE_LOG_DEBUG, _T(" (offset: %d)\n"), m_pTrimParam->offset);
    }
#endif //#if !VCE_AUO
    return AMF_OK;
}

AMF_RESULT VCECore::checkParam(VCEParam *prm) {
    auto srcInfo = m_pFileReader->GetInputFrameInfo();
    if (m_inputInfo.fps.num <= 0 || m_inputInfo.fps.den <= 0) {
        m_inputInfo.fps = srcInfo.fps;
    }
    if (srcInfo.srcWidth) {
        m_inputInfo.srcWidth = srcInfo.srcWidth;
    }
    if (srcInfo.srcHeight) {
        m_inputInfo.srcHeight = srcInfo.srcHeight;
    }
    if (srcInfo.frames) {
        m_inputInfo.frames = srcInfo.frames;
    }
    if (srcInfo.format) {
        m_inputInfo.format = srcInfo.format;
    }

    if (m_inputInfo.fps.num <= 0 || m_inputInfo.fps.den <= 0) {
        PrintMes(VCE_LOG_ERROR, _T("Invalid fps - zero or negative (%d/%d).\n"), m_inputInfo.fps.num, m_inputInfo.fps.den);
        return AMF_FAIL;
    }
    {
        int fps_gcd = vce_gcd(m_inputInfo.fps.num, m_inputInfo.fps.den);
        m_inputInfo.fps.num /= fps_gcd;
        m_inputInfo.fps.den /= fps_gcd;
    }
    if (m_inputInfo.srcWidth <= 0 || m_inputInfo.srcHeight <= 0) {
        PrintMes(VCE_LOG_ERROR, _T("Invalid frame size - zero or negative (%dx%d).\n"), m_inputInfo.srcWidth, m_inputInfo.srcHeight);
        return AMF_FAIL;
    }
    int h_mul = is_interlaced(prm) ? 4 : 2;
    if (m_inputInfo.srcWidth % 2 != 0) {
        PrintMes(VCE_LOG_ERROR, _T("Invalid input frame size - non mod2 (width: %d).\n"), m_inputInfo.srcWidth);
        return AMF_FAIL;
    }
    if (m_inputInfo.srcHeight % h_mul != 0) {
        PrintMes(VCE_LOG_ERROR, _T("Invalid input frame size - non mod%d (height: %d).\n"), h_mul, m_inputInfo.srcHeight);
        return AMF_FAIL;
    }
    if (m_inputInfo.srcWidth < (m_inputInfo.crop.left + m_inputInfo.crop.right)
        || m_inputInfo.srcHeight < (m_inputInfo.crop.bottom + m_inputInfo.crop.up)) {
        PrintMes(VCE_LOG_ERROR, _T("crop size is too big.\n"));
        return AMF_FAIL;
    }
    m_inputInfo.srcWidth -= (m_inputInfo.crop.left + m_inputInfo.crop.right);
    m_inputInfo.srcHeight -= (m_inputInfo.crop.bottom + m_inputInfo.crop.up);
    if (m_inputInfo.srcWidth % 2 != 0) {
        PrintMes(VCE_LOG_ERROR, _T("Invalid input frame size (after crop) - non mod2 (width: %d).\n"), m_inputInfo.srcWidth);
        return AMF_FAIL;
    }
    if (m_inputInfo.srcHeight % h_mul != 0) {
        PrintMes(VCE_LOG_ERROR, _T("Invalid input frame size (after crop) - non mod%d (height: %d).\n"), h_mul, m_inputInfo.srcHeight);
        return AMF_FAIL;
    }
    if (m_inputInfo.dstWidth <= 0) {
        m_inputInfo.dstWidth = m_inputInfo.srcWidth;
    }
    if (m_inputInfo.dstHeight <= 0) {
        m_inputInfo.dstHeight = m_inputInfo.srcHeight;
    }
    if (m_inputInfo.dstWidth % 2 != 0) {
        PrintMes(VCE_LOG_ERROR, _T("Invalid output frame size - non mod2 (width: %d).\n"), m_inputInfo.dstWidth);
        return AMF_FAIL;
    }
    if (m_inputInfo.dstHeight % h_mul != 0) {
        PrintMes(VCE_LOG_ERROR, _T("Invalid output frame size - non mod%d (height: %d).\n"), h_mul, m_inputInfo.dstHeight);
        return AMF_FAIL;
    }
    if (prm->nBframes > VCE_MAX_BFRAMES) {
        PrintMes(VCE_LOG_WARN, _T("Maximum consecutive B frames is %d.\n"), VCE_MAX_BFRAMES);
        prm->nBframes = VCE_MAX_BFRAMES;
    }
    if (prm->nBitrate > VCE_MAX_BITRATE) {
        PrintMes(VCE_LOG_WARN, _T("Maximum bitrate is %d.\n"), VCE_MAX_BITRATE);
        prm->nBitrate = VCE_MAX_BITRATE;
    }
    if (prm->nMaxBitrate > VCE_MAX_BITRATE) {
        PrintMes(VCE_LOG_WARN, _T("Maximum max bitrate is %d.\n"), VCE_MAX_BITRATE);
        prm->nMaxBitrate = VCE_MAX_BITRATE;
    }
    if (prm->nVBVBufferSize > VCE_MAX_BITRATE) {
        PrintMes(VCE_LOG_WARN, _T("Maximum vbv buffer size is %d.\n"), VCE_MAX_BITRATE);
        prm->nVBVBufferSize = VCE_MAX_BITRATE;
    }
    if (prm->nGOPLen > VCE_MAX_GOP_LEN) {
        PrintMes(VCE_LOG_WARN, _T("Maximum GOP len is %d.\n"), VCE_MAX_GOP_LEN);
        prm->nGOPLen = VCE_MAX_GOP_LEN;
    }
    if (abs(prm->nDeltaQPBFrame) > VCE_MAX_B_DELTA_QP) {
        PrintMes(VCE_LOG_WARN, _T("Maximum Delta QP for Bframes is %d.\n"), VCE_MAX_B_DELTA_QP);
        prm->nDeltaQPBFrame = clamp(prm->nDeltaQPBFrame, -1 * VCE_MAX_B_DELTA_QP, VCE_MAX_B_DELTA_QP);
    }
    if (abs(prm->nDeltaQPBFrameRef) > VCE_MAX_B_DELTA_QP) {
        PrintMes(VCE_LOG_WARN, _T("Maximum Delta QP for Bframes is %d.\n"), VCE_MAX_B_DELTA_QP);
        prm->nDeltaQPBFrameRef = clamp(prm->nDeltaQPBFrameRef, -1 * VCE_MAX_B_DELTA_QP, VCE_MAX_B_DELTA_QP);
    }
    prm->nQPMax = clamp(prm->nQPMax, 0, 51);
    prm->nQPMin = clamp(prm->nQPMin, 0, 51);
    prm->nQPI   = clamp(prm->nQPI,   0, 51);
    prm->nQPP   = clamp(prm->nQPP,   0, 51);
    prm->nQPB   = clamp(prm->nQPB,   0, 51);

    return AMF_OK;
}

AMF_RESULT VCECore::initOutput(VCEParam *prm) {
    m_pStatus->init(m_pVCELog, m_inputInfo.fps, m_inputInfo.frames);

    m_pOutput.reset(new VCEOutput());

    auto ret = m_pOutput->Init(prm->pOutputFile, m_pVCELog, m_pStatus);
    if (ret != AMF_OK) {
        PrintMes(VCE_LOG_ERROR, _T("Error: %s\n"), m_pOutput->GetOutputMessage().c_str());
        return ret;
    }
    return ret;
}

AMF_RESULT VCECore::initDevice(VCEParam *prm) {
    AMF_RESULT res = AMF_OK;

    if (prm->memoryTypeIn == amf::AMF_MEMORY_DX9) {
        if (AMF_OK != (res = m_deviceDX9.Init(true, prm->nAdapterId, false, m_inputInfo.srcWidth, m_inputInfo.srcHeight))) {
            PrintMes(VCE_LOG_ERROR, _T("Failed to initialize DX9 device.\n"));
            return AMF_FAIL;
        }
        PrintMes(VCE_LOG_DEBUG, _T("initialized DX9 device.\n"));
        if (AMF_OK != (res = m_pContext->InitDX9(m_deviceDX9.GetDevice()))) {
            PrintMes(VCE_LOG_ERROR, _T("Failed to InitDX9.\n"));
            return AMF_FAIL;
        }
        PrintMes(VCE_LOG_DEBUG, _T("initialized context for DX9.\n"));
    } else if (prm->memoryTypeIn == amf::AMF_MEMORY_DX11) {
        if (AMF_OK != (res = m_deviceDX11.Init(prm->nAdapterId, false))) {
            PrintMes(VCE_LOG_ERROR, _T("Failed to initialize DX11 device.\n"));
            return AMF_FAIL;
        }
        PrintMes(VCE_LOG_DEBUG, _T("initialized DX11 device.\n"));
        if (AMF_OK != (res = m_pContext->InitDX11(m_deviceDX11.GetDevice()))) {
            PrintMes(VCE_LOG_ERROR, _T("Failed to InitDX11.\n"));
            return AMF_FAIL;
        }
        PrintMes(VCE_LOG_DEBUG, _T("initialized context for DX11.\n"));
    } else {
        PrintMes(VCE_LOG_ERROR, _T("Invalid memory type.\n"));
        return AMF_FAIL;
    }
    return res;
}

AMF_RESULT VCECore::initDecoder(VCEParam *prm) {
#if ENABLE_AVCODEC_VCE_READER
    auto inputCodec = m_pFileReader->getInputCodec();
    if (inputCodec == VCE_CODEC_NONE) {
        return AMF_OK;
    }
    if (VCE_CODEC_UVD_NAME.find(inputCodec) == VCE_CODEC_UVD_NAME.end()) {
        PrintMes(VCE_LOG_ERROR, _T("Input codec \"%s\" not supported.\n"), CodecIdToStr(inputCodec));
        return AMF_NOT_SUPPORTED;
    }
    const auto codec_uvd_name = VCE_CODEC_UVD_NAME.at(inputCodec);
    auto res = AMFCreateComponent(m_pContext, codec_uvd_name, &m_pDecoder);
    if (res != AMF_OK) {
        PrintMes(VCE_LOG_ERROR, _T("Failed to create decoder context: %d\n"), res);
        return AMF_FAIL;
    }

    // our sample H264 parser provides decode order timestamps - change this depend on demuxer
    if (AMF_OK != (res = m_pDecoder->SetProperty(AMF_TIMESTAMP_MODE, amf_int64(AMF_TS_DECODE)))) {
        PrintMes(VCE_LOG_ERROR, _T("Failed to set deocder: %d\n"), res);
        return AMF_FAIL;
    }
    sBitstream header = { 0 };
    if (AMF_OK != (res = m_pFileReader->GetHeader(&header))) {
        PrintMes(VCE_LOG_ERROR, _T("Failed to get video header: %d\n"), res);
        return AMF_FAIL;
    }
    amf::AMFBufferPtr buffer;
    m_pContext->AllocBuffer(amf::AMF_MEMORY_HOST, header.DataLength, &buffer);

    memcpy(buffer->GetNative(), header.Data, header.DataLength);
    m_pDecoder->SetProperty(AMF_VIDEO_DECODER_EXTRADATA, amf::AMFVariant(buffer));

    if (AMF_OK != (res = m_pDecoder->Init(formatOut, m_inputInfo.srcWidth, m_inputInfo.srcHeight))) {
        PrintMes(VCE_LOG_ERROR, _T("Failed to init decoder: %d\n"), res);
        return res;
    }
    PrintMes(VCE_LOG_DEBUG, _T("Initialized decoder\n"), res);
    return res;
#else
    return AMF_NOT_SUPPORTED;
#endif
}

AMF_RESULT VCECore::initConverter(VCEParam *prm) {
#if ENABLE_AVCODEC_VCE_READER
    if (m_pFileReader->getInputCodec() == VCE_CODEC_NONE) {
        return AMF_OK;
    }
    if (m_inputInfo.dstWidth != m_inputInfo.srcWidth || m_inputInfo.dstHeight != m_inputInfo.srcHeight) {
        return AMF_OK;
    }
    auto res = AMFCreateComponent(m_pContext, AMFVideoConverter, &m_pConverter);
    if (res != AMF_OK) {
        PrintMes(VCE_LOG_ERROR, _T("Failed to create converter context: %d\n"), res);
        return AMF_FAIL;
    }

    res = m_pConverter->SetProperty(AMF_VIDEO_CONVERTER_MEMORY_TYPE, prm->memoryTypeIn);
    res = m_pConverter->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, formatOut);
    res = m_pConverter->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_SIZE, AMFConstructSize(m_inputInfo.dstWidth, m_inputInfo.dstHeight));
    res = m_pConverter->SetProperty(AMF_VIDEO_CONVERTER_SCALE, AMF_VIDEO_CONVERTER_SCALE_BICUBIC);
    if (AMF_OK != (res = m_pConverter->Init(formatOut, m_inputInfo.srcWidth, m_inputInfo.srcHeight))) {
        PrintMes(VCE_LOG_ERROR, _T("Failed to init converter: %d\n"), res);
        return res;
    }
    return res;
#else
return AMF_NOT_SUPPORTED;
#endif
}

AMF_RESULT VCECore::initEncoder(VCEParam *prm) {
    AMF_RESULT res = AMF_OK;

    if (m_pVCELog->getLogLevel() <= VCE_LOG_DEBUG) {
        TCHAR cpuInfo[256] = { 0 };
        TCHAR gpu_info[1024] = { 0 };
        std::wstring deviceName = (m_deviceDX9.GetDevice() == nullptr) ? m_deviceDX11.GetDisplayDeviceName() : m_deviceDX9.GetDisplayDeviceName();
        deviceName = str_replace(deviceName, L" (TM)", L"");
        deviceName = str_replace(deviceName, L" (R)", L"");
        deviceName = str_replace(deviceName, L" Series", L"");
        getCPUInfo(cpuInfo, _countof(cpuInfo));
        getGPUInfo("Advanced Micro Devices", gpu_info, _countof(gpu_info));
        PrintMes(VCE_LOG_DEBUG, _T("VCEEnc    %s (%s)\n"), VER_STR_FILEVERSION_TCHAR, BUILD_ARCH_STR);
        PrintMes(VCE_LOG_DEBUG, _T("OS        %s (%s)\n"), getOSVersion().c_str(), is_64bit_os() ? _T("x64") : _T("x86"));
        PrintMes(VCE_LOG_DEBUG, _T("CPU Info  %s\n"), cpuInfo);
        PrintMes(VCE_LOG_DEBUG, _T("GPU Info  %s [%s]\n"), wstring_to_string(deviceName).c_str(), gpu_info);
    }

    if (AMF_OK != (res = AMFCreateComponent(m_pContext, list_codecs[prm->nCodecId], &m_pEncoder))) {
        PrintMes(VCE_LOG_ERROR, _T("Failed to AMFCreateComponent.\n"));
        return AMF_FAIL;
    }
    PrintMes(VCE_LOG_DEBUG, _T("initialized Encoder component.\n"));

    m_Params.SetParamDescription(PARAM_NAME_INPUT,         ParamCommon, L"Input file name");
    m_Params.SetParamDescription(PARAM_NAME_INPUT_WIDTH,   ParamCommon, L"Input Frame width (integer, default = 0)");
    m_Params.SetParamDescription(PARAM_NAME_INPUT_HEIGHT,  ParamCommon, L"Input Frame height (integer, default = 0)");
    m_Params.SetParamDescription(PARAM_NAME_OUTPUT,        ParamCommon, L"Output file name");
    m_Params.SetParamDescription(PARAM_NAME_OUTPUT_WIDTH,  ParamCommon, L"Output Frame width (integer, default = 0)");
    m_Params.SetParamDescription(PARAM_NAME_OUTPUT_HEIGHT, ParamCommon, L"Output Frame height (integer, default = 0)");
    m_Params.SetParamDescription(PARAM_NAME_ENGINE,        ParamCommon, L"Specifies decoder/encoder engine type (DX9, DX11)");
    m_Params.SetParamDescription(PARAM_NAME_ADAPTERID,     ParamCommon, L"Specifies adapter ID (integer, default = 0)");
    m_Params.SetParamDescription(PARAM_NAME_CAPABILITY,    ParamCommon, L"Enable/Disable to display the device capabilities (true, false default =  false)");

    RegisterEncoderParams(&m_Params);

    m_Params.SetParamAsString(PARAM_NAME_INPUT,     tchar_to_wstring(prm->pInputFile));
    m_Params.SetParamAsString(PARAM_NAME_OUTPUT,    tchar_to_wstring(prm->pOutputFile));
    m_Params.SetParam(PARAM_NAME_ADAPTERID, (amf_int64)0);

    int nGOPLen = prm->nGOPLen;
    if (nGOPLen == 0) {
        nGOPLen = (int)(m_inputInfo.fps.num / (double)m_inputInfo.fps.den + 0.5) * 10;
    }

    m_Params.SetParam(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, (amf_int64)AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);

    m_Params.SetParam(PARAM_NAME_INPUT_WIDTH,   m_inputInfo.srcWidth);
    m_Params.SetParam(PARAM_NAME_INPUT_HEIGHT,  m_inputInfo.srcHeight);
    m_Params.SetParam(PARAM_NAME_OUTPUT_WIDTH,  m_inputInfo.dstWidth);
    m_Params.SetParam(PARAM_NAME_OUTPUT_HEIGHT, m_inputInfo.dstHeight);
    m_Params.SetParam(PARAM_NAME_CAPABILITY,    false);
    m_Params.SetParam(SETFRAMEPARAMFREQ_PARAM_NAME,   0);
    m_Params.SetParam(SETDYNAMICPARAMFREQ_PARAM_NAME, 0);

    m_Params.SetParam(AMF_VIDEO_ENCODER_FRAMESIZE,          AMFConstructSize(m_inputInfo.dstWidth, m_inputInfo.dstHeight));
    m_Params.SetParam(AMF_VIDEO_ENCODER_FRAMERATE,          AMFConstructRate(m_inputInfo.fps.num, m_inputInfo.fps.den));
    m_Params.SetParam(AMF_VIDEO_ENCODER_USAGE,              (amf_int64)AMF_VIDEO_ENCODER_USAGE_TRANSCONDING);
    m_Params.SetParam(AMF_VIDEO_ENCODER_PROFILE,            (amf_int64)prm->codecParam[prm->nCodecId].nProfile);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_PROFILE_LEVEL,      (amf_int64)prm->codecParam[prm->nCodecId].nLevel);
    m_Params.SetParam(AMF_VIDEO_ENCODER_SCANTYPE,           (amf_int64)is_interlaced(prm));
    m_Params.SetParam(AMF_VIDEO_ENCODER_QUALITY_PRESET,     (amf_int64)prm->nQualityPreset);

    m_Params.SetParam(AMF_VIDEO_ENCODER_B_PIC_DELTA_QP,     (amf_int64)prm->nDeltaQPBFrame);
    m_Params.SetParam(AMF_VIDEO_ENCODER_REF_B_PIC_DELTA_QP, (amf_int64)prm->nDeltaQPBFrameRef);


    m_Params.SetParam(AMF_VIDEO_ENCODER_ENFORCE_HRD,        true);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);

    m_Params.SetParam(AMF_VIDEO_ENCODER_GOP_SIZE,                       (amf_int64)nGOPLen);
    m_Params.SetParam(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,                (amf_int64)prm->nVBVBufferSize * 1000);
    m_Params.SetParam(AMF_VIDEO_ENCODER_INITIAL_VBV_BUFFER_FULLNESS,    (amf_int64)prm->nInitialVBVPercent);

    m_Params.SetParam(AMF_VIDEO_ENCODER_MIN_QP,                         (amf_int64)prm->nQPMin);
    m_Params.SetParam(AMF_VIDEO_ENCODER_MAX_QP,                         (amf_int64)prm->nQPMax);
    m_Params.SetParam(AMF_VIDEO_ENCODER_QP_I,                           (amf_int64)prm->nQPI);
    m_Params.SetParam(AMF_VIDEO_ENCODER_QP_P,                           (amf_int64)prm->nQPP);
    m_Params.SetParam(AMF_VIDEO_ENCODER_QP_B,                           (amf_int64)prm->nQPB);
    m_Params.SetParam(AMF_VIDEO_ENCODER_TARGET_BITRATE,                 (amf_int64)prm->nBitrate * 1000);
    m_Params.SetParam(AMF_VIDEO_ENCODER_PEAK_BITRATE,                   (amf_int64)prm->nMaxBitrate * 1000);
    m_Params.SetParam(AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE, !!prm->bEnableSkipFrame);
    m_Params.SetParam(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,            (amf_int64)prm->nRateControl);

    //m_Params.SetParam(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING,       (amf_int64)0);
    m_Params.SetParam(AMF_VIDEO_ENCODER_B_PIC_PATTERN,                  (amf_int64)prm->nBframes);
    m_Params.SetParam(AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER,             !!prm->bDeblockFilter);
    m_Params.SetParam(AMF_VIDEO_ENCODER_B_REFERENCE_ENABLE,             !!prm->bBPyramid);
    m_Params.SetParam(AMF_VIDEO_ENCODER_IDR_PERIOD,                     (amf_int64)nGOPLen);
    ////m_Params.SetParam(AMF_VIDEO_ENCODER_INTRA_REFRESH_NUM_MBS_PER_SLOT, false);
    m_Params.SetParam(AMF_VIDEO_ENCODER_SLICES_PER_FRAME,               (amf_int64)prm->nSlices);

    m_Params.SetParam(AMF_VIDEO_ENCODER_MOTION_HALF_PIXEL,              !!(prm->nMotionEst & VCE_MOTION_EST_HALF));
    m_Params.SetParam(AMF_VIDEO_ENCODER_MOTION_QUARTERPIXEL,            !!(prm->nMotionEst & VCE_MOTION_EST_QUATER));


    //m_Params.SetParam(AMF_VIDEO_ENCODER_END_OF_SEQUENCE,                false);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_END_OF_STREAM,                  false);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,             (amf_int64)AMF_VIDEO_ENCODER_PICTURE_TYPE_NONE);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_INSERT_AUD,                     false);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_INSERT_SPS,                     false);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_INSERT_PPS,                     false);
    m_Params.SetParam(AMF_VIDEO_ENCODER_PICTURE_STRUCTURE,              (amf_int64)prm->nInterlaced);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_MARK_CURRENT_WITH_LTR_INDEX,    false);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_FORCE_LTR_REFERENCE_BITFIELD,   (amf_int64)0);

    //m_Params.SetParam(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_ENUM);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_OUTPUT_MARKED_LTR_INDEX, (amf_int64)-1);
    //m_Params.SetParam(AMF_VIDEO_ENCODER_OUTPUT_REFERENCED_LTR_INDEX_BITFIELD, (amf_int64)0);

    // Usage is preset that will set many parameters
    PushParamsToPropertyStorage(&m_Params, ParamEncoderUsage, m_pEncoder);
    // override some usage parameters
    PushParamsToPropertyStorage(&m_Params, ParamEncoderStatic, m_pEncoder);

    const amf::AMF_SURFACE_FORMAT formatIn = amf::AMF_SURFACE_NV12;
    if (AMF_OK != (res = m_pEncoder->Init(formatIn, m_inputInfo.dstWidth, m_inputInfo.dstHeight))) {
        PrintMes(VCE_LOG_ERROR, _T("Failed to initalize encoder.\n"));
        return res;
    }
    PrintMes(VCE_LOG_DEBUG, _T("initalized encoder.\n"));

    PushParamsToPropertyStorage(&m_Params, ParamEncoderDynamic, m_pEncoder);

    // Connect pipeline
    if (AMF_OK != (res = Connect(m_pFileReader, 4))) {
        PrintMes(VCE_LOG_ERROR, _T("failed to connect input to pipeline.\n"));
        return res;
    }
    if (m_pDecoder) {
        if (AMF_OK != (res = Connect(PipelineElementPtr(new PipelineElementAMFComponent(m_pDecoder)), 4))) {
            PrintMes(VCE_LOG_ERROR, _T("failed to connect deocder to pipeline.\n"));
            return res;
        }
    }
    if (m_pConverter) {
        if (AMF_OK != (res = Connect(PipelineElementPtr(new PipelineElementAMFComponent(m_pConverter)), 4))) {
            PrintMes(VCE_LOG_ERROR, _T("failed to connect converter to pipeline.\n"));
            return res;
        }
    }
    if (AMF_OK != (res = Connect(PipelineElementPtr(new PipelineElementEncoder(m_pEncoder, &m_Params, 0, 0)), 10))) {
        PrintMes(VCE_LOG_ERROR, _T("failed to connect encoder to pipeline.\n"));
        return res;
    }
    if (AMF_OK != (res = Connect(m_pOutput, 5))) {
        PrintMes(VCE_LOG_ERROR, _T("failed to connect output to pipeline.\n"));
        return res;
    }
    PrintMes(VCE_LOG_DEBUG, _T("connected elements to pipeline.\n"));
    return res;
}

AMF_RESULT VCECore::init(VCEParam *prm, VCEInputInfo *inputInfo) {
    Terminate();

    tstring vce_check;
    if (!check_if_vce_available(vce_check)) {
        PrintMes(VCE_LOG_ERROR, _T("%s\n"), vce_check);
        return AMF_NO_DEVICE;
    }

    AMF_RESULT res = AMFCreateContext(&m_pContext);
    if (res != AMF_OK) {
        PrintMes(VCE_LOG_ERROR, _T("Failed to create AMF Context.\n"));
        return res;
    }
    PrintMes(VCE_LOG_DEBUG, _T("Created AMF Context.\n"));

    m_inputInfo = *inputInfo;

    if (prm->bTimerPeriodTuning) {
        m_bTimerPeriodTuning = true;
        timeBeginPeriod(1);
        PrintMes(VCE_LOG_DEBUG, _T("timeBeginPeriod(1)\n"));
    }

    if (AMF_OK != (res = initInput(prm, &m_inputInfo))) {
        return res;
    }

    if (AMF_OK != (res = checkParam(prm))) {
        return res;
    }

    if (AMF_OK != (res = initOutput(prm))) {
        return res;
    }

    if (AMF_OK != (res = initDevice(prm))) {
        return res;
    }

    if (AMF_OK != (res = initDecoder(prm))) {
        return res;
    }

    if (AMF_OK != (res = initConverter(prm))) {
        return res;
    }

    return initEncoder(prm);
}

AMF_RESULT VCECore::run() {
    AMF_RESULT res = AMF_OK;
    m_pStatus->SetStart();
    res = Pipeline::Start();
    if (res != AMF_OK) {
        PrintMes(VCE_LOG_ERROR, _T("failed to start pipeline\n"));
        return res;
    }
    PrintMes(VCE_LOG_DEBUG, _T("started pipeline.\n"));

    return AMF_OK;
}

void VCECore::PrintEncoderParam() {
    PrintMes(VCE_LOG_INFO, GetEncoderParam().c_str());
}

tstring VCECore::GetEncoderParam() {
    const amf::AMFPropertyStorage *pProperty = m_pEncoder;

    auto GetPropertyStr = [pProperty](const wchar_t *pName) {
        const wchar_t *pProp;
        pProperty->GetPropertyWString(pName, &pProp);
        return wstring_to_string(pProp);
    };

    auto GetPropertyInt = [pProperty](const wchar_t *pName) {
        int64_t value;
        pProperty->GetProperty(pName, &value);
        return (int)value;
    };

    auto GetPropertyBool = [pProperty](const wchar_t *pName) {
        bool value;
        pProperty->GetProperty(pName, &value);
        return value;
    };

    auto getPropertyDesc = [pProperty, GetPropertyInt](const wchar_t *pName, const CX_DESC *list) {
        return tstring(get_cx_desc(list, GetPropertyInt(pName)));
    };

    tstring mes;

    TCHAR cpu_info[256], gpu_info[256];
    getCPUInfo(cpu_info);
    getGPUInfo("Advanced Micro Device", gpu_info, _countof(gpu_info));

    AMFSize frameSize;
    pProperty->GetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, &frameSize);

    AMFRate frameRate;
    pProperty->GetProperty(AMF_VIDEO_ENCODER_FRAMERATE, &frameRate);

    uint32_t nMotionEst = 0x0;
    nMotionEst |= GetPropertyInt(AMF_VIDEO_ENCODER_MOTION_HALF_PIXEL) ? VCE_MOTION_EST_HALF : 0;
    nMotionEst |= GetPropertyInt(AMF_VIDEO_ENCODER_MOTION_QUARTERPIXEL) ? VCE_MOTION_EST_QUATER | VCE_MOTION_EST_HALF : 0;

    std::wstring deviceName = (m_deviceDX9.GetDevice() == nullptr) ? m_deviceDX11.GetDisplayDeviceName() : m_deviceDX9.GetDisplayDeviceName();
    deviceName = str_replace(deviceName, L" (TM)", L"");
    deviceName = str_replace(deviceName, L" (R)", L"");
    deviceName = str_replace(deviceName, L" Series", L"");

    mes += strsprintf(_T("VCEEnc %s (%s) / %s (%s)\n"), VER_STR_FILEVERSION_TCHAR, BUILD_ARCH_STR, getOSVersion().c_str(), is_64bit_os() ? _T("x64") : _T("x86"));
    mes += strsprintf(_T("CPU:           %s\n"), cpu_info);
    mes += strsprintf(_T("GPU:           %s [%s]\n"), wstring_to_tstring(deviceName).c_str(), gpu_info);
    mes += strsprintf(_T("Input:         %s\n"), m_pFileReader->GetInputInfoStr().c_str());
    if (m_inputInfo.crop.left || m_inputInfo.crop.up || m_inputInfo.crop.right || m_inputInfo.crop.bottom) {
        mes += strsprintf(_T("Crop:          %d,%d,%d,%d\n"), m_inputInfo.crop.left, m_inputInfo.crop.up, m_inputInfo.crop.right, m_inputInfo.crop.bottom);
    }
    mes += strsprintf(_T("Output:        H.264/AVC %s @ %s %dx%d%s %d/%d(%.3f) fps\n"),
        getPropertyDesc(AMF_VIDEO_ENCODER_PROFILE, list_avc_profile).c_str(),
        getPropertyDesc(AMF_VIDEO_ENCODER_PROFILE_LEVEL, list_avc_level).c_str(),
        frameSize.width, frameSize.height, GetPropertyInt(AMF_VIDEO_ENCODER_SCANTYPE) ? _T("i") : _T("p"), frameRate.num, frameRate.den, frameRate.num / (double)frameRate.den);
    mes += strsprintf(_T("Quality:       %s\n"), getPropertyDesc(AMF_VIDEO_ENCODER_QUALITY_PRESET, list_vce_quality_preset).c_str());
    if (GetPropertyInt(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD) == AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTRAINED_QP) {
        mes += strsprintf(_T("CQP:           I:%d, P:%d"),
            GetPropertyInt(AMF_VIDEO_ENCODER_QP_I),
            GetPropertyInt(AMF_VIDEO_ENCODER_QP_P));
        if (GetPropertyInt(AMF_VIDEO_ENCODER_B_PIC_PATTERN)) {
            mes += strsprintf(_T(", B:%d"), GetPropertyInt(AMF_VIDEO_ENCODER_QP_B));
        }
        mes += _T("\n");
    } else {
        mes += strsprintf(_T("%s:           %d kbps, Max %d kbps\n"),
            getPropertyDesc(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, list_vce_rc_method).c_str(),
            GetPropertyInt(AMF_VIDEO_ENCODER_TARGET_BITRATE) / 1000,
            GetPropertyInt(AMF_VIDEO_ENCODER_PEAK_BITRATE) / 1000);
        mes += strsprintf(_T("QP:            Min: %d, Max: %d\n"),
            GetPropertyInt(AMF_VIDEO_ENCODER_MIN_QP),
            GetPropertyInt(AMF_VIDEO_ENCODER_MAX_QP));
    }
    mes += strsprintf(_T("VBV Bufsize:   %d kbps\n"), GetPropertyInt(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE) / 1000);
    mes += strsprintf(_T("Bframes:       %d frames, b-pyramid: %s\n"),
        GetPropertyInt(AMF_VIDEO_ENCODER_B_PIC_PATTERN),
        (GetPropertyInt(AMF_VIDEO_ENCODER_B_PIC_PATTERN) && GetPropertyInt(AMF_VIDEO_ENCODER_B_REFERENCE_ENABLE) ? _T("on") : _T("off")));
    if (GetPropertyInt(AMF_VIDEO_ENCODER_B_PIC_PATTERN)) {
        mes += strsprintf(_T("Delta QP:      Bframe: %d, RefBframe: %d\n"), GetPropertyInt(AMF_VIDEO_ENCODER_B_PIC_DELTA_QP), GetPropertyInt(AMF_VIDEO_ENCODER_REF_B_PIC_DELTA_QP));
    }
    mes += strsprintf(_T("Motion Est:    %s\n"), get_cx_desc(list_mv_presicion, nMotionEst));
    mes += strsprintf(_T("Slices:        %d\n"), GetPropertyInt(AMF_VIDEO_ENCODER_SLICES_PER_FRAME));
    mes += strsprintf(_T("GOP Len:       %d frames\n"), GetPropertyInt(AMF_VIDEO_ENCODER_GOP_SIZE));
    tstring others;
    if (GetPropertyBool(AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE)) {
        others += _T("skip_frame ");
    }
    if (!GetPropertyBool(AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER)) {
        others += _T("no_deblock ");
    } else {
        others += _T("deblock ");
    }
    if (m_pVCELog->getLogLevel() <= VCE_LOG_DEBUG) {
        if (GetPropertyBool(AMF_VIDEO_ENCODER_INSERT_AUD)) {
            others += _T("aud ");
        }
        if (GetPropertyBool(AMF_VIDEO_ENCODER_INSERT_SPS)) {
            others += _T("sps ");
        }
        if (GetPropertyBool(AMF_VIDEO_ENCODER_INSERT_PPS)) {
            others += _T("pps ");
        }
    }
    if (GetPropertyBool(AMF_VIDEO_ENCODER_ENFORCE_HRD)) {
        others += _T("hrd ");
    }
    if (GetPropertyBool(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE)) {
        others += _T("filler ");
    }
    if (others.length() > 0) {
        mes += strsprintf(_T("Others:        %s\n"), others.c_str());
    }
    return mes;
}

AMF_RESULT VCECore::PrintResult() {
    m_pStatus->WriteResults();
    return AMF_OK;
}

bool check_if_vce_available(tstring& mes) {
#if _M_IX86
    const TCHAR *dllNameCore      = _T("amf-core-windesktop32.dll");
    const TCHAR *dllNameComponent = _T("amf-component-vce-windesktop32.dll");
#else
    const TCHAR *dllNameCore      = _T("amf-core-windesktop64.dll");
    const TCHAR *dllNameComponent = _T("amf-component-vce-windesktop64.dll");
#endif
    HMODULE hModuleCore = LoadLibrary(dllNameCore);
    HMODULE hModuleComponent = LoadLibrary(dllNameComponent);

    bool ret = true;
    mes = _T("");
    if (hModuleComponent == NULL) {
        ret = false;
        mes += tstring(dllNameComponent) + _T(" not found on system");
    } else {
        FreeLibrary(hModuleComponent);
    }
    if (hModuleCore == NULL) {
        ret = false;
        mes += tstring(dllNameCore) + _T(" not found on system");
    } else {
        FreeLibrary(hModuleCore);
    }
    if (ret) {
        uint32_t count = 0;
        amf::AMFContextPtr pContext;
        amf::AMFComponentPtr pEncoder;
        DeviceDX9 deviceDX9;
        if (   AMF_OK != deviceDX9.GetAdapterCount(&count)
            || count == 0
            || AMF_OK != AMFCreateContext(&pContext)
            || AMF_OK != pContext->InitDX9(deviceDX9.GetDevice())
            || AMF_OK != AMFCreateComponent(pContext, list_codecs[0], &pEncoder)) {
            ret = false;
            mes = _T("System has no GPU supporting VCE.");
        }

        if (pEncoder != nullptr) {
            pEncoder->Terminate();
            pEncoder = nullptr;
        }

        if (pContext != nullptr) {
            pContext->Terminate();
            pContext = nullptr;
        }

        deviceDX9.Terminate();
    }
    return ret;
}

bool check_if_vce_available() {
    tstring dummy;
    return check_if_vce_available(dummy);
}
