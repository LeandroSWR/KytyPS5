#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_OPENXRHEADSET_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_OPENXRHEADSET_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/vrInput.h"

#include <vector>

namespace Libs::Graphics {
struct GraphicContext;
class CommandBuffer;
class Image;
} // namespace Libs::Graphics

// Output to a real headset through OpenXR. Everything here is inert unless the virtual headset is
// enabled (--vr), an OpenXR runtime with a headset is available, and the build includes OpenXR.
// If any of those is missing the emulator keeps showing the eye image in its window only.
namespace Libs::Vr {

// Creates the OpenXR instance. Call before the Vulkan instance exists, because the runtime names
// the extensions and the GPU it needs. Returns true when a headset runtime is ready.
bool OpenXrInit();

// Vulkan extensions the runtime asks for, added only when available.
void OpenXrAppendInstanceExtensions(std::vector<const char*>& out,
                                    const std::vector<vk::ExtensionProperties>& available);
void OpenXrAppendDeviceExtensions(std::vector<const char*>&                 out,
                                  const std::vector<vk::ExtensionProperties>& available);

// The physical device the runtime needs. False when OpenXR is not in use.
bool OpenXrRequiredPhysicalDevice(vk::Instance instance, vk::PhysicalDevice& out);

// Creates the session and starts the frame thread. Call once the device and queue exist.
void OpenXrStartSession(Graphics::GraphicContext& graphics);

// Stops the frame thread and destroys the session. Call before the device is destroyed.
void OpenXrShutdown();

[[nodiscard]] bool OpenXrActive();

// Render thread. Records a copy of the game's headset image into a staging image and publishes
// it for the headset thread. Called after the image was transitioned for transfer reads.
void OpenXrQueueEyeSource(Graphics::CommandBuffer& command, Graphics::Image& source);

} // namespace Libs::Vr

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_OPENXRHEADSET_H_
