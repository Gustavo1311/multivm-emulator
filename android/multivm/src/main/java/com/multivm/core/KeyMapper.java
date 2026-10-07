package com.multivm.core;

import android.util.SparseIntArray;
import android.view.KeyEvent;

/** Converte codigos de tecla do Android (KeyEvent.KEYCODE_*) para codigos evdev do Linux. */
public final class KeyMapper {
    private static final SparseIntArray MAP = new SparseIntArray(128);

    static {
        // letras
        int[] letters = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
        for (int i = 0; i < 26; i++) MAP.put(KeyEvent.KEYCODE_A + i, letters[i]);
        // digitos
        MAP.put(KeyEvent.KEYCODE_0, 11);
        for (int i = 1; i <= 9; i++) MAP.put(KeyEvent.KEYCODE_0 + i, 1 + i);
        MAP.put(KeyEvent.KEYCODE_ESCAPE, 1);
        MAP.put(KeyEvent.KEYCODE_MINUS, 12);
        MAP.put(KeyEvent.KEYCODE_EQUALS, 13);
        MAP.put(KeyEvent.KEYCODE_DEL, 14);
        MAP.put(KeyEvent.KEYCODE_TAB, 15);
        MAP.put(KeyEvent.KEYCODE_LEFT_BRACKET, 26);
        MAP.put(KeyEvent.KEYCODE_RIGHT_BRACKET, 27);
        MAP.put(KeyEvent.KEYCODE_ENTER, 28);
        MAP.put(KeyEvent.KEYCODE_CTRL_LEFT, 29);
        MAP.put(KeyEvent.KEYCODE_SEMICOLON, 39);
        MAP.put(KeyEvent.KEYCODE_APOSTROPHE, 40);
        MAP.put(KeyEvent.KEYCODE_GRAVE, 41);
        MAP.put(KeyEvent.KEYCODE_SHIFT_LEFT, 42);
        MAP.put(KeyEvent.KEYCODE_BACKSLASH, 43);
        MAP.put(KeyEvent.KEYCODE_COMMA, 51);
        MAP.put(KeyEvent.KEYCODE_PERIOD, 52);
        MAP.put(KeyEvent.KEYCODE_SLASH, 53);
        MAP.put(KeyEvent.KEYCODE_SHIFT_RIGHT, 54);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_MULTIPLY, 55);
        MAP.put(KeyEvent.KEYCODE_ALT_LEFT, 56);
        MAP.put(KeyEvent.KEYCODE_SPACE, 57);
        MAP.put(KeyEvent.KEYCODE_CAPS_LOCK, 58);
        for (int i = 0; i < 10; i++) MAP.put(KeyEvent.KEYCODE_F1 + i, 59 + i);
        MAP.put(KeyEvent.KEYCODE_NUM_LOCK, 69);
        MAP.put(KeyEvent.KEYCODE_SCROLL_LOCK, 70);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_7, 71);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_8, 72);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_9, 73);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_SUBTRACT, 74);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_4, 75);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_5, 76);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_6, 77);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_ADD, 78);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_1, 79);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_2, 80);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_3, 81);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_0, 82);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_DOT, 83);
        MAP.put(KeyEvent.KEYCODE_F11, 87);
        MAP.put(KeyEvent.KEYCODE_F12, 88);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_ENTER, 96);
        MAP.put(KeyEvent.KEYCODE_CTRL_RIGHT, 97);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_DIVIDE, 98);
        MAP.put(KeyEvent.KEYCODE_SYSRQ, 99);
        MAP.put(KeyEvent.KEYCODE_ALT_RIGHT, 100);
        MAP.put(KeyEvent.KEYCODE_MOVE_HOME, 102);
        MAP.put(KeyEvent.KEYCODE_DPAD_UP, 103);
        MAP.put(KeyEvent.KEYCODE_PAGE_UP, 104);
        MAP.put(KeyEvent.KEYCODE_DPAD_LEFT, 105);
        MAP.put(KeyEvent.KEYCODE_DPAD_RIGHT, 106);
        MAP.put(KeyEvent.KEYCODE_MOVE_END, 107);
        MAP.put(KeyEvent.KEYCODE_DPAD_DOWN, 108);
        MAP.put(KeyEvent.KEYCODE_PAGE_DOWN, 109);
        MAP.put(KeyEvent.KEYCODE_INSERT, 110);
        MAP.put(KeyEvent.KEYCODE_FORWARD_DEL, 111);
        MAP.put(KeyEvent.KEYCODE_BREAK, 119);
        MAP.put(KeyEvent.KEYCODE_META_LEFT, 125);
        MAP.put(KeyEvent.KEYCODE_META_RIGHT, 126);
        MAP.put(KeyEvent.KEYCODE_MENU, 127);
    }

    private KeyMapper() {}

    /** Retorna o codigo evdev, ou 0 se a tecla nao tiver equivalente. */
    public static int toEvdev(int androidKeyCode) {
        return MAP.get(androidKeyCode, 0);
    }
}
