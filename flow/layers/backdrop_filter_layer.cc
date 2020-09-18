// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/backdrop_filter_layer.h"
#include "flutter/flow/paint_utils.h"

namespace flutter {

// State shared amongst matching layers across frames
class BackdropFilterLayerSharedState {
 public:
  explicit BackdropFilterLayerSharedState(
      fml::RefPtr<fml::TaskRunner> raster_task_runner)
      : raster_task_runner_(raster_task_runner) {}

  ~BackdropFilterLayerSharedState() {
    if (snapshot) {
      // TODO(knopp) we need to ensure that SkImage is destroyed
      // on raster thread, but propagating raster_task_runner_ all the way here
      // isn't right
      raster_task_runner_->PostTask([snapshot = this->snapshot]() {});
    }
  }

  // TODO(knopp) this should be probably managed by raster cache
  sk_sp<SkImage> snapshot;

  int no_change_frame_count = 0;
  fml::RefPtr<fml::TaskRunner> raster_task_runner_;
};

BackdropFilterLayer::BackdropFilterLayer(sk_sp<SkImageFilter> filter)
    : filter_(std::move(filter)) {}

BackdropFilterLayer::~BackdropFilterLayer() = default;

void BackdropFilterLayer::Preroll(PrerollContext* context,
                                  const SkMatrix& matrix) {
  // Damage pass
  if (context->damage_context &&
      context->damage_context->IsDeterminingDamage()) {
    // Backdrop filter blurs everything within clip area
    auto filter_bounds = filter_->computeFastBounds(context->cull_rect);

    auto handle = context->damage_context->AddLayerContribution(
        this, compare, matrix, filter_bounds, *context);

    const Layer* previous_layer = handle.PreviousLayer();
    if (previous_layer) {
      const BackdropFilterLayer* prev =
          reinterpret_cast<const BackdropFilterLayer*>(previous_layer);
      shared_state_ = prev->shared_state_;
    }
    paint_order_ = handle.PaintOrder();
    previous_paint_order_ = handle.PreviousPaintOrder();
    damage_below_ = SkRect::MakeEmpty();
    screen_bounds_ = matrix.mapRect(context->cull_rect);
    readback_bounds_ = matrix.mapRect(filter_bounds);

    handle.AddDelegate(this);

    if (!shared_state_) {
      shared_state_ = std::make_shared<BackdropFilterLayerSharedState>(
          context->damage_context->raster_task_runner());
    }
  }

  // Regular preroll; Check whether snapshot needs to be invalidated
  if (context->damage_context &&
      !context->damage_context->IsDeterminingDamage() && shared_state_) {
    if (damage_below_.isEmpty()) {
      ++shared_state_->no_change_frame_count;
    } else {
      shared_state_->no_change_frame_count = 0;
      shared_state_->snapshot = nullptr;
    }
  }

  bool need_save_layer = !shared_state_ || !shared_state_->snapshot;

  Layer::AutoPrerollSaveLayerState save =
      Layer::AutoPrerollSaveLayerState::Create(context, need_save_layer,
                                               bool(filter_));
  ContainerLayer::Preroll(context, matrix);
}

void BackdropFilterLayer::OnDamageAdded(const SkRect& rect,
                                        DamageContext::DamageSource source,
                                        int paint_order) {
  if (!shared_state_) {
    return;
  }

  if ((source == DamageContext::kPreviousFrame &&
       paint_order < previous_paint_order_) ||
      (source == DamageContext::kThisFrame && paint_order < paint_order_)) {
    if (readback_bounds_.intersects(rect)) {
      damage_below_.join(rect);
    }
  }
}

SkRect BackdropFilterLayer::OnReportAdditionalDamage(const SkRect& bounds) {
  if (shared_state_->snapshot) {
    if (!damage_below_.isEmpty()) {
      return readback_bounds_;
    }
  } else {
    if (readback_bounds_.intersects(bounds)) {
      return readback_bounds_;
    }
  }
  return SkRect::MakeEmpty();
}

void BackdropFilterLayer::Paint(PaintContext& context) const {
  TRACE_EVENT0("flutter", "BackdropFilterLayer::Paint");
  FML_DCHECK(needs_painting());

  const auto kThreshold = 60;

  if (shared_state_ && shared_state_->no_change_frame_count > kThreshold &&
      !shared_state_->snapshot) {
    // We're past no change threshold, take snapshot
    {
      Layer::AutoSaveLayer save = Layer::AutoSaveLayer::Create(
          context,
          SkCanvas::SaveLayerRec{&paint_bounds(), nullptr, filter_.get(), 0});
    }
    SkIRect screen_bounds;
    screen_bounds_.roundOut(&screen_bounds);
    shared_state_->snapshot =
        context.leaf_nodes_canvas->getSurface()->makeImageSnapshot(
            screen_bounds);
    PaintChildren(context);
  } else if (shared_state_ && shared_state_->snapshot) {
    // paint snapshot
    context.leaf_nodes_canvas->save();
    context.leaf_nodes_canvas->resetMatrix();
    context.leaf_nodes_canvas->drawImage(
        shared_state_->snapshot, screen_bounds_.left(), screen_bounds_.top());
    context.leaf_nodes_canvas->restore();
    DrawCheckerboard(context.leaf_nodes_canvas, paint_bounds());
    PaintChildren(context);
  } else {
    Layer::AutoSaveLayer save = Layer::AutoSaveLayer::Create(
        context,
        SkCanvas::SaveLayerRec{&paint_bounds(), nullptr, filter_.get(), 0});
    PaintChildren(context);
  }
}

bool BackdropFilterLayer::compare(const Layer* l1, const Layer* l2) {
  const auto* bf1 = reinterpret_cast<const BackdropFilterLayer*>(l1);
  const auto* bf2 = reinterpret_cast<const BackdropFilterLayer*>(l2);
  return bf1->filter_ == bf2->filter_;
}

}  // namespace flutter
