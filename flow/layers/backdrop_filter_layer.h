// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_LAYERS_BACKDROP_FILTER_LAYER_H_
#define FLUTTER_FLOW_LAYERS_BACKDROP_FILTER_LAYER_H_

#include "flutter/flow/layers/container_layer.h"
#include "third_party/skia/include/core/SkImageFilter.h"

namespace flutter {

class BackdropFilterLayerSharedState;

class BackdropFilterLayer : public ContainerLayer,
                            private DamageContext::Delegate {
 public:
  BackdropFilterLayer(sk_sp<SkImageFilter> filter);
  ~BackdropFilterLayer() override;

  void Preroll(PrerollContext* context, const SkMatrix& matrix) override;

  void Paint(PaintContext& context) const override;

 private:
  sk_sp<SkImageFilter> filter_;

  // State shared between matching layers across frames
  std::shared_ptr<BackdropFilterLayerSharedState> shared_state_;

  int paint_order_;           // this layer z-index
  int previous_paint_order_;  // past layer z index
  SkRect screen_bounds_;      // are where layer paints
  SkRect readback_bounds_;    // are where layer samples from
  SkRect damage_below_;       // accumulates damage below this layer

  // DamageContext::Delegate
  void OnDamageAdded(const SkRect& rect,
                     DamageContext::DamageSource source,
                     int paint_order) override;

  SkRect OnReportAdditionalDamage(const SkRect& bounds) override;

  static bool compare(const Layer* l1, const Layer* l2);

  FML_DISALLOW_COPY_AND_ASSIGN(BackdropFilterLayer);
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_LAYERS_BACKDROP_FILTER_LAYER_H_
