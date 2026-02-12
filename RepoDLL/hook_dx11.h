#pragma once

bool HookDx11();
void UnhookDx11();

int GetMenuToggleVirtualKey();
void SetMenuToggleVirtualKey(int vk);
void BeginMenuToggleKeyCapture();
bool IsMenuToggleKeyCaptureActive();
