// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterView.h"
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#import <QuartzCore/QuartzCore.h>

#import <functional>
#import <thread>
#include "flutter/fml/memory/ref_counted.h"

namespace flutter {

struct DamageRect {
  int x;
  int y;
  int w;
  int h;
};

extern DamageRect last_damage_rect;

extern int current_fbo;

}  // namespace flutter

@protocol SynchronizerDelegate

// Invoked on raster thread; Delegate should recreate IOSurface with given size
- (void)recreateSurfaceWithScaledSize:(CGSize)scaledSize;

// Invoked on platform thread; Delegate should flush the OpenGL context and
//  update the surface
- (void)commit;

- (void)scheduleOnRasterThread:(dispatch_block_t)block;

@end

namespace {
class ResizeSynchronizer;
}

enum {
  kFront = 0,
  kBack = 1,
};

@interface FlutterView () <SynchronizerDelegate> {
  __weak id<FlutterViewDelegate> _delegate;
  uint32_t _frameBufferId[2];
  uint32_t _backingTexture[2];
  IOSurfaceRef _ioSurface[2];
  fml::RefPtr<ResizeSynchronizer> synchronizer;
  BOOL active;
  CALayer* contentLayer;
  CALayer* damageLayer;
  CGRect prevDamageRect;
}

@end

@implementation FlutterView

namespace {

class ResizeSynchronizer : public fml::RefCountedThreadSafe<ResizeSynchronizer> {
 public:
  explicit ResizeSynchronizer(id<SynchronizerDelegate> delegate) : delegate_(delegate) {}

  // Begins resize event; Will hold the thread until Commit is called;
  // While holding the thread event loop is being processed
  void BeginResize(CGSize scaledSize, dispatch_block_t notify) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!delegate_) {
      return;
    }

    ++cookie_;

    // from now on, ignore all incoming commits until the block below gets
    // scheduled on raster thread
    accepting_ = false;

    // let pending commits finish to unblock the raster thread
    cond_run_.notify_all();

    // let the engine send resize notification
    notify();

    auto self = fml::Ref(this);

    // after this block is executed we start accepting commits
    // [delegate_ scheduleOnRasterThread:[self, this, cookie = cookie_, scaledSize]() {
    //   std::unique_lock<std::mutex> lock(mutex_);
    //   id<SynchronizerDelegate> delegate = delegate_;
    //   if (cookie_ == cookie && delegate) {
    //     accepting_ = true;
    //     [delegate recreateSurfaceWithScaledSize:scaledSize];
    //   }
    // }];
    width_ = int(scaledSize.width);
    height_ = int(scaledSize.height);

    waiting_ = true;

    cond_block_.wait(lock);

    if (pending_commit_) {
      [delegate_ commit];
      pending_commit_ = false;
      cond_run_.notify_all();
    }

    waiting_ = false;
  }

  void RequestFrameBuffer(int width, int height) {
    if (!accepting_) {
      if (width_ == width && height == height_) {
        accepting_ = true;
        [delegate_ recreateSurfaceWithScaledSize:CGSizeMake(width, height)];
      }
    }
  }

  // Must be invoked on raster thread; Will synchronously invoke
  // [delegate commit] on platform thread;
  void Commit() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!accepting_) {
      return;
    }

    if (waiting_) {  // BeginResize is in progress, interrupt it and schedule commit call
      pending_commit_ = true;
      cond_block_.notify_all();
      cond_run_.wait(lock);
    } else {
      // No resize, schedule commit on platform thread and wait until either done
      // or interrupted by incoming BeginResize
      dispatch_async(dispatch_get_main_queue(), [this, cookie = cookie_] {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cookie_ == cookie) {
          id<SynchronizerDelegate> delegate = delegate_;
          if (delegate) {
            [delegate commit];
          }
          cond_run_.notify_all();
        }
      });
      cond_run_.wait(lock);
    }
  }

 private:
  uint32_t cookie_ = 0;  // counter to detect stale callbacks
  std::mutex mutex_;
  std::condition_variable cond_run_;
  std::condition_variable cond_block_;
  bool accepting_ = true;
  int width_;
  int height_;

  bool waiting_ = false;
  bool pending_commit_ = false;

  __weak id<SynchronizerDelegate> delegate_;
};
}  // namespace

- (instancetype)initWithShareContext:(NSOpenGLContext*)shareContext
                            delegate:(id<FlutterViewDelegate>)delegate {
  return [self initWithFrame:NSZeroRect shareContext:shareContext delegate:delegate];
}

- (instancetype)initWithFrame:(NSRect)frame
                 shareContext:(NSOpenGLContext*)shareContext
                     delegate:(id<FlutterViewDelegate>)delegate {
  self = [super initWithFrame:frame];
  if (self) {
    self.openGLContext = [[NSOpenGLContext alloc] initWithFormat:shareContext.pixelFormat
                                                    shareContext:shareContext];

    [self setWantsLayer:YES];

    // covers entire view
    contentLayer = [[CALayer alloc] init];
    [self.layer addSublayer:contentLayer];

    // covers dirty region, repositioed on every update
    damageLayer = [[CALayer alloc] init];
    [self.layer addSublayer:damageLayer];

    synchronizer = fml::MakeRefCounted<ResizeSynchronizer>(self);

    [self.openGLContext makeCurrentContext];

    glGenFramebuffers(2, _frameBufferId);
    glGenTextures(2, _backingTexture);

    glBindFramebuffer(GL_FRAMEBUFFER, _frameBufferId[0]);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, _backingTexture[0]);
    glTexParameterf(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, _frameBufferId[1]);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, _backingTexture[1]);
    glTexParameterf(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);

    _delegate = delegate;
    flutter::current_fbo = _frameBufferId[kBack];
  }
  return self;
}

- (void)recreateSurfaceWithScaledSize:(CGSize)scaledSize {
  for (int i = 0; i < 2; ++i) {
    if (_ioSurface[i]) {
      CFRelease(_ioSurface[i]);
    }

    [self.openGLContext makeCurrentContext];

    unsigned pixelFormat = 'BGRA';
    unsigned bytesPerElement = 4;

    size_t bytesPerRow =
        IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, scaledSize.width * bytesPerElement);
    size_t totalBytes =
        IOSurfaceAlignProperty(kIOSurfaceAllocSize, scaledSize.height * bytesPerRow);
    NSDictionary* options = @{
      (id)kIOSurfaceWidth : @(scaledSize.width),
      (id)kIOSurfaceHeight : @(scaledSize.height),
      (id)kIOSurfacePixelFormat : @(pixelFormat),
      (id)kIOSurfaceBytesPerElement : @(bytesPerElement),
      (id)kIOSurfaceBytesPerRow : @(bytesPerRow),
      (id)kIOSurfaceAllocSize : @(totalBytes),
    };
    _ioSurface[i] = IOSurfaceCreate((CFDictionaryRef)options);

    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, _backingTexture[i]);

    CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE_ARB, GL_RGBA,
                           int(scaledSize.width), int(scaledSize.height), GL_BGRA,
                           GL_UNSIGNED_INT_8_8_8_8_REV, _ioSurface[i], 0 /* plane */);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, _frameBufferId[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE_ARB,
                           _backingTexture[i], 0);
  }
}

- (void)reshaped {
  if (active) {
    CGSize scaledSize = [self convertRectToBacking:self.bounds].size;
    synchronizer->BeginResize(scaledSize, ^{
      [_delegate viewDidReshape:self];
    });
  }
}

#pragma mark - NSView overrides

- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  [self reshaped];
}

- (void)blit:(CGRect)rect {
  [_openGLContext makeCurrentContext];
  glBindFramebuffer(GL_READ_FRAMEBUFFER, _frameBufferId[kBack]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _frameBufferId[kFront]);

  double factor = 2.0;  // TODO
  rect.origin.x *= factor;
  rect.origin.y *= factor;
  rect.size.width *= factor;
  rect.size.height *= factor;

  GLboolean prev = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);

  glBlitFramebuffer(CGRectGetMinX(rect), CGRectGetMinY(rect), CGRectGetMaxX(rect),
                    CGRectGetMaxY(rect), CGRectGetMinX(rect), CGRectGetMinY(rect),
                    CGRectGetMaxX(rect), CGRectGetMaxY(rect), GL_COLOR_BUFFER_BIT, GL_NEAREST);

  if (prev) {
    glEnable(GL_SCISSOR_TEST);
  }

  glFlush();
  [NSOpenGLContext clearCurrentContext];
}

- (void)commit {
  [self.openGLContext makeCurrentContext];
  glFlush();
  [NSOpenGLContext clearCurrentContext];

  [CATransaction begin];
  [CATransaction setDisableActions:YES];

  self.layer.frame = self.bounds;
  self.layer.sublayerTransform = CATransform3DTranslate(CATransform3DMakeScale(1, -1, 1), 0,
                                                        -self.layer.bounds.size.height, 0);
  contentLayer.frame = self.layer.bounds;

  if (flutter::last_damage_rect.w < 0 || flutter::last_damage_rect.h < 0) {
    [damageLayer setHidden:YES];
    [contentLayer setContents:(__bridge id)_ioSurface[kBack]];
    std::swap(_ioSurface[kBack], _ioSurface[kFront]);
    std::swap(_frameBufferId[kBack], _frameBufferId[kFront]);
    std::swap(_backingTexture[kBack], _backingTexture[kFront]);
    flutter::current_fbo = _frameBufferId[kBack];
  } else {
    CGRect dirty = CGRectMake(flutter::last_damage_rect.x / 2.0, flutter::last_damage_rect.y / 2.0,
                              flutter::last_damage_rect.w / 2.0, flutter::last_damage_rect.h / 2.0);

    // NSLog(@"Damage rect: %f %f %f %f", dirty.origin.x, dirty.origin.y, dirty.size.width,
    //       dirty.size.height);

    if (dirty.size.width == 0 || dirty.size.height == 0) {
      // for empty rect reuse previous one, otherwise the union below covers
      // unnecessary big area
      dirty = prevDamageRect;
    }

    // It is possible that current transaction will not actually make it until vsync
    // to prevent artifacts, always when updating frame content, always update area from
    // previous frame as well
    CGRect frame = CGRectUnion(dirty, prevDamageRect);

    double width = self.layer.bounds.size.width;
    double height = self.layer.bounds.size.height;

    frame.origin.y = height - frame.origin.y - frame.size.height;
    damageLayer.frame = frame;
    prevDamageRect = dirty;

    [self blit:frame];

    damageLayer.contentsRect = CGRectMake(frame.origin.x / width, frame.origin.y / height,
                                          frame.size.width / width, frame.size.height / height);

    [damageLayer setContents:nil];
    [damageLayer setContents:(__bridge id)_ioSurface[kFront]];
    [damageLayer setHidden:NO];
    [contentLayer setContents:(__bridge id)_ioSurface[kBack]];

#define DEBUG_DIRTY_RECT
#ifdef DEBUG_DIRTY_RECT
    if (@available(macOS 10.15, *)) {
      damageLayer.borderColor = CGColorCreateSRGB(1, 0, 1, 1);
      damageLayer.borderWidth = 2;
    } else {
      // Fallback on earlier versions
    }
#endif
  }

  [CATransaction commit];
}

- (void)scheduleOnRasterThread:(dispatch_block_t)block {
  [_delegate scheduleOnRasterTread:block];
}

- (void)present {
  synchronizer->Commit();
}

/**
 * Declares that the view uses a flipped coordinate system, consistent with Flutter conventions.
 */
- (BOOL)isFlipped {
  return YES;
}

- (BOOL)isOpaque {
  return YES;
}

- (void)dealloc {
  if (_ioSurface) {
    CFRelease(_ioSurface);
  }
}

- (int)getFrameBufferIdForWidth:(int)width height:(int)height {
  synchronizer->RequestFrameBuffer(width, height);

  // fprintf(stderr, "Returning fbo %i\n", _frameBufferId);
  return _frameBufferId[kBack];
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)viewDidChangeBackingProperties {
  fprintf(stderr, "Did change backing properties!\n");
  [super viewDidChangeBackingProperties];
  [self reshaped];
}

- (void)start {
  active = YES;
  [self reshaped];
}

@end
