#pragma once

#include <QElapsedTimer>
#include <QOpenGLWidget>

// The spinning cube from Stud's GL smoke test (tools/spinning_cube.h), on
// the About tab. It redraws on every swap, so it runs at the display's own
// pace and stops whenever the tab is not showing.
class CubeView : public QOpenGLWidget {
public:
    explicit CubeView(QWidget* parent = nullptr);

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    QElapsedTimer clock_;
    bool ready_ = false;
};
