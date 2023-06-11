// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Cocoa/Cocoa.h>

#include "flutter/shell/platform/embedder/embedder.h"

@class FlutterView;

@interface FlutterCursorCoordinator : NSObject

- (nonnull FlutterCursorCoordinator*)initWithFlutterView:(nonnull FlutterView*)flutterView;

@end

/// FlutterMutatorView contains platform view and is responsible for applying
/// FlutterLayer mutations to it.
@interface FlutterMutatorView : NSView

/// Designated initializer.
- (nonnull instancetype)initWithPlatformView:(nonnull NSView*)platformView
                            cursorCoordiator:(nullable FlutterCursorCoordinator*)coordinator;

- (nonnull instancetype)initWithPlatformView:(nonnull NSView*)platformView;

/// Returns wrapped platform view.
@property(readonly, nonnull) NSView* platformView;

/// Applies mutations from FlutterLayer to the platform view. This may involve
/// creating or removing intermediate subviews depending on current state and
/// requested mutations.
- (void)applyFlutterLayer:(nonnull const FlutterLayer*)layer;

- (void)resetHitTestRegion;
- (void)addHitTestIgnoreRegion:(CGRect)region;

@end
