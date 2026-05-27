#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>

#include "sim/SimSnapshot.h"

// Persons rendered as small instanced squares (1 m). No orientation needed —
// in Phase 3 we just use color-by-type. Picking + body shapes land later.
class PersonLayer {
public:
    PersonLayer();
    ~PersonLayer();

    void initGL(QOpenGLFunctions_3_3_Core* gl);
    void setSnapshot(const SimSnapshotPtr& snap);
    void draw(const float* projectionMatColMajor);

private:
    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
    GLuint m_program = 0;
    GLint  m_locProj = -1;

    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_quadVbo {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_posVbo  {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_colVbo  {QOpenGLBuffer::VertexBuffer};

    GLsizei m_instanceCount = 0;
};
