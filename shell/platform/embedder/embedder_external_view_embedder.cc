// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_view_embedder.h"

#include <algorithm>
#include <utility>

#include "flutter/shell/platform/embedder/embedder_layers.h"
#include "flutter/shell/platform/embedder/embedder_render_target.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "third_party/skia/include/gpu/GrDirectContext.h"

namespace flutter {

EmbedderExternalViewEmbedder::EmbedderExternalViewEmbedder(
    bool avoid_backing_store_cache,
    const CreateRenderTargetCallback& create_render_target_callback,
    const PresentCallback& present_callback)
    : avoid_backing_store_cache_(avoid_backing_store_cache),
      create_render_target_callback_(create_render_target_callback),
      present_callback_(present_callback) {
  FML_DCHECK(create_render_target_callback_);
  FML_DCHECK(present_callback_);
}

EmbedderExternalViewEmbedder::~EmbedderExternalViewEmbedder() = default;

void EmbedderExternalViewEmbedder::SetSurfaceTransformationCallback(
    SurfaceTransformationCallback surface_transformation_callback) {
  surface_transformation_callback_ = std::move(surface_transformation_callback);
}

SkMatrix EmbedderExternalViewEmbedder::GetSurfaceTransformation() const {
  if (!surface_transformation_callback_) {
    return SkMatrix{};
  }

  return surface_transformation_callback_();
}

void EmbedderExternalViewEmbedder::Reset() {
  pending_views_.clear();
  composition_order_.clear();
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::CancelFrame() {
  Reset();
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::BeginFrame(
    SkISize frame_size,
    GrDirectContext* context,
    double device_pixel_ratio,
    fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
  Reset();

  pending_frame_size_ = frame_size;
  pending_device_pixel_ratio_ = device_pixel_ratio;
  pending_surface_transformation_ = GetSurfaceTransformation();

  static const auto kRootViewIdentifier =
      EmbedderExternalView::ViewIdentifier{};

  pending_views_[kRootViewIdentifier] = std::make_unique<EmbedderExternalView>(
      pending_frame_size_, pending_surface_transformation_);
  composition_order_.push_back(kRootViewIdentifier);
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::PrerollCompositeEmbeddedView(
    int64_t view_id,
    std::unique_ptr<EmbeddedViewParams> params) {
  auto vid = EmbedderExternalView::ViewIdentifier(view_id);
  FML_DCHECK(pending_views_.count(vid) == 0);

  pending_views_[vid] = std::make_unique<EmbedderExternalView>(
      pending_frame_size_,              // frame size
      pending_surface_transformation_,  // surface xformation
      vid,                              // view identifier
      std::move(params)                 // embedded view params
  );
  composition_order_.push_back(vid);
}

// |ExternalViewEmbedder|
DlCanvas* EmbedderExternalViewEmbedder::GetRootCanvas() {
  auto found = pending_views_.find(EmbedderExternalView::ViewIdentifier{});
  if (found == pending_views_.end()) {
    FML_DLOG(WARNING)
        << "No root canvas could be found. This is extremely unlikely and "
           "indicates that the external view embedder did not receive the "
           "notification to begin the frame.";
    return nullptr;
  }
  return found->second->GetCanvas();
}

// |ExternalViewEmbedder|
DlCanvas* EmbedderExternalViewEmbedder::CompositeEmbeddedView(int64_t view_id) {
  auto vid = EmbedderExternalView::ViewIdentifier(view_id);
  auto found = pending_views_.find(vid);
  if (found == pending_views_.end()) {
    FML_DCHECK(false) << "Attempted to composite a view that was not "
                         "pre-rolled.";
    return nullptr;
  }
  return found->second->GetCanvas();
}

static FlutterBackingStoreConfig MakeBackingStoreConfig(
    const SkISize& backing_store_size) {
  FlutterBackingStoreConfig config = {};

  config.struct_size = sizeof(config);

  config.size.width = backing_store_size.width();
  config.size.height = backing_store_size.height();

  return config;
}

namespace {

struct PlatformView {
  EmbedderExternalView::ViewIdentifier view_identifier;
  const EmbeddedViewParams* params;
  SkRect clipped_frame;

  explicit PlatformView(const EmbedderExternalView* view) {
    assert(view->HasPlatformView());
    view_identifier = view->GetViewIdentifier();
    params = view->GetEmbeddedViewParams();

    clipped_frame = view->GetEmbeddedViewParams()->finalBoundingRect();
    SkMatrix transform;
    for (auto i = params->mutatorsStack().Begin();
         i != params->mutatorsStack().End(); ++i) {
      const auto& m = *i;
      if (m->GetType() == MutatorType::kTransform) {
        transform.preConcat(m->GetMatrix());
      } else if (m->GetType() == MutatorType::kClipRect) {
        auto rect = transform.mapRect(m->GetRect());
        if (!clipped_frame.intersect(rect)) {
          clipped_frame = SkRect::MakeEmpty();
        }
      } else if (m->GetType() == MutatorType::kClipRRect) {
        auto rect = transform.mapRect(m->GetRRect().getBounds());
        if (!clipped_frame.intersect(rect)) {
          clipped_frame = SkRect::MakeEmpty();
        }
      } else if (m->GetType() == MutatorType::kClipPath) {
        auto rect = transform.mapRect(m->GetPath().getBounds());
        if (!clipped_frame.intersect(rect)) {
          clipped_frame = SkRect::MakeEmpty();
        }
      }
    }
  }
};

class Layer {
 public:
  bool IntersectsPlatformView(const SkRect& rect) {
    for (auto& platform_view : platform_views_) {
      if (platform_view.clipped_frame.intersects(rect)) {
        return true;
      }
    }
    return false;
  }

  bool IntersectsPlatformView(const SkRegion& region) {
    for (auto& platform_view : platform_views_) {
      if (region.intersects(platform_view.clipped_frame.roundOut())) {
        return true;
      }
    }
    return false;
  }

  bool IntersectsFlutterContents(const SkRect& rect) {
    return flutter_contents_region_.intersects(rect.roundOut());
  }

  bool IntersectsFlutterContents(const SkRegion& region) {
    return flutter_contents_region_.intersects(region);
  }

  void AddPlatformView(const PlatformView& platform_view) {
    platform_views_.push_back(platform_view);
  }

  void AddFlutterContents(EmbedderExternalView* contents,
                          const SkRegion& contents_region) {
    flutter_contents_.push_back(contents);
    flutter_contents_region_.op(contents_region, SkRegion::kUnion_Op);
  }

  bool has_flutter_contents() const { return !flutter_contents_.empty(); }

  void SetRenderTarget(std::unique_ptr<EmbedderRenderTarget> target) {
    assert(render_target_ == nullptr);
    assert(has_flutter_contents());
    render_target_ = std::move(target);
  }

  void RenderFlutterContents() {
    assert(has_flutter_contents());
    assert(render_target_ != nullptr);

    bool clear_surface = true;
    for (auto c : flutter_contents_) {
      c->Render(*render_target_, clear_surface);
      clear_surface = false;
    }
  }

  const std::vector<PlatformView>& platform_views() const {
    return platform_views_;
  }

  const EmbedderRenderTarget* render_target() const {
    return render_target_.get();
  }

  std::list<SkRect> coverage() {
    std::list<SkRect> res;

    SkRegion::Iterator iterator(flutter_contents_region_);

    // Vertically deband cliperator rectangles.
    while (!iterator.done()) {
      auto rect = SkRect::Make(iterator.rect());
      auto iter = res.end();
      // If there is recangle previously in rect on which this one is a vertical
      // continuation, remove the previous rectangle and expand this one
      // vertically to cover the area.
      while (iter != res.begin()) {
        --iter;
        if (iter->bottom() < rect.top()) {
          // Went too far.
          break;
        } else if (iter->bottom() == rect.top() &&
                   iter->left() == rect.left() &&
                   iter->right() == rect.right()) {
          rect.fTop = iter->fTop;
          res.erase(iter);
          break;
        }
      }
      res.push_back(rect);
      iterator.next();
    }

    return res;
  }

 private:
  std::vector<PlatformView> platform_views_;
  std::vector<EmbedderExternalView*> flutter_contents_;
  SkRegion flutter_contents_region_;
  std::unique_ptr<EmbedderRenderTarget> render_target_;
};

class LayerBuilder {
 public:
  explicit LayerBuilder(SkISize frame_size) : frame_size_(frame_size) {
    layers_.push_back(Layer());
  }

  void AddExternalView(EmbedderExternalView* view) {
    if (view->HasPlatformView()) {
      PlatformView platform_view(view);
      AddPlatformView(platform_view);
    }
    if (view->HasEngineRenderedContents()) {
      AddFlutterContents(view);
    }
  }

  void PrepareBackingStore(
      const std::function<std::unique_ptr<EmbedderRenderTarget>(
          FlutterBackingStoreConfig)>& target_provider) {
    auto config = MakeBackingStoreConfig(frame_size_);
    for (auto& layer : layers_) {
      if (layer.has_flutter_contents()) {
        layer.SetRenderTarget(target_provider(config));
      }
    }
  }

  void Render() {
    for (auto& layer : layers_) {
      if (layer.has_flutter_contents()) {
        layer.RenderFlutterContents();
      }
    }
  }

  void PushLayers(EmbedderLayers& layers) {
    for (auto& layer : layers_) {
      for (auto& view : layer.platform_views()) {
        layers.PushPlatformViewLayer(
            view.view_identifier.platform_view_id.value(), *view.params);
      }
      if (layer.render_target() != nullptr) {
        layers.PushBackingStoreLayer(layer.render_target()->GetBackingStore(),
                                     layer.coverage());
      }
    }
  }

 private:
  void AddPlatformView(PlatformView view) {
    GetLayerForPlatformView(view).AddPlatformView(view);
  }

  void AddFlutterContents(EmbedderExternalView* contents) {
    assert(contents->HasEngineRenderedContents());

    SkRegion region;
    auto rects = contents->SearchNonOverlappingDrawnRects(
        SkRect::MakeIWH(frame_size_.width(), frame_size_.height()));
    for (const auto& r : rects) {
      region.op(r.roundOut(), SkRegion::kUnion_Op);
    }

    GetLayerForFlutterContentsRegion(region).AddFlutterContents(contents,
                                                                region);
  }

  Layer& GetLayerForPlatformView(PlatformView view) {
    for (auto iter = layers_.rbegin(); iter != layers_.rend(); ++iter) {
      if (iter->IntersectsFlutterContents(view.clipped_frame)) {
        if (iter == layers_.rbegin()) {
          layers_.emplace_back();
          return layers_.back();
        } else {
          --iter;
          return *iter;
        }
      }
      if (iter->IntersectsPlatformView(view.clipped_frame)) {
        return *iter;
      }
    }
    return layers_.front();
  }

  Layer& GetLayerForFlutterContentsRegion(const SkRegion& region) {
    for (auto iter = layers_.rbegin(); iter != layers_.rend(); ++iter) {
      if (iter->IntersectsPlatformView(region) ||
          iter->IntersectsFlutterContents(region)) {
        return *iter;
      }
    }
    return layers_.front();
  }

  std::vector<Layer> layers_;
  SkISize frame_size_;
};

};  // namespace

// https://flutter.dev/go/optimized-platform-view-layers
void EmbedderExternalViewEmbedder::SubmitFrameOptimized(
    GrDirectContext* context,
    std::unique_ptr<SurfaceFrame> frame) {
  SkRect _rect = SkRect::MakeIWH(pending_frame_size_.width(),
                                 pending_frame_size_.height());
  pending_surface_transformation_.mapRect(&_rect);

  LayerBuilder builder(SkISize::Make(_rect.width(), _rect.height()));

  for (auto view_id : composition_order_) {
    auto& view = pending_views_[view_id];
    builder.AddExternalView(view.get());
  }

  builder.PrepareBackingStore([&](FlutterBackingStoreConfig config) {
    return create_render_target_callback_(context, config);
  });

  if (context) {
    context->resetContext(kAll_GrBackendState);
  }

  builder.Render();

  if (context) {
    context->flushAndSubmit();
  }

  EmbedderLayers presented_layers(pending_frame_size_,
                                  pending_device_pixel_ratio_,
                                  pending_surface_transformation_);

  builder.PushLayers(presented_layers);

  presented_layers.InvokePresentCallback(present_callback_);

  (void)avoid_backing_store_cache_;

  frame->Submit();
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::SubmitFrameOriginal(
    GrDirectContext* context,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    std::unique_ptr<SurfaceFrame> frame) {
  auto [matched_render_targets, pending_keys] =
      render_target_cache_.GetExistingTargetsInCache(pending_views_);

  // This is where unused render targets will be collected. Control may flow to
  // the embedder. Here, the embedder has the opportunity to trample on the
  // OpenGL context.
  //
  // For optimum performance, we should tell the render target cache to clear
  // its unused entries before allocating new ones. This collection step before
  // allocating new render targets ameliorates peak memory usage within the
  // frame. But, this causes an issue in a known internal embedder. To work
  // around this issue while that embedder migrates, collection of render
  // targets is deferred after the presentation.
  //
  // @warning: Embedder may trample on our OpenGL context here.
  auto deferred_cleanup_render_targets =
      render_target_cache_.ClearAllRenderTargetsInCache();

  for (const auto& pending_key : pending_keys) {
    const auto& external_view = pending_views_.at(pending_key);

    // If the external view does not have engine rendered contents, it makes no
    // sense to ask to embedder to create a render target for us as we don't
    // intend to render into it and ask the embedder for presentation anyway.
    // Save some memory.
    if (!external_view->HasEngineRenderedContents()) {
      continue;
    }

    // This is the size of render surface we want the embedder to create for
    // us. As or right now, this is going to always be equal to the frame size
    // post transformation. But, in case optimizations are applied that make
    // it so that embedder rendered into surfaces that aren't full screen,
    // this assumption will break. So it's just best to ask view for its size
    // directly.
    const auto render_surface_size = external_view->GetRenderSurfaceSize();

    const auto backing_store_config =
        MakeBackingStoreConfig(render_surface_size);

    // This is where the embedder will create render targets for us. Control
    // flow to the embedder makes the engine susceptible to having the embedder
    // trample on the OpenGL context. Before any Skia operations are performed,
    // the context must be reset.
    //
    // @warning: Embedder may trample on our OpenGL context here.
    auto render_target = create_render_target_callback_(context, aiks_context,
                                                        backing_store_config);

    if (!render_target) {
      FML_LOG(ERROR) << "Embedder did not return a valid render target.";
      return;
    }
    matched_render_targets[pending_key] = std::move(render_target);
  }

  // The OpenGL context could have been trampled by the embedder at this point
  // as it attempted to collect old render targets and create new ones. Tell
  // Skia to not rely on existing bindings.
  if (context) {
    context->resetContext(kAll_GrBackendState);
  }

  // Scribble embedder provide render targets. The order in which we scribble
  // into the buffers is irrelevant to the presentation order.
  for (const auto& render_target : matched_render_targets) {
    if (!pending_views_.at(render_target.first)
             ->Render(*render_target.second)) {
      FML_LOG(ERROR)
          << "Could not render into the embedder supplied render target.";
      return;
    }
  }

  // We are going to be transferring control back over to the embedder there the
  // context may be trampled upon again. Flush all operations to the underlying
  // rendering API.
  //
  // @warning: Embedder may trample on our OpenGL context here.
  if (context) {
    context->flushAndSubmit();
  }

  // Submit the scribbled layer to the embedder for presentation.
  //
  // @warning: Embedder may trample on our OpenGL context here.
  {
    EmbedderLayers presented_layers(pending_frame_size_,
                                    pending_device_pixel_ratio_,
                                    pending_surface_transformation_);
    // In composition order, submit backing stores and platform views to the
    // embedder.
    for (const auto& view_id : composition_order_) {
      // If the external view has a platform view, ask the emebdder to place it
      // before the Flutter rendered contents for that interleaving level.
      const auto& external_view = pending_views_.at(view_id);
      if (external_view->HasPlatformView()) {
        presented_layers.PushPlatformViewLayer(
            // Covered by HasPlatformView().
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            external_view->GetViewIdentifier()
                .platform_view_id.value(),           // view id
            *external_view->GetEmbeddedViewParams()  // view params
        );
      }

      // If the view has engine rendered contents, ask the embedder to place
      // Flutter rendered contents for this interleaving level on top of a
      // platform view.
      if (external_view->HasEngineRenderedContents()) {
        const auto& exteral_render_target = matched_render_targets.at(view_id);
        const auto& external_view = pending_views_.at(view_id);
        auto rects =
            external_view->SearchNonOverlappingDrawnRects(SkRect::MakeIWH(
                pending_frame_size_.width(), pending_frame_size_.height()));

        presented_layers.PushBackingStoreLayer(
            exteral_render_target->GetBackingStore(), rects);
      }
    }

    // Flush the layer description down to the embedder for presentation.
    //
    // @warning: Embedder may trample on our OpenGL context here.
    presented_layers.InvokePresentCallback(present_callback_);
  }

  // See why this is necessary in the comment where this collection in realized.
  //
  // @warning: Embedder may trample on our OpenGL context here.
  deferred_cleanup_render_targets.clear();

  // Hold all rendered layers in the render target cache for one frame to
  // see if they may be reused next frame.
  for (auto& render_target : matched_render_targets) {
    if (!avoid_backing_store_cache_) {
      render_target_cache_.CacheRenderTarget(render_target.first,
                                             std::move(render_target.second));
    }
  }

  frame->Submit();
}

void EmbedderExternalViewEmbedder::SubmitFrame(
    GrDirectContext* context,
    std::unique_ptr<SurfaceFrame> frame) {
  auto optimized = getenv("FLUTTER_OPTIMIZE_SURFACES");
  if (!optimized || strcmp(optimized, "1") == 0) {
    SubmitFrameOptimized(context, std::move(frame));
  } else {
    SubmitFrameOriginal(context, std::move(frame));
  }
}

}  // namespace flutter
