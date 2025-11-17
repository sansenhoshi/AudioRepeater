#include "MainWindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QGroupBox>
#include <QComboBox>
#include <QPushButton>
#include <QListWidget>
#include <QMessageBox>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // 基本 UI 元件创建
    QWidget* central = new QWidget(this);
    setCentralWidget(central);
    // 设置图标
    setWindowIcon(QIcon(":/resources/favicon.png")); // Qt 资源路径

    outputCombo = new QComboBox(this);
    inputList = new QListWidget(this);
    inputList->setSelectionMode(QAbstractItemView::MultiSelection);

    refreshBtn = new QPushButton("刷新设备", this);
    startBtn = new QPushButton("开始", this);
    stopBtn = new QPushButton("停止", this);
    statusLabel = new QLabel("就绪", this);

    // 缓冲长度控件
    bufferSlider = new QSlider(Qt::Horizontal, this);
    bufferSlider->setRange(25, 500); // 25..500 ms
    bufferSlider->setValue(150);     // 默认 150 ms
    bufferLabel = new QLabel("缓冲长度: 150 ms", this);

    connect(bufferSlider, &QSlider::valueChanged, this, [this](int value){
        bufferLabel->setText(QString("缓冲长度: %1 ms").arg(value));
    });

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

    // 缓冲行
    auto bufferRow = new QHBoxLayout();
    bufferRow->addWidget(bufferLabel);
    bufferRow->addWidget(bufferSlider);
    layout->addLayout(bufferRow);

    auto ctrlRow = new QHBoxLayout();
    ctrlRow->addWidget(startBtn);
    ctrlRow->addWidget(stopBtn);
    ctrlRow->addStretch();
    ctrlRow->addWidget(statusLabel);
    layout->addLayout(ctrlRow);

    central->setLayout(layout);

    // 连接信号
    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(inputList, &QListWidget::itemSelectionChanged, this, &MainWindow::onInputSelectionChanged);
    connect(startBtn, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::onStopClicked);

    // 初始刷新
    refreshDevices();
    onInputSelectionChanged();
}

MainWindow::~MainWindow() {
    // 确保停止后端
    engine.stopCopy();
}

void MainWindow::refreshDevices() {
    outputCombo->clear();
    inputList->clear();

    auto devices = engine.listDeviceNames();

    // 把 loopback-capable 的 render 设备作为“输入来源”供多选（A/B）
    for (const auto &d : devices.loopbackSources) {
        inputList->addItem(QString::fromWCharArray(d.c_str()));
    }

    // 输出候选（先全部加入，后续会根据选中情况过滤）
    for (const auto &d : devices.outputs) {
        outputCombo->addItem(QString::fromWCharArray(d.c_str()));
    }

    // 保证初始时进行一次过滤
    onInputSelectionChanged();
}

void MainWindow::onInputSelectionChanged() {
    // 收集已选中的来源名称（QString）
    QSet<QString> selectedNames;
    for (auto *it : inputList->selectedItems()) {
        selectedNames.insert(it->text());
    }

    // 重新构建输出下拉：从 engine 列表中加入 outputs，但跳过已选的来源
    outputCombo->clear();
    auto devices = engine.listDeviceNames();
    for (const auto &d : devices.outputs) {
        QString name = QString::fromWCharArray(d.c_str());
        if (!selectedNames.contains(name)) {
            outputCombo->addItem(name);
        }
    }

    // 如果没有可用输出，禁用开始按钮
    if (outputCombo->count() == 0) {
        startBtn->setEnabled(false);
        statusLabel->setText("无可用输出（可能被选为来源）");
    } else {
        startBtn->setEnabled(true);
    }
}

void MainWindow::onStartClicked() {
    QList<QListWidgetItem *> items = inputList->selectedItems();
    if (items.isEmpty()) {
        QMessageBox::warning(this, "提示", "请至少选择一个输入设备");
        return;
    }
    if (outputCombo->currentIndex() < 0) {
        QMessageBox::warning(this, "提示", "请选择输出设备");
        return;
    }

    std::vector<std::wstring> sources;
    for (auto item: items) {
        sources.push_back(item->text().toStdWString());
    }
    std::wstring outName = outputCombo->currentText().toStdWString();

    DWORD bufferMs = static_cast<DWORD>(bufferSlider->value());

    // 禁用 start 按钮以避免重复启动
    startBtn->setEnabled(false);

    if (engine.startCopy(sources, outName, bufferMs)) {
        statusLabel->setText("运行中");
    } else {
        statusLabel->setText("启动失败");
        startBtn->setEnabled(true);
    }
}

void MainWindow::onStopClicked() {
    engine.stopCopy();
    statusLabel->setText("已停止");
    startBtn->setEnabled(true);
}