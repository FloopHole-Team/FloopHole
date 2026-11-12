#include <windows.h>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>
#include "FLPluginInstance.h"

static LONG WINAPI reportException(EXCEPTION_POINTERS* exception) {
    const auto* context = exception->ContextRecord;
    fprintf(stderr, "Exception 0x%08lx at %p; RCX=0x%llx RDX=0x%llx RIP=0x%llx\n",
            exception->ExceptionRecord->ExceptionCode,
            exception->ExceptionRecord->ExceptionAddress,
            (unsigned long long)context->Rcx, (unsigned long long)context->Rdx,
            (unsigned long long)context->Rip);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(reportException);
    SetEnvironmentVariableA("FLBR_TRACE_HOST", "1");
    const char* pluginName = argc > 1 ? argv[1] : "Harmor";
    const bool testPresets = argc > 2 && strcmp(argv[2], "--presets") == 0;
    if (argc > 2 && !testPresets) {
        fprintf(stderr, "Usage: FLBridge_probe.exe [plugin name] [--presets]\n");
        return 1;
    }
    fprintf(stderr, "FLBridge Windows construction probe: %s\n", pluginName);
    fflush(stderr);
    flbr::FLPluginInstance instance;
    if (!instance.loadPluginByName(pluginName)) {
        fprintf(stderr, "Plugin was not found or could not be loaded.\n");
        return 1;
    }
    fprintf(stderr, "Construction succeeded: %s (%d parameters)\n",
            instance.getPluginName().c_str(), instance.getNumParams());
    fflush(stderr);

    constexpr int blockSize = 512;
    constexpr double sampleRate = 48000.0;
    fprintf(stderr, "Configuring audio: %.0f Hz, %d frames\n", sampleRate, blockSize);
    fflush(stderr);
    instance.setSampleRate(sampleRate);
    instance.setBlockSize(blockSize);
    bool audioValid = true;
    std::thread audioThread([&] {
        std::vector<float> left(blockSize);
        std::vector<float> right(blockSize);
        auto render = [&](const char* stage, bool playing) {
            fprintf(stderr, "Rendering %s on audio thread\n", stage);
            fflush(stderr);
            float peak = 0.0f;
            for (int block = 0; block < 64; ++block) {
                instance.processAudio(nullptr, nullptr, left.data(), right.data(),
                                      blockSize, 120.0,
                                      block * blockSize / sampleRate * 2.0, playing);
                for (int frame = 0; frame < blockSize; ++frame) {
                    if (!std::isfinite(left[frame]) || !std::isfinite(right[frame])) {
                        audioValid = false;
                    }
                    peak = std::max(peak, std::max(std::abs(left[frame]), std::abs(right[frame])));
                }
            }
            fprintf(stderr, "Rendered: %s peak=%.6f\n", stage, peak);
            fflush(stderr);
            return peak;
        };
        render("silence", false);
        fprintf(stderr, "Sending MIDI note-on\n");
        fflush(stderr);
        const int note = instance.getPluginName() == "Drumaxx" ? 36 : 60;
        instance.noteOn(0, note, 0.8f);
        if (render("note-on", true) <= 0.001f) {
            fprintf(stderr, "ERROR: note-on produced no audible output.\n");
            audioValid = false;
        }
        fprintf(stderr, "Sending MIDI note-off\n");
        fflush(stderr);
        instance.noteOff(0, note, 0.5f);
        render("note-off", true);
        instance.allNotesOff();
    });
    audioThread.join();
    if (testPresets) {
        HWND parent = CreateWindowExW(0, L"STATIC", L"FLBridge preset probe",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 900,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!parent) {
            fprintf(stderr, "ERROR: could not create editor parent (Windows error %lu).\n", GetLastError());
            return 1;
        }
        int width = 0, height = 0;
        if (!instance.attachEditorToNSView(parent, width, height)) {
            fprintf(stderr, "ERROR: could not attach native editor for preset testing.\n");
            DestroyWindow(parent);
            return 1;
        }
        const auto presets = flbr::FLPluginScanner::getInstance().getPlugins()[instance.getCurrentPluginIndex()].presets;
        for (size_t i = 0; i < presets.size(); ++i) {
            fprintf(stderr, "Testing preset with editor attached %zu/%zu: %s\n",
                    i + 1, presets.size(), presets[i].displayName.c_str());
            fflush(stderr);
            std::vector<uint8_t> state;
            if (!instance.loadPreset(static_cast<int>(i)) || !instance.savePluginState(state) ||
                state.empty() || !instance.restorePluginState(state.data(), state.size())) {
                fprintf(stderr, "ERROR: preset load or state round trip failed.\n");
                audioValid = false;
                break;
            }
            instance.idleEditor();
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        instance.detachEditor();
        DestroyWindow(parent);
    }
    instance.unloadPlugin();
    fprintf(stderr, "Destruction succeeded.\n");
    return audioValid ? 0 : 1;
}