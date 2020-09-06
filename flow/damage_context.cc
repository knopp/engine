#include "flutter/flow/damage_context.h"
#include "flutter/flow/layers/layer.h"
#include "third_party/skia/include/core/SkImageFilter.h"

namespace flutter {

std::size_t DamageContext::LayerEntry::Hash::operator()(
    const LayerEntry& e) const noexcept {
  size_t res = e.paint_bounds.left();
  res = 37 * res + e.paint_bounds.top();
  res = 37 * res + e.paint_bounds.width();
  res = 37 * res + e.paint_bounds.height();
  res = 37 * res + size_t(reinterpret_cast<std::uintptr_t>(e.comparator));
  return res;
}

bool DamageContext::LayerEntry::operator==(const LayerEntry& e) const {
  return comparator == e.comparator && paint_bounds == e.paint_bounds &&
         std::equal(
             mutators.begin(), mutators.end(), e.mutators.begin(),
             e.mutators.end(),
             [](const std::shared_ptr<Mutator>& m1,
                const std::shared_ptr<Mutator>& m2) { return *m1 == *m2; }) &&
         (layer.get() == e.layer.get() ||
          comparator(layer.get(), e.layer.get()));
}

void DamageContext::InitFrame(const SkISize& tree_size,
                              const FrameDescription* previous_frame) {
  current_layer_tree_size_ = tree_size;
  previous_frame_ = previous_frame;
}

void DamageContext::PushLayerEntry(const Layer* layer,
                                   LayerComparator comparator,
                                   const SkMatrix& matrix,
                                   const PrerollContext& preroll_context,
                                   size_t index) {
  if (current_layer_tree_size_.isEmpty() || layer->paint_bounds().isEmpty()) {
    return;
  }

  LayerEntry e;
  e.layer = layer->shared_from_this();
  e.comparator = comparator;

  SkRect bounds = layer->paint_bounds();
  bounds.intersect(preroll_context.cull_rect);
  e.paint_bounds = matrix.mapRect(bounds);

  for (auto i = preroll_context.mutators_stack.Begin();
       i != preroll_context.mutators_stack.End(); ++i) {
    auto type = (*i)->GetType();
    // transforms are irrelevant because we compare paint_bounds in
    // screen coordinates
    if (type != MutatorType::transform) {
      e.mutators.push_back(*i);
    }
  }
  if (index == size_t(-1)) {
    layer_entries_.push_back(std::move(e));
  } else {
    layer_entries_.insert(layer_entries_.begin() + index, std::move(e));
  }
}

bool DamageContext::ApplyImageFilter(size_t from,
                                     size_t count,
                                     const SkImageFilter* filter,
                                     const SkMatrix& matrix,
                                     const SkRect& bounds) {
  SkMatrix inverted;
  if (!matrix.invert(&inverted)) {
    return false;
  }

  for (size_t i = from; i < count; ++i) {
    auto& entry = layer_entries_[i];
    SkRect layer_bounds(entry.paint_bounds);
    inverted.mapRect(layer_bounds);
    if (layer_bounds.intersects(bounds)) {
      // layer paint bounds after filter can not get bigger than union of
      // original paint bounds and filter bounds
      SkRect max(layer_bounds);
      max.join(bounds);

      layer_bounds = filter->computeFastBounds(layer_bounds);
      layer_bounds.intersect(max);

      matrix.mapRect(&layer_bounds);
      entry.paint_bounds = layer_bounds;
    }
  }

  return true;
}

DamageContext::DamageResult DamageContext::FinishFrame() {
  DamageResult res;
  LayerEntrySet entries;
  entries.insert(layer_entries_.begin(), layer_entries_.end());

  if (!previous_frame_ ||
      previous_frame_->layer_tree_size != current_layer_tree_size_) {
    res.damage_rect = SkRect::MakeIWH(current_layer_tree_size_.width(),
                                      current_layer_tree_size_.height());
  } else {
    // layer entries that are only found in one set (only this frame or only
    // previous frame) are for layers that were either added, removed, or
    // modified in any way (fail the equality check in LayerEntry) and thus
    // contribute to damage area

    res.damage_rect = SkRect::MakeEmpty();
    for (const auto& l : entries) {
      if (!res.damage_rect.contains(l.paint_bounds) &&
          previous_frame_->entries.find(l) == previous_frame_->entries.end()) {
        res.damage_rect.join(l.paint_bounds);
      }
    }
    for (const auto& l : previous_frame_->entries) {
      if (!res.damage_rect.contains(l.paint_bounds) &&
          entries.find(l) == entries.end()) {
        res.damage_rect.join(l.paint_bounds);
      }
    }
  }

  res.frame_description.reset(new FrameDescription());
  res.frame_description->entries = std::move(entries);
  res.frame_description->layer_tree_size = current_layer_tree_size_;

  previous_frame_ = nullptr;
  layer_entries_.clear();
  current_layer_tree_size_ = SkISize::MakeEmpty();

  return res;
}

}  // namespace flutter
