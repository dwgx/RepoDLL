#pragma once

bool HookDx11();
void UnhookDx11();

int GetMenuToggleVirtualKey();
void SetMenuToggleVirtualKey(int vk);
void BeginMenuToggleKeyCapture();
bool IsMenuToggleKeyCaptureActive();

int GetThirdPersonToggleVirtualKey();
void SetThirdPersonToggleVirtualKey(int vk);
void BeginThirdPersonToggleKeyCapture();
bool IsThirdPersonToggleKeyCaptureActive();
bool GetThirdPersonEnabled();
void SetThirdPersonEnabled(bool enabled);
