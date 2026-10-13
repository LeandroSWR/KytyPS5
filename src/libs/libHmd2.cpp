#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/tile.h"
#include "libs/agc.h"
#include "libs/errno.h"
#include "libs/hmd2.h"
#include "libs/libs.h"
#include "kernel/pthread.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

namespace Libs {

LIB_VERSION("Hmd2", 1, "Hmd2", 1, 1);

namespace Hmd2 {

constexpr int32_t ERROR_ALREADY_INITIALIZED = -1972240383; // 0x8a720001
constexpr int32_t ERROR_NOT_INITIALIZED     = -1972240382; // 0x8a720002
constexpr int32_t ERROR_PARAMETER_NULL      = -1972240376; // 0x8a720008
constexpr int32_t ERROR_REPROJECTION_ALREADY_INITIALIZED = -1972240372; // 0x8a72000c
constexpr int32_t ERROR_REPROJECTION_IN_VR_MODE = -1972240356; // 0x8a72001c

struct SceHmd2InitializeParam {
	void*   reserved0;
	uint8_t reserved[8];
};

struct SceHmd2DeviceInformation {
	uint32_t status;
	uint32_t reserve0;
	struct {
		struct {
			uint32_t width;
			uint32_t height;
		} panelResolution;
		struct {
			uint16_t refreshRate90Hz;
			uint16_t refreshRate120Hz;
		} flipToDisplayLatency;
	} deviceInfo;
	uint8_t hmuMount;
	uint8_t lensSeparationDistance;
	uint8_t reserve1[2];
	float   virtualImageDistance;
};

struct SceHmd2FieldOfView {
	float tanOut;
	float tanIn;
	float tanTop;
	float tanBottom;
};

struct SceHmd2ReprojectionInitializeParam {
	void*    pReprojectionBuff;
	void*    pDisplayBuff;
	int32_t  threadPriority;
	int32_t  cpuAffinityMask;
	uint32_t pipeId;
	uint32_t queueId;
	uint32_t type;
	void*    pSeeThroughBuff;
	int32_t  reprojectionTiming;
	uint32_t reserved[5];
};

static_assert(sizeof(SceHmd2InitializeParam) == 16);
static_assert(offsetof(SceHmd2InitializeParam, reserved) == 8);
static_assert(sizeof(SceHmd2DeviceInformation) == 28);
static_assert(offsetof(SceHmd2DeviceInformation, deviceInfo) == 8);
static_assert(offsetof(SceHmd2DeviceInformation, hmuMount) == 20);
static_assert(offsetof(SceHmd2DeviceInformation, virtualImageDistance) == 24);
static_assert(sizeof(SceHmd2FieldOfView) == 16);
static_assert(alignof(SceHmd2FieldOfView) == 4);
static_assert(sizeof(SceHmd2ReprojectionInitializeParam) == 72);
static_assert(alignof(SceHmd2ReprojectionInitializeParam) == 8);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, threadPriority) == 16);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, type) == 32);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, pSeeThroughBuff) == 40);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, reprojectionTiming) == 48);
static_assert(offsetof(SceHmd2ReprojectionInitializeParam, reserved) == 52);

static std::atomic<bool> g_initialized = false;
static std::mutex g_reprojection_mutex;
static std::atomic<ReprojectionState*> g_reprojection_state {nullptr};
static std::atomic<int32_t> g_hmd_handle {0};

constexpr int32_t HMD_HANDLE = 0x0f000001;

ReprojectionState* GetReprojectionState() {
	return g_reprojection_state.load(std::memory_order_acquire);
}

static Graphics::SizeAlign ReprojectionDisplaySize() {
	// The host display target is one uncompressed tiled 10-bit panel surface.
	Graphics::TileSizeAlign size {};
	Graphics::TileGetTextureTotalSize(Graphics::Prospero::BufferFormat::k10_10_10_2UNorm,
	                                  PANEL_WIDTH, PANEL_HEIGHT, 1, 1,
	                                  Graphics::Prospero::TileMode::kRenderTarget, false, size);
	EXIT_IF(size.size == 0 || size.align == 0);
	return {size.size, size.align};
}

static int32_t KYTY_SYSV_ABI Hmd2Initialize(const SceHmd2InitializeParam* param) {
	PRINT_NAME();
	if (param == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	if (g_initialized.exchange(true)) {
		return ERROR_ALREADY_INITIALIZED;
	}
	LOGF("Hmd2: initialized, virtual headset=%s\n", Config::VrEnabled() ? "enabled" : "disabled");
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2GetDeviceInformation(SceHmd2DeviceInformation* info) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (info == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	*info = {};
	if (Config::VrEnabled()) {
		// Virtual headset defaults: READY, MOUNT, 63 mm lens separation.
		info->hmuMount               = 1;
		info->lensSeparationDistance = 63;
		// Headset characteristics: 4000x2040 panel and 2 m virtual image distance.
		info->deviceInfo.panelResolution = {PANEL_WIDTH, PANEL_HEIGHT};
		info->virtualImageDistance       = 2.0f;
		// Host presentation has no emulated headset scan-out delay; latency remains zero.
	} else {
		info->status = 2; // SCE_HMD2_DEVICE_STATUS_NOT_DETECTED
	}
	LOGF("Hmd2: device status=%u panel=%ux%u mounted=%u\n", info->status,
	     info->deviceInfo.panelResolution.width, info->deviceInfo.panelResolution.height,
	     info->hmuMount);
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2GetFieldOfViewWithoutHandle(SceHmd2FieldOfView* fov) {
	PRINT_NAME();
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	if (fov == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	// Virtual ER15 field of view: 55/42.5/51.2/51.2 degrees.
	// These tangents describe that geometry, not bit-exact firmware output.
	*fov = {FOV_TAN_OUT, FOV_TAN_IN, FOV_TAN_TOP, FOV_TAN_BOTTOM};
	LOGF("Hmd2: FOV tangents out=%f in=%f top=%f bottom=%f\n", fov->tanOut, fov->tanIn,
	     fov->tanTop, fov->tanBottom);
	return OK;
}

static Graphics::SizeAlign KYTY_SYSV_ABI Hmd2ReprojectionQueryBufferSizeAlign() {
	const Graphics::SizeAlign size {sizeof(ReprojectionState), alignof(ReprojectionState)};
	LOGF("Hmd2: reprojection work size=%" PRIu64 " alignment=%zu\n", size.m_size, size.m_align);
	return size;
}

static Graphics::SizeAlign KYTY_SYSV_ABI Hmd2ReprojectionQueryDisplayBufferSizeAlign() {
	const auto size = ReprojectionDisplaySize();
	LOGF("Hmd2: reprojection display size=%" PRIu64 " alignment=%zu\n", size.m_size,
	     size.m_align);
	return size;
}

static int32_t KYTY_SYSV_ABI
Hmd2ReprojectionInitialize(const SceHmd2ReprojectionInitializeParam* param, void*) {
	std::scoped_lock lock {g_reprojection_mutex};
	if (GetReprojectionState() != nullptr) {
		return ERROR_REPROJECTION_ALREADY_INITIALIZED;
	}
	if (param == nullptr || param->pReprojectionBuff == nullptr || param->pDisplayBuff == nullptr) {
		return ERROR_PARAMETER_NULL;
	}
	EXIT_NOT_IMPLEMENTED(param->pSeeThroughBuff != nullptr);
	const auto display_size = ReprojectionDisplaySize();
	auto* state = new (param->pReprojectionBuff) ReprojectionState;
	if (param->reprojectionTiming != 0) {
		state->timing_us.store(static_cast<uint32_t>(param->reprojectionTiming),
		                      std::memory_order_relaxed);
	}
	g_reprojection_state.store(state, std::memory_order_release);
	LOGF("Hmd2: reprojection initialized work=%p display=%p size=%" PRIu64 " timing=%u\n",
	     static_cast<void*>(state), param->pDisplayBuff, display_size.m_size,
	     state->timing_us.load(std::memory_order_relaxed));
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionEnableVrMode(uint64_t output_mode) {
	auto* state = GetReprojectionState();
	if (state == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	if (output_mode != OUTPUT_MODE_89_91HZ && output_mode != OUTPUT_MODE_119_88HZ) {
		return VideoOut::VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	if (!Config::VrEnabled()) {
		return VideoOut::VIDEO_OUT_ERROR_NO_DEVICE;
	}
	uint64_t expected = 0;
	if (!state->output_mode.compare_exchange_strong(expected, output_mode)) {
		return ERROR_REPROJECTION_IN_VR_MODE;
	}
	LOGF("Hmd2: VR output mode=0x%" PRIx64 "\n", output_mode);
	return OK;
}

// sceHmd2Open
static int32_t KYTY_SYSV_ABI Hmd2Open(int32_t user_id, int32_t type, int32_t index,
                                       const void* param) {
	PRINT_NAME();
	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n"
	     "\t param   = %p\n",
	     user_id, type, index, param);
	if (!g_initialized.load()) {
		return ERROR_NOT_INITIALIZED;
	}
	int32_t expected = 0;
	g_hmd_handle.compare_exchange_strong(expected, HMD_HANDLE);
	const auto handle = g_hmd_handle.load();
	LOGF("Hmd2: open handle=%d\n", handle);
	return handle;
}

// sceHmd2ReprojectionBeginFrame
static int32_t KYTY_SYSV_ABI Hmd2ReprojectionBeginFrame(uint64_t handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%" PRIx64 "\n", handle);
	return OK;
}

static bool GuestPointer(uint64_t value) {
	// Guest allocations start at 8 GiB. Handles, including the virtual headset, stay below that.
	return value >= 0x200000000ull;
}

static uint64_t* PredictedTimeOutput(uint64_t arg0, uint64_t arg1) {
	if (GuestPointer(arg1)) {
		return reinterpret_cast<uint64_t*>(arg1);
	}
	if (GuestPointer(arg0)) {
		return reinterpret_cast<uint64_t*>(arg0);
	}
	return nullptr;
}

// sceHmd2ReprojectionGetPredictedDisplayTime
static int32_t KYTY_SYSV_ABI Hmd2ReprojectionGetPredictedDisplayTime(uint64_t arg0, uint64_t arg1) {
	PRINT_NAME();
	auto* out = PredictedTimeOutput(arg0, arg1);
	uint64_t frame_us = 11111;
	if (const auto* state = GetReprojectionState();
	    state != nullptr && state->output_mode.load() == OUTPUT_MODE_119_88HZ) {
		frame_us = 8333;
	}
	const auto predicted = LibKernel::KernelGetProcessTime() + frame_us;
	LOGF("\t arg0 = 0x%" PRIx64 "\n"
	     "\t arg1 = 0x%" PRIx64 "\n"
	     "\t time = %" PRIu64 "\n",
	     arg0, arg1, predicted);
	if (out != nullptr) {
		*out = predicted;
	}
	return OK;
}

// hA9LshbSkzw. Called once during headset setup. The symbol name is not identified yet.
static int32_t KYTY_SYSV_ABI Hmd2UnknownHA9LshbSkzw(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                                    uint64_t arg3) {
	PRINT_NAME();
	LOGF("\t hA9LshbSkzw args = (0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ")\n",
	     arg0, arg1, arg2, arg3);
	return OK;
}

static void* LastGuestPointer(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
	void* found = nullptr;
	for (const uint64_t value : {arg0, arg1, arg2}) {
		if (GuestPointer(value)) {
			found = reinterpret_cast<void*>(value);
		}
	}
	return found;
}

static bool DeclaredStructSize(const void* value, uint32_t* size) {
	if (value == nullptr) {
		return false;
	}
	const auto declared = *static_cast<const uint32_t*>(value);
	if (declared < 8 || declared > 512 || declared % 4 != 0) {
		return false;
	}
	*size = declared;
	return true;
}

// sceHmd2GazeGetResult. Writes only when the caller prefixed the result with its size.
static int32_t KYTY_SYSV_ABI Hmd2GazeGetResult(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
	PRINT_NAME();
	auto* result       = LastGuestPointer(arg0, arg1, arg2);
	uint32_t declared  = 0;
	const auto sized   = DeclaredStructSize(result, &declared);
	LOGF("\t arg0     = 0x%" PRIx64 "\n"
	     "\t arg1     = 0x%" PRIx64 "\n"
	     "\t arg2     = 0x%" PRIx64 "\n"
	     "\t declared = %u\n",
	     arg0, arg1, arg2, declared);
	if (!sized) {
		return OK;
	}
	auto* bytes            = static_cast<uint8_t*>(result);
	const auto preserved   = *reinterpret_cast<uint32_t*>(bytes);
	std::memset(bytes, 0, declared);
	*reinterpret_cast<uint32_t*>(bytes) = preserved;
	if (declared >= 8) {
		*reinterpret_cast<int32_t*>(bytes + 4) = 1;
	}
	if (declared >= 16) {
		*reinterpret_cast<uint64_t*>(bytes + 8) = LibKernel::KernelGetProcessTime();
	}
	// size, status, timestamp, then a forward gaze in head space.
	if (declared >= 28) {
		*reinterpret_cast<float*>(bytes + 24) = -1.0f;
	}
	return OK;
}

// sceHmd2ReprojectionSetParam. Per-frame companion of GetPredictedDisplayTime.
static int32_t KYTY_SYSV_ABI Hmd2ReprojectionSetParam(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
	PRINT_NAME();
	LOGF("\t arg0 = 0x%" PRIx64 "\n"
	     "\t arg1 = 0x%" PRIx64 "\n"
	     "\t arg2 = 0x%" PRIx64 "\n",
	     arg0, arg1, arg2);
	return OK;
}

// retc+-uRMhk. The symbol name is not identified yet.
static int32_t KYTY_SYSV_ABI Hmd2UnknownRetc(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                             uint64_t arg3) {
	PRINT_NAME();
	LOGF("\t retc+-uRMhk args = (0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ")\n",
	     arg0, arg1, arg2, arg3);
	return OK;
}

static int32_t KYTY_SYSV_ABI Hmd2ReprojectionSetTiming(int32_t timing) {
	auto* state = GetReprojectionState();
	if (state == nullptr) {
		return ERROR_REPROJECTION_NOT_INITIALIZED;
	}
	EXIT_NOT_IMPLEMENTED(timing < 2000 || timing > 7000);
	const auto rounded = static_cast<uint32_t>((timing + 99) / 100 * 100);
	state->timing_us.store(rounded, std::memory_order_release);
	LOGF("Hmd2: reprojection timing=%u us\n", rounded);
	return OK;
}

} // namespace Hmd2

LIB_DEFINE(InitHmd2_1) {
	LIB_FUNC("c812oYs7Vsc", Hmd2::Hmd2Initialize);
	LIB_FUNC("bIi4YUfSRys", Hmd2::Hmd2GetDeviceInformation);
	LIB_FUNC("gF8+lvc7GuQ", Hmd2::Hmd2GetFieldOfViewWithoutHandle);
	LIB_FUNC("U-CnbmeyYaA", Hmd2::Hmd2ReprojectionQueryBufferSizeAlign);
	LIB_FUNC("-C2nkoEYOnU", Hmd2::Hmd2ReprojectionQueryDisplayBufferSizeAlign);
	LIB_FUNC("C0rPwER-yxg", Hmd2::Hmd2ReprojectionInitialize);
	LIB_FUNC("VVvFh51o20s", Hmd2::Hmd2ReprojectionEnableVrMode);
	LIB_FUNC("FkQX7rjFomk", Hmd2::Hmd2ReprojectionSetTiming);
	LIB_FUNC("f3kPeoTZnIE", Hmd2::Hmd2Open);
	LIB_FUNC("Ocf081WpBpA", Hmd2::Hmd2ReprojectionBeginFrame);
	LIB_FUNC("SVEG+1D7qHA", Hmd2::Hmd2ReprojectionGetPredictedDisplayTime);
	LIB_FUNC("hA9LshbSkzw", Hmd2::Hmd2UnknownHA9LshbSkzw);
	LIB_FUNC("lAoFUedcfqA", Hmd2::Hmd2GazeGetResult);
	LIB_FUNC("xMo9ENEu2E0", Hmd2::Hmd2ReprojectionSetParam);
	LIB_FUNC("retc+-uRMhk", Hmd2::Hmd2UnknownRetc);
}

} // namespace Libs
