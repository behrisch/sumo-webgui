#pragma once

// Single include for QRhi + QShader that works against both:
//   - Qt 6.6+ (QRhi promoted to limited-compat public)  -> <rhi/qrhi.h>
//   - Qt 6.4 / 6.5 (QRhi only available as a private header) ->
//     <QtGui/private/qrhi_p.h>
//
// Both paths land on the same Q_GUI_EXPORT classes, so call sites only need
// to include this header.

#if __has_include(<rhi/qrhi.h>)
#  include <rhi/qrhi.h>
#  include <rhi/qrhi_platform.h>
#  include <rhi/qshader.h>
#else
#  include <QtGui/private/qrhi_p.h>
#  include <QtGui/private/qrhigles2_p.h>
#  include <QtGui/private/qshader_p.h>
#endif
