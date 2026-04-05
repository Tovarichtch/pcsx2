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

	settings_lock.unlock();
	AssignPointerIndices();
	settings_lock.lock();
	RebuildHandleMap();

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

static std::string GetDisplayNameForDevice(const std::string& device_path, const std::wstring& wdevice_path, u32 index)
{
	// Try to get the real USB product name via HID API.
	std::string product_name;
	if (!wdevice_path.empty())
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

	if (!product_name.empty())
		return fmt::format("{} (Mouse {})", product_name, index);

	// Fall back to VID/PID from device path.
	std::string vid, pid;
	const size_t vid_pos = device_path.find("VID_");
	const size_t pid_pos = device_path.find("PID_");
	if (vid_pos != std::string::npos && vid_pos + 8 <= device_path.size())
		vid = device_path.substr(vid_pos + 4, 4);
	if (pid_pos != std::string::npos && pid_pos + 8 <= device_path.size())
		pid = device_path.substr(pid_pos + 4, 4);

	if (!vid.empty() && !pid.empty())
		return fmt::format("Mouse {} (VID:{} PID:{})", index, vid, pid);

	return fmt::format("Mouse {}", index);
}

static std::string ExtractVidPid(const std::string& device_path)
{
	// Extract "VID_XXXX&PID_XXXX" from a HID device path for port-independent matching.
	const size_t vid_pos = device_path.find("VID_");
	const size_t pid_pos = device_path.find("PID_");
	if (vid_pos == std::string::npos || pid_pos == std::string::npos)
		return {};
	const std::string vid = (vid_pos + 8 <= device_path.size()) ? device_path.substr(vid_pos, 8) : "";
	const std::string pid = (pid_pos + 8 <= device_path.size()) ? device_path.substr(pid_pos, 8) : "";
	if (vid.empty() || pid.empty())
		return {};
	return vid + "&" + pid; // "VID_XXXX&PID_XXXX"
}

bool RawInputSource::ReloadDevices()
{
	// Re-enumerate raw mice. Match new HANDLEs to existing device_paths
	// so that pointer_index stays stable mid-session.

	UINT device_count = 0;
	if (GetRawInputDeviceList(nullptr, &device_count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
		return false;

	std::vector<RAWINPUTDEVICELIST> device_list(device_count);
	if (GetRawInputDeviceList(device_list.data(), &device_count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
		return false;

	// Build a map of VID+PID → new HANDLE+path from the fresh enumeration.
	std::unordered_map<std::string, std::pair<HANDLE, std::string>> new_vidpid_to_device;
	for (UINT i = 0; i < device_count; i++)
	{
		if (device_list[i].dwType != RIM_TYPEMOUSE)
			continue;

		UINT name_size = 0;
		GetRawInputDeviceInfoW(device_list[i].hDevice, RIDI_DEVICENAME, nullptr, &name_size);
		if (name_size == 0)
			continue;

		std::wstring wpath(name_size, L'\0');
		if (GetRawInputDeviceInfoW(device_list[i].hDevice, RIDI_DEVICENAME, wpath.data(), &name_size) == static_cast<UINT>(-1))
			continue;

		while (!wpath.empty() && wpath.back() == L'\0')
			wpath.pop_back();

		std::string path = StringUtil::WideStringToUTF8String(wpath);
		if (!path.empty())
		{
			const std::string vidpid = ExtractVidPid(path);
			if (!vidpid.empty())
				new_vidpid_to_device[vidpid] = {device_list[i].hDevice, path};
		}
	}

	// Update existing mice: match by VID+PID, refresh HANDLE, detect removed devices.
	bool changed = false;
	for (auto it = m_mice.begin(); it != m_mice.end();)
	{
		const std::string vidpid = ExtractVidPid(it->device_path);
		auto vidpid_it = (!vidpid.empty()) ? new_vidpid_to_device.find(vidpid) : new_vidpid_to_device.end();
		if (vidpid_it != new_vidpid_to_device.end())
		{
			// Device still present (same VID+PID) — update HANDLE and path.
			if (it->handle != vidpid_it->second.first)
			{
				it->handle = vidpid_it->second.first;
				it->device_path = vidpid_it->second.second;
				Console.WriteLn("(RawInput) Updated handle for pointer %u (%s).", it->pointer_index, it->display_name.c_str());
			}
			new_vidpid_to_device.erase(vidpid_it); // consumed
			++it;
		}
		else
		{
			// Device removed.
			Console.WriteLn("(RawInput) Device removed: pointer %u (%s).", it->pointer_index, it->display_name.c_str());
			const InputBindingKey key = MakeGenericControllerButtonKey(InputSourceType::RawInput, it->pointer_index, 0);
			InputManager::OnInputDeviceDisconnected(key, GetDeviceIdentifier(it->pointer_index));
			it = m_mice.erase(it);
			changed = true;
		}
	}

	// Any remaining entries are newly connected devices.
	for (const auto& [vidpid, device_info] : new_vidpid_to_device)
	{
		const auto& [handle, path] = device_info;
		if (m_mice.size() >= InputManager::MAX_POINTER_DEVICES)
			break;

		// Check ini for a saved pointer index for this VID+PID.
		// This restores the assignment after a full disconnect/reconnect,
		// even when m_mice was empty and order of reconnection differs.
		u32 preferred_slot = InputManager::MAX_POINTER_DEVICES; // sentinel = not found
		for (u32 slot = 0; slot < InputManager::MAX_POINTER_DEVICES; slot++)
		{
			const std::string key = fmt::format("Pointer{}Device", slot);
			const std::string stored = Host::GetBaseStringSettingValue("RawInput", key.c_str(), "");
			if (stored == vidpid)
			{
				preferred_slot = slot;
				break;
			}
		}

		// Use saved slot if free, otherwise fall back to first free slot.
		u32 taken_mask = 0;
		for (const auto& m : m_mice)
			if (m.pointer_index < InputManager::MAX_POINTER_DEVICES)
				taken_mask |= (1u << m.pointer_index);

		u32 assigned_slot;
		if (preferred_slot < InputManager::MAX_POINTER_DEVICES &&
			!(taken_mask & (1u << preferred_slot)))
		{
			assigned_slot = preferred_slot;
			Console.WriteLn("(RawInput) Restored pointer %u for %s (VID+PID match).", assigned_slot, vidpid.c_str());
		}
		else
		{
			assigned_slot = static_cast<u32>(std::countr_zero(~taken_mask));
			if (assigned_slot >= InputManager::MAX_POINTER_DEVICES)
				break;
		}

		// Get display name.
		std::wstring wpath;
		{
			const std::wstring tmp = StringUtil::UTF8StringToWideString(path);
			wpath = tmp;
		}
		std::string display_name = GetDisplayNameForDevice(path, wpath, assigned_slot);

		RawMouseDevice dev;
		dev.handle = handle;
		dev.device_path = path;
		dev.display_name = std::move(display_name);
		dev.pointer_index = assigned_slot;
		dev.button_state = 0;
		dev.seen_absolute = false;

		Console.WriteLn("(RawInput) New device: pointer %u (%s).", dev.pointer_index, dev.display_name.c_str());
		InputManager::OnInputDeviceConnected(GetDeviceIdentifier(dev.pointer_index), dev.display_name);
		m_mice.push_back(std::move(dev));
		changed = true;
	}

	RebuildHandleMap();
	return changed;
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

// ========================================================================
// Device enumeration + persistence
// ========================================================================


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

	for (UINT i = 0; i < device_count; i++)
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
				while (!wdevice_path.empty() && wdevice_path.back() == L'\0')
					wdevice_path.pop_back();
				device_path = StringUtil::WideStringToUTF8String(wdevice_path);
			}
		}

		if (device_path.empty())
			continue;

		if (m_mice.size() >= InputManager::MAX_POINTER_DEVICES)
			break;

		std::string display_name = GetDisplayNameForDevice(device_path, wdevice_path, static_cast<u32>(m_mice.size()));

		RawMouseDevice dev;
		dev.handle = handle;
		dev.device_path = std::move(device_path);
		dev.display_name = std::move(display_name);
		dev.pointer_index = 0; // assigned later by AssignPointerIndices
		dev.button_state = 0;
		dev.seen_absolute = false;

		m_mice.push_back(std::move(dev));
	}

	return true;
}

void RawInputSource::AssignPointerIndices()
{
	// Called from Initialize(), which is called from UpdateInputSourceState()
	// → ReloadSources() → always on the CPU thread. Host settings access is safe here.

	if (m_mice.empty())
		return;

	// Read stored VID+PID → pointer_index from ini.
	std::string stored_paths[InputManager::MAX_POINTER_DEVICES];
	bool has_stored = false;

	for (u32 slot = 0; slot < InputManager::MAX_POINTER_DEVICES; slot++)
	{
		const std::string key = fmt::format("Pointer{}Device", slot);
		stored_paths[slot] = Host::GetBaseStringSettingValue("RawInput", key.c_str(), "");
		if (!stored_paths[slot].empty())
			has_stored = true;
	}

	if (has_stored)
	{
		std::vector<bool> slot_taken(InputManager::MAX_POINTER_DEVICES, false);
		std::vector<bool> mouse_assigned(m_mice.size(), false);

		// First pass: VID+PID match.
		for (u32 slot = 0; slot < InputManager::MAX_POINTER_DEVICES; slot++)
		{
			if (stored_paths[slot].empty())
				continue;

			for (size_t m = 0; m < m_mice.size(); m++)
			{
				const std::string mouse_vidpid = ExtractVidPid(m_mice[m].device_path);
				if (!mouse_assigned[m] && !mouse_vidpid.empty() && mouse_vidpid == stored_paths[slot])
				{
					m_mice[m].pointer_index = slot;
					slot_taken[slot] = true;
					mouse_assigned[m] = true;
					Console.WriteLn("(RawInput) Matched stored VID+PID → pointer %u: %s", slot, m_mice[m].display_name.c_str());
					break;
				}
			}
		}

		// Second pass: unmatched mice get remaining free slots.
		u32 next_free = 0;
		for (size_t m = 0; m < m_mice.size(); m++)
		{
			if (mouse_assigned[m])
				continue;

			while (next_free < InputManager::MAX_POINTER_DEVICES && slot_taken[next_free])
				next_free++;

			if (next_free >= InputManager::MAX_POINTER_DEVICES)
			{
				Console.Warning("(RawInput) No free pointer slot for %s.", m_mice[m].display_name.c_str());
				continue;
			}

			m_mice[m].pointer_index = next_free;
			slot_taken[next_free] = true;
			mouse_assigned[m] = true;
			Console.WriteLn("(RawInput) Auto-assigned %s → pointer %u (no stored match).", m_mice[m].display_name.c_str(), next_free);
		}

		// Update display names to include the final pointer index.
		for (auto& mouse : m_mice)
		{
			// Regenerate display name with correct index.
			const std::wstring wpath = StringUtil::UTF8StringToWideString(mouse.device_path);
			mouse.display_name = GetDisplayNameForDevice(mouse.device_path, wpath, mouse.pointer_index);
		}
	}
	else
	{
		// No stored settings: enumerate order = pointer order.
		// Cap at MAX_POINTER_DEVICES.
		u32 idx = 0;
		for (auto& mouse : m_mice)
		{
			if (idx >= InputManager::MAX_POINTER_DEVICES)
				break;
			mouse.pointer_index = idx++;
		}

		// Save to ini so it persists across reboots.
		Console.WriteLn("(RawInput) No stored assignments, saving current order.");
		for (const auto& mouse : m_mice)
		{
			if (mouse.pointer_index >= InputManager::MAX_POINTER_DEVICES)
				continue;
			const std::string key = fmt::format("Pointer{}Device", mouse.pointer_index);
			const std::string vidpid = ExtractVidPid(mouse.device_path);
			if (!vidpid.empty())
				Host::SetBaseStringSettingValue("RawInput", key.c_str(), vidpid.c_str());
		}
		Host::CommitBaseSettingChanges();
	}
}

void RawInputSource::RebuildHandleMap()
{
	m_handle_to_mouse_index.clear();
	for (u32 i = 0; i < static_cast<u32>(m_mice.size()); i++)
		m_handle_to_mouse_index[m_mice[i].handle] = i;
}

// ========================================================================
// Queries
// ========================================================================

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

// ========================================================================
// Event processing
// ========================================================================

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

	// Position — absolute devices only (lightguns).
	if (rm.usFlags & MOUSE_MOVE_ABSOLUTE)
	{
		mouse.seen_absolute = true;

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

			if (ScreenToClient(coord_hwnd, &pt))
			{
				InputManager::UpdatePointerAbsolutePosition(
					pointer_index,
					static_cast<float>(pt.x),
					static_cast<float>(pt.y));
			}
		}
	}
	// Relative mice: position silently ignored. They use the Qt pointer path.

	// Buttons — always processed for all devices.
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
			mouse.button_state |= (1u << bm.button_index);
			InputManager::InvokeEvents(
				MakeGenericControllerButtonKey(InputSourceType::RawInput, pointer_index, bm.button_index),
				1.0f, GenericInputBinding::Unknown);
		}
		else if (rm.usButtonFlags & bm.up_flag)
		{
			mouse.button_state &= ~(1u << bm.button_index);
			InputManager::InvokeEvents(
				MakeGenericControllerButtonKey(InputSourceType::RawInput, pointer_index, bm.button_index),
				0.0f, GenericInputBinding::Unknown);
		}
	}
}

#endif // _WIN32
