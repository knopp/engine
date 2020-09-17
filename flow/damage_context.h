#ifndef FLUTTER_FLOW_CONTEXT_H_
#define FLUTTER_FLOW_CONTEXT_H_

#include <unordered_set>
#include "flutter/flow/embedded_views.h"
#include "flutter/fml/task_runner.h"
#include "third_party/skia/include/core/SkRect.h"

namespace flutter {

struct PrerollContext;
class Layer;

class DamageArea {
 public:
  FML_DISALLOW_COPY_AND_ASSIGN(DamageArea);

  DamageArea() = default;
  DamageArea(DamageArea&&) = default;
  DamageArea& operator=(DamageArea&&) = default;

  const SkIRect& bounds() const { return bounds_; }

  std::vector<SkIRect> GetRects() const;

  void AddRect(const SkRect& rect);
  void AddRect(const SkIRect& rect);

 private:
  SkIRect bounds_ = SkIRect::MakeEmpty();
};

class DamageContext {
 public:
  enum DamageSource {
    // Damage originated from layer in current frame
    kThisFrame,

    // Damage originated from layer in last frame rendered in target
    // framebuffer
    kPreviousFrame,
  };

  // Some layers (backdrop or image filter) need finer grained access to
  // damage pass, for example backdrop layer may need to determine that no
  // contents changed underneath between past and present frame so that it
  // can cache filtered backround
  class Delegate {
   public:
    // This is called every time a layer from this or past frame contributes
    // to damange on screen
    virtual void OnDamageAdded(const SkRect& screen_bounds,
                               DamageSource source,
                               int paint_order){};

    // At the end of damage pass each delegate will get chance to contribute
    // additional damage; This may be called multiple times, since additional
    // damage from one delegate may affect additional damage reported by other
    virtual SkRect OnReportAdditionalDamage(const SkRect& total_damage_bounds) {
      return SkRect::MakeEmpty();
    };
  };

  typedef bool (*LayerComparator)(const Layer* l1, const Layer* l2);

  // Opaque representation of a frame contents
  class FrameDescription;

  void InitFrame(const SkISize& frame_size,
                 const FrameDescription* previous_frame_description);

  class LayerContributionHandle;

  LayerContributionHandle AddLayerContribution(
      const Layer* layer,
      LayerComparator comparator,
      const SkMatrix& matrix,
      const SkRect& paint_bounds,
      const PrerollContext& preroll_context);

  LayerContributionHandle AddLayerContribution(
      const Layer* layer,
      LayerComparator comparator,
      const SkMatrix& matrix,
      const PrerollContext& preroll_context);

  bool IsDeterminingDamage() const { return previous_frame_ != nullptr; }

  struct DamageResult {
    DamageArea area;
    std::unique_ptr<FrameDescription> frame_description;
  };

  DamageResult FinishFrame();

  void set_raster_task_runner(fml::RefPtr<fml::TaskRunner> runner) {
    raster_task_runner_ = runner;
  }

  fml::RefPtr<fml::TaskRunner> raster_task_runner() const {
    return raster_task_runner_;
  }

  class LayerContributionHandle {
   public:
    // Updates the paint bounds according to layer current paint_bounds.
    void UpdatePaintBounds();

    // If there is matching contribution in past frame, returns its layer,
    // nullptr otherwise
    const Layer* PreviousLayer() const;

    // Returns paint order of this contribution
    int PaintOrder() const;

    // IF there is matching contribution in past frame, returns paint order
    // of the past contribution. Otherwise returns -1
    int PreviousPaintOrder() const;

    // Registers delegate for this damage pass; The delegate will be
    // unregistered automatically after damage pass is done
    void AddDelegate(Delegate* delegate);

   private:
    friend class DamageContext;
    DamageContext* context_ = nullptr;
    size_t index_ = static_cast<size_t>(-1);
    SkMatrix matrix_;
  };

 private:
  // Represents a layer contribution to screen contents
  // LayerContribution can compare itself with a LayerContribution from past
  // frame to determine if the content they'd produce is identical
  // Diffing set of LayerContributions will give us damage area
  struct LayerContribution {
    SkRect paint_bounds;  // in screen coordinates
    std::shared_ptr<const Layer> layer;
    LayerComparator comparator;
    std::shared_ptr<MutatorNode> mutator_node;
    int paint_order;

    bool operator==(const LayerContribution& e) const;
    bool operator!=(const LayerContribution& e) const { return !(*this == e); }

    struct Hash {
      std::size_t operator()(const LayerContribution& e) const noexcept;
    };
  };

  using LayerContributionSet =
      std::unordered_set<LayerContribution, LayerContribution::Hash>;
  using LayerContributionList = std::vector<LayerContribution>;

  fml::RefPtr<fml::TaskRunner> raster_task_runner_;

  const FrameDescription* previous_frame_ = nullptr;
  SkISize current_layer_tree_size_ = SkISize::MakeEmpty();
  LayerContributionList layer_entries_;

  struct DelegateRecord {
    Delegate* delegate;
    int paint_order = 0;
    SkRect reported_damage = SkRect::MakeEmpty();
  };
  std::vector<DelegateRecord> delegates_;

  void AddDamageRect(DamageArea& area,
                     const SkRect& rect,
                     DamageSource source,
                     int paint_order);

  void FinishDelegates(DamageArea& area);

 public:
  class FrameDescription {
    SkISize layer_tree_size;
    DamageContext::LayerContributionSet entries;
    friend class DamageContext;
  };
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_CONTEXT_H_
