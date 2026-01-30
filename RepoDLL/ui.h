#pragma once

#include <vector>
#include "mono_bridge.h"

void RenderOverlay(bool* menu_open);
const std::vector<PlayerState>& UiGetCachedItems();
const std::vector<PlayerState>& UiGetCachedEnemies();
bool UiGetCachedMatrices(Matrix4x4& view, Matrix4x4& proj);
