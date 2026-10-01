#include <Editor/Main/MainWindow.h>
#include <Editor/Main/TitleBar.h>

#include <QVBoxLayout>

EE_NAMESPACE_EDITOR_BEGIN

MainWindow::MainWindow(QWidget* parent) : QWidget(parent) {
	setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
	setAttribute(Qt::WA_TranslucentBackground);

	resize(800, 600);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addWidget(new TitleBar(this));

    auto* content = new QWidget(this);
    content->setStyleSheet("background:#f5f5f5;");
    layout->addWidget(content, 1);
}

EE_NAMESPACE_EDITOR_END