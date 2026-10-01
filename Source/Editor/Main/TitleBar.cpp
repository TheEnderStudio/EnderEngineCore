#include <Editor/Main/TitleBar.h>
#include <Editor/Main/Version.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QMouseEvent>
#include <QWindow>

EE_NAMESPACE_EDITOR_BEGIN

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
	setFixedHeight(36);

	setStyleSheet(
		R"(
QWidget { background:#2b2b2b; color:white; }
QToolButton {
	background:transparent; color:white; border:none;
	width:32px; height:28px;
}
QToolButton:hover { background:#444; })"
	);

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	auto* titleLabel = new QLabel(QString(std::format("  Ender Editor - {}.{}.{}-{{{}}}", EE_EDITOR_VERSION_MAJOR, EE_EDITOR_VERSION_MINOR, EE_EDITOR_VERSION_PATCH, EE_EDITOR_VERSION_GUID).c_str()), this);
	layout->addWidget(titleLabel);
	layout->addStretch();

	auto* minBtn = new QToolButton(this);
	minBtn->setText("—");

	auto* maxBtn = new QToolButton(this);
	maxBtn->setText("□");

	auto* closeBtn = new QToolButton(this);
	closeBtn->setText("×");
	closeBtn->setStyleSheet(
		"QToolButton:hover { background:#e81123; color:white; }"
	);

	layout->addWidget(minBtn);
	layout->addWidget(maxBtn);
	layout->addWidget(closeBtn);

	connect(minBtn, &QToolButton::clicked, this, [this] {
		window()->showMinimized();
		});

	connect(maxBtn, &QToolButton::clicked, this, [this, maxBtn] {
		if (window()->isMaximized())
			window()->showNormal();
		else
			window()->showMaximized();

		maxBtn->setText(window()->isMaximized() ? "❐" : "□");
		});

	connect(closeBtn, &QToolButton::clicked, this, [this] {
		window()->close();
		});
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