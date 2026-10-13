#ifndef EMULATOR_SRC_LIBS_HMD2_H_
#define EMULATOR_SRC_LIBS_HMD2_H_

#include <atomic>
#include <cstdint>

namespace Libs::Hmd2 {

inline constexpr uint32_t PANEL_WIDTH  = 4000;
inline constexpr uint32_t PANEL_HEIGHT = 2040;
// Tangents of the virtual headset field of view, per eye. "Out" is the temporal side and "in"
// the nasal side. The game renders with these and the headset output must present the same.
inline constexpr float FOV_TAN_OUT    = 1.4281480312f;
inline constexpr float FOV_TAN_IN     = 0.9163311720f;
inline constexpr float FOV_TAN_TOP    = 1.2437491417f;
inline constexpr float FOV_TAN_BOTTOM = 1.2437491417f;
inline constexpr uint64_t OUTPUT_MODE_89_91HZ  = 0x2000c;
inline constexpr uint64_t OUTPUT_MODE_119_88HZ = 0x2000f;
inline constexpr int32_t ERROR_REPROJECTION_NOT_INITIALIZED = -1972240373; // 0x8a72000b

struct ReprojectionState {
	std::atomic<uint64_t> output_mode {0};
	std::atomic<uint32_t> timing_us {3000};
};

[[nodiscard]] ReprojectionState* GetReprojectionState();

} // namespace Libs::Hmd2

#endif // EMULATOR_SRC_LIBS_HMD2_H_
