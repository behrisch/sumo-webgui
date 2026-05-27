#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVertexArrayObject>
#include <memory>
#include <vector>

struct NetworkGeometry;

// Static polygons from the loaded .poly.xml. Filled polygons get a
// triangle-fan about their centroid; unfilled polygons are drawn as line
// strips. Color is per-polygon (from libsumo::Polygon::getColor) and stored
// per-vertex in the VBO so a single draw covers all geometry of each kind.
class PolygonLayer {
public:
    PolygonLayer();
    ~PolygonLayer();

    void initGL(QOpenGLFunctions_3_3_Core* gl);
    void setGeometry(const std::shared_ptr<NetworkGeometry>& ng);
    void draw(const float* projectionMatColMajor);

private:
    void rebuildBuffers();

    QOpenGLFunctions_3_3_Core* m_gl = nullptr;

    GLuint m_program = 0;
    GLint  m_locProj = -1;

    // Fills: interleaved [x,y, r,g,b,a] (a as ubyte normalized in attr 1).
    QOpenGLVertexArrayObject m_fillVao;
    QOpenGLBuffer            m_fillVbo {QOpenGLBuffer::VertexBuffer};
    std::vector<GLint>       m_fillFirsts;
    std::vector<GLsizei>     m_fillCounts;

    QOpenGLVertexArrayObject m_lineVao;
    QOpenGLBuffer            m_lineVbo {QOpenGLBuffer::VertexBuffer};
    std::vector<GLint>       m_lineFirsts;
    std::vector<GLsizei>     m_lineCounts;

    std::shared_ptr<NetworkGeometry> m_ng;
    bool m_dirty = false;
};
