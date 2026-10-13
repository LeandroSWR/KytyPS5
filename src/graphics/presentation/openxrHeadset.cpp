#include "graphics/presentation/openxrHeadset.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "libs/hmd2.h"

#include <cstdlib>

#if defined(_WIN32) && __has_include(<openxr/openxr.h>)
#define KYTY_OPENXR_BUILT 1
#endif

#ifdef KYTY_OPENXR_BUILT

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
// clang-format off
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// clang-format on

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace Libs::Vr {

namespace {

using namespace Libs::Graphics;

constexpr int STAGING_COUNT = 3;

struct Latest {
	int           index = -1;
	uint64_t      tick  = 0;
	vk::Semaphore semaphore;
};

struct State {
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system   = XR_NULL_SYSTEM_ID;
	XrSession  session  = XR_NULL_HANDLE;
	XrSpace    space    = XR_NULL_HANDLE;
	bool       session_running = false;

	std::string instance_extensions;
	std::string device_extensions;
	std::vector<std::string> instance_extension_names;
	std::vector<std::string> device_extension_names;
	vk::PhysicalDevice       physical_device = nullptr;

	PFN_xrGetVulkanGraphicsDeviceKHR       get_graphics_device = nullptr;
	PFN_xrGetVulkanGraphicsRequirementsKHR get_requirements    = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR   get_instance_ext    = nullptr;
	PFN_xrGetVulkanDeviceExtensionsKHR     get_device_ext      = nullptr;

	GraphicContext* graphics = nullptr;
	std::thread     thread;
	std::atomic_bool stop {false};
	std::atomic_bool active {false};

	// Staging images that carry the game's headset image from the render thread to this one.
	std::mutex   mutex;
	VulkanImage  staging[STAGING_COUNT];
	vk::Extent2D staging_extent {};
	vk::Format   staging_format = vk::Format::eUndefined;
	bool         staging_failed = false;
	int          held           = -1;
	Latest       latest;

	// Per-eye swapchains, created on the first frame once the source size is known.
	XrSwapchain                           swapchain[2] {};
	std::vector<XrSwapchainImageVulkanKHR> swapchain_images[2];
	vk::Extent2D                          eye_extent {};

	vk::CommandPool   pool;
	vk::CommandBuffer command;
	vk::Fence         fence;

	// Controller input.
	XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
	XrActionSet    action_set    = XR_NULL_HANDLE;
	XrAction       a_trigger = XR_NULL_HANDLE, a_squeeze = XR_NULL_HANDLE, a_stick = XR_NULL_HANDLE,
	               a_stick_click = XR_NULL_HANDLE, a_primary = XR_NULL_HANDLE,
	               a_secondary = XR_NULL_HANDLE, a_menu = XR_NULL_HANDLE, a_grip = XR_NULL_HANDLE;
	XrPath            hand_path[2] {};
	XrSpace           grip_space[2] {};
	XrSpace           local_space = XR_NULL_HANDLE;
	bool              input_ready = false;
	std::mutex        input_mutex;
	VrControllerInput input[2];
	VrPose            head;
};

State g;

void Say(const std::string& text) {
	Log::WriteToConsoleAndLog(text + "\n");
}

bool Ok(XrResult result, const char* what) {
	if (XR_SUCCEEDED(result)) {
		return true;
	}
	char text[XR_MAX_RESULT_STRING_SIZE] = {};
	if (g.instance != XR_NULL_HANDLE) {
		xrResultToString(g.instance, result, text);
	}
	Say(fmt::format("OpenXR: {} failed: {} ({})", what, text, static_cast<int>(result)));
	return false;
}

std::vector<std::string> SplitWords(const std::string& text) {
	std::vector<std::string> words;
	std::istringstream       stream(text);
	for (std::string word; stream >> word;) {
		words.push_back(word);
	}
	return words;
}

bool Has(const std::vector<vk::ExtensionProperties>& available, const std::string& name) {
	for (const auto& extension: available) {
		if (name == extension.extensionName.data()) {
			return true;
		}
	}
	return false;
}

void AppendMissing(std::vector<const char*>& out, const std::vector<std::string>& names,
                   const std::vector<vk::ExtensionProperties>& available) {
	for (const auto& name: names) {
		if (!Has(available, name)) {
			continue;
		}
		bool present = false;
		for (const auto* existing: out) {
			present |= name == existing;
		}
		if (!present) {
			out.push_back(name.c_str());
		}
	}
}

// ---- staging images (render thread) ----------------------------------------------------------

bool EnsureStaging(const VulkanImage& source) {
	if (g.staging_failed) {
		return false;
	}
	if (g.staging_format != vk::Format::eUndefined) {
		if (source.format != g.staging_format || source.extent.width != g.staging_extent.width ||
		    source.extent.height != g.staging_extent.height) {
			return false;
		}
		return true;
	}
	for (auto& image: g.staging) {
		vk::ImageCreateInfo info {};
		info.imageType     = vk::ImageType::e2D;
		info.format        = source.format;
		info.extent        = vk::Extent3D {source.extent.width, source.extent.height, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = 1;
		info.samples       = vk::SampleCountFlagBits::e1;
		info.tiling        = vk::ImageTiling::eOptimal;
		info.usage         = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
		info.initialLayout = vk::ImageLayout::eUndefined;
		if (!g.graphics->CreateImage(info, image)) {
			Say("OpenXR: could not allocate the headset staging images");
			g.staging_failed = true;
			return false;
		}
	}
	g.staging_format = source.format;
	g.staging_extent = {source.extent.width, source.extent.height};
	Say(fmt::format("OpenXR: eye source {}x{} format {}", g.staging_extent.width,
	                g.staging_extent.height, vk::to_string(g.staging_format)));
	return true;
}

void Transit(vk::CommandBuffer command, VulkanImage& image, vk::ImageLayout layout,
             vk::PipelineStageFlags2 stage, vk::AccessFlags2 access) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask                   = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask                    = stage;
	barrier.dstAccessMask                   = access;
	barrier.oldLayout                       = image.state.layout;
	barrier.newLayout                       = layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.levelCount     = 1;
	barrier.subresourceRange.layerCount     = 1;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state.layout = layout;
}

// ---- headset thread --------------------------------------------------------------------------

vk::Format PickSwapchainFormat() {
	uint32_t count = 0;
	xrEnumerateSwapchainFormats(g.session, 0, &count, nullptr);
	std::vector<int64_t> formats(count);
	xrEnumerateSwapchainFormats(g.session, count, &count, formats.data());
	const vk::Format preferred[] = {vk::Format::eR8G8B8A8Unorm, vk::Format::eB8G8R8A8Unorm,
	                                vk::Format::eR8G8B8A8Srgb, vk::Format::eB8G8R8A8Srgb};
	for (const auto wanted: preferred) {
		for (const auto value: formats) {
			if (value == static_cast<int64_t>(wanted)) {
				return wanted;
			}
		}
	}
	return count != 0 ? static_cast<vk::Format>(formats[0]) : vk::Format::eUndefined;
}

bool CreateEyeSwapchains(vk::Extent2D eye) {
	const auto format = PickSwapchainFormat();
	if (format == vk::Format::eUndefined) {
		return false;
	}
	for (int i = 0; i < 2; i++) {
		XrSwapchainCreateInfo info {XR_TYPE_SWAPCHAIN_CREATE_INFO};
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
		                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		info.format      = static_cast<int64_t>(format);
		info.sampleCount = 1;
		info.width       = eye.width;
		info.height      = eye.height;
		info.faceCount   = 1;
		info.arraySize   = 1;
		info.mipCount    = 1;
		if (!Ok(xrCreateSwapchain(g.session, &info, &g.swapchain[i]), "xrCreateSwapchain")) {
			return false;
		}
		uint32_t count = 0;
		xrEnumerateSwapchainImages(g.swapchain[i], 0, &count, nullptr);
		g.swapchain_images[i].assign(count, XrSwapchainImageVulkanKHR {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
		xrEnumerateSwapchainImages(
		    g.swapchain[i], count, &count,
		    reinterpret_cast<XrSwapchainImageBaseHeader*>(g.swapchain_images[i].data()));
	}
	g.eye_extent = eye;
	Say(fmt::format("OpenXR: eye swapchains {}x{} format {}", eye.width, eye.height,
	                vk::to_string(format)));
	return true;
}

// Blits both eyes out of one staging image into the two swapchain images and waits for the GPU.
bool RenderEyes(const Latest& snapshot, VulkanImage& source) {
	auto&    device = g.graphics->device;
	uint32_t index[2] {};
	for (int eye = 0; eye < 2; eye++) {
		XrSwapchainImageAcquireInfo acquire {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		{
			Common::LockGuard queue_lock(g.graphics->queue_mutex);
			if (!Ok(xrAcquireSwapchainImage(g.swapchain[eye], &acquire, &index[eye]), "acquire")) {
				return false;
			}
		}
		XrSwapchainImageWaitInfo wait {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait.timeout = XR_INFINITE_DURATION;
		if (!Ok(xrWaitSwapchainImage(g.swapchain[eye], &wait), "wait swapchain image")) {
			return false;
		}
	}

	(void)device.resetFences(1, &g.fence);
	g.command.reset();
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	(void)g.command.begin(&begin);

	// The staging image was left in transfer-source layout by the render thread.
	const int64_t eye_width = static_cast<int64_t>(g.staging_extent.width / 2);
	for (int eye = 0; eye < 2; eye++) {
		VulkanImage target;
		target.image        = g.swapchain_images[eye][index[eye]].image;
		target.state.layout = vk::ImageLayout::eColorAttachmentOptimal;
		Transit(g.command, target, vk::ImageLayout::eTransferDstOptimal,
		        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
		vk::ImageBlit region {};
		region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		region.srcOffsets[0]  = vk::Offset3D {static_cast<int32_t>(eye * eye_width), 0, 0};
		region.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>((eye + 1) * eye_width),
		                                      static_cast<int32_t>(g.staging_extent.height), 1};
		region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		region.dstOffsets[1]  = vk::Offset3D {static_cast<int32_t>(g.eye_extent.width),
		                                      static_cast<int32_t>(g.eye_extent.height), 1};
		g.command.blitImage(source.image, vk::ImageLayout::eTransferSrcOptimal, target.image,
		                    vk::ImageLayout::eTransferDstOptimal, 1, &region, vk::Filter::eLinear);
		Transit(g.command, target, vk::ImageLayout::eColorAttachmentOptimal,
		        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
	}
	(void)g.command.end();

	vk::TimelineSemaphoreSubmitInfo timeline {};
	timeline.waitSemaphoreValueCount = 1;
	timeline.pWaitSemaphoreValues    = &snapshot.tick;
	vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
	vk::SubmitInfo         submit {};
	submit.pNext              = &timeline;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores    = &snapshot.semaphore;
	submit.pWaitDstStageMask  = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &g.command;
	vk::Result result;
	{
		Common::LockGuard queue_lock(g.graphics->queue_mutex);
		result = g.graphics->queue.submit(1, &submit, g.fence);
	}
	if (result != vk::Result::eSuccess) {
		Say(fmt::format("OpenXR: headset submit failed: {}", vk::to_string(result)));
		return false;
	}
	(void)device.waitForFences(1, &g.fence, VK_TRUE, UINT64_MAX);

	for (int eye = 0; eye < 2; eye++) {
		XrSwapchainImageReleaseInfo release {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		Common::LockGuard             queue_lock(g.graphics->queue_mutex);
		if (!Ok(xrReleaseSwapchainImage(g.swapchain[eye], &release), "release")) {
			return false;
		}
	}
	return true;
}

XrFovf EyeFov(int eye) {
	const float out    = std::atan(Hmd2::FOV_TAN_OUT);
	const float in     = std::atan(Hmd2::FOV_TAN_IN);
	const float top    = std::atan(Hmd2::FOV_TAN_TOP);
	const float bottom = std::atan(Hmd2::FOV_TAN_BOTTOM);
	XrFovf      fov {};
	// The outer side of each eye is its temporal side: left for the left eye, right for the right.
	fov.angleLeft  = eye == 0 ? -out : -in;
	fov.angleRight = eye == 0 ? in : out;
	fov.angleUp    = top;
	fov.angleDown  = -bottom;
	return fov;
}

void HandleEvents() {
	for (;;) {
		XrEventDataBuffer event {XR_TYPE_EVENT_DATA_BUFFER};
		if (xrPollEvent(g.instance, &event) != XR_SUCCESS) {
			return;
		}
		if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			continue;
		}
		const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
		Say(fmt::format("OpenXR: session state {}", static_cast<int>(changed.state)));
		g.session_state = changed.state;
		switch (changed.state) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo begin {XR_TYPE_SESSION_BEGIN_INFO};
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				g.session_running                  = Ok(xrBeginSession(g.session, &begin), "xrBeginSession");
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				xrEndSession(g.session);
				g.session_running = false;
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				g.session_running = false;
				g.stop            = true;
				break;
			default: break;
		}
	}
}

bool CreateInput() {
	xrStringToPath(g.instance, "/user/hand/left", &g.hand_path[0]);
	xrStringToPath(g.instance, "/user/hand/right", &g.hand_path[1]);
	XrActionSetCreateInfo set_info {XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(set_info.actionSetName, "gameplay");
	std::strcpy(set_info.localizedActionSetName, "Gameplay");
	if (!Ok(xrCreateActionSet(g.instance, &set_info, &g.action_set), "xrCreateActionSet")) {
		return false;
	}
	const auto make = [](XrAction& action, const char* name, XrActionType type) {
		XrActionCreateInfo info {XR_TYPE_ACTION_CREATE_INFO};
		info.actionType          = type;
		info.countSubactionPaths = 2;
		info.subactionPaths      = g.hand_path;
		std::strcpy(info.actionName, name);
		std::strcpy(info.localizedActionName, name);
		return Ok(xrCreateAction(g.action_set, &info, &action), "xrCreateAction");
	};
	if (!make(g.a_trigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT) ||
	    !make(g.a_squeeze, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT) ||
	    !make(g.a_stick, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
	    !make(g.a_stick_click, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
	    !make(g.a_primary, "primary", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
	    !make(g.a_secondary, "secondary", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
	    !make(g.a_menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
	    !make(g.a_grip, "grip_pose", XR_ACTION_TYPE_POSE_INPUT)) {
		return false;
	}
	std::vector<XrActionSuggestedBinding> bindings;
	const auto bind = [&](XrAction action, const std::string& path) {
		XrPath id = XR_NULL_PATH;
		xrStringToPath(g.instance, path.c_str(), &id);
		bindings.push_back({action, id});
	};
	for (const char* side: {"left", "right"}) {
		const std::string base = std::string("/user/hand/") + side + "/input/";
		bind(g.a_trigger, base + "trigger/value");
		bind(g.a_squeeze, base + "squeeze/value");
		bind(g.a_stick, base + "thumbstick");
		bind(g.a_stick_click, base + "thumbstick/click");
		bind(g.a_grip, base + "grip/pose");
	}
	bind(g.a_primary, "/user/hand/left/input/x/click");
	bind(g.a_secondary, "/user/hand/left/input/y/click");
	bind(g.a_menu, "/user/hand/left/input/menu/click");
	bind(g.a_primary, "/user/hand/right/input/a/click");
	bind(g.a_secondary, "/user/hand/right/input/b/click");
	XrInteractionProfileSuggestedBinding suggested {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	xrStringToPath(g.instance, "/interaction_profiles/oculus/touch_controller",
	               &suggested.interactionProfile);
	suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
	suggested.suggestedBindings      = bindings.data();
	if (!Ok(xrSuggestInteractionProfileBindings(g.instance, &suggested), "suggest bindings")) {
		return false;
	}
	XrSessionActionSetsAttachInfo attach {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets      = &g.action_set;
	if (!Ok(xrAttachSessionActionSets(g.session, &attach), "attach action sets")) {
		return false;
	}
	for (int hand = 0; hand < 2; hand++) {
		XrActionSpaceCreateInfo space {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		space.action            = g.a_grip;
		space.subactionPath     = g.hand_path[hand];
		space.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
		if (!Ok(xrCreateActionSpace(g.session, &space, &g.grip_space[hand]), "action space")) {
			return false;
		}
	}
	XrReferenceSpaceCreateInfo local {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	local.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;
	local.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
	if (!Ok(xrCreateReferenceSpace(g.session, &local, &g.local_space), "local space")) {
		return false;
	}
	g.input_ready = true;
	Say("OpenXR: controller input ready (Touch bindings)");
	return true;
}

void PollInput(XrTime time) {
	if (!g.input_ready) {
		return;
	}
	if (g.session_state != XR_SESSION_STATE_FOCUSED) {
		std::lock_guard lock(g.input_mutex);
		g.input[0] = {};
		g.input[1] = {};
		g.head     = {};
		return;
	}
	XrActiveActionSet active {g.action_set, XR_NULL_PATH};
	XrActionsSyncInfo sync {XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets      = &active;
	if (!Ok(xrSyncActions(g.session, &sync), "xrSyncActions")) {
		return;
	}
	VrControllerInput state[2];
	for (int hand = 0; hand < 2; hand++) {
		const auto info = [&](XrAction action) {
			XrActionStateGetInfo get {XR_TYPE_ACTION_STATE_GET_INFO};
			get.action        = action;
			get.subactionPath = g.hand_path[hand];
			return get;
		};
		const auto read_float = [&](XrAction action) {
			XrActionStateFloat value {XR_TYPE_ACTION_STATE_FLOAT};
			const auto         get = info(action);
			xrGetActionStateFloat(g.session, &get, &value);
			return value.isActive ? value.currentState : 0.0f;
		};
		const auto read_bool = [&](XrAction action) {
			XrActionStateBoolean value {XR_TYPE_ACTION_STATE_BOOLEAN};
			const auto           get = info(action);
			xrGetActionStateBoolean(g.session, &get, &value);
			return value.isActive && value.currentState == XR_TRUE;
		};
		auto& out       = state[hand];
		out.trigger     = read_float(g.a_trigger);
		out.squeeze     = read_float(g.a_squeeze);
		out.stick_click = read_bool(g.a_stick_click);
		out.primary     = read_bool(g.a_primary);
		out.secondary   = read_bool(g.a_secondary);
		out.menu        = read_bool(g.a_menu);
		XrActionStateVector2f stick {XR_TYPE_ACTION_STATE_VECTOR2F};
		const auto            stick_get = info(g.a_stick);
		xrGetActionStateVector2f(g.session, &stick_get, &stick);
		if (stick.isActive) {
			out.stick_x = stick.currentState.x;
			out.stick_y = stick.currentState.y;
		}
		XrActionStatePose pose {XR_TYPE_ACTION_STATE_POSE};
		const auto        pose_get = info(g.a_grip);
		xrGetActionStatePose(g.session, &pose_get, &pose);
		out.connected = pose.isActive == XR_TRUE;
		XrSpaceLocation location {XR_TYPE_SPACE_LOCATION};
		if (out.connected &&
		    XR_SUCCEEDED(xrLocateSpace(g.grip_space[hand], g.local_space, time, &location)) &&
		    (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
			out.pose_valid     = true;
			out.orientation[0] = location.pose.orientation.x;
			out.orientation[1] = location.pose.orientation.y;
			out.orientation[2] = location.pose.orientation.z;
			out.orientation[3] = location.pose.orientation.w;
			out.position[0]    = location.pose.position.x;
			out.position[1]    = location.pose.position.y;
			out.position[2]    = location.pose.position.z;
		}
	}
	static uint32_t previous_mask[2] = {0, 0};
	for (int hand = 0; hand < 2; hand++) {
		const auto&    in   = state[hand];
		const uint32_t mask = (in.trigger > 0.5f ? 1u : 0u) | (in.squeeze > 0.5f ? 2u : 0u) |
		                      (in.stick_click ? 4u : 0u) | (in.primary ? 8u : 0u) |
		                      (in.secondary ? 16u : 0u) | (in.menu ? 32u : 0u) |
		                      (in.connected ? 64u : 0u);
		if (mask != previous_mask[hand]) {
			Say(fmt::format("OpenXR: {} controller buttons 0x{:x} (trigger {:.2f})",
			                hand == 0 ? "left" : "right", mask, in.trigger));
			previous_mask[hand] = mask;
		}
	}
	VrPose          head;
	XrSpaceLocation head_location {XR_TYPE_SPACE_LOCATION};
	if (g.space != XR_NULL_HANDLE && XR_SUCCEEDED(xrLocateSpace(g.space, g.local_space, time, &head_location)) &&
	    (head_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
		head.valid          = true;
		head.orientation[0] = head_location.pose.orientation.x;
		head.orientation[1] = head_location.pose.orientation.y;
		head.orientation[2] = head_location.pose.orientation.z;
		head.orientation[3] = head_location.pose.orientation.w;
		head.position[0]    = head_location.pose.position.x;
		head.position[1]    = head_location.pose.position.y;
		head.position[2]    = head_location.pose.position.z;
	}
	std::lock_guard lock(g.input_mutex);
	g.input[0] = state[0];
	g.input[1] = state[1];
	g.head     = head;
}

void ThreadMain() {
	while (!g.stop) {
		HandleEvents();
		if (!g.session_running) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		XrFrameWaitInfo wait {XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState    state {XR_TYPE_FRAME_STATE};
		if (!Ok(xrWaitFrame(g.session, &wait, &state), "xrWaitFrame")) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		PollInput(state.predictedDisplayTime);

		XrFrameBeginInfo begin {XR_TYPE_FRAME_BEGIN_INFO};
		{
			Common::LockGuard queue_lock(g.graphics->queue_mutex);
			(void)Ok(xrBeginFrame(g.session, &begin), "xrBeginFrame");
		}

		XrCompositionLayerProjectionView views[2] {};
		XrCompositionLayerProjection     layer {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		const XrCompositionLayerBaseHeader* layers[1] = {};
		uint32_t                            layer_count = 0;

		Latest snapshot;
		bool   have = false;
		if (state.shouldRender) {
			std::lock_guard lock(g.mutex);
			if (g.latest.index >= 0) {
				snapshot = g.latest;
				g.held   = snapshot.index;
				have     = true;
			}
		}
		if (have) {
			bool rendered = true;
			if (g.swapchain[0] == XR_NULL_HANDLE) {
				const vk::Extent2D eye {g.staging_extent.width / 2, g.staging_extent.height};
				rendered = CreateEyeSwapchains(eye);
			}
			if (rendered) {
				rendered = RenderEyes(snapshot, g.staging[snapshot.index]);
			}
			{
				std::lock_guard lock(g.mutex);
				g.held = -1;
			}
			if (rendered) {
				// Head-locked: the picture stays in front of the user. Eye poses come from the
				// runtime so the two views keep their natural separation.
				XrViewLocateInfo locate {XR_TYPE_VIEW_LOCATE_INFO};
				locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				locate.displayTime           = state.predictedDisplayTime;
				locate.space                 = g.space;
				XrViewState located_state {XR_TYPE_VIEW_STATE};
				XrView      located[2] {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
				uint32_t    located_count = 0;
				(void)xrLocateViews(g.session, &locate, &located_state, 2, &located_count, located);
				for (int eye = 0; eye < 2; eye++) {
					views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
					views[eye].pose = located[eye].pose;
					if ((located_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
						views[eye].pose.orientation = {0, 0, 0, 1};
						views[eye].pose.position    = {eye == 0 ? -0.0315f : 0.0315f, 0, 0};
					}
					views[eye].fov                         = EyeFov(eye);
					views[eye].subImage.swapchain          = g.swapchain[eye];
					views[eye].subImage.imageRect.offset   = {0, 0};
					views[eye].subImage.imageRect.extent   = {static_cast<int32_t>(g.eye_extent.width),
					                                          static_cast<int32_t>(g.eye_extent.height)};
					views[eye].subImage.imageArrayIndex    = 0;
				}
				layer.space     = g.space;
				layer.viewCount = 2;
				layer.views     = views;
				layers[0]       = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
				layer_count     = 1;
			}
		}

		XrFrameEndInfo end {XR_TYPE_FRAME_END_INFO};
		end.displayTime          = state.predictedDisplayTime;
		end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		end.layerCount           = layer_count;
		end.layers               = layer_count != 0 ? layers : nullptr;
		Common::LockGuard queue_lock(g.graphics->queue_mutex);
		(void)Ok(xrEndFrame(g.session, &end), "xrEndFrame");
	}
}

} // namespace

bool OpenXrInit() {
	if (!Config::VrEnabled()) {
		return false;
	}
	if (const char* off = std::getenv("KYTY_OPENXR"); off != nullptr && std::strcmp(off, "0") == 0) {
		return false;
	}
	if (g.instance != XR_NULL_HANDLE) {
		return true;
	}
	const char* extensions[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
	XrInstanceCreateInfo info {XR_TYPE_INSTANCE_CREATE_INFO};
	std::strncpy(info.applicationInfo.applicationName, "KytyPS5", XR_MAX_APPLICATION_NAME_SIZE - 1);
	std::strncpy(info.applicationInfo.engineName, "Kyty", XR_MAX_ENGINE_NAME_SIZE - 1);
	info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	info.enabledExtensionCount      = 1;
	info.enabledExtensionNames      = extensions;
	if (XR_FAILED(xrCreateInstance(&info, &g.instance))) {
		g.instance = XR_NULL_HANDLE;
		Say("OpenXR: no runtime or headset available; showing the eye image in the window only");
		return false;
	}
	XrSystemGetInfo system {XR_TYPE_SYSTEM_GET_INFO};
	system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!Ok(xrGetSystem(g.instance, &system, &g.system), "xrGetSystem")) {
		xrDestroyInstance(g.instance);
		g.instance = XR_NULL_HANDLE;
		return false;
	}
	XrSystemProperties properties {XR_TYPE_SYSTEM_PROPERTIES};
	xrGetSystemProperties(g.instance, g.system, &properties);
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanGraphicsDeviceKHR",
	                      reinterpret_cast<PFN_xrVoidFunction*>(&g.get_graphics_device));
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanGraphicsRequirementsKHR",
	                      reinterpret_cast<PFN_xrVoidFunction*>(&g.get_requirements));
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanInstanceExtensionsKHR",
	                      reinterpret_cast<PFN_xrVoidFunction*>(&g.get_instance_ext));
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanDeviceExtensionsKHR",
	                      reinterpret_cast<PFN_xrVoidFunction*>(&g.get_device_ext));
	if (g.get_graphics_device == nullptr || g.get_requirements == nullptr ||
	    g.get_instance_ext == nullptr || g.get_device_ext == nullptr) {
		Say("OpenXR: the runtime does not provide XR_KHR_vulkan_enable");
		xrDestroyInstance(g.instance);
		g.instance = XR_NULL_HANDLE;
		return false;
	}
	XrGraphicsRequirementsVulkanKHR requirements {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	if (!Ok(g.get_requirements(g.instance, g.system, &requirements), "graphics requirements")) {
		xrDestroyInstance(g.instance);
		g.instance = XR_NULL_HANDLE;
		return false;
	}
	char     buffer[4096] = {};
	uint32_t length       = 0;
	if (XR_SUCCEEDED(g.get_instance_ext(g.instance, g.system, sizeof(buffer), &length, buffer))) {
		g.instance_extension_names = SplitWords(buffer);
	}
	std::memset(buffer, 0, sizeof(buffer));
	if (XR_SUCCEEDED(g.get_device_ext(g.instance, g.system, sizeof(buffer), &length, buffer))) {
		g.device_extension_names = SplitWords(buffer);
	}
	Say(fmt::format("OpenXR: headset '{}' ready", properties.systemName));
	return true;
}

void OpenXrAppendInstanceExtensions(std::vector<const char*>&                 out,
                                    const std::vector<vk::ExtensionProperties>& available) {
	if (g.instance != XR_NULL_HANDLE) {
		AppendMissing(out, g.instance_extension_names, available);
	}
}

void OpenXrAppendDeviceExtensions(std::vector<const char*>&                 out,
                                  const std::vector<vk::ExtensionProperties>& available) {
	if (g.instance != XR_NULL_HANDLE) {
		AppendMissing(out, g.device_extension_names, available);
	}
}

bool OpenXrRequiredPhysicalDevice(vk::Instance instance, vk::PhysicalDevice& out) {
	if (g.instance == XR_NULL_HANDLE) {
		return false;
	}
	VkPhysicalDevice device = VK_NULL_HANDLE;
	if (!Ok(g.get_graphics_device(g.instance, g.system, static_cast<VkInstance>(instance), &device),
	        "xrGetVulkanGraphicsDeviceKHR") ||
	    device == VK_NULL_HANDLE) {
		return false;
	}
	out = vk::PhysicalDevice(device);
	return true;
}

void OpenXrStartSession(GraphicContext& graphics) {
	if (g.instance == XR_NULL_HANDLE || g.session != XR_NULL_HANDLE) {
		return;
	}
	g.graphics = &graphics;
	XrGraphicsBindingVulkanKHR binding {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	binding.instance         = static_cast<VkInstance>(graphics.instance);
	binding.physicalDevice   = static_cast<VkPhysicalDevice>(graphics.physical_device);
	binding.device           = static_cast<VkDevice>(graphics.device);
	binding.queueFamilyIndex = graphics.queue_family;
	binding.queueIndex       = 0;
	XrSessionCreateInfo info {XR_TYPE_SESSION_CREATE_INFO};
	info.next     = &binding;
	info.systemId = g.system;
	if (!Ok(xrCreateSession(g.instance, &info, &g.session), "xrCreateSession")) {
		g.session = XR_NULL_HANDLE;
		return;
	}
	XrReferenceSpaceCreateInfo space {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_VIEW;
	space.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
	if (!Ok(xrCreateReferenceSpace(g.session, &space, &g.space), "xrCreateReferenceSpace")) {
		return;
	}

	(void)CreateInput();

	vk::CommandPoolCreateInfo pool {};
	pool.queueFamilyIndex = graphics.queue_family;
	pool.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	(void)graphics.device.createCommandPool(&pool, nullptr, &g.pool);
	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = g.pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	(void)graphics.device.allocateCommandBuffers(&allocate, &g.command);
	vk::FenceCreateInfo fence {};
	(void)graphics.device.createFence(&fence, nullptr, &g.fence);

	g.active = true;
	g.stop   = false;
	g.thread = std::thread(ThreadMain);
	Say("OpenXR: session created, waiting for the headset to become ready");
}

void OpenXrShutdown() {
	if (g.instance == XR_NULL_HANDLE) {
		return;
	}
	g.active = false;
	g.stop   = true;
	if (g.session != XR_NULL_HANDLE && g.session_running) {
		xrRequestExitSession(g.session);
	}
	if (g.thread.joinable()) {
		g.thread.join();
	}
	if (g.graphics != nullptr && g.graphics->device != nullptr) {
		(void)g.graphics->device.waitIdle();
	}
	for (auto& swapchain: g.swapchain) {
		if (swapchain != XR_NULL_HANDLE) {
			xrDestroySwapchain(swapchain);
			swapchain = XR_NULL_HANDLE;
		}
	}
	for (auto& space: g.grip_space) {
		if (space != XR_NULL_HANDLE) {
			xrDestroySpace(space);
			space = XR_NULL_HANDLE;
		}
	}
	if (g.local_space != XR_NULL_HANDLE) {
		xrDestroySpace(g.local_space);
		g.local_space = XR_NULL_HANDLE;
	}
	if (g.space != XR_NULL_HANDLE) {
		xrDestroySpace(g.space);
		g.space = XR_NULL_HANDLE;
	}
	if (g.session != XR_NULL_HANDLE) {
		xrDestroySession(g.session);
		g.session = XR_NULL_HANDLE;
	}
	if (g.graphics != nullptr && g.graphics->device != nullptr) {
		for (auto& image: g.staging) {
			if (image.image != nullptr) {
				g.graphics->DeleteImage(image);
			}
		}
		if (g.fence) {
			g.graphics->device.destroyFence(g.fence, nullptr);
		}
		if (g.pool) {
			g.graphics->device.destroyCommandPool(g.pool, nullptr);
		}
	}
	xrDestroyInstance(g.instance);
	g.instance = XR_NULL_HANDLE;
}

bool OpenXrActive() {
	return g.active.load();
}

bool OpenXrGetHeadPose(VrPose& out) {
	if (!g.active.load() || !g.input_ready) {
		return false;
	}
	std::lock_guard lock(g.input_mutex);
	out = g.head;
	return out.valid;
}

bool OpenXrGetController(int hand, VrControllerInput& out) {
	if (!g.active.load() || !g.input_ready || hand < 0 || hand > 1) {
		return false;
	}
	std::lock_guard lock(g.input_mutex);
	out = g.input[hand];
	return true;
}

void OpenXrQueueEyeSource(CommandBuffer& command, Image& source) {
	if (!g.active.load()) {
		return;
	}
	std::lock_guard lock(g.mutex);
	if (!EnsureStaging(source.backing)) {
		return;
	}
	int index = -1;
	for (int i = 0; i < STAGING_COUNT; i++) {
		if (i != g.held && i != g.latest.index) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		return;
	}
	auto& staging = g.staging[index];
	command.EndRendering();
	auto vk_command = command.Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               vk_command);
	Transit(vk_command, staging, vk::ImageLayout::eTransferDstOptimal,
	        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.extent         = {g.staging_extent.width, g.staging_extent.height, 1};
	vk_command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, staging.image,
	                     vk::ImageLayout::eTransferDstOptimal, copy);
	Transit(vk_command, staging, vk::ImageLayout::eTransferSrcOptimal,
	        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);

	auto& scheduler     = command.GetContext().GetCommandScheduler();
	g.latest.index      = index;
	g.latest.tick       = scheduler.CurrentTick();
	g.latest.semaphore  = scheduler.GetMasterSemaphore().Handle();
}

} // namespace Libs::Vr

#else // no OpenXR in this build

namespace Libs::Vr {

bool OpenXrInit() {
	return false;
}
void OpenXrAppendInstanceExtensions(std::vector<const char*>&, const std::vector<vk::ExtensionProperties>&) {}
void OpenXrAppendDeviceExtensions(std::vector<const char*>&, const std::vector<vk::ExtensionProperties>&) {}
bool OpenXrRequiredPhysicalDevice(vk::Instance, vk::PhysicalDevice&) {
	return false;
}
void OpenXrStartSession(Graphics::GraphicContext&) {}
void OpenXrShutdown() {}
bool OpenXrActive() {
	return false;
}
void OpenXrQueueEyeSource(Graphics::CommandBuffer&, Graphics::Image&) {}
bool OpenXrGetController(int, VrControllerInput&) {
	return false;
}
bool OpenXrGetHeadPose(VrPose&) {
	return false;
}

} // namespace Libs::Vr

#endif
