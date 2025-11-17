#pragma once

#include <QMainWindow>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include "AudioEngine.h"

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    ~MainWindow() override;

private slots:
    void refreshDevices() const;

    void onInputSelectionChanged() const;

    void onStartClicked();

    void onStopClicked();

private:
    // 后端引擎
    AudioEngine engine;

    // UI 元件
    QComboBox *outputCombo;
    QListWidget *inputList;
    QPushButton *refreshBtn;
    QPushButton *startBtn;
    QPushButton *stopBtn;
    QLabel *statusIcon;
    QLabel *statusText;

    void setStatus(const QString &color, const QString &text) const;


    // 缓冲长度控件
    QSlider *bufferSlider;
    QLabel *bufferLabel;
};