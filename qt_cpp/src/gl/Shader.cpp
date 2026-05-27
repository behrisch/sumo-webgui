#include "Shader.h"

#include <QByteArray>
#include <QDebug>
#include <vector>

namespace {

GLuint compileShader(QOpenGLFunctions_3_3_Core& gl, GLenum type,
                     const QString& source, const char* tag) {
    const GLuint sh = gl.glCreateShader(type);
    const QByteArray utf8 = source.toUtf8();
    const char* src = utf8.constData();
    const GLint  len = utf8.size();
    gl.glShaderSource(sh, 1, &src, &len);
    gl.glCompileShader(sh);
    GLint ok = GL_FALSE;
    gl.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint logLen = 0;
        gl.glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(static_cast<size_t>(std::max(logLen, 1)));
        gl.glGetShaderInfoLog(sh, logLen, nullptr, log.data());
        qWarning("Shader compile failed (%s): %s", tag, log.data());
        gl.glDeleteShader(sh);
        return 0;
    }
    return sh;
}

}  // namespace

GLuint buildProgram(QOpenGLFunctions_3_3_Core& gl,
                    const QString& vertexSource,
                    const QString& fragmentSource) {
    const GLuint vs = compileShader(gl, GL_VERTEX_SHADER, vertexSource, "vertex");
    if (!vs) return 0;
    const GLuint fs = compileShader(gl, GL_FRAGMENT_SHADER, fragmentSource, "fragment");
    if (!fs) {
        gl.glDeleteShader(vs);
        return 0;
    }
    const GLuint prog = gl.glCreateProgram();
    gl.glAttachShader(prog, vs);
    gl.glAttachShader(prog, fs);
    gl.glLinkProgram(prog);
    GLint ok = GL_FALSE;
    gl.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint logLen = 0;
        gl.glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(static_cast<size_t>(std::max(logLen, 1)));
        gl.glGetProgramInfoLog(prog, logLen, nullptr, log.data());
        qWarning("Program link failed: %s", log.data());
        gl.glDeleteProgram(prog);
        gl.glDeleteShader(vs);
        gl.glDeleteShader(fs);
        return 0;
    }
    gl.glDetachShader(prog, vs);
    gl.glDetachShader(prog, fs);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    return prog;
}
