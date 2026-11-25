#pragma once

#include "Chroma41DSP.h"
#include <complex>
#include <mutex>

namespace Chroma41 {

// Parameter IDs exposed to VST3 host & GUI
enum ParamID : uint32_t {
    kParamColor = 0,        // 0.0 .. 1.0 -> 0% .. 200% (Default 0.5 = 100%)
    kParamDecay = 1,        // 0.0 .. 1.0 -> 20 ms .. 2000 ms
    kParamSpread = 2,       // 0.0 .. 1.0 -> 0% .. 100% stereo detune & width
    kParamSpectralGate = 3, // 0.0 .. 1.0 -> -60 dB .. -6 dB
    kParamTransient = 4,    // 0.0 .. 1.0 -> 0% .. 100% transient punch preservation
    kParamLowCut = 5,       // 0.0 .. 1.0 -> 30 Hz .. 800 Hz
    kParamHighCut = 6,      // 0.0 .. 1.0 -> 1000 Hz .. 16000 Hz
    kParamSpectralSnap = 7, // 0.0 .. 1.0 -> 0% .. 100% FFT Spectral Pitch-Map vs Pure Resonator
    kParamRefFreq = 8,      // 0.0 .. 1.0 -> 220 Hz .. 440 Hz (Default Note 60 = 261.6256 Hz)
    kParamRootStep = 9,     // 0.0 .. 1.0 -> 0 .. 40 (41-EDO root offset)
    kParamMidiMode = 10,    // 0.0 .. 1.0 -> 0: Fold 41 (Scale+MIDI), 1: Direct 0-127, 2: UI Scale Only
    kParamOutputGain = 11,  // 0.0 .. 1.0 -> -12 dB .. +12 dB
    kParamPresetIdx = 12,   // 0.0 .. 1.0 -> Scale preset selector
    kParamDryLevel = 13,    // 0.0 .. 1.0 -> 0% .. 100% independent Dry signal level

    // 41 step toggles (kParamStepBase + 0 .. kParamStepBase + 40)
    kParamStepBase = 100,
    kParamTotalCount = 141
};

class Chroma41Engine {
public:
    Chroma41Engine() {
        initWindow();
        applyPreset(4); // Default: "Septimal Color Bass (7-Limit)" in 41-EDO
        reset(44100.0f);
    }

    void reset(float sr) {
        sampleRate = (sr > 8000.0f) ? sr : 44100.0f;
        for (auto& v : voices) v.reset();
        lowCutFilter.reset();
        highCutFilter.reset();
        outAirFilter.reset();

        fftInFIFO_L.fill(0.0f);
        fftInFIFO_R.fill(0.0f);
        fftOutAccum_L.fill(0.0f);
        fftOutAccum_R.fill(0.0f);
        prevPhaseL.fill(0.0f);
        prevPhaseR.fill(0.0f);
        synthPhaseL.fill(0.0f);
        synthPhaseR.fill(0.0f);
        visualSpectrumIn.fill(0.0f);
        visualSpectrumOut.fill(0.0f);
        visualStepEnergy.fill(0.0f);
        fftPos = 0;

        fastEnv = 0.0f;
        slowEnv = 0.0f;
        needRebuildVoices = true;
        rebuildVoices();
    }

    void applyPreset(int presetIndex) {
        auto presets = getScalePresets41();
        if (presetIndex < 0 || presetIndex >= static_cast<int>(presets.size())) return;
        uiScaleSteps.fill(false);
        for (int s : presets[presetIndex].steps) {
            if (s >= 0 && s < kStepsPerOctave) {
                uiScaleSteps[s] = true;
            }
        }
        needRebuildVoices = true;
    }

    void setStepActive(int step, bool active) {
        if (step >= 0 && step < kStepsPerOctave) {
            if (uiScaleSteps[step] != active) {
                uiScaleSteps[step] = active;
                needRebuildVoices = true;
            }
        }
    }

    bool isStepActive(int step) const {
        if (step >= 0 && step < kStepsPerOctave) return uiScaleSteps[step];
        return false;
    }

    // MIDI Note 0..127 handling (1 MIDI note step = 1 step of 41-EDO!)
    void noteOn(int midiNote, float velocity) {
        if (midiNote >= 0 && midiNote < kNumMidiNotes) {
            if (velocity > 0.0f) {
                midiHeldNotes[midiNote] = true;
            } else {
                midiHeldNotes[midiNote] = false;
            }
            updateMidiState();
        }
    }

    void noteOff(int midiNote) {
        if (midiNote >= 0 && midiNote < kNumMidiNotes) {
            midiHeldNotes[midiNote] = false;
            updateMidiState();
        }
    }

    void allNotesOff() {
        midiHeldNotes.fill(false);
        updateMidiState();
    }

    // Convert MIDI note number (0..127) directly to 41-EDO frequency
    // Note 60 = refFreqHz * 2^(rootStep / 41)
    // Each MIDI note +1 is +1 step of 41-EDO (2^(1/41))
    float midiNoteTo41EdoFreq(int midiNote) const {
        float stepOffset = static_cast<float>(midiNote - 60 + rootStep);
        return refFreqHz * std::pow(2.0f, stepOffset / static_cast<float>(kStepsPerOctave));
    }

    void setParameterNormalized(uint32_t id, float norm) {
        norm = std::clamp(norm, 0.0f, 1.0f);
        switch (id) {
            case kParamColor:
                colorAmount = norm * 2.0f; // 0.0 .. 2.0 (0% .. 200%)
                break;
            case kParamDecay:
                decayNormalized = norm;
                needRebuildVoices = true;
                break;
            case kParamSpread:
                spreadNormalized = norm;
                needRebuildVoices = true;
                break;
            case kParamSpectralGate:
                gateNormalized = norm;
                break;
            case kParamTransient:
                transientAmount = norm;
                break;
            case kParamLowCut:
                lowCutHz = 30.0f * std::pow(800.0f / 30.0f, norm);
                needRebuildVoices = true;
                break;
            case kParamHighCut:
                highCutHz = 1000.0f * std::pow(16000.0f / 1000.0f, norm);
                needRebuildVoices = true;
                break;
            case kParamSpectralSnap:
                spectralSnapAmount = norm;
                break;
            case kParamRefFreq:
                refFreqHz = 220.0f + norm * 220.0f; // 220 .. 440 Hz
                needRebuildVoices = true;
                break;
            case kParamRootStep: {
                int newRoot = std::clamp(static_cast<int>( std::round(norm * 40.0f) ), 0, 40);
                if (newRoot != rootStep) {
                    rootStep = newRoot;
                    needRebuildVoices = true;
                }
                break;
            }
            case kParamMidiMode: {
                int newMode = std::clamp(static_cast<int>( std::round(norm * 2.0f) ), 0, 2);
                if (newMode != midiMode) {
                    midiMode = newMode;
                    needRebuildVoices = true;
                }
                break;
            }
            case kParamOutputGain:
                outputGainDb = -12.0f + norm * 24.0f;
                outputGainLin = std::pow(10.0f, outputGainDb / 20.0f);
                break;
            case kParamPresetIdx: {
                auto presets = getScalePresets41();
                int idx = std::clamp(static_cast<int>(std::round(norm * static_cast<float>(presets.size() - 1))),
                                     0, static_cast<int>(presets.size() - 1));
                if (idx != currentPresetIdx) {
                    currentPresetIdx = idx;
                    applyPreset(idx);
                }
                break;
            }
            case kParamDryLevel:
                dryLevel = norm;
                break;
            default:
                if (id >= kParamStepBase && id < kParamStepBase + kStepsPerOctave) {
                    setStepActive(static_cast<int>(id - kParamStepBase), norm >= 0.5f);
                }
                break;
        }
    }

    float getParameterNormalized(uint32_t id) const {
        switch (id) {
            case kParamColor:        return colorAmount * 0.5f;
            case kParamDecay:        return decayNormalized;
            case kParamSpread:       return spreadNormalized;
            case kParamSpectralGate: return gateNormalized;
            case kParamTransient:    return transientAmount;
            case kParamLowCut:       return std::log(lowCutHz / 30.0f) / std::log(800.0f / 30.0f);
            case kParamHighCut:      return std::log(highCutHz / 1000.0f) / std::log(16000.0f / 1000.0f);
            case kParamSpectralSnap: return spectralSnapAmount;
            case kParamRefFreq:      return (refFreqHz - 220.0f) / 220.0f;
            case kParamRootStep:     return static_cast<float>(rootStep) / 40.0f;
            case kParamMidiMode:     return static_cast<float>(midiMode) / 2.0f;
            case kParamOutputGain:   return (outputGainDb + 12.0f) / 24.0f;
            case kParamPresetIdx: {
                auto presets = getScalePresets41();
                return static_cast<float>(currentPresetIdx) / static_cast<float>(std::max(1, static_cast<int>(presets.size()) - 1));
            }
            case kParamDryLevel:     return dryLevel;
            default:
                if (id >= kParamStepBase && id < kParamStepBase + kStepsPerOctave) {
                    return isStepActive(static_cast<int>(id - kParamStepBase)) ? 1.0f : 0.0f;
                }
                return 0.0f;
        }
    }

    // Effective active state of each of the 41 pitch classes (for UI glow & scale display)
    bool isEffectivePitchClassActive(int step41) const {
        if (step41 < 0 || step41 >= kStepsPerOctave) return false;
        if (midiMode == 0 && anyMidiHeld) {
            return midiPitchClassActive[step41];
        } else if (midiMode == 1 && anyMidiHeld) {
            return midiPitchClassActive[step41];
        }
        return uiScaleSteps[step41];
    }

    void processBlock(const float* inL, const float* inR, float* outL, float* outR, int numSamples) {
        if (needRebuildVoices) {
            rebuildVoices();
        }

        // Color 0..1 = Dry/Wet mix (0%..100%)
        // Color 1..2 = Metallic Resonator Overdrive & Shimmer Tail (100%..200%)
        const float wetMix = std::clamp(colorAmount, 0.0f, 1.0f);
        const float hyperColor = std::clamp(colorAmount - 1.0f, 0.0f, 1.0f);

        // Gate threshold in linear amplitude
        const float gateThresh = (gateNormalized <= 0.01f)
            ? 0.0f
            : std::pow(10.0f, (-60.0f + gateNormalized * 52.0f) / 20.0f);

        const int numVoices = activeVoiceCount;
        // Normalization factor so summing N parallel resonators stays balanced
        const float normVoiceGain = (numVoices > 0)
            ? (2.6f / std::sqrt(static_cast<float>(std::max(4, numVoices))))
            : 1.0f;

        // Ringing resonator boost at Color 100..200%
        const float ringMix = 0.25f + hyperColor * 2.25f;
        const float tightMix = 1.0f - hyperColor * 0.35f;

        for (int i = 0; i < numSamples; ++i) {
            float dryL = inL ? inL[i] : 0.0f;
            float dryR = inR ? inR[i] : dryL;

            // 1. Transient Detector (fast attack vs slow attack envelope difference)
            float absIn = 0.5f * (std::fabs(dryL) + std::fabs(dryR));
            fastEnv = 0.985f * fastEnv + 0.015f * absIn;
            slowEnv = 0.9992f * slowEnv + 0.0008f * absIn;
            float transientRatio = (fastEnv > slowEnv + 1e-5f)
                ? std::clamp((fastEnv - slowEnv) / (fastEnv + 0.01f), 0.0f, 1.0f)
                : 0.0f;
            float transientDuck = 1.0f - transientAmount * transientRatio * 0.85f;

            // 2. Band-limit input to [lowCutHz, highCutHz] + keep Out-of-Band lows/highs intact!
            float bandL = 0.0f, bandR = 0.0f;
            float lowKeepL = 0.0f, lowKeepR = 0.0f;
            lowCutFilter.processLowpass(dryL, dryR, lowKeepL, lowKeepR);
            float hpL = dryL - lowKeepL;
            float hpR = dryR - lowKeepR;

            highCutFilter.processLowpass(hpL, hpR, bandL, bandR);
            float highKeepL = hpL - bandL;
            float highKeepR = hpR - bandR;

            // 3. Spectral Pitch-Snap path (STFT Phase-Vocoder snapping to 41-EDO frequencies)
            float snapL = bandL;
            float snapR = bandR;
            if (spectralSnapAmount > 0.01f) {
                processSpectralSample(bandL, bandR, snapL, snapR);
                snapL = bandL * (1.0f - spectralSnapAmount) + snapL * spectralSnapAmount;
                snapR = bandR * (1.0f - spectralSnapAmount) + snapR * spectralSnapAmount;
            } else {
                // Still feed FFT at lighter rate for UI spectrum analyzer
                feedVisualizerSample(bandL, bandR);
            }

            // 4. Subtle Harmonic Pre-Exciter (drives upper 41-EDO micro-partials for Color Bass richness)
            float excDrive = 1.0f + hyperColor * 1.4f;
            float excL = std::tanh(snapL * excDrive);
            float excR = std::tanh(snapR * excDrive);

            // 5. Parallel 41-EDO Dual-Stage Resonator Network
            float resSumL = 0.0f;
            float resSumR = 0.0f;

            for (int v = 0; v < numVoices; ++v) {
                ResonatorVoice41& voice = voices[v];

                // Stage 1: Tight 41-EDO Spectral Filter
                float bpL = 0.0f, bpR = 0.0f;
                voice.preFilter.processBandpassNormalized(excL, excR, bpL, bpR);

                // Per-Voice Spectral Gate
                float vEnergy = 0.5f * (std::fabs(bpL) + std::fabs(bpR));
                voice.envFollower = 0.996f * voice.envFollower + 0.004f * vEnergy;

                float gateGain = 1.0f;
                if (gateThresh > 0.0f) {
                    float ratio = voice.envFollower / (gateThresh + 1e-6f);
                    gateGain = std::clamp(ratio * ratio, 0.0f, 1.0f);
                }

                // Stage 2: High-Q Metallic Stereo Shimmer Resonator (Color 100..200%)
                float ringL = 0.0f, ringR = 0.0f;
                float dummy = 0.0f;
                voice.ringFilterL.processBandpassNormalized(bpL, bpL, ringL, dummy);
                voice.ringFilterR.processBandpassNormalized(bpR, bpR, ringR, dummy);

                float voiceOutL = (bpL * tightMix + ringL * ringMix) * gateGain;
                float voiceOutR = (bpR * tightMix + ringR * ringMix) * gateGain;

                // Apply stereo spread across the 41-EDO pitch wheel
                float panBlendL = 1.0f + spreadNormalized * (voice.panL * 1.414f - 1.0f);
                float panBlendR = 1.0f + spreadNormalized * (voice.panR * 1.414f - 1.0f);

                resSumL += voiceOutL * panBlendL;
                resSumR += voiceOutR * panBlendR;

                // Accumulate per-step visual energy for the 41-EDO UI wheel/meter
                visualStepEnergy[voice.stepInOctave] =
                    std::max(visualStepEnergy[voice.stepInOctave] * 0.9995f, voice.envFollower * gateGain * 4.0f);
            }

            resSumL *= normVoiceGain * transientDuck;
            resSumR *= normVoiceGain * transientDuck;

            // Add transient punch back when transientAmount > 0
            float punchL = bandL * (1.0f - transientDuck);
            float punchR = bandR * (1.0f - transientDuck);

            // Soft-clip saturation on the wet signal + combine with untreated sub-bass & air
            float wetBandL = std::tanh((resSumL + punchL) * (1.0f + hyperColor * 0.4f));
            float wetBandR = std::tanh((resSumR + punchR) * (1.0f + hyperColor * 0.4f));

            // Recombine sub-bass (below lowCut) and air (above highCut) without doubling when dryLevel > 0
            float unDry = 1.0f - dryLevel;
            float wetOnlyL = wetBandL + lowKeepL * unDry + highKeepL * (1.0f - 0.5f * wetMix) * unDry;
            float wetOnlyR = wetBandR + lowKeepR * unDry + highKeepR * (1.0f - 0.5f * wetMix) * unDry;

            outL[i] = (dryL * dryLevel + wetOnlyL * wetMix) * outputGainLin;
            outR[i] = (dryR * dryLevel + wetOnlyR * wetMix) * outputGainLin;
        }
    }

    // Accessors for GUI visualization
    float getStepVisualEnergy(int step41) const {
        if (step41 >= 0 && step41 < kStepsPerOctave) return std::clamp(visualStepEnergy[step41], 0.0f, 1.0f);
        return 0.0f;
    }

    const std::array<float, kFftSize / 2>& getVisualSpectrumIn() const { return visualSpectrumIn; }
    const std::array<float, kFftSize / 2>& getVisualSpectrumOut() const { return visualSpectrumOut; }
    int getActiveVoiceCount() const { return activeVoiceCount; }
    int getRootStep() const { return rootStep; }
    float getRefFreqHz() const { return refFreqHz; }
    int getMidiMode() const { return midiMode; }
    int getCurrentPresetIdx() const { return currentPresetIdx; }
    bool isAnyMidiHeld() const { return anyMidiHeld; }
    const std::array<bool, kNumMidiNotes>& getMidiHeldNotes() const { return midiHeldNotes; }

private:
    float sampleRate = 44100.0f;

    // Parameters
    float colorAmount = 1.15f;       // Default 115% Color (shows off the signature Color Bass resonator!)
    float decayNormalized = 0.55f;   // Resonator Q / Decay
    float spreadNormalized = 0.65f;  // Stereo shimmer & detune
    float gateNormalized = 0.25f;    // Spectral gate
    float transientAmount = 0.35f;   // Transient punch preservation
    float lowCutHz = 95.0f;          // Low crossover (keeps sub clean!)
    float highCutHz = 7500.0f;       // High crossover
    float spectralSnapAmount = 0.60f;// Spectral FFT Pitch-Snap blend
    float refFreqHz = 261.6256f;     // Note 60 reference (C4)
    int rootStep = 0;                // 0..40 root step in 41-EDO
    int midiMode = 0;                // 0: Fold 41-EDO, 1: Direct 0..127, 2: UI Scale Only
    float outputGainDb = 0.0f;
    float outputGainLin = 1.0f;
    float dryLevel = 0.25f;          // Default 25% Dry blend (0% = Pure Wet, 100% = Full Dry layer)
    int currentPresetIdx = 4;

    // Scale & MIDI state
    std::array<bool, kStepsPerOctave> uiScaleSteps{};
    std::array<bool, kNumMidiNotes> midiHeldNotes{};
    std::array<bool, kStepsPerOctave> midiPitchClassActive{};
    bool anyMidiHeld = false;
    bool needRebuildVoices = true;

    // Filterbank / Resonators
    std::array<ResonatorVoice41, kMaxActiveResonators> voices;
    int activeVoiceCount = 0;
    std::vector<float> activeTargetFreqs; // Sorted active 41-EDO frequencies for FFT quantizer

    CytomicSVF lowCutFilter;
    CytomicSVF highCutFilter;
    CytomicSVF outAirFilter;
    float fastEnv = 0.0f;
    float slowEnv = 0.0f;

    // STFT Phase Vocoder & Visualizer buffers
    std::array<float, kFftSize> hannWindow{};
    std::array<float, kFftSize> fftInFIFO_L{};
    std::array<float, kFftSize> fftInFIFO_R{};
    std::array<float, kFftSize> fftOutAccum_L{};
    std::array<float, kFftSize> fftOutAccum_R{};
    std::array<float, kFftSize / 2 + 1> prevPhaseL{};
    std::array<float, kFftSize / 2 + 1> prevPhaseR{};
    std::array<float, kFftSize / 2 + 1> synthPhaseL{};
    std::array<float, kFftSize / 2 + 1> synthPhaseR{};
    std::array<float, kFftSize / 2> visualSpectrumIn{};
    std::array<float, kFftSize / 2> visualSpectrumOut{};
    std::array<float, kStepsPerOctave> visualStepEnergy{};
    int fftPos = 0;

    void initWindow() {
        for (int i = 0; i < kFftSize; ++i) {
            hannWindow[i] = 0.5f * (1.0f - std::cos(static_cast<float>(kTwoPi) * static_cast<float>(i) / static_cast<float>(kFftSize)));
        }
    }

    void updateMidiState() {
        anyMidiHeld = false;
        midiPitchClassActive.fill(false);
        for (int n = 0; n < kNumMidiNotes; ++n) {
            if (midiHeldNotes[n]) {
                anyMidiHeld = true;
                int step41 = ((n - 60) % kStepsPerOctave + kStepsPerOctave) % kStepsPerOctave;
                midiPitchClassActive[step41] = true;
            }
        }
        needRebuildVoices = true;
    }

    void rebuildVoices() {
        needRebuildVoices = false;

        lowCutFilter.setCoeffs(lowCutHz, 0.707f, sampleRate);
        highCutFilter.setCoeffs(highCutHz, 0.707f, sampleRate);

        // Resonator Q mapping:
        // Tight stage Q: 28 .. 65 (since 41-EDO steps are 29.27 cents apart, Q ~ 35 separates single steps!)
        // Ring stage Q:  50 .. 380 (long metallic Color Bass decay tail!)
        float qTight = 30.0f + decayNormalized * 35.0f;
        float qRing = 45.0f + std::pow(decayNormalized, 1.6f) * 340.0f;
        float detuneCents = spreadNormalized * 5.5f; // +/- 5.5 cents stereo shimmer (within 29.27c step)

        activeTargetFreqs.clear();
        int count = 0;

        if (midiMode == 1 && anyMidiHeld) {
            // Mode 1: Direct MIDI Note 0..127 -> 1 MIDI note = 1 step of 41-EDO!
            // Also adds 2nd & 3rd octave harmonics at softer weight if space allows
            for (int n = 0; n < kNumMidiNotes && count < kMaxActiveResonators; ++n) {
                if (midiHeldNotes[n]) {
                    float f = midiNoteTo41EdoFreq(n);
                    int step41 = ((n - 60) % kStepsPerOctave + kStepsPerOctave) % kStepsPerOctave;
                    for (int octMult = 1; octMult <= 4; octMult *= 2) {
                        float fHarm = f * static_cast<float>(octMult);
                        if (fHarm >= lowCutHz * 0.85f && fHarm <= std::min(highCutHz * 1.1f, sampleRate * 0.45f)) {
                            if (count < kMaxActiveResonators) {
                                voices[count].configure(fHarm, step41, qTight, qRing, detuneCents, sampleRate);
                                activeTargetFreqs.push_back(fHarm);
                                count++;
                            }
                        }
                    }
                }
            }
        } else {
            // Mode 0 (Scale + MIDI Fold 41-EDO) or Mode 2 (UI Scale Only)
            // Build frequencies across all audible octaves from ~32 Hz to highCutHz
            for (int oct = -3; oct <= 5; ++oct) {
                for (int s = 0; s < kStepsPerOctave; ++s) {
                    if (!isEffectivePitchClassActive(s)) continue;

                    int totalStepsFromRef = oct * kStepsPerOctave + s + rootStep;
                    float freq = refFreqHz * std::pow(2.0f, static_cast<float>(totalStepsFromRef) / static_cast<float>(kStepsPerOctave));
                    if (freq >= lowCutHz && freq <= std::min(highCutHz, sampleRate * 0.45f)) {
                        activeTargetFreqs.push_back(freq);
                        if (count < kMaxActiveResonators) {
                            voices[count].configure(freq, s, qTight, qRing, detuneCents, sampleRate);
                            count++;
                        }
                    }
                }
            }
        }

        std::sort(activeTargetFreqs.begin(), activeTargetFreqs.end());
        activeVoiceCount = count;
    }

    // Finds the closest active 41-EDO frequency for any spectral bin frequency
    float findNearest41EdoFreq(float freq) const {
        if (activeTargetFreqs.empty()) return freq;
        auto it = std::lower_bound(activeTargetFreqs.begin(), activeTargetFreqs.end(), freq);
        if (it == activeTargetFreqs.begin()) return *it;
        if (it == activeTargetFreqs.end()) return activeTargetFreqs.back();
        float fHigh = *it;
        float fLow = *(it - 1);
        // Compare in log-frequency (pitch) domain
        return (freq / fLow < fHigh / freq) ? fLow : fHigh;
    }

    void feedVisualizerSample(float inL, float inR) {
        fftInFIFO_L[fftPos] = inL;
        fftInFIFO_R[fftPos] = inR;
        fftPos++;
        if (fftPos >= kFftSize) {
            fftPos = kFftSize - kFftHop;
            std::complex<float> buf[kFftSize];
            for (int i = 0; i < kFftSize; ++i) {
                float mono = 0.5f * (fftInFIFO_L[i] + fftInFIFO_R[i]) * hannWindow[i];
                buf[i] = std::complex<float>(mono, 0.0f);
            }
            FastFFT::fft(buf, kFftSize, false);
            for (int k = 0; k < kFftSize / 2; ++k) {
                float mag = std::abs(buf[k]) * (2.0f / static_cast<float>(kFftSize));
                visualSpectrumIn[k] = 0.85f * visualSpectrumIn[k] + 0.15f * mag;
                visualSpectrumOut[k] = visualSpectrumIn[k];
            }
            std::memmove(fftInFIFO_L.data(), fftInFIFO_L.data() + kFftHop, (kFftSize - kFftHop) * sizeof(float));
            std::memmove(fftInFIFO_R.data(), fftInFIFO_R.data() + kFftHop, (kFftSize - kFftHop) * sizeof(float));
        }
    }

    void processSpectralSample(float inL, float inR, float& outL, float& outR) {
        fftInFIFO_L[fftPos] = inL;
        fftInFIFO_R[fftPos] = inR;

        outL = fftOutAccum_L[fftPos - (kFftSize - kFftHop)];
        outR = fftOutAccum_R[fftPos - (kFftSize - kFftHop)];

        fftPos++;
        if (fftPos >= kFftSize) {
            fftPos = kFftSize - kFftHop;
            processSpectralFrame();

            // Shift input FIFOs
            std::memmove(fftInFIFO_L.data(), fftInFIFO_L.data() + kFftHop, (kFftSize - kFftHop) * sizeof(float));
            std::memmove(fftInFIFO_R.data(), fftInFIFO_R.data() + kFftHop, (kFftSize - kFftHop) * sizeof(float));

            // Shift output accumulator
            std::memmove(fftOutAccum_L.data(), fftOutAccum_L.data() + kFftHop, (kFftSize - kFftHop) * sizeof(float));
            std::memmove(fftOutAccum_R.data(), fftOutAccum_R.data() + kFftHop, (kFftSize - kFftHop) * sizeof(float));
            std::memset(fftOutAccum_L.data() + (kFftSize - kFftHop), 0, kFftHop * sizeof(float));
            std::memset(fftOutAccum_R.data() + (kFftSize - kFftHop), 0, kFftHop * sizeof(float));
        }
    }

    void processSpectralFrame() {
        std::complex<float> specL[kFftSize];
        std::complex<float> specR[kFftSize];
        std::complex<float> mappedL[kFftSize];
        std::complex<float> mappedR[kFftSize];

        for (int i = 0; i < kFftSize; ++i) {
            specL[i] = std::complex<float>(fftInFIFO_L[i] * hannWindow[i], 0.0f);
            specR[i] = std::complex<float>(fftInFIFO_R[i] * hannWindow[i], 0.0f);
            mappedL[i] = std::complex<float>(0.0f, 0.0f);
            mappedR[i] = std::complex<float>(0.0f, 0.0f);
        }

        FastFFT::fft(specL, kFftSize, false);
        FastFFT::fft(specR, kFftSize, false);

        const int half = kFftSize / 2;
        const float binHz = sampleRate / static_cast<float>(kFftSize);
        const float expPhaseAdv = static_cast<float>(kTwoPi) * static_cast<float>(kFftHop) / static_cast<float>(kFftSize);

        for (int k = 1; k < half; ++k) {
            float freq = static_cast<float>(k) * binHz;
            float magL = std::abs(specL[k]);
            float magR = std::abs(specR[k]);

            visualSpectrumIn[k] = 0.8f * visualSpectrumIn[k] + 0.2f * ((magL + magR) / static_cast<float>(kFftSize));

            if (freq < lowCutHz || freq > highCutHz || activeTargetFreqs.empty()) {
                mappedL[k] += specL[k];
                mappedR[k] += specR[k];
                continue;
            }

            // Snap bin frequency to nearest active 41-EDO target frequency!
            float targetFreq = findNearest41EdoFreq(freq);
            int targetBin = std::clamp(static_cast<int>(std::round(targetFreq / binHz)), 1, half - 1);

            float phL = std::arg(specL[k]);
            float phR = std::arg(specR[k]);
            float dPhL = phL - prevPhaseL[k] - static_cast<float>(k) * expPhaseAdv;
            float dPhR = phR - prevPhaseR[k] - static_cast<float>(k) * expPhaseAdv;
            prevPhaseL[k] = phL;
            prevPhaseR[k] = phR;

            // Wrap phase [-pi, pi]
            dPhL -= static_cast<float>(kTwoPi) * std::round(dPhL / static_cast<float>(kTwoPi));
            dPhR -= static_cast<float>(kTwoPi) * std::round(dPhR / static_cast<float>(kTwoPi));

            float ratio = targetFreq / std::max(freq, 1.0f);
            synthPhaseL[targetBin] += (static_cast<float>(targetBin) * expPhaseAdv + dPhL * ratio);
            synthPhaseR[targetBin] += (static_cast<float>(targetBin) * expPhaseAdv + dPhR * ratio);

            mappedL[targetBin] += std::polar(magL, synthPhaseL[targetBin]);
            mappedR[targetBin] += std::polar(magR, synthPhaseR[targetBin]);
        }

        // Conjugate symmetry for real IFFT
        for (int k = 1; k < half; ++k) {
            mappedL[kFftSize - k] = std::conj(mappedL[k]);
            mappedR[kFftSize - k] = std::conj(mappedR[k]);
            float magOut = (std::abs(mappedL[k]) + std::abs(mappedR[k])) / static_cast<float>(kFftSize);
            visualSpectrumOut[k] = 0.8f * visualSpectrumOut[k] + 0.2f * magOut;
        }

        FastFFT::fft(mappedL, kFftSize, true);
        FastFFT::fft(mappedR, kFftSize, true);

        // Overlap-add synthesis (4x overlap with Hann window -> scale factor 2/3)
        const float olaGain = 0.6667f;
        for (int i = 0; i < kFftSize; ++i) {
            fftOutAccum_L[i] += mappedL[i].real() * hannWindow[i] * olaGain;
            fftOutAccum_R[i] += mappedR[i].real() * hannWindow[i] * olaGain;
        }
    }
};

} // namespace Chroma41
