#include <Editor/Main/MainWindow.h>
#include <Editor/Main/Version.h>

#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>

EE_NAMESPACE_EDITOR_BEGIN

MainWindow::MainWindow() {
	setWindowFlags(Qt::FramelessWindowHint);
	setAttribute(Qt::WA_TranslucentBackground);

	auto* centralWidget = new QWidget(this);
	setCentralWidget(centralWidget);

	auto* mainLayout = new QVBoxLayout(centralWidget);
	mainLayout->setContentsMargins(10, 10, 10, 10);
	mainLayout->setSpacing(0);

	auto* titleBar = new QWidget(centralWidget);
	titleBar->setFixedHeight(36);
	auto* titleLayout = new QHBoxLayout(titleBar);
	titleLayout->setContentsMargins(10, 0, 10, 0);

	auto* titleLabel = new QLabel(
		QString(
			std::format("  Ender Editor - {}.{}.{}-{{{}}}", 
				EE_EDITOR_VERSION_MAJOR, EE_EDITOR_VERSION_MINOR, 
				EE_EDITOR_VERSION_PATCH, EE_EDITOR_VERSION_GUID).c_str()
		), titleBar);
	titleLayout->addWidget(titleLabel);
	titleLayout->addStretch();

	auto* minBtn = new QPushButton("-", titleBar);
	auto* maxBtn = new QPushButton("□", titleBar);
	auto* closeBtn = new QPushButton("×", titleBar);
	titleLayout->addWidget(minBtn);
	titleLayout->addWidget(maxBtn);
	titleLayout->addWidget(closeBtn);

	connect(minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
	connect(maxBtn, &QPushButton::clicked, this, [this] {
		isMaximized() ? showNormal() : showMaximized();
		});
	connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);

	mainLayout->addWidget(titleBar);
}

EE_NAMESPACE_EDITOR_END