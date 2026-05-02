// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SIO/Pad/PadBase.h"

class PadGunCon1 final : public PadBase
{
public:
	enum Inputs
	{
		PAD_TRIGGER,
		PAD_A,
		PAD_B,
		PAD_SHOOT_OFFSCREEN,
		LENGTH,
	};

private:
	u16 buttons = 0xFFFF;
	u32 pointer_index = 0;
	bool shoot_offscreen = false;
	std::string cursor_path;
	float cursor_scale = 1.0f;

	static constexpr std::array<u8, 3> bitmaskMapping = {{
		5,  // PAD_TRIGGER → PS1 bit 13 → second SIO byte bit 5 → PCSX2 bit 5
		11, // PAD_A       → PS1 bit 3  → first SIO byte bit 3  → PCSX2 bit 11
		6,  // PAD_B       → PS1 bit 14 → second SIO byte bit 6 → PCSX2 bit 6
	}};

	void ConfigLog();

	u8 Poll(u8 commandByte);
	u8 Config(u8 commandByte);
	u8 StatusInfo(u8 commandByte);
	u8 Constant1(u8 commandByte);
	u8 Constant2(u8 commandByte);
	u8 Constant3(u8 commandByte);
	u8 VibrationMap(u8 commandByte);

	std::pair<s16, s16> CalculateGunPosition() const;

public:
	PadGunCon1(u8 unifiedSlot, size_t ejectTicks);
	~PadGunCon1() override;

	void SetPointerSource(const std::string& source);
	void LoadCursorSettings(const SettingsInterface& si, const std::string& section);

	Pad::ControllerType GetType() const override;
	const Pad::ControllerInfo& GetInfo() const override;
	void Set(u32 index, float value) override;
	void SetRawAnalogs(const std::tuple<u8, u8> left, const std::tuple<u8, u8> right) override;
	void SetRawPressureButton(u32 index, const std::tuple<bool, u8> value) override;
	void SetAxisScale(float deadzone, float scale) override;
	float GetVibrationScale(u32 motor) const override;
	void SetVibrationScale(u32 motor, float scale) override;
	float GetPressureModifier() const override;
	void SetPressureModifier(float mod) override;
	void SetButtonDeadzone(float deadzone) override;
	void SetAnalogInvertL(bool x, bool y) override;
	void SetAnalogInvertR(bool x, bool y) override;
	float GetEffectiveInput(u32 index) const override;
	u8 GetRawInput(u32 index) const override;
	std::tuple<u8, u8> GetRawLeftAnalog() const override;
	std::tuple<u8, u8> GetRawRightAnalog() const override;
	u32 GetButtons() const override;
	u8 GetPressure(u32 index) const override;
	bool IsAnalogLightEnabled() const override;
	bool IsAnalogLocked() const override;

	bool Freeze(StateWrapper& sw) override;

	u8 SendCommandByte(u8 commandByte) override;

	static const Pad::ControllerInfo ControllerInfo;
};
