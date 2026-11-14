#pragma once

#include <QElapsedTimer>
#include <QOpenGLWidget>

#include <functional>

// The spinning cube from Stud's GL smoke test (tools/spinning_cube.h), on
// the About tab. It redraws on every swap, so it runs at the display's own
// pace and stops whenever the tab is not showing.
class CubeView : public QOpenGLWidget {
public:
    explicit CubeView(QWidget* parent = nullptr);
    ~CubeView() override;

    // Called on every tenth click.
    std::function<void()> on_tenth_click;

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QElapsedTimer clock_;
    struct CubeGlState* state_ = nullptr;
    int clicks_ = 0;
};
