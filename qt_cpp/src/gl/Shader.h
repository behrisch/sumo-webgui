#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QString>

// Helper for building a small GL 3.3 vertex+fragment program. Logs to qWarning
// on compile/link errors and returns 0 on failure. The caller owns the
// returned program id and must glDeleteProgram() it.
GLuint buildProgram(QOpenGLFunctions_3_3_Core& gl,
                    const QString& vertexSource,
                    const QString& fragmentSource);
