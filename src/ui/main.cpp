#include "ui/MainWindow.h"
#include <QApplication>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("sportreview");
    app.setOrganizationName("sr");
    sr::MainWindow w;
    w.show();
    return app.exec();
}
