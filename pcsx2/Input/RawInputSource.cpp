// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef _WIN32

#include "Input/RawInputSource.h"
#include "Input/InputManager.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/StringUtil.h"

#include "Host.h"

#include "fmt/format.h"

#include <algorithm>

#include <hidsdi.h>

RawInputSource::RawInputSource() = default;

RawInputSource::~RawInputSource() = default;

std::string RawInputSource::GetDeviceIdentifier(u32 index)
{
	return fmt::format("RawMouse-{}", index);
}

bool RawInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	settings_lock.unlock();
	const std::optional<WindowInfo> toplevel_wi(Host::GetTopLevelWindowInfo());
	settings_lock.lock();

	if (!toplevel_wi.has_value() || toplevel_wi->type != WindowInfo::Type::Win32)
	{
		Console.Error("(RawInput) Missing top level window.");
		return false;
	}

	m_hwnd = static_cast<HWND>(toplevel_wi->window_handle);

	if (!EnumerateRawMice())
	{
		Console.Error("(RawInput) Failed to enumerate raw input devices.");
		return false;
	}

	m_initialized = true;

	if (m_mice.empty())
	{
		Console.Warning("(RawInput) No mice found.");
		return true;
	}

	Console.WriteLn("(RawInput) Initialized with %zu mice.", m_mice.size());
	for (const auto& mouse : m_mice)
	{
		Console.WriteLn("  [pointer %u] %s", mouse.pointer_index, mouse.display_name.c_str());
		Console.WriteLn("    path: %s", mouse.device_path.c_str());
		InputManager::OnInputDeviceConnected(GetDeviceIdentifier(mouse.pointer_index), mouse.display_name);
	}

	return true;
}

void RawInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
}

bool RawInputSource::ReloadDevices()
{
	const size_t old_count = m_mice.size();

	for (const auto& mouse : m_mice)
	{
		const InputBindingKey key = MakeGenericControllerButtonKey(InputSourceType::RawInput, mouse.pointer_index, 0);
		InputManager::OnInputDeviceDisconnected(key, GetDeviceIdentifier(mouse.pointer_index));
	}

	m_mice.clear();
	m_handle_to_mouse_index.clear();

	if (!EnumerateRawMice())
		return false;

	for (const auto& mouse : m_mice)
		InputManager::OnInputDeviceConnected(GetDeviceIdentifier(mouse.pointer_index), mouse.display_name);

	return (m_mice.size() != old_count);
}

void RawInputSource::Shutdown()
{
	for (const auto& mouse : m_mice)
	{
		const InputBindingKey key = MakeGenericControllerButtonKey(InputSourceType::RawInput, mouse.pointer_index, 0);
		InputManager::OnInputDeviceDisconnected(key, GetDeviceIdentifier(mouse.pointer_index));
	}

	m_mice.clear();
	m_handle_to_mouse_index.clear();
	m_hwnd = nullptr;
	m_initialized = false;
}

bool RawInputSource::IsInitialized()
{
	return m_initialized;
}

void RawInputSource::PollEvents()
{
	// Events arrive via MainWindow::nativeEvent -> ProcessRawInput.
}

std::vector<std::pair<std::string, std::string>> RawInputSource::EnumerateDevices()
{
	std::vector<std::pair<std::string, std::string>> devices;
	for (const auto& mouse : m_mice)
		devices.emplace_back(GetDeviceIdentifier(mouse.pointer_index), mouse.display_name);
	return devices;
}

std::vector<InputBindingKey> RawInputSource::EnumerateMotors()
{
	return {};
}

bool RawInputSource::GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
	return false;
}

InputLayout RawInputSource::GetControllerLayout(u32 index)
{
	return InputLayout::Unknown;
}

void RawInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
}

void RawInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity)
{
}

std::optional<InputBindingKey> RawInputSource::ParseKeyString(const std::string_view device, const std::string_view binding)
{
	if (!device.starts_with("RawMouse-") || binding.empty())
		return std::nullopt;

	const std::optional<s32> device_index = StringUtil::FromChars<s32>(device.substr(9));
	if (!device_index.has_value() || device_index.value() < 0)
		return std::nullopt;

	InputBindingKey key = {};
	key.source_type = InputSourceType::RawInput;
	key.source_index = static_cast<u32>(device_index.value());

	for (u32 i = 0; i < NUM_BUTTONS; i++)
	{
		if (binding == s_button_names[i])
		{
			key.source_subtype = InputSubclass::ControllerButton;
			key.data = i;
			return key;
		}
	}

	if (binding.starts_with("Button"))
	{
		const std::optional<u32> button_index = StringUtil::FromChars<u32>(binding.substr(6));
		if (!button_index.has_value())
			return std::nullopt;

		key.source_subtype = InputSubclass::ControllerButton;
		key.data = button_index.value();
		return key;
	}

	return std::nullopt;
}

TinyString RawInputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
	TinyString ret;

	if (key.source_type == InputSourceType::RawInput)
	{
		if (key.source_subtype == InputSubclass::ControllerButton)
		{
			if (display)
			{
				if (key.data < NUM_BUTTONS)
					ret.format("RawMouse-{} {}", u32{key.source_index}, s_button_display_names[key.data]);
				else
					ret.format("RawMouse-{} Button {}", u32{key.source_index}, key.data + 1);
			}
			else
			{
				if (key.data < NUM_BUTTONS)
					ret.format("RawMouse-{}/{}", u32{key.source_index}, s_button_names[key.data]);
				else
					ret.format("RawMouse-{}/Button{}", u32{key.source_index}, key.data);
			}
		}
	}

	return ret;
}

TinyString RawInputSource::ConvertKeyToIcon(InputBindingKey key)
{
	return {};
}

bool RawInputSource::EnumerateRawMice()
{
	UINT device_count = 0;
	if (GetRawInputDeviceList(nullptr, &device_count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
	{
		Console.Error("(RawInput) GetRawInputDeviceList(count) failed: %08X", GetLastError());
		return false;
	}

	if (device_count == 0)
		return true;

	std::vector<RAWINPUTDEVICELIST> device_list(device_count);
	if (GetRawInputDeviceList(device_list.data(), &device_count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
	{
		Console.Error("(RawInput) GetRawInputDeviceList(enum) failed: %08X", GetLastError());
		return false;
	}

	u32 mouse_count = 0;
	for (UINT i = 0; i < device_count && mouse_count < InputManager::MAX_POINTER_DEVICES; i++)
	{
		if (device_list[i].dwType != RIM_TYPEMOUSE)
			continue;

		const HANDLE handle = device_list[i].hDevice;

		UINT name_size = 0;
		GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, nullptr, &name_size);

		std::wstring wdevice_path;
		std::string device_path;
		if (name_size > 0)
		{
			wdevice_path.resize(name_size, L'\0');
			if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, wdevice_path.data(), &name_size) != static_cast<UINT>(-1))
			{
				// Trim trailing null characters — Windows includes the null terminator in name_size,
				// which would embed a \0 in the std::string and break comparisons with ini values.
				while (!wdevice_path.empty() && wdevice_path.back() == L'\0')
					wdevice_path.pop_back();
				device_path = StringUtil::WideStringToUTF8String(wdevice_path);
			}
		}

		if (device_path.empty())
		{
			Console.Warning("(RawInput) Skipping mouse with no device path (handle=%p).", handle);
			continue;
		}

		// Try to get the real USB product name via HID API.
		std::string product_name;
		{
			HANDLE hid_handle = CreateFileW(wdevice_path.c_str(), 0,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (hid_handle != INVALID_HANDLE_VALUE)
			{
				wchar_t product_string[256] = {};
				if (HidD_GetProductString(hid_handle, product_string, sizeof(product_string)) && product_string[0] != L'\0')
					product_name = StringUtil::WideStringToUTF8String(product_string);
				CloseHandle(hid_handle);
			}
		}

		// Build display name: prefer product name, fall back to VID/PID.
		std::string display_name;
		if (!product_name.empty())
		{
			display_name = fmt::format("{} (Mouse {})", product_name, mouse_count);
		}
		else
		{
			std::string vid, pid;
			const size_t vid_pos = device_path.find("VID_");
			const size_t pid_pos = device_path.find("PID_");
			if (vid_pos != std::string::npos && vid_pos + 8 <= device_path.size())
				vid = device_path.substr(vid_pos + 4, 4);
			if (pid_pos != std::string::npos && pid_pos + 8 <= device_path.size())
				pid = device_path.substr(pid_pos + 4, 4);

			if (!vid.empty() && !pid.empty())
				display_name = fmt::format("Mouse {} (VID:{} PID:{})", mouse_count, vid, pid);
			else
				display_name = fmt::format("Mouse {}", mouse_count);
		}

		RawMouseDevice dev;
		dev.handle = handle;
		dev.device_path = std::move(device_path);
		dev.display_name = std::move(display_name);
		dev.pointer_index = mouse_count;
		dev.button_state = 0;

		m_handle_to_mouse_index[handle] = mouse_count;
		m_mice.push_back(std::move(dev));
		mouse_count++;
	}

	if (mouse_count >= InputManager::MAX_POINTER_DEVICES)
	{
		u32 total_mice = 0;
		for (UINT i = 0; i < device_count; i++)
		{
			if (device_list[i].dwType == RIM_TYPEMOUSE)
				total_mice++;
		}
		if (total_mice > InputManager::MAX_POINTER_DEVICES)
			Console.Warning("(RawInput) %u mice detected, only using first %u.", total_mice, InputManager::MAX_POINTER_DEVICES);
	}

	return true;
}

std::optional<u32> RawInputSource::GetPointerIndexForDevicePath(const std::string_view device_path) const
{
	for (const auto& mouse : m_mice)
	{
		if (mouse.device_path == device_path)
			return mouse.pointer_index;
	}
	return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> RawInputSource::GetRawMouseDeviceList() const
{
	std::vector<std::pair<std::string, std::string>> result;
	for (const auto& mouse : m_mice)
		result.emplace_back(mouse.device_path, mouse.display_name);
	return result;
}

void RawInputSource::ProcessRawInput(const RAWINPUT* raw, HWND render_hwnd)
{
	if (!m_initialized || raw->header.dwType != RIM_TYPEMOUSE)
		return;

	const auto it = m_handle_to_mouse_index.find(raw->header.hDevice);
	if (it == m_handle_to_mouse_index.end())
		return;

	const u32 mouse_idx = it->second;
	pxAssert(mouse_idx < m_mice.size());
	RawMouseDevice& mouse = m_mice[mouse_idx];
	const RAWMOUSE& rm = raw->data.mouse;
	const u32 pointer_index = mouse.pointer_index;

	const HWND coord_hwnd = render_hwnd ? render_hwnd : m_hwnd;

	// DIAG: log first 5 motion events per device to check flags and coordinates
	static std::array<u32, InputManager::MAX_POINTER_DEVICES> s_diag_move_count = {};
	const bool diag_should_log_move = (pointer_index < s_diag_move_count.size() && s_diag_move_count[pointer_index] < 5);

	if (rm.usFlags & MOUSE_MOVE_ABSOLUTE)
	{
		const bool is_virtual_desktop = (rm.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
		const int screen_w = GetSystemMetrics(is_virtual_desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
		const int screen_h = GetSystemMetrics(is_virtual_desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);

		if (screen_w > 0 && screen_h > 0)
		{
			POINT pt;
			pt.x = static_cast<LONG>(static_cast<float>(rm.lLastX) / 65535.0f * static_cast<float>(screen_w));
			pt.y = static_cast<LONG>(static_cast<float>(rm.lLastY) / 65535.0f * static_cast<float>(screen_h));

			if (is_virtual_desktop)
			{
				pt.x += GetSystemMetrics(SM_XVIRTUALSCREEN);
				pt.y += GetSystemMetrics(SM_YVIRTUALSCREEN);
			}

			// DIAG: log absolute position details
			if (diag_should_log_move)
			{
				Console.WriteLn("(DIAG:RawInput) mouse[%u] ptr=%u ABS raw=(%ld,%ld) flags=0x%04X screen=%dx%d → pixel=(%ld,%ld) hwnd=%p render=%p",
					mouse_idx, pointer_index, rm.lLastX, rm.lLastY, rm.usFlags, screen_w, screen_h,
					pt.x, pt.y, coord_hwnd, render_hwnd);
			}

			if (ScreenToClient(coord_hwnd, &pt))
			{
				// DIAG: log client-space position
				if (diag_should_log_move)
				{
					Console.WriteLn("(DIAG:RawInput) mouse[%u] ptr=%u ScreenToClient OK → client=(%ld,%ld)",
						mouse_idx, pointer_index, pt.x, pt.y);
					s_diag_move_count[pointer_index]++;
				}

				InputManager::UpdatePointerAbsolutePosition(
					pointer_index,
					static_cast<float>(pt.x),
					static_cast<float>(pt.y));
			}
			else if (diag_should_log_move)
			{
				// DIAG: log ScreenToClient failure
				Console.Warning("(DIAG:RawInput) mouse[%u] ptr=%u ScreenToClient FAILED (hwnd=%p err=%u)",
					mouse_idx, pointer_index, coord_hwnd, GetLastError());
				s_diag_move_count[pointer_index]++;
			}
		}
	}
	else if (diag_should_log_move)
	{
		// DIAG: log relative mouse event (lightgun should never be here)
		Console.WriteLn("(DIAG:RawInput) mouse[%u] ptr=%u RELATIVE delta=(%ld,%ld) flags=0x%04X — IGNORED (not absolute)",
			mouse_idx, pointer_index, rm.lLastX, rm.lLastY, rm.usFlags);
		s_diag_move_count[pointer_index]++;
	}

	// DIAG: log button events
	static constexpr struct
	{
		USHORT down_flag;
		USHORT up_flag;
		u32 button_index;
	} button_map[] = {
		{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0},
		{RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 1},
		{RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2},
	};

	for (const auto& bm : button_map)
	{
		if (rm.usButtonFlags & bm.down_flag)
		{
			// DIAG: log button press
			Console.WriteLn("(DIAG:RawInput) mouse[%u] ptr=%u button %u DOWN", mouse_idx, pointer_index, bm.button_index);

			mouse.button_state |= (1u << bm.button_index);
			InputManager::InvokeEvents(
				MakeGenericControllerButtonKey(InputSourceType::RawInput, pointer_index, bm.button_index),
				1.0f, GenericInputBinding::Unknown);
		}
		else if (rm.usButtonFlags & bm.up_flag)
		{
			// DIAG: log button release
			Console.WriteLn("(DIAG:RawInput) mouse[%u] ptr=%u button %u UP", mouse_idx, pointer_index, bm.button_index);

			mouse.button_state &= ~(1u << bm.button_index);
			InputManager::InvokeEvents(
				MakeGenericControllerButtonKey(InputSourceType::RawInput, pointer_index, bm.button_index),
				0.0f, GenericInputBinding::Unknown);
		}
	}
}

#endif // _WIN32
