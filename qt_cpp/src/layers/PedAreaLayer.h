#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>
#include <memory>

struct NetworkGeometry;

// Overlays sidewalks, walking areas and pedestrian crossings on top of the
// base road colour so they stand out from roads. Drawn after NetworkLayer.
class PedAreaLayer {
public:
    PedAreaLayer();
    ~PedAreaLayer();

    void initGL(QOpenGLFunctions_3_3_Core* gl);
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);
    void draw(const float* projectionMatColMajor);

private:
    void rebuild();

public:
    struct Group {
        GLsizei vertexCount = 0;
        QOpenGLBuffer vbo {QOpenGLBuffer::VertexBuffer};
        QOpenGLVertexArrayObject vao;
        float color[4] {1, 1, 1, 1};
    };

private:

    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
    GLuint m_program = 0;
    GLint  m_locProj = -1;
    GLint  m_locColor = -1;

    Group m_sidewalk;
    Group m_walkArea;

    std::shared_ptr<NetworkGeometry> m_ng;
    bool    m_dirty = false;
};
