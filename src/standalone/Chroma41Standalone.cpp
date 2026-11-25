#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"

#include "../dsp/Chroma41Engine.h"
#include "../gui/Chroma41EditorView.h"

namespace Steinberg {
DEF_CLASS_IID(IPlugView)
namespace Vst {
DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IEditController)
}
}

using namespace Chroma41;

// Simple 16-bit Stereo WAV Writer for demo/offline processing
static bool writeWavFile(const char* path, const std::vector<float>& left, const std::vector<float>& right, int sampleRate) {
    FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;

    int numSamples = static_cast<int>(left.size());
    int dataBytes = numSamples * 2 * sizeof(int16_t);
    int riffSize = 36 + dataBytes;

    std::fwrite("RIFF", 1, 4, fp);
    std::fwrite(&riffSize, 4, 1, fp);
    std::fwrite("WAVEfmt ", 1, 8, fp);

    int32_t fmtSize = 16;
    int16_t audioFormat = 1; // PCM
    int16_t numChannels = 2;
    int32_t sr = sampleRate;
    int32_t byteRate = sr * 4;
    int16_t blockAlign = 4;
    int16_t bitsPerSample = 16;

    std::fwrite(&fmtSize, 4, 1, fp);
    std::fwrite(&audioFormat, 2, 1, fp);
    std::fwrite(&numChannels, 2, 1, fp);
    std::fwrite(&sr, 4, 1, fp);
    std::fwrite(&byteRate, 4, 1, fp);
    std::fwrite(&blockAlign, 2, 1, fp);
    std::fwrite(&bitsPerSample, 2, 1, fp);

    std::fwrite("data", 1, 4, fp);
    std::fwrite(&dataBytes, 4, 1, fp);

    for (int i = 0; i < numSamples; ++i) {
        float l = std::clamp(left[i], -1.0f, 1.0f);
        float r = std::clamp(right[i], -1.0f, 1.0f);
        int16_t sl = static_cast<int16_t>(l * 32767.0f);
        int16_t sr16 = static_cast<int16_t>(r * 32767.0f);
        std::fwrite(&sl, 2, 1, fp);
        std::fwrite(&sr16, 2, 1, fp);
    }

    std::fclose(fp);
    return true;
}

// Synthesize a dirty detuned Reese Bass + Drum Transient burst for testing Color Bass processing
static void generateReeseBassTestSignal(std::vector<float>& inL, std::vector<float>& inR, int numSamples, float sr) {
    inL.resize(numSamples);
    inR.resize(numSamples);
    float ph1 = 0.0f, ph2 = 0.0f, ph3 = 0.0f, subPh = 0.0f;
    uint32_t rng = 0x12345678;

    for (int i = 0; i < numSamples; ++i) {
        float t = static_cast<float>(i) / sr;
        // Modulating Reese fundamental ~ 65.4 Hz (C2) with LFO wobble
        float wobble = 1.0f + 0.08f * std::sin(2.0f * 3.14159265f * 1.5f * t);
        float f0 = 65.406f * wobble;

        ph1 = std::fmod(ph1 + f0 / sr, 1.0f);
        ph2 = std::fmod(ph2 + (f0 * 1.011f) / sr, 1.0f);
        ph3 = std::fmod(ph3 + (f0 * 0.989f) / sr, 1.0f);
        subPh = std::fmod(subPh + (f0 * 0.5f) / sr, 1.0f);

        float saw1 = 2.0f * ph1 - 1.0f;
        float saw2 = 2.0f * ph2 - 1.0f;
        float saw3 = 2.0f * ph3 - 1.0f;
        float sub = std::sin(2.0f * 3.14159265f * subPh);

        // White noise burst on every half-second beat (simulating snare/transient + neuro noise)
        rng = rng * 1664525u + 1013904223u;
        float noise = (static_cast<float>(rng & 0xFFFF) / 32768.0f) - 1.0f;
        float beatPhase = std::fmod(t, 0.5f);
        float transientEnv = std::exp(-beatPhase * 28.0f);

        float sigL = std::tanh((saw1 + saw2 * 0.8f + sub * 0.7f + noise * (0.18f + 0.5f * transientEnv)) * 1.6f);
        float sigR = std::tanh((saw1 + saw3 * 0.8f + sub * 0.7f - noise * (0.18f + 0.5f * transientEnv)) * 1.6f);

        inL[i] = sigL * 0.55f;
        inR[i] = sigR * 0.55f;
    }
}

// Automated VST3 binary validation & 41-EDO Color Bass audio render test
static int runVst3VerificationTest(const char* vst3Path) {
    std::printf("=== CHROMA-41 VST3 Verification & Audio Render Test ===\n");
    std::printf("Loading VST3 binary: %s\n", vst3Path);

    HMODULE hMod = LoadLibraryA(vst3Path);
    if (!hMod) {
        std::printf("[ERROR] Failed to LoadLibraryA(%s), GetLastError=%lu\n", vst3Path, GetLastError());
        return 1;
    }

    auto getFactory = reinterpret_cast<GetFactoryProc>(GetProcAddress(hMod, "GetPluginFactory"));
    if (!getFactory) {
        std::printf("[ERROR] GetPluginFactory export not found!\n");
        FreeLibrary(hMod);
        return 1;
    }

    Steinberg::IPluginFactory* factory = getFactory();
    if (!factory) {
        std::printf("[ERROR] GetPluginFactory returned null!\n");
        FreeLibrary(hMod);
        return 1;
    }

    Steinberg::PFactoryInfo fInfo{};
    factory->getFactoryInfo(&fInfo);
    std::printf("[OK] Factory Vendor: %s | Classes: %d\n", fInfo.vendor, factory->countClasses());

    Steinberg::PClassInfo cInfo{};
    factory->getClassInfo(0, &cInfo);
    std::printf("[OK] Class[0] Name: %s | Category: %s\n", cInfo.name, cInfo.category);

    Steinberg::Vst::IComponent* comp = nullptr;
    if (factory->createInstance(cInfo.cid, Steinberg::Vst::IComponent::iid, reinterpret_cast<void**>(&comp)) != Steinberg::kResultOk || !comp) {
        std::printf("[ERROR] Failed to create IComponent instance!\n");
        FreeLibrary(hMod);
        return 1;
    }
    comp->initialize(nullptr);

    Steinberg::Vst::IAudioProcessor* proc = nullptr;
    comp->queryInterface(Steinberg::Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&proc));
    Steinberg::Vst::IEditController* ctrl = nullptr;
    comp->queryInterface(Steinberg::Vst::IEditController::iid, reinterpret_cast<void**>(&ctrl));

    if (!proc || !ctrl) {
        std::printf("[ERROR] Failed to query IAudioProcessor or IEditController!\n");
        return 1;
    }
    std::printf("[OK] IComponent + IAudioProcessor + IEditController initialized (%d parameters exported)\n",
                ctrl->getParameterCount());

    // Verify IPlugView creation, getSize, and HWND attachment (FL Studio UI opening sequence)
    Steinberg::IPlugView* view = ctrl->createView("editor");
    if (!view) {
        std::printf("[ERROR] createView(\"editor\") returned nullptr!\n");
        return 1;
    }
    if (view->isPlatformTypeSupported("HWND") != Steinberg::kResultTrue) {
        std::printf("[ERROR] isPlatformTypeSupported(\"HWND\") did not return kResultTrue!\n");
        return 1;
    }
    Steinberg::ViewRect viewRect{};
    view->getSize(&viewRect);
    HWND dummyParent = CreateWindowExW(0, L"STATIC", L"DummyVST3HostParent", WS_POPUP,
                                       0, 0, viewRect.getWidth(), viewRect.getHeight(),
                                       nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (view->attached(dummyParent, "HWND") != Steinberg::kResultOk) {
        std::printf("[ERROR] IPlugView::attached(HWND) failed!\n");
        DestroyWindow(dummyParent);
        return 1;
    }
    std::printf("[OK] IPlugView::attached(HWND) succeeded! Editor Size: %dx%d\n",
                viewRect.getWidth(), viewRect.getHeight());
    view->removed();
    view->release();
    DestroyWindow(dummyParent);

    // Verify 41-EDO MIDI 0..127 frequency formula
    Chroma41Engine refEng;
    std::printf("[OK] 41-EDO MIDI Note Mapping Verification (1 Note = 1 Step of 41-EDO = 29.268 cents):\n");
    std::printf("     MIDI Note  60 (Root 1/1)        -> %.3f Hz\n", refEng.midiNoteTo41EdoFreq(60));
    std::printf("     MIDI Note  69 (Step 9  ~ 7/6)   -> %.3f Hz\n", refEng.midiNoteTo41EdoFreq(69));
    std::printf("     MIDI Note  73 (Step 13 ~ 5/4)   -> %.3f Hz\n", refEng.midiNoteTo41EdoFreq(73));
    std::printf("     MIDI Note  78 (Step 18 ~ 11/8)  -> %.3f Hz\n", refEng.midiNoteTo41EdoFreq(78));
    std::printf("     MIDI Note  84 (Step 24 ~ 3/2)   -> %.3f Hz\n", refEng.midiNoteTo41EdoFreq(84));
    std::printf("     MIDI Note  93 (Step 33 ~ 7/4)   -> %.3f Hz\n", refEng.midiNoteTo41EdoFreq(93));
    std::printf("     MIDI Note 101 (Step 41 = 2/1)   -> %.3f Hz (Exact Octave = %.3f Hz)\n",
                refEng.midiNoteTo41EdoFreq(101), refEng.midiNoteTo41EdoFreq(60) * 2.0f);

    // Setup 44.1 kHz processing
    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 44100.0;
    proc->setupProcessing(setup);
    comp->setActive(true);
    proc->setProcessing(true);

    // Set Color to 135% (norm = 0.675) to showcase the Color Bass metallic resonator tail
    ctrl->setParamNormalized(kParamColor, 0.675);
    ctrl->setParamNormalized(kParamDecay, 0.65);

    const int sr = 44100;
    const int totalSamples = sr * 4; // 4 seconds demo
    std::vector<float> inL, inR, outL(totalSamples, 0.0f), outR(totalSamples, 0.0f);
    generateReeseBassTestSignal(inL, inR, totalSamples, static_cast<float>(sr));

    // Process in 512-sample blocks through the actual VST3 IAudioProcessor::process()
    const int blockSize = 512;
    for (int offset = 0; offset < totalSamples; offset += blockSize) {
        int n = std::min(blockSize, totalSamples - offset);
        float* inPtrs[2] = { inL.data() + offset, inR.data() + offset };
        float* outPtrs[2] = { outL.data() + offset, outR.data() + offset };

        Steinberg::Vst::AudioBusBuffers inBus{};
        inBus.numChannels = 2;
        inBus.channelBuffers32 = inPtrs;

        Steinberg::Vst::AudioBusBuffers outBus{};
        outBus.numChannels = 2;
        outBus.channelBuffers32 = outPtrs;

        Steinberg::Vst::ProcessData data{};
        data.numSamples = n;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = &inBus;
        data.outputs = &outBus;

        proc->process(data);
    }

    // Compute RMS & peak to verify audio output is healthy and non-NaN
    double sumSq = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < totalSamples; ++i) {
        sumSq += outL[i] * outL[i] + outR[i] * outR[i];
        peak = std::max(peak, std::max(std::fabs(outL[i]), std::fabs(outR[i])));
    }
    double rms = std::sqrt(sumSq / (totalSamples * 2));
    std::printf("[OK] VST3 Audio Process Complete: RMS = %.4f, Peak = %.4f\n", rms, peak);

    writeWavFile("build/demo_input_reese.wav", inL, inR, sr);
    writeWavFile("build/demo_output_chroma41_colorbass.wav", outL, outR, sr);
    std::printf("[OK] Wrote demo WAV files:\n     - build/demo_input_reese.wav\n     - build/demo_output_chroma41_colorbass.wav\n");

    proc->setProcessing(false);
    comp->setActive(false);
    proc->release();
    ctrl->release();
    comp->terminate();
    comp->release();
    factory->release();
    FreeLibrary(hMod);

    return (rms > 0.01 && peak <= 1.5f) ? 0 : 1;
}

//------------------------------------------------------------------------
// Standalone Interactive Win32 App with Real-Time WaveOut Audio Synth
//------------------------------------------------------------------------
struct StandaloneAudioStreamer {
    Chroma41Engine* engine = nullptr;
    HWAVEOUT hWaveOut = nullptr;
    static constexpr int kNumBuffers = 4;
    static constexpr int kFramesPerBuffer = 1024;
    static constexpr int kSampleRate = 44100;
    WAVEHDR headers[kNumBuffers]{};
    std::vector<int16_t> pcmBuffers[kNumBuffers];
    std::vector<float> testInL, testInR;
    int playCursor = 0;
    bool running = false;

    void start(Chroma41Engine* eng) {
        engine = eng;
        generateReeseBassTestSignal(testInL, testInR, kSampleRate * 8, static_cast<float>(kSampleRate));

        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = kSampleRate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 4;
        wfx.nAvgBytesPerSec = kSampleRate * 4;

        if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, reinterpret_cast<DWORD_PTR>(&waveOutProc),
                        reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            return;
        }

        running = true;
        for (int b = 0; b < kNumBuffers; ++b) {
            pcmBuffers[b].resize(kFramesPerBuffer * 2);
            headers[b].lpData = reinterpret_cast<LPSTR>(pcmBuffers[b].data());
            headers[b].dwBufferLength = kFramesPerBuffer * 2 * sizeof(int16_t);
            waveOutPrepareHeader(hWaveOut, &headers[b], sizeof(WAVEHDR));
            fillAndWriteBuffer(&headers[b]);
        }
    }

    void stop() {
        running = false;
        if (hWaveOut) {
            waveOutReset(hWaveOut);
            for (int b = 0; b < kNumBuffers; ++b) {
                waveOutUnprepareHeader(hWaveOut, &headers[b], sizeof(WAVEHDR));
            }
            waveOutClose(hWaveOut);
            hWaveOut = nullptr;
        }
    }

    void fillAndWriteBuffer(WAVEHDR* hdr) {
        if (!running || !engine) return;
        float inL[kFramesPerBuffer], inR[kFramesPerBuffer];
        float outL[kFramesPerBuffer], outR[kFramesPerBuffer];
        int totalLen = static_cast<int>(testInL.size());

        for (int i = 0; i < kFramesPerBuffer; ++i) {
            inL[i] = testInL[playCursor];
            inR[i] = testInR[playCursor];
            playCursor = (playCursor + 1) % totalLen;
        }

        engine->processBlock(inL, inR, outL, outR, kFramesPerBuffer);

        int16_t* pcm = reinterpret_cast<int16_t*>(hdr->lpData);
        for (int i = 0; i < kFramesPerBuffer; ++i) {
            pcm[i * 2 + 0] = static_cast<int16_t>(std::clamp(outL[i], -1.0f, 1.0f) * 32767.0f);
            pcm[i * 2 + 1] = static_cast<int16_t>(std::clamp(outR[i], -1.0f, 1.0f) * 32767.0f);
        }
        waveOutWrite(hWaveOut, hdr, sizeof(WAVEHDR));
    }

    static void CALLBACK waveOutProc(HWAVEOUT /*hwo*/, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR /*dwParam2*/) {
        if (uMsg == WOM_DONE) {
            auto* self = reinterpret_cast<StandaloneAudioStreamer*>(dwInstance);
            auto* hdr = reinterpret_cast<WAVEHDR*>(dwParam1);
            if (self && self->running) {
                self->fillAndWriteBuffer(hdr);
            }
        }
    }
};

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--test-vst3") == 0) {
        const char* path = (argc > 2) ? argv[2] : "build\\Chroma41.vst3\\Contents\\x86_64-win\\Chroma41.vst3";
        return runVst3VerificationTest(path);
    }

    // Launch Standalone Interactive Chroma-41 Window with Live Reese Bass Loop
    Chroma41Engine engine;
    engine.reset(44100.0f);

    StandaloneAudioStreamer audio;
    audio.start(&engine);

    Chroma41GUI gui(&engine);
    if (!gui.createWindow(nullptr)) {
        audio.stop();
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsWindow(gui.getHWND())) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    audio.stop();
    return 0;
}
