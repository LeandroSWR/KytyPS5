#include "common/abi.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstring>

namespace Libs {

LIB_VERSION("VrTracker2", 1, "VrTracker2", 1, 1);

namespace VrTracker2 {

constexpr uint32_t TRACKER_MEMORY_SIZE  = 0x1000000;
constexpr uint32_t TRACKER_MEMORY_ALIGN = 0x10000;
constexpr int32_t  STATUS_TRACKING      = 1;

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
