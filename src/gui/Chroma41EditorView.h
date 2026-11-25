#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <cstdio>
#include <cmath>
#include <functional>
#include "../dsp/Chroma41Engine.h"

namespace Chroma41 {

class Chroma41GUI {
public:
    static constexpr int kWidth = 980;
    static constexpr int kHeight = 620;

    using ParamCallback = std::function<void(uint32_t paramId, float normalizedVal)>;

    Chroma41GUI(Chroma41Engine* eng, ParamCallback cb = nullptr)
        : engine(eng), onParamChanged(cb) {
        initKnobs();
    }

    ~Chroma41GUI() {
        destroyWindow();
    }

    bool createWindow(HWND parentWnd) {
        destroyWindow();

        HINSTANCE hInst = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Chroma41GUI::wndProcStatic),
            &hInst
        );
        if (!hInst) {
            hInst = GetModuleHandleW(nullptr);
        }
        const wchar_t* className = L"Chroma41VST3EditorClass_v2";

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        if (!GetClassInfoExW(hInst, className, &wc)) {
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            wc.lpfnWndProc = &Chroma41GUI::wndProcStatic;
            wc.hInstance = hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = className;
            RegisterClassExW(&wc);
        }

        DWORD style = parentWnd ? (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        hwnd = CreateWindowExW(
            0, className, L"CHROMA-41 | 41-EDO Spectral Resonator",
            style, 0, 0, kWidth, kHeight,
            parentWnd, nullptr, hInst, this
        );

        if (hwnd) {
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
            SetTimer(hwnd, 1, 30, nullptr); // ~33 FPS smooth spectrum & resonator glow
        }
        return hwnd != nullptr;
    }

    void destroyWindow() {
        if (hwnd) {
            KillTimer(hwnd, 1);
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }

    HWND getHWND() const { return hwnd; }

private:
    struct KnobControl {
        uint32_t paramId;
        const char* label;
        int cx, cy, radius;
        bool isBigColor;
    };

    Chroma41Engine* engine = nullptr;
    ParamCallback onParamChanged;
    HWND hwnd = nullptr;

    std::vector<KnobControl> knobs;
    int draggingKnobIdx = -1;
    int dragStartY = 0;
    float dragStartVal = 0.0f;
    int hoveredStep = -1;
    bool isPaintingSteps = false;
    bool paintStepState = true;

    void initKnobs() {
        knobs = {
            // Big COLOR knob (0% - 200%)
            { kParamColor,        "COLOR (0-200%)", 625, 195, 58, true  },
            // Surrounding DSP knobs
            { kParamDecay,        "DECAY TAIL",     770, 145, 32, false },
            { kParamSpread,       "SHIMMER/WIDE",   890, 145, 32, false },
            { kParamSpectralSnap, "SPECTRAL SNAP",  770, 255, 32, false },
            { kParamSpectralGate, "SPEC GATE",      890, 255, 32, false },
            { kParamDryLevel,     "DRY LEVEL",      538, 335, 25, false },
            { kParamTransient,    "TRANSIENT",      614, 335, 25, false },
            { kParamLowCut,       "LOW CUT",        690, 335, 25, false },
            { kParamHighCut,      "HIGH CUT",       766, 335, 25, false },
            { kParamRootStep,     "41-EDO ROOT",    842, 335, 25, false },
            { kParamOutputGain,   "OUTPUT",         918, 335, 25, false }
        };
    }

    void notifyParam(uint32_t id, float norm) {
        norm = std::clamp(norm, 0.0f, 1.0f);
        if (engine) {
            engine->setParameterNormalized(id, norm);
        }
        if (onParamChanged) {
            onParamChanged(id, norm);
        }
        if (hwnd) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    static LRESULT CALLBACK wndProcStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        Chroma41GUI* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Chroma41GUI*>(cs->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Chroma41GUI*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        }
        if (self) {
            return self->wndProc(hWnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    RECT getStepKeyRect(int step) const {
        // 41 microtonal keys across the bottom panel (x: 18..962, y: 422..596)
        const int leftMargin = 18;
        const int totalW = kWidth - 36;
        const float keyW = static_cast<float>(totalW) / static_cast<float>(kStepsPerOctave);
        int x0 = leftMargin + static_cast<int>(std::round(step * keyW));
        int x1 = leftMargin + static_cast<int>(std::round((step + 1) * keyW)) - 2;
        RECT r{ x0, 424, x1, 596 };
        return r;
    }

    int hitTestStepKey(int mx, int my) const {
        if (my < 424 || my > 596) return -1;
        for (int s = 0; s < kStepsPerOctave; ++s) {
            RECT r = getStepKeyRect(s);
            if (mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom) {
                return s;
            }
        }
        return -1;
    }

    LRESULT wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_TIMER:
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;

            case WM_ERASEBKGND:
                return 1;

            case WM_LBUTTONDOWN: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);
                SetCapture(hWnd);

                // Check header buttons:
                // 1) Preset Prev (<) [300..328, 14..44], Preset Next (>) [545..573, 14..44], Preset Box [330..543, 14..44]
                if (my >= 14 && my <= 44) {
                    auto presets = getScalePresets41();
                    int numP = static_cast<int>(presets.size());
                    int curP = engine ? engine->getCurrentPresetIdx() : 0;
                    if (mx >= 300 && mx <= 328) {
                        int nextP = (curP - 1 + numP) % numP;
                        notifyParam(kParamPresetIdx, static_cast<float>(nextP) / static_cast<float>(numP - 1));
                        return 0;
                    }
                    if (mx >= 330 && mx <= 573) {
                        int nextP = (curP + 1) % numP;
                        notifyParam(kParamPresetIdx, static_cast<float>(nextP) / static_cast<float>(numP - 1));
                        return 0;
                    }
                    // MIDI Mode toggle button [590..820, 14..44]
                    if (mx >= 590 && mx <= 820 && engine) {
                        int nextMode = (engine->getMidiMode() + 1) % 3;
                        notifyParam(kParamMidiMode, static_cast<float>(nextMode) / 2.0f);
                        return 0;
                    }
                    // All / Clear buttons [835..895] and [902..962]
                    if (mx >= 835 && mx <= 895) {
                        for (int s = 0; s < kStepsPerOctave; ++s) notifyParam(kParamStepBase + s, 1.0f);
                        return 0;
                    }
                    if (mx >= 902 && mx <= 962) {
                        notifyParam(kParamStepBase + 0, 1.0f);
                        for (int s = 1; s < kStepsPerOctave; ++s) notifyParam(kParamStepBase + s, 0.0f);
                        return 0;
                    }
                }

                // Check knobs
                for (size_t i = 0; i < knobs.size(); ++i) {
                    int dx = mx - knobs[i].cx;
                    int dy = my - knobs[i].cy;
                    int r = knobs[i].radius + 8;
                    if (dx * dx + dy * dy <= r * r) {
                        draggingKnobIdx = static_cast<int>(i);
                        dragStartY = my;
                        dragStartVal = engine ? engine->getParameterNormalized(knobs[i].paramId) : 0.5f;
                        return 0;
                    }
                }

                // Check 41-EDO keyboard steps
                int step = hitTestStepKey(mx, my);
                if (step >= 0 && engine) {
                    isPaintingSteps = true;
                    paintStepState = !engine->isStepActive(step);
                    notifyParam(kParamStepBase + step, paintStepState ? 1.0f : 0.0f);
                    return 0;
                }
                return 0;
            }

            case WM_LBUTTONDBLCLK: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);
                // Double click knob resets to default
                for (size_t i = 0; i < knobs.size(); ++i) {
                    int dx = mx - knobs[i].cx;
                    int dy = my - knobs[i].cy;
                    int r = knobs[i].radius + 8;
                    if (dx * dx + dy * dy <= r * r) {
                        float defVal = 0.5f;
                        if (knobs[i].paramId == kParamColor) defVal = 0.575f; // 115%
                        else if (knobs[i].paramId == kParamRootStep) defVal = 0.0f;
                        else if (knobs[i].paramId == kParamSpectralGate) defVal = 0.25f;
                        else if (knobs[i].paramId == kParamDryLevel) defVal = 0.25f;
                        notifyParam(knobs[i].paramId, defVal);
                        return 0;
                    }
                }
                return 0;
            }

            case WM_MOUSEMOVE: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);

                if (draggingKnobIdx >= 0 && draggingKnobIdx < static_cast<int>(knobs.size())) {
                    float sensitivity = (wParam & MK_SHIFT) ? 0.0015f : 0.005f;
                    float delta = static_cast<float>(dragStartY - my) * sensitivity;
                    notifyParam(knobs[draggingKnobIdx].paramId, dragStartVal + delta);
                    return 0;
                }

                int step = hitTestStepKey(mx, my);
                if (step != hoveredStep) {
                    hoveredStep = step;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }

                if (isPaintingSteps && step >= 0) {
                    notifyParam(kParamStepBase + step, paintStepState ? 1.0f : 0.0f);
                }
                return 0;
            }

            case WM_LBUTTONUP:
                draggingKnobIdx = -1;
                isPaintingSteps = false;
                ReleaseCapture();
                return 0;

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hWnd, &ps);
                RECT rc;
                GetClientRect(hWnd, &rc);
                int w = rc.right - rc.left;
                int h = rc.bottom - rc.top;

                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP memBmp = CreateCompatibleBitmap(hdc, kWidth, kHeight);
                HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

                renderGUI(memDC);

                if (w == kWidth && h == kHeight) {
                    BitBlt(hdc, 0, 0, kWidth, kHeight, memDC, 0, 0, SRCCOPY);
                } else {
                    SetStretchBltMode(hdc, HALFTONE);
                    StretchBlt(hdc, 0, 0, w, h, memDC, 0, 0, kWidth, kHeight, SRCCOPY);
                }

                SelectObject(memDC, oldBmp);
                DeleteObject(memBmp);
                DeleteDC(memDC);
                EndPaint(hWnd, &ps);
                return 0;
            }
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    static void fillRectColor(HDC hdc, int x0, int y0, int x1, int y1, COLORREF col) {
        RECT r{ x0, y0, x1, y1 };
        HBRUSH br = CreateSolidBrush(col);
        FillRect(hdc, &r, br);
        DeleteObject(br);
    }

    static void drawFrameRect(HDC hdc, int x0, int y0, int x1, int y1, COLORREF col) {
        RECT r{ x0, y0, x1, y1 };
        HBRUSH br = CreateSolidBrush(col);
        FrameRect(hdc, &r, br);
        DeleteObject(br);
    }

    void renderGUI(HDC hdc) {
        SetBkMode(hdc, TRANSPARENT);

        // 1. Deep Dark Background
        fillRectColor(hdc, 0, 0, kWidth, kHeight, RGB(13, 15, 22));

        // 2. Top Header Bar
        fillRectColor(hdc, 0, 0, kWidth, 56, RGB(20, 23, 34));
        fillRectColor(hdc, 0, 55, kWidth, 57, RGB(0, 210, 255)); // Neon cyan accent line

        HFONT fontTitle = CreateFontA(24, 0, 0, 0, FW_HEAVY, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        HFONT fontUI = CreateFontA(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        HFONT fontSmall = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");

        HGDIOBJ oldFont = SelectObject(hdc, fontTitle);
        SetTextColor(hdc, RGB(0, 235, 255));
        TextOutA(hdc, 20, 14, "CHROMA-41", 9);

        SelectObject(hdc, fontSmall);
        SetTextColor(hdc, RGB(170, 180, 210));
        TextOutA(hdc, 155, 21, "41-EDO SPECTRAL RESONATOR", 25);

        // Preset selector buttons
        auto presets = getScalePresets41();
        int curPreset = engine ? engine->getCurrentPresetIdx() : 0;
        const char* presetName = (curPreset >= 0 && curPreset < static_cast<int>(presets.size()))
            ? presets[curPreset].name : "Custom 41-EDO";

        fillRectColor(hdc, 300, 14, 328, 44, RGB(34, 39, 56));
        drawFrameRect(hdc, 300, 14, 328, 44, RGB(70, 85, 120));
        fillRectColor(hdc, 330, 14, 543, 44, RGB(28, 33, 48));
        drawFrameRect(hdc, 330, 14, 543, 44, RGB(0, 180, 230));
        fillRectColor(hdc, 545, 14, 573, 44, RGB(34, 39, 56));
        drawFrameRect(hdc, 545, 14, 573, 44, RGB(70, 85, 120));

        SelectObject(hdc, fontUI);
        SetTextColor(hdc, RGB(230, 240, 255));
        TextOutA(hdc, 310, 21, "<", 1);
        TextOutA(hdc, 555, 21, ">", 1);
        RECT rcPreset{ 336, 14, 538, 44 };
        DrawTextA(hdc, presetName, -1, &rcPreset, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // MIDI Mode button
        int midiMode = engine ? engine->getMidiMode() : 0;
        const char* midiModeLabels[3] = {
            "MIDI: FOLD 41-EDO (SCALE/CHORD)",
            "MIDI: 0..127 DIRECT (1 NOTE=1 STEP)",
            "MIDI: UI SCALE ONLY"
        };
        COLORREF midiBtnCol = (midiMode == 1) ? RGB(95, 28, 110) : (midiMode == 0 ? RGB(24, 68, 95) : RGB(40, 44, 58));
        fillRectColor(hdc, 590, 14, 820, 44, midiBtnCol);
        drawFrameRect(hdc, 590, 14, 820, 44, RGB(140, 210, 255));
        RECT rcMidi{ 596, 14, 814, 44 };
        DrawTextA(hdc, midiModeLabels[midiMode % 3], -1, &rcMidi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // All / Root buttons
        fillRectColor(hdc, 835, 14, 895, 44, RGB(34, 42, 60));
        drawFrameRect(hdc, 835, 14, 895, 44, RGB(80, 100, 140));
        RECT rcAll{ 835, 14, 895, 44 };
        DrawTextA(hdc, "ALL 41", -1, &rcAll, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        fillRectColor(hdc, 902, 14, 962, 44, RGB(34, 42, 60));
        drawFrameRect(hdc, 902, 14, 962, 44, RGB(80, 100, 140));
        RECT rcClr{ 902, 14, 962, 44 };
        DrawTextA(hdc, "ROOT", -1, &rcClr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 3. Left Panel: Real-Time Spectral & 41-EDO Quantizer Visualizer [18..485, 72..385]
        renderSpectrumDisplay(hdc, 18, 72, 485, 385, fontSmall);

        // 4. Right Panel: Knobs & Color Bass Resonator Controls [500..962, 72..385]
        fillRectColor(hdc, 500, 72, 962, 385, RGB(19, 22, 33));
        drawFrameRect(hdc, 500, 72, 962, 385, RGB(42, 50, 75));

        for (size_t i = 0; i < knobs.size(); ++i) {
            renderKnob(hdc, knobs[i], fontUI, fontSmall);
        }

        // 5. Bottom Panel: Interactive 41-EDO Keyboard & Harmonic Step Matrix [18..962, 398..604]
        render41EdoKeyboard(hdc, fontUI, fontSmall);

        SelectObject(hdc, oldFont);
        DeleteObject(fontTitle);
        DeleteObject(fontUI);
        DeleteObject(fontSmall);
    }

    void renderSpectrumDisplay(HDC hdc, int x0, int y0, int x1, int y1, HFONT fontSmall) {
        fillRectColor(hdc, x0, y0, x1, y1, RGB(14, 17, 26));
        drawFrameRect(hdc, x0, y0, x1, y1, RGB(45, 58, 88));

        if (!engine) return;

        int w = x1 - x0 - 4;
        int h = y1 - y0 - 42;
        int plotX = x0 + 2;
        int plotY = y0 + 26;

        // Draw vertical active 41-EDO resonator lines in log-frequency space (60 Hz .. 10000 Hz)
        const float minF = 60.0f;
        const float maxF = 10000.0f;
        const float logMin = std::log(minF);
        const float logRange = std::log(maxF) - logMin;

        HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(28, 42, 66));
        HGDIOBJ oldPen = SelectObject(hdc, gridPen);

        int rootStep = engine->getRootStep();
        float refHz = engine->getRefFreqHz();
        for (int oct = -2; oct <= 4; ++oct) {
            for (int s = 0; s < kStepsPerOctave; ++s) {
                if (!engine->isEffectivePitchClassActive(s)) continue;
                int totalSteps = oct * kStepsPerOctave + s + rootStep;
                float freq = refHz * std::pow(2.0f, static_cast<float>(totalSteps) / 41.0f);
                if (freq >= minF && freq <= maxF) {
                    float normX = (std::log(freq) - logMin) / logRange;
                    int px = plotX + static_cast<int>(normX * w);
                    MoveToEx(hdc, px, plotY, nullptr);
                    LineTo(hdc, px, plotY + h);
                }
            }
        }

        // Draw Spectrum Curve (Input in slate, Output Resonated in neon cyan/magenta)
        const auto& specIn = engine->getVisualSpectrumIn();
        const auto& specOut = engine->getVisualSpectrumOut();
        int numBins = static_cast<int>(specOut.size());

        HPEN inPen = CreatePen(PS_SOLID, 1, RGB(80, 95, 130));
        SelectObject(hdc, inPen);
        for (int k = 2; k < numBins; ++k) {
            float freq = static_cast<float>(k) * 44100.0f / static_cast<float>(kFftSize);
            if (freq < minF || freq > maxF) continue;
            float normX = (std::log(freq) - logMin) / logRange;
            int px = plotX + static_cast<int>(normX * w);
            float db = 20.0f * std::log10(specIn[k] + 1e-5f);
            float normY = std::clamp((db + 65.0f) / 60.0f, 0.0f, 1.0f);
            int py = plotY + h - static_cast<int>(normY * h);
            if (k == 2) MoveToEx(hdc, px, py, nullptr);
            else LineTo(hdc, px, py);
        }

        float colorAmt = engine->getParameterNormalized(kParamColor) * 2.0f;
        COLORREF waveCol = (colorAmt > 1.0f) ? RGB(235, 70, 255) : RGB(0, 230, 255);
        HPEN outPen = CreatePen(PS_SOLID, 2, waveCol);
        SelectObject(hdc, outPen);
        for (int k = 2; k < numBins; ++k) {
            float freq = static_cast<float>(k) * 44100.0f / static_cast<float>(kFftSize);
            if (freq < minF || freq > maxF) continue;
            float normX = (std::log(freq) - logMin) / logRange;
            int px = plotX + static_cast<int>(normX * w);
            float db = 20.0f * std::log10(specOut[k] * (1.0f + colorAmt * 0.5f) + 1e-5f);
            float normY = std::clamp((db + 65.0f) / 60.0f, 0.0f, 1.0f);
            int py = plotY + h - static_cast<int>(normY * h);
            if (k == 2) MoveToEx(hdc, px, py, nullptr);
            else LineTo(hdc, px, py);
        }

        SelectObject(hdc, oldPen);
        DeleteObject(gridPen);
        DeleteObject(inPen);
        DeleteObject(outPen);

        // Status overlay inside spectrum panel
        SelectObject(hdc, fontSmall);
        SetTextColor(hdc, RGB(160, 210, 255));
        char infoBuf[256];
        std::snprintf(infoBuf, sizeof(infoBuf),
            "41-EDO PITCH GRID | Step = 29.27 cents (2^(1/41)) | Active Resonators: %d | Root Step: %d",
            engine->getActiveVoiceCount(), engine->getRootStep());
        TextOutA(hdc, x0 + 10, y0 + 7, infoBuf, static_cast<int>(std::strlen(infoBuf)));

        // Show held MIDI notes (0..127 -> 41-EDO frequency readout)
        char midiBuf[256] = "MIDI 0..127 -> 41-EDO: ";
        if (engine->isAnyMidiHeld()) {
            const auto& held = engine->getMidiHeldNotes();
            int shown = 0;
            for (int n = 0; n < kNumMidiNotes && shown < 5; ++n) {
                if (held[n]) {
                    char item[48];
                    int step41 = ((n - 60) % 41 + 41) % 41;
                    std::snprintf(item, sizeof(item), "[Note %d = Step %d: %.1fHz] ",
                        n, step41, engine->midiNoteTo41EdoFreq(n));
                    std::strncat(midiBuf, item, sizeof(midiBuf) - std::strlen(midiBuf) - 1);
                    shown++;
                }
            }
            SetTextColor(hdc, RGB(255, 215, 90));
        } else {
            std::strncat(midiBuf, "No MIDI notes held (Note 60 = Step 0, Note 61 = +1/41 oct, Note 101 = +1 oct)",
                sizeof(midiBuf) - std::strlen(midiBuf) - 1);
            SetTextColor(hdc, RGB(120, 140, 175));
        }
        TextOutA(hdc, x0 + 10, y1 - 18, midiBuf, static_cast<int>(std::strlen(midiBuf)));
    }

    void renderKnob(HDC hdc, const KnobControl& k, HFONT fontUI, HFONT fontSmall) {
        float norm = engine ? engine->getParameterNormalized(k.paramId) : 0.5f;

        // Draw knob circle
        int r = k.radius;
        COLORREF ringCol = k.isBigColor
            ? (norm > 0.5f ? RGB(235, 65, 255) : RGB(0, 225, 255))
            : (k.paramId == kParamDryLevel ? RGB(255, 195, 60) : RGB(0, 195, 240));

        HPEN trackPen = CreatePen(PS_SOLID, k.isBigColor ? 4 : 3, RGB(38, 45, 68));
        HPEN activePen = CreatePen(PS_SOLID, k.isBigColor ? 4 : 3, ringCol);
        HBRUSH bodyBrush = CreateSolidBrush(RGB(26, 30, 44));

        HGDIOBJ oldPen = SelectObject(hdc, trackPen);
        HGDIOBJ oldBr = SelectObject(hdc, bodyBrush);
        Ellipse(hdc, k.cx - r, k.cy - r, k.cx + r, k.cy + r);

        // Indicator line from center to angle (-135 deg to +135 deg)
        float angleDeg = -135.0f + norm * 270.0f;
        float angleRad = (angleDeg - 90.0f) * static_cast<float>(kPi) / 180.0f;
        int ix0 = k.cx + static_cast<int>(std::cos(angleRad) * (r * 0.25f));
        int iy0 = k.cy + static_cast<int>(std::sin(angleRad) * (r * 0.25f));
        int ix1 = k.cx + static_cast<int>(std::cos(angleRad) * (r * 0.88f));
        int iy1 = k.cy + static_cast<int>(std::sin(angleRad) * (r * 0.88f));

        SelectObject(hdc, activePen);
        MoveToEx(hdc, ix0, iy0, nullptr);
        LineTo(hdc, ix1, iy1);

        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBr);
        DeleteObject(trackPen);
        DeleteObject(activePen);
        DeleteObject(bodyBrush);

        // Label below knob
        SelectObject(hdc, fontSmall);
        SetTextColor(hdc, RGB(175, 190, 220));
        RECT rcLabel{ k.cx - 65, k.cy + r + 5, k.cx + 65, k.cy + r + 21 };
        DrawTextA(hdc, k.label, -1, &rcLabel, DT_CENTER | DT_TOP | DT_SINGLELINE);

        // Formatted value inside or below
        char valBuf[32] = "";
        switch (k.paramId) {
            case kParamColor:
                std::snprintf(valBuf, sizeof(valBuf), "%.0f%%", norm * 200.0f);
                break;
            case kParamDecay:
                std::snprintf(valBuf, sizeof(valBuf), "%.0f ms", 25.0f + std::pow(norm, 1.5f) * 1975.0f);
                break;
            case kParamSpread:
            case kParamSpectralSnap:
            case kParamTransient:
            case kParamDryLevel:
                std::snprintf(valBuf, sizeof(valBuf), "%.0f%%", norm * 100.0f);
                break;
            case kParamSpectralGate:
                if (norm <= 0.01f) std::snprintf(valBuf, sizeof(valBuf), "OFF");
                else std::snprintf(valBuf, sizeof(valBuf), "%.0f dB", -60.0f + norm * 52.0f);
                break;
            case kParamLowCut:
                std::snprintf(valBuf, sizeof(valBuf), "%.0f Hz", 30.0f * std::pow(800.0f / 30.0f, norm));
                break;
            case kParamHighCut:
                std::snprintf(valBuf, sizeof(valBuf), "%.1f kHz", (1000.0f * std::pow(16000.0f / 1000.0f, norm)) * 0.001f);
                break;
            case kParamRootStep:
                std::snprintf(valBuf, sizeof(valBuf), "Step %d", static_cast<int>(std::round(norm * 40.0f)));
                break;
            case kParamOutputGain:
                std::snprintf(valBuf, sizeof(valBuf), "%+.1f dB", -12.0f + norm * 24.0f);
                break;
        }

        SelectObject(hdc, k.isBigColor ? fontUI : fontSmall);
        SetTextColor(hdc, ringCol);
        RECT rcVal{ k.cx - 55, k.cy + r + 19, k.cx + 55, k.cy + r + 36 };
        DrawTextA(hdc, valBuf, -1, &rcVal, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    void render41EdoKeyboard(HDC hdc, HFONT fontUI, HFONT fontSmall) {
        // Header bar above the 41 keys
        SelectObject(hdc, fontUI);
        SetTextColor(hdc, RGB(210, 225, 255));
        TextOutA(hdc, 20, 400, "41-EDO MICROTONAL SCALE & RESONATOR MATRIX (Click or Drag Steps 0..40)", 70);

        if (hoveredStep >= 0 && hoveredStep < kStepsPerOctave) {
            char hoverBuf[160];
            float cents = hoveredStep * (1200.0f / 41.0f);
            std::snprintf(hoverBuf, sizeof(hoverBuf),
                "Hover Step %d/41 | %.2f cents | JI Approx: %s (%s)",
                hoveredStep, cents, kEdo41StepTable[hoveredStep].name, kEdo41StepTable[hoveredStep].ratioHint);
            SetTextColor(hdc, RGB(0, 235, 255));
            TextOutA(hdc, 540, 400, hoverBuf, static_cast<int>(std::strlen(hoverBuf)));
        }

        SelectObject(hdc, fontSmall);

        for (int s = 0; s < kStepsPerOctave; ++s) {
            RECT r = getStepKeyRect(s);
            bool uiOn = engine ? engine->isStepActive(s) : false;
            bool effOn = engine ? engine->isEffectivePitchClassActive(s) : uiOn;
            float energy = engine ? engine->getStepVisualEnergy(s) : 0.0f;
            int cat = kEdo41StepTable[s].category;

            // Color coding by harmonic category & active state
            COLORREF baseCol;
            COLORREF borderCol;
            if (effOn) {
                if (cat == 0) { // Root 0 (1/1)
                    baseCol = RGB(220, 165, 20);
                    borderCol = RGB(255, 225, 100);
                } else if (cat == 1) { // 5-Limit Consonant
                    baseCol = RGB(0, 145, 195);
                    borderCol = RGB(80, 235, 255);
                } else if (cat == 2) { // 7/11-Limit Harmonic Color
                    baseCol = RGB(150, 45, 195);
                    borderCol = RGB(235, 120, 255);
                } else { // Comma / Microstep
                    baseCol = RGB(55, 95, 165);
                    borderCol = RGB(120, 175, 255);
                }
            } else {
                baseCol = (cat == 1 || cat == 0) ? RGB(26, 31, 45) : RGB(19, 22, 32);
                borderCol = (s == hoveredStep) ? RGB(120, 160, 220) : RGB(40, 48, 70);
            }

            fillRectColor(hdc, r.left, r.top, r.right, r.bottom, baseCol);

            // Real-time resonance glow bar rising from the bottom of the key
            if (energy > 0.01f) {
                int barH = static_cast<int>(std::clamp(energy, 0.0f, 1.0f) * (r.bottom - r.top - 30));
                fillRectColor(hdc, r.left + 2, r.bottom - barH, r.right - 2, r.bottom - 2, RGB(255, 240, 150));
            }

            drawFrameRect(hdc, r.left, r.top, r.right, r.bottom, borderCol);

            // Step number at top of key
            char stepNum[8];
            std::snprintf(stepNum, sizeof(stepNum), "%d", s);
            SetTextColor(hdc, effOn ? RGB(255, 255, 255) : RGB(120, 135, 170));
            RECT rcTop{ r.left, r.top + 6, r.right, r.top + 22 };
            DrawTextA(hdc, stepNum, -1, &rcTop, DT_CENTER | DT_TOP | DT_SINGLELINE);

            // Ratio hint for key harmonic steps
            const char* ratioStr = "";
            switch (s) {
                case 0:  ratioStr = "1/1"; break;
                case 4:  ratioStr = "16/15"; break;
                case 7:  ratioStr = "9/8"; break;
                case 8:  ratioStr = "8/7"; break;
                case 9:  ratioStr = "7/6"; break;
                case 10: ratioStr = "6/5"; break;
                case 11: ratioStr = "11/9"; break;
                case 13: ratioStr = "5/4"; break;
                case 14: ratioStr = "9/7"; break;
                case 17: ratioStr = "4/3"; break;
                case 18: ratioStr = "11/8"; break;
                case 20: ratioStr = "7/5"; break;
                case 24: ratioStr = "3/2"; break;
                case 27: ratioStr = "14/9"; break;
                case 28: ratioStr = "8/5"; break;
                case 30: ratioStr = "18/11"; break;
                case 31: ratioStr = "5/3"; break;
                case 33: ratioStr = "7/4"; break;
                case 34: ratioStr = "16/9"; break;
                case 36: ratioStr = "11/6"; break;
                case 37: ratioStr = "15/8"; break;
            }
            if (ratioStr[0] != '\0') {
                SetTextColor(hdc, effOn ? RGB(240, 250, 190) : RGB(90, 110, 145));
                RECT rcBot{ r.left - 2, r.bottom - 24, r.right + 2, r.bottom - 4 };
                DrawTextA(hdc, ratioStr, -1, &rcBot, DT_CENTER | DT_TOP | DT_SINGLELINE);
            }
        }
    }
};

} // namespace Chroma41
