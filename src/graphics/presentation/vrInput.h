#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_VRINPUT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_VRINPUT_H_

namespace Libs::Vr {

// One headset controller as reported by the runtime. Sticks are -1..1 with +y up, triggers 0..1.
struct VrControllerInput {
	bool  connected   = false;
	float trigger     = 0.0f;
	float squeeze     = 0.0f;
	float stick_x     = 0.0f;
	float stick_y     = 0.0f;
	bool  primary     = false; // X on the left controller, A on the right one.
	bool  secondary   = false; // Y on the left controller, B on the right one.
	bool  stick_click = false;
	bool  menu        = false;
	bool  pose_valid  = false;
	float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
	float position[3]    = {0.0f, 0.0f, 0.0f};       // metres, in the tracking space
};

// A pose in the tracking space: metres, right-handed, +y up, -z forward (the OpenXR convention).
struct VrPose {
	bool  valid          = false;
	float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
	float position[3]    = {0.0f, 0.0f, 0.0f};
};

// Latest head pose. False when no headset session is running or the head is not tracked.
[[nodiscard]] bool OpenXrGetHeadPose(VrPose& out);

// Latest state of the left (0) or right (1) controller. False when no headset session is running,
// in which case the caller keeps using the keyboard or gamepad.
[[nodiscard]] bool OpenXrGetController(int hand, VrControllerInput& out);

} // namespace Libs::Vr

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_VRINPUT_H_
