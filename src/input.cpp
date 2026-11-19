#include "input.h"
#include <cmath>

Input::Input() : profile(default_input_profile()) {}
Input::~Input() { stop_rumble(); }
void Input::init(HWND native_window) {
    window = native_window;
    RAWINPUTDEVICE devices[] = {{0x01, 0x06, 0, window}, {0x01, 0x02, 0, window}};
    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)))
        throw std::runtime_error("Raw Input registration failed");
    check(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                             reinterpret_cast<void **>(direct.GetAddressOf()), nullptr),
          "DirectInput8 initialization");
    RegisterTouchWindow(window, 0);
    rescan();
}
BOOL CALLBACK Input::range(const DIDEVICEOBJECTINSTANCEW *object, void *opaque) {
    auto &joystick = *static_cast<Joystick *>(opaque);
    if (object->dwType & DIDFT_AXIS) {
        DIPROPRANGE range{};
        range.diph.dwSize = sizeof(range);
        range.diph.dwHeaderSize = sizeof(range.diph);
        range.diph.dwHow = DIPH_BYID;
        range.diph.dwObj = object->dwType;
        range.lMin = -32768;
        range.lMax = 32767;
        joystick.device->SetProperty(DIPROP_RANGE, &range.diph);
        const DWORD offsets[] = {DIJOFS_X,  DIJOFS_Y,  DIJOFS_Z,         DIJOFS_RX,
                                 DIJOFS_RY, DIJOFS_RZ, DIJOFS_SLIDER(0), DIJOFS_SLIDER(1)};
        for (int i = 0; i < 8; ++i)
            if (object->dwOfs == offsets[i])
                joystick.info.axis_names[i] = narrow(object->tszName);
    } else if ((object->dwType & DIDFT_BUTTON) && object->dwOfs >= DIJOFS_BUTTON(0) &&
               object->dwOfs <= DIJOFS_BUTTON(127))
        joystick.info.button_names[int(object->dwOfs - DIJOFS_BUTTON(0))] = narrow(object->tszName);
    return DIENUM_CONTINUE;
}
BOOL CALLBACK Input::enumerate(const DIDEVICEINSTANCEW *instance, void *opaque) {
    auto &self = *static_cast<Input *>(opaque);
    UINT count = 0;
    GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> raw_devices(count);
    if (count && GetRawInputDeviceList(raw_devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) != UINT(-1))
        for (const auto &raw : raw_devices) {
            if (raw.dwType != RIM_TYPEHID)
                continue;
            RID_DEVICE_INFO info{};
            info.cbSize = sizeof(info);
            UINT size = sizeof(info);
            if (GetRawInputDeviceInfoW(raw.hDevice, RIDI_DEVICEINFO, &info, &size) == UINT(-1))
                continue;
            if (DWORD(MAKELONG(info.hid.dwVendorId, info.hid.dwProductId)) != instance->guidProduct.Data1)
                continue;
            UINT chars = 0;
            GetRawInputDeviceInfoW(raw.hDevice, RIDI_DEVICENAME, nullptr, &chars);
            std::wstring path(chars, 0);
            if (chars &&
                GetRawInputDeviceInfoW(raw.hDevice, RIDI_DEVICENAME, path.data(), &chars) != UINT(-1) &&
                path.find(L"IG_") != path.npos)
                return DIENUM_CONTINUE;
        }
    Joystick joystick;
    if (FAILED(self.direct->CreateDevice(instance->guidInstance, &joystick.device, nullptr)))
        return DIENUM_CONTINUE;
    if (FAILED(joystick.device->SetDataFormat(&c_dfDIJoystick2)) ||
        FAILED(joystick.device->SetCooperativeLevel(self.window, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE)))
        return DIENUM_CONTINUE;
    wchar_t guid[64]{};
    StringFromGUID2(instance->guidInstance, guid, 64);
    joystick.info.id = "dinput:" + narrow(guid);
    joystick.info.name = narrow(instance->tszProductName);
    joystick.device->EnumObjects(range, &joystick, DIDFT_AXIS | DIDFT_BUTTON);
    joystick.device->Acquire();
    if (!self.profile.assignments.count(joystick.info.id)) {
        uint32_t occupied = 0;
        for (DWORD slot = 0; slot < 4; ++slot) {
            XINPUT_STATE state{};
            if (XInputGetState(slot, &state) == ERROR_SUCCESS)
                occupied |= self.profile.assignments["xinput:" + std::to_string(slot)];
        }
        for (const auto &known : self.joysticks)
            occupied |= self.profile.assignments[known.info.id];
        unsigned player = 0;
        while (player + 1 < input_players && (occupied & (1u << player)))
            ++player;
        self.profile.assignments[joystick.info.id] = 1u << player;
    }
    self.joysticks.push_back(std::move(joystick));
    return DIENUM_CONTINUE;
}
void Input::rescan() {
    joysticks.clear();
    if (direct)
        direct->EnumDevices(DI8DEVCLASS_GAMECTRL, enumerate, this, DIEDFL_ATTACHEDONLY);
}
void Input::focus(bool value) {
    focused = value;
    if (!value) {
        suppress_frame = true;
        keyboard.fill(false);
        mouse_buttons = 0;
        pending_x = pending_y = 0;
        wheel = horizontal_wheel = 0;
        pending_text.clear();
        high_surrogate = 0;
        touch_points.clear();
        cancel_capture();
        stop_rumble();
    }
}
void Input::raw(LPARAM parameter) {
    UINT length = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(parameter), RID_INPUT, nullptr, &length,
                        sizeof(RAWINPUTHEADER)) == UINT(-1))
        return;
    std::vector<uint8_t> bytes(length);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(parameter), RID_INPUT, bytes.data(), &length,
                        sizeof(RAWINPUTHEADER)) != length)
        return;
    const auto &raw = *reinterpret_cast<const RAWINPUT *>(bytes.data());
    if (!focused)
        return;
    if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        auto key = raw.data.keyboard.VKey;
        if (key == VK_SHIFT)
            key = WORD(MapVirtualKeyW(raw.data.keyboard.MakeCode, MAPVK_VSC_TO_VK_EX));
        if (key == VK_CONTROL)
            key = raw.data.keyboard.Flags & RI_KEY_E0 ? VK_RCONTROL : VK_LCONTROL;
        if (key == VK_MENU)
            key = raw.data.keyboard.Flags & RI_KEY_E0 ? VK_RMENU : VK_LMENU;
        if (key < keyboard.size())
            keyboard[key] = !(raw.data.keyboard.Flags & RI_KEY_BREAK);
    } else if (raw.header.dwType == RIM_TYPEMOUSE) {
        if (!(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            pending_x = std::clamp<int64_t>(int64_t(pending_x) + raw.data.mouse.lLastX, -32768, 32767);
            pending_y = std::clamp<int64_t>(int64_t(pending_y) + raw.data.mouse.lLastY, -32768, 32767);
        }
        for (unsigned i = 0; i < 5; ++i) {
            if (raw.data.mouse.usButtonFlags & (1u << (i * 2)))
                mouse_buttons |= 1u << i;
            if (raw.data.mouse.usButtonFlags & (2u << (i * 2)))
                mouse_buttons &= ~(1u << i);
        }
        if (raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
            wheel += SHORT(raw.data.mouse.usButtonData) / WHEEL_DELTA;
        if (raw.data.mouse.usButtonFlags & RI_MOUSE_HWHEEL)
            horizontal_wheel += SHORT(raw.data.mouse.usButtonData) / WHEEL_DELTA;
    }
}
uint16_t Input::xinput_buttons(const XINPUT_GAMEPAD &pad) {
    const WORD buttons[] = {XINPUT_GAMEPAD_A,
                            XINPUT_GAMEPAD_X,
                            XINPUT_GAMEPAD_BACK,
                            XINPUT_GAMEPAD_START,
                            XINPUT_GAMEPAD_DPAD_UP,
                            XINPUT_GAMEPAD_DPAD_DOWN,
                            XINPUT_GAMEPAD_DPAD_LEFT,
                            XINPUT_GAMEPAD_DPAD_RIGHT,
                            XINPUT_GAMEPAD_B,
                            XINPUT_GAMEPAD_Y,
                            XINPUT_GAMEPAD_LEFT_SHOULDER,
                            XINPUT_GAMEPAD_RIGHT_SHOULDER,
                            0,
                            0,
                            XINPUT_GAMEPAD_LEFT_THUMB,
                            XINPUT_GAMEPAD_RIGHT_THUMB};
    uint16_t mask = 0;
    for (unsigned i = 0; i < 16; ++i)
        if (pad.wButtons & buttons[i])
            mask |= 1u << i;
    if (pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_L2;
    if (pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
        mask |= 1u << RETRO_DEVICE_ID_JOYPAD_R2;
    return mask;
}

void Input::character(wchar_t c) {
    if (c >= 0xd800 && c <= 0xdbff) {
        high_surrogate = c;
        return;
    }
    uint32_t code = c;
    if (c >= 0xdc00 && c <= 0xdfff && high_surrogate)
        code = 0x10000 + ((high_surrogate - 0xd800) << 10) + (c - 0xdc00);
    high_surrogate = 0;
    if (code >= 32)
        pending_text.push_back(code);
}
void Input::touch(WPARAM count, LPARAM handle) {
    auto touch_handle = reinterpret_cast<HTOUCHINPUT>(handle);
    std::vector<TOUCHINPUT> points(LOWORD(count));
    if (GetTouchInputInfo(touch_handle, UINT(points.size()), points.data(), sizeof(TOUCHINPUT)))
        for (const auto &point : points) {
            if (point.dwFlags & TOUCHEVENTF_UP)
                touch_points.erase(point.dwID);
            else
                touch_points[point.dwID] = {TOUCH_COORD_TO_PIXEL(point.x), TOUCH_COORD_TO_PIXEL(point.y)};
        }
    CloseTouchInputHandle(touch_handle);
}
void Input::poll(bool capture_keyboard, bool capture_mouse) {
    InputSnapshot snapshot;
    snapshot.focused = focused;
    snapshot.keys = keyboard;
    snapshot.mouse_buttons = mouse_buttons;
    snapshot.mouse_x = pending_x;
    snapshot.mouse_y = pending_y;
    snapshot.wheel = wheel;
    snapshot.horizontal_wheel = horizontal_wheel;
    pending_x = pending_y = 0;
    wheel = horizontal_wheel = 0;
    snapshot.text = std::move(pending_text);
    pending_text.clear();
    RECT viewport = pointer_viewport;
    if (viewport.right <= viewport.left || viewport.bottom <= viewport.top)
        GetClientRect(window, &viewport);
    auto pointer = [&](POINT position, bool pressed) {
        InputPointer result;
        ScreenToClient(window, &position);
        int width = viewport.right - viewport.left, height = viewport.bottom - viewport.top;
        if (width > 0 && height > 0) {
            result.inside = position.x >= viewport.left && position.x < viewport.right &&
                            position.y >= viewport.top && position.y < viewport.bottom;
            result.x = int16_t(
                std::clamp((double(position.x - viewport.left) / width * 2 - 1) * 32767, -32767.0, 32767.0));
            result.y = int16_t(
                std::clamp((double(position.y - viewport.top) / height * 2 - 1) * 32767, -32767.0, 32767.0));
        }
        result.pressed = pressed && result.inside;
        return result;
    };
    POINT cursor{};
    if (GetCursorPos(&cursor))
        snapshot.pointer = pointer(cursor, (mouse_buttons & 1) != 0);
    for (const auto &point : touch_points)
        snapshot.touch.push_back(pointer(point.second, true));
    for (DWORD slot = 0; slot < 4; ++slot) {
        XINPUT_STATE state{};
        if (XInputGetState(slot, &state) != ERROR_SUCCESS)
            continue;
        InputController controller;
        controller.id = "xinput:" + std::to_string(slot);
        controller.name = "Xbox controller " + std::to_string(slot + 1);
        controller.xinput = true;
        auto mask = xinput_buttons(state.Gamepad);
        for (unsigned b = 0; b < 16; ++b)
            controller.buttons[b] = (mask & (1u << b)) != 0;
        auto invert = [](SHORT y) { return int16_t(std::clamp(-int(y), -32768, 32767)); };
        controller.axes = {state.Gamepad.sThumbLX,
                           invert(state.Gamepad.sThumbLY),
                           state.Gamepad.sThumbRX,
                           invert(state.Gamepad.sThumbRY),
                           int16_t(unsigned(state.Gamepad.bLeftTrigger) * 32767 / 255),
                           int16_t(unsigned(state.Gamepad.bRightTrigger) * 32767 / 255),
                           0,
                           0};
        snapshot.controllers.push_back(std::move(controller));
    }
    if (focused)
        for (auto &joystick : joysticks) {
            if (FAILED(joystick.device->Poll())) {
                joystick.device->Acquire();
                joystick.device->Poll();
            }
            DIJOYSTATE2 state{};
            if (FAILED(joystick.device->GetDeviceState(sizeof(state), &state)))
                continue;
            auto controller = joystick.info;
            for (unsigned b = 0; b < 128; ++b)
                controller.buttons[b] = (state.rgbButtons[b] & 0x80) != 0;
            const LONG values[] = {state.lX,  state.lY,  state.lZ,           state.lRx,
                                   state.lRy, state.lRz, state.rglSlider[0], state.rglSlider[1]};
            for (unsigned a = 0; a < 8; ++a)
                controller.axes[a] = int16_t(std::clamp<LONG>(values[a], -32768, 32767));
            for (unsigned h = 0; h < 4; ++h)
                controller.hats[h] = LOWORD(state.rgdwPOV[h]) == 0xffff ? -1 : int(state.rgdwPOV[h]);
            snapshot.controllers.push_back(std::move(controller));
        }
    apply_snapshot(snapshot, capture_keyboard, capture_mouse);
}
void Input::stop_rumble() {
    for (auto &v : vibrations)
        v = {};
    update_rumble();
}
void Input::update_rumble() {
    for (unsigned slot = 0; slot < 4; ++slot) {
        XINPUT_VIBRATION value{};
        for (unsigned player = 0; player < input_players; ++player)
            if (assigned("xinput:" + std::to_string(slot), player)) {
                value.wLeftMotorSpeed = std::max(value.wLeftMotorSpeed, vibrations[player].wLeftMotorSpeed);
                value.wRightMotorSpeed =
                    std::max(value.wRightMotorSpeed, vibrations[player].wRightMotorSpeed);
            }
        const bool requested = value.wLeftMotorSpeed || value.wRightMotorSpeed;
        if (requested || rumble_owned[slot]) {
            if (XInputSetState(slot, &value) == ERROR_SUCCESS)
                rumble_owned[slot] = requested;
            else
                rumble_owned[slot] = false;
        }
    }
}
bool Input::rumble(unsigned port, retro_rumble_effect effect, uint16_t strength) {
    if (port >= input_players)
        return false;
    if (effect == RETRO_RUMBLE_STRONG)
        vibrations[port].wLeftMotorSpeed = strength;
    else if (effect == RETRO_RUMBLE_WEAK)
        vibrations[port].wRightMotorSpeed = strength;
    else
        return false;
    update_rumble();
    for (const auto &controller : latest.controllers)
        if (controller.xinput && controller.connected && assigned(controller.id, port))
            return true;
    return false;
}
