/*
  XTulator: A portable, open-source 80186 PC emulator.
  Copyright (C)2020 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifdef _WIN32

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "machine.h"
#include <Windows.h>
#include <SDL.h>
#include "config.h"
#include "modules/disk/biosdisk.h"
#include "timing.h"
#include "utility.h"
#include "menus.h"

#define IDM_FILE_RESET       1001
#define IDM_FILE_SEPARATOR1  1002
#define IDM_FILE_EXIT        1003

#define IDM_DISK_FLOPPY0     2001
#define IDM_DISK_FLOPPY1     2002
#define IDM_DISK_SEPARATOR1  2003
#define IDM_DISK_EJECT0      2004
#define IDM_DISK_EJECT1      2005
#define IDM_DISK_SEPARATOR2  2006
#define IDM_DISK_HARD0       2007
#define IDM_DISK_HARD1       2008
#define IDM_DISK_SEPARATOR3  2009
#define IDM_DISK_BOOTFD0     2010
#define IDM_DISK_BOOTHD0     2011

#define IDM_EMULATION_SPEED477    3001
#define IDM_EMULATION_SPEED8      3002
#define IDM_EMULATION_SPEED10     3003
#define IDM_EMULATION_SPEED16     3004
#define IDM_EMULATION_SPEED25     3005
#define IDM_EMULATION_SPEED50     3006
#define IDM_EMULATION_SPEEDUNLIM  3007


WNDPROC menus_oldProc;
MACHINE_t* menus_useMachine = NULL;
uint32_t menus_resetTimer;
const uint8_t menus_ctrlaltdel[3] = { 0x1D, 0x38, 0x53 };
uint8_t menus_resetPos = 0;

MENU_t menu_file[] = {
    { TEXT("Soft &reset (Ctrl-Alt-Del)"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_FILE_RESET },
    { TEXT(""), MENUS_ENABLED, MENUS_SEPARATOR, NULL },
    { TEXT("E&xit"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_FILE_EXIT },
    { NULL }
};

MENU_t menu_disk[] = {
    { TEXT("Change floppy 0..."), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_DISK_FLOPPY0 },
    { TEXT("Change floppy 1..."), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_DISK_FLOPPY1 },
    { TEXT(""), MENUS_ENABLED, MENUS_SEPARATOR, NULL },
    { TEXT("Eject floppy 0"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_DISK_EJECT0 },
    { TEXT("Eject floppy 1"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_DISK_EJECT1 },
    { TEXT(""), MENUS_ENABLED, MENUS_SEPARATOR, NULL },
    { TEXT("Insert hard disk 0... (forces immediate reboot)"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_DISK_HARD0 },
    { TEXT("Insert hard disk 1... (forces immediate reboot)"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_DISK_HARD1 },
    { TEXT(""), MENUS_ENABLED, MENUS_SEPARATOR, NULL },
    { TEXT("Set boot drive to fd0"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_DISK_BOOTFD0 },
    { TEXT("Set boot drive to hd0"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_DISK_BOOTHD0 },
    { NULL }
};

MENU_t menu_emulation[] = {
    { TEXT("Set CPU speed to 4.77 MHz"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_EMULATION_SPEED477 },
    { TEXT("Set CPU speed to 8 MHz"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_EMULATION_SPEED8 },
    { TEXT("Set CPU speed to 10 MHz"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_EMULATION_SPEED10 },
    { TEXT("Set CPU speed to 16 MHz"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_EMULATION_SPEED16 },
    { TEXT("Set CPU speed to 25 MHz"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_EMULATION_SPEED25 },
    { TEXT("Set CPU speed to 50 MHz"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_EMULATION_SPEED50 },
    { TEXT("Set CPU speed to unlimited"), MENUS_ENABLED, MENUS_FUNCTION, (void*)IDM_EMULATION_SPEEDUNLIM },
    { NULL }
};

void menus_resetCallback(void* dummy) {
    uint8_t scancode = menus_ctrlaltdel[menus_resetPos++];
    i8042_send_scancode(&menus_useMachine->i8042, scancode);

    if (menus_resetPos == 3) {
        timing_timerDisable(menus_resetTimer);
    }
}


LRESULT CALLBACK menus_wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND: {
        int menuId = LOWORD(wParam);
        switch (menuId) {
        case IDM_FILE_RESET:
            menus_reset();
            break;
        case IDM_FILE_EXIT:
            menus_exit();
            break;
        case IDM_DISK_FLOPPY0:
            menus_changeFloppy0();
            break;
        case IDM_DISK_FLOPPY1:
            menus_changeFloppy1();
            break;
        case IDM_DISK_EJECT0:
            menus_ejectFloppy0();
            break;
        case IDM_DISK_EJECT1:
            menus_ejectFloppy1();
            break;
        case IDM_DISK_HARD0:
            menus_insertHard0();
            break;
        case IDM_DISK_HARD1:
            menus_insertHard1();
            break;
        case IDM_DISK_BOOTFD0:
            menus_setBootFloppy0();
            break;
        case IDM_DISK_BOOTHD0:
            menus_setBootHard0();
            break;
        case IDM_EMULATION_SPEED477:
            menus_speed477();
            break;
        case IDM_EMULATION_SPEED8:
            menus_speed8();
            break;
        case IDM_EMULATION_SPEED10:
            menus_speed10();
            break;
        case IDM_EMULATION_SPEED16:
            menus_speed16();
            break;
        case IDM_EMULATION_SPEED25:
            menus_speed25();
            break;
        case IDM_EMULATION_SPEED50:
            menus_speed50();
            break;
        case IDM_EMULATION_SPEEDUNLIM:
            menus_speedunlimited();
            break;
        default:
            break;
        }
        return 0;
    }
    }

    return(CallWindowProc(menus_oldProc, hwnd, msg, wParam, lParam));
}

int menus_buildMenu(HMENU* hmenuBar, wchar_t* title, MENU_t* menu) {
    HMENU hmenu;
    UINT i;

    hmenu = CreateMenu();
    if (hmenu == NULL) {
        perror("CreateMenu failed");
        return -1;
    }

    i = 0;
    while (menu[i].title != NULL) {
        if (menu[i].type == MENUS_FUNCTION) {
            if (AppendMenuW(hmenu, MF_STRING, (UINT)(INT_PTR)menu[i].function, menu[i].title) == 0) {
                perror("AppendMenuW (function) failed");
                return -1;
            }
        }
        else if (menu[i].type == MENUS_SEPARATOR) {
            if (AppendMenuW(hmenu, MF_SEPARATOR, 0, NULL) == 0) {
                perror("AppendMenuW (separator) failed");
                return -1;
            }
        }
        i++;
    }
    if (AppendMenuW(*hmenuBar, MF_POPUP, (UINT_PTR)hmenu, title) == 0) {
        perror("AppendMenuW (popup) failed");
        return -1;
    }

    return 0;
}

int menus_init(HWND hwnd) {
    HMENU hmenuBar;

    hmenuBar = CreateMenu();
    if (hmenuBar == NULL) {
        perror("CreateMenu (menubar) failed");
        return -1;
    }

    if (menus_buildMenu(&hmenuBar, TEXT("File"), menu_file) < 0) {
        return -1;
    }
    if (menus_buildMenu(&hmenuBar, TEXT("Emulation"), menu_emulation) < 0) {
        return -1;
    }
    if (menus_buildMenu(&hmenuBar, TEXT("Disk"), menu_disk) < 0) {
        return -1;
    }

    if (SetMenu(hwnd, hmenuBar) == 0) {
        perror("SetMenu failed");
        return -1;
    }
    DrawMenuBar(hwnd);

    menus_oldProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)menus_wndProc);
    if (menus_oldProc == NULL) {
        perror("SetWindowLongPtr failed");
        return -1;
    }
    menus_resetTimer = timing_addTimer(menus_resetCallback, NULL, 10, TIMING_DISABLED);

    return 0;
}

void menus_setMachine(MACHINE_t* machine) {
    menus_useMachine = machine;
}

void menus_exit() {
    running = 0;
}

void menus_openFloppyFile(uint8_t disk) {
    OPENFILENAMEW of_dlg;
    wchar_t filename[MAX_PATH + 1] = { 0 };

    memset(&of_dlg, 0, sizeof(of_dlg));
    of_dlg.lStructSize = sizeof(of_dlg);
    of_dlg.hwndOwner = GetActiveWindow();
    of_dlg.lpstrTitle = TEXT("Open disk image");
    of_dlg.hInstance = NULL;
    of_dlg.lpstrFile = filename;
    of_dlg.lpstrFilter = TEXT("Floppy disk images (*.img)\0*.img\0All files (*.*)\0*.*\0\0");
    of_dlg.nMaxFile = MAX_PATH;
    of_dlg.Flags = OFN_FILEMUSTEXIST | OFN_LONGNAMES | OFN_EXPLORER; // OFN_EXPLORER for modern look
    if (GetOpenFileNameW(&of_dlg)) { // Use GetOpenFileNameW for Unicode
        size_t size;
        char* filembs;
        size = wcstombs(NULL, of_dlg.lpstrFile, 0);
        filembs = (char*)malloc(size + 1);
        if (filembs == NULL) {
            return;
        }
        wcstombs(filembs, of_dlg.lpstrFile, size + 1);
        biosdisk_insert(&menus_useMachine->CPU, disk, filembs);
        free(filembs);
    }
}

void menus_openHardFile(uint8_t disk) {
    OPENFILENAMEW of_dlg;
    wchar_t filename[MAX_PATH + 1] = { 0 };

    memset(&of_dlg, 0, sizeof(of_dlg));
    of_dlg.lStructSize = sizeof(of_dlg);
    of_dlg.hwndOwner = GetActiveWindow();
    of_dlg.lpstrTitle = TEXT("Open disk image");
    of_dlg.hInstance = NULL;
    of_dlg.lpstrFile = filename;
    of_dlg.lpstrFilter = TEXT("Hard disk images (*.img)\0*.img\0All files (*.*)\0*.*\0\0");
    of_dlg.nMaxFile = MAX_PATH;
    of_dlg.Flags = OFN_FILEMUSTEXIST | OFN_LONGNAMES | OFN_EXPLORER;
    if (GetOpenFileNameW(&of_dlg)) {
        size_t size;
        char* filembs;
        size = wcstombs(NULL, of_dlg.lpstrFile, 0);
        filembs = (char*)malloc(size + 1);
        if (filembs == NULL) {
            return;
        }
        wcstombs(filembs, of_dlg.lpstrFile, size + 1);
        biosdisk_insert(&menus_useMachine->CPU, disk, filembs);
        free(filembs);
        menus_reset();
    }
}

void menus_changeFloppy0() {
    menus_openFloppyFile(0);
}

void menus_changeFloppy1() {
    menus_openFloppyFile(1);
}

void menus_ejectFloppy0() {
    biosdisk_eject(&menus_useMachine->CPU, 0);
}

void menus_ejectFloppy1() {
    biosdisk_eject(&menus_useMachine->CPU, 1);
}

void menus_insertHard0() {
    menus_openHardFile(2);
}

void menus_insertHard1() {
    menus_openHardFile(3);
}

void menus_setBootFloppy0() {
    bootdrive = 0;
}

void menus_setBootHard0() {
    bootdrive = 2;
}

void menus_reset() {
    menus_resetPos = 0;
    timing_timerEnable(menus_resetTimer);
}

void menus_speed477() {
    setspeed(4.77);
}

void menus_speed8() {
    setspeed(8.0);
}

void menus_speed10() {
    setspeed(10.0);
}

void menus_speed16() {
    setspeed(16.0);
}

void menus_speed25() {
    setspeed(25.0);
}

void menus_speed50() {
    setspeed(50.0);
}

void menus_speedunlimited() {
    setspeed(0);
}

#endif