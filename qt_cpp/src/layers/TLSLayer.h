#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>
#include <memory>

#include "sim/SimSnapshot.h"

struct NetworkGeometry;

// Traffic-light heads. Positions are static (built from NetworkGeometry),
// per-instance colors come from the per-step TLS state characters in
// SimSnapshot::tls_states. One small square per controlled link.
class TLSLayer {
public:
    TLSLayer();
    ~TLSLayer();

    void initGL(QOpenGLFunctions_3_3_Core* gl);
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);
    void setSnapshot(const SimSnapshotPtr& snap);
    void draw(const float* projectionMatColMajor);

private:
    void uploadPositions();
    void uploadColors();

    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
    GLuint m_program = 0;
    GLint  m_locProj = -1;

    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_quadVbo {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_posVbo  {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_colVbo  {QOpenGLBuffer::VertexBuffer};

    std::shared_ptr<NetworkGeometry> m_ng;
    SimSnapshotPtr                   m_snap;
    GLsizei m_instanceCount = 0;
    bool    m_posDirty = false;
    bool    m_colDirty = false;
};
