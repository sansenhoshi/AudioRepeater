#include "AudioEngine.h"

#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <atlbase.h>
#include <avrt.h>            // AvSetMmThreadCharacteristics
#include <iostream>
#include <thread>
#include <algorithm>

#pragma comment(lib, "Avrt.lib")

using Microsoft::WRL::ComPtr;

AudioEngine::AudioEngine() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    mixFormat = nullptr;
    running = false;
}

AudioEngine::~AudioEngine() {
    stopCopy();
    if (mixFormat) {
        CoTaskMemFree(mixFormat);
        mixFormat = nullptr;
    }
    CoUninitialize();
}

DeviceNames AudioEngine::listDeviceNames() {
    DeviceNames names;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return names;

    // 输出设备（render） —— 既是播放目标，也可以作为 loopback 源
    ComPtr<IMMDeviceCollection> collection;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            collection->Item(i, &dev);
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                    names.outputs.push_back(std::wstring(varName.pwszVal));
                    // 同时把 render 设备也作为 loopback 源暴露（UI 层用来选择要捕获的播放设备）
                    names.loopbackSources.push_back(std::wstring(varName.pwszVal));
                }
                PropVariantClear(&varName);
            }
        }
    }

    // 物理输入设备（capture）
    collection.Reset();
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            collection->Item(i, &dev);
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                    names.inputs.push_back(std::wstring(varName.pwszVal));
                }
                PropVariantClear(&varName);
            }
        }
    }

    return names;
}

bool AudioEngine::startCopy(const std::vector<std::wstring>& inputNames, const std::wstring& outputName,const DWORD bufferMs) {
    if (inputNames.empty()) return false;

    std::lock_guard<std::mutex> lock(audioMutex);

    // 初始化枚举器
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return false;

    // 找到目标输出设备
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);

    if (FAILED(hr)) return false;
    UINT count = 0; collection->GetCount(&count);

    ComPtr<IMMDevice> outDev;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        collection->Item(i, &dev);
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT varName; PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (outputName == std::wstring(varName.pwszVal)) outDev = dev;
            }
            PropVariantClear(&varName);
        }
    }
    if (!outDev) return false;

    // 激活输出客户端
    hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &outputClient);
    if (FAILED(hr)) return false;

    // 获取 output 的 mix format（后续作为 render 的格式）
    hr = outputClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) return false;

    // 为演示简化：选择第一个 inputNames 中的源作为 loopback 捕获（当前代码仅支持单个 loopback 源的实际捕获）
    // UI 已经允许多选来源并屏蔽输出候选；如果需要多源捕获与混音，这里需扩展为对每个 inputNames 激活独立 loopback client 并做混音逻辑
    std::wstring firstInputName = inputNames[0];

    // 在 render 列表中查找与 firstInputName 对应的设备，然后在该设备上 Activate IAudioClient 并 Initialize 为 loopback
    collection.Reset();
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return false;
    UINT renderCount = 0; collection->GetCount(&renderCount);
    ComPtr<IMMDevice> loopbackDev;
    for (UINT i = 0; i < renderCount; ++i) {
        ComPtr<IMMDevice> dev;
        collection->Item(i, &dev);
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT varName; PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (firstInputName == std::wstring(varName.pwszVal)) {
                    loopbackDev = dev;
                }
            }
            PropVariantClear(&varName);
        }
    }
    if (!loopbackDev) return false;

    ComPtr<IAudioClient> inputClient;
    hr = loopbackDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &inputClient);
    if (FAILED(hr)) return false;

    // 同步采样率/格式（若无法同步则继续，但 mixFormat 已取自 output）
    syncSampleRate(inputClient, outputClient);

    // 获取 buffer size（使用一个合理的 buffer duration，比如 bufferMs）
    REFERENCE_TIME hnsBufferDuration = 10000 * bufferMs; // ms -> 100-ns units; 可调整
    UINT32 bufferFrameCount = 0;

    // 初始化输出 client （共享模式 + event callback）
    hr = outputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsBufferDuration, 0, mixFormat, nullptr);
    if (FAILED(hr)) return false;
    hr = outputClient->GetService(IID_PPV_ARGS(&renderClient));
    if (FAILED(hr)) return false;
    hr = outputClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) return false;

    // 初始化输入 client （loopback + event callback）
    hr = inputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsBufferDuration, 0, mixFormat, nullptr);
    if (FAILED(hr)) return false;
    hr = inputClient->GetService(IID_PPV_ARGS(&captureClient));
    if (FAILED(hr)) return false;

    // 为输入注册事件（当捕获可读时被触发）
    if (captureEvent) { CloseHandle(captureEvent); captureEvent = nullptr; }
    captureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent) return false;
    hr = inputClient->SetEventHandle(captureEvent);
    if (FAILED(hr)) return false;

    // 为输出也注册事件（可选）
    if (renderEvent) { CloseHandle(renderEvent); renderEvent = nullptr; }
    renderEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!renderEvent) return false;
    hr = outputClient->SetEventHandle(renderEvent);
    if (FAILED(hr)) {
        // 若不支持则继续（不是致命）
    }

    // 保存 client 引用并开始线程
    inputClients.clear();
    inputClients.push_back(inputClient);
    this->outputClient = outputClient;
    running = true;

    // 启动工作线程
    captureThread = std::thread(&AudioEngine::captureLoop, this);

    return true;
}

void AudioEngine::stopCopy() {
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        running = false;
    }

    if (captureThread.joinable()) {
        if (captureEvent) SetEvent(captureEvent);
        captureThread.join();
    }

    std::lock_guard<std::mutex> lock(audioMutex);
    // 停止并释放
    if (inputClients.size() > 0) {
        for (auto &c : inputClients) {
            if (c) {
                c->Stop();
            }
        }
        inputClients.clear();
    }
    if (outputClient) {
        outputClient->Stop();
        renderClient.Reset();
        outputClient.Reset();
    }
    captureClient.Reset();

    if (captureEvent) { CloseHandle(captureEvent); captureEvent = nullptr; }
    if (renderEvent) { CloseHandle(renderEvent); renderEvent = nullptr; }
}

void AudioEngine::captureLoop() {
    // 快速检查
    if (inputClients.empty() || !renderClient || !captureClient || !outputClient) return;

    // 提升线程优先级（MMCSS）
    DWORD mmcssTaskIndex = 0;
    HANDLE mmHandle = nullptr;
    mmHandle = AvSetMmThreadCharacteristicsA("Pro Audio", &mmcssTaskIndex);
    if (!mmHandle) {
        // 备用策略：设置线程为实时优先
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    auto inClient = inputClients[0].Get();
    HRESULT hr = inClient->Start();
    if (FAILED(hr)) {
        std::cerr << "Failed to start input client: " << std::hex << hr << std::endl;
    }
    hr = outputClient->Start();
    if (FAILED(hr)) {
        std::cerr << "Failed to start output client: " << std::hex << hr << std::endl;
    }

    // 获取 buffer size
    UINT32 renderBufferFrames = 0;
    outputClient->GetBufferSize(&renderBufferFrames);

    // 主循环：等待 captureEvent（事件驱动）
    while (true) {
        {
            std::lock_guard<std::mutex> lock(audioMutex);
            if (!running) break;
        }

        DWORD waitResult = WaitForSingleObject(captureEvent, 2000); // 超时 2s 保守值
        if (waitResult == WAIT_TIMEOUT) {
            // 长时间未被唤醒，检查 running 并继续
            continue;
        } else if (waitResult != WAIT_OBJECT_0) {
            // 错误，退出
            break;
        }

        // 处理所有可用包
        UINT32 packetLength = 0;
        hr = captureClient->GetNextPacketSize(&packetLength);
        while (SUCCEEDED(hr) && packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                std::cerr << "GetBuffer failed: " << std::hex << hr << std::endl;
                break;
            }

            // 计算字节
            size_t bytesPerFrame = mixFormat->nBlockAlign;
            size_t bytesToCopy = static_cast<size_t>(framesAvailable) * bytesPerFrame;

            // 获取 render 的当前填充来决定能写多少帧
            UINT32 padding = 0;
            outputClient->GetCurrentPadding(&padding);
            UINT32 framesAvailableForWrite = 0;
            if (renderBufferFrames > padding) framesAvailableForWrite = renderBufferFrames - padding;
            else framesAvailableForWrite = 0;

            UINT32 framesToWrite = std::min<UINT32>(framesAvailable, framesAvailableForWrite);

            // 如果无法写入任何帧（render 缓冲已满），则选择丢弃或稍等
            if (framesToWrite == 0) {
                // 释放 capture buffer（避免死锁），但仍要读取并丢弃数据（防止下次事件无限增长）
                captureClient->ReleaseBuffer(framesAvailable);
                // 继续处理下一个包或等待
                break;
            }

            // 获取输出缓冲
            BYTE* outBuf = nullptr;
            hr = renderClient->GetBuffer(framesToWrite, &outBuf);
            if (FAILED(hr)) {
                // 无法获得渲染缓冲，释放输入并中断
                captureClient->ReleaseBuffer(framesAvailable);
                break;
            }

            // 确保不会越界：仅复制 framesToWrite * bytesPerFrame
            size_t bytesToActuallyCopy = static_cast<size_t>(framesToWrite) * bytesPerFrame;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // 如果输入是 silent，写零
                memset(outBuf, 0, bytesToActuallyCopy);
            } else {
                // 直接 memcpy（假设格式兼容）
                memcpy(outBuf, data, bytesToActuallyCopy);
            }

            // 提交渲染缓冲
            hr = renderClient->ReleaseBuffer(framesToWrite, 0);
            if (FAILED(hr)) {
                std::cerr << "ReleaseBuffer (render) failed: " << std::hex << hr << std::endl;
            }

            // 释放输入缓冲：注意参数必须是 framesConsumed 对应 input 的帧数
            // 如果 framesToWrite < framesAvailable，我们仍然从 capture 释放全部 framesAvailable（因为我们已经消费了这些帧概念上）
            // 为简单起见，先释放 framesAvailable（防止缓冲被卡住）。若需更精确的流对齐，可使用环形缓存存储未消费部分。
            captureClient->ReleaseBuffer(framesAvailable);

            // 继续检查是否还有包
            hr = captureClient->GetNextPacketSize(&packetLength);
        }
        // loop 回到等待
    }

    // 清理
    inClient->Stop();
    outputClient->Stop();

    if (mmHandle) AvRevertMmThreadCharacteristics(mmHandle);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
}

auto AudioEngine::syncSampleRate(ComPtr<IAudioClient> inputClient, ComPtr<IAudioClient> outputClient) -> bool {
    if (!inputClient || !outputClient) return false;

    WAVEFORMATEX* inFormat = nullptr;
    WAVEFORMATEX* outFormat = nullptr;
    HRESULT hr = inputClient->GetMixFormat(&inFormat);
    if (FAILED(hr) || !inFormat) return false;
    hr = outputClient->GetMixFormat(&outFormat);
    if (FAILED(hr) || !outFormat) { CoTaskMemFree(inFormat); return false; }

    // 如果不等则以 input 为准（只同步采样率和 bytes/sec）
    if (inFormat->nSamplesPerSec != outFormat->nSamplesPerSec ||
        inFormat->wBitsPerSample != outFormat->wBitsPerSample ||
        inFormat->nChannels != outFormat->nChannels) {
        // 简单策略：用 input 的关键字段覆盖 output 的对应字段（注意：若格式完全不兼容，后面应该做重采样）
        outFormat->nSamplesPerSec = inFormat->nSamplesPerSec;
        outFormat->nAvgBytesPerSec = inFormat->nAvgBytesPerSec;
        outFormat->nBlockAlign = inFormat->nBlockAlign;
        outFormat->wBitsPerSample = inFormat->wBitsPerSample;
        outFormat->nChannels = inFormat->nChannels;
    }

    if (mixFormat) CoTaskMemFree(mixFormat);
    mixFormat = outFormat; // 使用同步后的格式（注意：outFormat 从 GetMixFormat 分配，后面由 this 释放）
    CoTaskMemFree(inFormat);
    return true;
}
