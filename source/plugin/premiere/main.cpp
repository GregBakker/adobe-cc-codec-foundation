#include "configure.hpp"
#include "logging.hpp"
#include "main.hpp"
#include "premiereParams.hpp"
#include "prstring.hpp"
#include "export_settings.hpp"
#include "exporter.hpp"
#include "configure.hpp"
#include "string_conversion.hpp"
#include "presets.hpp"
#include <vector>
#include <locale>

#ifdef WIN32
#include <Windows.h>
#include "StackWalker.h"
#endif


csSDK_int32 GetNumberOfAudioChannels(csSDK_int32 audioChannelType);
static void renderAndWriteAllAudio(exDoExportRec *exportInfoP, prMALError &error, Exporter& exporter);

// For SEH and stack dump on win32 this is called from an SEH wrapper
prMALError wrapped_xSDKExport(csSDK_int32 selector, exportStdParms* stdParmsP, void* param1, void* param2)
{
    prMALError result = exportReturn_Unsupported;
    
    FDN_DEBUG("xSDKExport selector=", selector);

    try {
        switch (selector)
        {
        case exSelStartup:
            FDN_DEBUG("exSelStartup");
            FDN_INFO("Premiere plugin startup");
            result = startup(stdParmsP, reinterpret_cast<exExporterInfoRec*>(param1));
            break;

        case exSelShutdown:
            FDN_DEBUG("exSelShutdown");
            FDN_INFO("Premiere plugin shutdown");
            CodecRegistry::codec().reset();
            break;

        case exSelBeginInstance:
            FDN_DEBUG("exSelBeginInstance");
            result = beginInstance(stdParmsP, reinterpret_cast<exExporterInstanceRec*>(param1));
            break;

        case exSelEndInstance:
            FDN_DEBUG("exSelEndInstance");
            result = endInstance(stdParmsP, reinterpret_cast<exExporterInstanceRec*>(param1));
            break;

        case exSelGenerateDefaultParams:
            FDN_DEBUG("exSelGenerateDefaultParams");
            result = generateDefaultParams(stdParmsP, reinterpret_cast<exGenerateDefaultParamRec*>(param1));
            break;

        case exSelPostProcessParams:
            FDN_DEBUG("exSelPostProcessParams");
            result = postProcessParams(stdParmsP, reinterpret_cast<exPostProcessParamsRec*>(param1));
            break;

        case exSelGetParamSummary:
            FDN_DEBUG("exSelGetParamSummary");
            result = getParamSummary(stdParmsP, reinterpret_cast<exParamSummaryRec*>(param1));
            break;

        case exSelQueryOutputSettings:
            FDN_DEBUG("exSelQueryOutputSettings");
            result = queryOutputSettings(stdParmsP, reinterpret_cast<exQueryOutputSettingsRec*>(param1));
            break;

        case exSelQueryExportFileExtension:
            FDN_DEBUG("exSelQueryExportFileExtension");
            result = fileExtension(stdParmsP, reinterpret_cast<exQueryExportFileExtensionRec*>(param1));
            break;

        case exSelParamButton:
            FDN_DEBUG("exSelParamButton");
            result = paramButton(stdParmsP, reinterpret_cast<exParamButtonRec*>(param1));
            break;

        case exSelValidateParamChanged:
            FDN_DEBUG("exSelValidateParamChanged");
            result = validateParamChanged(stdParmsP, reinterpret_cast<exParamChangedRec*>(param1));
            break;

        case exSelValidateOutputSettings:
            FDN_DEBUG("exSelValidateOutputSettings");
            result = malNoError;
            break;

        case exSelExport:
            FDN_DEBUG("exSelExport");
            result = doExport(stdParmsP, reinterpret_cast<exDoExportRec*>(param1));
            break;

        default:
            FDN_DEBUG("selector unhandled");
            break;
        }
    }
    catch (const std::exception& ex) {
        FDN_ERROR("exception thrown: ", ex.what());
        return exportReturn_ErrOther;
    }

    return result;
}

#ifdef WIN32
class FDNStackWalker : public StackWalker
{
public:
    FDNStackWalker() : StackWalker() {}

    std::stringstream ss;

protected:
    virtual void OnOutput(LPCSTR szText) {
        ss << szText;
        StackWalker::OnOutput(szText);
    }
};

// The exception filter function:
LONG WINAPI ExpFilter(EXCEPTION_POINTERS* pExp, DWORD dwExpCode)
{
    FDNStackWalker sw;
    sw.ShowCallstack(GetCurrentThread(), pExp->ContextRecord);
    FDN_FATAL("SEH exception thrown - ", sw.ss.str());
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif


DllExport PREMPLUGENTRY xSDKExport(csSDK_int32 selector, exportStdParms* stdParmsP, void* param1, void* param2)
{
    prMALError result = exportReturn_Unsupported;

#ifdef WIN32
    __try {
#endif
        result = wrapped_xSDKExport(selector, stdParmsP, param1, param2);
#ifdef WIN32
    }
    __except (ExpFilter(GetExceptionInformation(), GetExceptionCode()))
    {
        return malUnknownError;
    }
#endif
    return result;
}

static void checkPresetsInstalled()
{
    /*
     Because presets have a new install location for every major CC version, but plugins
     are in a fixed location, it is possible for the plugin to be loaded without presets
     when a new version of CC is installed.

     Check for up(or down)graded versions of CC without presets, and install them
     */
    auto presets = Presets::getPresetFileNames();
    // Do nothing if we have no presets at the plugin install location
    if (presets.empty())
    {
        return;
    }
    auto src_dir = Presets::getSourceDirectoryPath();
    auto dsts = Presets::getDestinationDirectoryPaths();
    for (const auto &destination : dsts)
    {
        // Create the Presets directory if needed
        bool exists = Presets::directoryExists(destination);
        if (!exists)
        {
            exists = Presets::createDirectory(destination);
        }
        if (exists)
        {
            // Install the presets if not already present
            for (const auto &preset : presets)
            {
                Presets::copy(preset, src_dir, destination, false);
            }
        }
    }
}

prMALError startup(exportStdParms* stdParms, exExporterInfoRec* infoRec)
{
    if (infoRec->exportReqIndex == 0)
    {
        checkPresetsInstalled();

        // singleton needed from here on
        const auto &codec = *CodecRegistry::codec();

        infoRec->classID = reinterpret_cast<const uint32_t &>(codec.details().videoFormat);
        infoRec->fileType = reinterpret_cast<const uint32_t&>(codec.details().fileFormat);
        infoRec->hideInUI = kPrFalse;
        infoRec->isCacheable = kPrFalse;
        infoRec->exportReqIndex = 0;
        infoRec->canExportVideo = kPrTrue;
        infoRec->canExportAudio = kPrTrue;
        infoRec->canEmbedCaptions = kPrFalse;
        infoRec->canConformToMatchParams = kPrTrue;
        infoRec->singleFrameOnly = kPrFalse;
        infoRec->wantsNoProgressBar = kPrFalse;
        infoRec->doesNotSupportAudioOnly = kPrTrue;
        infoRec->interfaceVersion = EXPORTMOD_VERSION;
		SDKStringConvert::to_buffer(codec.logName(), infoRec->fileTypeName);
		SDKStringConvert::to_buffer(codec.details().videoFileExt, infoRec->fileTypeDefaultExtension);
        return exportReturn_IterateExporter;
    }

    return exportReturn_IterateExporterDone;
}

prMALError beginInstance(exportStdParms* stdParmsP, exExporterInstanceRec* instanceRecP)
{
	SPErr spError = kSPNoError;
	PrSDKMemoryManagerSuite* memorySuite;
	SPBasicSuite* spBasic = stdParmsP->getSPBasicSuite();

	if (spBasic == nullptr)
		return exportReturn_ErrMemory;

	spError = spBasic->AcquireSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&memorySuite)));
	void *settingsMem = (memorySuite->NewPtrClear(sizeof(ExportSettings)));

	if (settingsMem == nullptr)
		return exportReturn_ErrMemory;

	ExportSettings* settings = new(settingsMem) ExportSettings();

	settings->fileType = instanceRecP->fileType;
	settings->spBasic = spBasic;
	settings->memorySuite = memorySuite;
    spError = spBasic->AcquireSuite(kPrSDKExporterUtilitySuite, kPrSDKExporterUtilitySuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->exporterUtilitySuite))));
    spError = spBasic->AcquireSuite(kPrSDKExportParamSuite, kPrSDKExportParamSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->exportParamSuite))));
    spError = spBasic->AcquireSuite(kPrSDKExportProgressSuite, kPrSDKExportProgressSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->exportProgressSuite))));
	spError = spBasic->AcquireSuite(kPrSDKExportFileSuite, kPrSDKExportFileSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->exportFileSuite))));
	spError = spBasic->AcquireSuite(kPrSDKExportInfoSuite, kPrSDKExportInfoSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->exportInfoSuite))));
	spError = spBasic->AcquireSuite(kPrSDKErrorSuite, kPrSDKErrorSuiteVersion3, const_cast<const void**>(reinterpret_cast<void**>(&(settings->errorSuite))));
	spError = spBasic->AcquireSuite(kPrSDKClipRenderSuite, kPrSDKClipRenderSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->clipRenderSuite))));
	spError = spBasic->AcquireSuite(kPrSDKMarkerSuite, kPrSDKMarkerSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->markerSuite))));
	spError = spBasic->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->ppixSuite))));
	spError = spBasic->AcquireSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->timeSuite))));
    spError = spBasic->AcquireSuite(kPrSDKWindowSuite, kPrSDKWindowSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->windowSuite))));
    spError = spBasic->AcquireSuite(kPrSDKAudioSuite, kPrSDKAudioSuiteVersion, const_cast<const void**>(reinterpret_cast<void**>(&(settings->audioSuite))));
    spError = spBasic->AcquireSuite(kPrSDKSequenceAudioSuite, kPrSDKSequenceAudioSuiteVersion1, const_cast<const void**>(reinterpret_cast<void**>(&(settings->sequenceAudioSuite))));

    // convenience callback
    auto report = settings->exporterUtilitySuite->ReportEvent;
    auto pluginId = instanceRecP->exporterPluginID;

    // !!! integrate into FD_* 
    settings->reportError = [report, pluginId](const std::string& error) {

        StringForPr title(CodecRegistry::logName() + " - ERROR");
        StringForPr detail(error);

        report(
            pluginId, PrSDKErrorSuite2::kEventTypeError,
            title,
            detail);
    };

    // !!! integrate into FD_*
    settings->logMessage = [report, pluginId](const std::string& message) {

        StringForPr title(CodecRegistry::logName());
        StringForPr detail(message);

        report(
            pluginId, PrSDKErrorSuite2::kEventTypeError,
            title,
            detail);
    };

	instanceRecP->privateData = reinterpret_cast<void*>(settings);

	return malNoError;
}

prMALError endInstance(exportStdParms* stdParmsP, exExporterInstanceRec* instanceRecP)
{
	prMALError result = malNoError;
	ExportSettings* settings = reinterpret_cast<ExportSettings*>(instanceRecP->privateData);
	SPBasicSuite* spBasic = stdParmsP->getSPBasicSuite();

	if (spBasic == nullptr || settings == nullptr)
		return malNoError;

    if (settings->exporterUtilitySuite)
        result = spBasic->ReleaseSuite(kPrSDKExporterUtilitySuite, kPrSDKExporterUtilitySuiteVersion);

    if (settings->exportParamSuite)
        result = spBasic->ReleaseSuite(kPrSDKExportParamSuite, kPrSDKExportParamSuiteVersion);

    if (settings->exportProgressSuite)
		result = spBasic->ReleaseSuite(kPrSDKExportProgressSuite, kPrSDKExportProgressSuiteVersion);

	if (settings->exportFileSuite)
		result = spBasic->ReleaseSuite(kPrSDKExportFileSuite, kPrSDKExportFileSuiteVersion);

	if (settings->exportInfoSuite)
		result = spBasic->ReleaseSuite(kPrSDKExportInfoSuite, kPrSDKExportInfoSuiteVersion);

	if (settings->errorSuite)
		result = spBasic->ReleaseSuite(kPrSDKErrorSuite, kPrSDKErrorSuiteVersion3);

	if (settings->clipRenderSuite)
		result = spBasic->ReleaseSuite(kPrSDKClipRenderSuite, kPrSDKClipRenderSuiteVersion);

	if (settings->markerSuite)
		result = spBasic->ReleaseSuite(kPrSDKMarkerSuite, kPrSDKMarkerSuiteVersion);

	if (settings->ppixSuite)
		result = spBasic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);

	if (settings->timeSuite)
		result = spBasic->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);

	if (settings->windowSuite)
		result = spBasic->ReleaseSuite(kPrSDKWindowSuite, kPrSDKWindowSuiteVersion);

    if (settings->audioSuite)
        result = spBasic->ReleaseSuite(kPrSDKAudioSuite, kPrSDKAudioSuiteVersion);

    if (settings->sequenceAudioSuite)
        result = spBasic->ReleaseSuite(kPrSDKSequenceAudioSuite, kPrSDKSequenceAudioSuiteVersion1);

	settings->~ExportSettings();

	if (settings->memorySuite)
	{
		PrSDKMemoryManagerSuite* memorySuite = settings->memorySuite;
		memorySuite->PrDisposePtr(reinterpret_cast<PrMemoryPtr>(settings));
		result = spBasic->ReleaseSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion);
	}

	return result;
}
// !!! intermediate functionality to be used prior to unifying creating
// !!! an EncoderParams from premiere's exporter settings
struct EncoderSettings
{
    CodecAlpha alpha;
    Codec4CC videoFormat;
    int quality;
};

static EncoderSettings getVideoEncoderSettings(PrSDKExportParamSuite* paramSuite, csSDK_uint32 exID)
{
    const csSDK_int32 mgroupIndex = 0;

    const auto& codec = *CodecRegistry::codec();

    Codec4CC videoFormat;
    if (codec.details().hasSubTypes())
    {
        exParamValues subTypeParamValue;
        paramSuite->GetParamValue(exID, mgroupIndex, ADBEVideoCodec, &subTypeParamValue);
        videoFormat = reinterpret_cast<Codec4CC&>(subTypeParamValue.value.intValue);
    }
    else {
        videoFormat = codec.details().videoFormat;
    }

    CodecAlpha alpha{ withAlpha };
    auto codecAlphaDetails = codec.details().alpha;
    if (codecAlphaDetails.hasPerSubtypeAlphaSupport) {
        alpha = codecAlphaDetails.subtypeAlphaSupport[videoFormat];
    } else {
        bool hasExplicitAlphaChannel{ codec.details().hasExplicitIncludeAlphaChannel };
        if (hasExplicitAlphaChannel) {
            exParamValues includeAlphaChannel;
            paramSuite->GetParamValue(exID, 0, codec.details().premiereIncludeAlphaChannelName.c_str(), &includeAlphaChannel);
            alpha = includeAlphaChannel.value.intValue ? withAlpha : withoutAlpha;
        }
    };

    exParamValues qualityParamValue;
    paramSuite->GetParamValue(exID, 0, ADBEVideoQuality, &qualityParamValue);
    auto codecQualityDetails = codec.details().quality;
    int clampedQuality{0};
    if (codec.details().hasQualityForSubType(videoFormat)) {
        auto qualityValue = qualityParamValue.value.intValue;
        auto description = codecQualityDetails.descriptions.find(qualityValue);
        clampedQuality = (description == codecQualityDetails.descriptions.end()) ? codecQualityDetails.defaultQuality : qualityValue;
    }

    return EncoderSettings{alpha, videoFormat, clampedQuality};
}

prMALError queryOutputSettings(exportStdParms *stdParmsP, exQueryOutputSettingsRec *outputSettingsP)
{
	const csSDK_uint32 exID = outputSettingsP->exporterPluginID;
    exParamValues width, height, frameRate;
    ExportSettings* privateData = reinterpret_cast<ExportSettings*>(outputSettingsP->privateData);
	PrSDKExportParamSuite* paramSuite = privateData->exportParamSuite;
	const csSDK_int32 mgroupIndex = 0;
	float fps = 0.0f;

    const auto& codec = *CodecRegistry::codec();


	if (outputSettingsP->inExportVideo)
	{
		paramSuite->GetParamValue(exID, mgroupIndex, ADBEVideoWidth, &width);
		outputSettingsP->outVideoWidth = width.value.intValue;
		paramSuite->GetParamValue(exID, mgroupIndex, ADBEVideoHeight, &height);
		outputSettingsP->outVideoHeight = height.value.intValue;
		paramSuite->GetParamValue(exID, mgroupIndex, ADBEVideoFPS, &frameRate);
		outputSettingsP->outVideoFrameRate = frameRate.value.timeValue;

        outputSettingsP->outVideoAspectNum = 1;
		outputSettingsP->outVideoAspectDen = 1;
		// paramSuite->GetParamValue(exID, mgroupIndex, ADBEVideoFieldType, &fieldType);
		outputSettingsP->outVideoFieldType = prFieldsNone;

    	// Calculate bitrate
	    PrTime ticksPerSecond = 0;
	    csSDK_uint32 videoBitrate = 0;

		privateData->timeSuite->GetTicksPerSecond(&ticksPerSecond);
		fps = static_cast<float>(ticksPerSecond) / frameRate.value.timeValue;

        auto [alpha, videoFormat, quality] = getVideoEncoderSettings(paramSuite, exID);

        videoBitrate = static_cast<csSDK_uint32>(width.value.intValue * height.value.intValue * codec.getPixelFormatSize(alpha, videoFormat, quality) * fps);

        outputSettingsP->outBitratePerSecond = videoBitrate * 8 / 1000;
    }
 
    if (outputSettingsP->inExportAudio)
    {
        exParamValues sampleRate, channelType;
        paramSuite->GetParamValue(exID, mgroupIndex, ADBEAudioRatePerSecond, &sampleRate);
        paramSuite->GetParamValue(exID, mgroupIndex, ADBEAudioNumChannels, &channelType);
        
        outputSettingsP->outAudioChannelType = static_cast<PrAudioChannelType>(channelType.value.intValue);
        outputSettingsP->outAudioSampleRate = sampleRate.value.floatValue;
        outputSettingsP->outAudioSampleType = kPrAudioSampleType_16BitInt;
    }

	return malNoError;
}

prMALError fileExtension(exportStdParms* stdParmsP, exQueryExportFileExtensionRec* exportFileExtensionRecP)
{
	SDKStringConvert::to_buffer(CodecRegistry::codec()->details().videoFileExt, exportFileExtensionRecP->outFileExtension);

	return malNoError;
}

static prMALError wrapped_c_onFrameComplete(
    csSDK_uint32 inWhichPass,
    csSDK_uint32 inFrameNumber,
    csSDK_uint32 inFrameRepeatCount,
    PPixHand inRenderedFrame,
    void* inCallbackData)
{
    ExportSettings* settings = reinterpret_cast<ExportSettings*>(inCallbackData);

    try
    {
        FDN_DEBUG("received frame inWhichPass=", inWhichPass, " inFrameNumber=", inFrameNumber, " inFrameRepeatCount=", inFrameRepeatCount);

        char* bgra_buffer;
        int32_t bgra_stride;
        prMALError error = settings->ppixSuite->GetPixels(inRenderedFrame, PrPPixBufferAccess_ReadOnly, &bgra_buffer);
        if (malNoError != error)
            throw std::runtime_error("could not GetPixels on completed frame");

        error = settings->ppixSuite->GetRowBytes(inRenderedFrame, &bgra_stride);
        if (malNoError != error)
            throw std::runtime_error("could not GetRowBytes on completed frame");

        // !!! we know we're getting this format because we asked for it, but
        // !!! would probably be better to ask inRenderedFrame what it is
        const auto& codec = *CodecRegistry::codec();
        ChannelFormat channelFormat(codec.details().isHighBitDepth ? ChannelFormat_U16_32k : ChannelFormat_U8); // requested frames in keeping with the codec's high bit depth
        FrameFormat format(channelFormat | FrameOrigin_BottomLeft | ChannelLayout_BGRA);

        for (auto iFrame = inFrameNumber; iFrame < inFrameNumber + inFrameRepeatCount; ++iFrame)
            settings->exporter->dispatchVideo(iFrame, (uint8_t*)bgra_buffer, bgra_stride, format);
    }
    catch (const std::exception& ex)
    {
        FDN_ERROR("error exporting frame #", inFrameNumber, " - ", ex.what());
        settings->reportError(ex.what());
        return malUnknownError;
    }
    catch (...)
    {
        FDN_ERROR("unknown exception thrown while exporting frame #", inFrameNumber);
        settings->reportError("unspecified error while processing frame");
        return malUnknownError;
    }

    return malNoError;
}

static prMALError c_onFrameComplete(
    csSDK_uint32 inWhichPass,
    csSDK_uint32 inFrameNumber,
    csSDK_uint32 inFrameRepeatCount,
    PPixHand inRenderedFrame,
    void* inCallbackData)
{
#ifdef WIN32
    __try
    {
#endif
        return wrapped_c_onFrameComplete(inWhichPass, inFrameNumber, inFrameRepeatCount, inRenderedFrame, inCallbackData);

#ifdef WIN32
    }
    __except (ExpFilter(GetExceptionInformation(), GetExceptionCode()))
    {
        return malUnknownError;
    }
#endif
}

static MovieFile createMovieFile(PrSDKExportFileSuite* exportFileSuite, csSDK_int32 fileObject)
{
    // cache some things
    auto Write = exportFileSuite->Write;
    auto Seek = exportFileSuite->Seek;
    auto Close = exportFileSuite->Close;

    MovieFile fileWrapper;
    fileWrapper.onOpenForWrite = [=]() {
        //--- this error flag may be overwritten fairly deeply in callbacks so original error may be
        //--- passed up to Adobe
        csSDK_int32 outPathLength{ 255 };
        prUTF16Char outputFilePath[256] = { '\0' };
        exportFileSuite->GetPlatformPath(fileObject, &outPathLength, outputFilePath);
        FDN_INFO("opening ", SDKStringConvert::to_string(outputFilePath), " for writing");

        prMALError error = exportFileSuite->Open(fileObject);
        if (malNoError != error)
            throw std::runtime_error("couldn't open output file");
    };
    fileWrapper.onWrite = [=](const uint8_t* buffer, size_t size) {
        prMALError writeError = Write(fileObject, (void *)buffer, (int32_t)size);
        if (malNoError != writeError) {
            FDN_ERROR("Could not write to file");
            return -1;
        }
        return 0;
    };
    fileWrapper.onSeek = [=](int64_t offset, int whence) {
        int64_t newPosition;
        ExFileSuite_SeekMode seekMode;
        if (whence == SEEK_SET)
            seekMode = fileSeekMode_Begin;
        else if (whence == SEEK_END)
            seekMode = fileSeekMode_End;
        else if (whence == SEEK_CUR)
            seekMode = fileSeekMode_Current;
        else
            throw std::runtime_error("unhandled file seek mode");
        prMALError seekError = Seek(fileObject, offset, newPosition, seekMode);
        if (malNoError != seekError) {
            FDN_ERROR("Could not seek in file");
            return -1;
        }
        return 0;
    };
    fileWrapper.onClose = [=]() {
        return (malNoError == Close(fileObject)) ? 0 : -1;
    };

    return fileWrapper;
}

void exportLoop(exDoExportRec* exportInfoP, prMALError& error)
{
    const csSDK_uint32 exID = exportInfoP->exporterPluginID;
    ExportSettings* settings = reinterpret_cast<ExportSettings*>(exportInfoP->privateData);

    ExportLoopRenderParams renderParams;

    renderParams.inRenderParamsSize = sizeof(ExportLoopRenderParams);
    renderParams.inRenderParamsVersion = kPrSDKExporterUtilitySuiteVersion;
    auto isHighBitDepth = CodecRegistry::codec()->details().isHighBitDepth;
    auto [alpha, videoFormat, quality] = getVideoEncoderSettings(settings->exportParamSuite, exID);
    renderParams.inFinalPixelFormat = 
        (alpha==withAlpha) ? (isHighBitDepth
                              ? PrPixelFormat_BGRA_4444_16u // PrPixelFormat_BGRA_4444_32f
                              : PrPixelFormat_BGRA_4444_8u)
                           : (isHighBitDepth
                              ? PrPixelFormat_BGRX_4444_16u // PrPixelFormat_BGRA_4444_32f
                              : PrPixelFormat_BGRX_4444_8u);
    renderParams.inStartTime = exportInfoP->startTime;
    renderParams.inEndTime = exportInfoP->endTime;
    renderParams.inReservedProgressPreRender = 0.0; //!!!
    renderParams.inReservedProgressPostRender = 0.0; //!!!

    prMALError multipassExportError = settings->exporterUtilitySuite->DoMultiPassExportLoop(
        exportInfoP->exporterPluginID,
        &renderParams,
        1,  // number of passes
        c_onFrameComplete,
        settings
    );
    if (malNoError != multipassExportError)
    {
        if (error == malNoError)  // retain error if it was set in per-frame export
            error = multipassExportError;
        throw std::runtime_error("DoMultiPassExportLoop failed");
    }
}

static void renderAndWriteAllVideo(exDoExportRec* exportInfoP, prMALError& error)
{
    const csSDK_uint32 exID = exportInfoP->exporterPluginID;
    ExportSettings* settings = reinterpret_cast<ExportSettings*>(exportInfoP->privateData);
    exParamValues ticksPerFrame, width, height, chunkCountParam;
    PrTime ticksPerSecond;

    const auto& codec = *CodecRegistry::codec();

    settings->logMessage("codec implementation: " + CodecRegistry::logName());

    settings->exportParamSuite->GetParamValue(exID, 0, ADBEVideoFPS, &ticksPerFrame);
    settings->exportParamSuite->GetParamValue(exID, 0, ADBEVideoWidth, &width);
    settings->exportParamSuite->GetParamValue(exID, 0, ADBEVideoHeight, &height);

    auto [alpha, videoFormat, quality] = getVideoEncoderSettings(settings->exportParamSuite, exID);

    int chunkCount{ 0 };
    if (codec.details().hasChunkCount) {
        settings->exportParamSuite->GetParamValue(exID, 0, codec.details().premiereChunkCountName.c_str(), &chunkCountParam);
        // currently 0 means auto, which until we have more information about the playback device will be 1 chunk
        chunkCount = (chunkCountParam.optionalParamEnabled == 1) ?
            std::max(1, chunkCountParam.value.intValue)  // force old param to 1
            : 1;
    }
    HapChunkCounts chunkCounts{ static_cast<unsigned int>(chunkCount), static_cast<unsigned int>(chunkCount) };

    settings->timeSuite->GetTicksPerSecond(&ticksPerSecond);

    Rational unsnappedFrameRate{ticksPerSecond, ticksPerFrame.value.timeValue};
    Rational frameRate = SimplifyAndSnapToMpegFrameRate(unsnappedFrameRate);
    if (frameRate != unsnappedFrameRate) {
        FDN_INFO("snapped frame rate from ", unsnappedFrameRate, " to ", frameRate);
    }

    int32_t maxFrames = int32_t(double((exportInfoP->endTime - exportInfoP->startTime)) / ticksPerFrame.value.timeValue);
    
    ChannelFormat channelFormat(codec.details().isHighBitDepth ? ChannelFormat_U16_32k : ChannelFormat_U8); // we're going to request frames in keeping with the codec's high bit depth
    FrameFormat format(channelFormat | FrameOrigin_BottomLeft | ChannelLayout_BGRA);
    FrameSize frameSize{width.value.intValue, height.value.intValue};
    FrameDef frameDef{frameSize, format};

    MovieFile movieFile(createMovieFile(settings->exportFileSuite, exportInfoP->fileObject));

    std::optional<AudioDef> audio;
    if (exportInfoP->exportAudio) {
        exParamValues sampleRate, channelType;
        settings->exportParamSuite->GetParamValue(exID, 0, ADBEAudioRatePerSecond, &sampleRate);
        settings->exportParamSuite->GetParamValue(exID, 0, ADBEAudioNumChannels, &channelType);

        audio = AudioDef{
            GetNumberOfAudioChannels(channelType.value.intValue),
            (int)sampleRate.value.floatValue,
            2,
            AudioEncoding_Signed_PCM
        };
    };

    try {
        settings->exporter = createExporter(
            frameSize, alpha, videoFormat, chunkCounts, quality,
            frameRate,
            maxFrames,
            exportInfoP->reserveMetaDataSpace,
            movieFile,
            audio,
            true  // writeMoovTagEarly
        );

        if (audio)
            renderAndWriteAllAudio(exportInfoP, error, *settings->exporter);

        exportLoop(exportInfoP, error);

        // this may throw
        try
        {
            settings->exporter->close();
        }
        catch (const MovieWriterInvalidData&)
        {
            // this will happen if we MovieWriter didn't guess large enough on header size.
            //  => we have to guess a header size else Adobe will copy the file and rejig it
            //     with the header at the start. This is unwanted but unavoidable if the header
            //     isn't placed ahead of mdat
            // => simplest way out is to redo the export, without the guess, and let adobe do its copy

            FDN_WARNING("incorrect estimate of header size; re-exporting");

            // start with as clean a slate as possible
            try {
                settings->exporter.reset(nullptr);
            }
            catch (...)
            {
            }

            settings->exporter = createExporter(
                frameSize, alpha, videoFormat, chunkCounts, quality,
                frameRate,
                maxFrames,
                exportInfoP->reserveMetaDataSpace,
                movieFile,
                audio,
                false   // writeMoovTagEarly
            );

            if (audio)
                renderAndWriteAllAudio(exportInfoP, error, *settings->exporter);

            exportLoop(exportInfoP, error);

            settings->exporter->close();
        }
    }
    catch (...)
    {
        FDN_DEBUG("exception thrown in renderAndWriteAllVideo");

        settings->exporter.reset(nullptr);
        throw;
    }

    settings->exporter.reset(nullptr);
}

csSDK_int32 GetNumberOfAudioChannels(csSDK_int32 audioChannelType)
{
    csSDK_int32 numberOfChannels = -1;

    if (audioChannelType == kPrAudioChannelType_Mono)
        numberOfChannels = 1;

    else if (audioChannelType == kPrAudioChannelType_Stereo)
        numberOfChannels = 2;

    else if (audioChannelType == kPrAudioChannelType_51)
        numberOfChannels = 6;

    return numberOfChannels;
}

static void renderAndWriteAllAudio(exDoExportRec *exportInfoP, prMALError &error, Exporter& exporter)
{
    // All audio calls to and from Premiere use arrays of buffers of 32-bit floats to pass audio.
    // Audio is not interleaved, rather separate channels are stored in separate buffers.
    const int kAudioSampleSizePremiere = sizeof(float_t);

    // Assume we export 16bit audio and pack up to 1024 samples per packet
    const int kAudioSampleSizeOutput = sizeof(int16_t);
    const int kMaxAudioSamplesPerPacket = 1024;

    const csSDK_uint32 exID = exportInfoP->exporterPluginID;
    ExportSettings *settings = reinterpret_cast<ExportSettings *>(exportInfoP->privateData);
    exParamValues ticksPerFrame, sampleRate, channelType;

    settings->exportParamSuite->GetParamValue(exID, 0, ADBEVideoFPS, &ticksPerFrame);
    settings->exportParamSuite->GetParamValue(exID, 0, ADBEAudioRatePerSecond, &sampleRate);
    settings->exportParamSuite->GetParamValue(exID, 0, ADBEAudioNumChannels, &channelType);
    csSDK_int32 numAudioChannels = GetNumberOfAudioChannels(channelType.value.intValue);

    csSDK_uint32 audioRenderID = 0;
    settings->sequenceAudioSuite->MakeAudioRenderer(exID,
                                                    exportInfoP->startTime,
                                                    (PrAudioChannelType)channelType.value.intValue,
                                                    kPrAudioSampleType_32BitFloat,
                                                    (float)sampleRate.value.floatValue,
                                                    &audioRenderID);

    PrTime ticksPerSample = 0;
    settings->timeSuite->GetTicksPerAudioSample((float)sampleRate.value.floatValue, &ticksPerSample);

    PrTime exportDuration = exportInfoP->endTime - exportInfoP->startTime;
    csSDK_int64 totalAudioSamples = exportDuration / ticksPerSample;
    csSDK_int64 samplesRemaining = totalAudioSamples;

    // Allocate audio buffers
    csSDK_int32 audioBufferSize = kMaxAudioSamplesPerPacket;
    auto audioBufferOut = (csSDK_int16 *)settings->memorySuite->NewPtr(numAudioChannels * audioBufferSize * kAudioSampleSizeOutput);
    float *audioBuffer[kMaxAudioChannelCount];
    for (csSDK_int32 bufferIndexL = 0; bufferIndexL < numAudioChannels; bufferIndexL++)
    {
        audioBuffer[bufferIndexL] = (float *)settings->memorySuite->NewPtr(audioBufferSize * kAudioSampleSizePremiere);
    }

    // Progress bar init with label
    float progress = 0.f;
	// Annoyingly SetProgressString takes a non-const string argument, so use a buffer
    prUTF16Char tempStrProgress[256];
    SDKStringConvert::to_buffer(L"Preparing Audio...", tempStrProgress);
    settings->exportProgressSuite->SetProgressString(exID, tempStrProgress);

    // GetAudio loop
    csSDK_int32 samplesRequested, maxBlipSize;
    csSDK_int64 samplesExported = 0; // pts
    prMALError resultS = malNoError;
    while (samplesRemaining && (resultS == malNoError))
    {
        // Find size of blip to ask for
        settings->sequenceAudioSuite->GetMaxBlip(audioRenderID, ticksPerFrame.value.timeValue, &maxBlipSize);
        samplesRequested = std::min(audioBufferSize, maxBlipSize);
        if (samplesRequested > samplesRemaining)
            samplesRequested = (csSDK_int32)samplesRemaining;

        // Fill the buffer with audio
        resultS = settings->sequenceAudioSuite->GetAudio(audioRenderID, samplesRequested, audioBuffer, kPrFalse);
        if (resultS != malNoError)
            break;

        settings->audioSuite->ConvertAndInterleaveTo16BitInteger(audioBuffer, audioBufferOut,
                                                                 numAudioChannels, samplesRequested);

        // Write audioBufferOut as one packet
        exporter.writeAudioFrame(reinterpret_cast<const uint8_t *>(audioBufferOut),
                                 int64_t(samplesRequested) * int64_t(numAudioChannels) * int64_t(kAudioSampleSizeOutput),
                                 samplesExported);

        // Write audioBufferOut as separate samples
        // auto buf = reinterpret_cast<const uint8_t *>(audioBufferOut);
        // for (csSDK_int32 i = 0; i < samplesRequested; i++)
        // {
        //     csSDK_int32 offset = i * numAudioChannels * kAudioSampleSizeOutput;
        //     exporter->writeAudioFrame(&buf[offset],
        //                             numAudioChannels * kAudioSampleSizeOutput,
        //                             samplesExported + i);
        // }

        // Calculate remaining audio
        samplesExported += samplesRequested;
        samplesRemaining -= samplesRequested;

        // Update progress bar percent
        progress = (float) samplesExported / totalAudioSamples * 0.06f;
        settings->exportProgressSuite->UpdateProgressPercent(exID, progress);
    }
    error = resultS;

    // Reset progress bar label
    SDKStringConvert::to_buffer(L"", tempStrProgress);
    settings->exportProgressSuite->SetProgressString(exID, tempStrProgress);

    // Free up
    settings->memorySuite->PrDisposePtr((PrMemoryPtr)audioBufferOut);
    for (csSDK_int32 bufferIndexL = 0; bufferIndexL < numAudioChannels; bufferIndexL++)
    {
        settings->memorySuite->PrDisposePtr((PrMemoryPtr)audioBuffer[bufferIndexL]);
    }
    settings->sequenceAudioSuite->ReleaseAudioRenderer(exID, audioRenderID);
}

prMALError doExport(exportStdParms* stdParmsP, exDoExportRec* exportInfoP)
{
    ExportSettings* settings = reinterpret_cast<ExportSettings*>(exportInfoP->privateData);
    prMALError error = malNoError;

    try {
        // if (exportInfoP->exportAudio)
        //     renderAndWriteAllAudio(exportInfoP, error);

        if (exportInfoP->exportVideo)
            renderAndWriteAllVideo(exportInfoP, error);
    }
    catch (const std::exception& ex)
    {
        FDN_ERROR("exception thrown during export", ex.what());
        settings->reportError(ex.what());
        return (error == malNoError) ? malUnknownError : error;
    }
    catch (...)
    {
        FDN_ERROR("unknown exception thrown during export");
        settings->reportError("unspecified error while rendering and writing video");
        return (error == malNoError) ? malUnknownError : error;
    }

    return 	malNoError;
}
