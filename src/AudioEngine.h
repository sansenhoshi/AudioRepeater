#pragma once

#include <string>
#include <vector>
#include <wrl/client.h>
#include <audioclient.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <windows.h>

struct DeviceNames {
    std::vector<std::wstring> inputs;   // 物理 capture 设备（麦克风等）
    std::vector<std::wstring> outputs;  // render 设备（播放目标）
    std::vector<std::wstring> loopbackSources; // 可作为 loopback 捕获的 render 设备（用于 UI 多选来源）
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // 列出可用的输入与输出设备
    DeviceNames listDeviceNames();

    // 启动转发：输入设备名列表（loopback 源名列表），输出设备名
    bool startCopy(const std::vector<std::wstring>& inputDevices,
               const std::wstring& outputDevice,
               DWORD bufferMs = 150);
    void stopCopy();

private:
    void captureLoop();
    bool syncSampleRate(Microsoft::WRL::ComPtr<IAudioClient> inputClient,
                        Microsoft::WRL::ComPtr<IAudioClient> outputClient);

    HANDLE captureEvent = nullptr;
    HANDLE renderEvent = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;

    std::atomic<bool> running{ false };
    std::mutex audioMutex;

    std::thread captureThread;

    std::vector<Microsoft::WRL::ComPtr<IAudioClient>> inputClients;
    Microsoft::WRL::ComPtr<IAudioClient> outputClient;

    // 以下成员与现有实现匹配（如需扩展多输入，请在此处增加 captureClients、device ids 等）
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient;
};