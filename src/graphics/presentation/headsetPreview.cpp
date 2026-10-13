#include "graphics/presentation/headsetPreview.h"

#include "common/emulatorConfig.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "libs/hmd2.h"

#include <atomic>
#include <mutex>

namespace Libs::Graphics {

namespace {

std::mutex       g_mutex;
ImageInfo        g_info;
std::atomic_bool g_active {false};

// The game renders each eye side by side into one surface a little larger than the 4000x2040
// panel (4032x2048 observed). Anything of about that size and a single sample is a candidate.
bool LooksLikeHeadsetFrame(const ImageInfo& info) {
	constexpr uint32_t slack = 128;
	return info.extent.width + slack >= Hmd2::PANEL_WIDTH &&
	       info.extent.width <= Hmd2::PANEL_WIDTH + slack &&
	       info.extent.height + slack >= Hmd2::PANEL_HEIGHT &&
	       info.extent.height <= Hmd2::PANEL_HEIGHT + slack && info.extent.depth == 1 &&
	       info.resources.layers == 1 && info.samples == 1 && !info.IsDepth() && !info.IsBlock() &&
	       !info.IsVolume();
}

} // namespace

bool HeadsetPreviewEnabled() {
	static const bool vr = Config::VrEnabled();
	return vr;
}

void HeadsetPreviewNote(const ImageInfo& info) {
	if (!HeadsetPreviewEnabled() || !LooksLikeHeadsetFrame(info)) {
		return;
	}
	std::lock_guard lock(g_mutex);
	g_info = info;
	g_active.store(true, std::memory_order_release);
}

bool HeadsetPreviewActive() {
	return g_active.load(std::memory_order_acquire);
}

bool HeadsetPreviewGet(ImageInfo& out) {
	if (!HeadsetPreviewActive()) {
		return false;
	}
	std::lock_guard lock(g_mutex);
	out = g_info;
	return true;
}

} // namespace Libs::Graphics
