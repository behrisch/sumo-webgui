#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>

#include "sim/SimSnapshot.h"

// Instanced rendering of vehicles. Each vehicle is one quad (5m long x 2m
// wide by default), oriented by cos/sin of the math-frame heading. Color
// comes from the per-instance rgba8 buffer in SimSnapshot.
class VehicleLayer {
public:
    VehicleLayer();
    ~VehicleLayer();

    VehicleLayer(const VehicleLayer&)            = delete;
    VehicleLayer& operator=(const VehicleLayer&) = delete;

    void initGL(QOpenGLFunctions_3_3_Core* gl);

    // Update per-instance buffers from a snapshot. Snapshot may be null.
    void setSnapshot(const SimSnapshotPtr& snap);

    void draw(const float* projectionMatColMajor);

private:
    QOpenGLFunctions_3_3_Core* m_gl = nullptr;

    GLuint m_program = 0;
    GLint  m_locProj = -1;

    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_quadVbo {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_posVbo  {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_rotVbo  {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_colVbo  {QOpenGLBuffer::VertexBuffer};

    GLsizei m_instanceCount = 0;
};
