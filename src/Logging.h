#pragma once

// Minimal no-op logging shim so call sites can stay unchanged without pulling in
// any external logging dependency.
namespace Logger
{
    template <class... A> inline void info(A&&...) {}
    template <class... A> inline void warn(A&&...) {}
    template <class... A> inline void error(A&&...) {}
    template <class... A> inline void trace(A&&...) {}
}
