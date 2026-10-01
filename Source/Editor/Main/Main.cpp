#include <Editor/Main/MainWindow.h>

#include <QApplication>

int main(int argc, char** argv) {
	QApplication app(argc, argv);

	EnderEngine::Editor::MainWindow window;
	window.show();

	return app.exec();
}