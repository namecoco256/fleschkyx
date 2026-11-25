#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <complex>
#include <algorithm>
#include <array>
#include <vector>
#include <atomic>

namespace Chroma41 {

static constexpr int kStepsPerOctave = 41;
static constexpr int kNumMidiNotes = 128;
static constexpr int kNumOctaves = 8; // Covers ~32.7 Hz (C1) to ~8372 Hz (C9)
static constexpr int kTotalGridPitches = kStepsPerOctave * kNumOctaves; // 328 microtonal bins
static constexpr int kMaxActiveResonators = 64;
static constexpr int kFftSize = 1024;
static constexpr int kFftHop = 256;
static constexpr double kPi = 3.14159265358979323846;
static constexpr double kTwoPi = 6.28318530717958647692;

// 41-EDO Interval Ratio Reference (for UI labels & presets)
struct Edo41StepInfo {
    int step;
    const char* name;
    const char* ratioHint;
    int category; // 0 = root, 1 = consonant 5-limit, 2 = 7/11-limit harmonic, 3 = microtonal step
};

static const Edo41StepInfo kEdo41StepTable[kStepsPerOctave] = {
    { 0, "0 (1/1)",   "Root Unison",     0 },
    { 1, "1",         "81/80 Comma",     3 },
    { 2, "2",         "33/32",           3 },
    { 3, "3 (21/20)", "Subminor 2nd",    2 },
    { 4, "4 (16/15)", "Minor 2nd",       1 },
    { 5, "5 (12/11)", "Neutral 2nd-",    2 },
    { 6, "6 (11/10)", "Neutral 2nd+",    2 },
    { 7, "7 (9/8)",   "Major 2nd",       1 },
    { 8, "8 (8/7)",   "Septimal M2",     2 },
    { 9, "9 (7/6)",   "Subminor 3rd",    2 },
    {10, "10 (6/5)",  "Minor 3rd",       1 },
    {11, "11 (11/9)", "Neutral 3rd",     2 },
    {12, "12",        "Supraneutral 3rd",3 },
    {13, "13 (5/4)",  "Major 3rd",       1 },
    {14, "14 (9/7)",  "Supermajor 3rd",  2 },
    {15, "15 (13/10)","Tridecimal 3rd",  3 },
    {16, "16 (21/16)","Narrow 4th",      2 },
    {17, "17 (4/3)",  "Perfect 4th",     1 },
    {18, "18 (11/8)", "11th Harmonic",   2 },
    {19, "19 (18/13)","Neutral 4th",     3 },
    {20, "20 (7/5)",  "Sub-Tritone",     2 },
    {21, "21 (10/7)", "Super-Tritone",   2 },
    {22, "22 (13/9)", "Neutral 5th",     3 },
    {23, "23 (16/11)","Undecimal 5th",   2 },
    {24, "24 (3/2)",  "Perfect 5th",     1 },
    {25, "25 (32/21)","Wide 5th",        2 },
    {26, "26",        "20/13",           3 },
    {27, "27 (14/9)", "Subminor 6th",    2 },
    {28, "28 (8/5)",  "Minor 6th",       1 },
    {29, "29",        "Subneutral 6th",  3 },
    {30, "30 (18/11)","Neutral 6th",     2 },
    {31, "31 (5/3)",  "Major 6th",       1 },
    {32, "32 (12/7)", "Supermajor 6th",  2 },
    {33, "33 (7/4)",  "Harmonic 7th",    2 },
    {34, "34 (16/9)", "Minor 7th",       1 },
    {35, "35 (20/11)","Neutral 7th-",    2 },
    {36, "36 (11/6)", "Neutral 7th+",    2 },
    {37, "37 (15/8)", "Major 7th",       1 },
    {38, "38 (40/21)","Supermajor 7th",  2 },
    {39, "39",        "64/33",           3 },
    {40, "40",        "160/81",          3 }
};

struct ScalePreset41 {
    const char* name;
    std::vector<int> steps;
};

static inline std::vector<ScalePreset41> getScalePresets41() {
    return {
        { "JI Major (5-Limit)",           { 0, 7, 13, 17, 24, 31, 37 } },
        { "JI Natural Minor",             { 0, 7, 10, 17, 24, 28, 34 } },
        { "Otonal Harmonics (8..16)",     { 0, 7, 13, 18, 24, 27, 33, 37 } },
        { "Utonal Subharmonics",          { 0, 4, 8, 14, 17, 23, 28, 34 } },
        { "Septimal Color Bass (7-Limit)",{ 0, 8, 9, 14, 17, 20, 24, 33 } },
        { "Undecimal Neutral (11-Limit)", { 0, 6, 11, 17, 18, 24, 30, 36 } },
        { "Superpythagorean Pentatonic",  { 0, 7, 14, 24, 31 } },
        { "Neutral Triad + 7th",          { 0, 11, 24, 33 } },
        { "Subminor Dark Chord",          { 0, 9, 17, 24, 33 } },
        { "8-Step Symmetric (5x8+1)",     { 0, 5, 10, 15, 20, 25, 30, 35, 40 } },
        { "All 41-EDO Chromatic",         { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,
                                            21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40 } }
    };
}

// High-stability Cytomic SVF (Bandpass / Lowpass / Highpass)
struct CytomicSVF {
    float ic1eqL = 0.0f, ic2eqL = 0.0f;
    float ic1eqR = 0.0f, ic2eqR = 0.0f;
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    float k = 1.0f;

    void reset() {
        ic1eqL = ic2eqL = ic1eqR = ic2eqR = 0.0f;
    }

    void setCoeffs(float freq, float Q, float sampleRate) {
        float nyquist = sampleRate * 0.48f;
        freq = std::clamp(freq, 20.0f, nyquist);
        Q = std::max(Q, 0.5f);
        float g = std::tan(static_cast<float>(kPi) * freq / sampleRate);
        k = 1.0f / Q;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    // Returns normalized bandpass output (unity peak gain at center frequency)
    inline void processBandpassNormalized(float inL, float inR, float& outL, float& outR) {
        float v3L = inL - ic2eqL;
        float v1L = a1 * ic1eqL + a2 * v3L;
        float v2L = ic2eqL + a2 * ic1eqL + a3 * v3L;
        ic1eqL = 2.0f * v1L - ic1eqL;
        ic2eqL = 2.0f * v2L - ic2eqL;
        outL = v1L * k; // Normalized peak = 1.0

        float v3R = inR - ic2eqR;
        float v1R = a1 * ic1eqR + a2 * v3R;
        float v2R = ic2eqR + a2 * ic1eqR + a3 * v3R;
        ic1eqR = 2.0f * v1R - ic1eqR;
        ic2eqR = 2.0f * v2R - ic2eqR;
        outR = v1R * k;
    }

    inline void processLowpass(float inL, float inR, float& outL, float& outR) {
        float v3L = inL - ic2eqL;
        float v1L = a1 * ic1eqL + a2 * v3L;
        float v2L = ic2eqL + a2 * ic1eqL + a3 * v3L;
        ic1eqL = 2.0f * v1L - ic1eqL;
        ic2eqL = 2.0f * v2L - ic2eqL;
        outL = v2L;

        float v3R = inR - ic2eqR;
        float v1R = a1 * ic1eqR + a2 * v3R;
        float v2R = ic2eqR + a2 * ic1eqR + a3 * v3R;
        ic1eqR = 2.0f * v1R - ic1eqR;
        ic2eqR = 2.0f * v2R - ic2eqR;
        outR = v2R;
    }

    inline void processHighpass(float inL, float inR, float& outL, float& outR) {
        float v3L = inL - ic2eqL;
        float v1L = a1 * ic1eqL + a2 * v3L;
        float v2L = ic2eqL + a2 * ic1eqL + a3 * v3L;
        ic1eqL = 2.0f * v1L - ic1eqL;
        ic2eqL = 2.0f * v2L - ic2eqL;
        outL = inL - k * v1L - v2L;

        float v3R = inR - ic2eqR;
        float v1R = a1 * ic1eqR + a2 * v3R;
        float v2R = ic2eqR + a2 * ic1eqR + a3 * v3R;
        ic1eqR = 2.0f * v1R - ic1eqR;
        ic2eqR = 2.0f * v2R - ic2eqR;
        outR = inR - k * v1R - v2R;
    }
};

// Complex Resonator Voice tuned to a specific 41-EDO frequency
// Uses a 2-stage cascade (tight spectral filter + high-Q metallic ringing resonator with stereo spread)
struct ResonatorVoice41 {
    float freq = 440.0f;
    int stepInOctave = 0;
    int midiEquivalent = 60;
    float panL = 1.0f;
    float panR = 1.0f;
    float envFollower = 0.0f;
    float gainSmooth = 0.0f;
    bool targetActive = false;

    CytomicSVF preFilter;   // Stage 1: Medium-Q spectral selection (Color 0..100)
    CytomicSVF ringFilterL; // Stage 2L: High-Q metallic sustain resonator (Color 100..200)
    CytomicSVF ringFilterR; // Stage 2R: Slightly detuned stereo shimmer resonator

    void reset() {
        preFilter.reset();
        ringFilterL.reset();
        ringFilterR.reset();
        envFollower = 0.0f;
        gainSmooth = 0.0f;
    }

    void configure(float f, int step41, float qTight, float qRing, float stereoDetuneCents, float sampleRate) {
        freq = f;
        stepInOctave = ((step41 % kStepsPerOctave) + kStepsPerOctave) % kStepsPerOctave;
        preFilter.setCoeffs(freq, qTight, sampleRate);

        float detuneRatio = std::pow(2.0f, stereoDetuneCents / 1200.0f);
        ringFilterL.setCoeffs(freq / detuneRatio, qRing, sampleRate);
        ringFilterR.setCoeffs(freq * detuneRatio, qRing, sampleRate);

        // Alternate subtle stereo panning across 41-EDO steps for wide Color Bass image
        float angle = static_cast<float>((stepInOctave * 17) % kStepsPerOctave) / static_cast<float>(kStepsPerOctave);
        float pan = (angle - 0.5f) * 0.7f; // -0.35 .. +0.35
        panL = std::cos((pan + 1.0f) * 0.25f * static_cast<float>(kPi));
        panR = std::sin((pan + 1.0f) * 0.25f * static_cast<float>(kPi));
    }
};

// Minimal radix-2 Cooley-Tukey FFT for Spectral Pitch Mapping & UI Visualizer
class FastFFT {
public:
    static void fft(std::complex<float>* data, int n, bool inverse) {
        int j = 0;
        for (int i = 0; i < n; ++i) {
            if (i < j) std::swap(data[i], data[j]);
            int m = n >> 1;
            while (m >= 1 && j >= m) {
                j -= m;
                m >>= 1;
            }
            j += m;
        }

        for (int len = 2; len <= n; len <<= 1) {
            float angle = static_cast<float>(kTwoPi) / static_cast<float>(len) * (inverse ? 1.0f : -1.0f);
            std::complex<float> wlen(std::cos(angle), std::sin(angle));
            int half = len >> 1;
            for (int i = 0; i < n; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                for (int k = 0; k < half; ++k) {
                    std::complex<float> u = data[i + k];
                    std::complex<float> v = data[i + k + half] * w;
                    data[i + k] = u + v;
                    data[i + k + half] = u - v;
                    w *= wlen;
                }
            }
        }

        if (inverse) {
            float invN = 1.0f / static_cast<float>(n);
            for (int i = 0; i < n; ++i) {
                data[i] *= invN;
            }
        }
    }
};

} // namespace Chroma41
