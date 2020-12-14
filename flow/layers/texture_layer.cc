// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/texture_layer.h"

#include "flutter/common/graphics/texture.h"

namespace flutter {

TextureLayer::TextureLayer(const SkPoint& offset,
                           const SkSize& size,
                           int64_t texture_id,
                           bool freeze,
                           SkFilterQuality filter_quality)
    : offset_(offset),
      size_(size),
      texture_id_(texture_id),
      freeze_(freeze),
      filter_quality_(filter_quality) {}

#ifdef FLUTTER_ENABLE_DIFF_CONTEXT

void TextureLayer::Diff(DiffContext* context, const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  if (!context->IsSubtreeDirty()) {
    FML_DCHECK(old_layer);
    auto prev = old_layer->as_texture_layer();
    // TODO(knopp) It would be nice to be able to determine that a texture is
    // dirty
    context->MarkSubtreeDirty(context->GetOldLayerPaintRegion(prev));
  }
  context->AddPaintRegion(SkRect::MakeXYWH(offset_.x(), offset_.y(),
                                           size_.width(), size_.height()));
  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}

#endif  // FLUTTER_ENABLE_DIFF_CONTEXT

void TextureLayer::Preroll(PrerollContext* context, const SkMatrix& matrix) {
  TRACE_EVENT0("flutter", "TextureLayer::Preroll");

#if defined(LEGACY_FUCHSIA_EMBEDDER)
  CheckForChildLayerBelow(context);
#endif

  set_paint_bounds(SkRect::MakeXYWH(offset_.x(), offset_.y(), size_.width(),
                                    size_.height()));
}

void TextureLayer::Paint(PaintContext& context) const {
  TRACE_EVENT0("flutter", "TextureLayer::Paint");
  FML_DCHECK(needs_painting(context));

  std::shared_ptr<Texture> texture =
      context.texture_registry.GetTexture(texture_id_);
  if (!texture) {
    TRACE_EVENT_INSTANT0("flutter", "null texture");
    return;
  }
  texture->Paint(*context.leaf_nodes_canvas, paint_bounds(), freeze_,
                 context.gr_context, filter_quality_);
}

}  // namespace flutter
