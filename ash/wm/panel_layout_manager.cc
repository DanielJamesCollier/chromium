// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/wm/panel_layout_manager.h"

#include <algorithm>

#include "ash/launcher/launcher.h"
#include "ash/shell.h"
#include "ash/wm/property_util.h"
#include "base/auto_reset.h"
#include "ui/aura/client/activation_client.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/root_window.h"
#include "ui/aura/window.h"
#include "ui/gfx/rect.h"
#include "ui/gfx/size.h"
#include "ui/views/widget/widget.h"

namespace {
const int kPanelMarginEdge = 4;
const int kPanelMarginMiddle = 8;

const int kMinimizedHeight = 24;

const float kMaxHeightFactor = .80f;
const float kMaxWidthFactor = .50f;
}

namespace ash {
namespace internal {

////////////////////////////////////////////////////////////////////////////////
// PanelLayoutManager public implementation:
PanelLayoutManager::PanelLayoutManager(aura::Window* panel_container)
    : panel_container_(panel_container),
      in_layout_(false),
      dragged_panel_(NULL),
      launcher_(NULL),
      last_active_panel_(NULL) {
  DCHECK(panel_container);
  Shell::GetRootWindow()->AddObserver(this);
}

PanelLayoutManager::~PanelLayoutManager() {
  if (launcher_)
    launcher_->RemoveIconObserver(this);
  Shell::GetRootWindow()->RemoveObserver(this);
}

void PanelLayoutManager::StartDragging(aura::Window* panel) {
  DCHECK(!dragged_panel_);
  DCHECK(panel->parent() == panel_container_);
  dragged_panel_ = panel;
}

void PanelLayoutManager::FinishDragging() {
  DCHECK(dragged_panel_);
  dragged_panel_ = NULL;
  Relayout();
}

void PanelLayoutManager::SetLauncher(ash::Launcher* launcher) {
  launcher_ = launcher;
  launcher_->AddIconObserver(this);
}

void PanelLayoutManager::ToggleMinimize(aura::Window* panel) {
  DCHECK(panel->parent() == panel_container_);
  if (panel->GetProperty(aura::client::kShowStateKey) ==
      ui::SHOW_STATE_MINIMIZED) {
    const gfx::Rect& old_bounds = panel->bounds();
    panel->SetProperty(aura::client::kShowStateKey, ui::SHOW_STATE_NORMAL);

    gfx::Rect new_bounds(old_bounds);
    const gfx::Rect* restore_bounds = GetRestoreBounds(panel);
    if (restore_bounds) {
      new_bounds.set_height(restore_bounds->height());
      new_bounds.set_y(old_bounds.bottom() - restore_bounds->height());
      SetChildBounds(panel, new_bounds);
      ClearRestoreBounds(panel);
    }
  } else {
    const gfx::Rect& old_bounds = panel->bounds();
    panel->SetProperty(aura::client::kShowStateKey, ui::SHOW_STATE_MINIMIZED);
    SetRestoreBounds(panel, old_bounds);
    SetChildBounds(panel,
                   gfx::Rect(old_bounds.x(),
                             old_bounds.bottom() - kMinimizedHeight,
                             old_bounds.width(),
                             kMinimizedHeight));
  }
  Relayout();
}

////////////////////////////////////////////////////////////////////////////////
// PanelLayoutManager, aura::LayoutManager implementation:
void PanelLayoutManager::OnWindowResized() {
  Relayout();
}

void PanelLayoutManager::OnWindowAddedToLayout(aura::Window* child) {
  panel_windows_.push_back(child);
  Relayout();
}

void PanelLayoutManager::OnWillRemoveWindowFromLayout(aura::Window* child) {
  PanelList::iterator found =
      std::find(panel_windows_.begin(), panel_windows_.end(), child);
  if (found != panel_windows_.end())
    panel_windows_.erase(found);

  if (dragged_panel_ == child)
    dragged_panel_ = NULL;

  if (last_active_panel_ == child)
    last_active_panel_ = NULL;

  Relayout();
}

void PanelLayoutManager::OnWindowRemovedFromLayout(aura::Window* child) {
}

void PanelLayoutManager::OnChildWindowVisibilityChanged(aura::Window* child,
                                                        bool visible) {
  Relayout();
}

void PanelLayoutManager::SetChildBounds(aura::Window* child,
                                        const gfx::Rect& requested_bounds) {
  gfx::Rect bounds(requested_bounds);
  const gfx::Rect& max_bounds = panel_container_->GetRootWindow()->bounds();
  const int max_width = max_bounds.width() * kMaxWidthFactor;
  const int max_height = max_bounds.height() * kMaxHeightFactor;
  if (bounds.width() > max_width)
    bounds.set_width(max_width);
  if (bounds.height() > max_height)
    bounds.set_height(max_height);

  // Reposition dragged panel in the panel order.
  if (dragged_panel_ == child) {
    PanelList::iterator dragged_panel_iter =
      std::find(panel_windows_.begin(), panel_windows_.end(), dragged_panel_);
    DCHECK(dragged_panel_iter != panel_windows_.end());
    PanelList::iterator new_position;
    for (new_position = panel_windows_.begin();
         new_position != panel_windows_.end();
         ++new_position) {
      const gfx::Rect& bounds = (*new_position)->bounds();
      if (bounds.x() + bounds.width()/2 <= requested_bounds.x()) break;
    }
    if (new_position != dragged_panel_iter) {
      panel_windows_.erase(dragged_panel_iter);
      panel_windows_.insert(new_position, dragged_panel_);
    }
  }

  SetChildBoundsDirect(child, bounds);
  Relayout();
}

////////////////////////////////////////////////////////////////////////////////
// PanelLayoutManager, ash::LauncherIconObserver implementation:
void PanelLayoutManager::OnLauncherIconPositionsChanged() {
  Relayout();
}

////////////////////////////////////////////////////////////////////////////////
// PanelLayoutManager, aura::WindowObserver implementation:
void PanelLayoutManager::OnWindowPropertyChanged(aura::Window* window,
                                                 const void* key,
                                                 intptr_t old) {
  if (key == aura::client::kRootWindowActiveWindowKey) {
    aura::Window* active = window->GetProperty(
        aura::client::kRootWindowActiveWindowKey);
    if (active && active->type() == aura::client::WINDOW_TYPE_PANEL)
      UpdateStacking(active);
  }
}


////////////////////////////////////////////////////////////////////////////////
// PanelLayoutManager private implementation:
void PanelLayoutManager::Relayout() {
  if (in_layout_)
    return;
  AutoReset<bool> auto_reset_in_layout(&in_layout_, true);

  aura::Window* active_panel = NULL;
  for (PanelList::iterator iter = panel_windows_.begin();
       iter != panel_windows_.end(); ++iter) {
    aura::Window* panel = *iter;
    if (!panel->IsVisible() || panel == dragged_panel_)
      continue;

    gfx::Rect icon_bounds =
        launcher_->GetScreenBoundsOfItemIconForWindow(panel);

    // An empty rect indicates that there is no icon for the panel in the
    // launcher. Just use the current bounds, as there's no icon to draw the
    // panel above.
    // TODO(dcheng): Need to anchor to overflow icon.
    if (icon_bounds.IsEmpty())
      continue;

    if (panel->HasFocus()) {
      DCHECK(!active_panel);
      active_panel = panel;
    }

    gfx::Point icon_origin = icon_bounds.origin();
    aura::Window::ConvertPointToWindow(panel_container_->GetRootWindow(),
                                       panel_container_, &icon_origin);

    // TODO(dcheng): Need to clamp to screen edges.
    gfx::Rect bounds = panel->bounds();
    bounds.set_x(
        icon_origin.x() + icon_bounds.width() / 2 - bounds.width() / 2);
    bounds.set_y(icon_origin.y() - bounds.height());
    SetChildBoundsDirect(panel, bounds);
  }

  UpdateStacking(active_panel);
}

void PanelLayoutManager::UpdateStacking(aura::Window* active_panel) {
  if (!active_panel) {
    if (!last_active_panel_)
      return;
    active_panel = last_active_panel_;
  }

  // We want to to stack the panels like a deck of cards:
  // ,--,--,--,-------.--.--.
  // |  |  |  |       |  |  |
  // |  |  |  |       |  |  |
  //
  // We use the middle of each panel to figure out how to stack the panels. This
  // allows us to update the stacking when a panel is being dragged around by
  // the titlebar--even though it doesn't update the launcher icon positions, we
  // still want the visual effect.
  std::map<int, aura::Window*> window_ordering;
  for (PanelList::const_iterator it = panel_windows_.begin();
       it != panel_windows_.end(); ++it) {
    gfx::Rect bounds = (*it)->bounds();
    window_ordering.insert(std::make_pair(bounds.x() + bounds.width() / 2,
                                          *it));
  }

  aura::Window* previous_panel = NULL;
  for (std::map<int, aura::Window*>::const_iterator it =
       window_ordering.begin();
       it != window_ordering.end() && it->second != active_panel; ++it) {
    if (previous_panel)
      panel_container_->StackChildAbove(it->second, previous_panel);
    previous_panel = it->second;
  }

  previous_panel = NULL;
  for (std::map<int, aura::Window*>::const_reverse_iterator it =
       window_ordering.rbegin();
       it != window_ordering.rend() && it->second != active_panel; ++it) {
    if (previous_panel)
      panel_container_->StackChildAbove(it->second, previous_panel);
    previous_panel = it->second;
  }

  panel_container_->StackChildAtTop(active_panel);
  last_active_panel_ = active_panel;
}

}  // namespace internal
}  // namespace ash
