#include "AudioEngine.h"
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <atlbase.h>
#include <iostream>
#include <thread>

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
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));

    // 输出设备
    ComPtr<IMMDeviceCollection> collection;
    enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
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

    // 输入设备
    enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
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

    return names;
}

bool AudioEngine::syncSampleRate(ComPtr<IAudioClient> inputClient, ComPtr<IAudioClient> outputClient) {
    WAVEFORMATEX* inFormat = nullptr;
    inputClient->GetMixFormat(&inFormat);
    if (!inFormat) return false;

    WAVEFORMATEX* outFormat = nullptr;
    outputClient->GetMixFormat(&outFormat);
    if (!outFormat) return false;

    if (inFormat->nSamplesPerSec != outFormat->nSamplesPerSec) {
        outFormat->nSamplesPerSec = inFormat->nSamplesPerSec;
        outFormat->nAvgBytesPerSec = inFormat->nAvgBytesPerSec;
    }

    if (mixFormat) CoTaskMemFree(mixFormat);
    mixFormat = outFormat; // 使用同步后的格式
    CoTaskMemFree(inFormat);
    return true;
}

bool AudioEngine::startCopy(const std::vector<std::wstring>& inputNames, const std::wstring& outputName) {
    if (inputNames.empty()) return false;

    std::lock_guard<std::mutex> lock(audioMutex);

    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));

    // 输出设备
    ComPtr<IMMDevice> outDev;
    ComPtr<IMMDeviceCollection> collection;
    enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    UINT count = 0; collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev; collection->Item(i, &dev);
        ComPtr<IPropertyStore> props; dev->OpenPropertyStore(STGM_READ, &props);
        PROPVARIANT varName; PropVariantInit(&varName);
        props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (outputName == std::wstring(varName.pwszVal)) outDev = dev;
        PropVariantClear(&varName);
    }
    if (!outDev) return false;

    // 激活输出客户端
    outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &outputClient);
    if (!outputClient) return false;

    // 获取默认回环输入 (系统播放捕获)
    ComPtr<IMMDevice> loopbackDev;
    enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &loopbackDev);

    ComPtr<IAudioClient> inputClient;
    loopbackDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &inputClient);
    if (!inputClient) return false;

    syncSampleRate(inputClient, outputClient);

    // 初始化客户端
    outputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFormat, nullptr);
    outputClient->GetService(IID_PPV_ARGS(&renderClient));

    inputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, mixFormat, nullptr);
    inputClient->GetService(IID_PPV_ARGS(&captureClient));
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
}

void AudioEngine::captureLoop() {
    if (inputClients.empty() || !renderClient) return;

    auto inClient = inputClients[0].Get();
    inClient->Start();
    outputClient->Start();

    while (running) {
        UINT32 packetLength = 0;
        captureClient->GetNextPacketSize(&packetLength);
        while (packetLength > 0) {
            BYTE* data;
            UINT32 frames;
            DWORD flags;
            captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);

            BYTE* outBuf = nullptr;
            renderClient->GetBuffer(frames, &outBuf);

            size_t bytes = frames * mixFormat->nBlockAlign;
            memcpy(outBuf, data, bytes);

            renderClient->ReleaseBuffer(frames, 0);
            captureClient->ReleaseBuffer(frames);

            captureClient->GetNextPacketSize(&packetLength);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    inClient->Stop();
    outputClient->Stop();
}
