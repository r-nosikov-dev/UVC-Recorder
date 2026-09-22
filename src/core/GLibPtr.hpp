#pragma once

#include <gst/gst.h>

#include <memory>

namespace uvc {

struct GstUnref {
    template <typename T>
    void operator()(T* pointer) const noexcept {
        if (pointer) {
            gst_object_unref(pointer);
        }
    }
};

struct GFree {
    void operator()(gchar* pointer) const noexcept { g_free(pointer); }
};

template <typename T>
using GstPtr = std::unique_ptr<T, GstUnref>;

using GCharPtr = std::unique_ptr<gchar, GFree>;

}  // namespace uvc
