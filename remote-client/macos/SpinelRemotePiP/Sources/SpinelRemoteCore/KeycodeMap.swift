import AppKit
import Carbon.HIToolbox
import Foundation

public enum MacToLinuxKeymap {
    // Translates macOS CGKeyCode (kVK_*) to Linux input-event-codes (evdev, which Wayland zwp_virtual_keyboard uses).
    public static func evdevCode(for macKeyCode: UInt16, mapCommandToCtrl: Bool) -> UInt32? {
        if mapCommandToCtrl && (macKeyCode == UInt16(kVK_Command) || macKeyCode == UInt16(kVK_RightCommand)) {
            return macKeyCode == UInt16(kVK_Command) ? 29 : 97 // KEY_LEFTCTRL / KEY_RIGHTCTRL
        }
        return table[macKeyCode]
    }

    // Builds XKB depressed modifiers bitmask matching desktop/src/shell/sandbox_pip.cpp:
    // Shift=1, Caps=2, Ctrl=4, Alt=8, Logo(Super)=64
    public static func xkbModifiers(from flags: NSEvent.ModifierFlags, mapCommandToCtrl: Bool) -> UInt32 {
        var mask: UInt32 = 0
        if flags.contains(.shift) { mask |= 1 }
        if flags.contains(.capsLock) { mask |= 2 }
        if flags.contains(.control) { mask |= 4 }
        if flags.contains(.option) { mask |= 8 }
        if flags.contains(.command) {
            mask |= mapCommandToCtrl ? 4 : 64
        }
        return mask
    }

    private static let table: [UInt16: UInt32] = [
        UInt16(kVK_Escape): 1,        // KEY_ESC
        UInt16(kVK_ANSI_1): 2,
        UInt16(kVK_ANSI_2): 3,
        UInt16(kVK_ANSI_3): 4,
        UInt16(kVK_ANSI_4): 5,
        UInt16(kVK_ANSI_5): 6,
        UInt16(kVK_ANSI_6): 7,
        UInt16(kVK_ANSI_7): 8,
        UInt16(kVK_ANSI_8): 9,
        UInt16(kVK_ANSI_9): 10,
        UInt16(kVK_ANSI_0): 11,
        UInt16(kVK_ANSI_Minus): 12,
        UInt16(kVK_ANSI_Equal): 13,
        UInt16(kVK_Delete): 14,       // KEY_BACKSPACE
        UInt16(kVK_Tab): 15,
        UInt16(kVK_ANSI_Q): 16,
        UInt16(kVK_ANSI_W): 17,
        UInt16(kVK_ANSI_E): 18,
        UInt16(kVK_ANSI_R): 19,
        UInt16(kVK_ANSI_T): 20,
        UInt16(kVK_ANSI_Y): 21,
        UInt16(kVK_ANSI_U): 22,
        UInt16(kVK_ANSI_I): 23,
        UInt16(kVK_ANSI_O): 24,
        UInt16(kVK_ANSI_P): 25,
        UInt16(kVK_ANSI_LeftBracket): 26,
        UInt16(kVK_ANSI_RightBracket): 27,
        UInt16(kVK_Return): 28,       // KEY_ENTER
        UInt16(kVK_Control): 29,      // KEY_LEFTCTRL
        UInt16(kVK_ANSI_A): 30,
        UInt16(kVK_ANSI_S): 31,
        UInt16(kVK_ANSI_D): 32,
        UInt16(kVK_ANSI_F): 33,
        UInt16(kVK_ANSI_G): 34,
        UInt16(kVK_ANSI_H): 35,
        UInt16(kVK_ANSI_J): 36,
        UInt16(kVK_ANSI_K): 37,
        UInt16(kVK_ANSI_L): 38,
        UInt16(kVK_ANSI_Semicolon): 39,
        UInt16(kVK_ANSI_Quote): 40,
        UInt16(kVK_ANSI_Grave): 41,
        UInt16(kVK_Shift): 42,        // KEY_LEFTSHIFT
        UInt16(kVK_ANSI_Backslash): 43,
        UInt16(kVK_ANSI_Z): 44,
        UInt16(kVK_ANSI_X): 45,
        UInt16(kVK_ANSI_C): 46,
        UInt16(kVK_ANSI_V): 47,
        UInt16(kVK_ANSI_B): 48,
        UInt16(kVK_ANSI_N): 49,
        UInt16(kVK_ANSI_M): 50,
        UInt16(kVK_ANSI_Comma): 51,
        UInt16(kVK_ANSI_Period): 52,
        UInt16(kVK_ANSI_Slash): 53,
        UInt16(kVK_RightShift): 54,
        UInt16(kVK_Option): 56,       // KEY_LEFTALT
        UInt16(kVK_Space): 57,
        UInt16(kVK_CapsLock): 58,
        UInt16(kVK_F1): 59,
        UInt16(kVK_F2): 60,
        UInt16(kVK_F3): 61,
        UInt16(kVK_F4): 62,
        UInt16(kVK_F5): 63,
        UInt16(kVK_F6): 64,
        UInt16(kVK_F7): 65,
        UInt16(kVK_F8): 66,
        UInt16(kVK_F9): 67,
        UInt16(kVK_F10): 68,
        UInt16(kVK_F11): 87,
        UInt16(kVK_F12): 88,
        UInt16(kVK_RightControl): 97,
        UInt16(kVK_RightOption): 100,
        UInt16(kVK_Home): 102,
        UInt16(kVK_UpArrow): 103,
        UInt16(kVK_PageUp): 104,
        UInt16(kVK_LeftArrow): 105,
        UInt16(kVK_RightArrow): 106,
        UInt16(kVK_End): 107,
        UInt16(kVK_DownArrow): 108,
        UInt16(kVK_PageDown): 109,
        UInt16(kVK_ForwardDelete): 111, // KEY_DELETE
        UInt16(kVK_Command): 125,     // KEY_LEFTMETA
        UInt16(kVK_RightCommand): 126 // KEY_RIGHTMETA
    ]
}
