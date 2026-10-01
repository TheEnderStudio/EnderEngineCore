#pragma once
#include <QWidget>

#include <Engine/Core/Macros.h>

EE_NAMESPACE_EDITOR_BEGIN

class TitleBar : public QWidget {
	Q_OBJECT
public:
	explicit TitleBar(QWidget* parent = nullptr);
protected:
	void mousePressEvent(QMouseEvent*) override;
	void mouseDoubleClickEvent(QMouseEvent*) override;
};

EE_NAMESPACE_EDITOR_END