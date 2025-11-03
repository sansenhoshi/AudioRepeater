#include "AudioEngine.h"
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <atlbase.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>

using Microsoft::WRL::ComPtr;

AudioEngine::AudioEngine() {
    CoInitialize(nullptr);
}

AudioEngine::~AudioEngine() {
    stopCopy();
    if (mixFormat) CoTaskMemFree(mixFormat);
    CoUninitialize();
}

DeviceNames AudioEngine::listDeviceNames() {
    DeviceNames names;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        std::cerr << "Failed to create MMDeviceEnumerator" << std::endl;
        return names;
    }

    ComPtr<IMMDeviceCollection> collection;

    // 输出设备
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            collection->Item(i, &dev);
            ComPtr<IPropertyStore> props;
            dev->OpenPropertyStore(STGM_READ, &props);
            PROPVARIANT varName;
            PropVariantInit(&varName);
            props->GetValue(PKEY_Device_FriendlyName, &varName);
            names.outputs.push_back(std::wstring(varName.pwszVal));
            PropVariantClear(&varName);
        }
    }

    // 输入设备
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            collection->Item(i, &dev);
            ComPtr<IPropertyStore> props;
            dev->OpenPropertyStore(STGM_READ, &props);
            PROPVARIANT varName;
            PropVariantInit(&varName);
            props->GetValue(PKEY_Device_FriendlyName, &varName);
            names.inputs.push_back(std::wstring(varName.pwszVal));
            PropVariantClear(&varName);
        }
    }

    return names;
}

bool AudioEngine::startCopy(const std::vector<std::wstring>& inputNames, const std::wstring& outputName) {
    if (inputNames.empty()) return false;

    std::lock_guard<std::mutex> lock(audioMutex);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        std::cerr << "Failed to create MMDeviceEnumerator" << std::endl;
        return false;
    }

    // 输出设备
    ComPtr<IMMDevice> outDev;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) return false;

    UINT count = 0; collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        collection->Item(i, &dev);
        ComPtr<IPropertyStore> props;
        dev->OpenPropertyStore(STGM_READ, &props);
        PROPVARIANT varName; PropVariantInit(&varName);
        props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (outputName == std::wstring(varName.pwszVal)) outDev = dev;
        PropVariantClear(&varName);
    }

    if (!outDev) {
        std::cerr << "Output device not found" << std::endl;
        return false;
    }

    if (FAILED(outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &outputClient))) return false;

    // 获取默认回环输入设备 (系统播放捕获)
    ComPtr<IMMDevice> loopbackDev;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &loopbackDev))) return false;

    ComPtr<IAudioClient> inputClient;
    if (FAILED(loopbackDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &inputClient))) return false;

    // 获取输入/输出格式
    WAVEFORMATEX* inFormat = nullptr;
    if (FAILED(inputClient->GetMixFormat(&inFormat)) || !inFormat) return false;

    WAVEFORMATEX* outFormat = nullptr;
    if (FAILED(outputClient->GetMixFormat(&outFormat)) || !outFormat) {
        CoTaskMemFree(inFormat);
        return false;
    }

    // 同步采样率
    outFormat->nSamplesPerSec = inFormat->nSamplesPerSec;
    outFormat->nAvgBytesPerSec = inFormat->nAvgBytesPerSec;

    if (mixFormat) CoTaskMemFree(mixFormat);
    mixFormat = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX) + outFormat->cbSize);
    if (!mixFormat) {
        CoTaskMemFree(inFormat);
        CoTaskMemFree(outFormat);
        return false;
    }
    std::memcpy(mixFormat, outFormat, sizeof(WAVEFORMATEX) + outFormat->cbSize);

    CoTaskMemFree(inFormat);
    CoTaskMemFree(outFormat);

    // 初始化客户端
    if (FAILED(outputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFormat, nullptr))) return false;
    if (FAILED(outputClient->GetService(IID_PPV_ARGS(&renderClient)))) return false;

    if (FAILED(inputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, mixFormat, nullptr))) return false;
    if (FAILED(inputClient->GetService(IID_PPV_ARGS(&captureClient)))) return false;

    inputClients.push_back(inputClient);

    running = true;
    captureThread = std::thread(&AudioEngine::captureLoop, this);

    return true;
}

void AudioEngine::stopCopy() {
    running = false;
    if (captureThread.joinable())
        captureThread.join();

    std::lock_guard<std::mutex> lock(audioMutex);

    inputClients.clear();
    renderClient.Reset();
    outputClient.Reset();
    captureClient.Reset();

    if (mixFormat) {
        CoTaskMemFree(mixFormat);
        mixFormat = nullptr;
    }
}

void AudioEngine::captureLoop() {
    std::lock_guard<std::mutex> lock(audioMutex);
    if (inputClients.empty() || !renderClient || !captureClient || !mixFormat) {
        std::cerr << "captureLoop: missing components" << std::endl;
        return;
    }

    auto inClient = inputClients[0].Get();
    inClient->Start();
    outputClient->Start();

    while (running) {
        UINT32 packetLength = 0;
        if (FAILED(captureClient->GetNextPacketSize(&packetLength))) break;

        while (packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr)) || !data) break;

            BYTE* outBuf = nullptr;
            if (FAILED(renderClient->GetBuffer(frames, &outBuf)) || !outBuf) {
                captureClient->ReleaseBuffer(frames);
                break;
            }

            size_t bytes = frames * mixFormat->nBlockAlign;
            std::memcpy(outBuf, data, bytes);

            renderClient->ReleaseBuffer(frames, 0);
            captureClient->ReleaseBuffer(frames);

            if (FAILED(captureClient->GetNextPacketSize(&packetLength))) packetLength = 0;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    inClient->Stop();
    outputClient->Stop();
}
