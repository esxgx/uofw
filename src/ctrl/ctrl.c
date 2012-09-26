/* Copyright (C) 2011, 2012 The uOFW team
   See the file COPYING for copying permission.
*/

/*
 * uOFW/trunk/src/ctrl/ctrl.c
 * 
 * The controller libraries (controller and controller_driver) main 
 * function is to notify an application of information that is output
 * from the controller, such as button and analog stick information.
 * 
 * The controller module consists of 64 internal button data buffers
 * which can be read by the user in order to obtain data.  These buffers
 * are filled with the content of the PSP's hardware registers containing
 * controller data.  This data is transfered into the internal controller
 * data buffers via the SYSCON microcontroller.
 * 
 * By default communication with SYSCON takes place once per frame via
 * the VBlank interrupt and updates the internal controller data buffers.
 * The VBlank interrupt occurs approximately 60 times per second.
 *
 */

#include <ctrl.h>
#include <display.h>
#include <interruptman.h>
#include <modulemgr_init.h>
#include <syscon.h>
#include <sysmem_kdebug.h>
#include <sysmem_kernel.h>
#include <sysmem_suspend_kernel.h>
#include <sysmem_sysclib.h>
#include <sysmem_sysevent.h>
#include <systimer.h>
#include <threadman_kernel.h>

SCE_MODULE_INFO("sceController_Service", SCE_MODULE_KERNEL | SCE_MODULE_ATTR_CANT_STOP | SCE_MODULE_ATTR_EXCLUSIVE_LOAD | 
                                         SCE_MODULE_ATTR_EXCLUSIVE_START, 1, 1);
SCE_MODULE_BOOTSTART("CtrlInit");
SCE_MODULE_REBOOT_BEFORE("CtrlRebootBefore");
SCE_SDK_VERSION(SDK_VERSION);

#define USER_MODE                               (0)
#define KERNEL_MODE                             (1)

#define FALSE                                   (0)
#define TRUE                                    (1)

#define SVALALIGN64(v)                          ((v) - (((((u32)((s32)(v) >> 31)) >> 26) + (v)) & 0xFFFFFFC0))

/* 
 * When the controller module's update event flag is set with this bit,
 * we signal a finished update of the internal controller button data 
 * buffers
 */
#define UPDATE_INTERRUPT_EVENT_FLAG_ON          (1)

/* 
 * The number of the controller module's internal controller button data
 * buffers.
 */
#define CTRL_INTERNAL_CONTROLLER_BUFFERS        (64)
#define CTRL_MAX_INTERNAL_CONTROLLER_BUFFER     (CTRL_INTERNAL_CONTROLLER_BUFFERS - 1)

/* The center position of the analog stick on both axes. */
#define CTRL_ANALOG_PAD_CENTER_VALUE            (128)

/* The minimum position of the analog stick on both axes. */
#define CTRL_ANALOG_PAD_MIN_VALUE               (0)

/* The maximum position of the analog stick on both axes. */
#define CTRL_ANALOG_PAD_MAX_VALUE               (255)

/* 
 * The smallest offset from the analog stick's center position defining 
 * the guaranteed range (center position +/- this offset) the stick 
 * returns to when being released.
 */
#define CTRL_ANALOG_PAD_CENTER_POS_ERROR_MARGIN (37)

/* 
 * When the analog stick idle timer threshold data contains this value, the 
 * analog stick cannot cancel the idle timer despite being moved.
 */
#define CTRL_ANALOG_PAD_NO_CANCEL_IDLE_TIMER    (129)

#define CTRL_MAX_EXTRA_SUSPEND_SAMPLES          (300)

/* 
 * The minimum time in microseconds a custom update interval for the 
 * controller data buffers can be long.
 */
#define CTRL_BUFFER_UPDATE_MIN_CUSTOM_CYCLES    (5555)

/* 
 * The maximum time in microseconds a custom update interval for the 
 * controller data buffers can be long.
 */
#define CTRL_BUFFER_UPDATE_MAX_CUSTOM_CYCLES    (20000)

/*
 * In case the target PSP has an SDK version smaller than or equal to
 * this SDK version, the buffer update cycle value will be increased
 * by one.  This happens when the user sets an own update time.
 */
#define CTRL_BUF_UPDATE_INCREASE_CYCLE_SDK_VER  (0x204FFFF)

/* The default start time of the controller module's alarm handler. */
#define CTRL_ALARM_START_TIME                   (700)

#define CTRL_SAMPLING_MODES                     (2)
#define CTRL_SAMPLING_MODE_MAX_MODE             (CTRL_SAMPLING_MODES - 1)

/* The maximum number of button callbacks which can be registered. */
#define CTRL_BUTTON_CALLBACK_SLOTS              (4)
#define CTRL_BUTTON_CALLBACK_MAX_SLOT           (CTRL_BUTTON_CALLBACK_SLOTS - 1)

#define CTRL_DATA_EMULATION_SLOTS               (4)
#define CTRL_DATA_EMULATION_MAX_SLOT            (CTRL_DATA_EMULATION_SLOTS - 1)

/* The maximum number of rapid fire events which can be registered. */
#define CTRL_BUTTONS_RAPID_FIRE_SLOTS           (16)
#define CTRL_BUTTONS_RAPID_FIRE_MAX_SLOT        (CTRL_BUTTONS_RAPID_FIRE_SLOTS - 1)

/* Obtain the left apply time of a rapid fire event mode. */
#define RF_EVENT_GET_APPLY_TIME_LEFT(data)      ((data) & 0x3F)

/* Obtain the event mode of a rapid fire event. */
#define RF_EVENT_GET_MODE(data)                 ((u32)(data) >> 6)

/* 
 * Set the event mode of a rapid fire event. One of 
 * ::SceCtrlRapidFireEventModes. 
 */
#define RF_EVENT_SET_MODE(data)                 ((data) << 6)

/* Defines the buttons which can be read by User mode applications. */
#define CTRL_USER_MODE_BUTTONS_DEFAULT          (SCE_CTRL_SELECT | SCE_CTRL_START | SCE_CTRL_UP | SCE_CTRL_RIGHT | \
                                                 SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | \
                                                 SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_SQUARE | \
                                                 SCE_CTRL_HOME | SCE_CTRL_HOLD)

#define CTRL_USER_MODE_BUTTONS_EXTENDED         (0x3FFFF)

/* 
 * The default button group which can cancel the PSP's idle timer every 
 * one of its buttons is pressed.
 */
#define CTRL_ALL_TIME_IDLE_TIMER_RESET_BUTTONS   (SCE_CTRL_SELECT | SCE_CTRL_START | SCE_CTRL_UP | SCE_CTRL_RIGHT | \
                                                 SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | \
                                                 SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_SQUARE | \
                                                 SCE_CTRL_HOME | SCE_CTRL_VOLUP | SCE_CTRL_VOLDOWN | SCE_CTRL_SCREEN | \
                                                 SCE_CTRL_NOTE)

#define CTRL_ALL_SUPPORTED_BUTTONS              (0x39FFF3F9)
//SCE_CTRL_MS | SCE_CTRL_DISC | SCE_CTRL_REMOTE | SCE_CTRL_WLAN_UP | SCE_CTRL_HOLD | ?
#define CTRL_HARDWARE_IO_BUTTONS                (0x3B0E0000)

/* Controller buffer read modes. */
enum SceCtrlReadBufferModes {
    PEEK_BUFFER_POSITIVE = 0,
    PEEK_BUFFER_NEGATIVE = 1,
    READ_BUFFER_POSITIVE = 2,
    READ_BUFFER_NEGATIVE = 3,
    PEEK_BUFFER_POSITIVE_EXTRA = 4,
    PEEK_BUFFER_NEGATIVE_EXTRA = 5,
    READ_BUFFER_POSITIVE_EXTRA = 6,
    READ_BUFFER_NEGATIVE_EXTRA = 7,
};

/* Defined rapid fire event modes. */
enum SceCtrlRapidFireEventModes {
    /* The buttons belonging to a rapid fire event will be set to ON (they
     * will show up as being pressed. 
     */
    RAPID_FIRE_EVENT_BUTTONS_ON = 2,
    /* The buttons belonging to a rapid fire event will be set to OFF (they
     * will show up as being un-pressed. 
     */
    RAPID_FIRE_EVENT_BUTTONS_OFF = 3,
};

/*
 * This structure represents a rapid fire event for PSP buttons.  A rapid
 * fire event is a time period in which buttons are constantly turned ON and
 * OFF without the user actually pressing the buttons.
 */
typedef struct {
    /* The buttons which potentially can trigger the rapid fire event.
     * This usage is restricted for now. 
     */
    u32 eventSupportButtons;
    /* The buttons which will start the rapid fire event for the specified 
     * buttons when being pressed.
     */
    u32 eventTriggerButtons;
    /* The buttons on which the rapid fire event will be applied to.  User 
     * mode buttons only.
     */
    u32 buttons;
    /*
     *  7          6 5            0
     * +------------+--------------+
     * | EVENT MODE |  APPLY TIME  |
     * +------------+--------------+
     * 
     * Bit 7 - 6:
     *    The rapid fire event mode.  Defines whether the button is turned
     *    ON or OFF.  One of ::SceCtrlRapidFireEventModes.
     * 
     * Bit 5 - 0:
     *    The remaining apply time for one event mode.  The time is measured
     *    in the number of controller button data updates (and thus is a 
     *    multiple of the update interval).
     */
    u8 eventData;
    /* The number of consecutive internal controller buffer updates the 
     * buttons will be set to ON (pressed). 
     */
    u8 buttonsOnTime;
    /* The number of consecutive internal controller buffer updates the 
     * buttons will be set to OFF (un-pressed). 
     */
    u8 buttonsOffTime;
    /* The number of consecutive internal controller buffer updates the 
     * buttons will be set to ON.  It will only be applied for the first 
     * ON period of a (not canceled) rapid fire event. 
     */
    u8 eventOnTime;
} SceCtrlRapidFire;

/* 
 * This structure represents emulation data applied to controller buffer
 * button data.  The emulation data is set by the user.
 */
typedef struct {
    /* Emulated analog pad X-axis offset. */
    u8 analogX;
    /* Emulated analog pad Y-axis offset. */
    u8 analogY;
    /* The number of consecutive internal controller data buffer updates 
     * the emulated analog pad values will be applied. 
     */
    u32 ctrlBufUpdatesForAnalog;
    /* Emulated user buttons of ::SceCtrlButtons.  You cannot emulate 
     * kernel buttons and the emulated buttons will only be applied for 
     * applications running in User mode.
     */
    u32 userButtons;
    /* Emulated buttons of ::SceCtrlButtons (you can emulate both user 
     * and kernel buttons).  The emulated buttons will only be applied 
     * for applications running in Kernel mode.
     */
    u32 kernelButtons;
    /* The number of consecutive internal controller data buffer updates
     * the emulated buttons will be applied. 
     */
    u32 ctrlBufUpdatesForButtons;
} SceCtrlEmulationData;

/*
 * This structure represents a button callback.  A button callback is a
 * function which is called when at least one of the specified callback
 * buttons is pressed.  The callback can be used to handle controller input
 * without the need to obtain button data via functions like
 * sceCtrlPeekBufferPositive().
 */
typedef struct {
    /* Bitwise OR'ed buttons of ::SceCtrlButtons which will be checked 
     * for being pressed. 
     */
    u32 buttonMask;
    /* Pointer to a callback function handling the controller input. */
    SceKernelButtonCallbackFunction callback;
    /* The global pointer value of the controller module. */
    u32 gp;
    /* An optional pointer being passed as the third argument to the 
     * callback function. 
     */
    void *arg;
} SceCtrlButtonCallback;

/* 
 * This structure represents an internal button data structure.  An object
 * of this structure is used to receive the transfered button data from
 * SYSCON and fill the internal controller data buffers with that data.
 * It also updates the internal latch data of the controller module
 */
typedef struct {
    /* Button is newly pressed (it wasn't pressed one frame before the
     * current one. 
     */
    u32 btnMake;
    /* Stop of button press. It was pressed one frame before the current 
     * one. 
     */
    u32 btnBreak;
    /* Button is pressed. */
    u32 btnPress;
    /* Button is not pressed. */
    u32 btnRelease;
    /* Count the internal latch buffer reads.  Is set to 0 when the 
     * buffer is reset. 
     */
    u32 readLatchCount;
    /* Index representing the first updated controller button data buffer
     * which wasn't read while containing the updated data before.  Thus, 
     * if we have 13 updated buffers (from position 0 - 12), and this member 
     * is set to 10, the buffers at index 10, 11 and 12 are updated buffers 
     * which weren't read with the updated data before.
     */
    u32 firstUnReadUpdatedBufIndex;
    /* The index describing the current buffer which will be updated the
     * next time a SYSCON hardware button data transfer occurs.
     */
    u32 curUpdatableBufIndex;
    /* An array of three pointers to 64 internal button data buffers 
     * containing controller information. 
     */
    void *sceCtrlBuf[3];
} SceCtrlInternalData;

/*
 * The controller module's control block.  Takes care of every important
 * data of the controller.
 */
typedef struct {
    /* The timer ID used for the custom update interval. */
    SceSysTimerId timerID;
    /* Signals the end of an update interval of the controller button data 
     * buffers.  Used in the context of sceCtrlReadBuffer* functions which 
     * need to wait for a finished update of the controller data buffers 
     * before obtaining data from them.
     */
    s32 updateEventId;
    /* The custom timing at which communication with SYSCON takes place. 
     * In case it is 0, the VBlank interrupt is used to define the update
     * interval.
     */
    u32 updateCycle;
    /* The sampling modes of the internal controller button data buffers. 
     * samplingMode[0] = mode for User buffers, samplingMode[1] = mode for
     * Kernel buffers.  One of ::SceCtrlPadInputMode.
     */
    u8 samplingMode[CTRL_SAMPLING_MODES];
    /* Unknown. */
    u8 unk14;
    /* Unknown. */
    u8 unk15;
    /* The busy status of the SYSCON microcontroller for the first communication
     * function (_sceCtrlSysconCmdIntr1).
     */
    u8 sysconBusyIntr1;
    /* The busy status of the SYSCON microcontroller for the second communication
     * function (_sceCtrlSysconCmdIntr2).
     */
    u8 sysconBusyIntr2;
    /** Reserved. */
    u8 resv[2];
    /* TRUE = reset the PSP's idle timer, FALSE = don't reset it. */
    u8 cancelIdleTimer;
    /* Poll mode of the controller.  One of ::SceCtrlPadPollMode. */
    u8 pollMode;
    /* The number of samples of the PSP hardware button registers before
     * suspending the PSP device.
     */
    s16 suspendSamples;
    /* The number of SYSCON hardware button transfers left. */
    s32 sysconTransfersLeft;
    /* The packets sent to SYSCON in order to obtain fresh button data. */
    SceSysconPacket sysPacket[2];
    /* The internal controller data for User mode applications. */
    SceCtrlInternalData userModeData;
    /* The internal controller data for Kernel mode applications. */
    SceCtrlInternalData kernelModeData;
    /* The rapid fire structure objects. */
    SceCtrlRapidFire rapidFire[CTRL_BUTTONS_RAPID_FIRE_SLOTS];
    /* The obtained button data from a SYSCON hardware transfer. */
    u32 pureButtons;
    /* Previously pressed kernel buttons. */
    u32 prevKernelButtons[2];
    /* Previously pressed buttons (one frame past the current one).  Button 
     * emulation data is applied to it.
     */
    u32 prevButtons;
    /* Currently pressed User buttons, combined with custom settings, 
     * like rapid fire events and masked buttons.
     */
    u32 userButtons;
    /* Records the possibly user-modified, pressed buttons of the past 
     * VBlank interrupt before the current one. 
     */
    u32 prevModifiedButtons;
    /* The current X axis value of the analog pad obtained by a SYSCON 
     * transfer. 
     */
    u8 analogX;
    /* The current Y axis value of the analog pad obtained by a SYSCON 
     * transfer. 
     */
    u8 analogY;
    /* Unknown. */
    s8 unk582;
    /* Unknown. */
    s8 unk583;
    /* The emulation data structure objects. */
    SceCtrlEmulationData emulationData[CTRL_DATA_EMULATION_SLOTS];
    /* The buttons which can be read in User mode applications. */
    u32 userModeButtons;
    /* Bit mask containing the buttons which status (pressed, un-pressed)
     * is normally recognized. 
     */
    u32 maskSupportButtons;
    /* Bit mask containing the buttons which are simulated as being
     * pressed, although the user might not press them. 
     */
    u32 maskApplyButtons;
    /* General group of buttons which are able to reset the PSP's idle 
     * timer. 
     */
    s32 idleResetAllSupportedButtons;
    /* Group of buttons which reset the PSP's idle timer the first frame of a 
     * time period where they are being pressed.  They reset the timer when HOLD 
     * mode is inactive. 
     */
    s32 oneTimeIdleResetButtons;
    /* Group of buttons which reset the PSP's idle timer during every frame where
     * they are being pressed.  They reset the timer when HOLD mode is inactive. 
     */
    s32 allTimeIdleResetButtons;
    /* Group of buttons which reset the PSP's idle timer the first frame of a 
     * time period where they are being pressed.  They reset the timer when HOLD 
     * mode is active. 
     */
    s32 oneTimeIdleHoldModeResetButtons;
    /* Group of buttons which reset the PSP's idle timer during every frame where
     * they are being pressed.  They reset the timer when HOLD mode is active. 
     */
    s32 allTimeIdleHoldModeResetButtons;
    /* Analog Stick threshold used to cancel the PSP's idle timer when
     * HOLD mode is inactive.
     */
    s32 unHoldThreshold;
    /* Analog Stick threshold used to cancel the PSP's idle timer when
     * HOLD mode is active.
     */
    s32 holdThreshold;
    /* The button callback objects for the controller module. */
    SceCtrlButtonCallback buttonCallback[CTRL_BUTTON_CALLBACK_SLOTS];
    /* Unknown. */
    s32 unk768;
    /* Unknown. */
    SceCtrlUnkStruct *unk772[2]; 
    /* Unknown. */
    s32 unk780[2];
} SceCtrl;

static s32 _sceCtrlSysEventHandler(s32 eventId, char *eventName __attribute__((unused)), void *param __attribute__((unused)), 
                                   s32 *result __attribute__((unused)));
static SceUInt _sceCtrlDummyAlarm(void *common);
static s32 _sceCtrlVblankIntr(s32 subIntNm, void *arg);
static s32 _sceCtrlTimerIntr(s32, s32, s32, s32);
static s32 _sceCtrlSysconCmdIntr1(SceSysconPacket *sysPacket, void *argp);
static s32 _sceCtrlSysconCmdIntr2(SceSysconPacket *packet, void *argp);
static s32 _sceCtrlUpdateButtons(u32 pureButtons, u8 aX, u8 aY);
static s32 _sceCtrlReadBuf(SceCtrlDataExt *data, u8 nBufs, s32 arg3, u8 mode);

/* 
 * The controller module's manager. Keeps track of every important
 * controller data.
 */
SceCtrl g_ctrl;

SceKernelDeci2Ops g_ctrlDeci2Ops = { 
    .size = 0x28,
    .ops = {
        [0] = (void *)sceCtrlGetSamplingMode,
        [1] = (void *)sceCtrlGetSamplingCycle,
        [2] = (void *)sceCtrlPeekBufferPositive,
        [3] = (void *)sceCtrlPeekLatch,
        [4] = (void *)sceCtrlSetRapidFire,
        [5] = (void *)sceCtrlClearRapidFire,
        [6] = (void *)sceCtrlSetButtonEmulation,
        [7] = (void *)sceCtrlSetAnalogEmulation,
        [8] = (void *)sceCtrlExtendInternalCtrlBuffers
    },
};

/*
 * The controller module's SYSCON event handler.  It is used to handle
 * PSP hardware (power) effects, i.e. entering Sleep mode and resuming
 * from Sleep mode.
 */
SceSysEventHandler g_ctrlSysEvent = {
    .size = sizeof(SceSysEventHandler),
    .name = "SceCtrl",
    .typeMask = SCE_SUSPEND_EVENTS | SCE_RESUME_EVENTS,
    .handler = _sceCtrlSysEventHandler,
    .gp = 0,
    .busy = FALSE,
    .next = NULL,
    .reserved = {
        [0] = 0,
        [1] = 0,
        [2] = 0,
        [3] = 0,
        [4] = 0,
        [5] = 0,
        [6] = 0,
        [7] = 0,
        [8] = 0,
    }
};

/* 
 * The 64 interval controller button data buffers used for User mode
 * applications.  The member "buttons" of these buffers only contains
 * User buttons.
 */
SceCtrlData ctrlUserBufs[CTRL_INTERNAL_CONTROLLER_BUFFERS];
/* 
 * The 64 interval controller button data buffers used for Kernel mode
 * applications.
 */
SceCtrlData ctrlKernelBufs[CTRL_INTERNAL_CONTROLLER_BUFFERS];


s32 sceCtrlInit(void) 
{   
    s32 eventId;
    s32 appType;
    SceSysTimerId timerId;
    u32 supportedUserButtons;
    void (*func)(SceKernelDeci2Ops *);
    s32 *retPtr;
    s32 pspModel; 

    memset(&g_ctrl, 0, sizeof(SceCtrl));

    g_ctrl.pollMode = SCE_CTRL_POLL_ACTIVE;
    g_ctrl.userModeData.sceCtrlBuf[0] = ctrlUserBufs;
    g_ctrl.analogY = CTRL_ANALOG_PAD_CENTER_VALUE;
    g_ctrl.analogX = CTRL_ANALOG_PAD_CENTER_VALUE;
    g_ctrl.kernelModeData.sceCtrlBuf[0] = ctrlKernelBufs;
    g_ctrl.sysconTransfersLeft = -1;

    /* 
     * Create the event used to signal the end of an update process of 
     * internal controller button data buffers.
     */
    eventId = sceKernelCreateEventFlag("SceCtrl", SCE_EVENT_WAITOR, 0, NULL);
    g_ctrl.updateEventId = eventId;

    /* 
     * Allocate a timer used for the custom update interval set by the
     * user.
     */
    timerId = sceSTimerAlloc();
    if (timerId < 0)
        return timerId;

    g_ctrl.timerID = timerId;
    sceSTimerSetPrscl(timerId, 1, 0x30);
    
    g_ctrl.unk15 = -1;
    
    sceKernelRegisterSysEventHandler(&g_ctrlSysEvent);
    sceSysconSetAffirmativeRertyMode(0);

    appType = sceKernelApplicationType();
    if (appType == SCE_INIT_APPLICATION_UPDATER)
        supportedUserButtons = CTRL_USER_MODE_BUTTONS_EXTENDED;

    if (appType > SCE_INIT_APPLICATION_UPDATER) {
        supportedUserButtons = CTRL_USER_MODE_BUTTONS_DEFAULT;
        if (appType == SCE_INIT_APPLICATION_POPS)
            supportedUserButtons = CTRL_USER_MODE_BUTTONS_EXTENDED;
    }
    if (appType == 0) {
        supportedUserButtons = CTRL_USER_MODE_BUTTONS_DEFAULT;
    }
    else {
         supportedUserButtons = CTRL_USER_MODE_BUTTONS_DEFAULT;
         if (appType == SCE_INIT_APPLICATION_VSH)
             supportedUserButtons = CTRL_USER_MODE_BUTTONS_EXTENDED;
    }
    g_ctrl.userModeButtons = supportedUserButtons;
    g_ctrl.maskApplyButtons = 0;
    g_ctrl.maskSupportButtons = supportedUserButtons;

    pspModel = sceKernelGetModel();
    switch (pspModel) { 
    case 4: case 5: case 7: case 9:
        g_ctrl.idleResetAllSupportedButtons = CTRL_ALL_SUPPORTED_BUTTONS;
        break;
    default:
        g_ctrl.idleResetAllSupportedButtons = 0x1FFF3F9;
        break;
    }
    g_ctrl.oneTimeIdleResetButtons = CTRL_ALL_SUPPORTED_BUTTONS; 
    g_ctrl.allTimeIdleResetButtons = CTRL_ALL_TIME_IDLE_TIMER_RESET_BUTTONS;
    g_ctrl.oneTimeIdleHoldModeResetButtons = 0x390E0000;
    g_ctrl.allTimeIdleHoldModeResetButtons = 0;
    g_ctrl.unHoldThreshold = CTRL_ANALOG_PAD_NO_CANCEL_IDLE_TIMER;
    g_ctrl.holdThreshold = CTRL_ANALOG_PAD_NO_CANCEL_IDLE_TIMER;

    sceKernelRegisterSubIntrHandler(SCE_VBLANK_INT, 0x13, _sceCtrlVblankIntr, NULL); 
    sceKernelEnableSubIntr(SCE_VBLANK_INT, 0x13);
   
    retPtr = sceKernelDeci2pReferOperations();
    if ((retPtr != NULL) && (*retPtr == 48)) {
         func = (void (*)(SceKernelDeci2Ops *))*(retPtr + 11);
         func(&g_ctrlDeci2Ops);
    }
    return SCE_ERROR_OK;
}

s32 sceCtrlEnd(void) 
{
    SceSysTimerId timerId;
    s32 sysconStatus;

    sceKernelUnregisterSysEventHandler(&g_ctrlSysEvent);
    sceSysconSetAffirmativeRertyMode(1);
    sceDisplayWaitVblankStart();

    timerId = g_ctrl.timerID;
    g_ctrl.timerID = -1;  
    if (timerId >= 0) { 
        sceSTimerStopCount(timerId);
        sceSTimerFree(timerId);
    }
    
    sceKernelReleaseSubIntrHandler(SCE_VBLANK_INT, 0x13);
    sceKernelDeleteEventFlag(g_ctrl.updateEventId);

    sysconStatus = *((u32 *)&g_ctrl.sysconBusyIntr1);
    while (sysconStatus != 0) { 
           sceDisplayWaitVblankStart();
           sysconStatus = *((u32 *)&g_ctrl.sysconBusyIntr1);
    }
    return SCE_ERROR_OK;
}

s32 sceCtrlSuspend(void) 
{
    s32 cycle;
    
    cycle = g_ctrl.updateCycle;
    if (cycle != 0)
        sceSTimerStopCount(g_ctrl.timerID);
    else
        sceKernelDisableSubIntr(SCE_VBLANK_INT, 0x13);

    return SCE_ERROR_OK;
}

s32 sceCtrlResume(void) 
{
    s32 status;

    status = sceSyscon_driver_97765E27();
    if (status == 1)
        g_ctrl.prevButtons |= 0x20000000;  
    else
        g_ctrl.prevButtons &= ~0x20000000;

    g_ctrl.unk15 = -1;
    
    if (g_ctrl.updateCycle == 0) {
        sceKernelEnableSubIntr(SCE_VBLANK_INT, 0x13);
    }
    else {
        sceSTimerStartCount(g_ctrl.timerID);
        sceSTimerSetHandler(g_ctrl.timerID, g_ctrl.updateCycle, _sceCtrlTimerIntr, 0);
    }
    return SCE_ERROR_OK;
}

u32 sceCtrlSetPollingMode(u8 pollMode) 
{   
    g_ctrl.pollMode = pollMode;    
    return SCE_ERROR_OK;
}

u32 sceCtrlGetSamplingMode(u8 *mode) 
{
    s32 oldK1;
    
    oldK1 = pspShiftK1();     
    if (pspK1PtrOk(mode))
        *mode = g_ctrl.samplingMode[!pspK1IsUserMode()];

    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

s32 sceCtrlSetSamplingMode(u8 mode) 
{
    s32 intrState;
    s32 index;
    u8 prevMode;
    s32 oldK1;

    if (mode > CTRL_SAMPLING_MODE_MAX_MODE)
        return SCE_ERROR_INVALID_MODE;

    intrState = sceKernelCpuSuspendIntr(); 
    oldK1 = pspShiftK1();
    
    index = !pspK1IsUserMode();
    prevMode = g_ctrl.samplingMode[index];
    g_ctrl.samplingMode[index] = mode;

    pspSetK1(oldK1);
    sceKernelCpuResumeIntr(intrState);
    return prevMode;
}

u32 sceCtrlGetSamplingCycle(u32 *cycle) 
{
    s32 oldK1;

    oldK1 = pspShiftK1();
    if (pspK1PtrOk(cycle))
        *cycle = g_ctrl.updateCycle;
    
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

s32 sceCtrlSetSamplingCycle(u32 cycle) 
{
    s32 intrState;
    u32 prevCycle;
    s32 sdkVersion;
    s32 oldK1;

    oldK1 = pspShiftK1();  
    intrState = sceKernelCpuSuspendIntr();

    if (cycle == 0) {
        prevCycle = g_ctrl.updateCycle;
        sceKernelEnableSubIntr(SCE_VBLANK_INT, 0x13);
        g_ctrl.updateCycle = 0;

        sceSTimerSetHandler(g_ctrl.timerID, 0, NULL, 0);
        sceSTimerStopCount(g_ctrl.timerID);
    }
    else if (cycle < CTRL_BUFFER_UPDATE_MIN_CUSTOM_CYCLES || cycle > CTRL_BUFFER_UPDATE_MAX_CUSTOM_CYCLES) {
             return SCE_ERROR_INVALID_VALUE;
    }
    else {
        prevCycle = g_ctrl.updateCycle;
        sceSTimerStartCount(g_ctrl.timerID);
        g_ctrl.updateCycle = cycle;
        sdkVersion = sceKernelGetCompiledSdkVersion();
        
        cycle = (sdkVersion > CTRL_BUF_UPDATE_INCREASE_CYCLE_SDK_VER) ? cycle : cycle + 1;
        sceSTimerSetHandler(g_ctrl.timerID, cycle, _sceCtrlTimerIntr, 0);
        sceKernelDisableSubIntr(SCE_VBLANK_INT, 0x13);
    }
    sceKernelCpuResumeIntr(intrState); 
    pspSetK1(oldK1);
    
    return prevCycle;
}

u32 sceCtrlGetIdleCancelKey(u32 *oneTimeResetButtons, u32 *allTimeResetButtons, u32 *oneTimeHoldResetButtons, 
                            u32 *allTimeHoldResetButtons)
{  
    if (oneTimeResetButtons != NULL)
        *oneTimeResetButtons = g_ctrl.oneTimeIdleResetButtons;

    if (allTimeResetButtons != NULL)
        *allTimeResetButtons = g_ctrl.allTimeIdleResetButtons;

    if (oneTimeHoldResetButtons != NULL)
        *oneTimeHoldResetButtons = g_ctrl.oneTimeIdleHoldModeResetButtons;

    if (allTimeHoldResetButtons != NULL)
        *allTimeHoldResetButtons = g_ctrl.allTimeIdleHoldModeResetButtons;

    return SCE_ERROR_OK;
}

u32 sceCtrlSetIdleCancelKey(u32 oneTimeResetButtons, u32 allTimeResetButtons, u32 oneTimeHoldResetButtons, 
                            u32 allTimeHoldResetButtons)
{      
    g_ctrl.oneTimeIdleResetButtons = oneTimeResetButtons; 
    g_ctrl.allTimeIdleResetButtons = allTimeResetButtons;
    g_ctrl.oneTimeIdleHoldModeResetButtons = oneTimeHoldResetButtons;
    g_ctrl.allTimeIdleHoldModeResetButtons = allTimeHoldResetButtons;

    return SCE_ERROR_OK;
}

s32 sceCtrlGetIdleCancelThreshold(s32 *iUnHoldThreshold, s32 *iHoldThreshold) 
{
    s32 oldK1;
    s32 intrState;

    oldK1 = pspShiftK1();  
    if (!pspK1PtrOk(iUnHoldThreshold) || !pspK1PtrOk(iHoldThreshold)) {
        pspSetK1(oldK1);
        return SCE_ERROR_PRIV_REQUIRED;
    }   

    intrState = sceKernelCpuSuspendIntr();
    
    if (iUnHoldThreshold != NULL)
        *iUnHoldThreshold = (g_ctrl.unHoldThreshold == CTRL_ANALOG_PAD_NO_CANCEL_IDLE_TIMER) ? -1 : g_ctrl.unHoldThreshold;
  
    if (iHoldThreshold != NULL)
        *iHoldThreshold = (g_ctrl.holdThreshold == CTRL_ANALOG_PAD_NO_CANCEL_IDLE_TIMER) ? -1 : g_ctrl.holdThreshold;
  
    sceKernelCpuResumeIntr(intrState);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

s32 sceCtrlSetIdleCancelThreshold(s32 iUnHoldThreshold, s32 iHoldThreshold) 
{
    s32 intrState;
    
    if (((iUnHoldThreshold < -1 || iUnHoldThreshold > 128) && iHoldThreshold < -1) || iHoldThreshold > 128)
        return SCE_ERROR_INVALID_VALUE;
    
    intrState = sceKernelCpuSuspendIntr();

    g_ctrl.holdThreshold = (iHoldThreshold == -1) ? CTRL_ANALOG_PAD_NO_CANCEL_IDLE_TIMER : iHoldThreshold;
    g_ctrl.unHoldThreshold = (iUnHoldThreshold == -1) ? CTRL_ANALOG_PAD_NO_CANCEL_IDLE_TIMER : iUnHoldThreshold;

    sceKernelCpuResumeIntr(intrState);
    return SCE_ERROR_OK;
}

s16 sceCtrlGetSuspendingExtraSamples(void) 
{
    return g_ctrl.suspendSamples;
}

s32 sceCtrlSetSuspendingExtraSamples(s16 suspendSamples) 
{
    if (suspendSamples > CTRL_MAX_EXTRA_SUSPEND_SAMPLES)
        return SCE_ERROR_INVALID_VALUE;

    g_ctrl.suspendSamples = (suspendSamples == 1) ? 0 : suspendSamples;
    return SCE_ERROR_OK;
}

s32 sceCtrlExtendInternalCtrlBuffers(u8 mode, SceCtrlUnkStruct *arg2, s32 arg3) 
{
    SceUID poolId;
    void *ctrlBuf;

    if (mode < 1 || mode > 2)
        return SCE_ERROR_INVALID_VALUE;
    
    if (g_ctrl.unk772[mode - 1] == NULL) {
        poolId = sceKernelCreateFpl("SceCtrlBuf", SCE_KERNEL_PRIMARY_KERNEL_PARTITION, 0, 
                                    2 * sizeof(SceCtrlDataExt) * CTRL_INTERNAL_CONTROLLER_BUFFERS, 1, NULL);
        if (poolId < 0)
            return poolId;

        sceKernelTryAllocateFpl(poolId, &ctrlBuf);
        g_ctrl.kernelModeData.sceCtrlBuf[0] = (SceCtrlDataExt *)(ctrlBuf + CTRL_INTERNAL_CONTROLLER_BUFFERS);
        g_ctrl.userModeData.sceCtrlBuf[0] = (SceCtrlDataExt *)ctrlBuf;
    }
    g_ctrl.unk780[mode - 1] = arg3;
    g_ctrl.unk772[mode - 1] = arg2;
    
    return SCE_ERROR_OK;
}

s32 sceCtrlPeekLatch(SceCtrlLatch *latch) 
{
    SceCtrlInternalData *latchPtr;
    s32 intrState;
    s32 oldK1;

    oldK1 = pspShiftK1();
    intrState = sceKernelCpuSuspendIntr();

    if (!pspK1PtrOk(latch)) {
        sceKernelCpuResumeIntr(intrState);
        pspSetK1(oldK1);
        return SCE_ERROR_PRIV_REQUIRED;
    }
    
    if (pspK1IsUserMode())
        latchPtr = &g_ctrl.userModeData;
    else
        latchPtr = &g_ctrl.kernelModeData;

    latch->buttonMake = latchPtr->btnMake;
    latch->buttonBreak = latchPtr->btnBreak;
    latch->buttonPress = latchPtr->btnPress;
    latch->buttonRelease = latchPtr->btnRelease;

    sceKernelCpuResumeIntr(intrState);
    pspSetK1(oldK1);
    return latchPtr->readLatchCount;
}

s32 sceCtrlReadLatch(SceCtrlLatch *latch) 
{
    SceCtrlInternalData *latchPtr;
    s32 intrState;
    s32 readLatchCount;
    s32 oldK1;

    oldK1 = pspShiftK1();
    intrState = sceKernelCpuSuspendIntr();

    if (!pspK1PtrOk(latch)) {
        sceKernelCpuResumeIntr(intrState);
        pspSetK1(oldK1);
        return SCE_ERROR_PRIV_REQUIRED;
    }
    
    if (pspK1IsUserMode())
        latchPtr = &g_ctrl.userModeData;
    else
        latchPtr = &g_ctrl.kernelModeData;

    readLatchCount = latchPtr->readLatchCount;
    latchPtr->readLatchCount = 0;
    
    latch->buttonMake = latchPtr->btnMake;
    latch->buttonBreak = latchPtr->btnBreak;
    latch->buttonPress = latchPtr->btnPress;
    latch->buttonRelease = latchPtr->btnRelease;

    latchPtr->btnMake = 0;
    latchPtr->btnBreak = 0;
    latchPtr->btnPress = 0;
    latchPtr->btnRelease = 0;

    sceKernelCpuResumeIntr(intrState);
    pspSetK1(oldK1); 
    return readLatchCount;
}

s32 sceCtrlPeekBufferPositive(SceCtrlData *data, u8 nBufs) 
{
    return _sceCtrlReadBuf((SceCtrlDataExt *)data, nBufs, 0, PEEK_BUFFER_POSITIVE);
}

s32 sceCtrlPeekBufferNegative(SceCtrlData *data, u8 nBufs) 
{
    return _sceCtrlReadBuf((SceCtrlDataExt *)data, nBufs, 0, PEEK_BUFFER_NEGATIVE);
}

s32 sceCtrlReadBufferPositive(SceCtrlData *data, u8 nBufs) 
{
   return _sceCtrlReadBuf((SceCtrlDataExt *)data, nBufs, 0, READ_BUFFER_POSITIVE);
}

s32 sceCtrlReadBufferNegative(SceCtrlData *data, u8 nBufs) 
{
    return _sceCtrlReadBuf((SceCtrlDataExt *)data, nBufs, 0, READ_BUFFER_NEGATIVE);
}

s32 sceCtrlPeekBufferPositiveExtra(s32 arg1, SceCtrlDataExt *data, u8 nBufs) 
{
    return _sceCtrlReadBuf(data, nBufs, arg1, PEEK_BUFFER_POSITIVE_EXTRA);
}

s32 sceCtrlPeekBufferNegativeExtra(s32 arg1, SceCtrlDataExt *data, u8 nBufs) 
{
    return _sceCtrlReadBuf(data, nBufs, arg1, PEEK_BUFFER_NEGATIVE_EXTRA);
}

s32 sceCtrlReadBufferPositiveExtra(s32 arg1, SceCtrlDataExt *data, u8 nBufs) 
{
    return _sceCtrlReadBuf(data, nBufs, arg1, READ_BUFFER_POSITIVE_EXTRA);
}

s32 sceCtrlReadBufferNegativeExtra(s32 arg1, SceCtrlDataExt *data, u8 nBufs) 
{
    return _sceCtrlReadBuf(data, nBufs, arg1, READ_BUFFER_NEGATIVE_EXTRA);
}

s32 sceCtrlClearRapidFire(u8 slot) 
{   
    if (slot > CTRL_BUTTONS_RAPID_FIRE_MAX_SLOT)
        return SCE_ERROR_INVALID_INDEX;
    
    g_ctrl.rapidFire[slot].eventSupportButtons = 0; 
    return SCE_ERROR_OK;
}

s32 sceCtrlSetRapidFire(u8 slot, u32 eventSupportButtons, u32 eventTriggerButtons, u32 buttons, u8 eventOnTime, 
                        u8 buttonsOnTime, u8 buttonsOffTime) 
{
    u32 usedButtons;
    u32 invalidButtons;
    s32 oldK1;
    s32 intrState;

    if (slot > CTRL_BUTTONS_RAPID_FIRE_MAX_SLOT)
        return SCE_ERROR_INVALID_INDEX;

    if ((eventOnTime | buttonsOnTime | buttonsOffTime) > CTRL_MAX_INTERNAL_CONTROLLER_BUFFER)
        return SCE_ERROR_INVALID_VALUE;

    oldK1 = pspShiftK1();

    usedButtons = eventSupportButtons | eventTriggerButtons | buttons;  
    
    /*
     * Don't allow kernel buttons or the HOLD button to be used in User
     * mode applications.
     */
    if (pspK1IsUserMode()) { 
        invalidButtons = (~g_ctrl.userModeButtons | SCE_CTRL_HOLD);
        if (invalidButtons & usedButtons) {
            pspSetK1(oldK1);
            return SCE_ERROR_PRIV_REQUIRED;
        }
    }
    intrState = sceKernelCpuSuspendIntr();

    g_ctrl.rapidFire[slot].buttonsOnTime = buttonsOnTime;
    g_ctrl.rapidFire[slot].eventSupportButtons = eventSupportButtons; 
    g_ctrl.rapidFire[slot].eventTriggerButtons = eventTriggerButtons;
    g_ctrl.rapidFire[slot].buttons = buttons;
    g_ctrl.rapidFire[slot].buttonsOffTime = buttonsOffTime;
    g_ctrl.rapidFire[slot].eventOnTime = eventOnTime;
    g_ctrl.rapidFire[slot].eventData = 0;

    sceKernelCpuResumeIntr(intrState);
    pspSetK1(oldK1);
    return SCE_ERROR_OK;
}

s32 sceCtrlSetAnalogEmulation(u8 slot, u8 aX, u8 aY, u32 bufUpdates)
{    
    if (slot > CTRL_DATA_EMULATION_MAX_SLOT)
        return SCE_ERROR_INVALID_VALUE;

    g_ctrl.emulationData[slot].analogX = aX;
    g_ctrl.emulationData[slot].analogY = aY;
    g_ctrl.emulationData[slot].ctrlBufUpdatesForAnalog = bufUpdates;

    return SCE_ERROR_OK;
}

s32 sceCtrlSetButtonEmulation(u8 slot, u32 userButtons, u32 kernelButtons, u32 bufUpdates)
{    
    if (slot > CTRL_DATA_EMULATION_MAX_SLOT)
        return SCE_ERROR_INVALID_VALUE;

    g_ctrl.emulationData[slot].userButtons = userButtons;
    g_ctrl.emulationData[slot].kernelButtons = kernelButtons;
    g_ctrl.emulationData[slot].ctrlBufUpdatesForButtons = bufUpdates;

    return SCE_ERROR_OK;
}

u32 sceCtrlGetButtonIntercept(u32 buttons)
{
    s32 intrState;
    s32 btnMaskMode;

    intrState = sceKernelCpuSuspendIntr();

    btnMaskMode = SCE_CTRL_MASK_IGNORE_BUTTONS;
    if (g_ctrl.maskSupportButtons & buttons)
        btnMaskMode = (g_ctrl.maskApplyButtons & buttons) ? SCE_CTRL_MASK_APPLY_BUTTONS : SCE_CTRL_MASK_NO_MASK;

    sceKernelCpuResumeIntr(intrState);
    return btnMaskMode;
}

u32 sceCtrlSetButtonIntercept(u32 buttons, u32 buttonMaskMode) 
{
    s32 intrState;
    s32 prevBtnMaskMode;    

    intrState = sceKernelCpuSuspendIntr();

    /* Return the previous mask type of the specified buttons. */
    prevBtnMaskMode = SCE_CTRL_MASK_IGNORE_BUTTONS;
    if (buttons & g_ctrl.maskSupportButtons)
        prevBtnMaskMode = (buttons & g_ctrl.maskApplyButtons) ? SCE_CTRL_MASK_APPLY_BUTTONS : SCE_CTRL_MASK_NO_MASK;

    if (buttonMaskMode != SCE_CTRL_MASK_NO_MASK) {
        /* Simulate the specified buttons as always not being pressed. */
        if (buttonMaskMode == SCE_CTRL_MASK_IGNORE_BUTTONS) {
            g_ctrl.maskSupportButtons &= ~buttons;
            g_ctrl.maskApplyButtons &= ~buttons;
        }
        /* Simulate the specified buttons as always being pressed. */
        else if (buttonMaskMode == SCE_CTRL_MASK_APPLY_BUTTONS) {
            g_ctrl.maskSupportButtons |= buttons;
            g_ctrl.maskApplyButtons |= buttons;
        }
    }
    /* Remove any existing simulation from the specified buttons. */
    else {
        g_ctrl.maskSupportButtons |= buttons;
        g_ctrl.maskApplyButtons &= ~buttons;
    }
    sceKernelCpuResumeIntr(intrState);
    return prevBtnMaskMode;
}

s32 sceCtrlSetSpecialButtonCallback(u32 slot, u32 buttonMask, SceKernelButtonCallbackFunction callback, void *opt)
{
    s32 intrState;    

    if (slot > CTRL_BUTTON_CALLBACK_MAX_SLOT)
        return SCE_ERROR_INVALID_INDEX;

    intrState = sceKernelCpuSuspendIntr();

    g_ctrl.buttonCallback[slot].buttonMask = buttonMask;
    g_ctrl.buttonCallback[slot].callback = callback;
    g_ctrl.buttonCallback[slot].arg = opt;
    g_ctrl.buttonCallback[slot].gp = pspGetGp();

    sceKernelCpuResumeIntr(intrState);
    return SCE_ERROR_OK;
}

u32 sceCtrl_driver_6C86AF22(s32 arg1)
{
    g_ctrl.unk768 = arg1;  
    return SCE_ERROR_OK;
}

u32 sceCtrl_driver_5886194C(s8 arg1) 
{   
    g_ctrl.unk583 = arg1;  
    return SCE_ERROR_OK;
}

u32 sceCtrlUpdateCableTypeReq(void) 
{   
    g_ctrl.unk14 = 1;
    return SCE_ERROR_OK;
}

/*
 * Handle PSP hardware events like entering low-power state or 
 * resuming from low-power state.
 * 
 * Entering low-power state:
 *    Put the controller device into a low-power state.
 *    Suspend the update timer, if it is used, or disable the VBlank
 *    interrupt to stop the updates of the internal controller data 
 *    buffers.
 * 
 * Resuming from low-power state:
 *    Bring the controller device back from a low-power state.
 *    Re-start the update timer, if it was used before, or re-enable the
 *    VBlank interrupt to re-start the updates of the internal controller
 *    data buffers.
 * 
 * Returns 0 on success.
 */
static s32 _sceCtrlSysEventHandler(s32 eventId, char *eventName __attribute__((unused)), void *param __attribute__((unused)), 
                                   s32 *result __attribute__((unused))) 
{
    s32 sysconStatus;

    if (eventId == 0x402) {
        sysconStatus = *((u32 *)&g_ctrl.sysconBusyIntr1);
        if (sysconStatus == 0)
            return SCE_ERROR_OK;

        if (g_ctrl.sysconTransfersLeft == 0)
            return SCE_ERROR_OK;

        return SCE_ERROR_BUSY;
    }
    if (eventId < 0x403) {
        if (eventId == 0x400)
            g_ctrl.sysconTransfersLeft = g_ctrl.suspendSamples;

        return SCE_ERROR_OK;
    } 
    else if (eventId == 0x400C) {
             return sceCtrlSuspend();
    }   
    else {
        if (eventId == 0x1000C) {
            sceCtrlResume();
            g_ctrl.sysconTransfersLeft = -1;
        }
        return SCE_ERROR_OK;
    }
}

/* 
 * The alarm handler updates the internal controller buffers with 
 * already transfered controller device input data.  It does not 
 * communicate via the SYSCON microcontroller with the hardware registers
 * in order to obtain fresh button data. 
 *   
 * It is called when one (or both) of the following conditions 
 * are true:
 *    1) The SYSCON microcontroller is busy.
 *    2) The controller driver is NOT polling for new controller device 
 *       input.  
 * 
 * Returns 0.        
 */
static SceUInt _sceCtrlDummyAlarm(void *opt __attribute__((unused))) 
{
    s32 intrState;
    u32 pureButtons;   

    intrState = sceKernelCpuSuspendIntr();
    
    pureButtons = g_ctrl.pureButtons;
    if (g_ctrl.sysconTransfersLeft == 0)
        pureButtons &= ~0x1FFFF;
    
    _sceCtrlUpdateButtons(pureButtons, g_ctrl.analogX, g_ctrl.analogY);
    
    /* Set the event flag to indicate a finished update interval. */
    sceKernelSetEventFlag(g_ctrl.updateEventId, UPDATE_INTERRUPT_EVENT_FLAG_ON);
    
    sceKernelCpuResumeIntr(intrState);
    return SCE_ERROR_OK;
}

/* 
 * The VBlank interrupt handler is called whenever the VBlank interrupt
 * occurs (approximately 60 times/second).  Its purpose is to communicate
 * with the hardware button registers via the SYSCON microcontroller to 
 * update the internal controller data buffers with the new controller 
 * device input data.
 * 
 * A SYSCON hardware register data transfer is requested when the following 
 * two conditions are true:
 *    1) the SYSCON microcontroller is NOT busy.
 *    2) The controller driver is polling for new controller device input.
 * 
 * Returns -1.
 */
static s32 _sceCtrlVblankIntr(s32 subIntNm __attribute__((unused)), void *arg __attribute__((unused)))
{
    s32 intrState;
    s32 status;   
    
    intrState = sceKernelCpuSuspendIntr();
    
    /* 
     * Ensure that calling the VBlank interrupt handler is the right
     * choice (and not the custom user timer handler).
     */
    if (g_ctrl.updateCycle == 0) {      
        if (g_ctrl.sysconBusyIntr1 == FALSE && g_ctrl.pollMode == SCE_CTRL_POLL_ACTIVE) {
            g_ctrl.sysconBusyIntr1 = TRUE;     
            
            /* Specify the requested controller device input data. */
            if ((g_ctrl.samplingMode[USER_MODE] | g_ctrl.samplingMode[KERNEL_MODE]) == SCE_CTRL_INPUT_DIGITAL_ONLY)
                g_ctrl.sysPacket[0].tx[PSP_SYSCON_TX_CMD] = PSP_SYSCON_CMD_GET_KERNEL_DIGITAL_KEY;
            else
               g_ctrl.sysPacket[0].tx[PSP_SYSCON_TX_CMD] = PSP_SYSCON_CMD_GET_KERNEL_DIGITAL_KEY_ANALOG;

            g_ctrl.sysPacket[0].tx[PSP_SYSCON_TX_LEN] = 2;
            status = sceSysconCmdExecAsync(&g_ctrl.sysPacket[0], 1, _sceCtrlSysconCmdIntr1, NULL);
            if (status < SCE_ERROR_OK)
                g_ctrl.sysconBusyIntr1 = FALSE;
        }
        else {             
            sceKernelSetAlarm(CTRL_ALARM_START_TIME, _sceCtrlDummyAlarm, NULL);
        }
    }
    if (g_ctrl.cancelIdleTimer != FALSE) {
        g_ctrl.cancelIdleTimer = FALSE;
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    }   
    sceKernelCpuResumeIntr(intrState);
    return -1;
}

/*
 * The timer handler is called every 'g_ctrl.cycle' microseconds
 * (cycle greater 0).  Its purpose is to communicate with the hardware 
 * button registers via the SYSCON microcontroller to update the 
 * internal controller data buffers with the new controller device input 
 * data.
 * 
 * A SYSCON hardware register data transfer is requested when the following 
 * two conditions are true:
 *    1) the SYSCON microcontroller is NOT busy.
 *    2) The controller driver is polling for new controller device input.
 *
 * Returns -1.
 */
static int _sceCtrlTimerIntr(int unused0 __attribute__((unused)), int unused1 __attribute__((unused)), 
                             int unused2 __attribute__((unused)), int unused3 __attribute__((unused))) 
{
    u8 sysconCtrlCmd;
    s32 intrState;
    s32 status;    

    sysconCtrlCmd = PSP_SYSCON_CMD_GET_KERNEL_DIGITAL_KEY;  
    intrState = sceKernelCpuSuspendIntr();
    
    /* 
     * Ensure that calling the timer handler is the right choice
     * (and not the VBlank interrupt handler).
     */
    if (g_ctrl.updateCycle != 0) {
        if ((g_ctrl.sysconBusyIntr1 == FALSE) && (g_ctrl.pollMode == SCE_CTRL_POLL_ACTIVE)) {
            g_ctrl.sysconBusyIntr1 = TRUE;

            /* Specify the requested controller device input data. */
            if (g_ctrl.samplingMode[USER_MODE] != SCE_CTRL_INPUT_DIGITAL_ONLY)
                sysconCtrlCmd = PSP_SYSCON_CMD_GET_KERNEL_DIGITAL_KEY_ANALOG;

            g_ctrl.sysPacket[0].tx[PSP_SYSCON_TX_CMD] = sysconCtrlCmd;
            status = sceSysconCmdExecAsync(&g_ctrl.sysPacket[0], 1, _sceCtrlSysconCmdIntr1, NULL);
            if (status < SCE_ERROR_OK)
                g_ctrl.sysconBusyIntr1 = FALSE;
        }
        else {
             sceKernelSetAlarm(CTRL_ALARM_START_TIME, _sceCtrlDummyAlarm, NULL);
        }
    }

    if (g_ctrl.cancelIdleTimer != FALSE) {
        g_ctrl.cancelIdleTimer = FALSE;
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    }

    sceKernelCpuResumeIntr(intrState);
    return -1;
}

/*
 * Receive a SYSCON packet with the transferred button data from the 
 * hardware registers. Update the internal controller data buffers of 
 * the controller module with the obtained button data.
 *
 * Returns 0.
 */
static s32 _sceCtrlSysconCmdIntr1(SceSysconPacket *sysPacket, void *argp __attribute__((unused)))
{
    u32 pureButtons;
    s32 intrState;
    s32 status;
    u32 curButtons, prevButtons, newButtons;
    u8 analogX, analogY;
    u8 aXCenterOffset, aYCenterOffset;
    u8 idleVal;
    u8 sampling;
    u32 oneTimeIdleResetButtons, allTimeIdleResetButtons;
    u32 idleResetButtons;

    curButtons = 0;
    g_ctrl.sysconBusyIntr1 = FALSE;
    
    if (g_ctrl.sysconTransfersLeft > 0)
        g_ctrl.sysconTransfersLeft--;

    if (g_ctrl.sysconTransfersLeft == 0) {
        intrState = sceKernelCpuSuspendIntr();
        
        if (g_ctrl.sysconTransfersLeft == 0)    
            pureButtons = g_ctrl.pureButtons & ~0x1FFFF;
        else
            pureButtons = g_ctrl.pureButtons;
 
        _sceCtrlUpdateButtons(pureButtons, g_ctrl.analogX, g_ctrl.analogY);
        
        /* Set the event flag to indicate a finished update interval. */
        sceKernelSetEventFlag(g_ctrl.updateEventId, UPDATE_INTERRUPT_EVENT_FLAG_ON);
        sceKernelCpuResumeIntr(intrState);

        return SCE_ERROR_OK;
    }
    else {
        prevButtons = g_ctrl.pureButtons;
        
        /*
         * A received SYSCON data packet differs in size and content.  Size and
         * content for the controller module depend on the command send to the
         * SYSCON microcontroller.  We test against the different commands in 
         * order to obtain the button data from the right packet location.
         * 
         * Packet example_1:  The command 
         * PSP_SYSCON_CMD_GET_KERNEL_DIGITAL_KEY_ANALOG requests the largest 
         * packet and contains both kernel and user buttons as well as analog 
         * stick data.
         * Packet example_2: The command PSP_SYSCON_CMD_GET_ANALOG requests the
         * smallest package, containing only analog stick data.
         */
        if (sysPacket->tx[PSP_SYSCON_TX_CMD] != PSP_SYSCON_CMD_GET_DIGITAL_KEY 
          && sysPacket->tx[PSP_SYSCON_TX_CMD] != PSP_SYSCON_CMD_GET_DIGITAL_KEY_ANALOG) {
            if (sysPacket->tx[PSP_SYSCON_TX_CMD] >= PSP_SYSCON_CMD_GET_KERNEL_DIGITAL_KEY 
              && sysPacket->tx[PSP_SYSCON_TX_CMD] <= PSP_SYSCON_CMD_GET_KERNEL_DIGITAL_KEY_ANALOG) {
                curButtons = (((sysPacket->rx[PSP_SYSCON_RX_DATA(3)] & 3) << 28) 
                          | ((sysPacket->rx[PSP_SYSCON_RX_DATA(2)] & 0xBF) << 20)
                          | ((sysPacket->rx[PSP_SYSCON_RX_DATA(1)] & 0xF0) << 12)
                          | ((sysPacket->rx[PSP_SYSCON_RX_DATA(0)] & 0xF0) << 8)
                          | ((sysPacket->rx[PSP_SYSCON_RX_DATA(1)] & 6) << 7)
                          | ((sysPacket->rx[PSP_SYSCON_RX_DATA(0)] & 0xF) << 4)
                          | (sysPacket->rx[PSP_SYSCON_RX_DATA(1)] & 9)) 
                    ^ 0x20F7F3F9;
            }
        }
        else {
            curButtons = ((((sysPacket->rx[PSP_SYSCON_RX_DATA(1)] & 0xF0) << 12)
                       | ((sysPacket->rx[PSP_SYSCON_RX_DATA(0)] & 0xF0) << 8)
                       | ((sysPacket->rx[PSP_SYSCON_RX_DATA(1)] & 0x6) << 7)
                       | ((sysPacket->rx[PSP_SYSCON_RX_DATA(0)] & 0xF) << 4)
                       | (sysPacket->rx[PSP_SYSCON_RX_DATA(1)] & 0x9)) 
                    ^ 0x7F3F9) 
                    | (g_ctrl.pureButtons & 0xFFF00000);
        }
        g_ctrl.pureButtons = curButtons;

        if (sysPacket->tx[PSP_SYSCON_TX_CMD] == PSP_SYSCON_CMD_GET_ANALOG) {
            analogY = sysPacket->rx[PSP_SYSCON_RX_DATA(1)];
            analogX = sysPacket->rx[PSP_SYSCON_RX_DATA(0)];
        }
        else if (sysPacket->tx[PSP_SYSCON_TX_CMD] == PSP_SYSCON_CMD_GET_DIGITAL_KEY_ANALOG) {
            analogY = sysPacket->rx[PSP_SYSCON_RX_DATA(3)];
            analogX = sysPacket->rx[PSP_SYSCON_RX_DATA(2)];
        }
        else if (sysPacket->tx[PSP_SYSCON_TX_CMD] == PSP_SYSCON_CMD_GET_KERNEL_DIGITAL_KEY_ANALOG) {
            analogY = sysPacket->rx[PSP_SYSCON_RX_DATA(5)];
            analogX = sysPacket->rx[PSP_SYSCON_RX_DATA(4)];
        }
        else {
            analogY = g_ctrl.analogY;
            analogX = g_ctrl.analogX;
        }
        g_ctrl.analogX = analogX;
        g_ctrl.analogY = analogY;
        _sceCtrlUpdateButtons(curButtons, analogX, analogY);

        if (g_ctrl.cancelIdleTimer == FALSE) {
            newButtons = prevButtons ^ curButtons;
            if ((curButtons & SCE_CTRL_HOLD) == 0) {
                oneTimeIdleResetButtons = g_ctrl.oneTimeIdleResetButtons;
                allTimeIdleResetButtons = g_ctrl.allTimeIdleResetButtons;
                idleVal = g_ctrl.unHoldThreshold;
            }
            else {
                oneTimeIdleResetButtons = g_ctrl.oneTimeIdleHoldModeResetButtons;
                allTimeIdleResetButtons = g_ctrl.allTimeIdleHoldModeResetButtons;
                idleVal = g_ctrl.holdThreshold;
            }
            oneTimeIdleResetButtons &= newButtons;
            allTimeIdleResetButtons &= curButtons;
            idleResetButtons = oneTimeIdleResetButtons | allTimeIdleResetButtons;
               
            idleResetButtons &= g_ctrl.idleResetAllSupportedButtons;
            if (idleResetButtons == 0) {
                if (g_ctrl.samplingMode[USER_MODE] != SCE_CTRL_INPUT_DIGITAL_ONLY) {         
                    aXCenterOffset = (u8)pspMax(analogX - CTRL_ANALOG_PAD_CENTER_VALUE, 
                                                -(analogX - CTRL_ANALOG_PAD_CENTER_VALUE));
                    aYCenterOffset = (u8)pspMax(analogY - CTRL_ANALOG_PAD_CENTER_VALUE, 
                                                -(analogY - CTRL_ANALOG_PAD_CENTER_VALUE));
                    aXCenterOffset = (aXCenterOffset == (CTRL_ANALOG_PAD_CENTER_VALUE - 1)) ? 
                                                                CTRL_ANALOG_PAD_CENTER_VALUE : aXCenterOffset;
                    aYCenterOffset = (aYCenterOffset == (CTRL_ANALOG_PAD_CENTER_VALUE - 1)) ? 
                                                                CTRL_ANALOG_PAD_CENTER_VALUE : aYCenterOffset;

                    if (aXCenterOffset >= idleVal || aYCenterOffset >= idleVal)
                        g_ctrl.cancelIdleTimer = TRUE;
                }
            }
            else {
                g_ctrl.cancelIdleTimer = TRUE;
            }
        }
        sampling = (*(u16 *)g_ctrl.samplingMode) != 0;
        sampling = (g_ctrl.unk14 != 0) ? (sampling | 2) : sampling;

        if (sampling != g_ctrl.unk15 && (g_ctrl.sysconBusyIntr2 == FALSE)) {
            g_ctrl.sysconBusyIntr2 = TRUE;
            g_ctrl.sysPacket[1].tx[PSP_SYSCON_TX_CMD] = PSP_SYSCON_CMD_CTRL_ANALOG_XY_POLLING;
            g_ctrl.sysPacket[1].tx[PSP_SYSCON_TX_LEN] = 3;
            g_ctrl.sysPacket[1].tx[PSP_SYSCON_TX_DATA(0)] = sampling;

            status = sceSysconCmdExecAsync(&g_ctrl.sysPacket[1], 0, _sceCtrlSysconCmdIntr2, NULL);
            if (status < SCE_ERROR_OK)
                g_ctrl.sysconBusyIntr2 = FALSE;
        }
        
        /* Set the event flag to indicate a finished update interval. */
        sceKernelSetEventFlag(g_ctrl.updateEventId, UPDATE_INTERRUPT_EVENT_FLAG_ON);
        return SCE_ERROR_OK;
    }
}

static s32 _sceCtrlSysconCmdIntr2(SceSysconPacket *packet __attribute__((unused)), void *argp __attribute__((unused)))
{   
    g_ctrl.unk14 = 0;
    g_ctrl.unk15 = g_ctrl.sysPacket[1].tx[PSP_SYSCON_TX_DATA(0)] & 0x1;
    g_ctrl.sysconBusyIntr2 = FALSE;

    return SCE_ERROR_OK;
}

/*
 * Update the internal controller button data buffers with the obtained
 * button data from a SYSCON hardware register transfer.  Furthermore, 
 * apply custom user settings to the button data.
 * 
 * Returns 0.
 */
static s32 _sceCtrlUpdateButtons(u32 pureButtons, u8 aX, u8 aY) 
{
    SceCtrlData *ctrlUserBuf, *ctrlKernelBuf;
    SceCtrlDataExt *ctrlUserBufExt, *ctrlKernelBufExt;
    SceKernelButtonCallbackFunction buttonCallback;
    u32 sysTimeLow;
    u32 buttons, curButtons, prevButtons, pressedButtons;
    u32 index;
    u8 analogX, tmpAnalogX, tmpAnalogX2;
    u8 analogY, tmpAnalogY, tmpAnalogY2;
    u8 aXCenterOffset, aXCenterOffset2;
    u8 aYCenterOffset, aYCenterOffset2;
    u8 minIdleReset;
    s32 (*func)(s32);
    s32 status;
    s32 res;
    u32 storeData;
    /* Contains the real pressed buttons (received via SYSCON) and the 
     * custom button settings. 
     */
    u32 updatedButtons;
    /* Records buttons which are currently not being pressed. */
    u32 unpressedButtons;
    /* Contains buttons which have a different state compared between 
     * the current frame and the last one.  That means it includes the
     * buttons which were pressed during the past frame but not during
     * the current one or vice-versa.
     */
    u32 changedButtons;
    u32 uModeBtnEmulation, uModeBtnEmulationAll;
    u32 kModeBtnEmulation, kModeBtnEmulationAll;
    u32 numBufUpdates;
    u8 rfEventMode, rfEventModeTimeLeft;
    u32 i, j;
    /* global pointer. */
    u32 gp;
    u8 check = 0;

    sysTimeLow = sceKernelGetSystemTimeLow();

    index = g_ctrl.userModeData.curUpdatableBufIndex;
    ctrlUserBuf = ((SceCtrlData *)g_ctrl.userModeData.sceCtrlBuf[0] + index);
    ctrlUserBuf->activeTime = sysTimeLow;

    index = g_ctrl.kernelModeData.curUpdatableBufIndex;
    ctrlKernelBuf = ((SceCtrlData *)g_ctrl.kernelModeData.sceCtrlBuf[0] + index);
    ctrlUserBuf->activeTime = sysTimeLow;

    analogX = aX;
    analogY = aY;

    for (i = 0; i < CTRL_DATA_EMULATION_SLOTS; i++) {
         if (g_ctrl.emulationData[i].ctrlBufUpdatesForAnalog > 0) {
             g_ctrl.emulationData[i].ctrlBufUpdatesForAnalog--;
             analogX = g_ctrl.emulationData[i].analogX;
             analogY = g_ctrl.emulationData[i].analogY;
         }
    } 
    ctrlUserBuf->aX = analogX; 
    ctrlUserBuf->aY = analogY;
    ctrlKernelBuf->aX = analogX;
    ctrlKernelBuf->aY = analogY;

    if (g_ctrl.samplingMode[USER_MODE] == SCE_CTRL_INPUT_DIGITAL_ONLY) {
        ctrlUserBuf->aY = CTRL_ANALOG_PAD_CENTER_VALUE;
        ctrlUserBuf->aX = CTRL_ANALOG_PAD_CENTER_VALUE;
    }
    if (g_ctrl.samplingMode[KERNEL_MODE] == SCE_CTRL_INPUT_DIGITAL_ONLY) {
        ctrlKernelBuf->aY = CTRL_ANALOG_PAD_CENTER_VALUE;
        ctrlKernelBuf->aX = CTRL_ANALOG_PAD_CENTER_VALUE;
    }
    tmpAnalogX = 0;
    tmpAnalogY = 0;
    
    for (i = 0; i < sizeof ctrlUserBuf->rsrv; i++)
         ctrlUserBuf->rsrv[i] = 0;

    for (i = 0; i < sizeof ctrlKernelBuf->rsrv; i++)
         ctrlKernelBuf->rsrv[i] = 0;

    for (i = 0; i < 2; i++) {
         if (g_ctrl.unk772[i + 1] != NULL) {                        
             ctrlKernelBufExt = (SceCtrlDataExt *)g_ctrl.kernelModeData.sceCtrlBuf[i + 1];
             ctrlKernelBufExt = ctrlKernelBufExt + 1;
             ctrlKernelBufExt->activeTime = sysTimeLow;

             func = (g_ctrl.unk772[i + 1]->func);
             status = func(g_ctrl.unk780[i + 1]);
             if (status < 0) {
                 for (j = 0; j < sizeof ctrlKernelBufExt->rsrv; j++)
                      ctrlKernelBufExt->rsrv[j] = 0;

                 ctrlKernelBufExt->activeTime = 0;
                 ctrlKernelBufExt->buttons = 0;
                 ctrlKernelBufExt->unk1 = 0;
                 ctrlKernelBufExt->unk2 = 0;
                 ctrlKernelBufExt->unk3 = 0;
                 ctrlKernelBufExt->unk4 = 0;
                 ctrlKernelBufExt->unk5 = 0;
                 ctrlKernelBufExt->unk6 = 0;
                 ctrlKernelBufExt->unk7 = 0;
                 ctrlKernelBufExt->unk8 = 0;
                 ctrlKernelBufExt->aX = CTRL_ANALOG_PAD_CENTER_VALUE;
                 ctrlKernelBufExt->aY = CTRL_ANALOG_PAD_CENTER_VALUE;
                 ctrlKernelBufExt->rsrv[0] = -128;
                 ctrlKernelBufExt->rsrv[1] = -128;
             }
             else {
                 if (g_ctrl.cancelIdleTimer == FALSE) {
                     res = (ctrlKernelBufExt->buttons ^ g_ctrl.prevKernelButtons[i]) | ctrlKernelBufExt->buttons;
                     res &= 0x1FFFF;
                     g_ctrl.prevKernelButtons[i] = ctrlKernelBufExt->buttons;
                     if (res != 0)
                         g_ctrl.cancelIdleTimer = TRUE;
                 }
                 if (g_ctrl.cancelIdleTimer == FALSE && g_ctrl.samplingMode[USER_MODE] == SCE_CTRL_INPUT_DIGITAL_ANALOG) {
                     aXCenterOffset = pspMax(ctrlKernelBufExt->aX - CTRL_ANALOG_PAD_CENTER_VALUE, 
                                             -(ctrlKernelBufExt->aX - CTRL_ANALOG_PAD_CENTER_VALUE));
                     tmpAnalogX = aXCenterOffset;
                     aYCenterOffset = pspMax(ctrlKernelBufExt->aY - CTRL_ANALOG_PAD_CENTER_VALUE, 
                                             -(ctrlKernelBufExt->aY - CTRL_ANALOG_PAD_CENTER_VALUE));
                     tmpAnalogY = aYCenterOffset;
                     
                     /* 
                      * When the analog stick is released, it automatically returns to the
                      * center position.  However, depending on where the stick is released,
                      * it may no not return to the exact center position.  The guaranteed
                      * range for returning to the center when the stick is released is
                      * CTRL_ANALOG_PAD_CENTER_VALUE +/- CTRL_ANALOG_PAD_CENTER_POS_ERROR_MARGIN.
                      */                              
                     minIdleReset = (g_ctrl.unHoldThreshold < CTRL_ANALOG_PAD_CENTER_POS_ERROR_MARGIN) ? 
                                                CTRL_ANALOG_PAD_CENTER_POS_ERROR_MARGIN : g_ctrl.unHoldThreshold;
                     aXCenterOffset = (tmpAnalogX == (CTRL_ANALOG_PAD_CENTER_VALUE - 1)) ? CTRL_ANALOG_PAD_CENTER_VALUE : 
                                                                                           aXCenterOffset;
                     aYCenterOffset = (tmpAnalogY == (CTRL_ANALOG_PAD_CENTER_VALUE - 1)) ? CTRL_ANALOG_PAD_CENTER_VALUE : 
                                                                                           aYCenterOffset;

                     aXCenterOffset2 = pspMax(ctrlKernelBufExt->rsrv[0] - CTRL_ANALOG_PAD_CENTER_VALUE, 
                                              -ctrlKernelBufExt->rsrv[0] + CTRL_ANALOG_PAD_CENTER_VALUE);
                     aYCenterOffset2 = pspMax(ctrlKernelBufExt->rsrv[1] - CTRL_ANALOG_PAD_CENTER_VALUE, 
                                              -ctrlKernelBufExt->rsrv[1] + CTRL_ANALOG_PAD_CENTER_VALUE);
                     aXCenterOffset2 = (aXCenterOffset2 == (CTRL_ANALOG_PAD_CENTER_VALUE - 1)) ? 
                                                CTRL_ANALOG_PAD_CENTER_VALUE : aXCenterOffset2;
                     aYCenterOffset2 = (aYCenterOffset2 == (CTRL_ANALOG_PAD_CENTER_VALUE - 1)) ? 
                                                CTRL_ANALOG_PAD_CENTER_VALUE : aYCenterOffset2;

                     if (aYCenterOffset >= minIdleReset || aXCenterOffset >= minIdleReset || 
                       aYCenterOffset2 >= minIdleReset || aXCenterOffset2 >= minIdleReset)
                         g_ctrl.cancelIdleTimer = TRUE;
                 }
             }
             ctrlUserBufExt = (SceCtrlDataExt *)g_ctrl.userModeData.sceCtrlBuf[1] + i;
             ctrlUserBufExt->activeTime = ctrlKernelBufExt->activeTime;
             ctrlUserBufExt->buttons = ctrlKernelBufExt->buttons;
             ctrlUserBufExt->aX = ctrlKernelBufExt->aX;
             ctrlUserBufExt->aY = ctrlKernelBufExt->aY;
             
             for (j = 0; j < sizeof ctrlUserBufExt->rsrv; j++)
                  ctrlUserBufExt->rsrv[j] = ctrlKernelBufExt->rsrv[j];

             ctrlUserBufExt->unk1 = ctrlKernelBufExt->unk1;
             ctrlUserBufExt->unk2 = ctrlKernelBufExt->unk2;
             ctrlUserBufExt->unk3 = ctrlKernelBufExt->unk3;
             ctrlUserBufExt->unk4 = ctrlKernelBufExt->unk4;
             ctrlUserBufExt->unk5 = ctrlKernelBufExt->unk5;
             ctrlUserBufExt->unk6 = ctrlKernelBufExt->unk6;
             ctrlUserBufExt->unk7 = ctrlKernelBufExt->unk7;
             ctrlUserBufExt->unk8 = ctrlKernelBufExt->unk8;

             ctrlUserBufExt->buttons &= ~SCE_CTRL_HOME;
             ctrlUserBufExt->buttons &= g_ctrl.maskSupportButtons;
             ctrlUserBufExt->buttons |= g_ctrl.maskApplyButtons;

             if ((g_ctrl.unk768 >> i) == 1) {
                 buttons = ctrlKernelBufExt->buttons;
                 g_ctrl.emulationData[i + 1].ctrlBufUpdatesForButtons = 1;
                 kModeBtnEmulation = (buttons & 0xFFFFF0FF)
                                   | (((buttons & 0x500) != 0) << 8)
                                   | (((buttons & 0xA00) != 0) << 9);
                 uModeBtnEmulation = kModeBtnEmulation & g_ctrl.userModeButtons;
                 g_ctrl.emulationData[i + 1].userButtons = uModeBtnEmulation;
                 g_ctrl.emulationData[i + 1].kernelButtons = kModeBtnEmulation;

                 aXCenterOffset = pspMax(ctrlKernelBufExt->aX - CTRL_ANALOG_PAD_CENTER_VALUE, 
                                         -(ctrlKernelBufExt->aX - CTRL_ANALOG_PAD_CENTER_VALUE));
                 aYCenterOffset = pspMax(ctrlKernelBufExt->aY - CTRL_ANALOG_PAD_CENTER_VALUE, 
                                         -(ctrlKernelBufExt->aY - CTRL_ANALOG_PAD_CENTER_VALUE));
                 if (aXCenterOffset <= CTRL_ANALOG_PAD_CENTER_POS_ERROR_MARGIN && 
                   aYCenterOffset > CTRL_ANALOG_PAD_CENTER_POS_ERROR_MARGIN) {
                     check = 1;
                     tmpAnalogY = analogY & 0xFF;
                     tmpAnalogY -= CTRL_ANALOG_PAD_CENTER_VALUE;
                     tmpAnalogX -= CTRL_ANALOG_PAD_CENTER_VALUE;
                 }
             }
         }
    }
    if ((tmpAnalogX | tmpAnalogY) != 0) {            
        tmpAnalogX2 = tmpAnalogX + analogX; 
        storeData = 1;
        if ((analogX - 65) < (CTRL_ANALOG_PAD_CENTER_VALUE - 1)) { 
            analogX = CTRL_ANALOG_PAD_MAX_VALUE;
            if ((analogY - 65) < (CTRL_ANALOG_PAD_CENTER_VALUE - 1)) {
                analogX = CTRL_ANALOG_PAD_CENTER_VALUE;
                analogY = CTRL_ANALOG_PAD_CENTER_VALUE;
                tmpAnalogX2 = tmpAnalogX + analogX;
            }
            else {
                storeData = 0; //simple check to not break the ASM code.
            }
        }
        if (storeData)
            analogX = CTRL_ANALOG_PAD_MAX_VALUE;

        tmpAnalogY += analogY;
        tmpAnalogX = pspMin(tmpAnalogX2, analogX);
        if ((s8)tmpAnalogX2 < CTRL_ANALOG_PAD_MIN_VALUE)
            tmpAnalogX = CTRL_ANALOG_PAD_MIN_VALUE;

        analogY = CTRL_ANALOG_PAD_MAX_VALUE;
        tmpAnalogY2 = pspMin(tmpAnalogY, analogY);
        if ((s8)tmpAnalogY < CTRL_ANALOG_PAD_MIN_VALUE)
            tmpAnalogY2 = CTRL_ANALOG_PAD_MIN_VALUE;

        if (g_ctrl.samplingMode[USER_MODE] == SCE_CTRL_INPUT_DIGITAL_ANALOG) {
            ctrlUserBuf->aX = tmpAnalogX;
            ctrlUserBuf->aY = tmpAnalogY2;
        }
        if (g_ctrl.samplingMode[KERNEL_MODE] == SCE_CTRL_INPUT_DIGITAL_ANALOG) {
            ctrlKernelBuf->aX = tmpAnalogX;
            ctrlKernelBuf->aY = tmpAnalogY2;
        }
    }
    
    /*
     * Update the controller module's kernel controller data buffers.
     */
    index = SVALALIGN64(g_ctrl.userModeData.curUpdatableBufIndex + 1);
    g_ctrl.userModeData.curUpdatableBufIndex = index; 
    if (index == g_ctrl.userModeData.firstUnReadUpdatedBufIndex)
        g_ctrl.userModeData.firstUnReadUpdatedBufIndex = SVALALIGN64(index + 1);

    index = SVALALIGN64(g_ctrl.kernelModeData.curUpdatableBufIndex + 1);
    g_ctrl.kernelModeData.curUpdatableBufIndex = index;
    if (index == g_ctrl.kernelModeData.firstUnReadUpdatedBufIndex)
        g_ctrl.kernelModeData.firstUnReadUpdatedBufIndex = SVALALIGN64(index + 1);
 
    updatedButtons = pureButtons;
    if (pureButtons & SCE_CTRL_HOLD)
        updatedButtons &= CTRL_HARDWARE_IO_BUTTONS;

    for (i = 0; i < CTRL_DATA_EMULATION_SLOTS; i++)
         updatedButtons |= g_ctrl.emulationData[i].kernelButtons;

    ctrlKernelBuf->buttons = updatedButtons;

    /* Update the kernel latch data with the current button states. */
    unpressedButtons = ~updatedButtons;
    changedButtons = g_ctrl.prevModifiedButtons ^ updatedButtons;

    pressedButtons = g_ctrl.kernelModeData.btnPress | updatedButtons;
    g_ctrl.prevModifiedButtons = updatedButtons;
    
    updatedButtons = pureButtons & g_ctrl.maskSupportButtons;

    g_ctrl.kernelModeData.btnPress = pressedButtons;
    g_ctrl.kernelModeData.btnRelease |= unpressedButtons;
    g_ctrl.kernelModeData.btnMake |= (changedButtons & updatedButtons);
    g_ctrl.kernelModeData.btnBreak |= (changedButtons & unpressedButtons);
    g_ctrl.kernelModeData.readLatchCount++;

    if (updatedButtons & SCE_CTRL_HOLD)
        updatedButtons &= (SCE_CTRL_HOLD | SCE_CTRL_HOME);
    
    /* 
     * Apply custom emulation data of the digital pad to the transfered
     * button data.
     */
    uModeBtnEmulationAll = 0;
    kModeBtnEmulationAll = 0;
    for (i = 0; i < CTRL_DATA_EMULATION_SLOTS; i++) {
         uModeBtnEmulation = g_ctrl.emulationData[i].userButtons;
         kModeBtnEmulation = g_ctrl.emulationData[i].kernelButtons;
         numBufUpdates = g_ctrl.emulationData[i].ctrlBufUpdatesForButtons;

         uModeBtnEmulationAll |= uModeBtnEmulation;
         kModeBtnEmulationAll |= kModeBtnEmulation;
         if (numBufUpdates > 0) {
             if (--numBufUpdates == 0) {
                 g_ctrl.emulationData[i].userButtons = 0;
                 g_ctrl.emulationData[i].userButtons = 0;
             }
             g_ctrl.emulationData[i].ctrlBufUpdatesForButtons = numBufUpdates;
         }
    }
    updatedButtons |= uModeBtnEmulationAll;
    gp = pspGetGp();

    curButtons = kModeBtnEmulationAll | pureButtons; 
    prevButtons = g_ctrl.prevButtons;
    g_ctrl.prevButtons = curButtons;
    pressedButtons = curButtons ^ prevButtons;

    /*
     * Make sure we check for the call conditions of custom registered
     * button callbacks and execute them if necessary.  A button callback
     * is executed when at least one of the specified callback buttons
     * is pressed.
     */
    for (i = 0; i < CTRL_BUTTON_CALLBACK_SLOTS; i++) {
         if ((g_ctrl.buttonCallback[i].buttonMask & pressedButtons) != 0) {
             if (g_ctrl.buttonCallback[i].callback != NULL) {
                 pspSetGp(g_ctrl.buttonCallback[i].gp);
                 buttonCallback = g_ctrl.buttonCallback[i].callback;
                 buttonCallback(curButtons, prevButtons, g_ctrl.buttonCallback[i].arg);
             }
         }
    }
    pspSetGp(gp);

    /* 
     * Here, we check for rapid fire events belonging to buttons waiting
     * for their execution.
     */
    for (i = 0; i < CTRL_BUTTONS_RAPID_FIRE_SLOTS; i++) {
         if (g_ctrl.rapidFire[i].eventSupportButtons != 0) {
             pressedButtons = updatedButtons & g_ctrl.rapidFire[i].eventSupportButtons;
             if (pressedButtons == g_ctrl.rapidFire[i].eventTriggerButtons) {
                 rfEventModeTimeLeft = RF_EVENT_GET_APPLY_TIME_LEFT(g_ctrl.rapidFire[i].eventData) - 1;               
                 rfEventMode = RF_EVENT_GET_MODE(g_ctrl.rapidFire[i].eventData);
                 if (rfEventMode != 0) {
                     if (rfEventMode == RAPID_FIRE_EVENT_BUTTONS_ON)
                         updatedButtons |= g_ctrl.rapidFire[i].buttons;
                     else
                         updatedButtons &= ~g_ctrl.rapidFire[i].buttons;
                     
                     /*
                      * The apply time of one part of the rapid fire event (buttons turned 
                      * either ON or OFF) has finished and now the second part of rapid fire
                      * event will be set.  That means, for example, a button which was 
                      * turned ON will now be turned OFF for the specified OFF time.  Once
                      * this part is finished, the first part will be executed again.
                      */
                     if (rfEventModeTimeLeft == 0) {                        
                         rfEventMode = (rfEventMode == RAPID_FIRE_EVENT_BUTTONS_ON) ? RAPID_FIRE_EVENT_BUTTONS_OFF : 
                                                                                      RAPID_FIRE_EVENT_BUTTONS_ON;
                         rfEventModeTimeLeft = (rfEventMode == RAPID_FIRE_EVENT_BUTTONS_OFF) ? 
                                                        g_ctrl.rapidFire[i].buttonsOffTime : 
                                                        g_ctrl.rapidFire[i].buttonsOnTime;
                     }
                 }
                 else {
                     rfEventModeTimeLeft = g_ctrl.rapidFire[i].eventOnTime;
                     rfEventMode = RAPID_FIRE_EVENT_BUTTONS_ON;
                 }
                 g_ctrl.rapidFire[i].eventData = RF_EVENT_SET_MODE(rfEventMode) | rfEventModeTimeLeft;
             }
             else {
                  g_ctrl.rapidFire[i].eventData = 0;
             }
        }
    }
    
    /* Apply the custom button mask settings set by the user. */
    updatedButtons &= g_ctrl.maskSupportButtons;
    updatedButtons |= g_ctrl.maskApplyButtons;
    if (updatedButtons & SCE_CTRL_HOLD) {
        if (check == 0) { 
            if (uModeBtnEmulationAll != 0) {               
                updatedButtons = ((updatedButtons & g_ctrl.userModeButtons) != 0) ? (updatedButtons & ~SCE_CTRL_HOLD) : 
                                                                                     updatedButtons;
            }
        }
        else {
            updatedButtons &= ~SCE_CTRL_HOLD;
        }
    }
    changedButtons = updatedButtons ^ g_ctrl.userButtons;
    if (updatedButtons & SCE_CTRL_HOLD) {
        updatedButtons &= (SCE_CTRL_HOLD | SCE_CTRL_HOME);
        changedButtons = updatedButtons ^ g_ctrl.userButtons;
    }
    ctrlUserBuf->buttons = updatedButtons;
    g_ctrl.userButtons = updatedButtons;
    
    if (changedButtons & SCE_CTRL_HOLD)
        g_ctrl.unk582 = g_ctrl.unk583;

    if (g_ctrl.unk582 != 0) {
        updatedButtons &= SCE_CTRL_HOME;
        g_ctrl.unk582--;
        changedButtons = updatedButtons ^ g_ctrl.userButtons;
    }
    unpressedButtons = ~updatedButtons;
    updatedButtons &= changedButtons;

    pressedButtons = g_ctrl.userModeData.btnPress | updatedButtons;

    /* Update the user mode latch data. */
    g_ctrl.userModeData.btnRelease |= unpressedButtons;
    g_ctrl.userModeData.btnBreak |= (unpressedButtons & changedButtons);
    g_ctrl.userModeData.btnPress |= pressedButtons;
    g_ctrl.userModeData.btnMake |= changedButtons;
    g_ctrl.userModeData.readLatchCount++;

    return SCE_ERROR_OK;
}

/*
 * Read the internal controller data buffers and fill the passed data 
 * buffer with the obtained data.
 * 
 * Returns the number of read internal controller data buffers.
 */
static s32 _sceCtrlReadBuf(SceCtrlDataExt *data, u8 nBufs, s32 arg3, u8 mode) 
{
    SceCtrlInternalData *intDataPtr;
    SceCtrlDataExt *ctrlBuf;
    s32 oldK1;
    u32 i;
    u32 buttons;
    s8 bufIndex, startBufIndex;
    s32 numReadIntBufs;
    s32 status;
    s32 intrState;

    if (nBufs >= CTRL_INTERNAL_CONTROLLER_BUFFERS)
        return SCE_ERROR_INVALID_SIZE;

    if (arg3 > 2)
        return SCE_ERROR_INVALID_VALUE;

    if (arg3 != 0 && (mode & READ_BUFFER_POSITIVE))
        return SCE_ERROR_NOT_SUPPORTED;

    oldK1 = pspShiftK1();

    /* Protect Kernel memory from User Mode. */
    if (!pspK1PtrOk(data)) {
        pspSetK1(oldK1);
        return SCE_ERROR_PRIV_REQUIRED;
    }
    if (pspK1IsUserMode())
        intDataPtr = &g_ctrl.userModeData;
    else
        intDataPtr = &g_ctrl.kernelModeData;

    if (arg3 != 0 && intDataPtr->sceCtrlBuf[arg3] == NULL)
        return SCE_ERROR_NOT_SUPPORTED;

    /*
     * Make sure we wait for the next update interval of the internal
     * controller data buffers before reading them, in case the user
     * used the "SceCtrlReadBuffer*()" functions to obtain button data.
     * We do this to make sure the user obtains the newest updated button
     * data and does not receive previously read old button data.
     */
    if (mode & READ_BUFFER_POSITIVE) {
        status = sceKernelWaitEventFlag(g_ctrl.updateEventId, UPDATE_INTERRUPT_EVENT_FLAG_ON, SCE_EVENT_WAITOR, NULL,
                                        NULL);
        if (status < SCE_ERROR_OK) {
            pspSetK1(oldK1);
            return status;
        }
        intrState = sceKernelCpuSuspendIntr();
        
        /* 
         * Clear the event flag so we can wait for the next finished update
         * interval again. 
         */
        sceKernelClearEventFlag(g_ctrl.updateEventId, ~UPDATE_INTERRUPT_EVENT_FLAG_ON);

        startBufIndex = intDataPtr->firstUnReadUpdatedBufIndex;
        numReadIntBufs = intDataPtr->curUpdatableBufIndex - startBufIndex;
        if (numReadIntBufs < 0)
            numReadIntBufs += CTRL_INTERNAL_CONTROLLER_BUFFERS;

        intDataPtr->firstUnReadUpdatedBufIndex = intDataPtr->curUpdatableBufIndex;
        if (nBufs < numReadIntBufs) {
            startBufIndex = intDataPtr->curUpdatableBufIndex - nBufs;
            startBufIndex = (startBufIndex < 0) ? startBufIndex + CTRL_INTERNAL_CONTROLLER_BUFFERS : startBufIndex;
            numReadIntBufs = nBufs;
        }
    }
    else {
        intrState = sceKernelCpuSuspendIntr();
        
        bufIndex = intDataPtr->curUpdatableBufIndex;
        startBufIndex = bufIndex;
        if (nBufs < CTRL_INTERNAL_CONTROLLER_BUFFERS) {
            startBufIndex = bufIndex - nBufs;
            startBufIndex = (startBufIndex < 0) ? startBufIndex + CTRL_INTERNAL_CONTROLLER_BUFFERS : startBufIndex;
            numReadIntBufs = nBufs;
        }
    }
    if (arg3 != 0)
        ctrlBuf = (SceCtrlDataExt *)intDataPtr->sceCtrlBuf[arg3] + startBufIndex;
    else
         ctrlBuf = (SceCtrlDataExt *)((SceCtrlData *)intDataPtr->sceCtrlBuf[0] + startBufIndex);

    if (numReadIntBufs < 0) {
        sceKernelCpuResumeIntr(intrState);
        pspSetK1(oldK1);
        return numReadIntBufs;
    }
    
    /*
     * Read "nBufs" internal controller data buffers and obtain their data.
     */
    while (nBufs-- > 0) {
           data->activeTime = ctrlBuf->activeTime;

           buttons = ctrlBuf->buttons;
           if (pspK1IsUserMode())
               buttons &= g_ctrl.userModeButtons;

           /*
            * Here, we decide whether the button data is represented in positive
            * logic or negative logic.  "Negative" functions instruct us to use
            * negative logic.
            */
           data->buttons = (mode & PEEK_BUFFER_NEGATIVE) ? ~buttons : buttons;
           data->aX = ctrlBuf->aX;
           data->aY = ctrlBuf->aY;

           if (mode < PEEK_BUFFER_POSITIVE_EXTRA) {
               for (i = 0; i < sizeof data->rsrv; i++)
                    data->rsrv[i] = 0;

               data = (SceCtrlDataExt *)((SceCtrlData *)data + 1);
           }
           /* 
            * We test if the data buffer used to obtain the internal button data 
            * is an extended data buffer and fill it accordingly.
            */
           if (mode >= PEEK_BUFFER_POSITIVE_EXTRA) {
               data->rsrv[2] = 0;
               data->rsrv[3] = 0;
               if (arg3 == 0) {
                   data->rsrv[0] = -128;
                   data->rsrv[1] = -128;
                   data->rsrv[4] = 0;
                   data->rsrv[5] = 0;
                   data->unk1 = 0;
                   data->unk2 = 0;
                   data->unk3 = 0;
                   data->unk4 = 0;
                   data->unk5 = 0;
                   data->unk6 = 0;
                   data->unk7 = 0;
                   data->unk8 = 0;
               }
               else {
                   data->rsrv[0] = ctrlBuf->rsrv[0];
                   data->rsrv[1] = ctrlBuf->rsrv[1];
                   data->rsrv[4] = ctrlBuf->rsrv[4];
                   data->rsrv[5] = ctrlBuf->rsrv[5];
                   data->unk1 = ctrlBuf->unk1;
                   data->unk2 = ctrlBuf->unk2;
                   data->unk3 = ctrlBuf->unk3;
                   data->unk4 = ctrlBuf->unk4;
                   data->unk5 = ctrlBuf->unk5;
                   data->unk6 = ctrlBuf->unk6;
                   data->unk7 = ctrlBuf->unk7;
                   data->unk8 = ctrlBuf->unk8;
               }
               data += 1;
           }
           startBufIndex++;
           if (arg3 == 0)
               ctrlBuf = (SceCtrlDataExt *)((SceCtrlData *)ctrlBuf + startBufIndex);
           else
               ctrlBuf = (SceCtrlDataExt *)ctrlBuf + startBufIndex;

           if (startBufIndex == CTRL_INTERNAL_CONTROLLER_BUFFERS) {
               startBufIndex = 0;

               if (arg3 != 0)
                   ctrlBuf = (SceCtrlDataExt *)intDataPtr->sceCtrlBuf[0];
               else
                   ctrlBuf = (SceCtrlDataExt *)intDataPtr->sceCtrlBuf[arg3];
           }
    }
    sceKernelCpuResumeIntr(intrState);
    pspSetK1(oldK1);
    return numReadIntBufs;
}

s32 CtrlInit(void) 
{
    sceCtrlInit();    
    return SCE_ERROR_OK;
}

s32 CtrlRebootBefore(void) 
{
    sceCtrlEnd();   
    return SCE_ERROR_OK;
}

