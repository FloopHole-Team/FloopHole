#pragma once

#include "NativePluginCatalog.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include <atomic>

namespace flbr {

template<class Component>
class FloopHoleFactory final : public Steinberg::IPluginFactory3 {
public:
    FloopHoleFactory() : m_classes(discoverNativePluginClasses()) {}

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override {
        if (!object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        using namespace Steinberg;
        if (FUnknownPrivate::iidEqual(iid, IPluginFactory3::iid) ||
            FUnknownPrivate::iidEqual(iid, IPluginFactory2::iid) ||
            FUnknownPrivate::iidEqual(iid, IPluginFactory::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *object = static_cast<IPluginFactory3*>(this);
            addRef();
            return kResultOk;
        }
        return kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_references; }
    Steinberg::uint32 PLUGIN_API release() override {
        const auto remaining = --m_references;
        if (!remaining) delete this;
        return remaining;
    }
    Steinberg::tresult PLUGIN_API getFactoryInfo(Steinberg::PFactoryInfo* info) override {
        if (!info) return Steinberg::kInvalidArgument;
        *info = Steinberg::PFactoryInfo("FloopHole", "https://github.com/milestorm/FL-bridge-native",
            "", Steinberg::PFactoryInfo::kUnicode);
        return Steinberg::kResultOk;
    }
    Steinberg::int32 PLUGIN_API countClasses() override { return static_cast<Steinberg::int32>(m_classes.size()); }

    Steinberg::tresult PLUGIN_API getClassInfo(Steinberg::int32 index, Steinberg::PClassInfo* info) override {
        if (!info || index < 0 || static_cast<size_t>(index) >= m_classes.size()) return Steinberg::kInvalidArgument;
        const auto& entry = m_classes[index];
        Steinberg::FUID id;
        if (!id.fromString(entry.cid.c_str())) return Steinberg::kInternalError;
        *info = Steinberg::PClassInfo(id.toTUID(), Steinberg::PClassInfo::kManyInstances,
            kVstAudioEffectClass, entry.name.c_str());
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API getClassInfo2(Steinberg::int32 index, Steinberg::PClassInfo2* info) override {
        if (!info || index < 0 || static_cast<size_t>(index) >= m_classes.size()) return Steinberg::kInvalidArgument;
        const auto& entry = m_classes[index];
        Steinberg::FUID id;
        if (!id.fromString(entry.cid.c_str())) return Steinberg::kInternalError;
        *info = Steinberg::PClassInfo2(id.toTUID(), Steinberg::PClassInfo::kManyInstances,
            kVstAudioEffectClass, entry.name.c_str(), Steinberg::Vst::kSimpleModeSupported,
            entry.isGenerator ? Steinberg::Vst::PlugType::kInstrumentSynth : Steinberg::Vst::PlugType::kFx,
            "FloopHole", "1.0.0", kVstVersionString);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API getClassInfoUnicode(Steinberg::int32 index, Steinberg::PClassInfoW* info) override {
        if (!info) return Steinberg::kInvalidArgument;
        Steinberg::PClassInfo2 narrow;
        const auto result = getClassInfo2(index, &narrow);
        if (result == Steinberg::kResultOk) info->fromAscii(narrow);
        return result;
    }
    Steinberg::tresult PLUGIN_API setHostContext(Steinberg::FUnknown*) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::FIDString cid, Steinberg::FIDString iid, void** object) override {
        if (!cid || !iid || !object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        for (const auto& entry : m_classes) {
            Steinberg::FUID id;
            if (!id.fromString(entry.cid.c_str())) return Steinberg::kInternalError;
            if (!Steinberg::FUnknownPrivate::iidEqual(cid, id.toTUID())) continue;
            auto* component = new Component(entry);
            const auto result = component->queryInterface(iid, object);
            component->release();
            return result;
        }
        return Steinberg::kNoInterface;
    }

private:
    std::atomic<Steinberg::uint32> m_references{1};
    const std::vector<NativePluginClass> m_classes;
};

}
