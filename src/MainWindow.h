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
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    void buildUi();
    void refreshDevices();
    void onRefresh();
    void onStart();
    void onStop();

    QWidget* central;
    QComboBox* outputCombo;
    QListWidget* inputList;
    QPushButton* refreshBtn;
    QPushButton* startBtn;
    QPushButton* stopBtn;
    QLabel* statusLabel;

    // 新增缓冲长度控件
    QSlider* bufferSlider;
    QLabel* bufferLabel;

    AudioEngine engine;
};
