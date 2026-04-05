// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "USB/deviceproxy.h"
#include <atomic>

// GunCon2 photodiode state — shared between GSRenderer (writer) and guncon2 (reader).
// Written by the GS thread, read by the EE/IOP thread.
extern std::atomic<bool> g_guncon2_display_dark;
extern std::atomic<int>  g_guncon2_count;


namespace usb_lightgun
{
	class GunCon2Device final : public DeviceProxy
	{
	public:
		USBDevice* CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const override;
		const char* Name() const override;
		const char* TypeName() const override;
		const char* IconName() const override;
		bool Freeze(USBDevice* dev, StateWrapper& sw) const override;
		void UpdateSettings(USBDevice* dev, SettingsInterface& si) const override;
		float GetBindingValue(const USBDevice* dev, u32 bind_index) const override;
		void SetBindingValue(USBDevice* dev, u32 bind_index, float value) const override;
		std::span<const InputBindingInfo> Bindings(u32 subtype) const override;
		std::span<const SettingInfo> Settings(u32 subtype) const override;
	};
} // namespace usb_lightgun
