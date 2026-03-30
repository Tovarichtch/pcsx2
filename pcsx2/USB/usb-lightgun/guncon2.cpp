// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GS.h"
#include "Host.h"
#include "IconsPromptFont.h"
#include "ImGui/ImGuiManager.h"
#include "Input/InputManager.h"
#include "Memory.h"
#include "StateWrapper.h"
#include "USB/USB.h"
#include "USB/deviceproxy.h"
#include "USB/qemu-usb/USBinternal.h"
#include "USB/qemu-usb/desc.h"
#include "USB/usb-lightgun/guncon2.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <chrono>
#include <tuple>

namespace usb_lightgun
{
	enum : u32
	{
		GUNCON2_FLAG_PROGRESSIVE = 0x0100,

		GUNCON2_CALIBRATION_DELAY = 12,
		GUNCON2_CALIBRATION_REPORT_DELAY = 5,
	};

	enum : u32
	{
		BID_C = 1,
		BID_B = 2,
		BID_A = 3,
		BID_DPAD_UP = 4,
		BID_DPAD_RIGHT = 5,
		BID_DPAD_DOWN = 6,
		BID_DPAD_LEFT = 7,
		BID_TRIGGER = 13,
		BID_SELECT = 14,
		BID_START = 15,
		BID_SHOOT_OFFSCREEN = 16,
		BID_RECALIBRATE = 17,
		BID_RELATIVE_LEFT = 18,
		BID_RELATIVE_RIGHT = 19,
		BID_RELATIVE_UP = 20,
		BID_RELATIVE_DOWN = 21,
	};

	// Button the player presses to confirm calibration is done.
	enum CalibDoneBtn : u8
	{
		CALIB_BTN_NONE  = 0, // No button lock (VC: uses settle timer; GF2: no_photodiode)
		CALIB_BTN_AB    = 1, // A or B on GunCon2 (Namco, DCOX, GC2)
		CALIB_BTN_START = 2, // START on GunCon2 (Capcom DS, GS, REDA)
		CALIB_BTN_OFF   = 3, // Offscreen shot
	};

	// Right pain in the arse. Different games seem to have different scales..
	// Not worth putting these in the gamedb for such few games.
	// Values are from the old nuvee plugin.
	struct GameConfig
	{
		const char* serial;
		float scale_x, scale_y;
		u32 center_x, center_y;
		u32 screen_width, screen_height;
		bool force_2point; // Auto-inject Trigger+C offscreen at boot for 2-Point Adjustment (TC3).
		u32 dark_threshold; // Per-game photodiode dark entry threshold (0 = default 44). Exit = threshold * 2.
		bool no_photodiode; // Lock dark=false from boot — game calibrates without photodiode.
		CalibDoneBtn calib_done_btn; // Button that locks calibration (NONE = settle timer for VC).
		u32 dark_delay;    // Trigger-delayed dark injection: microseconds before injecting dark (0 = use photodiode).
		u32 dark_duration; // Microseconds to hold dark after delay (0 = use photodiode).
		bool fire_once;    // true = dark inject only during calibration. false = every shot (VC).
	};

	static constexpr const GameConfig s_game_config[] = {
		// ELF-based calibration: cx/cy from addiu opcodes, W/H from slti thresholds.
		// screen_height = ELF height / 2 (interlaced half-field).
		// scale_x/scale_y calibrated empirically per game by Réda (Tovarichtch).
		// dark_delay/dark_duration in MICROSECONDS (std::chrono::steady_clock).
		// fire_once: true = calibration only, false = every shot (Sega VC).
		// calib_done_btn: AB/START/OFF/NONE — button that confirms calibration is done.
		//                                       sx       sy     cx   cy    w    h   2pt  dark nopd  done_btn        dly    dur    f1
		{"SLPM-62401",  89.75f, 113.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,        0,     0, false}, // Death Crimson OX+ (J) NTSC vanilla
		{"SLES-50930",  89.5f,  103.0f,  422, 134, 512, 224, false, 0, false, CALIB_BTN_START,      0,     0, false}, // Dino Stalker (E, En) PAL Capcom vanilla
		{"SLES-51095",  89.5f,  103.0f,  422, 134, 512, 224, false, 0, false, CALIB_BTN_START,      0,     0, false}, // Dino Stalker (E, Fr) PAL Capcom vanilla
		{"SLES-51096",  89.5f,  103.0f,  422, 134, 512, 224, false, 0, false, CALIB_BTN_START,      0,     0, false}, // Dino Stalker (E, De) PAL Capcom vanilla
		{"SLUS-20485",  89.5f,  103.0f,  422, 134, 512, 224, false, 0, false, CALIB_BTN_START,      0,     0, false}, // Dino Stalker (U) NTSC Capcom vanilla
		{"SLUS-20389",  89.25f,  93.5f,  422, 134, 640, 240, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Endgame (U) NTSC (untested)
		{"SLES-50936", 112.0f,  100.0f,  320, 120, 512, 256, false, 0, false, CALIB_BTN_AB,    60000, 20000, true},  // Endgame (E) PAL (untested)
		{"SLPM-65060", 100.0f,  101.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_START, 67000, 17000, true},  // Gun Survivor 2 (J) NTSC Capcom thr=12 f4
		{"SLPM-65139", 100.0f,  100.0f,  422, 134, 512, 224, false, 0, false, CALIB_BTN_START,      0,     0, false}, // Gun Survivor 3 (J) NTSC Capcom vanilla
		{"SLPM-67529", 100.0f,  100.0f,  422, 134, 512, 224, false, 0, false, CALIB_BTN_START,      0,     0, false}, // Gun Survivor 3 (KR) NTSC Capcom vanilla
		{"SLPM-65245", 100.0f,  101.25f, 422, 134, 640, 224, false, 0, false, CALIB_BTN_START, 50000, 17000, true},  // Gun Survivor 4 (J) NTSC Capcom
		{"SLES-52620",  89.75f, 112.0f,  422, 148, 640, 256, false, 0, false, CALIB_BTN_AB,        0,     0, false}, // Guncom 2 (E) PAL vanilla
		{"SLES-51289", 105.0f,   88.0f,  422, 164, 512, 256, false, 0, true,  CALIB_BTN_NONE,  60000, 20000, true},  // Gunfighter II (E) PAL no_photodiode
		{"SLPS-25165",  90.0f,  105.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Gunvari Collection (J) NTSC Namco
		{"SCES-50889",  90.0f,   97.5f,  422, 169, 640, 240, false, 0, false, CALIB_BTN_AB,    60000, 20000, true},  // Ninja Assault (E) PAL Namco
		{"SLPS-20218",  90.0f,   92.0f,  422, 134, 640, 240, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Ninja Assault (J) NTSC Namco
		{"SCPS-56015",  90.0f,   92.0f,  422, 134, 640, 240, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Ninja Assault (KR) NTSC Namco
		{"SLUS-20492",  90.0f,   92.0f,  422, 134, 640, 240, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Ninja Assault (U) NTSC Namco
		{"SLES-51448",  90.25f, 108.0f,  422, 134, 640, 225, false, 0, false, CALIB_BTN_START, 60000, 20000, true},  // RE Dead Aim (E) PAL
		{"SLUS-20669",  90.5f,  114.0f,  422, 134, 640, 240, false, 0, false, CALIB_BTN_START, 50000, 17000, true},  // RE Dead Aim (U) NTSC
		{"SLES-50650", 100.0f,  100.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_START, 80000, 20000, true},  // RE Survivor 2 (E) PAL Capcom thr=12 f4
		{"SLES-51617",  90.0f,   82.5f,  422, 134, 640, 256, false, 0, false, CALIB_BTN_AB,    40000, 20000, true},  // Starsky & Hutch (E, En) PAL thr=14 f2
		{"SLES-51783",  90.0f,   82.5f,  422, 134, 640, 256, false, 0, false, CALIB_BTN_AB,    40000, 20000, true},  // Starsky & Hutch (E, Fr/De) PAL thr=14 f2
		{"SLKA-25090",  90.0f,  104.5f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    33000, 17000, true},  // Starsky & Hutch (KR) NTSC thr=14 f2
		{"SLUS-20619",  90.0f,  104.5f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    33000, 17000, true},  // Starsky & Hutch (U) NTSC thr=14 f2
		{"SCES-50300",  90.0f,  103.0f,  437, 164, 640, 256, false, 0, false, CALIB_BTN_AB,    60000, 20000, true},  // Time Crisis II (E) PAL Namco dist_8101
		{"SLPS-20122",  89.75f, 104.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Time Crisis II (J) NTSC Namco dist_8101
		{"SCKA-20002",  89.75f, 104.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Time Crisis II (KR) NTSC Namco dist_8101
		{"SLUS-20219",  89.75f, 104.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Time Crisis II (U) NTSC Namco dist_8101
		{"SCAJ-20060",  89.75f, 104.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Time Crisis 3 (Asia) NTSC Namco dist_8101
		{"SCES-51844",  90.0f,  103.0f,  437, 164, 640, 256, false, 0, false, CALIB_BTN_AB,    60000, 20000, true},  // Time Crisis 3 (E) PAL Namco dist_8101
		{"SLPS-25290",  89.75f, 104.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Time Crisis 3 (J) NTSC Namco dist_8101
		{"SCKA-20015",  89.75f, 104.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Time Crisis 3 (KR) NTSC Namco dist_8101
		{"SLUS-20645",  89.75f, 104.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Time Crisis 3 (U) NTSC Namco dist_8101
		{"SCES-52530",  90.0f,  103.0f,  422, 153, 640, 256, false, 0, false, CALIB_BTN_AB,    60000, 20000, true},  // Crisis Zone (E) PAL Namco
		{"SCKA-20038",  90.0f,  104.5f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Crisis Zone (KR) NTSC Namco
		{"SLUS-20927",  90.0f,  104.5f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Crisis Zone (U) NTSC Namco VERIFIED
		{"SCES-50411",  89.75f, 115.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    60000, 20000, true},  // Vampire Night (E) PAL Namco
		{"SLPS-25077",  89.75f, 105.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Vampire Night (J) NTSC Namco
		{"SLUS-20221",  89.75f, 105.0f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_AB,    50000, 17000, true},  // Vampire Night (U) NTSC Namco
		{"SLES-51229", 111.0f,  100.0f,  424, 134, 512, 256, false, 0, false, CALIB_BTN_NONE,  56000, 20000, false}, // Virtua Cop Elite Edition (E) PAL Sega
		{"SLPM-62205",  89.75f, 104.5f,  422, 134, 640, 224, false, 0, false, CALIB_BTN_NONE,  64000, 17000, false}, // Virtua Cop Re-Birth (J) NTSC Sega
	};

	static constexpr s32 DEFAULT_SCREEN_WIDTH = 640;
	static constexpr s32 DEFAULT_SCREEN_HEIGHT = 240;
	static constexpr float DEFAULT_CENTER_X = 320.0f;
	static constexpr float DEFAULT_CENTER_Y = 120.0f;
	static constexpr float DEFAULT_SCALE_X = 100.0f;
	static constexpr float DEFAULT_SCALE_Y = 100.0f;

#pragma pack(push, 1)
	union GunCon2Out
	{
		u8 bits[6];

		struct
		{
			u16 buttons;
			s16 pos_x;
			s16 pos_y;
		};
	};
	static_assert(sizeof(GunCon2Out) == 6);
#pragma pack(pop)

	struct GunCon2State
	{
		explicit GunCon2State(u32 port_);
		~GunCon2State();

		USBDevice dev{};
		USBDesc desc{};
		USBDescDevice desc_dev{};

		u32 port = 0;

		//////////////////////////////////////////////////////////////////////////
		// Configuration
		//////////////////////////////////////////////////////////////////////////
		bool has_relative_binds = false;
		bool custom_config = false;
		u32 screen_width = 640;
		u32 screen_height = 240;
		float center_x = 320;
		float center_y = 120;
		float scale_x = 1.0f;
		float scale_y = 1.0f;

		//////////////////////////////////////////////////////////////////////////
		// Host State (Not Saved)
		//////////////////////////////////////////////////////////////////////////
		u32 button_state = 0;
		u32 pointer_index = 0; // which pointer device to read position from
		std::string cursor_path;
		float cursor_scale = 1.0f;
		u32 cursor_color = 0xFFFFFFFF;
		float relative_pos[4] = {};

		//////////////////////////////////////////////////////////////////////////
		// Device State (Saved)
		//////////////////////////////////////////////////////////////////////////
		s16 param_x = 0;
		s16 param_y = 0;
		u16 param_mode = 0;

		u16 calibration_timer = 0;
		s16 calibration_pos_x = 0;
		s16 calibration_pos_y = 0;

		// TC3 progressive scan: auto-inject Trigger+C offscreen to force 2-Point mode.
		bool needs_2point_init = false;
		u32 init_2point_state = 0;   // 0=wait dark, 1=in dark, 2=injecting
		u32 init_2point_polls = 0;

		// Calibration lock: dark_inject runs on every trigger until locked.
		// Lock triggered by calib_done_btn press after game has responded to calibration.
		// has_triggered: set on first trigger press (gates boot-time SET_PARAMs).
		// calib_responded: set when SET_PARAM received while has_triggered=true.
		// Lock condition: has_triggered && calib_responded && done_button pressed.
		bool calibration_locked = false;
		bool calibration_pending = false; // For settle-timer path (VC only).
		u32 pending_poll_count = 0; // polls since last SET_PARAM while pending (VC only).
		bool photodiode_disabled = false; // Permanent lock for no_photodiode games — never unlocks.
		CalibDoneBtn calib_done_btn = CALIB_BTN_NONE;
		bool has_triggered = false;    // Player has pressed trigger at least once.
		bool calib_responded = false;  // Game sent SET_PARAM after has_triggered.

		// Trigger-delayed dark injection: replaces photodiode for games with configured dark_delay.
		// Real GunCon2 on CRT: photodiode detects dark within the same vsync.
		// Our ring buffer: dark arrives ~13 polls later (too late for CZ, latency for VC).
		// Fix: on trigger, wait dark_delay polls then inject (0,0) for dark_duration polls.
		u16 dark_inject_countdown = 0;
		u16 dark_inject_active = 0;
		bool dark_inject_fired = false; // Edge detect: only inject once per trigger press.
		u32 dark_delay = 0;    // 0 = use photodiode (default). Microseconds. Set from GameConfig.
		u32 dark_duration = 0; // Microseconds. Set from GameConfig.
		bool fire_once = false; // true = calibration-on-trigger mode (CZ). false = dark inject every shot (VC).

		// fire_once calibration: vanilla-style mechanism triggered by first trigger.
		// Force trigger down, send stored position during delay µs, then send (0,0)
		// for duration µs. Both measured via steady_clock. No memRead/memWrite.
		bool calibration_active = false;   // true = currently in calibration sequence
		std::chrono::steady_clock::time_point calibration_start_time; // sub-poll precision trigger timestamp

		static constexpr u32 LOCK_SETTLE_POLLS = 750; // ~6 seconds at 125Hz USB poll rate — no SET_PARAM activity = calibration done

		// DIAG: comprehensive state tracking for debugging
		bool diag_trigger_was_down = false;
		u32 diag_poll_count = 0;
		bool diag_is_cz_us = false; // Gate CZ-specific memRead32 on SLUS-20927 addresses
		u32 diag_last_btn_proc = 0; // Track btn_proc changes for shoot logging
		u32 diag_last_fire_tmr = 0; // Track fire_tmr changes for shoot logging

		bool auto_config_done = false;

		void AutoConfigure();

		std::tuple<s16, s16> CalculatePosition();

		// 0..1, not -1..1.
		std::pair<float, float> GetAbsolutePositionFromRelativeAxes() const;
		u32 GetSoftwarePointerIndex() const;
		void UpdateSoftwarePointerPosition();
	};

	static const USBDescStrings desc_strings = {
		"Namco GunCon2",
	};

	/* mostly the same values as the Bochs USB Keyboard device */
	static const uint8_t guncon2_dev_desc[] = {
		/* bLength             */ 0x12,
		/* bDescriptorType     */ 0x01,
		/* bcdUSB              */ WBVAL(0x0100),
		/* bDeviceClass        */ 0x00,
		/* bDeviceSubClass     */ 0x00,
		/* bDeviceProtocol     */ 0x00,
		/* bMaxPacketSize0     */ 0x08,
		/* idVendor            */ WBVAL(0x0b9a),
		/* idProduct           */ WBVAL(0x016a),
		/* bcdDevice           */ WBVAL(0x0100),
		/* iManufacturer       */ 0x00,
		/* iProduct            */ 0x00,
		/* iSerialNumber       */ 0x00,
		/* bNumConfigurations  */ 0x01,
	};

	static const uint8_t guncon2_config_desc[] = {
		0x09, // Length
		0x02, // Type (Config)
		0x19, 0x00, // Total size

		0x01, // # interfaces
		0x01, // Configuration #
		0x00, // index of string descriptor
		0x80, // Attributes (bus powered)
		0x19, // Max power in mA


		// Interface
		0x09, // Length
		0x04, // Type (Interface)

		0x00, // Interface #
		0x00, // Alternative #
		0x01, // # endpoints

		0xff, // Class
		0x6a, // Subclass
		0x00, // Protocol
		0x00, // index of string descriptor


		// Endpoint
		0x07, // Length
		0x05, // Type (Endpoint)

		0x81, // Address
		0x03, // Attributes (interrupt transfers)
		0x08, 0x00, // Max packet size

		0x08, // Polling interval (frame counts)
	};

	static void guncon2_handle_control(
		USBDevice* dev, USBPacket* p, int request, int value, int index, int length, uint8_t* data)
	{
		GunCon2State* const us = USB_CONTAINER_OF(dev, GunCon2State, dev);

		// Apply per-game configuration on the first control packet.
		// Always runs to apply features (lock, threshold, no_photodiode) by serial.
		// Position values (scale, center, screen) are skipped if custom_config is set.
		if (!us->auto_config_done)
		{
			us->AutoConfigure();
			us->auto_config_done = true;
		}

		if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0)
			return;

		if (request == (ClassInterfaceOutRequest | 0x09))
		{
			const s16 old_px = us->param_x;
			const s16 old_py = us->param_y;
			const u16 old_mode = us->param_mode;
			us->param_x = static_cast<u16>(data[0]) | (static_cast<u16>(data[1]) << 8);
			us->param_y = static_cast<u16>(data[2]) | (static_cast<u16>(data[3]) << 8);
			us->param_mode = static_cast<u16>(data[4]) | (static_cast<u16>(data[5]) << 8);
			// Log SET_PARAM — game writing calibration offsets.
			Console.WriteLn("(GunCon2) Port %u SET_PARAM mode=0x%04X param_x=%d param_y=%d (was mode=0x%04X x=%d y=%d)",
				us->port, us->param_mode, us->param_x, us->param_y,
				old_mode, old_px, old_py);

			// Calibration lock logic in SET_PARAM:
			// - Button-based (calib_done_btn != NONE): just mark calib_responded.
			//   Lock happens in poll handler when the done button is pressed.
			// - Settle-timer (VC, calib_done_btn == NONE, fire_once=false): PENDING → LOCKED.
			// - no_photodiode (GF2): already locked at boot, ignore all SET_PARAMs.
			if (us->calib_done_btn != CALIB_BTN_NONE)
			{
				// Button-based games: mark that the game responded to calibration.
				if (us->has_triggered && !us->calibration_locked)
				{
					if (!us->calib_responded)
					{
						us->calib_responded = true;
						Console.WriteLn("(GunCon2) Port %u: game responded to calibration (param: %d,%d) — waiting for done button",
							us->port, us->param_x, us->param_y);
					}
				}
			}
			else if (!us->photodiode_disabled)
			{
				// VC (settle timer): PENDING on SET_PARAM, LOCKED after idle.
				if (us->calibration_locked)
				{
					us->calibration_locked = false;
					us->calibration_pending = true;
					us->pending_poll_count = 0;
					Console.WriteLn("(GunCon2) Port %u: calibration UNLOCKED (SET_PARAM received, re-calibrating)",
						us->port);
				}
				else if (!us->calibration_pending)
				{
					us->calibration_pending = true;
					us->pending_poll_count = 0;
					Console.WriteLn("(GunCon2) Port %u: calibration PENDING (SET_PARAM received, param_x=%d)",
						us->port, us->param_x);
				}
				else
				{
					us->pending_poll_count = 0;
				}
			}
			// photodiode_disabled (GF2): already locked at boot, ignore all SET_PARAMs.

			return;
		}

		p->status = USB_RET_STALL;
	}

	static void guncon2_handle_data(USBDevice* dev, USBPacket* p)
	{
		GunCon2State* const us = USB_CONTAINER_OF(dev, GunCon2State, dev);

		switch (p->pid)
		{
			case USB_TOKEN_IN:
			{
				if (p->ep->nr == 1)
				{
					const auto [pos_x, pos_y] = us->CalculatePosition();

					// GunCon2 calibration: games blank the screen for a few
					// frames, expecting the photodiode to report (0,0) when
					// it sees no light. The game then computes aiming offsets
					// from the first non-zero position after the blank.
					// We detect darkness via GPU pixel sampling in Merge().

					// Buttons are active low.
					// Buttons are active low. Bit 8 (GUNCON2_FLAG_PROGRESSIVE) is left
					// at its natural ~button_state value (always 1, since no BID maps to
					// bit 8). This matches PCSX2 vanilla behavior and allows the game's
					// FUNC_A progressive detection to run its normal course at boot.
					// Photodiode simulation for step 2 brightness is deferred — step 1
					// calibration (dark_inject timing) handles all shooting calibration.
					const bool dark = g_guncon2_display_dark.load(std::memory_order_relaxed);
					GunCon2Out out;
					out.buttons = static_cast<u16>(~us->button_state);
					out.pos_x = pos_x;
					out.pos_y = pos_y;

					if (us->button_state & (1u << BID_SHOOT_OFFSCREEN))
					{
						out.buttons &= ~(1u << BID_TRIGGER);
						out.pos_x = 0;
						out.pos_y = 0;
					}

					if (us->fire_once && us->dark_delay > 0)
					{
						// fire_once calibration (CZ): trigger-activated, full sub-poll precision.
						// On first trigger: store position and timestamp. Force trigger down.
						// Send stored pos until dark_delay µs elapsed (50ms NTSC, 60ms PAL).
						// Then send (0,0) until dark_delay+dark_duration µs elapsed (8ms = 1 frame).
						// Both delay and duration use steady_clock — zero poll granularity.
						// After calibration_locked: do nothing — trigger = normal shot.
						if (!us->calibration_locked)
						{
							// Start calibration on trigger press edge
							if ((us->button_state & (1u << BID_TRIGGER)) && !us->calibration_active && !us->dark_inject_fired)
							{
								if (!us->has_triggered)
								{
									us->has_triggered = true;
									Console.WriteLn("(GunCon2) Port %u: first trigger — params (%d,%d)",
										us->port, us->param_x, us->param_y);
								}
								us->calibration_active = true;
								us->calibration_start_time = std::chrono::steady_clock::now();
								us->calibration_pos_x = pos_x;
								us->calibration_pos_y = pos_y;
								us->dark_inject_fired = true;
								Console.WriteLn("(DIAG:FIRE) Port %u poll=%u: CALIBRATION STARTED (delay=%uus dur=%uus pos=(%d,%d))",
									us->port, us->diag_poll_count, us->dark_delay, us->dark_duration, pos_x, pos_y);
							}
							if (!us->calibration_active && !(us->button_state & (1u << BID_TRIGGER)))
								us->dark_inject_fired = false;

							if (us->calibration_active)
							{
								// Force trigger down during calibration sequence to ensure
								// the game sees trigger for the full flash cycle, even with
								// very fast clicks (< 67ms).
								out.buttons &= ~(1u << BID_TRIGGER);

								const auto elapsed = std::chrono::steady_clock::now() - us->calibration_start_time;
								const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
								const s64 dark_end = static_cast<s64>(us->dark_delay) + static_cast<s64>(us->dark_duration);

								if (elapsed_us < static_cast<s64>(us->dark_delay))
								{
									// Delay phase: send stored position (game accumulates)
									out.pos_x = us->calibration_pos_x;
									out.pos_y = us->calibration_pos_y;
								}
								else if (elapsed_us < dark_end)
								{
									// Dark phase: send (0,0)
									out.pos_x = 0;
									out.pos_y = 0;
								}
								else
								{
									// Done — sequence expired, return control to player.
									// Calibration sequence finished. Don't lock here — lock
									// happens when done button is pressed (calib_done_btn).
									// dark_inject_fired stays true until trigger is RELEASED
									// (line below), preventing double dark_inject from a
									// single trigger press (human press ≈ 80-150ms > 67ms sequence).
									us->calibration_active = false;
									Console.WriteLn("(DIAG:FIRE) Port %u poll=%u: CALIBRATION COMPLETE — NOT locked (elapsed=%lldus, dark_end=%lldus)",
										us->port, us->diag_poll_count, static_cast<long long>(elapsed_us), static_cast<long long>(dark_end));
								}

								if (us->calibration_active)
								{
									Console.WriteLn("(DIAG:CALIB) Port %u poll=%u | elapsed=%lldus phase=%s | out=(%d,%d)",
										us->port, us->diag_poll_count,
										static_cast<long long>(elapsed_us),
										(elapsed_us < static_cast<s64>(us->dark_delay)) ? "DELAY" : "DARK",
										out.pos_x, out.pos_y);
								}
							}
						}
						// After calibration_locked: trigger = normal. No dark, no forced trigger.
					}
					else if (us->dark_delay > 0 || us->dark_duration > 0)
					{
						// dark_inject every shot (VC): existing mechanism.
						// On each trigger, wait dark_delay µs (converted to polls) then
						// inject (0,0) for dark_duration µs (converted to polls).
						// VC needs dark_inject in gameplay too — no lock gate.
						if ((us->button_state & (1u << BID_TRIGGER)) && !us->dark_inject_fired &&
							us->dark_inject_countdown == 0 && us->dark_inject_active == 0)
						{
							if (us->dark_delay > 0)
								us->dark_inject_countdown = static_cast<u16>(us->dark_delay / 8000); // µs to polls
							else
								us->dark_inject_active = static_cast<u16>(us->dark_duration / 8000); // µs to polls
							us->dark_inject_fired = true;
						}
						if (!(us->button_state & (1u << BID_TRIGGER)))
							us->dark_inject_fired = false;

						if (us->dark_inject_countdown > 0)
						{
							us->dark_inject_countdown--;
							if (us->dark_inject_countdown == 0)
								us->dark_inject_active = static_cast<u16>(us->dark_duration / 8000); // µs to polls
						}

						if (us->dark_inject_active > 0)
						{
							out.pos_x = 0;
							out.pos_y = 0;
							us->dark_inject_active--;
						}
						else if (!us->calibration_locked && dark)
						{
							out.pos_x = 0;
							out.pos_y = 0;
						}
					}
					else
					{
						// Standard photodiode for games without dark_inject (NA, TC2, etc).
						// Before calibration lock: dark → pos=(0,0) for calibration to work.
						// After calibration lock: dark BLOCKED — prevents false darks.
						if (dark && !us->calibration_locked)
						{
							out.pos_x = 0;
							out.pos_y = 0;
						}
					}

					// Deferred lock: confirm PENDING → LOCKED after settle period.
					// Only for VC (calib_done_btn == NONE, fire_once == false).
					if (us->calibration_pending && !us->calibration_locked)
					{
						if (++us->pending_poll_count >= us->LOCK_SETTLE_POLLS)
						{
							us->calibration_locked = true;
							us->calibration_pending = false;
							Console.WriteLn("(GunCon2) Port %u: calibration LOCKED (settled after %u polls) — dark flag ignored",
								us->port, us->pending_poll_count);
						}
					}

					// Button-based lock: player presses done button after game responded.
					// has_triggered: player has fired at least once.
					// calib_responded: game sent SET_PARAM after trigger (calibration happened).
					// Both conditions prevent premature lock (boot buttons, logo skips).
					if (us->calib_done_btn != CALIB_BTN_NONE &&
						us->has_triggered && us->calib_responded && !us->calibration_locked)
					{
						bool done_pressed = false;
						switch (us->calib_done_btn)
						{
						case CALIB_BTN_AB:
							done_pressed = (us->button_state & ((1u << BID_A) | (1u << BID_B))) != 0;
							break;
						case CALIB_BTN_START:
							done_pressed = (us->button_state & (1u << BID_START)) != 0;
							break;
						case CALIB_BTN_OFF:
							done_pressed = (us->button_state & (1u << BID_SHOOT_OFFSCREEN)) != 0;
							break;
						default:
							break;
						}
						if (done_pressed)
						{
							us->calibration_locked = true;
							if (us->calibration_active)
								us->calibration_active = false;
							Console.WriteLn("(GunCon2) Port %u: calibration LOCKED [button] (param: %d,%d)",
								us->port, us->param_x, us->param_y);
						}
					}

					// TC3/NA 2-Point Adjustment: auto-inject Trigger+C offscreen.
					// These Namco games in progressive scan mode require Trigger+C
					// while aiming offscreen to enable 2-Point calibration. Without
					// it, the standard calibration path is dead (flash_enable=0).
					// We inject after every dark→light transition until the game
					// confirms calibration succeeded (param_x != 0 from SET_PARAM).
					// This is robust regardless of region, boot sequence, or logos.
					if (us->needs_2point_init)
					{
						// Calibration succeeded — stop injecting.
						if (us->param_x != 0)
						{
							us->needs_2point_init = false;
							us->init_2point_state = 0;
							Console.WriteLn("(GunCon2) Port %u: 2-Point calibration confirmed (param_x=%d)", us->port, us->param_x);
						}
						else switch (us->init_2point_state)
						{
						case 0: // Waiting for a dark frame
							if (dark)
								us->init_2point_state = 1;
							break;
						case 1: // In dark, waiting for it to end
							if (!dark)
							{
								us->init_2point_state = 2;
								us->init_2point_polls = 0;
								Console.WriteLn("(GunCon2) Port %u: injecting Trigger+C offscreen for 2-Point init", us->port);
							}
							break;
						case 2: // Injecting Trigger+C with pos=(0,0)
							out.buttons &= ~(1u << BID_TRIGGER); // active low: clear bit = pressed
							out.buttons &= ~(1u << BID_C);
							out.pos_x = 0;
							out.pos_y = 0;
							if (++us->init_2point_polls >= 60)
							{
								// Go back to waiting — will retry on next dark→light.
								us->init_2point_state = 0;
								Console.WriteLn("(GunCon2) Port %u: 2-Point injection done, waiting for calibration...", us->port);
							}
							break;
						}
					}

					// ================================================================
					// DIAG: comprehensive trigger/inject/game-state logging
					// ================================================================
					us->diag_poll_count++;
					const bool trigger_down = (us->button_state & (1u << BID_TRIGGER)) != 0;
					const bool trigger_edge_down = trigger_down && !us->diag_trigger_was_down;
					const bool trigger_edge_up = !trigger_down && us->diag_trigger_was_down;

					// CZ US game RAM reads (only when serial matches).
					// Addresses from ELF reverse engineering (Ingénieur - PS2 sessions).
					u32 g_dispatch = 0, g_flash_timer = 0, g_flash_enable = 0;
					u32 g_calib_state = 0, g_gameplay = 0, g_fire_timer = 0;
					u32 g_trigger_flag = 0, g_screen_x = 0, g_held_buttons = 0;
					u32 g_buttons_proc = 0, g_pos_valid = 0;
					u32 g_base_delay = 0, g_extra_delay = 0;
					u32 g_dark_count = 0, g_progressive = 0, g_frame_count = 0, g_prog_done = 0;
					u32 g_game_state = 0, g_calib_sub = 0, g_calib_next = 0;
					if (us->diag_is_cz_us)
					{
						// Global vars (gp=0x0045C4F0 relative)
						g_dispatch     = memRead32(0x00456E40); // dispatch_state: 5=gameplay, 6/7=degraded
						g_flash_timer  = memRead32(0x00456DFC); // flash countdown 16→0
						g_flash_enable = memRead32(0x00456E10); // 1=flash active this frame
						g_base_delay   = memRead32(0x00456E18); // fire_timer base (init=2)
						g_extra_delay  = memRead32(0x00456E14); // fire_timer extra (init=2)
						// FUNC_A (progressive detection)
						g_dark_count   = memRead32(0x00456E04); // dark frames counted (need >=4)
						g_progressive  = memRead32(0x00456E1C); // 0=interlaced, 1=progressive
						g_frame_count  = memRead32(0x00456E08); // frame counter in FUNC_A (0→8)
						g_prog_done    = memRead32(0x00456E24); // 1=FUNC_A completed
						// Game-side calibration screen (0x146D00)
						g_game_state   = memRead32(0x004569A0); // 0x51=calibration screen active
						g_calib_sub    = memRead32(0x00456A30); // sub-state in calibration screen
						g_calib_next   = memRead32(0x00456AB0); // next calibration step
						// Per-player gun_state P0 (base=0x7FFC30, stride=0x3C)
						g_calib_state  = memRead32(0x007FFC68); // +0x38: 0→8, SET_PARAM at 4
						g_gameplay     = memRead32(0x007FFC54); // +0x24: 1=gameplay active
						g_fire_timer   = memRead32(0x007FFC40); // +0x10: fire countdown (FUNC_B calib)
						g_trigger_flag = memRead32(0x007FFC44); // +0x14: 1=edge trigger (1 frame only)
						g_screen_x     = memRead32(0x007FFC4C); // +0x1C: -1=dark
						g_held_buttons = memRead32(0x007FFC58); // +0x28: held buttons bitmask
						g_buttons_proc = memRead32(0x007FFC5C); // +0x2C: processed buttons — SHOOT reads bit 5
						g_pos_valid    = memRead32(0x007FFC3C); // +0x0C: pos_valid
					}

					// Log trigger down with FULL state dump.
					if (trigger_edge_down)
					{
						Console.WriteLn("(DIAG:TRIGGER) Port %u poll=%u DOWN | out=(%d,%d) calc=(%d,%d) | dark=%d locked=%d pending=%d resp=%d | inject: dly=%u dur=%u cd=%u act=%u fired=%d",
							us->port, us->diag_poll_count, out.pos_x, out.pos_y, pos_x, pos_y,
							dark ? 1 : 0, us->calibration_locked ? 1 : 0, us->calibration_pending ? 1 : 0,
							us->calib_responded ? 1 : 0,
							us->dark_delay, us->dark_duration, us->dark_inject_countdown, us->dark_inject_active,
							us->dark_inject_fired ? 1 : 0);
						if (us->diag_is_cz_us)
						{
							Console.WriteLn("(DIAG:CZ-RAM) Port %u poll=%u | dispatch=%u flash_tmr=%u flash_en=%u | calib=%u gameplay=%u fire_tmr=%u trig_flag=%u scrn_x=%d | held=0x%X btn_proc=0x%X pos_valid=%u | base_dly=%u extra_dly=%u | dark_cnt=%u prog=%u frm_cnt=%u prog_done=%u | gstate=0x%X csub=%u cnext=%u",
								us->port, us->diag_poll_count,
								g_dispatch, g_flash_timer, g_flash_enable,
								g_calib_state, g_gameplay, g_fire_timer, g_trigger_flag,
								static_cast<s32>(g_screen_x),
								g_held_buttons, g_buttons_proc, g_pos_valid,
								g_base_delay, g_extra_delay,
								g_dark_count, g_progressive, g_frame_count, g_prog_done,
								g_game_state, g_calib_sub, g_calib_next);
						}
					}

					// Log trigger up.
					if (trigger_edge_up)
					{
						Console.WriteLn("(DIAG:TRIGGER) Port %u poll=%u UP | out=(%d,%d) | dark=%d locked=%d",
							us->port, us->diag_poll_count, out.pos_x, out.pos_y,
							dark ? 1 : 0, us->calibration_locked ? 1 : 0);
						if (us->diag_is_cz_us)
						{
							Console.WriteLn("(DIAG:CZ-RAM) Port %u poll=%u | dispatch=%u flash_tmr=%u flash_en=%u | gameplay=%u fire_tmr=%u scrn_x=%d btn_proc=0x%X | dark_cnt=%u prog=%u prog_done=%u | gstate=0x%X csub=%u cnext=%u",
								us->port, us->diag_poll_count,
								g_dispatch, g_flash_timer, g_flash_enable,
								g_gameplay, g_fire_timer, static_cast<s32>(g_screen_x), g_buttons_proc,
								g_dark_count, g_progressive, g_prog_done,
								g_game_state, g_calib_sub, g_calib_next);
						}
					}
					us->diag_trigger_was_down = trigger_down;

					// Log gameplay shooting: track btn_proc and fire_tmr changes while trigger held.
					// This captures autofire pulses (btn_proc=0x20 = shot registered) that were
					// invisible in the previous logging (only heartbeat every 300 polls).
					if (us->diag_is_cz_us && trigger_down && us->calibration_locked)
					{
						if (g_buttons_proc != us->diag_last_btn_proc || g_fire_timer != us->diag_last_fire_tmr)
						{
							Console.WriteLn("(DIAG:SHOOT) Port %u poll=%u | fire_tmr=%u btn_proc=0x%X scrn_x=%d | dispatch=%u base_dly=%u",
								us->port, us->diag_poll_count,
								g_fire_timer, g_buttons_proc, static_cast<s32>(g_screen_x),
								g_dispatch, g_base_delay);
							us->diag_last_btn_proc = g_buttons_proc;
							us->diag_last_fire_tmr = g_fire_timer;
						}
					}

					// Log every poll during dark_inject countdown or active.
					if (us->dark_inject_countdown > 0 || us->dark_inject_active > 0)
					{
						Console.WriteLn("(DIAG:INJECT) Port %u poll=%u | cd=%u act=%u | out=(%d,%d) | dark=%d locked=%d",
							us->port, us->diag_poll_count,
							us->dark_inject_countdown, us->dark_inject_active,
							out.pos_x, out.pos_y, dark ? 1 : 0, us->calibration_locked ? 1 : 0);
						if (us->diag_is_cz_us)
						{
							Console.WriteLn("(DIAG:CZ-RAM) Port %u poll=%u | dispatch=%u flash_tmr=%u flash_en=%u | gameplay=%u fire_tmr=%u scrn_x=%d btn_proc=0x%X | dark_cnt=%u prog=%u prog_done=%u | gstate=0x%X csub=%u cnext=%u",
								us->port, us->diag_poll_count,
								g_dispatch, g_flash_timer, g_flash_enable,
								g_gameplay, g_fire_timer, static_cast<s32>(g_screen_x), g_buttons_proc,
								g_dark_count, g_progressive, g_prog_done,
								g_game_state, g_calib_sub, g_calib_next);
						}
					}

					// Heartbeat every 300 polls with full state.
					if ((us->diag_poll_count % 300) == 1)
					{
						Console.WriteLn("(DIAG:HEARTBEAT) Port %u poll=%u | param=(%d,%d) mode=0x%X | locked=%d pending=%d htrig=%d resp=%d disabled=%d | dark=%d | dly=%u dur=%u | trig=%d",
							us->port, us->diag_poll_count, us->param_x, us->param_y, us->param_mode,
							us->calibration_locked ? 1 : 0, us->calibration_pending ? 1 : 0,
							us->has_triggered ? 1 : 0, us->calib_responded ? 1 : 0,
							us->photodiode_disabled ? 1 : 0,
							dark ? 1 : 0, us->dark_delay, us->dark_duration, trigger_down ? 1 : 0);
						if (us->diag_is_cz_us)
						{
							Console.WriteLn("(DIAG:CZ-RAM) Port %u poll=%u | dispatch=%u flash_tmr=%u flash_en=%u | calib=%u gameplay=%u fire_tmr=%u scrn_x=%d | held=0x%X btn_proc=0x%X pos_valid=%u | dark_cnt=%u prog=%u frm_cnt=%u prog_done=%u | gstate=0x%X csub=%u cnext=%u",
								us->port, us->diag_poll_count,
								g_dispatch, g_flash_timer, g_flash_enable,
								g_calib_state, g_gameplay, g_fire_timer,
								static_cast<s32>(g_screen_x), g_held_buttons, g_buttons_proc, g_pos_valid,
								g_dark_count, g_progressive, g_frame_count, g_prog_done,
								g_game_state, g_calib_sub, g_calib_next);
						}
					}

					usb_packet_copy(p, &out, sizeof(out));
					break;
				}
			}
				[[fallthrough]];

			case USB_TOKEN_OUT:
			default:
			{
				Console.Error("Unhandled GunCon2 request pid=%d ep=%u", p->pid, p->ep->nr);
				p->status = USB_RET_STALL;
			}
			break;
		}
	}

	static void usb_hid_unrealize(USBDevice* dev)
	{
		GunCon2State* us = USB_CONTAINER_OF(dev, GunCon2State, dev);

		if (!us->cursor_path.empty())
			ImGuiManager::ClearSoftwareCursor(us->GetSoftwarePointerIndex());

		delete us;
	}

	GunCon2State::GunCon2State(u32 port_)
		: port(port_)
	{
		g_guncon2_count.fetch_add(1, std::memory_order_relaxed);
	}

	GunCon2State::~GunCon2State()
	{
		g_guncon2_count.fetch_sub(1, std::memory_order_relaxed);
	}

	void GunCon2State::AutoConfigure()
	{
		const std::string serial = VMManager::GetDiscSerial();
		for (const GameConfig& gc : s_game_config)
		{
			if (serial != gc.serial)
				continue;

			Console.WriteLn(fmt::format("(GunCon2) Found game config for '{}'", serial));

			// DIAG: enable CZ US-specific game RAM reads for deep logging.
			if (serial == "SLUS-20927")
			{
				diag_is_cz_us = true;
				Console.WriteLn(fmt::format("(GunCon2) Port {}: DIAG enabled for CZ US (memRead32 at CZ addresses)", port));
			}

			// Position values: only apply if NOT using custom manual config.
			if (!custom_config)
			{
				Console.WriteLn(fmt::format("  Scale: {}x{}", gc.scale_x / 100.0f, gc.scale_y / 100.0f));
				Console.WriteLn(fmt::format("  Center Position: {}x{}", gc.center_x, gc.center_y));
				Console.WriteLn(fmt::format("  Screen Size: {}x{}", gc.screen_width, gc.screen_height));

				scale_x = gc.scale_x / 100.0f;
				scale_y = gc.scale_y / 100.0f;
				center_x = static_cast<float>(gc.center_x);
				center_y = static_cast<float>(gc.center_y);
				screen_width = gc.screen_width;
				screen_height = gc.screen_height;
			}
			else
			{
				Console.WriteLn("  Position values: SKIPPED (manual config active)");
			}

			// Per-game features: ALWAYS apply regardless of custom_config.
			if (gc.force_2point)
			{
				needs_2point_init = true;
				Console.WriteLn(fmt::format("(GunCon2) Port {}: 2-Point Adjustment will be auto-injected at boot", port));
			}

			// Per-game photodiode dark threshold (0 = use defaults in GSRenderer).
			g_guncon2_dark_threshold.store(gc.dark_threshold, std::memory_order_relaxed);
			if (gc.dark_threshold)
				Console.WriteLn(fmt::format("(GunCon2) Custom dark threshold: entry={}, exit={}", gc.dark_threshold, gc.dark_threshold * 2));

			// No-photodiode games: permanently lock so dark flag is never used.
			if (gc.no_photodiode)
			{
				calibration_locked = true;
				photodiode_disabled = true;
				Console.WriteLn(fmt::format("(GunCon2) Port {}: photodiode DISABLED — dark flag permanently ignored", port));
			}

			// Calibration done button: which button locks dark_inject after calibration.
			calib_done_btn = gc.calib_done_btn;
			if (gc.calib_done_btn != CALIB_BTN_NONE)
			{
				static const char* btn_names[] = {"NONE", "A/B", "START", "OFFSCREEN"};
				Console.WriteLn(fmt::format("(GunCon2) Port {}: calibration done button = {}", port, btn_names[gc.calib_done_btn]));
			}

			// Per-game dark injection timing. Always set from GameConfig to
			// override any stale values from PCSX2 saved settings.
			dark_delay = gc.dark_delay;
			dark_duration = gc.dark_duration;
			fire_once = gc.fire_once;

			if (gc.dark_delay > 0 || gc.dark_duration > 0)
			{
				Console.WriteLn(fmt::format("(GunCon2) Port {}: dark inject enabled (delay={}us, duration={}us, fire_once={})",
					port, dark_delay, dark_duration, fire_once ? "YES" : "NO"));
			}
			else
			{
				Console.WriteLn(fmt::format("(GunCon2) Port {}: dark inject disabled (vanilla photodiode)", port));
			}

			return;
		}

		Console.Warning(fmt::format("(GunCon2) No game config found for '{}'.", serial));
		g_guncon2_dark_threshold.store(0, std::memory_order_relaxed);
	}

	std::tuple<s16, s16> GunCon2State::CalculatePosition()
	{
		float pointer_x, pointer_y;
		const auto& [window_x, window_y] =
			(has_relative_binds) ? GetAbsolutePositionFromRelativeAxes() : InputManager::GetPointerAbsolutePosition(pointer_index);
		GSTranslateWindowToDisplayCoordinates(window_x, window_y, &pointer_x, &pointer_y);

		s16 pos_x, pos_y;
		if (pointer_x < 0.0f || pointer_y < 0.0f)
		{
			// off-screen
			pos_x = 0;
			pos_y = 0;
		}
		else
		{
			// scale to internal coordinate system and center
			float fx = (pointer_x * static_cast<float>(screen_width)) - static_cast<float>(screen_width / 2u);
			float fy = (pointer_y * static_cast<float>(screen_height)) - static_cast<float>(screen_height / 2u);

			// apply curvature scale
			fx *= scale_x;
			fy *= scale_y;

			// and re-center based on game center
			s32 x = static_cast<s32>(std::round(fx + center_x));
			s32 y = static_cast<s32>(std::round(fy + center_y));

			// apply game-configured offset
			if (param_mode & GUNCON2_FLAG_PROGRESSIVE)
			{
				x -= param_x / 2;
				y -= param_y / 2;
			}
			else
			{
				x -= param_x;
				y -= param_y;
			}

			// 0,0 is reserved for offscreen, so ensure we don't send that
			pos_x = static_cast<s16>(std::max(x, 1));
			pos_y = static_cast<s16>(std::max(y, 1));
		}

		return std::tie(pos_x, pos_y);
	}

	std::pair<float, float> GunCon2State::GetAbsolutePositionFromRelativeAxes() const
	{
		const float screen_rel_x = (((relative_pos[1] > 0.0f) ? relative_pos[1] : -relative_pos[0]) + 1.0f) * 0.5f;
		const float screen_rel_y = (((relative_pos[3] > 0.0f) ? relative_pos[3] : -relative_pos[2]) + 1.0f) * 0.5f;
		return std::make_pair(
			screen_rel_x * ImGuiManager::GetWindowWidth(), screen_rel_y * ImGuiManager::GetWindowHeight());
	}

	u32 GunCon2State::GetSoftwarePointerIndex() const
	{
		return has_relative_binds ? (InputManager::MAX_POINTER_DEVICES + pointer_index) : pointer_index;
	}

	void GunCon2State::UpdateSoftwarePointerPosition()
	{
		if (cursor_path.empty())
			return;

		const auto& [window_x, window_y] = GetAbsolutePositionFromRelativeAxes();
		ImGuiManager::SetSoftwareCursorPosition(GetSoftwarePointerIndex(), window_x, window_y);
	}

	const char* GunCon2Device::Name() const
	{
		return TRANSLATE_NOOP("USB", "GunCon 2");
	}

	const char* GunCon2Device::TypeName() const
	{
		return "guncon2";
	}

	const char* GunCon2Device::IconName() const
	{
		return ICON_PF_GUNCON2;
	}

	USBDevice* GunCon2Device::CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const
	{
		GunCon2State* s = new GunCon2State(port);
		s->desc.full = &s->desc_dev;
		s->desc.str = desc_strings;

		if (usb_desc_parse_dev(guncon2_dev_desc, sizeof(guncon2_dev_desc), s->desc, s->desc_dev) < 0)
			goto fail;
		if (usb_desc_parse_config(guncon2_config_desc, sizeof(guncon2_config_desc), s->desc_dev) < 0)
			goto fail;

		s->dev.speed = USB_SPEED_FULL;
		s->dev.klass.handle_attach = usb_desc_attach;
		s->dev.klass.handle_control = guncon2_handle_control;
		s->dev.klass.handle_data = guncon2_handle_data;
		s->dev.klass.unrealize = usb_hid_unrealize;
		s->dev.klass.usb_desc = &s->desc;
		s->dev.klass.product_desc = s->desc.str[2];

		usb_desc_init(&s->dev);
		usb_ep_init(&s->dev);

		UpdateSettings(&s->dev, si);

		return &s->dev;
	fail:
		usb_hid_unrealize(&s->dev);
		return nullptr;
	}

	void GunCon2Device::UpdateSettings(USBDevice* dev, SettingsInterface& si) const
	{
		GunCon2State* s = USB_CONTAINER_OF(dev, GunCon2State, dev);

		s->custom_config = USB::GetConfigBool(si, s->port, TypeName(), "custom_config", false);

		// GUI override for dark inject timing (-1 = Auto, use GameConfig. 0+ = value in polls, converted to µs).
		const s32 gui_delay = USB::GetConfigInt(si, s->port, TypeName(), "dark_delay", -1);
		const s32 gui_duration = USB::GetConfigInt(si, s->port, TypeName(), "dark_duration", -1);
		if (gui_delay >= 0 || gui_duration >= 0)
		{
			if (gui_delay >= 0)
				s->dark_delay = static_cast<u32>(gui_delay) * 8000; // GUI polls → µs
			if (gui_duration >= 0)
				s->dark_duration = static_cast<u32>(gui_duration) * 8000; // GUI polls → µs
			Console.WriteLn("(GunCon2) Port %u: GUI override dark inject (delay=%uus, duration=%uus)",
				s->port, s->dark_delay, s->dark_duration);
		}

		// Don't override auto config if we've set it.
		if (!s->auto_config_done || s->custom_config)
		{
			s->screen_width = USB::GetConfigInt(si, s->port, TypeName(), "screen_width", DEFAULT_SCREEN_WIDTH);
			s->screen_height = USB::GetConfigInt(si, s->port, TypeName(), "screen_height", DEFAULT_SCREEN_HEIGHT);
			s->center_x = USB::GetConfigFloat(si, s->port, TypeName(), "center_x", DEFAULT_CENTER_X);
			s->center_y = USB::GetConfigFloat(si, s->port, TypeName(), "center_y", DEFAULT_CENTER_Y);
			s->scale_x = USB::GetConfigFloat(si, s->port, TypeName(), "scale_x", DEFAULT_SCALE_X) / 100.0f;
			s->scale_y = USB::GetConfigFloat(si, s->port, TypeName(), "scale_y", DEFAULT_SCALE_Y) / 100.0f;
		}

		// Pointer settings.
		const std::string pointer_source = USB::GetConfigString(si, s->port, TypeName(), "pointer_source", "Auto");
		if (pointer_source == "Auto" || pointer_source.empty())
		{
			s->pointer_index = s->port;
		}
		else
		{
			// pointer_source is a device path — resolve to pointer index.
			const std::optional<u32> idx = InputManager::GetPointerIndexForRawDevice(pointer_source);
			s->pointer_index = idx.value_or(s->port);
		}

		const std::string pointer_binding = USB::GetConfigString(si, s->port, TypeName(), "Pointer", "");
		std::string cursor_path(USB::GetConfigString(si, s->port, TypeName(), "cursor_path"));
		const float cursor_scale = USB::GetConfigFloat(si, s->port, TypeName(), "cursor_scale", 1.0f);
		u32 cursor_color = 0xFFFFFF;
		if (std::string cursor_color_str(USB::GetConfigString(si, s->port, TypeName(), "cursor_color")); !cursor_color_str.empty())
		{
			// Strip the leading hash, if it's a CSS style colour.
			const std::optional<u32> cursor_color_opt(
				StringUtil::FromChars<u32>(cursor_color_str[0] == '#' ?
					std::string_view(cursor_color_str).substr(1) : std::string_view(cursor_color_str), 16));
			if (cursor_color_opt.has_value())
				cursor_color = cursor_color_opt.value();
		}

		const s32 prev_pointer_index = s->GetSoftwarePointerIndex();

		s->has_relative_binds = (USB::ConfigKeyExists(si, s->port, TypeName(), "RelativeLeft") ||
			USB::ConfigKeyExists(si, s->port, TypeName(), "RelativeRight") ||
			USB::ConfigKeyExists(si, s->port, TypeName(), "RelativeUp") ||
			USB::ConfigKeyExists(si, s->port, TypeName(), "RelativeDown"));

		const s32 new_pointer_index = s->GetSoftwarePointerIndex();

		if (prev_pointer_index != new_pointer_index || s->cursor_path != cursor_path ||
			s->cursor_scale != cursor_scale || s->cursor_color != cursor_color)
		{
			if (prev_pointer_index != new_pointer_index)
				ImGuiManager::ClearSoftwareCursor(prev_pointer_index);

			// Pointer changed, so need to update software cursor.
			const bool had_software_cursor = !s->cursor_path.empty();
			s->cursor_path = std::move(cursor_path);
			s->cursor_scale = cursor_scale;
			s->cursor_color = cursor_color;
			if (!s->cursor_path.empty())
			{
				ImGuiManager::SetSoftwareCursor(new_pointer_index, s->cursor_path, s->cursor_scale, s->cursor_color);
				s->UpdateSoftwarePointerPosition();
			}
			else if (had_software_cursor)
			{
				ImGuiManager::ClearSoftwareCursor(new_pointer_index);
			}
		}
	}

	float GunCon2Device::GetBindingValue(const USBDevice* dev, u32 bind_index) const
	{
		GunCon2State* s = USB_CONTAINER_OF(dev, GunCon2State, dev);

		const u32 bit = 1u << bind_index;
		return ((s->button_state & bit) != 0) ? 1.0f : 0.0f;
	}

	void GunCon2Device::SetBindingValue(USBDevice* dev, u32 bind_index, float value) const
	{
		GunCon2State* s = USB_CONTAINER_OF(dev, GunCon2State, dev);

		if (bind_index < BID_RELATIVE_LEFT)
		{
			const u32 bit = 1u << bind_index;
			if (value >= 0.5f)
				s->button_state |= bit;
			else
				s->button_state &= ~bit;
		}
		else if (bind_index <= BID_RELATIVE_DOWN)
		{
			const u32 rel_index = bind_index - BID_RELATIVE_LEFT;
			if (s->relative_pos[rel_index] != value)
			{
				s->relative_pos[rel_index] = value;
				s->UpdateSoftwarePointerPosition();
			}
		}
	}

	std::span<const InputBindingInfo> GunCon2Device::Bindings(u32 subtype) const
	{
		static constexpr const InputBindingInfo bindings[] = {
			//{"pointer", "Pointer/Aiming", InputBindingInfo::Type::Pointer, BID_POINTER_X, GenericInputBinding::Unknown},
			{"Up", TRANSLATE_NOOP("USB", "D-Pad Up"), nullptr, InputBindingInfo::Type::Button, BID_DPAD_UP, GenericInputBinding::DPadUp},
			{"Down", TRANSLATE_NOOP("USB", "D-Pad Down"), nullptr, InputBindingInfo::Type::Button, BID_DPAD_DOWN, GenericInputBinding::DPadDown},
			{"Left", TRANSLATE_NOOP("USB", "D-Pad Left"), nullptr, InputBindingInfo::Type::Button, BID_DPAD_LEFT, GenericInputBinding::DPadLeft},
			{"Right", TRANSLATE_NOOP("USB", "D-Pad Right"), nullptr, InputBindingInfo::Type::Button, BID_DPAD_RIGHT,
				GenericInputBinding::DPadRight},
			{"Trigger", TRANSLATE_NOOP("USB", "Trigger"), nullptr, InputBindingInfo::Type::Button, BID_TRIGGER, GenericInputBinding::R2},
			{"ShootOffscreen", TRANSLATE_NOOP("USB", "Shoot Offscreen"), nullptr, InputBindingInfo::Type::Button, BID_SHOOT_OFFSCREEN,
				GenericInputBinding::R1},
			{"A", TRANSLATE_NOOP("USB", "A"), nullptr, InputBindingInfo::Type::Button, BID_A, GenericInputBinding::Cross},
			{"B", TRANSLATE_NOOP("USB", "B"), nullptr, InputBindingInfo::Type::Button, BID_B, GenericInputBinding::Circle},
			{"C", TRANSLATE_NOOP("USB", "C"), nullptr, InputBindingInfo::Type::Button, BID_C, GenericInputBinding::Triangle},
			{"Select", TRANSLATE_NOOP("USB", "Select"), nullptr, InputBindingInfo::Type::Button, BID_SELECT, GenericInputBinding::Select},
			{"Start", TRANSLATE_NOOP("USB", "Start"), nullptr, InputBindingInfo::Type::Button, BID_START, GenericInputBinding::Start},
			{"RelativeLeft", TRANSLATE_NOOP("USB", "Relative Left"), nullptr, InputBindingInfo::Type::HalfAxis, BID_RELATIVE_LEFT, GenericInputBinding::Unknown},
			{"RelativeRight", TRANSLATE_NOOP("USB", "Relative Right"), nullptr, InputBindingInfo::Type::HalfAxis, BID_RELATIVE_RIGHT, GenericInputBinding::Unknown},
			{"RelativeUp", TRANSLATE_NOOP("USB", "Relative Up"), nullptr, InputBindingInfo::Type::HalfAxis, BID_RELATIVE_UP, GenericInputBinding::Unknown},
			{"RelativeDown", TRANSLATE_NOOP("USB", "Relative Down"), nullptr, InputBindingInfo::Type::HalfAxis, BID_RELATIVE_DOWN, GenericInputBinding::Unknown},
		};

		return bindings;
	}

	static std::vector<std::pair<std::string, std::string>> GetPointerDeviceList()
	{
		std::vector<std::pair<std::string, std::string>> result;
		result.emplace_back("Auto", "Auto (use USB port number)");

		// Add all raw mouse devices, keyed by device_path for persistence.
		for (const auto& [device_path, display_name] : InputManager::EnumerateRawPointerDevices())
			result.emplace_back(device_path, display_name);

		return result;
	}

	std::span<const SettingInfo> GunCon2Device::Settings(u32 subtype) const
	{
		static constexpr const SettingInfo info[] = {
			{SettingInfo::Type::StringList, "pointer_source", TRANSLATE_NOOP("USB", "Pointer Device"),
				TRANSLATE_NOOP("USB", "Selects which mouse/lightgun device controls the aiming for this port. "
									  "'Auto' uses the USB port number (USB1=Pointer 0, USB2=Pointer 1)."),
				"Auto", nullptr, nullptr, nullptr, nullptr, nullptr, &GetPointerDeviceList},
			{SettingInfo::Type::Path, "cursor_path", TRANSLATE_NOOP("USB", "Cursor Path"),
				TRANSLATE_NOOP("USB", "Sets the crosshair image that this lightgun will use. Setting a crosshair image "
									  "will disable the system cursor."),
				""},
			{SettingInfo::Type::Float, "cursor_scale", TRANSLATE_NOOP("USB", "Cursor Scale"),
				TRANSLATE_NOOP("USB", "Scales the crosshair image set above."), "1", "0.01", "10", "0.01", TRANSLATE_NOOP("USB", "%.0f%%"),
				nullptr, nullptr, 100.0f},
			{SettingInfo::Type::String, "cursor_color", TRANSLATE_NOOP("USB", "Cursor Color"),
				TRANSLATE_NOOP("USB", "Applies a color to the chosen crosshair images, can be used for multiple "
									  "players. Specify in HTML/CSS format (e.g. #aabbcc)"),
				"#ffffff"},
			{SettingInfo::Type::Boolean, "custom_config", TRANSLATE_NOOP("USB", "Manual Screen Configuration"),
				TRANSLATE_NOOP("USB",
					"Forces the use of the screen parameters below, instead of automatic parameters if available."),
				"false"},
			{SettingInfo::Type::Float, "scale_x", TRANSLATE_NOOP("USB", "X Scale (Sensitivity)"),
				TRANSLATE_NOOP("USB", "Scales the position to simulate CRT curvature."), "100", "0", "200", "0.1",
				TRANSLATE_NOOP("USB", "%.2f%%"), nullptr, nullptr, 1.0f},
			{SettingInfo::Type::Float, "scale_y", TRANSLATE_NOOP("USB", "Y Scale (Sensitivity)"),
				TRANSLATE_NOOP("USB", "Scales the position to simulate CRT curvature."), "100", "0", "200", "0.1",
				TRANSLATE_NOOP("USB", "%.2f%%"), nullptr, nullptr, 1.0f},
			{SettingInfo::Type::Float, "center_x", TRANSLATE_NOOP("USB", "Center X"),
				TRANSLATE_NOOP("USB", "Sets the horizontal center position of the simulated screen."), "320", "0",
				"1024", "1", TRANSLATE_NOOP("USB", "%.0fpx"), nullptr, nullptr, 1.0f},
			{SettingInfo::Type::Float, "center_y", TRANSLATE_NOOP("USB", "Center Y"),
				TRANSLATE_NOOP("USB", "Sets the vertical center position of the simulated screen."), "120", "0", "1024",
				"1", TRANSLATE_NOOP("USB", "%.0fpx"), nullptr, nullptr, 1.0f},
			{SettingInfo::Type::Integer, "screen_width", TRANSLATE_NOOP("USB", "Screen Width"),
				TRANSLATE_NOOP("USB", "Sets the width of the simulated screen."), "640", "1", "1024", "1", TRANSLATE_NOOP("USB", "%dpx"),
				nullptr, nullptr, 1.0f},
			{SettingInfo::Type::Integer, "screen_height", TRANSLATE_NOOP("USB", "Screen Height"),
				TRANSLATE_NOOP("USB", "Sets the height of the simulated screen."), "240", "1", "1024", "1", TRANSLATE_NOOP("USB", "%dpx"),
				nullptr, nullptr, 1.0f},
			{SettingInfo::Type::Integer, "dark_delay", TRANSLATE_NOOP("USB", "Dark Inject Delay"),
				TRANSLATE_NOOP("USB", "Polls to wait after trigger before injecting dark. -1 = Auto (use game default)."), "-1", "-1", "30", "1", TRANSLATE_NOOP("USB", "%d polls"),
				nullptr, nullptr, 1.0f},
			{SettingInfo::Type::Integer, "dark_duration", TRANSLATE_NOOP("USB", "Dark Inject Duration"),
				TRANSLATE_NOOP("USB", "Polls to hold dark after delay. -1 = Auto (use game default)."), "-1", "-1", "15", "1", TRANSLATE_NOOP("USB", "%d polls"),
				nullptr, nullptr, 1.0f},
		};
		return info;
	}

	bool GunCon2Device::Freeze(USBDevice* dev, StateWrapper& sw) const
	{
		GunCon2State* s = USB_CONTAINER_OF(dev, GunCon2State, dev);

		if (!sw.DoMarker("GunCon2Device"))
			return false;

		sw.Do(&s->param_x);
		sw.Do(&s->param_y);
		sw.Do(&s->param_mode);
		sw.Do(&s->calibration_timer);
		sw.Do(&s->calibration_pos_x);
		sw.Do(&s->calibration_pos_y);
		sw.Do(&s->auto_config_done);

		float scale_x = s->scale_x;
		float scale_y = s->scale_y;
		float center_x = s->center_x;
		float center_y = s->center_y;
		u32 screen_width = s->screen_width;
		u32 screen_height = s->screen_height;
		sw.Do(&scale_x);
		sw.Do(&scale_y);
		sw.Do(&center_x);
		sw.Do(&center_y);
		sw.Do(&screen_width);
		sw.Do(&screen_height);

		// Only save automatic settings to state.
		if (sw.IsReading() && !s->custom_config && s->auto_config_done)
		{
			s->scale_x = scale_x;
			s->scale_y = scale_y;
			s->center_x = center_x;
			s->center_y = center_y;
			s->screen_width = screen_width;
			s->screen_height = screen_height;
		}

		return !sw.HasError();
	}
} // namespace usb_lightgun
