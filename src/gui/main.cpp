#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  MainWindow window;
  window.setWindowTitle("Vinput Configuration");
  window.resize(600, 400);
  window.show();

  return app.exec();
}
