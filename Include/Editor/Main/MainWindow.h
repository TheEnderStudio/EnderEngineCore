#pragma once

#include <QMainWindow>

#include <Engine/Core/Macros.h>

EE_NAMESPACE_EDITOR_BEGIN

class MainWindow : public QWidget {
	Q_OBJECT
public:
	explicit MainWindow(QWidget* parent = nullptr);
};

EE_NAMESPACE_EDITOR_END