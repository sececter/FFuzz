#include <iostream>

#include "MsPowerPointApp.h"
#include "Tickler.h"


void MsPowerPointApp::initCPUMonitor(PDH_HQUERY &cpuQuery) {
    PdhAddCounter(cpuQuery, L"\\Process(POWERPNT)\\% Processor Time", NULL, &m_cpuTotal);
}


void MsPowerPointApp::getCurrentCpuUsage(PLONG Total, PLONG targets) {
    PDH_FMT_COUNTERVALUE counterVal;

    PdhGetFormattedCounterValue(m_cpuTotal, PDH_FMT_LONG, NULL, &counterVal);
    *targets = counterVal.longValue;
}

bool MsPowerPointApp::handleWindowOpenEventByWindowClass(BSTR &className, IUIAutomationElement *pSender) {
    if (!m_startedHandlingOpenEvent) {
        return true;
    }

    if (!IsFromSameProcess(pSender)) {
        TICKLERMSG("ignoring window from different PID, taking screenshot\n");
        TakeScreenShot();
        return true;
    }

    return false;
}

bool MsPowerPointApp::handleWindowOpenEventByWindowName(BSTR &name, IUIAutomationElement *pSender) {

    /* XXX: sometimes windows pop up before the main one */
    /* Close "Microsoft Office Activation Wizard" window */
    if (wcscmp(name, L"Microsoft Office Activation Wizard") == 0) {
        TakeScreenShot();
        TICKLERMSG("closing Microsoft Office Activation Wizard\n");
        CloseWindow(pSender);
        return true;
    } else if (wcscmp(name, L"Password") == 0) {
        TakeScreenShot();
        TICKLERMSG("Found password window, trying to open in read only mode\n");
        if (!ClickDialogButton(pSender, std::string("read only"))) {
            TICKLERMSG("  could not click on read only\n");
        }
        return true;
    }

    if (!m_startedHandlingOpenEvent) {
        return true;
    }

    if (!IsFromSameProcess(pSender)) {
        TICKLERMSG("ignoring window from different PID, taking screenshot\n");
        TakeScreenShot();
        return true;
    }

    m_lastTimeWindowOpened = BaseInstrGetHostClockMs();

    UIA_HWND windowHandle;
    pSender->get_CurrentNativeWindowHandle(&windowHandle);
    SendWindowInfoToS2E(pSender, pAutomation);


    TICKLERMSG("clicking button on window %#x\n", windowHandle);
    ClickDialogButton(pSender, true);


    return false;
}

VOID MsPowerPointApp::PostScrollHandler(HWND windowHandle)
{

}

/**
 * Word needs a mouse click on the document to properly scroll.
 * Sending messages doesn't work so just press the key.
 * Don't know whether this is required for PowerPoint.
 */
VOID MsPowerPointApp::PrepareScroll(HWND ScrollableWindow)
{
    /* Find a clickable area */
    HWND ClickableWindow = FindWindowWithTitleOrClass(ScrollableWindow, "mdiClass", TRUE, TRUE);
    if (!ClickableWindow) {
        ClickableWindow = ScrollableWindow;
    }

    RECT Rect;
    if (GetWindowRect(ClickableWindow, &Rect)) {
        TICKLERMSG("GetWindowRect x:%d y:%d\n", Rect.left, Rect.top);
        Click((Rect.left + Rect.right) / 2, (Rect.top + Rect.bottom) / 2);
    } else {
        TICKLERMSG("GetWindowRect failed\n");
    }

    /**
     * TODO: need to figure out how to ensure that the pages were scrolled.
     */
    Sleep(2000);

    /* Put this in the common code eventually */
    PressKey(VK_NEXT, 500);
}

HWND MsPowerPointApp::GetScrollableWindow(HWND MainWindow)
{
    unsigned Timeout = 240; /* Can take a lot of time... */
    HWND Ret;
    while (!(Ret = FindWindowWithTitleOrClass(m_topLevelWindowHandle, "MDIClient", TRUE, TRUE))) {
        TICKLERMSG("Waiting for scrollable window to appear (root=%p)...\n", MainWindow);

        Sleep(1000);
        if (--Timeout == 0) {
            return NULL;
        }
    }

    Sleep(5000);

    /* Return the main window handle after the document appears */
    return MainWindow;
}
