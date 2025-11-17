#include "MainWindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QDebug>
#include <QLabel>
#include <QSlider>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowIcon(QIcon(":/resources/favicon.png")); // Qt 资源路径
    buildUi();
    refreshDevices();
}

MainWindow::~MainWindow() {
}

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

    // 缓冲长度控件
    bufferSlider = new QSlider(Qt::Horizontal);
    bufferSlider->setRange(25, 500); // 50~500 ms
    bufferSlider->setValue(150); // 默认 150ms
    bufferLabel = new QLabel("缓冲长度: 150 ms");

    connect(bufferSlider, &QSlider::valueChanged, this, [this](int value){
        bufferLabel->setText(QString("缓冲长度: %1 ms").arg(value));
    });

    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefresh);
    connect(startBtn, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);

    auto layout = new QVBoxLayout();

    auto topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel("设备输出:"));
    topRow->addWidget(outputCombo);
    topRow->addWidget(refreshBtn);
    layout->addLayout(topRow);

    auto group = new QGroupBox("复制设备 (多选)");
    auto gLayout = new QVBoxLayout();
    gLayout->addWidget(inputList);
    group->setLayout(gLayout);
    layout->addWidget(group);

    // 缓冲长度
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
}

void MainWindow::refreshDevices() {
    outputCombo->clear();
    inputList->clear();

    auto devices = engine.listDeviceNames();
    for (const auto &d: devices.outputs)
        outputCombo->addItem(QString::fromWCharArray(d.c_str()));
    for (const auto &d: devices.inputs)
        inputList->addItem(QString::fromWCharArray(d.c_str()));
}

void MainWindow::onRefresh() {
    refreshDevices();
    statusLabel->setText("已刷新");
}

void MainWindow::onStart() {
    QList<QListWidgetItem *> items = inputList->selectedItems();
    if (items.isEmpty()) {
        QMessageBox::warning(this, "提示", "请至少选择一个输入设备");
        return;
    }
    if (outputCombo->currentIndex() < 0) {
        QMessageBox::warning(this, "提示", "请选择输出设备");
        return;
    }

    std::vector<std::wstring> inputs;
    for (auto item: items)
        inputs.push_back(item->text().toStdWString());
    std::wstring output = outputCombo->currentText().toStdWString();

    DWORD bufferMs = static_cast<DWORD>(bufferSlider->value());

    if (!engine.startCopy(inputs, output, bufferMs)) {
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
