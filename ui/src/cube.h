#pragma once

#include <QPainter>
#include <QRectF>

// The spinning stud cube (geometry and colours from c2d7fa's opengl-cube, CC0-1.0),
// drawn with QPainter into `area`, one turn every four seconds.
void draw_cube(QPainter& painter, const QRectF& area, double seconds);

// Call after a client download: the cube reloads Roblox's stud texture on its next frame.
void cube_client_changed();
