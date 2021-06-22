// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/android/android_context_gl.h"

#include <EGL/eglext.h>

#include <list>
#include <utility>

#include "flutter/fml/trace_event.h"

namespace flutter {

template <class T>
using EGLResult = std::pair<bool, T>;

static void LogLastEGLError() {
  struct EGLNameErrorPair {
    const char* name;
    EGLint code;
  };

#define _EGL_ERROR_DESC(a) \
  { #a, a }

  const EGLNameErrorPair pairs[] = {
      _EGL_ERROR_DESC(EGL_SUCCESS),
      _EGL_ERROR_DESC(EGL_NOT_INITIALIZED),
      _EGL_ERROR_DESC(EGL_BAD_ACCESS),
      _EGL_ERROR_DESC(EGL_BAD_ALLOC),
      _EGL_ERROR_DESC(EGL_BAD_ATTRIBUTE),
      _EGL_ERROR_DESC(EGL_BAD_CONTEXT),
      _EGL_ERROR_DESC(EGL_BAD_CONFIG),
      _EGL_ERROR_DESC(EGL_BAD_CURRENT_SURFACE),
      _EGL_ERROR_DESC(EGL_BAD_DISPLAY),
      _EGL_ERROR_DESC(EGL_BAD_SURFACE),
      _EGL_ERROR_DESC(EGL_BAD_MATCH),
      _EGL_ERROR_DESC(EGL_BAD_PARAMETER),
      _EGL_ERROR_DESC(EGL_BAD_NATIVE_PIXMAP),
      _EGL_ERROR_DESC(EGL_BAD_NATIVE_WINDOW),
      _EGL_ERROR_DESC(EGL_CONTEXT_LOST),
  };

#undef _EGL_ERROR_DESC

  const auto count = sizeof(pairs) / sizeof(EGLNameErrorPair);

  EGLint last_error = eglGetError();

  for (size_t i = 0; i < count; i++) {
    if (last_error == pairs[i].code) {
      FML_LOG(ERROR) << "EGL Error: " << pairs[i].name << " (" << pairs[i].code
                     << ")";
      return;
    }
  }

  FML_LOG(ERROR) << "Unknown EGL Error";
}

static EGLResult<EGLContext> CreateContext(EGLDisplay display,
                                           EGLConfig config,
                                           EGLContext share = EGL_NO_CONTEXT) {
  EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

  EGLContext context = eglCreateContext(display, config, share, attributes);

  return {context != EGL_NO_CONTEXT, context};
}

static EGLResult<EGLConfig> ChooseEGLConfiguration(EGLDisplay display) {
  EGLint attributes[] = {
      // clang-format off
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_ALPHA_SIZE,      8,
      EGL_DEPTH_SIZE,      0,
      EGL_STENCIL_SIZE,    0,
      EGL_NONE,            // termination sentinel
      // clang-format on
  };

  EGLint config_count = 0;
  EGLConfig egl_config = nullptr;

  if (eglChooseConfig(display, attributes, &egl_config, 1, &config_count) !=
      EGL_TRUE) {
    return {false, nullptr};
  }

  bool success = config_count > 0 && egl_config != nullptr;

  return {success, success ? egl_config : nullptr};
}

static bool TeardownContext(EGLDisplay display, EGLContext context) {
  if (context != EGL_NO_CONTEXT) {
    return eglDestroyContext(display, context) == EGL_TRUE;
  }

  return true;
}

class AndroidEGLSurfaceDamage {
 public:
  void init(EGLDisplay display, EGLContext context) {
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);

    has_buffer_age_ = HasExtension(extensions, "EGL_EXT_buffer_age");

    // EGL_EXT_buffer_age extension is reqired for partial repaint
    if (has_buffer_age_) {
      if (HasExtension(extensions, "EGL_KHR_partial_update")) {
        set_damage_region = reinterpret_cast<PFNEGLSETDAMAGEREGIONKHRPROC>(
            eglGetProcAddress("eglSetDamageRegionKHR"));
      }

      if (HasExtension(extensions, "EGL_EXT_swap_buffers_with_damage")) {
        swap_buffers_with_damage =
            reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC>(
                eglGetProcAddress("eglSwapBuffersWithDamageEXT"));
      } else if (HasExtension(extensions, "EGL_KHR_swap_buffers_with_damage")) {
        swap_buffers_with_damage =
            reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC>(
                eglGetProcAddress("eglSwapBuffersWithDamageKHR"));
      }
    }
  }

  void SetDamageRegion(EGLDisplay display,
                       EGLSurface surface,
                       const std::vector<SkIRect>& region) {
    if (set_damage_region) {
      auto rects = RectsToInts(display, surface, region);
      set_damage_region(display, surface, rects.data(), region.size());
    }
  }

  std::vector<SkIRect> InitialDamage(EGLDisplay display, EGLSurface surface) {
    std::vector<SkIRect> res;
    if (!has_buffer_age_) {
      return res;
    }

    EGLint age;
    eglQuerySurface(display, surface, EGL_BUFFER_AGE_EXT, &age);

    // age == 0 - full repaint (empty res)
    if (age == 1) {
      // no initial damage
      res.push_back(SkIRect::MakeEmpty());
    } else if (age > 1) {
      age -= 2;
      for (auto i = damage_history_.begin();
           i != damage_history_.end() && age >= 0; ++i, --age) {
        res.insert(res.end(), i->begin(), i->end());
      }
    }

    return res;
  }

  bool SwapBuffersWithDamage(EGLDisplay display,
                             EGLSurface surface,
                             std::vector<SkIRect> damage) {
    auto rects = RectsToInts(display, surface, damage);
    damage_history_.push_back(std::move(damage));
    if (damage_history_.size() > 2) {
      damage_history_.pop_front();
    }
    if (swap_buffers_with_damage) {
      return swap_buffers_with_damage(display, surface, rects.data(),
                                      damage.size());

    } else {
      return eglSwapBuffers(display, surface);
    }
  }

 private:
  std::vector<EGLint> RectsToInts(EGLDisplay display,
                                  EGLSurface surface,
                                  const std::vector<SkIRect>& rects) {
    std::vector<EGLint> res;
    EGLint height;
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    res.reserve(rects.size() * 4);
    for (const auto& r : rects) {
      res.push_back(r.left());
      res.push_back(height - r.bottom());
      res.push_back(r.width());
      res.push_back(r.height());
    }
    return res;
  }

  PFNEGLSETDAMAGEREGIONKHRPROC set_damage_region = nullptr;
  PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC swap_buffers_with_damage = nullptr;
  bool has_buffer_age_;

  bool HasExtension(const char* extensions, const char* name) {
    const char* r = strstr(extensions, name);
    auto len = strlen(name);
    // check that the extension name is terminated by space or null terminator
    return r != nullptr && (r[len] == ' ' || r[len] == 0);
  }

  std::list<std::vector<SkIRect>> damage_history_;
};

AndroidEGLSurface::AndroidEGLSurface(EGLSurface surface,
                                     EGLDisplay display,
                                     EGLContext context)
    : surface_(surface),
      display_(display),
      context_(context),
      damage_(std::make_unique<AndroidEGLSurfaceDamage>()) {
  damage_->init(display, context);
}

AndroidEGLSurface::~AndroidEGLSurface() {
  auto result = eglDestroySurface(display_, surface_);
  FML_DCHECK(result == EGL_TRUE);
}

bool AndroidEGLSurface::IsValid() const {
  return surface_ != EGL_NO_SURFACE;
}

bool AndroidEGLSurface::MakeCurrent() const {
  if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
    FML_LOG(ERROR) << "Could not make the context current";
    LogLastEGLError();
    return false;
  }
  return true;
}

void AndroidEGLSurface::SetDamageRegion(std::vector<SkIRect> buffer_damage) {
  damage_->SetDamageRegion(display_, surface_, buffer_damage);
}

bool AndroidEGLSurface::SwapBuffers(std::vector<SkIRect> surface_damage) {
  TRACE_EVENT0("flutter", "AndroidContextGL::SwapBuffers");
  return damage_->SwapBuffersWithDamage(display_, surface_,
                                        std::move(surface_damage));
}

std::vector<SkIRect> AndroidEGLSurface::InitialDamage() {
  return damage_->InitialDamage(display_, surface_);
}

SkISize AndroidEGLSurface::GetSize() const {
  EGLint width = 0;
  EGLint height = 0;

  if (!eglQuerySurface(display_, surface_, EGL_WIDTH, &width) ||
      !eglQuerySurface(display_, surface_, EGL_HEIGHT, &height)) {
    FML_LOG(ERROR) << "Unable to query EGL surface size";
    LogLastEGLError();
    return SkISize::Make(0, 0);
  }
  return SkISize::Make(width, height);
}

AndroidContextGL::AndroidContextGL(
    AndroidRenderingAPI rendering_api,
    fml::RefPtr<AndroidEnvironmentGL> environment)
    : AndroidContext(AndroidRenderingAPI::kOpenGLES),
      environment_(environment),
      config_(nullptr) {
  if (!environment_->IsValid()) {
    FML_LOG(ERROR) << "Could not create an Android GL environment.";
    return;
  }

  bool success = false;

  // Choose a valid configuration.
  std::tie(success, config_) = ChooseEGLConfiguration(environment_->Display());
  if (!success) {
    FML_LOG(ERROR) << "Could not choose an EGL configuration.";
    LogLastEGLError();
    return;
  }

  // Create a context for the configuration.
  std::tie(success, context_) =
      CreateContext(environment_->Display(), config_, EGL_NO_CONTEXT);
  if (!success) {
    FML_LOG(ERROR) << "Could not create an EGL context";
    LogLastEGLError();
    return;
  }

  std::tie(success, resource_context_) =
      CreateContext(environment_->Display(), config_, context_);
  if (!success) {
    FML_LOG(ERROR) << "Could not create an EGL resource context";
    LogLastEGLError();
    return;
  }

  // All done!
  valid_ = true;
}

AndroidContextGL::~AndroidContextGL() {
  if (!TeardownContext(environment_->Display(), context_)) {
    FML_LOG(ERROR)
        << "Could not tear down the EGL context. Possible resource leak.";
    LogLastEGLError();
  }

  if (!TeardownContext(environment_->Display(), resource_context_)) {
    FML_LOG(ERROR) << "Could not tear down the EGL resource context. Possible "
                      "resource leak.";
    LogLastEGLError();
  }
}

std::unique_ptr<AndroidEGLSurface> AndroidContextGL::CreateOnscreenSurface(
    fml::RefPtr<AndroidNativeWindow> window) const {
  EGLDisplay display = environment_->Display();

  const EGLint attribs[] = {EGL_NONE};

  EGLSurface surface = eglCreateWindowSurface(
      display, config_, reinterpret_cast<EGLNativeWindowType>(window->handle()),
      attribs);
  return std::make_unique<AndroidEGLSurface>(surface, display, context_);
}

std::unique_ptr<AndroidEGLSurface> AndroidContextGL::CreateOffscreenSurface()
    const {
  // We only ever create pbuffer surfaces for background resource loading
  // contexts. We never bind the pbuffer to anything.
  EGLDisplay display = environment_->Display();

  const EGLint attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};

  EGLSurface surface = eglCreatePbufferSurface(display, config_, attribs);
  return std::make_unique<AndroidEGLSurface>(surface, display,
                                             resource_context_);
}

fml::RefPtr<AndroidEnvironmentGL> AndroidContextGL::Environment() const {
  return environment_;
}

bool AndroidContextGL::IsValid() const {
  return valid_;
}

bool AndroidContextGL::ClearCurrent() const {
  if (eglGetCurrentContext() != context_) {
    return true;
  }
  if (eglMakeCurrent(environment_->Display(), EGL_NO_SURFACE, EGL_NO_SURFACE,
                     EGL_NO_CONTEXT) != EGL_TRUE) {
    FML_LOG(ERROR) << "Could not clear the current context";
    LogLastEGLError();
    return false;
  }
  return true;
}

EGLContext AndroidContextGL::CreateNewContext() const {
  bool success;
  EGLContext context;
  std::tie(success, context) =
      CreateContext(environment_->Display(), config_, EGL_NO_CONTEXT);
  return success ? context : EGL_NO_CONTEXT;
}

}  // namespace flutter
