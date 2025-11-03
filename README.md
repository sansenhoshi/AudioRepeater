# AudioRepeater — Qt + WASAPI C++ 可编译工程模板

完整模板包含：

```
AudioRepeater/
├── CMakeLists.txt
├── README.md
└── src/
    ├── main.cpp
    ├── MainWindow.h
    ├── MainWindow.cpp
    ├── AudioEngine.h
    └── AudioEngine.cpp
```

> 说明：本模板使用 Qt Widgets 做 GUI（Qt6），使用原生 WASAPI 做音频捕获（loopback）与渲染。工程采用 CMake 构建，适用于 Visual Studio / MSVC 工具链（Windows 10/11）。

---

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(AudioRepeater LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 COMPONENTS Widgets REQUIRED)

add_executable(AudioRepeater
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/AudioEngine.cpp
    src/AudioEngine.h
)

target_link_libraries(AudioRepeater PRIVATE Qt6::Widgets)

target_compile_definitions(AudioRepeater PRIVATE UNICODE _UNICODE)

# Link required Windows libraries
target_link_libraries(AudioRepeater PRIVATE ole32 uuid avrt)

# Optional: set output directory
set_target_properties(AudioRepeater PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
```

---

## src/main.cpp

```cpp
#include <QApplication>
#include "MainWindow.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    return a.exec();
}
```

---

## src/MainWindow.h

```cpp
#pragma once

#include <QMainWindow>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include "AudioEngine.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    void onRefresh();
    void onStart();
    void onStop();

private:
    void buildUi();
    void refreshDevices();

    // UI
    QWidget* central;
    QComboBox* outputCombo;
    QListWidget* inputList;
    QPushButton* refreshBtn;
    QPushButton* startBtn;
    QPushButton* stopBtn;
    QLabel* statusLabel;

    AudioEngine engine;
};
```

---

## src/MainWindow.cpp

```cpp
#include "MainWindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMessageBox>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    refreshDevices();
}

MainWindow::~MainWindow() {}

void MainWindow::buildUi() {
    central = new QWidget(this);
    setCentralWidget(central);

    outputCombo = new QComboBox();
    inputList = new QListWidget();
    inputList->setSelectionMode(QAbstractItemView::MultiSelection);

    refreshBtn = new QPushButton("刷新设备");
    startBtn = new QPushButton("开始");
    stopBtn = new QPushButton("停止");
    statusLabel = new QLabel("就绪");

    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefresh);
    connect(startBtn, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);

    auto layout = new QVBoxLayout();

    auto topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel("输出设备:"));
    topRow->addWidget(outputCombo);
    topRow->addWidget(refreshBtn);
    layout->addLayout(topRow);

    auto group = new QGroupBox("输入设备 (多选)");
    auto gLayout = new QVBoxLayout();
    gLayout->addWidget(inputList);
    group->setLayout(gLayout);
    layout->addWidget(group);

    auto ctrlRow = new QHBoxLayout();
    ctrlRow->addWidget(startBtn);
    ctrlRow->addWidget(stopBtn);
    ctrlRow->addStretch();
    ctrlRow->addWidget(statusLabel);
    layout->addLayout(ctrlRow);

    central->setLayout(layout);
}

void MainWindow::refreshDevices() {
    outputCombo->clear();
    inputList->clear();

    auto devices = engine.listDeviceNames();
    for (const auto &d : devices.outputs) outputCombo->addItem(QString::fromWCharArray(d.c_str()));
    for (const auto &d : devices.inputs) inputList->addItem(QString::fromWCharArray(d.c_str()));
}

void MainWindow::onRefresh() {
    refreshDevices();
    statusLabel->setText("已刷新");
}

void MainWindow::onStart() {
    QList<QListWidgetItem*> items = inputList->selectedItems();
    if (items.isEmpty()) {
        QMessageBox::warning(this, "提示", "请至少选择一个输入设备");
        return;
    }
    if (outputCombo->currentIndex() < 0) {
        QMessageBox::warning(this, "提示", "请选择输出设备");
        return;
    }

    std::vector<std::wstring> inputs;
    for (auto item : items) inputs.push_back(item->text().toStdWString());
    std::wstring output = outputCombo->currentText().toStdWString();

    if (!engine.startCopy(inputs, output)) {
        QMessageBox::critical(this, "错误", "启动失败，请查看日志");
        statusLabel->setText("启动失败");
    } else {
        statusLabel->setText("运行中");
    }
}

void MainWindow::onStop() {
    engine.stopCopy();
    statusLabel->setText("已停止");
}
```

---

## src/AudioEngine.h

```cpp
#pragma once

#include <string>
#include <vector>
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

    DeviceNames listDeviceNames();

    // 启动转发：输入设备名列表，输出设备名
    bool startCopy(const std::vector<std::wstring>& inputNames, const std::wstring& outputName);
    void stopCopy();

private:
    void captureLoop();

    bool running = false;
    std::vector<Microsoft::WRL::ComPtr<IAudioClient>> inputClients;
    Microsoft::WRL::ComPtr<IAudioClient> outputClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient;
    WAVEFORMATEX* mixFormat = nullptr;
};
```

---

## src/AudioEngine.cpp

```cpp
#include "AudioEngine.h"
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <atlbase.h>
#include <thread>
#include <iostream>

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

    ComPtr<IMMDeviceCollection> collection;
    enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        collection->Item(i, &dev);
        PWSTR id = nullptr;
        dev->GetId(&id);
        ComPtr<IPropertyStore> props;
        dev->OpenPropertyStore(STGM_READ, &props);
        PROPVARIANT varName;
        PropVariantInit(&varName);
        props->GetValue(PKEY_Device_FriendlyName, &varName);
        names.outputs.push_back(std::wstring(varName.pwszVal));
        PropVariantClear(&varName);
        CoTaskMemFree(id);
    }

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

bool AudioEngine::startCopy(const std::vector<std::wstring>& inputNames, const std::wstring& outputName) {
    if (inputNames.empty()) return false;

    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));

    // 选择输出设备
    ComPtr<IMMDevice> outDev;
    {
        ComPtr<IMMDeviceCollection> collection;
        enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        UINT count = 0; collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev; collection->Item(i, &dev);
            ComPtr<IPropertyStore> props; dev->OpenPropertyStore(STGM_READ, &props);
            PROPVARIANT varName; PropVariantInit(&varName);
            props->GetValue(PKEY_Device_FriendlyName, &varName);
            if (outputName == std::wstring(varName.pwszVal)) { outDev = dev; }
            PropVariantClear(&varName);
        }
    }
    if (!outDev) return false;

    // 激活输出客户端
    outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &outputClient);
    outputClient->GetMixFormat(&mixFormat);
    outputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFormat, nullptr);
    outputClient->GetService(IID_PPV_ARGS(&renderClient));

    // 激活一个输入设备的 loopback (使用默认 render loopback)
    // 这里只示例使用默认 render 的 loopback（即监听系统播放），复杂场景可为每个输入单独激活
    ComPtr<IMMDevice> loopbackDev;
    enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &loopbackDev);
    ComPtr<IAudioClient> inputClient;
    loopbackDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &inputClient);
    inputClient->GetMixFormat(&mixFormat);
    inputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, mixFormat, nullptr);
    inputClient->GetService(IID_PPV_ARGS(&captureClient));

    inputClients.push_back(inputClient);

    running = true;
    std::thread(&AudioEngine::captureLoop, this).detach();
    return true;
}

void AudioEngine::stopCopy() {
    running = false;
    inputClients.clear();
    renderClient.Reset();
    outputClient.Reset();
}

void AudioEngine::captureLoop() {
    if (!inputClients.empty() && renderClient) {
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
                // 假设混音格式为 32-bit float 或者正确的帧字节数
                SIZE_T bytes = frames * mixFormat->nBlockAlign;
                memcpy(outBuf, data, bytes);
                renderClient->ReleaseBuffer(frames, 0);

                captureClient->ReleaseBuffer(frames);
                captureClient->GetNextPacketSize(&packetLength);
            }
            Sleep(1);
        }

        inClient->Stop();
        outputClient->Stop();
    }
}
```

---

## README.md

````md
# AudioRepeater (Qt + WASAPI)

## 依赖
- Windows 10/11
- Qt6 (Widgets)
- Visual Studio / MSVC toolchain
- CMake >= 3.16

## 构建

```bash
mkdir build && cd build
cmake -G "Visual Studio 17 2022" ..
cmake --build . --config Release
````

## 运行

可执行位于 `build/bin/`，直接运行 `AudioRepeater.exe`。

## 说明

* 本模板演示基础设备列表、选择输出、启动 loopback 捕获并写回输出。
* 仍需为多输入/多输出、重采样、稳态同步做更多工程化处理。

```

---

如果你需要，我可以：
- 将此工程打包为 ZIP 下载；
- 或把 AudioEngine 的 loopback 扩展为“多输入同时捕获并混音到单输出 / 多输出”的版本；
- 或者改为使用 `IMMDeviceEnumerator` 为每个选择的输入设备激活独立 loopback（更复杂）。

告诉我你要哪种后续扩展，我会继续完善。

```
