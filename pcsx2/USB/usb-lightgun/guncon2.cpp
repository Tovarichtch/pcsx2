// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GS.h"
#include "Host.h"
#include "IconsPromptFont.h"
#include "ImGui/ImGuiManager.h"
#include "Input/InputManager.h"
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
		bool lock_permanent; // Once calibration locks, it NEVER unlocks (blocks SET_PARAM x=0 spam).
		u16 dark_delay;    // Trigger-delayed dark injection: polls before injecting dark (0 = use photodiode).
		u16 dark_duration; // How many polls to hold dark (0 = use photodiode).
	};

	static constexpr const GameConfig s_game_config[] = {
		// RE-verified values: center_x=422 confirmed by `addiu -0x1A6` in all Namco/Sega MIPS binaries.
		// dark_delay/dark_duration: trigger-delayed dark injection (0/0 = use photodiode).
		//   CZ: delay=9 (ftimer≤13, below game's <13 threshold), duration=5.
		//   VC: delay=1 (immediate, no early-dark protection), duration=3.
		//                                      sx       sy     cx   cy    w    h   2pt  dark nopd perm  dly dur
		{"SLPM-62401",  89.0f,  103.0f,  422, 130, 640, 240, false, 0, false, false, 0, 0}, // Death Crimson OX+ (J)
		{"SLES-50930", 100.0f,  100.0f,  422, 134, 640, 256, false, 0, false, false, 0, 0}, // Dino Stalker (E, En)
		{"SLES-51095", 100.0f,  100.0f,  422, 134, 640, 256, false, 0, false, false, 0, 0}, // Dino Stalker (E, Fr)
		{"SLES-51096", 100.0f,  100.0f,  422, 134, 640, 256, false, 0, false, false, 0, 0}, // Dino Stalker (E, De)
		{"SLUS-20485", 100.0f,  100.0f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Dino Stalker (U)
		{"SLUS-20389",  89.25f,  93.5f,  422, 141, 640, 240, false, 0, false, false, 0, 0}, // Endgame (U)
		{"SLES-50936", 112.0f,  100.0f,  320, 120, 512, 256, false, 0, false, false, 0, 0}, // Endgame (E) (USB port 2)
		{"SLPM-65059", 100.0f,  100.0f,  422, 132, 640, 256, false, 0, false, false, 0, 0}, // Gun Survivor 2 (J)
		{"SLPM-65139", 100.0f,  100.0f,  320, 120, 640, 240, false, 0, false, false, 0, 0}, // Gun Survivor 3 (J)
		{"SLPM-67529", 100.0f,  100.0f,  320, 120, 640, 240, false, 0, false, false, 0, 0}, // Gun Survivor 3 (KR)
		{"SLPM-65245", 100.0f,  100.0f,  422, 132, 640, 240, false, 0, false, false, 0, 0}, // Gun Survivor 4 (J)
		{"SLES-52620",  90.25f, 113.0f,  430, 160, 640, 256, false, 0, false, false, 0, 0}, // Guncom 2 (E)
		{"SLES-51289",  84.0f,   88.8f,  422, 134, 640, 256, false, 0, true,  false, 0, 0}, // Gunfighter II (E)
		{"SLPS-25165",  90.0f,   97.0f,  422, 135, 640, 240, false, 0, false, false, 0, 0}, // Gunvari Collection (J)
		{"SCES-50889",  90.25f,  92.0f,  422, 169, 640, 256, false, 0, false, false, 0, 0}, // Ninja Assault (E)
		{"SLPS-20218",  90.0f,   92.0f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Ninja Assault (J)
		{"SCPS-56015",  90.25f,  92.0f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Ninja Assault (KR)
		{"SLUS-20492",  90.25f,  92.0f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Ninja Assault (U)
		{"SLES-51448",  90.5f,   91.5f,  422, 134, 640, 256, false, 0, false, false, 0, 0}, // RE Dead Aim (E)
		{"SLUS-20669",  90.5f,   91.5f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // RE Dead Aim (U)
		{"SLES-50650",  90.5f,   91.5f,  422, 134, 640, 256, false, 0, false, false, 0, 0}, // RE Survivor 2 (E)
		{"SLES-51617",  90.0f,   88.0f,  320, 134, 640, 240, false, 0, false, false, 0, 0}, // Starsky & Hutch (E, En)
		{"SLES-51783",  90.0f,   87.7f,  320, 134, 640, 240, false, 0, false, false, 0, 0}, // Starsky & Hutch (E, Fr/De)
		{"SLKA-25090",  90.0f,   97.4f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Starsky & Hutch (KR)
		{"SLUS-20619",  90.25f,  91.75f, 453, 154, 640, 256, false, 0, false, false, 0, 0}, // Starsky & Hutch (U)
		{"SCES-50300",  90.0f,  103.0f,  422, 134, 640, 256, false, 0, false, false, 0, 0}, // Time Crisis II (E)
		{"SLPS-20122",  90.25f,  97.0f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Time Crisis II (J)
		{"SCKA-20002",  90.0f,   97.5f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Time Crisis II (KR)
		{"SLUS-20219",  90.0f,   97.5f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Time Crisis II (U)
		{"SCAJ-20060",  90.0f,   97.5f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Time Crisis 3 (Asia)
		{"SCES-51844",  90.0f,  100.0f,  422, 134, 640, 256, false, 0, false, false, 0, 0}, // Time Crisis 3 (E)
		{"SLPS-25290",  90.25f,  99.0f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Time Crisis 3 (J)
		{"SCKA-20015",  90.0f,   98.0f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Time Crisis 3 (KR)
		{"SLUS-20645",  90.25f, 100.0f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Time Crisis 3 (U)
		{"SCES-52530",  90.0f,  103.0f,  422, 153, 640, 256, false, 0, false, true,  9, 5}, // Crisis Zone (E)
		{"SCKA-20038",  90.0f,   97.5f,  422, 134, 640, 240, false, 0, false, true,  9, 5}, // Crisis Zone (KR)
		{"SLUS-20927",  90.25f,  97.5f,  422, 134, 640, 240, false, 0, false, true,  9, 5}, // Crisis Zone (U)
		{"SCES-50411",  90.0f,  100.0f,  422, 134, 640, 256, false, 0, false, false, 0, 0}, // Vampire Night (E)
		{"SLPS-25077",  90.0f,   97.5f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Vampire Night (J)
		{"SLUS-20221",  90.0f,   98.0f,  422, 134, 640, 240, false, 0, false, false, 0, 0}, // Vampire Night (U)
		{"SLES-51229", 111.0f,  100.0f,  422, 134, 512, 256, false, 0, false, false, 7, 3}, // Virtua Cop Elite Edition (E)
		{"SLPM-62205",  90.0f,   97.5f,  422, 134, 640, 240, false, 0, false, false, 8, 3}, // Virtua Cop Re-Birth (J)
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

		// Photodiode calibration lock: after calibration completes, lock dark=false
		// so gameplay never sees false darks (interlaced flicker, dark scenes).
		// Lock is PENDING after param_x changes, then CONFIRMED after no SET_PARAM
		// activity for a while — letting the game finish its full calibration cycle.
		bool calibration_locked = false;
		bool calibration_pending = false; // param_x changed, waiting for activity to settle
		u32 pending_poll_count = 0; // polls since last SET_PARAM while pending
		bool photodiode_disabled = false; // Permanent lock for no_photodiode games — never unlocks.
		bool lock_permanent = false; // Lock never unlocks once triggered (CZ: blocks brightness spam).
		bool param_x_initialized = false;
		s16 initial_param_x = 0;

		// Trigger-delayed dark injection: replaces photodiode for games with configured dark_delay.
		// Real GunCon2 on CRT: photodiode detects dark within the same vsync.
		// Our ring buffer: dark arrives ~13 polls later (too late for CZ, latency for VC).
		// Fix: on trigger, wait dark_delay polls then inject (0,0) for dark_duration polls.
		u16 dark_inject_countdown = 0;
		u16 dark_inject_active = 0;
		bool dark_inject_fired = false; // Edge detect: only inject once per trigger press.
		u16 dark_delay = 0;    // 0 = use photodiode (default). Set from GameConfig.
		u16 dark_duration = 0; // Set from GameConfig.

		static constexpr u32 LOCK_SETTLE_POLLS = 750; // ~6 seconds at 125Hz USB poll rate — no SET_PARAM activity = calibration done

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

			// Calibration lock: deferred activation.
			// ANY SET_PARAM after init → enter PENDING (or reset settle timer).
			// After LOCK_SETTLE_POLLS with no SET_PARAM → LOCKED → dark blocked.
			// Any SET_PARAM while LOCKED → UNLOCK + re-enter PENDING (re-calibration).
			// CZ (lock_permanent): immediate lock on param_x change, never unlocks.
			if (us->param_x_initialized)
			{
				if (us->lock_permanent)
				{
					// CZ: immediate permanent lock on param_x change.
					// SET_PARAM x=0 spam after lock is ignored (permanent).
					if (!us->calibration_locked && us->param_x != us->initial_param_x)
					{
						us->calibration_locked = true;
						Console.WriteLn("(GunCon2) Port %u: calibration LOCKED [permanent/immediate] (param_x: %d -> %d)",
							us->port, us->initial_param_x, us->param_x);
					}
				}
				else if (!us->photodiode_disabled)
				{
					// All other games: any SET_PARAM manages the settle timer.
					if (us->calibration_locked)
					{
						// Re-calibration: unlock and re-enter pending.
						us->calibration_locked = false;
						us->calibration_pending = true;
						us->pending_poll_count = 0;
						Console.WriteLn("(GunCon2) Port %u: calibration UNLOCKED (SET_PARAM received, re-calibrating)",
							us->port);
					}
					else if (!us->calibration_pending)
					{
						// First SET_PARAM since boot/unlock: enter pending.
						us->calibration_pending = true;
						us->pending_poll_count = 0;
						Console.WriteLn("(GunCon2) Port %u: calibration PENDING (SET_PARAM received, param_x=%d)",
							us->port, us->param_x);
					}
					else
					{
						// Already pending: reset settle timer.
						us->pending_poll_count = 0;
					}
				}
			}
			else
			{
				us->initial_param_x = us->param_x;
				us->param_x_initialized = true;
			}

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
					GunCon2Out out;
					out.buttons = static_cast<u16>(~us->button_state) | (us->param_mode & GUNCON2_FLAG_PROGRESSIVE);
					out.pos_x = pos_x;
					out.pos_y = pos_y;

					if (us->button_state & (1u << BID_SHOOT_OFFSCREEN))
					{
						out.buttons &= ~(1u << BID_TRIGGER);
						out.pos_x = 0;
						out.pos_y = 0;
					}

					const bool dark = g_guncon2_display_dark.load(std::memory_order_relaxed);

					if ((us->dark_delay > 0 || us->dark_duration > 0) && (!us->calibration_locked || !us->lock_permanent))
					{
						// Trigger-delayed dark injection — per-game optimized timing.
						// On trigger press, wait dark_delay polls then force pos=(0,0) for
						// dark_duration polls. Replaces vanilla's fixed-timer approach with
						// exact timing measured per game from ELF analysis.
						// CZ: delay=9, dur=5. VC EU: delay=7, dur=3. NA: delay=0, dur=N (immediate).
						if ((us->button_state & (1u << BID_TRIGGER)) && !us->dark_inject_fired &&
							us->dark_inject_countdown == 0 && us->dark_inject_active == 0)
						{
							if (us->dark_delay > 0)
								us->dark_inject_countdown = us->dark_delay;
							else
								us->dark_inject_active = us->dark_duration; // delay=0: immediate dark
							us->dark_inject_fired = true;
						}
						if (!(us->button_state & (1u << BID_TRIGGER)))
							us->dark_inject_fired = false;

						if (us->dark_inject_countdown > 0)
						{
							us->dark_inject_countdown--;
							if (us->dark_inject_countdown == 0)
								us->dark_inject_active = us->dark_duration;
						}

						if (us->dark_inject_active > 0)
						{
							out.pos_x = 0;
							out.pos_y = 0;
							us->dark_inject_active--;
						}
						else if (!us->lock_permanent && !us->param_x_initialized && dark)
						{
							// Photodiode fallback: before calibration lock, let the vanilla
							// photodiode pass dark through for boot/init screen calibration.
							// Blocked after lock to prevent "blouse noire" on dark scenes.
							out.pos_x = 0;
							out.pos_y = 0;
						}
					}
					else
					{
						// Standard photodiode for games without dark_inject (NA, TC2, etc).
						// Before calibration lock: dark → pos=(0,0) for calibration to work.
						// After calibration lock: dark BLOCKED for ALL games — prevents false
						// darks from dark scenes/interlaced flicker causing random reloads.
						if (dark && !us->calibration_locked)
						{
							out.pos_x = 0;
							out.pos_y = 0;
						}
					}

					// Deferred lock: confirm PENDING → LOCKED after settle period.
					// During pending, dark/photodiode works normally for calibration.
					// Lock confirms only when the game stops sending SET_PARAMs.
					if (us->calibration_pending && !us->calibration_locked)
					{
						if (++us->pending_poll_count >= us->LOCK_SETTLE_POLLS)
						{
							us->calibration_locked = true;
							us->calibration_pending = false;
							Console.WriteLn("(GunCon2) Port %u: calibration LOCKED%s (settled after %u polls) — dark flag ignored",
								us->port, us->lock_permanent ? " [permanent]" : "",
								us->pending_poll_count);
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

			// Permanent lock: once calibration locks (param_x change), it never unlocks.
			if (gc.lock_permanent)
			{
				lock_permanent = true;
				Console.WriteLn(fmt::format("(GunCon2) Port {}: calibration lock is PERMANENT once triggered", port));
			}

			// Per-game dark injection timing (both 0 = use photodiode).
			if (gc.dark_delay > 0 || gc.dark_duration > 0)
			{
				dark_delay = gc.dark_delay;
				dark_duration = gc.dark_duration;
				Console.WriteLn(fmt::format("(GunCon2) Port {}: dark inject enabled (delay={}, duration={})", port, dark_delay, dark_duration));
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

		// GUI override for dark inject timing (-1 = Auto, use GameConfig. 0+ = actual value).
		const s32 gui_delay = USB::GetConfigInt(si, s->port, TypeName(), "dark_delay", -1);
		const s32 gui_duration = USB::GetConfigInt(si, s->port, TypeName(), "dark_duration", -1);
		if (gui_delay >= 0 || gui_duration >= 0)
		{
			if (gui_delay >= 0)
				s->dark_delay = static_cast<u16>(gui_delay);
			if (gui_duration >= 0)
				s->dark_duration = static_cast<u16>(gui_duration);
			Console.WriteLn("(GunCon2) Port %u: GUI override dark inject (delay=%u, duration=%u)",
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
