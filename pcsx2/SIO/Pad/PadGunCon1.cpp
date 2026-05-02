// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Pad/PadGunCon1.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Sio.h"

#include "Common.h"
#include "GS/GS.h"
#include "Host.h"
#include "ImGui/ImGuiManager.h"
#include "Input/InputManager.h"
#include "USB/usb-lightgun/guncon2.h"

#include "IconsPromptFont.h"

// ========================================================================
// GunCon1 (G-Con 45) — PS1 lightgun emulation on PS2
//
// Protocol: SIO device ID 0x63, 6 data bytes per poll.
//   Bytes 0-1: buttons (inverted, trigger=bit5/B6, A=bit3/B5, B=bit6/B6)
//   Bytes 2-3: X position (s16 little-endian)
//   Bytes 4-5: Y position (s16 little-endian)
//   Offscreen = (0, 0)
//
// Based on nuvee plugin by psx-author/paubi.
// ========================================================================

// ========================================================================
// GunCon1 hardware coordinate model
// Source: PCSX-ReARMed libpcsxcore/pad.c (GPL-2.0), Mednafen psx/input/guncon.cpp
//
// X axis: fixed by video standard (8MHz ceramic resonator timing)
//   gun_x = X_OFS + (W * absX >> 10)
// Y axis: dynamic, reads vertical position from GS DISPLAY register
//   gun_y = DY + (DH * absY >> 10)
//
// No per-game configuration needed.
// ========================================================================

static constexpr s16 GC1_X_OFS  = 0x40;  // 64 — horizontal visible start
static constexpr s16 GC1_W_NTSC = 378;   // horizontal visible width, NTSC
static constexpr s16 GC1_W_PAL  = 385;   // horizontal visible width, PAL
// Y axis: DY and DH read dynamically from GS DISPLAY register at runtime.
// Fallback values if GS unavailable: NTSC DY=25(0x19) DH=240, PAL DY=48(0x30) DH=256.
static constexpr float GC1_OFFSCREEN_BORDER = 0.02f;

static const InputBindingInfo s_bindings[] = {
	// clang-format off
	{"Trigger", TRANSLATE_NOOP("Pad", "Trigger"), ICON_PF_CROSS, InputBindingInfo::Type::Button, PadGunCon1::Inputs::PAD_TRIGGER, GenericInputBinding::Cross},
	{"A", TRANSLATE_NOOP("Pad", "A Button"), nullptr, InputBindingInfo::Type::Button, PadGunCon1::Inputs::PAD_A, GenericInputBinding::Circle},
	{"B", TRANSLATE_NOOP("Pad", "B Button"), nullptr, InputBindingInfo::Type::Button, PadGunCon1::Inputs::PAD_B, GenericInputBinding::Triangle},
	{"ShootOffscreen", TRANSLATE_NOOP("Pad", "Shoot Offscreen"), nullptr, InputBindingInfo::Type::Button, PadGunCon1::Inputs::PAD_SHOOT_OFFSCREEN, GenericInputBinding::Unknown},
	// clang-format on
};

static std::vector<std::pair<std::string, std::string>> GetPointerDeviceList()
{
	std::vector<std::pair<std::string, std::string>> result;
	result.emplace_back("Auto", "Auto");
	for (const auto& [id, name] : InputManager::EnumerateRawPointerDevices())
		result.emplace_back(id, name);
	return result;
}

static const SettingInfo s_settings[] = {
	{SettingInfo::Type::StringList, "pointer_source", TRANSLATE_NOOP("Pad", "Pointer Device"),
		TRANSLATE_NOOP("Pad", "Selects which pointer device to use for the lightgun. Auto uses the port number."),
		"Auto", nullptr, nullptr, nullptr, nullptr, nullptr, &GetPointerDeviceList},
	{SettingInfo::Type::String, "cursor_path", TRANSLATE_NOOP("Pad", "Cursor Image Path"),
		TRANSLATE_NOOP("Pad", "Sets the crosshair image displayed on screen. Leave empty for no crosshair."),
		"", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
	{SettingInfo::Type::Float, "cursor_scale", TRANSLATE_NOOP("Pad", "Cursor Scale"),
		TRANSLATE_NOOP("Pad", "Sets the scale of the crosshair image."),
		"1.0", "0.01", "10.0", "0.01", "%.0f%%", nullptr, nullptr, 100.0f},
};

const Pad::ControllerInfo PadGunCon1::ControllerInfo = {Pad::ControllerType::GunCon1, "GunCon1",
	TRANSLATE_NOOP("Pad", "GunCon (G-Con 45)"), ICON_PF_CROSS, s_bindings, s_settings, Pad::VibrationCapabilities::NoVibration};

// ========================================================================
// Construction / info
// ========================================================================

PadGunCon1::PadGunCon1(u8 unifiedSlot, size_t ejectTicks)
	: PadBase(unifiedSlot, ejectTicks)
{
	currentMode = Pad::Mode::PS1_NAMCO_LIGHTGUN;
	const auto [port, slot] = sioConvertPadToPortAndSlot(unifiedSlot);
	pointer_index = std::min(static_cast<u32>(port), InputManager::MAX_POINTER_DEVICES - 1u);

	// Activate GS photodiode sampling (shared with GunCon2).
	// GunCon1 is a passive photodetector — it only reports position when
	// the screen pixel at the gun position is bright enough.
	g_guncon2_count.fetch_add(1, std::memory_order_relaxed);
}

PadGunCon1::~PadGunCon1()
{
	if (!cursor_path.empty())
		ImGuiManager::ClearSoftwareCursor(pointer_index);

	if (g_guncon2_count.fetch_sub(1, std::memory_order_relaxed) == 1)
		g_guncon2_display_dark.store(false, std::memory_order_release);
}

Pad::ControllerType PadGunCon1::GetType() const
{
	return Pad::ControllerType::GunCon1;
}

const Pad::ControllerInfo& PadGunCon1::GetInfo() const
{
	return ControllerInfo;
}

// ========================================================================
// Pointer source resolution
// ========================================================================

void PadGunCon1::SetPointerSource(const std::string& source)
{
	const auto [port, slot] = sioConvertPadToPortAndSlot(unifiedSlot);

	if (source.empty() || source == "Auto")
	{
		pointer_index = std::min(static_cast<u32>(port), InputManager::MAX_POINTER_DEVICES - 1u);
		Console.WriteLn("(GunCon1) Port %u: Auto pointer → Mouse %u", port, pointer_index);
	}
	else
	{
		const std::optional<u32> idx = InputManager::GetPointerIndexForRawDevice(source);
		pointer_index = std::min(idx.value_or(static_cast<u32>(port)), InputManager::MAX_POINTER_DEVICES - 1u);
		Console.WriteLn("(GunCon1) Port %u: Manual pointer → index %u", port, pointer_index);
	}
}

void PadGunCon1::LoadCursorSettings(const SettingsInterface& si, const std::string& section)
{
	std::string new_cursor_path = si.GetStringValue(section.c_str(), "cursor_path", "");
	const float new_cursor_scale = si.GetFloatValue(section.c_str(), "cursor_scale", 1.0f);

	if (cursor_path != new_cursor_path || cursor_scale != new_cursor_scale)
	{
		const bool had_cursor = !cursor_path.empty();
		cursor_path = std::move(new_cursor_path);
		cursor_scale = new_cursor_scale;

		if (!cursor_path.empty())
		{
			ImGuiManager::SetSoftwareCursor(pointer_index, cursor_path, cursor_scale);
		}
		else if (had_cursor)
		{
			ImGuiManager::ClearSoftwareCursor(pointer_index);
		}
	}
}

// ========================================================================
// Position calculation — hardware GunCon1 model
// ========================================================================

std::pair<s16, s16> PadGunCon1::CalculateGunPosition() const
{
	// GunCon1 offscreen: (0x0001, 0x000A)
	if (shoot_offscreen)
		return {0x0001, 0x000A};

	// GunCon1 is a passive photodetector — no light detected = offscreen.
	// Reuses the GunCon2 photodiode (samples center pixel of framebuffer).
	if (g_guncon2_display_dark.load(std::memory_order_acquire))
		return {0x0001, 0x000A};

	const auto [window_x, window_y] = InputManager::GetPointerAbsolutePosition(pointer_index);

	float display_x, display_y;
	GSTranslateWindowToDisplayCoordinates(window_x, window_y, &display_x, &display_y);

	if (display_x < GC1_OFFSCREEN_BORDER || display_y < GC1_OFFSCREEN_BORDER ||
		display_x > (1.0f - GC1_OFFSCREEN_BORDER) || display_y > (1.0f - GC1_OFFSCREEN_BORDER))
	{
		return {0x0001, 0x000A};
	}

	const bool is_pal = (GSgetDisplayMode() == GSVideoMode::PAL);

	// X: fixed by video standard (horizontal beam timing is constant)
	const int w = is_pal ? GC1_W_PAL : GC1_W_NTSC;
	const int absX = static_cast<int>(display_x * 1023.0f);
	const s16 gun_x = static_cast<s16>(GC1_X_OFS + (w * absX >> 10));

	// Y: read vertical position dynamically from GS DISPLAY register
	int dy, dh;
	GSgetDisplayYInfo(&dy, &dh);
	const int absY = static_cast<int>(display_y * 1023.0f);
	const s16 gun_y = static_cast<s16>(dy + (dh * absY >> 10));

	return {
		std::max(gun_x, static_cast<s16>(1)),
		std::max(gun_y, static_cast<s16>(1))
	};
}

// ========================================================================
// Input handling
// ========================================================================

void PadGunCon1::Set(u32 index, float value)
{
	if (index >= Inputs::LENGTH)
		return;

	if (index == Inputs::PAD_SHOOT_OFFSCREEN)
	{
		shoot_offscreen = (value != 0.0f);
		// ShootOffscreen also presses trigger
		if (shoot_offscreen)
			this->buttons &= ~(1u << bitmaskMapping[Inputs::PAD_TRIGGER]);
		else
			this->buttons |= (1u << bitmaskMapping[Inputs::PAD_TRIGGER]);
		return;
	}

	this->rawInputs[index] = static_cast<u8>(std::clamp(value * 255.0f, 0.0f, 255.0f));

	if (this->rawInputs[index] > 0)
		this->buttons &= ~(1u << bitmaskMapping[index]);
	else
		this->buttons |= (1u << bitmaskMapping[index]);
}

// ========================================================================
// SIO command handlers
// ========================================================================

void PadGunCon1::ConfigLog()
{
	const auto [port, slot] = sioConvertPadToPortAndSlot(unifiedSlot);
	Console.WriteLn("(GunCon1) Port %u: Config Finished - pointer %u", port + 1, pointer_index);
}

u8 PadGunCon1::Poll(u8 commandByte)
{
	const auto [pos_x, pos_y] = CalculateGunPosition();

	switch (this->commandBytesReceived)
	{
		case 3:
			return (this->buttons >> 8) & 0xFF; // First button byte (A)
		case 4:
			return this->buttons & 0xFF; // Second button byte (Trigger, B)
		case 5:
			return static_cast<u8>(pos_x & 0xFF); // X low
		case 6:
			return static_cast<u8>((pos_x >> 8) & 0xFF); // X high
		case 7:
			return static_cast<u8>(pos_y & 0xFF); // Y low
		case 8:
			return static_cast<u8>((pos_y >> 8) & 0xFF); // Y high
	}

	Console.Warning("%s(%02X) Did not reach a valid return path! Returning zero.", __FUNCTION__, commandByte);
	return 0x00;
}

u8 PadGunCon1::Config(u8 commandByte)
{
	if (this->commandBytesReceived == 3)
	{
		if (commandByte)
			this->isInConfig = true;
		else
			this->isInConfig = false;
	}

	// GunCon1 config responses are all 0xFF
	return 0xFF;
}

u8 PadGunCon1::StatusInfo(u8 commandByte)
{
	// PS2 "custom" model — all zeros (from nuvee)
	switch (this->commandBytesReceived)
	{
		case 3: return 0x00;
		case 4: return 0x00;
		case 5: return 0x00;
		case 6: return 0x00;
		case 7: return 0x00;
		case 8: return 0x00;
	}
	return 0x00;
}

u8 PadGunCon1::Constant1(u8 commandByte)
{
	return StatusInfo(commandByte);
}

u8 PadGunCon1::Constant2(u8 commandByte)
{
	return StatusInfo(commandByte);
}

u8 PadGunCon1::Constant3(u8 commandByte)
{
	return StatusInfo(commandByte);
}

u8 PadGunCon1::VibrationMap(u8 commandByte)
{
	// GunCon1 has no vibration — return 0xFF
	return 0xFF;
}

// ========================================================================
// SendCommandByte — main SIO dispatch
// ========================================================================

u8 PadGunCon1::SendCommandByte(u8 commandByte)
{
	u8 ret = 0;

	switch (this->commandBytesReceived)
	{
		case 0:
			ret = 0x00;
			break;
		case 1:
			this->currentCommand = static_cast<Pad::Command>(commandByte);

			if (this->currentCommand != Pad::Command::POLL && this->currentCommand != Pad::Command::CONFIG && !this->isInConfig)
			{
				Console.Warning("%s(%02X) Config-only command sent outside config mode!", __FUNCTION__, commandByte);
			}

			ret = this->isInConfig ? static_cast<u8>(Pad::Mode::CONFIG) : static_cast<u8>(Pad::Mode::PS1_NAMCO_LIGHTGUN);
			break;
		case 2:
			ret = 0x5A;
			break;
		default:
			switch (this->currentCommand)
			{
				case Pad::Command::POLL:
					ret = Poll(commandByte);
					break;
				case Pad::Command::CONFIG:
					ret = Config(commandByte);
					break;
				case Pad::Command::STATUS_INFO:
					ret = StatusInfo(commandByte);
					break;
				case Pad::Command::CONST_1:
					ret = Constant1(commandByte);
					break;
				case Pad::Command::CONST_2:
					ret = Constant2(commandByte);
					break;
				case Pad::Command::CONST_3:
					ret = Constant3(commandByte);
					break;
				case Pad::Command::VIBRATION_MAP:
					ret = VibrationMap(commandByte);
					break;
				default:
					ret = 0x00;
					break;
			}
	}

	this->commandBytesReceived++;
	return ret;
}

// ========================================================================
// PadBase stubs — GunCon1 has no analog sticks, vibration, or pressure
// ========================================================================

void PadGunCon1::SetRawAnalogs(const std::tuple<u8, u8> left, const std::tuple<u8, u8> right) {}
void PadGunCon1::SetRawPressureButton(u32 index, const std::tuple<bool, u8> value) {}
void PadGunCon1::SetAxisScale(float deadzone, float scale) {}
float PadGunCon1::GetVibrationScale(u32 motor) const { return 0.0f; }
void PadGunCon1::SetVibrationScale(u32 motor, float scale) {}
float PadGunCon1::GetPressureModifier() const { return 0.0f; }
void PadGunCon1::SetPressureModifier(float mod) {}
void PadGunCon1::SetButtonDeadzone(float deadzone) {}
void PadGunCon1::SetAnalogInvertL(bool x, bool y) {}
void PadGunCon1::SetAnalogInvertR(bool x, bool y) {}

float PadGunCon1::GetEffectiveInput(u32 index) const
{
	return GetRawInput(index);
}

u8 PadGunCon1::GetRawInput(u32 index) const
{
	return (index < Inputs::LENGTH) ? rawInputs[index] : 0;
}

std::tuple<u8, u8> PadGunCon1::GetRawLeftAnalog() const { return {0x7F, 0x7F}; }
std::tuple<u8, u8> PadGunCon1::GetRawRightAnalog() const { return {0x7F, 0x7F}; }

u32 PadGunCon1::GetButtons() const
{
	return buttons;
}

u8 PadGunCon1::GetPressure(u32 index) const { return 0; }
bool PadGunCon1::IsAnalogLightEnabled() const { return false; }
bool PadGunCon1::IsAnalogLocked() const { return false; }

// ========================================================================
// Savestate
// ========================================================================

bool PadGunCon1::Freeze(StateWrapper& sw)
{
	if (!PadBase::Freeze(sw) || !sw.DoMarker("PadGunCon1"))
		return false;

	sw.Do(&buttons);
	sw.Do(&shoot_offscreen);
	return !sw.HasError();
}
