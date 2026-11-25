#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include "../dsp/Chroma41Engine.h"
#include "../gui/Chroma41EditorView.h"

#include <atomic>
#include <cstring>
#include <cstdio>

namespace Steinberg {
DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)
namespace Vst {
DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IProcessContextRequirements)
DEF_CLASS_IID(IEditController)
DEF_CLASS_IID(IComponentHandler)
DEF_CLASS_IID(IConnectionPoint)
DEF_CLASS_IID(IMessage)
DEF_CLASS_IID(IEventList)
DEF_CLASS_IID(IParameterChanges)
DEF_CLASS_IID(IParamValueQueue)
} // namespace Vst
} // namespace Steinberg

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace Chroma41;

// Unique Class IDs for Chroma-41 Processor & Controller
static const FUID kChroma41ClassUID(0x41ED0C80, 0x7A914B32, 0x9C41E041, 0xC804A041);
static const FUID kChroma41ControllerUID(0x41ED0C81, 0x7A914B32, 0x9C41E041, 0xC804A042);

static void copyAsciiToUTF16(char16* dst, const char* src, int32 maxChars = 128) {
    int32 i = 0;
    for (; i < maxChars - 1 && src && src[i] != '\0'; ++i) {
        dst[i] = static_cast<char16>(static_cast<unsigned char>(src[i]));
    }
    dst[i] = 0;
}

//------------------------------------------------------------------------
// IPlugView Implementation for Win32 HWND
//------------------------------------------------------------------------
class Chroma41PlugView : public IPlugView {
public:
    Chroma41PlugView(Chroma41Engine* eng, IComponentHandler* handler)
        : engine(eng), componentHandler(handler) {}

    virtual ~Chroma41PlugView() {
        if (gui) {
            gui->destroyWindow();
            delete gui;
            gui = nullptr;
        }
    }

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IPlugView)
        QUERY_INTERFACE(_iid, obj, IPlugView::iid, IPlugView)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override {
        uint32 r = --refCount;
        if (r == 0) delete this;
        return r;
    }

    // IPlugView
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        if (type && std::strcmp(type, kPlatformTypeHWND) == 0)
            return kResultTrue;
        return kResultFalse;
    }

    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (isPlatformTypeSupported(type) != kResultTrue) return kResultFalse;
        if (!gui) {
            gui = new Chroma41GUI(engine, [this](uint32_t paramId, float normVal) {
                if (componentHandler) {
                    componentHandler->beginEdit(paramId);
                    componentHandler->performEdit(paramId, normVal);
                    componentHandler->endEdit(paramId);
                }
            });
        }
        if (gui->createWindow(static_cast<HWND>(parent))) {
            if (plugFrame) {
                ViewRect desired(0, 0, Chroma41GUI::kWidth, Chroma41GUI::kHeight);
                plugFrame->resizeView(this, &desired);
            }
            return kResultOk;
        }
        return kResultFalse;
    }

    tresult PLUGIN_API removed() override {
        if (gui) {
            gui->destroyWindow();
        }
        return kResultOk;
    }

    tresult PLUGIN_API onWheel(float /*distance*/) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16 /*key*/, int16 /*keyCode*/, int16 /*modifiers*/) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16 /*key*/, int16 /*keyCode*/, int16 /*modifiers*/) override { return kResultFalse; }

    tresult PLUGIN_API getSize(ViewRect* size) override {
        if (!size) return kInvalidArgument;
        *size = currentRect;
        return kResultOk;
    }

    tresult PLUGIN_API onSize(ViewRect* newSize) override {
        if (!newSize) return kInvalidArgument;
        if (newSize->getWidth() > 0 && newSize->getHeight() > 0) {
            currentRect = *newSize;
        }
        if (gui && gui->getHWND()) {
            SetWindowPos(gui->getHWND(), nullptr, 0, 0,
                         currentRect.getWidth(), currentRect.getHeight(),
                         SWP_NOZORDER | SWP_NOMOVE | SWP_SHOWWINDOW);
            InvalidateRect(gui->getHWND(), nullptr, FALSE);
        }
        return kResultOk;
    }

    tresult PLUGIN_API onFocus(TBool /*state*/) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame* frame) override {
        plugFrame = frame;
        return kResultOk;
    }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override {
        if (!rect) return kInvalidArgument;
        if (rect->getWidth() < 640) rect->right = rect->left + 640;
        if (rect->getHeight() < 420) rect->bottom = rect->top + 420;
        return kResultOk;
    }

private:
    std::atomic<uint32> refCount{1};
    Chroma41Engine* engine = nullptr;
    IComponentHandler* componentHandler = nullptr;
    IPlugFrame* plugFrame = nullptr;
    Chroma41GUI* gui = nullptr;
    ViewRect currentRect{0, 0, Chroma41GUI::kWidth, Chroma41GUI::kHeight};
};

//------------------------------------------------------------------------
// Single-Component VST3 Processor + EditController
//------------------------------------------------------------------------
class Chroma41Plugin : public IComponent,
                       public IAudioProcessor,
                       public IProcessContextRequirements,
                       public IEditController,
                       public IConnectionPoint {
public:
    bool controllerClaimed = false;
    static Chroma41Plugin* sLastUnpairedPlugin;

    Chroma41Plugin() {
        buildParamDefinitions();
    }

    virtual ~Chroma41Plugin() {
        if (sLastUnpairedPlugin == this) {
            sLastUnpairedPlugin = nullptr;
        }
        if (componentHandler) {
            componentHandler->release();
            componentHandler = nullptr;
        }
    }

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, IEditController::iid)) {
            controllerClaimed = true;
        }
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IComponent)
        QUERY_INTERFACE(_iid, obj, IPluginBase::iid, IComponent)
        QUERY_INTERFACE(_iid, obj, IComponent::iid, IComponent)
        QUERY_INTERFACE(_iid, obj, IAudioProcessor::iid, IAudioProcessor)
        QUERY_INTERFACE(_iid, obj, IProcessContextRequirements::iid, IProcessContextRequirements)
        QUERY_INTERFACE(_iid, obj, IEditController::iid, IEditController)
        QUERY_INTERFACE(_iid, obj, IConnectionPoint::iid, IConnectionPoint)
        *obj = nullptr;
        return kNoInterface;
    }

    // IConnectionPoint
    tresult PLUGIN_API connect(IConnectionPoint* other) override {
        return other ? kResultOk : kInvalidArgument;
    }
    tresult PLUGIN_API disconnect(IConnectionPoint* /*other*/) override {
        return kResultOk;
    }
    tresult PLUGIN_API notify(IMessage* /*message*/) override {
        return kResultOk;
    }

    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override {
        uint32 r = --refCount;
        if (r == 0) delete this;
        return r;
    }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown* /*context*/) override {
        return kResultOk;
    }

    tresult PLUGIN_API terminate() override {
        if (componentHandler) {
            componentHandler->release();
            componentHandler = nullptr;
        }
        return kResultOk;
    }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID classId) override {
        std::memcpy(classId, kChroma41ControllerUID.toTUID(), sizeof(TUID));
        return kResultOk;
    }

    tresult PLUGIN_API setIoMode(IoMode /*mode*/) override { return kResultOk; }

    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
        if (type == kAudio && (dir == kInput || dir == kOutput)) return 1;
        if (type == kEvent && dir == kInput) return 1; // MIDI Note 0..127 input bus!
        return 0;
    }

    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (index != 0) return kInvalidArgument;
        if (type == kAudio && dir == kInput) {
            bus.mediaType = kAudio;
            bus.direction = kInput;
            bus.channelCount = 2;
            copyAsciiToUTF16(bus.name, "Stereo In");
            bus.busType = kMain;
            bus.flags = BusInfo::kDefaultActive;
            return kResultOk;
        }
        if (type == kAudio && dir == kOutput) {
            bus.mediaType = kAudio;
            bus.direction = kOutput;
            bus.channelCount = 2;
            copyAsciiToUTF16(bus.name, "Stereo Out");
            bus.busType = kMain;
            bus.flags = BusInfo::kDefaultActive;
            return kResultOk;
        }
        if (type == kEvent && dir == kInput) {
            bus.mediaType = kEvent;
            bus.direction = kInput;
            bus.channelCount = 16;
            copyAsciiToUTF16(bus.name, "41-EDO MIDI In");
            bus.busType = kMain;
            bus.flags = BusInfo::kDefaultActive;
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult PLUGIN_API getRoutingInfo(RoutingInfo& /*inInfo*/, RoutingInfo& /*outInfo*/) override {
        return kNotImplemented;
    }

    tresult PLUGIN_API activateBus(MediaType /*type*/, BusDirection /*dir*/, int32 index, TBool /*state*/) override {
        return (index == 0) ? kResultOk : kInvalidArgument;
    }

    tresult PLUGIN_API setActive(TBool state) override {
        if (state) {
            engine.reset(currentSampleRate);
        } else {
            engine.allNotesOff();
        }
        return kResultOk;
    }

    // State serialization (shared by Component & Controller)
    struct SavedStateBlob {
        uint32_t magic = 0x43483431; // 'CH41'
        uint32_t version = 2;
        float params[14];
        uint8_t steps[kStepsPerOctave];
    };

    tresult PLUGIN_API setState(IBStream* state) override {
        if (!state) return kInvalidArgument;
        SavedStateBlob blob{};
        int32 numRead = 0;
        if (state->read(&blob, sizeof(SavedStateBlob), &numRead) == kResultOk &&
            numRead == sizeof(SavedStateBlob) && blob.magic == 0x43483431) {
            uint32_t paramCount = (blob.version >= 2) ? 14u : 13u;
            for (uint32_t i = 0; i < paramCount; ++i) {
                engine.setParameterNormalized(i, blob.params[i]);
            }
            for (int s = 0; s < kStepsPerOctave; ++s) {
                engine.setStepActive(s, blob.steps[s] != 0);
            }
            return kResultOk;
        }
        return kResultFalse;
    }

    tresult PLUGIN_API getState(IBStream* state) override {
        if (!state) return kInvalidArgument;
        SavedStateBlob blob{};
        for (uint32_t i = 0; i < 14; ++i) {
            blob.params[i] = engine.getParameterNormalized(i);
        }
        for (int s = 0; s < kStepsPerOctave; ++s) {
            blob.steps[s] = engine.isStepActive(s) ? 1 : 0;
        }
        int32 numWritten = 0;
        state->write(&blob, sizeof(SavedStateBlob), &numWritten);
        return (numWritten == sizeof(SavedStateBlob)) ? kResultOk : kResultFalse;
    }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                          SpeakerArrangement* outputs, int32 numOuts) override {
        if (numIns == 1 && numOuts == 1 &&
            inputs[0] == SpeakerArr::kStereo && outputs[0] == SpeakerArr::kStereo) {
            return kResultTrue;
        }
        return kResultFalse;
    }

    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
        if (index != 0) return kInvalidArgument;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override {
        return (symbolicSampleSize == kSample32) ? kResultTrue : kResultFalse;
    }

    uint32 PLUGIN_API getLatencySamples() override {
        return 0; // Zero-latency resonator path + parallel spectral enhancer
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        currentSampleRate = static_cast<float>(setup.sampleRate);
        engine.reset(currentSampleRate);
        return kResultOk;
    }

    tresult PLUGIN_API setProcessing(TBool /*state*/) override {
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData& data) override {
        // 1. Apply incoming parameter automation changes
        if (data.inputParameterChanges) {
            int32 numChanges = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < numChanges; ++i) {
                if (IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i)) {
                    int32 numPoints = queue->getPointCount();
                    if (numPoints > 0) {
                        int32 sampleOffset = 0;
                        ParamValue val = 0.0;
                        if (queue->getPoint(numPoints - 1, sampleOffset, val) == kResultOk) {
                            engine.setParameterNormalized(queue->getParameterId(), static_cast<float>(val));
                        }
                    }
                }
            }
        }

        // 2. Process incoming MIDI Note On / Note Off events (Notes 0..127 -> 41-EDO steps)
        if (data.inputEvents) {
            int32 numEvents = data.inputEvents->getEventCount();
            for (int32 i = 0; i < numEvents; ++i) {
                Event ev{};
                if (data.inputEvents->getEvent(i, ev) == kResultOk) {
                    if (ev.type == Event::kNoteOnEvent) {
                        engine.noteOn(ev.noteOn.pitch, ev.noteOn.velocity);
                    } else if (ev.type == Event::kNoteOffEvent) {
                        engine.noteOff(ev.noteOff.pitch);
                    }
                }
            }
        }

        // 3. Process Audio
        if (data.numSamples <= 0 || data.numOutputs == 0 || !data.outputs) {
            return kResultOk;
        }

        AudioBusBuffers& outBus = data.outputs[0];
        if (outBus.numChannels < 1 || !outBus.channelBuffers32) {
            return kResultOk;
        }

        float* outL = outBus.channelBuffers32[0];
        float* outR = (outBus.numChannels > 1) ? outBus.channelBuffers32[1] : outL;

        const float* inL = nullptr;
        const float* inR = nullptr;
        if (data.numInputs > 0 && data.inputs && data.inputs[0].numChannels > 0 && data.inputs[0].channelBuffers32) {
            inL = data.inputs[0].channelBuffers32[0];
            inR = (data.inputs[0].numChannels > 1) ? data.inputs[0].channelBuffers32[1] : inL;
        }

        engine.processBlock(inL, inR, outL, outR, data.numSamples);
        outBus.silenceFlags = 0;
        return kResultOk;
    }

    uint32 PLUGIN_API getTailSamples() override {
        // Up to 2 seconds resonator tail
        return static_cast<uint32>(currentSampleRate * 2.0f);
    }

    // IProcessContextRequirements
    uint32 PLUGIN_API getProcessContextRequirements() override {
        return 0;
    }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream* state) override {
        return setState(state);
    }

    int32 PLUGIN_API getParameterCount() override {
        return static_cast<int32>(paramInfos.size());
    }

    tresult PLUGIN_API getParameterInfo(int32 paramIndex, ParameterInfo& info) override {
        if (paramIndex < 0 || paramIndex >= static_cast<int32>(paramInfos.size()))
            return kInvalidArgument;
        info = paramInfos[paramIndex];
        return kResultOk;
    }

    tresult PLUGIN_API getParamStringByValue(Steinberg::Vst::ParamID id, ParamValue valueNormalized, String128 string) override {
        char buf[64] = "";
        float norm = static_cast<float>(valueNormalized);
        switch (id) {
            case kParamColor:
                std::snprintf(buf, sizeof(buf), "%.1f %%", norm * 200.0f);
                break;
            case kParamDecay:
                std::snprintf(buf, sizeof(buf), "%.0f ms", 25.0f + std::pow(norm, 1.5f) * 1975.0f);
                break;
            case kParamSpread:
            case kParamSpectralSnap:
            case kParamTransient:
            case kParamDryLevel:
                std::snprintf(buf, sizeof(buf), "%.0f %%", norm * 100.0f);
                break;
            case kParamSpectralGate:
                if (norm <= 0.01f) std::snprintf(buf, sizeof(buf), "Off");
                else std::snprintf(buf, sizeof(buf), "%.1f dB", -60.0f + norm * 52.0f);
                break;
            case kParamLowCut:
                std::snprintf(buf, sizeof(buf), "%.0f Hz", 30.0f * std::pow(800.0f / 30.0f, norm));
                break;
            case kParamHighCut:
                std::snprintf(buf, sizeof(buf), "%.0f Hz", 1000.0f * std::pow(16000.0f / 1000.0f, norm));
                break;
            case kParamRefFreq:
                std::snprintf(buf, sizeof(buf), "%.2f Hz", 220.0f + norm * 220.0f);
                break;
            case kParamRootStep:
                std::snprintf(buf, sizeof(buf), "Step %d / 41", static_cast<int>(std::round(norm * 40.0f)));
                break;
            case kParamMidiMode: {
                int m = std::clamp(static_cast<int>(std::round(norm * 2.0f)), 0, 2);
                const char* names[3] = { "Fold 41-EDO Scale", "Direct 0..127 (1Note=1Step)", "UI Scale Only" };
                std::snprintf(buf, sizeof(buf), "%s", names[m]);
                break;
            }
            case kParamOutputGain:
                std::snprintf(buf, sizeof(buf), "%+.1f dB", -12.0f + norm * 24.0f);
                break;
            case kParamPresetIdx: {
                auto presets = getScalePresets41();
                int idx = std::clamp(static_cast<int>(std::round(norm * (presets.size() - 1))), 0, static_cast<int>(presets.size() - 1));
                std::snprintf(buf, sizeof(buf), "%s", presets[idx].name);
                break;
            }
            default:
                if (id >= kParamStepBase && id < kParamStepBase + kStepsPerOctave) {
                    std::snprintf(buf, sizeof(buf), "%s", norm >= 0.5f ? "Active" : "Muted");
                } else {
                    std::snprintf(buf, sizeof(buf), "%.2f", norm);
                }
                break;
        }
        copyAsciiToUTF16(string, buf);
        return kResultOk;
    }

    tresult PLUGIN_API getParamValueByString(Steinberg::Vst::ParamID /*id*/, TChar* /*string*/, ParamValue& /*valueNormalized*/) override {
        return kNotImplemented;
    }

    ParamValue PLUGIN_API normalizedParamToPlain(Steinberg::Vst::ParamID /*id*/, ParamValue valueNormalized) override {
        return valueNormalized;
    }

    ParamValue PLUGIN_API plainParamToNormalized(Steinberg::Vst::ParamID /*id*/, ParamValue plainValue) override {
        return plainValue;
    }

    ParamValue PLUGIN_API getParamNormalized(Steinberg::Vst::ParamID id) override {
        return engine.getParameterNormalized(id);
    }

    tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID id, ParamValue value) override {
        engine.setParameterNormalized(id, static_cast<float>(value));
        return kResultOk;
    }

    tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) override {
        if (componentHandler == handler) return kResultOk;
        if (componentHandler) componentHandler->release();
        componentHandler = handler;
        if (componentHandler) componentHandler->addRef();
        return kResultOk;
    }

    IPlugView* PLUGIN_API createView(FIDString name) override {
        if (name && std::strcmp(name, ViewType::kEditor) == 0) {
            return new Chroma41PlugView(&engine, componentHandler);
        }
        return nullptr;
    }

private:
    std::atomic<uint32> refCount{1};
    Chroma41Engine engine;
    float currentSampleRate = 44100.0f;
    IComponentHandler* componentHandler = nullptr;
    std::vector<ParameterInfo> paramInfos;

    void addParam(uint32_t id, const char* title, const char* shortTitle, const char* units,
                  int32 stepCount, float defaultNorm) {
        ParameterInfo info{};
        info.id = id;
        copyAsciiToUTF16(info.title, title);
        copyAsciiToUTF16(info.shortTitle, shortTitle);
        copyAsciiToUTF16(info.units, units);
        info.stepCount = stepCount;
        info.defaultNormalizedValue = defaultNorm;
        info.unitId = 0;
        info.flags = ParameterInfo::kCanAutomate;
        paramInfos.push_back(info);
    }

    void buildParamDefinitions() {
        addParam(kParamColor,        "Color",          "Color",   "%",    0,  0.575f);
        addParam(kParamDecay,        "Decay Tail",     "Decay",   "ms",   0,  0.55f);
        addParam(kParamSpread,       "Shimmer Spread", "Spread",  "%",    0,  0.65f);
        addParam(kParamSpectralGate, "Spectral Gate",  "Gate",    "dB",   0,  0.25f);
        addParam(kParamTransient,    "Transient",      "Trans",   "%",    0,  0.35f);
        addParam(kParamLowCut,       "Low Cut",        "LowCut",  "Hz",   0,  0.35f);
        addParam(kParamHighCut,      "High Cut",       "HiCut",   "Hz",   0,  0.72f);
        addParam(kParamSpectralSnap, "Spectral Snap",  "Snap",    "%",    0,  0.60f);
        addParam(kParamRefFreq,      "Ref Pitch C4",   "RefHz",   "Hz",   0,  0.1892f);
        addParam(kParamRootStep,     "41-EDO Root",    "Root41",  "Step", 40, 0.0f);
        addParam(kParamMidiMode,     "MIDI Mode",      "MidiMd",  "",     2,  0.0f);
        addParam(kParamOutputGain,   "Output Gain",    "Gain",    "dB",   0,  0.5f);
        addParam(kParamPresetIdx,    "Scale Preset",   "Preset",  "",     10, 0.4f);
        addParam(kParamDryLevel,     "Dry Level",      "Dry",     "%",    0,  0.25f);

        for (int s = 0; s < kStepsPerOctave; ++s) {
            char title[48], shortT[16];
            std::snprintf(title, sizeof(title), "41-EDO Step %d (%s)", s, kEdo41StepTable[s].name);
            std::snprintf(shortT, sizeof(shortT), "Step %d", s);
            addParam(kParamStepBase + s, title, shortT, "", 1, engine.isStepActive(s) ? 1.0f : 0.0f);
        }
    }
};

Chroma41Plugin* Chroma41Plugin::sLastUnpairedPlugin = nullptr;

//------------------------------------------------------------------------
// VST3 Plugin Factory (IPluginFactory3)
//------------------------------------------------------------------------
class Chroma41Factory : public IPluginFactory3 {
public:
    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IPluginFactory3)
        QUERY_INTERFACE(_iid, obj, IPluginFactory::iid, IPluginFactory3)
        QUERY_INTERFACE(_iid, obj, IPluginFactory2::iid, IPluginFactory3)
        QUERY_INTERFACE(_iid, obj, IPluginFactory3::iid, IPluginFactory3)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override { return --refCount; }

    // IPluginFactory
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        if (!info) return kInvalidArgument;
        *info = PFactoryInfo("Fleschkyx Audio", "https://github.com/fleschkyx/chroma41",
                             "info@fleschkyx.audio", PFactoryInfo::kUnicode);
        return kResultOk;
    }

    int32 PLUGIN_API countClasses() override { return 2; }

    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (!info) return kInvalidArgument;
        if (index == 0) {
            *info = PClassInfo(kChroma41ClassUID.toTUID(), PClassInfo::kManyInstances,
                               kVstAudioEffectClass, "Chroma-41 (41-EDO)");
            return kResultOk;
        }
        if (index == 1) {
            *info = PClassInfo(kChroma41ControllerUID.toTUID(), PClassInfo::kManyInstances,
                               kVstComponentControllerClass, "Chroma-41 (41-EDO) Controller");
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (!info) return kInvalidArgument;
        if (index == 0) {
            *info = PClassInfo2(kChroma41ClassUID.toTUID(), PClassInfo::kManyInstances,
                                kVstAudioEffectClass, "Chroma-41 (41-EDO)",
                                0, PlugType::kFxPitchShift,
                                "Fleschkyx Audio", "1.0.1", "VST 3.7.9");
            return kResultOk;
        }
        if (index == 1) {
            *info = PClassInfo2(kChroma41ControllerUID.toTUID(), PClassInfo::kManyInstances,
                                kVstComponentControllerClass, "Chroma-41 (41-EDO) Controller",
                                0, "",
                                "Fleschkyx Audio", "1.0.1", "VST 3.7.9");
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW* info) override {
        if (!info || (index != 0 && index != 1)) return kInvalidArgument;
        PClassInfo2 ci2;
        getClassInfo2(index, &ci2);
        info->fromAscii(ci2);
        return kResultOk;
    }

    tresult PLUGIN_API setHostContext(FUnknown* /*context*/) override {
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        FUID requestedCID = FUID::fromTUID(cid);
        if (requestedCID == kChroma41ClassUID || requestedCID == kChroma41ControllerUID) {
            if ((FUnknownPrivate::iidEqual(_iid, IEditController::iid) || requestedCID == kChroma41ControllerUID) &&
                Chroma41Plugin::sLastUnpairedPlugin != nullptr && !Chroma41Plugin::sLastUnpairedPlugin->controllerClaimed) {
                Chroma41Plugin* existing = Chroma41Plugin::sLastUnpairedPlugin;
                Chroma41Plugin::sLastUnpairedPlugin = nullptr;
                existing->controllerClaimed = true;
                return existing->queryInterface(_iid, obj);
            }

            auto* instance = new Chroma41Plugin();
            if (!FUnknownPrivate::iidEqual(_iid, IEditController::iid) && requestedCID != kChroma41ControllerUID) {
                Chroma41Plugin::sLastUnpairedPlugin = instance;
            } else {
                instance->controllerClaimed = true;
            }
            tresult res = instance->queryInterface(_iid, obj);
            instance->release();
            return res;
        }
        return kResultFalse;
    }

private:
    std::atomic<uint32> refCount{1};
};

extern "C" {

SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() {
    static Chroma41Factory factory;
    factory.addRef();
    return &factory;
}

SMTG_EXPORT_SYMBOL bool InitDll() { return true; }
SMTG_EXPORT_SYMBOL bool ExitDll() { return true; }

} // extern "C"
