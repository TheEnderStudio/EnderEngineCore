#include <Editor/Main/TitleBar.h>
#include <Editor/Main/Version.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QMouseEvent>
#include <QWindow>

EE_NAMESPACE_EDITOR_BEGIN

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
}

void TitleBar::mousePressEvent(QMouseEvent* e) {
	if (e->button() == Qt::LeftButton) {
		if (window()->windowHandle()) {
			window()->windowHandle()->startSystemMove();
			e->accept();
			return;
		}
	}
	QWidget::mousePressEvent(e);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* e) {
	if (e->button() == Qt::LeftButton) {
		if (window()->isMaximized())
			window()->showNormal();
		else
			window()->showMaximized();
	}
	QWidget::mouseDoubleClickEvent(e);
}

EE_NAMESPACE_EDITOR_END