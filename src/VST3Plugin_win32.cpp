#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <shlobj.h>
#include <filesystem>

#include "FLPluginInstance.h"
#include "FLPathManager.h"
#include "FLPluginScanner.h"
#include "BridgeEditorLayout.h"
#include "FloopHoleFactory.h"
#include "ToolbarControls_win32.h"
#include "Win32EditorDrawing.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/gui/iplugview.h"

#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <memory>
#include <cmath>
#include <cstdio>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace flbr {

static constexpr COLORREF kToolbarBackground = RGB((ToolbarTheme::background >> 16) & 255,
    (ToolbarTheme::background >> 8) & 255, ToolbarTheme::background & 255);
static constexpr int32 kToolbarHeight = 54;
static constexpr int32 kLegacyAutomatableParams = 128;
static constexpr int32 kMaxAutomatableParams = 2048;

static constexpr ParamID kParamPitchBend    = 20000;
static constexpr ParamID kParamModWheel     = 20001;
static constexpr ParamID kParamAftertouch   = 20002;
static constexpr ParamID kParamSustain      = 20003;

static constexpr UINT_PTR kTimerIdIdle  = 2001;

static DWORD gMainThreadId = 0;

static void utf8ToString128(const std::string& src, String128 dst) {
    memset(dst, 0, sizeof(String128));
    size_t n = std::min<size_t>(src.size(), 127);
    for (size_t i = 0; i < n; ++i) {
        dst[i] = (TChar)(uint8_t)src[i];
    }
}

class FLBridgeComponent;
class FLBridgePlugView;

// ============================================================================
// FLBridgePlugView: Steinberg::IPlugView hosting Win32 Toolbar + Native FL HWND
// ============================================================================

class FLBridgePlugView : public IPlugView {
public:
    FLBridgePlugView(FLBridgeComponent* owner, FLPluginInstance* flInstance);
    virtual ~FLBridgePlugView();

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override;
    uint32 PLUGIN_API addRef() override { return ++m_refCount; }
    uint32 PLUGIN_API release() override {
        uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    // IPlugView
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override;
    tresult PLUGIN_API attached(void* parent, FIDString type) override;
    tresult PLUGIN_API removed() override;
    tresult PLUGIN_API onWheel(float distance) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16 key, int16 keyCode, int16 modifiers) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16 key, int16 keyCode, int16 modifiers) override { return kResultFalse; }
    tresult PLUGIN_API getSize(ViewRect* size) override;
    tresult PLUGIN_API onSize(ViewRect* newSize) override;
    tresult PLUGIN_API onFocus(TBool state) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame* frame) override { m_plugFrame = frame; return kResultOk; }
    tresult PLUGIN_API canResize() override;
    tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override;

    // UI Actions
    void selectPresetFromMenu(int presetIndex);
    void stepPreset(int delta);
    void promptLoadFstFile();
    void onTimerTick();
    void refreshToolbarState();
    void setHintText(const std::string& text);
    void handlePluginResized(int plugW, int plugH);
    void resizeFromCorner(int width, int height);
    double getOutputControlValue() const { return m_flInstance->getOutputControlValue(); }
    double getOutputControlDefault() const { return m_flInstance->getOutputControlDefault(); }
    std::string getOutputControlHint() const { return m_flInstance->getOutputControlHint(); }
    bool isEffect() const { return !m_flInstance->isInstrument(); }
    void editOutputControl(double value, bool begin, bool end);

    static LRESULT CALLBACK RootWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK GripWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK VolumeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    void rebuildPresetMenu();
    void layoutSubviews(int plugW, int plugH);
    void constrainEditorDimensions(int& width, int& height);

    std::atomic<uint32> m_refCount{1};
    FLBridgeComponent* m_owner = nullptr;
    FLPluginInstance* m_flInstance = nullptr;
    IPlugFrame* m_plugFrame = nullptr;
    bool m_resizingEditor = false;

    int32 m_plugWidth = 1024;
    int32 m_nativeWidth = 1024;
    int32 m_nativeHeight = 600;
    int32 m_plugHeight = 680;
    int32 m_toolbarHeight = 54;
    int32 m_resizeFooterHeight = 0;
    std::array<HWND, kBridgeToolbarControlCount> m_toolbarControls{};
    HWND m_resizeGrip = nullptr;
    HWND m_volumeKnob = nullptr;
    ULONG_PTR m_gdiplusToken = 0;
    double m_drawnVolume = kDefaultMasterVolume;
    double m_volumeStart = kDefaultMasterVolume;
    int m_volumeStartY = 0;
    bool m_volumeDragging = false;
    POINT m_dragStart{};
    int m_dragWidth = 0, m_dragHeight = 0;

    HWND m_rootHwnd = nullptr;
    HINSTANCE m_windowClassModule = nullptr;
    std::unique_ptr<WindowsToolbar> m_toolbar;
    HWND m_pluginHostHwnd = nullptr;
    HBRUSH m_bgBrush = nullptr;

};

// ============================================================================
// FLBridgeComponent: Combined VST3 Processor + EditController
// ============================================================================

class FLBridgeComponent : public IComponent,
                          public IAudioProcessor,
                          public IEditController,
                          public IEditController2,
                          public IMidiMapping {
public:
    explicit FLBridgeComponent(const NativePluginClass& plugin);
    virtual ~FLBridgeComponent();

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override;
    uint32 PLUGIN_API addRef() override { return ++m_refCount; }
    uint32 PLUGIN_API release() override {
        uint32 r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown* context) override;
    tresult PLUGIN_API terminate() override;

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID classId) override;
    tresult PLUGIN_API setIoMode(IoMode mode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override;
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override;
    tresult PLUGIN_API getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) override;
    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool state) override;
    tresult PLUGIN_API setActive(TBool state) override;
    tresult PLUGIN_API setState(IBStream* state) override;
    tresult PLUGIN_API getState(IBStream* state) override;

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                          SpeakerArrangement* outputs, int32 numOuts) override;
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override;
    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override;
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override;
    tresult PLUGIN_API setProcessing(TBool state) override;
    tresult PLUGIN_API process(ProcessData& data) override;
    uint32 PLUGIN_API getTailSamples() override { return kNoTail; }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream* state) override;
    int32 PLUGIN_API getParameterCount() override;
    tresult PLUGIN_API getParameterInfo(int32 paramIndex, ParameterInfo& info) override;
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) override;
    tresult PLUGIN_API getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) override;
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue valueNormalized) override { return valueNormalized; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue plainValue) override { return plainValue; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override;
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override;
    tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) override;
    IPlugView* PLUGIN_API createView(FIDString name) override;

    // IEditController2
    tresult PLUGIN_API setKnobMode(KnobMode mode) override { return kResultOk; }
    tresult PLUGIN_API openHelp(TBool onlyCheck) override { return kResultFalse; }
    tresult PLUGIN_API openAboutBox(TBool onlyCheck) override { return kResultFalse; }

    // IMidiMapping
    tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel,
                                                   CtrlNumber midiControllerNumber, ParamID& id) override;

    FLPluginInstance& getFLInstance() { return m_flInstance; }
    ParamID getOutputControlID() const { return m_defaultIsEffect ? kParamEffectMix : kParamMasterVolume; }
    void editOutputControl(double value, bool begin, bool end);
    void onViewDestroyed(FLBridgePlugView* v) {
        if (m_activeView == v) m_activeView = nullptr;
    }

private:
    std::atomic<uint32> m_refCount{1};
    const NativePluginClass m_pluginClass;
    bool m_defaultIsEffect = false;
    bool m_initialized = false;
    FLPluginInstance m_flInstance;
    IComponentHandler* m_componentHandler = nullptr;
    FLBridgePlugView* m_activeView = nullptr;

    double m_pitchBendNorm = 0.5;
    double m_modWheelNorm = 0.0;
    double m_aftertouchNorm = 0.0;
    double m_sustainNorm = 0.0;
    double m_transportSampleRate = 44100.0;
    double m_nextProjectQuarters = 0.0;
    double m_nextProjectSeconds = 0.0;
    int32 m_timeSigNumerator = 4;
    int32 m_timeSigDenominator = 4;
};

// ============================================================================
// FLBridgePlugView Implementation (Win32)
// ============================================================================

static Win32WindowClasses& editorWindowClasses() {
    static Win32WindowClasses classes({
        {L"FloopHoleVST3RootWnd", FLBridgePlugView::RootWndProc, CS_HREDRAW | CS_VREDRAW, LoadCursor(nullptr, IDC_ARROW)},
        {L"FloopHoleVST3ResizeGrip", FLBridgePlugView::GripWndProc, CS_HREDRAW | CS_VREDRAW, LoadCursor(nullptr, IDC_SIZENWSE)},
        {L"FloopHoleOutputKnob", FLBridgePlugView::VolumeWndProc, CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, LoadCursor(nullptr, IDC_HAND)}
    });
    return classes;
}

LRESULT CALLBACK FLBridgePlugView::GripWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<FLBridgePlugView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        switch (msg) {
            case WM_LBUTTONDOWN:
                if (self->canResize() != kResultTrue) return 0;
                GetCursorPos(&self->m_dragStart);
                self->m_dragWidth = self->m_plugWidth;
                self->m_dragHeight = self->m_plugHeight + self->m_toolbarHeight + self->m_resizeFooterHeight;
                SetCapture(hwnd);
                return 0;
            case WM_MOUSEMOVE:
                if (GetCapture() == hwnd) {
                    POINT point{};
                    GetCursorPos(&point);
                    self->resizeFromCorner(self->m_dragWidth + point.x - self->m_dragStart.x,
                        self->m_dragHeight + point.y - self->m_dragStart.y);
                }
                return 0;
            case WM_LBUTTONUP:
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(hwnd, &paint);
                RECT rect{};
                GetClientRect(hwnd, &rect);
                drawEditorResizeGrip(dc, rect);
                EndPaint(hwnd, &paint);
                return 0;
            }
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FLBridgePlugView::VolumeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<FLBridgePlugView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            if (bounds.right <= 0 || bounds.bottom <= 0) {
                EndPaint(hwnd, &paint);
                return 0;
            }
            Gdiplus::Bitmap buffer(bounds.right, bounds.bottom, PixelFormat32bppPARGB);
            {
                Gdiplus::Graphics graphics(&buffer);
                graphics.Clear(Gdiplus::Color(255, GetRValue(kToolbarBackground),
                    GetGValue(kToolbarBackground), GetBValue(kToolbarBackground)));
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
                const float scale = std::min(bounds.right / 40.0f, bounds.bottom / 38.0f);
                graphics.TranslateTransform(bounds.right / 2.0f, 0);
                graphics.ScaleTransform(scale, scale);
                Gdiplus::LinearGradientBrush body(Gdiplus::PointF(0, 4), Gdiplus::PointF(0, 29),
                    Gdiplus::Color(255, 84, 92, 100), Gdiplus::Color(255, 33, 38, 46));
                graphics.FillEllipse(&body, -12.0f, 3.0f, 24.0f, 24.0f);
                Gdiplus::Pen outline(Gdiplus::Color(255, 20, 20, 20), 1);
                graphics.DrawEllipse(&outline, -12.0f, 3.0f, 24.0f, 24.0f);
                Gdiplus::Pen track(self->isEffect() ? Gdiplus::Color(255, 102, 79, 51) :
                    Gdiplus::Color(255, 74, 92, 87), 1.5f);
                Gdiplus::Pen active(self->isEffect() ? Gdiplus::Color(255, 255, 158, 41) :
                    Gdiplus::Color(255, 128, 240, 186), 1.5f);
                graphics.DrawArc(&track, -14.0f, 1.0f, 28.0f, 28.0f, 135.0f, 270.0f);
                graphics.DrawArc(&active, -14.0f, 1.0f, 28.0f, 28.0f, 135.0f,
                    static_cast<float>(270 * self->getOutputControlValue()));
                const double angle = (135 + 270 * self->getOutputControlValue()) * std::acos(-1.0) / 180;
                Gdiplus::SolidBrush marker(Gdiplus::Color(255, 240, 240, 240));
                graphics.FillEllipse(&marker, static_cast<float>(9 * std::cos(angle) - 1),
                    static_cast<float>(15 + 9 * std::sin(angle) - 1), 2.0f, 2.0f);
                Gdiplus::FontFamily family(L"Segoe UI");
                Gdiplus::Font font(&family, 7, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush text(Gdiplus::Color(255, 217, 217, 217));
                Gdiplus::StringFormat format = editorStringFormat();
                format.SetAlignment(Gdiplus::StringAlignmentCenter);
                format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                graphics.DrawString(self->isEffect() ? L"MIX" : L"VOL", -1, &font,
                    Gdiplus::RectF(-20, 29, 40, 9), &format, &text);
            }
            {
                Gdiplus::Graphics target(dc);
                target.DrawImage(&buffer, 0, 0);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            POINT point{};
            GetCursorPos(&point);
            self->m_volumeStartY = point.y;
            self->m_volumeStart = self->getOutputControlValue();
            self->m_volumeDragging = true;
            SetCapture(hwnd);
            self->editOutputControl(self->m_volumeStart, true, false);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (GetCapture() == hwnd) {
                POINT point{};
                GetCursorPos(&point);
                const double range = GetKeyState(VK_SHIFT) & 0x8000 ? 1500 : 150;
                self->editOutputControl(std::clamp(self->m_volumeStart + (self->m_volumeStartY - point.y) / range,
                    0.0, 1.0), false, false);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else self->setHintText(self->getOutputControlHint());
            return 0;
        }
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) {
                self->m_volumeDragging = false;
                self->editOutputControl(self->getOutputControlValue(), false, true);
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (self->m_volumeDragging) {
                self->m_volumeDragging = false;
                self->editOutputControl(self->getOutputControlValue(), false, true);
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            self->editOutputControl(self->getOutputControlDefault(), true, true);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            self->editOutputControl(std::clamp(self->getOutputControlValue() + GET_WHEEL_DELTA_WPARAM(wParam) / 120.0 * 0.01,
                0.0, 1.0), true, true);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_UP || wParam == VK_RIGHT || wParam == VK_DOWN || wParam == VK_LEFT) {
                const double step = GetKeyState(VK_SHIFT) & 0x8000 ? 0.001 : 0.01;
                const double direction = wParam == VK_UP || wParam == VK_RIGHT ? 1 : -1;
                self->editOutputControl(std::clamp(self->getOutputControlValue() + direction * step, 0.0, 1.0), true, true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

FLBridgePlugView::FLBridgePlugView(FLBridgeComponent* owner, FLPluginInstance* flInstance)
    : m_owner(owner), m_flInstance(flInstance) {
}

FLBridgePlugView::~FLBridgePlugView() {
    removed();
    if (m_owner) {
        m_owner->onViewDestroyed(this);
    }
}

tresult PLUGIN_API FLBridgePlugView::queryInterface(const TUID _iid, void** obj) {
    if (FUnknownPrivate::iidEqual(_iid, IPlugView::iid) ||
        FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
        addRef();
        *obj = static_cast<IPlugView*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API FLBridgePlugView::isPlatformTypeSupported(FIDString type) {
    if (type && strcmp(type, kPlatformTypeHWND) == 0) {
        return kResultTrue;
    }
    return kResultFalse;
}

LRESULT CALLBACK FLBridgePlugView::RootWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<FLBridgePlugView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_TIMER && wParam == kTimerIdIdle) {
        self->onTimerTick();
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        FillRect(dc, &paint.rcPaint, self->m_bgBrush);
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (msg == WM_CTLCOLORSTATIC) {
        SetBkColor(reinterpret_cast<HDC>(wParam), kToolbarBackground);
        return reinterpret_cast<LRESULT>(self->m_bgBrush);
    }
    if (msg == WM_APP + 1) {
        SetFocus(GetParent(hwnd));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

tresult PLUGIN_API FLBridgePlugView::attached(void* parent, FIDString type) {
    if (!parent || isPlatformTypeSupported(type) != kResultTrue) {
        return kResultFalse;
    }

    if (m_rootHwnd || m_windowClassModule) {
        fprintf(stderr, "FloopHole: editor is already attached\n");
        return kResultFalse;
    }
    gMainThreadId = GetCurrentThreadId();
    m_windowClassModule = editorWindowClasses().acquire();
    if (!m_windowClassModule) return kResultFalse;
    Gdiplus::GdiplusStartupInput graphicsInput;
    graphicsInput.SuppressExternalCodecs = TRUE;
    const auto graphicsStatus = Gdiplus::GdiplusStartup(&m_gdiplusToken, &graphicsInput, nullptr);
    if (graphicsStatus != Gdiplus::Ok) {
        fprintf(stderr, "FloopHole: vector volume rendering could not initialize (%d)\n", graphicsStatus);
        m_gdiplusToken = 0;
        editorWindowClasses().release();
        m_windowClassModule = nullptr;
        return kResultFalse;
    }

    HWND parentHwnd = (HWND)parent;
    HINSTANCE hInst = m_windowClassModule;

    m_bgBrush = CreateSolidBrush(kToolbarBackground);
    if (!m_bgBrush) {
        fprintf(stderr, "FloopHole: could not create editor background (%lu)\n", GetLastError());
        removed();
        return kResultFalse;
    }

    m_rootHwnd = CreateWindowExW(0, L"FloopHoleVST3RootWnd", L"",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                 0, 0, m_plugWidth, m_plugHeight + m_toolbarHeight + m_resizeFooterHeight,
                                 parentHwnd, nullptr, hInst, this);
    if (!m_rootHwnd) {
        fprintf(stderr, "FloopHole: could not create editor window (%lu)\n", GetLastError());
        removed();
        return kResultFalse;
    }

    m_pluginHostHwnd = CreateWindowExW(0, L"STATIC", L"",
                                       WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                       0, kToolbarHeight, m_plugWidth, m_plugHeight,
                                       m_rootHwnd, nullptr, hInst, nullptr);
    if (!m_pluginHostHwnd) {
        fprintf(stderr, "FloopHole: could not create native editor container (%lu)\n", GetLastError());
        removed();
        return kResultFalse;
    }

    WindowsToolbar::Actions actions;
    actions.preset = [this](int preset) {
        selectPresetFromMenu(preset);
        PostMessageW(m_rootHwnd, WM_APP + 1, 0, 0);
    };
    actions.previous = [this] { stepPreset(-1); };
    actions.next = [this] { stepPreset(1); };
    actions.load = [this] { promptLoadFstFile(); };
    m_toolbar = std::make_unique<WindowsToolbar>(m_rootHwnd, std::move(actions));
    if (!m_toolbar->attach()) {
        removed();
        return kResultFalse;
    }
    std::copy(m_toolbar->controls().begin(), m_toolbar->controls().end(), m_toolbarControls.begin());
    m_volumeKnob = CreateWindowExW(0, L"FloopHoleOutputKnob", isEffect() ? L"Effect mix" : L"Master volume",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 40, 38, m_rootHwnd, nullptr, hInst, this);
    if (!m_volumeKnob) {
        fprintf(stderr, "FloopHole: could not create output knob (%lu)\n", GetLastError());
        removed();
        return kResultFalse;
    }
    m_toolbarControls[kBridgeMasterVolumeControl] = m_volumeKnob;
    m_resizeGrip = CreateWindowExW(0, L"FloopHoleVST3ResizeGrip", L"",
        WS_CHILD, 0, 0, kBridgeResizeFooterHeight, kBridgeResizeFooterHeight,
        m_rootHwnd, nullptr, hInst, this);
    if (!m_resizeGrip) {
        fprintf(stderr, "FloopHole: could not create resize grip (Windows error %lu)\n", GetLastError());
        removed();
        return kResultFalse;
    }
    rebuildPresetMenu();
    if (m_flInstance) {
        m_flInstance->onHintChanged = [this](const std::string& hint) {
            setHintText(hint);
        };
        m_flInstance->onEditorResized = [this](int w, int h) {
            handlePluginResized(w, h);
        };

        int pw = 900, ph = 600;
        if (m_flInstance->attachEditorToNSView((void*)m_pluginHostHwnd, pw, ph)) {
            layoutSubviews(pw, ph);
            if (m_plugFrame) {
                ViewRect vr{};
                getSize(&vr);
                m_plugFrame->resizeView(this, &vr);
            }
        }
    }

    SetTimer(m_rootHwnd, kTimerIdIdle, 16, nullptr);
    return kResultOk;
}

tresult PLUGIN_API FLBridgePlugView::removed() {
    if (!m_rootHwnd && !m_windowClassModule && !m_toolbar && !m_gdiplusToken) return kResultOk;
    if (m_toolbar) m_toolbar->closeMenu();
    if (m_volumeKnob && GetCapture() == m_volumeKnob) {
        m_volumeDragging = false;
        editOutputControl(getOutputControlValue(), false, true);
        ReleaseCapture();
    }
    if (m_resizeGrip && GetCapture() == m_resizeGrip) ReleaseCapture();
    m_resizeGrip = nullptr;
    m_toolbarControls.fill(nullptr);
    if (m_rootHwnd) {
        KillTimer(m_rootHwnd, kTimerIdIdle);
    }
    if (m_flInstance && m_rootHwnd) {
        m_flInstance->onHintChanged = nullptr;
        m_flInstance->onEditorResized = nullptr;
        m_flInstance->detachEditor();
    }
    m_toolbar.reset();
    if (m_rootHwnd) {
        SetWindowLongPtrW(m_rootHwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_rootHwnd);
        m_rootHwnd = nullptr;
    }
    m_pluginHostHwnd = nullptr;
    m_volumeKnob = nullptr;
    if (m_gdiplusToken) { Gdiplus::GdiplusShutdown(m_gdiplusToken); m_gdiplusToken = 0; }
    if (m_windowClassModule) {
        editorWindowClasses().release();
        m_windowClassModule = nullptr;
    }

    if (m_bgBrush) { DeleteObject(m_bgBrush); m_bgBrush = nullptr; }

    return kResultOk;
}

void FLBridgePlugView::layoutSubviews(int plugW, int plugH) {
    m_nativeWidth = plugW;
    m_nativeHeight = plugH;
    m_plugWidth = plugW;
    m_plugHeight = plugH;
    const auto toolbar = BridgeToolbarLayout::compute(plugW);
    m_toolbarHeight = toolbar.height;
    m_resizeFooterHeight = canResize() == kResultTrue ? kBridgeResizeFooterHeight : 0;
    if (m_rootHwnd) {
        SetWindowPos(m_rootHwnd, nullptr, 0, 0, m_plugWidth, m_plugHeight + m_toolbarHeight + m_resizeFooterHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (m_pluginHostHwnd) {
        SetWindowPos(m_pluginHostHwnd, nullptr, 0, m_toolbarHeight, plugW, plugH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (size_t i = 0; i < m_toolbarControls.size(); ++i) {
        const auto& frame = toolbar.controls[i];
        HWND control = m_toolbarControls[i];
        if (control) SetWindowPos(control, nullptr, frame.x, frame.y, frame.width,
            frame.height,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (m_resizeGrip) {
        SetWindowPos(m_resizeGrip, nullptr, std::max(0, plugW - kBridgeResizeFooterHeight),
            m_toolbarHeight + plugH, kBridgeResizeFooterHeight, kBridgeResizeFooterHeight,
            SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(m_resizeGrip, m_resizeFooterHeight ? SW_SHOWNA : SW_HIDE);
    }
}

void FLBridgePlugView::handlePluginResized(int plugW, int plugH) {
    if (m_resizingEditor) return;
    layoutSubviews(plugW, plugH);
    if (m_plugFrame) {
        ViewRect vr{};
        getSize(&vr);
        m_plugFrame->resizeView(this, &vr);
    }
}

tresult PLUGIN_API FLBridgePlugView::getSize(ViewRect* size) {
    if (!size) return kInvalidArgument;
    size->left = 0;
    size->top = 0;
    size->right = m_plugWidth;
    size->bottom = m_plugHeight + m_toolbarHeight + m_resizeFooterHeight;
    return kResultOk;
}

tresult PLUGIN_API FLBridgePlugView::onSize(ViewRect* newSize) {
    if (!newSize) return kInvalidArgument;
    const int w = newSize->getWidth(), h = newSize->getHeight();
    if (m_resizingEditor || (w == m_plugWidth && h == m_plugHeight + m_toolbarHeight + m_resizeFooterHeight)) return kResultOk;
    if (w <= 0 || h <= 0 || !m_flInstance || !m_flInstance->isEditorOpen()) {
        fprintf(stderr, "FloopHole: cannot apply this editor size before native attachment\n");
        return kInvalidArgument;
    }
    int nativeWidth = w, nativeHeight = h;
    constrainEditorDimensions(nativeWidth, nativeHeight);
    m_resizingEditor = true;
    const bool resized = canResize() == kResultFalse || m_flInstance->resizeEditor(nativeWidth, nativeHeight);
    m_resizingEditor = false;
    if (!resized) return kResultFalse;
    layoutSubviews(nativeWidth, nativeHeight);
    if ((w != m_plugWidth || h != m_plugHeight + m_toolbarHeight + m_resizeFooterHeight) && m_plugFrame) {
        ViewRect corrected{};
        getSize(&corrected);
        m_plugFrame->resizeView(this, &corrected);
    }
    return kResultOk;
}

tresult PLUGIN_API FLBridgePlugView::canResize() {
    return m_flInstance && m_flInstance->isLoaded() && m_flInstance->getEditorSizeConstraints().isResizable()
        ? kResultTrue : kResultFalse;
}

void FLBridgePlugView::constrainEditorDimensions(int& width, int& height) {
    const auto constraints = m_flInstance->getEditorSizeConstraints();
    int retainedHeight = m_nativeHeight;
    constraints.constrain(width, retainedHeight);
    height -= BridgeToolbarLayout::compute(width).height +
        (constraints.isResizable() ? kBridgeResizeFooterHeight : 0);
    constraints.constrain(width, height);
}

tresult PLUGIN_API FLBridgePlugView::checkSizeConstraint(ViewRect* rect) {
    if (!rect) return kInvalidArgument;
    if (m_flInstance && m_flInstance->isEditorOpen()) {
        int width = rect->getWidth(), height = rect->getHeight();
        constrainEditorDimensions(width, height);
        rect->right = rect->left + width;
        rect->bottom = rect->top + bridgeEditorHeight(width, height,
            m_flInstance->getEditorSizeConstraints().isResizable());
        return kResultOk;
    }
    rect->right = rect->left + m_plugWidth;
    rect->bottom = rect->top + m_plugHeight + m_toolbarHeight + m_resizeFooterHeight;
    return kResultOk;
}

void FLBridgePlugView::resizeFromCorner(int width, int height) {
    if (!m_flInstance || canResize() != kResultTrue) return;
    constrainEditorDimensions(width, height);
    ViewRect requested(0, 0, width, bridgeEditorHeight(width, height, true));
    if (!m_plugFrame || m_plugFrame->resizeView(this, &requested) != kResultOk) {
        fprintf(stderr, "FloopHole: host rejected native editor resize\n");
        setHintText("Host rejected editor resizing.");
    }
}

void FLBridgePlugView::rebuildPresetMenu() {
    if (!m_toolbar) return;
    const auto plugins = FLPluginScanner::getInstance().getPlugins();
    const int index = m_flInstance->getCurrentPluginIndex();
    const std::vector<FLPresetEntry> empty;
    m_toolbar->setPresets(index >= 0 && index < static_cast<int>(plugins.size()) ? plugins[index].presets : empty,
        m_flInstance->getCurrentPresetIndex());
}

void FLBridgePlugView::selectPresetFromMenu(int presetIndex) {
    if (!m_flInstance) return;
    if (m_flInstance->loadPreset(presetIndex)) {
        rebuildPresetMenu();
        const auto& plugins = FLPluginScanner::getInstance().getPlugins();
        int curPlug = m_flInstance->getCurrentPluginIndex();
        if (curPlug >= 0 && curPlug < (int)plugins.size() &&
            presetIndex >= 0 && presetIndex < (int)plugins[curPlug].presets.size()) {
            setHintText("Preset: " + plugins[curPlug].presets[presetIndex].displayName);
        }
    } else {
        setHintText("Could not load preset. See the host log for details.");
    }
}

void FLBridgePlugView::stepPreset(int delta) {
    if (!m_flInstance) return;
    const auto& plugins = FLPluginScanner::getInstance().getPlugins();
    int curPlug = m_flInstance->getCurrentPluginIndex();
    if (curPlug < 0 || curPlug >= (int)plugins.size()) return;
    int count = (int)plugins[curPlug].presets.size();
    if (count <= 0) return;

    int next = m_flInstance->getCurrentPresetIndex() + delta;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;

    selectPresetFromMenu(next);
    rebuildPresetMenu();
}

void FLBridgePlugView::promptLoadFstFile() {
    if (!m_flInstance) return;
    char fileBuf[1024] = {0};
    std::string initDir = FLPluginScanner::getInstance().getFLRootPath() +
                          "Data\\Patches\\Plugin presets\\" +
                          (m_flInstance->isGenerator() ? "Generators\\" : "Effects\\") +
                          m_flInstance->getPluginName();
    if (m_flInstance->getPluginName() == "FLEX")
        initDir = FLPluginScanner::getInstance().getFlexDataPath(PI_User) + "Packs";

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = m_rootHwnd;
    ofn.lpstrFilter = "Presets, sample banks and FLEX packs\0*.fst;*.tbio;*.mrp;*.gmsynth;*.flms;*.sf2;*.dwp;*.dwb;*.flexpack\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrInitialDir = initDir.c_str();
    ofn.lpstrTitle = "Load preset or import FLEX pack";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        if (m_flInstance->loadPresetFromFile(fileBuf)) {
            setHintText(FLPluginScanner::isFlexPackFile(fileBuf)
                ? "Imported FLEX pack. Use a new FLEX instance to refresh its library."
                : std::string("Loaded ") + fileBuf);
        } else {
            setHintText("Could not load file. Check its format and permissions; existing packs are not overwritten.");
        }
    }
}

void FLBridgePlugView::onTimerTick() {
    if (m_drawnVolume != getOutputControlValue()) {
        m_drawnVolume = getOutputControlValue();
        if (m_volumeKnob) InvalidateRect(m_volumeKnob, nullptr, FALSE);
        POINT point{};
        if (m_volumeKnob && GetCursorPos(&point) && WindowFromPoint(point) == m_volumeKnob)
            setHintText(getOutputControlHint());
    }
    if (m_flInstance) {
        m_flInstance->idleEditor();
    }
}

void FLBridgePlugView::refreshToolbarState() {
    if (m_volumeKnob) InvalidateRect(m_volumeKnob, nullptr, FALSE);
    rebuildPresetMenu();
}

void FLBridgePlugView::setHintText(const std::string& text) {
    if (m_toolbar) m_toolbar->setHint(text);
}

// ============================================================================
// FLBridgeComponent Implementation (Win32)
// ============================================================================

FLBridgeComponent::FLBridgeComponent(const NativePluginClass& plugin)
    : m_pluginClass(plugin), m_defaultIsEffect(!plugin.isGenerator), m_flInstance(plugin.isGenerator) {
}

FLBridgeComponent::~FLBridgeComponent() {
    if (m_componentHandler) {
        m_componentHandler->release();
        m_componentHandler = nullptr;
    }
}

tresult PLUGIN_API FLBridgeComponent::queryInterface(const TUID _iid, void** obj) {
    if (FUnknownPrivate::iidEqual(_iid, IComponent::iid) ||
        FUnknownPrivate::iidEqual(_iid, IPluginBase::iid) ||
        FUnknownPrivate::iidEqual(_iid, FUnknown::iid)) {
        addRef();
        *obj = static_cast<IComponent*>(this);
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, IAudioProcessor::iid)) {
        addRef();
        *obj = static_cast<IAudioProcessor*>(this);
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, IEditController::iid)) {
        addRef();
        *obj = static_cast<IEditController*>(this);
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, IEditController2::iid)) {
        addRef();
        *obj = static_cast<IEditController2*>(this);
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(_iid, IMidiMapping::iid)) {
        addRef();
        *obj = static_cast<IMidiMapping*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API FLBridgeComponent::initialize(FUnknown* context) {
    if (m_initialized) return kResultOk;
    if (gMainThreadId == 0) gMainThreadId = GetCurrentThreadId();
    auto& scanner = FLPluginScanner::getInstance();
    if (!scanner.discover()) return kResultFalse;

    m_flInstance.onParamChangedFromPlug = [this](int paramIdx, double normVal) {
        if (paramIdx >= 0 && paramIdx < kMaxAutomatableParams && m_componentHandler) {
            ParamID pid = (ParamID)paramIdx;
            if (GetCurrentThreadId() == gMainThreadId) {
                m_componentHandler->beginEdit(pid);
                m_componentHandler->performEdit(pid, normVal);
                m_componentHandler->endEdit(pid);
            }
        }
    };

    const int pluginIndex = scanner.findPluginIndexByIdentifier(m_pluginClass.identifier);
    if (pluginIndex < 0 || !m_flInstance.loadPlugin(pluginIndex)) {
        fprintf(stderr, "FloopHole: cannot initialize %s (%s)\n",
            m_pluginClass.name.c_str(), m_pluginClass.identifier.c_str());
        return kResultFalse;
    }

    m_initialized = true;
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::terminate() {
    m_flInstance.unloadPlugin();
    if (m_componentHandler) {
        m_componentHandler->release();
        m_componentHandler = nullptr;
    }
    m_initialized = false;
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::getControllerClassId(TUID classId) {
    memset(classId, 0, sizeof(TUID));
    return kResultFalse;
}

int32 PLUGIN_API FLBridgeComponent::getBusCount(MediaType type, BusDirection dir) {
    if (type == kAudio) {
        if (dir == kOutput) return 1;
        if (dir == kInput)  return 1;
    }
    if (type == kEvent) {
        if (dir == kInput)  return 1;
        if (dir == kOutput) return 0;
    }
    return 0;
}

tresult PLUGIN_API FLBridgeComponent::getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) {
    if (index != 0) return kInvalidArgument;
    if (type == kAudio && dir == kOutput) {
        bus.mediaType = kAudio;
        bus.direction = kOutput;
        bus.channelCount = 2;
        utf8ToString128("Stereo Out", bus.name);
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    if (type == kAudio && dir == kInput) {
        bus.mediaType = kAudio;
        bus.direction = kInput;
        bus.channelCount = 2;
        utf8ToString128("Stereo In", bus.name);
        bus.busType = m_defaultIsEffect ? kMain : kAux;
        bus.flags = m_defaultIsEffect ? BusInfo::kDefaultActive : 0;
        return kResultOk;
    }
    if (type == kEvent && dir == kInput) {
        bus.mediaType = kEvent;
        bus.direction = kInput;
        bus.channelCount = 16;
        utf8ToString128("MIDI In", bus.name);
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    return kInvalidArgument;
}

tresult PLUGIN_API FLBridgeComponent::getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) {
    outInfo.mediaType = kAudio;
    outInfo.busIndex = 0;
    outInfo.channel = -1;
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::activateBus(MediaType type, BusDirection dir, int32 index, TBool state) {
    return (index == 0) ? kResultOk : kInvalidArgument;
}

tresult PLUGIN_API FLBridgeComponent::setActive(TBool state) {
    m_flInstance.setEnabled(state);
    if (!state) {
        m_flInstance.allNotesOff();
    }
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::getState(IBStream* state) {
    if (!state) return kInvalidArgument;

    const char magic[4] = {'F', 'L', 'B', 'R'};
    const std::string samplePath = m_flInstance.getChannelSamplePath();
    uint32_t version = 5;
    std::string plugName = m_pluginClass.identifier;
    uint32_t nameLen = (uint32_t)plugName.size();
    int32_t presetIdx = (int32_t)m_flInstance.getCurrentPresetIndex();

    std::vector<uint8_t> rawState;
    m_flInstance.savePluginState(rawState);
    uint32_t stateLen = (uint32_t)rawState.size();

    const auto write = [&](const void* data, int32 size) {
        int32 written = 0;
        return state->write(const_cast<void*>(data), size, &written) == kResultOk && written == size;
    };
    if (!write(magic, 4) || !write(&version, 4) || !write(&nameLen, 4) ||
        (nameLen && !write(plugName.data(), nameLen)) || !write(&presetIdx, 4) || !write(&stateLen, 4) ||
        (stateLen && !write(rawState.data(), stateLen))) return kResultFalse;
    if (version >= 2) {
        const uint32_t length = static_cast<uint32_t>(samplePath.size());
        if (!write(&length, 4) || !write(samplePath.data(), length)) return kResultFalse;
    }
    const double volume = m_flInstance.getOutputControlValue();
    if (!write(&volume, sizeof(volume))) return kResultFalse;
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::setState(IBStream* state) {
    if (!state) return kInvalidArgument;

    char magic[4] = {0};
    int32 numRead = 0;
    if (state->read(magic, 4, &numRead) != kResultOk || numRead != 4 || memcmp(magic, "FLBR", 4) != 0) {
        fprintf(stderr, "FloopHole: missing or invalid project state header\n");
        return kResultFalse;
    }

    uint32_t version = 0;
    uint32_t nameLen = 0;
    const auto read = [&](void* data, int32 size) {
        return state->read(data, size, &numRead) == kResultOk && numRead == size;
    };
    if (!read(&version, 4) || version < 1 || version > 5) return kResultFalse;
    if (!read(&nameLen, 4) || nameLen > (version >= 5 ? 4096u : 256u)) return kResultFalse;

    std::string plugName(nameLen, '\0');
    if (nameLen > 0) {
        if (!read(&plugName[0], nameLen)) return kResultFalse;
    }

    int32_t presetIdx = -1;
    uint32_t stateLen = 0;
    if (!read(&presetIdx, 4) || !read(&stateLen, 4) || stateLen >= 64 * 1024 * 1024) return kResultFalse;

    std::vector<uint8_t> rawState;
    if (stateLen > 0 && stateLen < 64 * 1024 * 1024) {
        rawState.resize(stateLen);
        if (!read(rawState.data(), stateLen)) return kResultFalse;
    }
    std::string samplePath;
    if (version >= 2) {
        uint32_t length = 0;
        if (!read(&length, 4) || length >= 256) return kResultFalse;
        samplePath.resize(length);
        if (length && !read(samplePath.data(), length)) return kResultFalse;
        if (samplePath.find('\0') != std::string::npos) return kResultFalse;
    }
    double volume = m_flInstance.getOutputControlDefault();
    if (version >= 3 && (!read(&volume, sizeof(volume)) || !std::isfinite(volume) || volume < 0 || volume > 1)) {
        fprintf(stderr, "FloopHole: invalid output control in project state\n");
        return kResultFalse;
    }
    if ((version >= 5 && plugName != m_pluginClass.identifier) ||
        (version < 5 && !plugName.empty() && plugName != m_pluginClass.name)) {
        fprintf(stderr, "FloopHole: rejecting state for '%s' in fixed class '%s'\n",
            plugName.c_str(), m_pluginClass.identifier.c_str());
        return kResultFalse;
    }
    if (!m_flInstance.isLoaded()) {
        const int index = FLPluginScanner::getInstance().findPluginIndexByIdentifier(m_pluginClass.identifier);
        if (index < 0 || !m_flInstance.loadPlugin(index)) return kResultFalse;
    }
    if (m_defaultIsEffect && version <= 3) volume = kDefaultEffectMix;
    if (!m_flInstance.setOutputControlValue(volume)) return kResultFalse;

    if (!rawState.empty()) {
        if (!m_flInstance.restorePluginState(rawState.data(), rawState.size())) return kResultFalse;
    } else if (presetIdx >= 0) {
        if (!m_flInstance.loadPreset(presetIdx)) return kResultFalse;
    }
    if (samplePath.empty()) m_flInstance.clearChannelSample();
    else if (!m_flInstance.loadChannelSample(samplePath)) return kResultFalse;

    if (m_activeView) {
        m_activeView->refreshToolbarState();
    }
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::setComponentState(IBStream* state) {
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                                         SpeakerArrangement* outputs, int32 numOuts) {
    if (numOuts >= 1 && outputs[0] != SpeakerArr::kStereo) {
        return kResultFalse;
    }
    if (numIns >= 1 && inputs[0] != SpeakerArr::kStereo && inputs[0] != SpeakerArr::kEmpty) {
        return kResultFalse;
    }
    return kResultTrue;
}

tresult PLUGIN_API FLBridgeComponent::getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) {
    if (index != 0) return kInvalidArgument;
    arr = SpeakerArr::kStereo;
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::canProcessSampleSize(int32 symbolicSampleSize) {
    return (symbolicSampleSize == kSample32) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API FLBridgeComponent::setupProcessing(ProcessSetup& setup) {
    if (!std::isfinite(setup.sampleRate) || setup.sampleRate <= 0) {
        fprintf(stderr, "FloopHole: invalid processing sample rate\n");
        return kInvalidArgument;
    }
    m_transportSampleRate = setup.sampleRate;
    m_flInstance.setSampleRate(setup.sampleRate);
    m_flInstance.setBlockSize(setup.maxSamplesPerBlock);
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::setProcessing(TBool state) {
    if (!state) {
        m_flInstance.allNotesOff();
    }
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::process(ProcessData& data) {
    if (data.inputParameterChanges) {
        int32 numParamChanged = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < numParamChanged; ++i) {
            IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
            if (!queue) continue;
            int32 pts = queue->getPointCount();
            if (pts <= 0) continue;
            int32 sampleOffset = 0;
            ParamValue value = 0.0;
            if (queue->getPoint(pts - 1, sampleOffset, value) == kResultTrue) {
                ParamID pid = queue->getParameterId();
                if (pid < kMaxAutomatableParams) {
                    m_flInstance.setParamNormalized((int)pid, value, false);
                } else if (pid == kParamPitchBend) {
                    m_pitchBendNorm = value;
                    float semitones = (float)((value - 0.5) * 4.0);
                    m_flInstance.setPitchBendSemitones(semitones);
                } else if (pid == getOutputControlID()) {
                    if (!m_flInstance.setOutputControlValue(value)) return kInvalidArgument;
                } else if (pid == kParamModWheel) {
                    m_modWheelNorm = value;
                    uint8_t ccVal = (uint8_t)std::clamp((int)std::lround(value * 127.0), 0, 127);
                    m_flInstance.sendMIDIMessage(0xB0, 1, ccVal);
                } else if (pid == kParamSustain) {
                    m_sustainNorm = value;
                    uint8_t ccVal = (uint8_t)(value >= 0.5 ? 127 : 0);
                    m_flInstance.sendMIDIMessage(0xB0, 64, ccVal);
                }
            }
        }
    }

    if (data.inputEvents) {
        int32 eventCount = data.inputEvents->getEventCount();
        Event e;
        for (int32 i = 0; i < eventCount; ++i) {
            if (data.inputEvents->getEvent(i, e) != kResultOk) continue;
            if (e.type == Event::kNoteOnEvent) {
                if (e.noteOn.velocity > 0.0f) {
                    m_flInstance.noteOn(e.noteOn.channel, e.noteOn.pitch, e.noteOn.velocity);
                } else {
                    m_flInstance.noteOff(e.noteOn.channel, e.noteOn.pitch, 0.5f);
                }
            } else if (e.type == Event::kNoteOffEvent) {
                m_flInstance.noteOff(e.noteOff.channel, e.noteOff.pitch, e.noteOff.velocity);
            }
        }
    }

    if (data.numSamples <= 0 || data.numOutputs <= 0 || !data.outputs) {
        return kResultOk;
    }

    float* outL = (data.outputs[0].numChannels >= 1) ? data.outputs[0].channelBuffers32[0] : nullptr;
    float* outR = (data.outputs[0].numChannels >= 2) ? data.outputs[0].channelBuffers32[1] : nullptr;

    const float* inL = nullptr;
    const float* inR = nullptr;
    if (data.numInputs > 0 && data.inputs && data.inputs[0].channelBuffers32) {
        if (data.inputs[0].numChannels >= 1) inL = data.inputs[0].channelBuffers32[0];
        if (data.inputs[0].numChannels >= 2) inR = data.inputs[0].channelBuffers32[1];
    }

    double tempo = m_flInstance.getTempoBPM();
    double ppq = m_nextProjectQuarters;
    double seconds = m_nextProjectSeconds;
    bool playing = false;
    if (data.processContext) {
        if (data.processContext->state & ProcessContext::kTempoValid) {
            if (std::isfinite(data.processContext->tempo) && data.processContext->tempo > 0)
                tempo = data.processContext->tempo;
            else fprintf(stderr, "FloopHole: invalid host tempo; retaining the previous tempo\n");
        }
        if ((data.processContext->state & ProcessContext::kProjectTimeMusicValid) &&
            std::isfinite(data.processContext->projectTimeMusic)) {
            ppq = data.processContext->projectTimeMusic;
        } else {
            if (data.processContext->state & ProcessContext::kProjectTimeMusicValid)
                fprintf(stderr, "FloopHole: invalid host musical position; extrapolating from its sample timeline\n");
            ppq += (data.processContext->projectTimeSamples / m_transportSampleRate - seconds) * tempo / 60.0;
        }
        seconds = data.processContext->projectTimeSamples / m_transportSampleRate;
        if (data.processContext->state & ProcessContext::kTimeSigValid) {
            m_timeSigNumerator = data.processContext->timeSigNumerator;
            m_timeSigDenominator = data.processContext->timeSigDenominator;
        }
        playing = (data.processContext->state & ProcessContext::kPlaying) != 0;
    }

    m_flInstance.processAudio(inL, inR, outL, outR, data.numSamples, tempo, ppq, playing,
                              seconds, m_timeSigNumerator, m_timeSigDenominator);
    const double elapsed = playing ? data.numSamples / m_transportSampleRate : 0.0;
    m_nextProjectQuarters = ppq + elapsed * tempo / 60.0;
    m_nextProjectSeconds = seconds + elapsed;
    data.outputs[0].silenceFlags = 0;
    return kResultOk;
}

int32 PLUGIN_API FLBridgeComponent::getParameterCount() {
    return kMaxAutomatableParams + 5;
}

tresult PLUGIN_API FLBridgeComponent::getParameterInfo(int32 paramIndex, ParameterInfo& info) {
    if (paramIndex < 0 || paramIndex >= kMaxAutomatableParams + 5) return kInvalidArgument;
    memset(&info, 0, sizeof(ParameterInfo));
    info.unitId = kRootUnitId;
    if (paramIndex == kMaxAutomatableParams + 4) {
        info.id = getOutputControlID();
        utf8ToString128(m_defaultIsEffect ? "Effect Mix" : "Master Volume", info.title);
        utf8ToString128(m_defaultIsEffect ? "Mix" : "Volume", info.shortTitle);
        info.defaultNormalizedValue = m_flInstance.getOutputControlDefault();
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }

    // Append new native slots after the original native and bridge controls.
    if (paramIndex < kLegacyAutomatableParams || paramIndex >= kLegacyAutomatableParams + 4) {
        const int32 nativeIndex = paramIndex < kLegacyAutomatableParams ? paramIndex : paramIndex - 4;
        info.id = (ParamID)nativeIndex;
        std::string pname = m_flInstance.getParamName(nativeIndex);
        if (pname.empty()) {
            char fallback[64];
            snprintf(fallback, sizeof(fallback), "Param %d", nativeIndex + 1);
            pname = fallback;
        }
        utf8ToString128(pname, info.title);
        utf8ToString128(pname.substr(0, 16), info.shortTitle);
        info.stepCount = 0;
        info.defaultNormalizedValue = m_flInstance.getParamNormalized(nativeIndex);
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }

    int extra = paramIndex - kLegacyAutomatableParams;
    if (extra == 0) {
        info.id = kParamPitchBend;
        utf8ToString128("Pitch Bend", info.title);
        utf8ToString128("PitchBend", info.shortTitle);
        info.defaultNormalizedValue = 0.5;
        info.flags = ParameterInfo::kCanAutomate;
    } else if (extra == 1) {
        info.id = kParamModWheel;
        utf8ToString128("Mod Wheel", info.title);
        utf8ToString128("ModWheel", info.shortTitle);
        info.defaultNormalizedValue = 0.0;
        info.flags = ParameterInfo::kCanAutomate;
    } else if (extra == 2) {
        info.id = kParamAftertouch;
        utf8ToString128("Aftertouch", info.title);
        utf8ToString128("ATouch", info.shortTitle);
        info.defaultNormalizedValue = 0.0;
        info.flags = ParameterInfo::kCanAutomate;
    } else if (extra == 3) {
        info.id = kParamSustain;
        utf8ToString128("Sustain Pedal", info.title);
        utf8ToString128("Sustain", info.shortTitle);
        info.stepCount = 1;
        info.defaultNormalizedValue = 0.0;
        info.flags = ParameterInfo::kCanAutomate;
    }
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) {
    if (id == getOutputControlID()) {
        if (!std::isfinite(valueNormalized) || valueNormalized < 0 || valueNormalized > 1) {
            fprintf(stderr, "FloopHole: invalid output control display value\n");
            return kInvalidArgument;
        }
        utf8ToString128(m_flInstance.getOutputControlDisplay(valueNormalized), string);
        return kResultOk;
    }
    if (id < kMaxAutomatableParams) {
        std::string disp = m_flInstance.getParamDisplay((int)id);
        if (!disp.empty()) {
            utf8ToString128(disp, string);
            return kResultOk;
        }
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f%%", valueNormalized * 100.0);
    utf8ToString128(buf, string);
    return kResultOk;
}

tresult PLUGIN_API FLBridgeComponent::getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) {
    return kResultFalse;
}

ParamValue PLUGIN_API FLBridgeComponent::getParamNormalized(ParamID id) {
    if (id == getOutputControlID()) return m_flInstance.getOutputControlValue();
    if (id < kMaxAutomatableParams) {
        return m_flInstance.getParamNormalized((int)id);
    }
    if (id == kParamPitchBend)  return m_pitchBendNorm;
    if (id == kParamModWheel)   return m_modWheelNorm;
    if (id == kParamAftertouch) return m_aftertouchNorm;
    if (id == kParamSustain)    return m_sustainNorm;
    return 0.0;
}

tresult PLUGIN_API FLBridgeComponent::setParamNormalized(ParamID id, ParamValue value) {
    if (id == getOutputControlID()) return m_flInstance.setOutputControlValue(value) ? kResultOk : kInvalidArgument;
    if (id < kMaxAutomatableParams) {
        m_flInstance.setParamNormalized((int)id, value, true);
        return kResultOk;
    }
    if (id == kParamPitchBend) {
        m_pitchBendNorm = value;
        m_flInstance.setPitchBendSemitones((float)((value - 0.5) * 4.0));
        return kResultOk;
    }
    if (id == kParamModWheel)   { m_modWheelNorm = value; return kResultOk; }
    if (id == kParamAftertouch) { m_aftertouchNorm = value; return kResultOk; }
    if (id == kParamSustain)    { m_sustainNorm = value; return kResultOk; }
    return kResultFalse;
}

tresult PLUGIN_API FLBridgeComponent::setComponentHandler(IComponentHandler* handler) {
    if (m_componentHandler == handler) return kResultOk;
    if (m_componentHandler) m_componentHandler->release();
    m_componentHandler = handler;
    if (m_componentHandler) m_componentHandler->addRef();
    return kResultOk;
}

void FLBridgeComponent::editOutputControl(double value, bool begin, bool end) {
    if (!m_flInstance.setOutputControlValue(value)) return;
    if (m_componentHandler) {
        if (begin) m_componentHandler->beginEdit(getOutputControlID());
        m_componentHandler->performEdit(getOutputControlID(), value);
        if (end) m_componentHandler->endEdit(getOutputControlID());
    }
}

void FLBridgePlugView::editOutputControl(double value, bool begin, bool end) {
    m_owner->editOutputControl(value, begin, end);
    setHintText(getOutputControlHint());
}

IPlugView* PLUGIN_API FLBridgeComponent::createView(FIDString name) {
    if (name && strcmp(name, ViewType::kEditor) == 0) {
        auto* view = new FLBridgePlugView(this, &m_flInstance);
        m_activeView = view;
        return view;
    }
    return nullptr;
}

tresult PLUGIN_API FLBridgeComponent::getMidiControllerAssignment(int32 busIndex, int16 channel,
                                                                  CtrlNumber midiControllerNumber, ParamID& id) {
    if (busIndex != 0) return kResultFalse;
    switch (midiControllerNumber) {
        case kPitchBend:
            id = kParamPitchBend;
            return kResultTrue;
        case kCtrlModWheel:
            id = kParamModWheel;
            return kResultTrue;
        case kAfterTouch:
            id = kParamAftertouch;
            return kResultTrue;
        case kCtrlSustainOnOff:
            id = kParamSustain;
            return kResultTrue;
        default:
            return kResultFalse;
    }
}


} // namespace flbr

// ============================================================================
// VST3 Bundle Entry Points (Windows x86_64)
// ============================================================================

extern "C" {

SMTG_EXPORT_SYMBOL bool InitDll() {
    flbr::FLPluginScanner::getInstance().discover(true);
    return true;
}

SMTG_EXPORT_SYMBOL bool ExitDll() {
    return true;
}

SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory() {
    return new flbr::FloopHoleFactory<flbr::FLBridgeComponent>();
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL;
    (void)fdwReason;
    (void)lpvReserved;
    return TRUE;
}

#endif // _WIN32
