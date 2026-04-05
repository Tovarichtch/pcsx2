// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSState.h"
#include <memory>
#include <string>

class GSRenderer : public GSState
{
private:
	bool Merge(int field);
	bool BeginPresentFrame(bool frame_skip);
	void EndPresentFrame();

	u64 m_shader_time_start = 0;

	std::string m_snapshot;
	u32 m_dump_frames = 0;
	u32 m_skipped_duplicate_frames = 0;

	// Tracking draw counters for idle frame detection.
	int m_last_draw_n = 0;
	int m_last_transfer_n = 0;

	// GunCon2 photodiode: async GPU readback to detect calibration blanks.
	//
	// Samples the center pixel of the merged PCRTC output each frame via a
	// ring buffer of download textures (no GPU pipeline stall). Luminance
	// (R+G+B, range 0-765) is compared against two thresholds with hysteresis:
	//
	//   bright->dark : lum < PHOTODIODE_DARK_ENTRY  (44)
	//   dark->bright : lum > PHOTODIODE_DARK_EXIT   (80)
	//
	// Entry threshold: VC JP interlaced HW renders alternating lum=46/181 per
	// field — entry must be below 46. All observed dark frames (calibration
	// blanks, loading screens) peak at lum=43. Margin: 2 units.
	//
	// Exit threshold: minimum observed gameplay luminance is ~83 (VC JP).
	// Hysteresis eliminates oscillation during screen fades where center-pixel
	// lum fluctuates between 30-70 across consecutive frames.
	//
	// The sampled texture is m_current (post-Merge, pre-Interlace/ShadeBoost),
	// corresponding to the raw PCRTC output the hardware photodiode would see.
	//
	// Ring buffer: 3 slots, 2 frames of readback latency (~33ms at 60fps).
	// Write to slot [frame%3], read from slot [(frame+1)%3]. Guard
	// m_photodiode_frame >= 2 ensures we never read stale data after a reset.
	static constexpr u32 PHOTODIODE_RING_SIZE = 3;
	static constexpr u32 PHOTODIODE_DARK_ENTRY = 44;
	static constexpr u32 PHOTODIODE_DARK_EXIT = 80;

	std::unique_ptr<GSDownloadTexture> m_photodiode_ring[PHOTODIODE_RING_SIZE];
	u32 m_photodiode_frame = 0;

	// Diagnostic counters — reset on renderer recreation (game change).

	void UpdatePhotodiode();
	void ResetPhotodiode();

protected:
	GSVector2i m_real_size{0, 0};
	bool m_texture_shuffle = false;
	bool m_process_texture = false;
	bool m_copy_16bit_to_target_shuffle = false;
	bool m_same_group_texture_shuffle = false;
	bool m_downscale_source = false;

	virtual GSTexture* GetOutput(int i, float& scale, int& y_offset) = 0;
	virtual GSTexture* GetFeedbackOutput(float& scale) { return nullptr; }

public:
	GSRenderer();
	virtual ~GSRenderer();

	virtual void Reset(bool hardware_reset) override;

	virtual void Destroy();

	virtual void UpdateRenderFixes();

	virtual void VSync(u32 field, bool registers_written, bool idle_frame);
	virtual bool CanUpscale() { return false; }
	virtual float GetUpscaleMultiplier() { return 1.0f; }
	virtual float GetTextureScaleFactor() { return 1.0f; }
	GSVector2i GetInternalResolution();
	float GetModXYOffset();

	virtual GSTexture* LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size);

	bool IsIdleFrame() const;

	bool SaveSnapshotToMemory(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
		u32* width, u32* height, std::vector<u32>* pixels);

	void QueueSnapshot(const std::string& path, const u32 gsdump_frames);
	void StopGSDump();
	void PresentCurrentFrame();
	bool BeginCapture(std::string filename, const GSVector2i& size = GSVector2i(0, 0));
	void EndCapture();
};

extern std::unique_ptr<GSRenderer> g_gs_renderer;
