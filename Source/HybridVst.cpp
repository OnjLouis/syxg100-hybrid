#include "Vst2Abi.h"

#include <windows.h>

#include <cstring>
#include <new>

namespace {

struct WrapperState {
    HMODULE module {};
    vst2::AEffect* child {};
};

WrapperState* state(vst2::AEffect* effect)
{
    return static_cast<WrapperState*>(effect->object);
}

vst2::IntPtr dispatch(vst2::AEffect* effect, std::int32_t opcode,
                      std::int32_t index, vst2::IntPtr value, void* data,
                      float option)
{
    auto* wrapper = state(effect);
    if (wrapper == nullptr || wrapper->child == nullptr)
        return 0;

    if (opcode != vst2::close)
        return wrapper->child->dispatcher(wrapper->child, opcode, index, value,
                                          data, option);

    const auto result = wrapper->child->dispatcher(wrapper->child, opcode, index,
                                                   value, data, option);
    if (wrapper->module != nullptr)
        FreeLibrary(wrapper->module);
    delete wrapper;
    delete effect;
    return result;
}

void process(vst2::AEffect* effect, float** inputs, float** outputs,
             std::int32_t frames)
{
    auto* child = state(effect)->child;
    if (child->process != nullptr)
        child->process(child, inputs, outputs, frames);
}

void processReplacing(vst2::AEffect* effect, float** inputs, float** outputs,
                      std::int32_t frames)
{
    auto* child = state(effect)->child;
    if (child->processReplacing != nullptr)
        child->processReplacing(child, inputs, outputs, frames);
    else if (child->process != nullptr)
        child->process(child, inputs, outputs, frames);
}

void setParameter(vst2::AEffect* effect, std::int32_t index, float value)
{
    auto* child = state(effect)->child;
    child->setParameter(child, index, value);
}

float getParameter(vst2::AEffect* effect, std::int32_t index)
{
    auto* child = state(effect)->child;
    return child->getParameter(child, index);
}

} // namespace

extern "C" __declspec(dllexport) vst2::AEffect* VSTPluginMain(
    vst2::HostCallback host)
{
    wchar_t wrapperPath[MAX_PATH] {};
    HMODULE self {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&VSTPluginMain), &self)
        || GetModuleFileNameW(self, wrapperPath, MAX_PATH) == 0)
        return nullptr;

    auto* separator = wcsrchr(wrapperPath, L'\\');
    if (separator == nullptr)
        return nullptr;
    wcscpy(separator + 1, L"syxg50-engine.dll");

    const auto module = LoadLibraryW(wrapperPath);
    if (module == nullptr)
        return nullptr;
    const auto entry = reinterpret_cast<vst2::EntryPoint>(
        GetProcAddress(module, "main"));
    if (entry == nullptr) {
        FreeLibrary(module);
        return nullptr;
    }

    auto* child = entry(host);
    if (child == nullptr || child->magic != vst2::effectMagic) {
        FreeLibrary(module);
        return nullptr;
    }

    auto* wrapperState = new (std::nothrow) WrapperState { module, child };
    auto* effect = new (std::nothrow) vst2::AEffect {};
    if (wrapperState == nullptr || effect == nullptr) {
        delete wrapperState;
        delete effect;
        FreeLibrary(module);
        return nullptr;
    }

    *effect = *child;
    effect->dispatcher = dispatch;
    effect->process = process;
    effect->setParameter = setParameter;
    effect->getParameter = getParameter;
    effect->object = wrapperState;
    effect->processReplacing = processReplacing;
    return effect;
}
