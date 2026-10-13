#include "common/abi.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include "graphics/presentation/vrInput.h"

#include <cstdint>
#include <cstring>

namespace Libs {

LIB_VERSION("VrTracker2", 1, "VrTracker2", 1, 1);

namespace VrTracker2 {

constexpr uint32_t TRACKER_MEMORY_SIZE  = 0x1000000;
constexpr uint32_t TRACKER_MEMORY_ALIGN = 0x10000;
constexpr int32_t  STATUS_TRACKING      = 1;

// Device ids the game asks poses for.
constexpr uint32_t DEVICE_HMD              = 0x0f000001;
constexpr uint32_t DEVICE_CONTROLLER_LEFT  = 2;
constexpr uint32_t DEVICE_CONTROLLER_RIGHT = 3;
constexpr uint32_t REQUEST_SIZE            = 0x30;

// Pose result layout. The game reads a tracking state at +0x1C and +0x20 and, from them, decides
// whether the position and orientation at +0x60 are usable. State 3 in both marks both as valid.
constexpr uint32_t RESULT_STATE_A_OFFSET = 0x1C;
constexpr uint32_t RESULT_STATE_B_OFFSET = 0x20;
constexpr uint32_t RESULT_POSE_OFFSET    = 0x60;
constexpr uint32_t RESULT_STATE_VALID    = 3;
constexpr uint32_t RESULT_MIN_SIZE       = 0x80;

// A pose as the game reads it back: position, padding, then the orientation quaternion (x, y, z, w).
struct ResultPose {
	float position[3];
	float reserved;
	float orientation[4];
};
static_assert(sizeof(ResultPose) == 0x20);

// Pose requests are 0x30 bytes: size at +0, device id at +4. The answer has no size header, so the
// request is what tells the two shapes apart.
static bool WritePose(const void* request, void* result) {
	if (request == nullptr || result == nullptr || reinterpret_cast<uintptr_t>(request) < 0x200000000ull ||
	    reinterpret_cast<uintptr_t>(result) < 0x200000000ull) {
		return false;
	}
	const auto* words = static_cast<const uint32_t*>(request);
	if (words[0] != REQUEST_SIZE) {
		return false;
	}
	const auto device = words[1];
	if (device != DEVICE_HMD && device != DEVICE_CONTROLLER_LEFT && device != DEVICE_CONTROLLER_RIGHT) {
		return false;
	}

	ResultPose pose {};
	pose.orientation[3] = 1.0f;
	if (device == DEVICE_HMD) {
		Vr::VrPose head;
		if (Vr::OpenXrGetHeadPose(head)) {
			std::memcpy(pose.position, head.position, sizeof(pose.position));
			std::memcpy(pose.orientation, head.orientation, sizeof(pose.orientation));
		}
	} else {
		Vr::VrControllerInput hand;
		if (Vr::OpenXrGetController(device == DEVICE_CONTROLLER_LEFT ? 0 : 1, hand) && hand.pose_valid) {
			std::memcpy(pose.position, hand.position, sizeof(pose.position));
			std::memcpy(pose.orientation, hand.orientation, sizeof(pose.orientation));
		}
	}
	auto* bytes = static_cast<uint8_t*>(result);
	*reinterpret_cast<uint32_t*>(bytes + RESULT_STATE_A_OFFSET) = RESULT_STATE_VALID;
	*reinterpret_cast<uint32_t*>(bytes + RESULT_STATE_B_OFFSET) = RESULT_STATE_VALID;
	std::memcpy(bytes + RESULT_POSE_OFFSET, &pose, sizeof(pose));
	return true;
}

static bool DeclaredStructSize(const void* value, uint32_t* size) {
	if (value == nullptr || reinterpret_cast<uintptr_t>(value) < 0x200000000ull) {
		return false;
	}
	const auto declared = *static_cast<const uint32_t*>(value);
	if (declared < 8 || declared > 512 || declared % 4 != 0) {
		return false;
	}
	*size = declared;
	return true;
}

// sceVrTracker2QueryMemory
static int KYTY_SYSV_ABI QueryMemory(const void* param, void* result) {
	PRINT_NAME();
	uint32_t declared = 0;
	const auto sized  = DeclaredStructSize(result, &declared);
	LOGF("\t param    = %p\n"
	     "\t result   = %p\n"
	     "\t declared = %u\n",
	     param, result, declared);
	if (!sized) {
		return OK;
	}

	auto* fields = static_cast<uint32_t*>(result);
	if (declared >= 28) {
		fields[1] = 0x400000;
		fields[2] = TRACKER_MEMORY_ALIGN;
		fields[3] = 0x3000000;
		fields[4] = TRACKER_MEMORY_ALIGN;
		fields[5] = TRACKER_MEMORY_SIZE;
		fields[6] = TRACKER_MEMORY_ALIGN;
	} else {
		fields[1] = TRACKER_MEMORY_SIZE;
		if (declared >= 12) {
			fields[2] = TRACKER_MEMORY_ALIGN;
		}
	}
	return OK;
}

// sceVrTracker2Initialize
static int KYTY_SYSV_ABI Initialize(const void* param) {
	PRINT_NAME();
	LOGF("\t param = %p\n", param);
	return OK;
}

// sceVrTracker2RegisterDevice
static int KYTY_SYSV_ABI RegisterDevice(int32_t device_type, int32_t handle) {
	PRINT_NAME();
	LOGF("\t device_type = %d\n"
	     "\t handle      = %d\n",
	     device_type, handle);
	return OK;
}

// sceVrTracker2GetResult
static int KYTY_SYSV_ABI GetResult(uint64_t arg0, uint64_t arg1) {
	PRINT_NAME();
	auto guest_pointer = [](uint64_t value) { return value >= 0x200000000ull; };
	if (WritePose(reinterpret_cast<const void*>(arg0), reinterpret_cast<void*>(arg1))) {
		return OK;
	}

	void* result      = nullptr;
	if (guest_pointer(arg1)) {
		result = reinterpret_cast<void*>(arg1);
	} else if (guest_pointer(arg0)) {
		result = reinterpret_cast<void*>(arg0);
	}

	uint32_t declared = 0;
	const auto sized  = DeclaredStructSize(result, &declared);
	LOGF("\t arg0     = 0x%" PRIx64 "\n"
	     "\t arg1     = 0x%" PRIx64 "\n"
	     "\t declared = %u\n",
	     arg0, arg1, declared);
	if (!sized) {
		return OK;
	}

	auto* bytes = static_cast<uint8_t*>(result);
	const auto preserved = *reinterpret_cast<uint32_t*>(bytes);
	std::memset(bytes, 0, declared);
	*reinterpret_cast<uint32_t*>(bytes) = preserved;
	if (declared >= 8) {
		*reinterpret_cast<int32_t*>(bytes + 4) = STATUS_TRACKING;
	}
	if (declared >= 16) {
		*reinterpret_cast<uint64_t*>(bytes + 8) = LibKernel::KernelGetProcessTime();
	}
	// position at +16, orientation xyzw packed at +28, so w is +40.
	if (declared >= 44) {
		*reinterpret_cast<float*>(bytes + 40) = 1.0f;
	}
	return OK;
}

} // namespace VrTracker2

LIB_DEFINE(InitVrTracker2_1) {
	LIB_FUNC("TwqZnaIjWv4", VrTracker2::QueryMemory);
	LIB_FUNC("6Jy73SRfG-o", VrTracker2::Initialize);
	LIB_FUNC("Dog+g25QYjw", VrTracker2::RegisterDevice);
	LIB_FUNC("J4Vh3VVX0iU", VrTracker2::GetResult);
}

} // namespace Libs
