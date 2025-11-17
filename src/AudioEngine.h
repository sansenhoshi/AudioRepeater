#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

struct DeviceNames {
    std::vector<std::wstring> inputs;
    std::vector<std::wstring> outputs;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // 列出可用的输入与输出设备
    DeviceNames listDeviceNames();

    // 启动转发：输入设备名列表，输出设备名
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
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient;
};