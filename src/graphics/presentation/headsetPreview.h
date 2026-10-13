#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_HEADSETPREVIEW_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_HEADSETPREVIEW_H_

namespace Libs::Graphics {

struct ImageInfo;

// VR only. The renderer reports every panel-sized color image the guest writes. The headset flip
// shows the latest one in the window, so the eye images are visible without a headset. All
// functions do nothing unless the virtual headset is enabled, so other titles are unaffected.
void HeadsetPreviewNote(const ImageInfo& info);

// True only when the virtual headset is enabled. Check it before gathering an image to report.
[[nodiscard]] bool HeadsetPreviewEnabled();

// True once a candidate image has been seen.
[[nodiscard]] bool HeadsetPreviewActive();

// Copies the latest candidate. Returns false when there is none.
[[nodiscard]] bool HeadsetPreviewGet(ImageInfo& out);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_HEADSETPREVIEW_H_
