/*
 *  OpenVPN-GUI -- A Windows GUI for OpenVPN.
 *
 *  Copyright (C) 2004 Mathias Sundman <mathias@nilings.se>
 *                2010 Heiko Hund <heikoh@users.sf.net>
 *                2016 Selva Nair <selva.nair@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <windows.h>
#include <windowsx.h>
#include <versionhelpers.h>
#include <tchar.h>
#include <stdlib.h>
#include <stdio.h>
#include <process.h>
#include <richedit.h>
#include <time.h>
#include <commctrl.h>
#include <inttypes.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#ifdef ENABLE_OVPN3
#include <json-c/json.h>
#endif

#include "tray.h"
#include "main.h"
#include "openvpn.h"
#include "openvpn_config.h"
#include "openvpn-gui-res.h"
#include "options.h"
#include "scripts.h"
#include "viewlog.h"
#include "proxy.h"
#include "localization.h"
#include "misc.h"
#include "access.h"
#include "save_pass.h"
#include "env_set.h"
#include "echo.h"
#include "pkcs11.h"
#include "service.h"
#include "qr.h"

#define OPENVPN_SERVICE_PIPE_NAME_OVPN2 L"\\\\.\\pipe\\openvpn\\service"
#define OPENVPN_SERVICE_PIPE_NAME_OVPN3 L"\\\\.\\pipe\\ovpnagent"

extern options_t o;

static BOOL TerminateOpenVPN(connection_t *c);

static BOOL LaunchOpenVPN(connection_t *c);

const TCHAR *cfgProp = _T("conn");

void
free_auth_param(auth_param_t *param)
{
    if (!param)
    {
        return;
    }
    free(param->str);
    free(param->id);
    free(param->user);
    free(param->cr_response);
    free(param);
}

void
AppendTextToCaption(HANDLE hwnd, const WCHAR *str)
{
    WCHAR old[256];
    WCHAR new[256];
    GetWindowTextW(hwnd, old, _countof(old));
    _sntprintf_0(new, L"%ls (%ls)", old, str);
    SetWindowText(hwnd, new);
}

/*
 * Show an error tooltip with msg attached to the specified
 * editbox handle.
 */
static void
show_error_tip(HWND editbox, const WCHAR *msg)
{
    EDITBALLOONTIP bt;
    bt.cbStruct = sizeof(EDITBALLOONTIP);
    bt.pszText = msg;
    bt.pszTitle = L"Invalid input";
    bt.ttiIcon = TTI_ERROR_LARGE;

    SendMessage(editbox, EM_SHOWBALLOONTIP, 0, (LPARAM)&bt);
}

/*
 * Receive banner on connection to management interface
 * Format: <BANNER>
 */
void
OnReady(connection_t *c, UNUSED char *msg)
{
    ManagementCommand(c, "state on", NULL, regular);
    ManagementCommand(c, "log on all", OnLogLine, combined);
    ManagementCommand(c, "echo on all", OnEcho, combined);
    ManagementCommand(c, "bytecount 5", NULL, regular);

    /* ask for the current state, especially useful when the daemon was prestarted */
    ManagementCommand(c, "state", OnStateChange, regular);

    if (c->flags & FLAG_DAEMON_PERSISTENT && o.enable_persistent == 2)
    {
        c->auto_connect = true;
    }
}


/*
 * Handle the request to release a hold from the OpenVPN management interface
 */
void
OnHold(connection_t *c, UNUSED char *msg)
{
    EnableWindow(GetDlgItem(c->hwndStatus, ID_RESTART), TRUE);
    if ((c->flags & FLAG_DAEMON_PERSISTENT) && (c->state == disconnecting || c->state == resuming))
    {
        /* retain the hold state if we are here while disconnecting  */
        c->state = onhold;
        SetMenuStatus(c, onhold);
        SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_ONHOLD));
        SetStatusWinIcon(c->hwndStatus, ID_ICO_DISCONNECTED);
        EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), FALSE);
        CheckAndSetTrayIcon();
        return;
    }
    EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), TRUE);
    ManagementCommand(c, "hold off", NULL, regular);
    ManagementCommand(c, "hold release", NULL, regular);
}

/*
 * Handle a log line from the OpenVPN management interface
 * Format <TIMESTAMP>,<FLAGS>,<MESSAGE>
 */
void
OnLogLine(connection_t *c, char *line)
{
    HWND logWnd = GetDlgItem(c->hwndStatus, ID_EDT_LOG);
    time_t timestamp;
    TCHAR *datetime;
    const SETTEXTEX ste = { .flags = ST_SELECTION, .codepage = CP_UTF8 };

    char *flags = strchr(line, ',');
    if (flags == NULL)
    {
        return;
    }
    flags++;

    char *message = strchr(flags, ',');
    if (message == NULL)
    {
        return;
    }
    message++;
    size_t flag_size = message - flags - 1; /* message is always > flags */

    /* Remove lines from log window if it is getting full */
    if (SendMessage(logWnd, EM_GETLINECOUNT, 0, 0) > MAX_LOG_LINES)
    {
        int pos = SendMessage(logWnd, EM_LINEINDEX, DEL_LOG_LINES, 0);
        SendMessage(logWnd, EM_SETSEL, 0, pos);
        SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) _T(""));
    }

    timestamp = strtol(line, NULL, 10);
    datetime = _tctime(&timestamp);
    datetime[24] = _T(' ');

    /* deselect current selection, if any */
    SendMessage(logWnd, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);

    /* change text color if Warning or Error */
    COLORREF text_clr = 0;

    if (memchr(flags, 'N', flag_size) || memchr(flags, 'F', flag_size))
    {
        text_clr = o.clr_error;
    }
    else if (memchr(flags, 'W', flag_size))
    {
        text_clr = o.clr_warning;
    }

    if (text_clr != 0)
    {
        CHARFORMAT cfm = {
            .cbSize = sizeof(CHARFORMAT),
            .dwMask = CFM_COLOR | CFM_BOLD,
            .dwEffects = 0,
            .crTextColor = text_clr,
        };
        SendMessage(logWnd, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfm);
    }

    /* Append line to log window */
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM)datetime);
    SendMessage(logWnd, EM_SETTEXTEX, (WPARAM)&ste, (LPARAM)message);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) _T("\n"));

    /* scroll to the caret */
    SendMessage(logWnd, EM_SCROLLCARET, 0, 0);
}

/* expect ipv4,remote,port,,,ipv6 */
static void
parse_assigned_ip(connection_t *c, const char *msg)
{
    char *sep;
    CLEAR(c->ip);
    CLEAR(c->ipv6);

    /* extract local ipv4 address if available*/
    sep = strchr(msg, ',');
    if (sep == NULL)
    {
        return;
    }

    /* Convert the IP address to Unicode */
    if (sep - msg > 0
        && MultiByteToWideChar(CP_UTF8, 0, msg, sep - msg, c->ip, _countof(c->ip) - 1) == 0)
    {
        WriteStatusLog(c,
                       L"GUI> ",
                       L"Failed to extract the assigned ipv4 address (error = %d)",
                       GetLastError());
        c->ip[0] = L'\0';
    }

    /* extract local ipv6 address if available */

    /* skip 4 commas */
    for (int i = 0; i < 4 && sep; i++)
    {
        sep = strchr(sep + 1, ',');
    }
    if (!sep)
    {
        return;
    }

    sep++; /* start of ipv6 address */
    /* Convert the IP address to Unicode */
    if (MultiByteToWideChar(CP_UTF8, 0, sep, -1, c->ipv6, _countof(c->ipv6) - 1) == 0)
    {
        WriteStatusLog(c,
                       L"GUI> ",
                       L"Failed to extract the assigned ipv6 address (error = %d)",
                       GetLastError());
        c->ipv6[0] = L'\0';
    }
}

/*
 * Send a custom message to Window hwnd when state changes
 * hwnd : handle of the window to which the message is sent
 * lParam : pointer to the state string (char *) received from
 *          OpenVPN daemon (e., "CONNECTED")
 * The function signature matches callback for EnumThreadWindows.
 */
BOOL CALLBACK
NotifyStateChange(HWND hwnd, LPARAM lParam)
{
    SendMessage(hwnd, WM_OVPN_STATE, 0, lParam);
    return TRUE;
}

/*
 * Handle a state change notification from the OpenVPN management interface
 * Format <TIMESTAMP>,<STATE>,[<MESSAGE>],[<LOCAL_IP>][,<REMOTE_IP>]
 */
void
OnStateChange(connection_t *c, char *data)
{
    char *pos, *state, *message;

    if (data == NULL)
    {
        return;
    }

    pos = strchr(data, ',');
    if (pos == NULL)
    {
        return;
    }
    *pos = '\0';

    state = pos + 1;
    pos = strchr(state, ',');
    if (pos == NULL)
    {
        return;
    }
    *pos = '\0';

    message = pos + 1;
    pos = strchr(message, ',');
    if (pos == NULL)
    {
        return;
    }
    *pos = '\0';

    /* notify the all windows in the thread of state change */
    EnumThreadWindows(GetCurrentThreadId(), NotifyStateChange, (LPARAM)state);

    strncpy_s(c->daemon_state, _countof(c->daemon_state), state, _TRUNCATE);

    /* Connected state message could be SUCCESS or ERROR, ROUTE_ERROR.
     * We treat both SUCCESS and ROUTE_ERROR similarly to preserve the
     * current behaviour but show the status window and do not change
     * icons to green if not "SUCCESS".
     */
    if (strcmp(state, "CONNECTED") == 0)
    {
        bool success = !strcmp(message, "SUCCESS");

        parse_assigned_ip(c, pos + 1);

        if (!success) /* connection completed with errors */
        {
            SetForegroundWindow(c->hwndStatus);
            ShowWindow(c->hwndStatus, SW_SHOW);
            /* The daemon does not currently log this error. Add a line to the status window */
            if (!strcmp(message, "ROUTE_ERROR"))
            {
                WriteStatusLog(
                    c,
                    L"ERROR: ",
                    L"Some routes were not successfully added. The connection may not function correctly",
                    false);
            }
            else
            {
                WriteStatusLog(c, L"ERROR: ", L"Connection completed with critical errors.", false);
                return;
            }
        }

        /* concatenate ipv4 and ipv6 addresses into one string */
        WCHAR ip_txt[256];
        WCHAR ip[64];
        wcs_concat2(ip, _countof(ip), c->ip, c->ipv6, L", ");
        LoadLocalizedStringBuf(ip_txt, _countof(ip_txt), IDS_NFO_ASSIGN_IP, ip);

        /* Run Connect Script */
        if (!(c->flags & FLAG_DAEMON_PERSISTENT)
            && (c->state == connecting || c->state == resuming))
        {
            RunConnectScript(c, false);
        }

        /* Show connection tray balloon */
        if ((c->state == connecting && o.show_balloon != 0)
            || (c->state == resuming && o.show_balloon != 0)
            || (c->state == reconnecting && o.show_balloon == 2))
        {
            TCHAR msg[256];
            DWORD id = success ? IDS_NFO_NOW_CONNECTED : IDS_NFO_NOTIFY_ROUTE_ERROR;
            LoadLocalizedStringBuf(msg, _countof(msg), id, c->config_name);
            ShowTrayBalloon(msg, (ip[0] ? ip_txt : _T("")));
        }

        /* Save time when we got connected. */
        c->connected_since = atoi(data);
        c->failed_psw_attempts = 0;
        c->failed_auth_attempts = 0;
        c->state = connected;

        SetMenuStatus(c, connected);
        SetTrayIcon(connected);

        SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_CONNECTED));
        SetDlgItemTextW(c->hwndStatus, ID_TXT_IP, ip_txt);

        if (!success)
        {
            SetDlgItemText(
                c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_ROUTE_ERROR));
            return;
        }

        SetStatusWinIcon(c->hwndStatus, ID_ICO_CONNECTED);

        /* Hide Status Window */
        ShowWindow(c->hwndStatus, SW_HIDE);
    }
    else if (strcmp(state, "RECONNECTING") == 0)
    {
        if (!c->dynamic_cr)
        {
            if (strcmp(message, "auth-failure") == 0)
            {
                c->failed_auth_attempts++;
            }
            else if (strcmp(message, "private-key-password-failure") == 0)
            {
                c->failed_psw_attempts++;
            }
        }

        echo_msg_clear(c, false); /* do not clear history */
        /* We change the state to reconnecting only if there was a prior successful connection. */
        if (c->state == connected)
        {
            c->state = reconnecting;

            /* Update the tray icon */
            CheckAndSetTrayIcon();

            /* And the texts in the status window */
            SetDlgItemText(
                c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_RECONNECTING));
            SetDlgItemTextW(c->hwndStatus, ID_TXT_IP, L"");
            SetStatusWinIcon(c->hwndStatus, ID_ICO_CONNECTING);
        }

        /* close QR dialog, if any */
        CloseQRDialog();
    }
    else if (strcmp(state, "ASSIGN_IP") == 0)
    {
        /* first state after completion of web-based auth, now it is safe to close QR dialog */
        CloseQRDialog();
    }
    else
    {
        CheckAndSetTrayIcon();
    }
}

static void
SimulateButtonPress(HWND hwnd, UINT btn)
{
    SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(btn, BN_CLICKED), (LPARAM)GetDlgItem(hwnd, btn));
}

/* A private struct to keep autoclose parameters */
typedef struct autoclose
{
    UINT start;      /* start time in msec */
    UINT timeout;    /* timeout in msec at which autoclose triggers */
    UINT btn;        /* button which is 'pressed' for autoclose */
    UINT txtid;      /* id of a text control to display an informaltional message */
    UINT txtres;     /* resource id of localized text to use for message:
                      * LoadLocalizedString(txtid, time_remaining_in_seconds)
                      * is used to generate the message */
    COLORREF txtclr; /* color for message text */
} autoclose;

/* Cancel scheduled auto close of a dialog */
static void
AutoCloseCancel(HWND hwnd)
{
    autoclose *ac = (autoclose *)GetProp(hwnd, L"AutoClose");
    if (!ac)
    {
        return;
    }

    if (ac->txtid)
    {
        SetDlgItemText(hwnd, ac->txtid, L"");
    }
    KillTimer(hwnd, 1);
    RemoveProp(hwnd, L"AutoClose");
    free(ac);
}

/* Called when autoclose timer expires. */
static void CALLBACK
AutoCloseHandler(HWND hwnd, UINT UNUSED msg, UINT_PTR id, DWORD now)
{
    autoclose *ac = (autoclose *)GetProp(hwnd, L"AutoClose");
    if (!ac)
    {
        return;
    }

    DWORD elapsed = now - ac->start;
    if (elapsed >= ac->timeout)
    {
        SimulateButtonPress(hwnd, ac->btn);
        AutoCloseCancel(hwnd);
    }
    else
    {
        SetDlgItemText(
            hwnd, ac->txtid, LoadLocalizedString(ac->txtres, (ac->timeout - elapsed) / 1000));
        SetTimer(hwnd, id, 500, AutoCloseHandler);
    }
}

/* Schedule an event to automatically activate specified btn after timeout seconds.
 * Used for automatically closing a dialog after some delay unless user interrupts.
 * Optionally a txtid and txtres resource may be specified to display a message.
 * %u in the message is replaced by time remaining before timeout will trigger.
 * Call AutoCloseCancel to cancel the scheduled event and clear the displayed
 * text.
 */
static void
AutoCloseSetup(HWND hwnd, UINT btn, UINT timeout, UINT txtid, UINT txtres)
{
    if (timeout == 0)
    {
        SimulateButtonPress(hwnd, btn);
    }
    else
    {
        autoclose *ac = GetPropW(hwnd, L"AutoClose"); /* reuse if set */
        if (!ac && (ac = malloc(sizeof(autoclose))) == NULL)
        {
            return;
        }

        *ac = (autoclose){ .start = GetTickCount(),
                           .timeout = timeout * 1000,
                           .btn = btn,
                           .txtid = txtid,
                           .txtres = txtres,
                           .txtclr = GetSysColor(COLOR_WINDOWTEXT) };

        SetTimer(hwnd, 1, 500, AutoCloseHandler); /* using timer id = 1 */
        if (txtid && txtres)
        {
            SetDlgItemText(hwnd, txtid, LoadLocalizedString(txtres, timeout));
        }
        SetPropW(hwnd, L"AutoClose", (HANDLE)ac); /* failure not critical */
    }
}

/*
 * DialogProc for OpenVPN username/password/challenge auth dialog windows
 */
INT_PTR CALLBACK
UserAuthDialogFunc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auth_param_t *param;
    WCHAR username[USER_PASS_LEN] = L"";
    WCHAR password[USER_PASS_LEN] = L"";

    switch (msg)
    {
        case WM_INITDIALOG:
            /* Set connection for this dialog and show it */
            param = (auth_param_t *)lParam;
            TRY_SETPROP(hwndDlg, cfgProp, (HANDLE)param);
            SetStatusWinIcon(hwndDlg, ID_ICO_APP);

            if (param->str)
            {
                LPWSTR wstr = Widen(param->str);
                HWND wnd_challenge = GetDlgItem(hwndDlg, ID_EDT_AUTH_CHALLENGE);

                if (!wstr)
                {
                    WriteStatusLog(param->c,
                                   L"GUI> ",
                                   L"Error converting challenge string to widechar",
                                   false);
                }
                else
                {
                    SetDlgItemTextW(hwndDlg, ID_TXT_AUTH_CHALLENGE, wstr);
                }

                free(wstr);

                /* Set/Remove style ES_PASSWORD by SetWindowLong(GWL_STYLE) does nothing,
                 * send EM_SETPASSWORDCHAR just works. */
                if (param->flags & FLAG_CR_ECHO)
                {
                    SendMessage(wnd_challenge, EM_SETPASSWORDCHAR, 0, 0);
                }
            }
            else if (param->flags & FLAG_CR_TYPE_CONCAT)
            {
                SetDlgItemTextW(
                    hwndDlg, ID_TXT_AUTH_CHALLENGE, LoadLocalizedString(IDS_NFO_OTP_PROMPT));
            }
            if (RecallUsername(param->c->config_name, username))
            {
                SetDlgItemTextW(hwndDlg, ID_EDT_AUTH_USER, username);
                SetFocus(GetDlgItem(hwndDlg, ID_EDT_AUTH_PASS));
            }
            if (RecallAuthPass(param->c->config_name, password))
            {
                SetDlgItemTextW(hwndDlg, ID_EDT_AUTH_PASS, password);
                if (username[0] != L'\0'
                    && !(param->flags & (FLAG_CR_TYPE_SCRV1 | FLAG_CR_TYPE_CONCAT))
                    && password[0] != L'\0' && param->c->failed_auth_attempts == 0)
                {
                    /* user/pass available and no challenge response needed: skip dialog
                     * if silent_connection is on, else auto submit after a few seconds.
                     * User can interrupt.
                     */
                    SetFocus(GetDlgItem(hwndDlg, IDOK));
                    UINT timeout = o.silent_connection ? 0 : 6; /* in seconds */
                    AutoCloseSetup(hwndDlg, IDOK, timeout, ID_TXT_WARNING, IDS_NFO_AUTO_CONNECT);
                }
                /* if auth failed, highlight password so that user can type over */
                else if (param->c->failed_auth_attempts)
                {
                    SendMessage(
                        GetDlgItem(hwndDlg, ID_EDT_AUTH_PASS), EM_SETSEL, 0, MAKELONG(0, -1));
                }
                else if (param->flags & (FLAG_CR_TYPE_SCRV1 | FLAG_CR_TYPE_CONCAT))
                {
                    SetFocus(GetDlgItem(hwndDlg, ID_EDT_AUTH_CHALLENGE));
                }
                SecureZeroMemory(password, sizeof(password));
            }
            if (param->c->flags & FLAG_DISABLE_SAVE_PASS)
            {
                ShowWindow(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), SW_HIDE);
            }
            else if (param->c->flags & FLAG_SAVE_AUTH_PASS)
            {
                Button_SetCheck(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), BST_CHECKED);
            }

            SetWindowText(hwndDlg, param->c->config_name);
            if (param->c->failed_auth_attempts > 0)
            {
                SetDlgItemTextW(
                    hwndDlg, ID_TXT_WARNING, LoadLocalizedString(IDS_NFO_AUTH_PASS_RETRY));
            }

            if (param->c->state == resuming)
            {
                ForceForegroundWindow(hwndDlg);
            }
            else
            {
                SetForegroundWindow(hwndDlg);
            }
            ResetPasswordReveal(
                GetDlgItem(hwndDlg, ID_EDT_AUTH_PASS), GetDlgItem(hwndDlg, ID_PASSWORD_REVEAL), 0);
            break;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
            /* user interrupt */
            AutoCloseCancel(hwndDlg);
            break;

        case WM_COMMAND:
            param = (auth_param_t *)GetProp(hwndDlg, cfgProp);
            switch (LOWORD(wParam))
            {
                case ID_EDT_AUTH_PASS:
                    ResetPasswordReveal(GetDlgItem(hwndDlg, ID_EDT_AUTH_PASS),
                                        GetDlgItem(hwndDlg, ID_PASSWORD_REVEAL),
                                        wParam);

                /* fall through */
                case ID_EDT_AUTH_USER:
                case ID_EDT_AUTH_CHALLENGE:
                    if (HIWORD(wParam) == EN_UPDATE)
                    {
                        /* enable OK button only if username and either password or response are
                         * filled */
                        BOOL enableOK =
                            GetWindowTextLength(GetDlgItem(hwndDlg, ID_EDT_AUTH_USER))
                            && (GetWindowTextLength(GetDlgItem(hwndDlg, ID_EDT_AUTH_PASS))
                                || ((param->flags & (FLAG_CR_TYPE_SCRV1 | FLAG_CR_TYPE_CONCAT))
                                    && GetWindowTextLength(
                                        GetDlgItem(hwndDlg, ID_EDT_AUTH_CHALLENGE))));
                        EnableWindow(GetDlgItem(hwndDlg, IDOK), enableOK);
                    }
                    AutoCloseCancel(hwndDlg); /* user interrupt */
                    break;

                case ID_CHK_SAVE_PASS:
                    param->c->flags ^= FLAG_SAVE_AUTH_PASS;
                    if (param->c->flags & FLAG_SAVE_AUTH_PASS)
                    {
                        Button_SetCheck(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), BST_CHECKED);
                    }
                    else
                    {
                        DeleteSavedAuthPass(param->c->config_name);
                        Button_SetCheck(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), BST_UNCHECKED);
                    }
                    AutoCloseCancel(hwndDlg); /* user interrupt */
                    break;

                case IDOK:
                    if (GetDlgItemTextW(hwndDlg, ID_EDT_AUTH_USER, username, _countof(username)))
                    {
                        if (!validate_input(username, L"\n"))
                        {
                            show_error_tip(GetDlgItem(hwndDlg, ID_EDT_AUTH_USER),
                                           LoadLocalizedString(IDS_ERR_INVALID_USERNAME_INPUT));
                            return 0;
                        }
                        SaveUsername(param->c->config_name, username);
                    }
                    if (GetDlgItemTextW(hwndDlg, ID_EDT_AUTH_PASS, password, _countof(password)))
                    {
                        if (!validate_input(password, L"\n"))
                        {
                            show_error_tip(GetDlgItem(hwndDlg, ID_EDT_AUTH_PASS),
                                           LoadLocalizedString(IDS_ERR_INVALID_PASSWORD_INPUT));
                            SecureZeroMemory(password, sizeof(password));
                            return 0;
                        }
                        if (param->c->flags & FLAG_SAVE_AUTH_PASS && wcslen(password))
                        {
                            SaveAuthPass(param->c->config_name, password);
                        }
                        if (param->flags & FLAG_CR_TYPE_CONCAT)
                        {
                            GetDlgItemTextW(hwndDlg,
                                            ID_EDT_AUTH_CHALLENGE,
                                            password + wcslen(password),
                                            _countof(password) - wcslen(password));
                            SetDlgItemTextW(hwndDlg, ID_EDT_AUTH_PASS, password);
                            /* erase potentially secret contents in the response text box */
                            memset(password, L'x', wcslen(password));
                            SetDlgItemTextW(hwndDlg, ID_EDT_AUTH_CHALLENGE, password);
                        }

                        SecureZeroMemory(password, sizeof(password));
                    }
                    ManagementCommandFromInput(
                        param->c, "username \"Auth\" \"%s\"", hwndDlg, ID_EDT_AUTH_USER);

                    if (param->flags & FLAG_CR_TYPE_SCRV1)
                    {
                        ManagementCommandFromTwoInputsBase64(param->c,
                                                             "password \"Auth\" \"SCRV1:%s:%s\"",
                                                             hwndDlg,
                                                             ID_EDT_AUTH_PASS,
                                                             ID_EDT_AUTH_CHALLENGE);
                    }
                    else
                    {
                        ManagementCommandFromInput(
                            param->c, "password \"Auth\" \"%s\"", hwndDlg, ID_EDT_AUTH_PASS);
                    }
                    EndDialog(hwndDlg, LOWORD(wParam));
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwndDlg, LOWORD(wParam));
                    StopOpenVPN(param->c);
                    return TRUE;

                case ID_PASSWORD_REVEAL: /* password reveal symbol clicked */
                    ChangePasswordVisibility(GetDlgItem(hwndDlg, ID_EDT_AUTH_PASS),
                                             GetDlgItem(hwndDlg, ID_PASSWORD_REVEAL),
                                             wParam);
                    return TRUE;
            }
            break;

        case WM_OVPN_STATE: /* state changed -- destroy the dialog */
            EndDialog(hwndDlg, LOWORD(wParam));
            return TRUE;

        case WM_CTLCOLORSTATIC:
            if (GetDlgCtrlID((HWND)lParam) == ID_TXT_WARNING)
            {
                autoclose *ac = (autoclose *)GetProp(hwndDlg, L"AutoClose");
                HBRUSH br = (HBRUSH)DefWindowProc(hwndDlg, msg, wParam, lParam);
                /* This text id is used for auth failure warning or autoclose message. Use
                 * appropriate color */
                COLORREF clr = o.clr_warning;
                if (ac && ac->txtid == ID_TXT_WARNING)
                {
                    clr = ac->txtclr;
                }
                SetTextColor((HDC)wParam, clr);
                return (INT_PTR)br;
            }
            break;

        case WM_CLOSE:
            EndDialog(hwndDlg, LOWORD(wParam));
            param = (auth_param_t *)GetProp(hwndDlg, cfgProp);
            StopOpenVPN(param->c);
            return TRUE;

        case WM_NCDESTROY:
            param = (auth_param_t *)GetProp(hwndDlg, cfgProp);
            free_auth_param(param);
            AutoCloseCancel(hwndDlg);
            RemoveProp(hwndDlg, cfgProp);
            break;
    }

    return FALSE;
}

/*
 * DialogProc for challenge-response, token PIN etc.
 */
INT_PTR CALLBACK
GenericPassDialogFunc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auth_param_t *param;
    WCHAR password[USER_PASS_LEN];

    switch (msg)
    {
        case WM_INITDIALOG:
            param = (auth_param_t *)lParam;
            TRY_SETPROP(hwndDlg, cfgProp, (HANDLE)param);

            WCHAR *wstr = Widen(param->str);
            if (!wstr)
            {
                WriteStatusLog(
                    param->c, L"GUI> ", L"Error converting challenge string to widechar", false);
                EndDialog(hwndDlg, LOWORD(wParam));
                break;
            }
            if (param->flags & FLAG_CR_TYPE_CRV1 || param->flags & FLAG_CR_TYPE_CRTEXT)
            {
                SetDlgItemTextW(hwndDlg, ID_TXT_DESCRIPTION, wstr);

                /* Set password echo on if needed */
                if (param->flags & FLAG_CR_ECHO)
                {
                    SendMessage(GetDlgItem(hwndDlg, ID_EDT_RESPONSE), EM_SETPASSWORDCHAR, 0, 0);
                }

                /* Rendered size of challenge text and window rectangle */
                SIZE sz = { 0 };
                RECT rect = { 0 };
                HDC hdc = GetDC(GetDlgItem(hwndDlg, ID_TXT_DESCRIPTION));
                GetWindowRect(hwndDlg, &rect);
                rect.right -= rect.left;
                rect.bottom -= rect.top;

                /* if space for text + some margin exceeds the window size, resize */
                if (GetTextExtentPoint32W(hdc, wstr, wcslen(wstr), &sz)
                    && LPtoDP(hdc, (POINT *)&sz, 1)        /* logical to device units */
                    && sz.cx + DPI_SCALE(15) > rect.right) /* 15 nominal pixel margin space */
                {
                    /* new horizontal dimension with a max of 640 nominal pixels */
                    rect.right = min(DPI_SCALE(640), sz.cx + DPI_SCALE(15));
                    SetWindowPos(hwndDlg, NULL, 0, 0, rect.right, rect.bottom, SWP_NOMOVE);
                    PrintDebug(L"Window resized to = %d %d", rect.right, rect.bottom);
                }
                ShowWindow(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), SW_HIDE);
            }
            else if (param->flags & FLAG_PASS_TOKEN)
            {
                SetWindowText(hwndDlg, LoadLocalizedString(IDS_NFO_TOKEN_PASSWORD_CAPTION));
                SetDlgItemText(hwndDlg,
                               ID_TXT_DESCRIPTION,
                               LoadLocalizedString(IDS_NFO_TOKEN_PASSWORD_REQUEST, param->id));

                if (param->c->flags & FLAG_DISABLE_SAVE_PASS) 
                {
                    ShowWindow(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), SW_HIDE);
                }
                else
                {
                    if (RecallSmartCardPin(param->c->config_name, password))
                    {
                        if (password[0] != L'\0' && param->c->failed_auth_attempts == 0)
                        {
                            SetDlgItemTextW(hwndDlg, ID_EDT_RESPONSE, password);
                            Button_SetCheck(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), BST_CHECKED);

                            const char * template = "password \"%s\" \"%%s\"";
                            char* fmt = malloc(strlen(template) + strlen(param->id));
                            if (fmt)
                            {
                                sprintf(fmt, template, param->id);
                                PrintDebug(L"Send passwd to mgmt with format: '%hs'", fmt);
                                ManagementCommandFromInput(param->c, fmt, hwndDlg, ID_EDT_RESPONSE);
                                free(fmt);

                                EndDialog(hwndDlg, IDOK);
                                return TRUE;
                            }
                            else /* no memory? send stop signal */
                            {
                                WriteStatusLog(param->c,
                                            L"GUI> ",
                                            L"Out of memory in password dialog: sending stop signal",
                                            false);
                                StopOpenVPN(param->c);
                            }
                        } 
                        else if (param->c->failed_auth_attempts > 0)
                        {
                            SendMessage(
                                GetDlgItem(hwndDlg, ID_EDT_RESPONSE), EM_SETSEL, 0, MAKELONG(0, -1));

                            SetDlgItemTextW(
                                hwndDlg, ID_TXT_WARNING, LoadLocalizedString(IDS_NFO_KEY_PASS_RETRY));

                            // Extend window size for Warning
                            RECT rect = { 0 };
                            GetWindowRect(hwndDlg, &rect);
                            rect.right -= rect.left;
                            rect.bottom -= rect.top;

                            HWND warningText = GetDlgItem(hwndDlg, ID_TXT_WARNING);
                            RECT warningRect = { 0 };
                            GetWindowRect(warningText, &warningRect);

                            SetWindowPos(hwndDlg, NULL, 0, 0, rect.right, rect.bottom + warningRect.bottom - warningRect.top + DPI_SCALE(4), SWP_NOMOVE);
                        }

                        SecureZeroMemory(password, sizeof(password));
                    }
                }
            }
            else
            {
                WriteStatusLog(param->c, L"GUI> ", L"Unknown password request", false);
                SetDlgItemText(hwndDlg, ID_TXT_DESCRIPTION, wstr);
                ShowWindow(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), SW_HIDE);
            }
            free(wstr);

            AppendTextToCaption(hwndDlg, param->c->config_name);
            if (param->c->state == resuming)
            {
                ForceForegroundWindow(hwndDlg);
            }
            else
            {
                SetForegroundWindow(hwndDlg);
            }

            /* If response is not required hide the response field */
            if ((param->flags & FLAG_CR_TYPE_CRV1 || param->flags & FLAG_CR_TYPE_CRTEXT)
                && !(param->flags & FLAG_CR_RESPONSE))
            {
                ShowWindow(GetDlgItem(hwndDlg, ID_LTEXT_RESPONSE), SW_HIDE);
                ShowWindow(GetDlgItem(hwndDlg, ID_EDT_RESPONSE), SW_HIDE);
                ShowWindow(GetDlgItem(hwndDlg, ID_PASSWORD_REVEAL), SW_HIDE);
            }
            else
            {
                /* disable OK button until response is filled-in */
                EnableWindow(GetDlgItem(hwndDlg, IDOK), FALSE);
                ResetPasswordReveal(GetDlgItem(hwndDlg, ID_EDT_RESPONSE),
                                    GetDlgItem(hwndDlg, ID_PASSWORD_REVEAL),
                                    0);
            }

            break;

        case WM_COMMAND:
            param = (auth_param_t *)GetProp(hwndDlg, cfgProp);
            const char *template;
            char *fmt;
            switch (LOWORD(wParam))
            {
                case ID_EDT_RESPONSE:
                    if (!(param->flags & FLAG_CR_ECHO) && HIWORD(wParam) != EN_KILLFOCUS)
                    {
                        ResetPasswordReveal(GetDlgItem(hwndDlg, ID_EDT_RESPONSE),
                                            GetDlgItem(hwndDlg, ID_PASSWORD_REVEAL),
                                            wParam);
                    }
                    if (HIWORD(wParam) == EN_UPDATE)
                    {
                        /* enable OK if response is non-empty */
                        BOOL enableOK = GetWindowTextLength((HWND)lParam);
                        EnableWindow(GetDlgItem(hwndDlg, IDOK), enableOK);
                    }
                    break;

                case ID_CHK_SAVE_PASS:
                    param->c->flags ^= FLAG_SAVE_KEY_PASS;
                    if (param->c->flags & FLAG_SAVE_KEY_PASS)
                    {
                        Button_SetCheck(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), BST_CHECKED);
                    }
                    else
                    {
                        DeleteSavedSmartCardPin(param->c->config_name);
                        Button_SetCheck(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), BST_UNCHECKED);
                    }
                    break; 

                case IDOK:
                    if (GetDlgItemTextW(hwndDlg, ID_EDT_RESPONSE, password, _countof(password))
                        && !validate_input(password, L"\n"))
                    {
                        show_error_tip(GetDlgItem(hwndDlg, ID_EDT_RESPONSE),
                                       LoadLocalizedString(IDS_ERR_INVALID_PASSWORD_INPUT));
                        SecureZeroMemory(password, sizeof(password));
                        return 0;
                    }
                    if (param->flags & FLAG_CR_TYPE_CRTEXT)
                    {
                        ManagementCommandFromInputBase64(
                            param->c, "cr-response \"%s\"", hwndDlg, ID_EDT_RESPONSE);
                        EndDialog(hwndDlg, LOWORD(wParam));
                        return TRUE;
                    }
                    if (param->flags & FLAG_CR_TYPE_CRV1)
                    {
                        /* send username */
                        template = "username \"Auth\" \"%s\"";
                        char *username = escape_string(param->user);
                        fmt = malloc(strlen(template) + strlen(username));

                        if (fmt && username)
                        {
                            sprintf(fmt, template, username);
                            ManagementCommand(param->c, fmt, NULL, regular);
                        }
                        else /* no memory? send an emty username and let it error out */
                        {
                            WriteStatusLog(
                                param->c,
                                L"GUI> ",
                                L"Out of memory: sending a generic username for dynamic CR",
                                false);
                            ManagementCommand(
                                param->c, "username \"Auth\" \"user\"", NULL, regular);
                        }
                        free(fmt);
                        free(username);

                        /* password template */
                        template = "password \"Auth\" \"CRV1::%s::%%s\"";
                    }
                    else /* generic password request of type param->id */
                    {
                        template = "password \"%s\" \"%%s\"";
                    }

                    fmt = malloc(strlen(template) + strlen(param->id));
                    if (fmt)
                    {
                        sprintf(fmt, template, param->id);
                        PrintDebug(L"Send passwd to mgmt with format: '%hs'", fmt);
                        ManagementCommandFromInput(param->c, fmt, hwndDlg, ID_EDT_RESPONSE);
                        free(fmt);

                        if (param->flags & FLAG_PASS_TOKEN && param->c->flags & FLAG_SAVE_KEY_PASS)
                        {
                            SaveSmartCardPin(param->c->config_name, password);
                        }
                    }
                    else /* no memory? send stop signal */
                    {
                        WriteStatusLog(param->c,
                                       L"GUI> ",
                                       L"Out of memory in password dialog: sending stop signal",
                                       false);
                        StopOpenVPN(param->c);
                    }

                    EndDialog(hwndDlg, LOWORD(wParam));
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwndDlg, LOWORD(wParam));
                    StopOpenVPN(param->c);
                    return TRUE;

                case ID_PASSWORD_REVEAL: /* password reveal symbol clicked */
                    ChangePasswordVisibility(GetDlgItem(hwndDlg, ID_EDT_RESPONSE),
                                             GetDlgItem(hwndDlg, ID_PASSWORD_REVEAL),
                                             wParam);
                    return TRUE;
            }
            break;

        case WM_OVPN_STATE: /* state change message is received from OpenVPN daemon */
            /*
             * AUTH_PENDING immediately transitions to GET_CONFIG until
             * auth succeeds or connection restarts/aborts.
             * Close the CR_TEXT dialog if state changes to anything
             * other than GET_CONFIG. The state is in lParam.
             * For other auth dialogs any state change signals a restart: end the dialog
             */
            param = (auth_param_t *)GetProp(hwndDlg, cfgProp);
            if (!(param->flags & FLAG_CR_TYPE_CRTEXT) || strcmp((const char *)lParam, "GET_CONFIG"))
            {
                EndDialog(hwndDlg, LOWORD(wParam));
            }
            return TRUE;

        case WM_CLOSE:
            EndDialog(hwndDlg, LOWORD(wParam));
            return TRUE;

        case WM_NCDESTROY:
            param = (auth_param_t *)GetProp(hwndDlg, cfgProp);
            free_auth_param(param);
            RemoveProp(hwndDlg, cfgProp);
            break;
    }

    return FALSE;
}

/*
 * DialogProc for OpenVPN private key password dialog windows
 */
INT_PTR CALLBACK
PrivKeyPassDialogFunc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    connection_t *c;
    WCHAR passphrase[KEY_PASS_LEN];

    switch (msg)
    {
        case WM_INITDIALOG:
            /* Set connection for this dialog and show it */
            c = (connection_t *)lParam;
            TRY_SETPROP(hwndDlg, cfgProp, (HANDLE)c);
            AppendTextToCaption(hwndDlg, c->config_name);
            if (RecallKeyPass(c->config_name, passphrase) && wcslen(passphrase)
                && c->failed_psw_attempts == 0)
            {
                /* Use the saved password and skip the dialog */
                SetDlgItemTextW(hwndDlg, ID_EDT_PASSPHRASE, passphrase);
                SecureZeroMemory(passphrase, sizeof(passphrase));
                ManagementCommandFromInput(
                    c, "password \"Private Key\" \"%s\"", hwndDlg, ID_EDT_PASSPHRASE);
                EndDialog(hwndDlg, IDOK);
                return TRUE;
            }
            if (c->flags & FLAG_DISABLE_SAVE_PASS)
            {
                ShowWindow(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), SW_HIDE);
            }
            else if (c->flags & FLAG_SAVE_KEY_PASS)
            {
                Button_SetCheck(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), BST_CHECKED);
            }
            if (c->failed_psw_attempts > 0)
            {
                SetDlgItemTextW(
                    hwndDlg, ID_TXT_WARNING, LoadLocalizedString(IDS_NFO_KEY_PASS_RETRY));
            }
            if (c->state == resuming)
            {
                ForceForegroundWindow(hwndDlg);
            }
            else
            {
                SetForegroundWindow(hwndDlg);
            }

            /* disable OK button by default - not disabled in resources */
            EnableWindow(GetDlgItem(hwndDlg, IDOK), FALSE);
            ResetPasswordReveal(
                GetDlgItem(hwndDlg, ID_EDT_PASSPHRASE), GetDlgItem(hwndDlg, ID_PASSWORD_REVEAL), 0);
            break;

        case WM_COMMAND:
            c = (connection_t *)GetProp(hwndDlg, cfgProp);
            switch (LOWORD(wParam))
            {
                case ID_CHK_SAVE_PASS:
                    c->flags ^= FLAG_SAVE_KEY_PASS;
                    if (c->flags & FLAG_SAVE_KEY_PASS)
                    {
                        Button_SetCheck(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), BST_CHECKED);
                    }
                    else
                    {
                        Button_SetCheck(GetDlgItem(hwndDlg, ID_CHK_SAVE_PASS), BST_UNCHECKED);
                        DeleteSavedKeyPass(c->config_name);
                    }
                    break;

                case ID_EDT_PASSPHRASE:
                    ResetPasswordReveal(GetDlgItem(hwndDlg, ID_EDT_PASSPHRASE),
                                        GetDlgItem(hwndDlg, ID_PASSWORD_REVEAL),
                                        wParam);
                    if (HIWORD(wParam) == EN_UPDATE)
                    {
                        /* enable OK if response is non-empty */
                        BOOL enableOK = GetWindowTextLength((HWND)lParam);
                        EnableWindow(GetDlgItem(hwndDlg, IDOK), enableOK);
                    }
                    break;

                case IDOK:
                    if (GetDlgItemTextW(
                            hwndDlg, ID_EDT_PASSPHRASE, passphrase, _countof(passphrase)))
                    {
                        if (!validate_input(passphrase, L"\n"))
                        {
                            show_error_tip(GetDlgItem(hwndDlg, ID_EDT_PASSPHRASE),
                                           LoadLocalizedString(IDS_ERR_INVALID_PASSWORD_INPUT));
                            SecureZeroMemory(passphrase, sizeof(passphrase));
                            return 0;
                        }
                        if ((c->flags & FLAG_SAVE_KEY_PASS) && wcslen(passphrase) > 0)
                        {
                            SaveKeyPass(c->config_name, passphrase);
                        }
                        SecureZeroMemory(passphrase, sizeof(passphrase));
                    }
                    ManagementCommandFromInput(
                        c, "password \"Private Key\" \"%s\"", hwndDlg, ID_EDT_PASSPHRASE);
                    EndDialog(hwndDlg, LOWORD(wParam));
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwndDlg, LOWORD(wParam));
                    StopOpenVPN(c);
                    return TRUE;

                case ID_PASSWORD_REVEAL: /* password reveal symbol clicked */
                    ChangePasswordVisibility(GetDlgItem(hwndDlg, ID_EDT_PASSPHRASE),
                                             GetDlgItem(hwndDlg, ID_PASSWORD_REVEAL),
                                             wParam);
                    return TRUE;
            }
            break;

        case WM_OVPN_STATE: /* state changed -- destroy the dialog */
            EndDialog(hwndDlg, LOWORD(wParam));
            return TRUE;

        case WM_CTLCOLORSTATIC:
            if (GetDlgCtrlID((HWND)lParam) == ID_TXT_WARNING)
            {
                HBRUSH br = (HBRUSH)DefWindowProc(hwndDlg, msg, wParam, lParam);
                SetTextColor((HDC)wParam, o.clr_warning);
                return (INT_PTR)br;
            }
            break;

        case WM_CLOSE:
            EndDialog(hwndDlg, LOWORD(wParam));
            return TRUE;

        case WM_NCDESTROY:
            RemoveProp(hwndDlg, cfgProp);
            break;
    }
    return FALSE;
}

static void
free_dynamic_cr(connection_t *c)
{
    free(c->dynamic_cr);
    c->dynamic_cr = NULL;
}

/*
 * Parse dynamic challenge string received from the server. Returns
 * true on success. The caller must free param->str and param->id
 * even on error.
 */
BOOL
parse_dynamic_cr(const char *str, auth_param_t *param)
{
    BOOL ret = FALSE;
    char *token[4] = { 0 };
    char *p = strdup(str);

    int i;
    char *p1;

    if (!param || !p)
    {
        goto out;
    }

    /* expected: str = "E,R:challenge_id:user_b64:challenge_str" */
    const char *delim = ":";
    for (i = 0, p1 = p; i < 4; ++i, p1 = NULL)
    {
        if (i == 3)
        {
            delim = "";               /* take the entire trailing string as the challenge */
        }
        token[i] = strtok(p1, delim); /* strtok is thread-safe on Windows */
        if (!token[i])
        {
            WriteStatusLog(param->c, L"GUI> ", L"Error parsing dynamic challenge string", false);
            goto out;
        }
    }

    if (Base64Decode(token[2], &param->user) < 0)
    {
        WriteStatusLog(
            param->c, L"GUI> ", L"Error decoding the username in dynamic challenge string", false);
        goto out;
    }

    param->flags |= FLAG_CR_TYPE_CRV1;
    param->flags |= strchr(token[0], 'E') ? FLAG_CR_ECHO : 0;
    param->flags |= strchr(token[0], 'R') ? FLAG_CR_RESPONSE : 0;
    param->id = strdup(token[1]);
    param->str = strdup(token[3]);
    if (!param->id || !param->str)
    {
        goto out;
    }

    ret = TRUE;

out:
    free(p);
    return ret;
}

/*
 * Parse crtext string received from the server. Returns
 * true on success. The caller must free param->str even on error.
 */
static BOOL
parse_crtext(const char *str, auth_param_t *param)
{
    BOOL ret = FALSE;
    char *token[2] = { 0 };
    char *p = strdup(str);
    char *p1;

    if (!param || !p)
    {
        goto out;
    }

    /* expected: str = "E,R:challenge_str" */
    token[0] = p;
    p1 = strchr(p, ':');
    if (!p1)
    {
        WriteStatusLog(param->c, L"GUI> ", L"Error parsing crtext challenge string", false);
        goto out;
    }

    *p1 = '\0';
    token[1] = p1 + 1;

    param->flags |= FLAG_CR_TYPE_CRTEXT;
    param->flags |= strchr(token[0], 'E') ? FLAG_CR_ECHO : 0;
    param->flags |= strchr(token[0], 'R') ? FLAG_CR_RESPONSE : 0;
    param->str = strdup(token[1]);
    if (!param->str)
    {
        goto out;
    }

    ret = TRUE;

out:
    free(p);
    return ret;
}

/*
 * Parse password or string request of the form "Need 'What' password/string MSG:message"
 * and assign param->id = What, param->str = message. Also set param->flags if the type
 * of the requested info is known. If message is empty param->id is copied to param->str.
 * Return true on succsess. The caller must free param even when the function fails.
 */
static BOOL
parse_input_request(const char *msg, auth_param_t *param)
{
    BOOL ret = FALSE;
    char *p = strdup(msg);
    char *sep[4] = { " ", "'", " ", "" }; /* separators to use to break up msg */
    char *token[4];

    char *p1 = p;
    for (int i = 0; i < 4; ++i, p1 = NULL)
    {
        token[i] = strtok(p1, sep[i]); /* strtok is thread-safe on Windows */
        if (!token[i] && i < 3)        /* first three tokens required */
        {
            goto out;
        }
    }
    if (token[3] && strncmp(token[3], "MSG:", 4) == 0)
    {
        token[3] += 4;
    }
    if (!token[3] || !*token[3]) /* use id as the description if none provided */
    {
        token[3] = token[1];
    }

    PrintDebug(L"Tokens: '%hs' '%hs' '%hs' '%hs'", token[0], token[1], token[2], token[3]);

    if (strcmp(token[0], "Need") != 0)
    {
        goto out;
    }

    if ((param->id = strdup(token[1])) == NULL)
    {
        goto out;
    }

    if (strcmp(token[2], "password") == 0)
    {
        if (strcmp(param->id, "Private Key") == 0)
        {
            param->flags |= FLAG_PASS_PKEY;
        }
        else
        {
            param->flags |= FLAG_PASS_TOKEN;
        }
    }
    else if (strcmp(token[2], "string") == 0 && strcmp(param->id, "pkcs11-id-request") == 0)
    {
        param->flags |= FLAG_STRING_PKCS11;
    }

    param->str = strdup(token[3]);
    if (param->str == NULL)
    {
        goto out;
    }

    PrintDebug(L"parse_input_request: id = '%hs' str = '%hs' flags = %u",
               param->id,
               param->str,
               param->flags);
    ret = TRUE;

out:
    free(p);
    if (!ret)
    {
        PrintDebug(L"Error parsing password/string request msg: <%hs>", msg);
    }
    return ret;
}

/*
 * Handle >ECHO: request from OpenVPN management interface
 * Expect msg = timestamp,message
 */
void
OnEcho(connection_t *c, char *msg)
{
    time_t timestamp = strtoul(msg, NULL, 10); /* openvpn prints these as %u */

    PrintDebug(L"OnEcho with msg = %hs", msg);
    if (!(msg = strchr(msg, ',')))
    {
        PrintDebug(L"OnEcho: msg format not recognized");
        return;
    }
    msg++;
    if (strcmp(msg, "forget-passwords") == 0)
    {
        DeleteSavedPasswords(c->config_name);
    }
    else if (strcmp(msg, "save-passwords") == 0)
    {
        if (c->flags & FLAG_DISABLE_SAVE_PASS)
        {
            WriteStatusLog(c,
                           L"GUI> echo save-passwords: ",
                           L"Ignored as disable_save_passwords is enabled.",
                           false);
        }
        else
        {
            c->flags |= (FLAG_SAVE_KEY_PASS | FLAG_SAVE_AUTH_PASS);
        }
    }
    else if (strbegins(msg, "setenv "))
    {
        process_setenv(c, timestamp, msg);
    }
    else if (strbegins(msg, "msg"))
    {
        echo_msg_process(c, timestamp, msg);
    }
    else
    {
        wchar_t errmsg[256];
        _sntprintf_0(errmsg, L"WARNING: Unknown ECHO directive '%hs' ignored.", msg);
        WriteStatusLog(c, L"GUI> ", errmsg, false);
    }
}

/*
 * Handle >PASSWORD: request from OpenVPN management interface
 */
void
OnPassword(connection_t *c, char *msg)
{
    PrintDebug(L"OnPassword with msg = %hs", msg);
    if (strncmp(msg, "Verification Failed", 19) == 0)
    {
        /* If the failure is due to dynamic challenge save the challenge string */
        char *chstr = strstr(msg, "CRV1:");

        free_dynamic_cr(c);
        if (chstr)
        {
            chstr += 5; /* beginning of dynamic CR string */

            /* Save the string for later processing during next Auth request */
            c->dynamic_cr = strdup(chstr);
            if (c->dynamic_cr && (chstr = strstr(c->dynamic_cr, "']")) != NULL)
            {
                *chstr = '\0';
            }

            PrintDebug(L"Got dynamic challenge: <%hs>", c->dynamic_cr);
        }

        return;
    }

    if (strstr(msg, "'Auth'"))
    {
        char *chstr;
        auth_param_t *param = (auth_param_t *)calloc(1, sizeof(auth_param_t));

        if (!param)
        {
            WriteStatusLog(
                c, L"GUI> ", L"Error: Out of memory - ignoring user-auth request", false);
            return;
        }
        param->c = c;

        if (c->dynamic_cr)
        {
            if (!parse_dynamic_cr(c->dynamic_cr, param))
            {
                WriteStatusLog(c, L"GUI> ", L"Error parsing dynamic challenge string", FALSE);
                free_dynamic_cr(c);
                free_auth_param(param);
                return;
            }
            LocalizedDialogBoxParamEx(
                ID_DLG_CHALLENGE_RESPONSE, c->hwndStatus, GenericPassDialogFunc, (LPARAM)param);
            free_dynamic_cr(c);
        }
        else if ((chstr = strstr(msg, "SC:")) && strlen(chstr) > 5)
        {
            ULONG flags = strtoul(chstr + 3, NULL, 10);
            param->flags |= (flags & 0x2) ? FLAG_CR_TYPE_CONCAT : FLAG_CR_TYPE_SCRV1;
            param->flags |= (flags & 0x1) ? FLAG_CR_ECHO : 0;
            param->str = strdup(chstr + 5);
            LocalizedDialogBoxParamEx(
                ID_DLG_AUTH_CHALLENGE, c->hwndStatus, UserAuthDialogFunc, (LPARAM)param);
        }
        else if (o.auth_pass_concat_otp)
        {
            param->flags |= FLAG_CR_ECHO | FLAG_CR_TYPE_CONCAT;
            LocalizedDialogBoxParamEx(
                ID_DLG_AUTH_CHALLENGE, c->hwndStatus, UserAuthDialogFunc, (LPARAM)param);
        }
        else
        {
            LocalizedDialogBoxParamEx(
                ID_DLG_AUTH, c->hwndStatus, UserAuthDialogFunc, (LPARAM)param);
        }
    }
    else if (strstr(msg, "'Private Key'"))
    {
        LocalizedDialogBoxParamEx(
            ID_DLG_PASSPHRASE, c->hwndStatus, PrivKeyPassDialogFunc, (LPARAM)c);
    }
    else if (strstr(msg, "'HTTP Proxy'"))
    {
        QueryProxyAuth(c, http);
    }
    else if (strstr(msg, "'SOCKS Proxy'"))
    {
        QueryProxyAuth(c, socks);
    }
    /* All other password requests such as PKCS11 pin */
    else if (strncmp(msg, "Need '", 6) == 0)
    {
        auth_param_t *param = (auth_param_t *)calloc(1, sizeof(auth_param_t));

        if (!param)
        {
            WriteStatusLog(
                c, L"GUI> ", L"Error: Out of memory - ignoring user-auth request", false);
            return;
        }
        param->c = c;
        if (!parse_input_request(msg, param))
        {
            free_auth_param(param);
            return;
        }
        LocalizedDialogBoxParamEx(
            ID_DLG_CHALLENGE_RESPONSE, c->hwndStatus, GenericPassDialogFunc, (LPARAM)param);
    }
}

/*
 * Handle management connection timeout
 */
void
OnTimeout(connection_t *c, UNUSED char *msg)
{
    /* Connection to management timed out -- keep trying unless killed
     * by a startup error from service or from openvpn daemon process.
     * The user can terminate by pressing disconnect.
     */
    if (o.silent_connection == 0)
    {
        SetForegroundWindow(c->hwndStatus);
        ShowWindow(c->hwndStatus, SW_SHOW);
    }
    WriteStatusLog(c, L"GUI> ", LoadLocalizedString(IDS_NFO_CONN_TIMEOUT, c->log_path), false);
    WriteStatusLog(c, L"GUI> ", L"Retrying. Press disconnect to abort", false);
    c->state = connecting;
    if (!OpenManagement(c))
    {
        MessageBoxExW(c->hwndStatus,
                      L"Failed to open management",
                      _T(PACKAGE_NAME),
                      MB_OK | MB_SETFOREGROUND | MB_ICONERROR | MBOX_RTL_FLAGS,
                      GetGUILanguage());
        StopOpenVPN(c);
    }
    return;
}

/*
 * Handle exit of the OpenVPN process
 */
void
OnStop(connection_t *c, UNUSED char *msg)
{
    UINT txt_id, msg_id;
    SetMenuStatus(c, disconnected);

    switch (c->state)
    {
        case connected:
            /* OpenVPN process ended unexpectedly */
            c->failed_psw_attempts = 0;
            c->failed_auth_attempts = 0;
            c->state = disconnected;
            CheckAndSetTrayIcon();
            SetDlgItemText(
                c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_DISCONNECTED));
            SetStatusWinIcon(c->hwndStatus, ID_ICO_DISCONNECTED);
            EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), FALSE);
            EnableWindow(GetDlgItem(c->hwndStatus, ID_RESTART), FALSE);
            if (o.silent_connection == 0)
            {
                SetForegroundWindow(c->hwndStatus);
                ShowWindow(c->hwndStatus, SW_SHOW);
            }
            MessageBoxExW(c->hwndStatus,
                          LoadLocalizedString(IDS_NFO_CONN_TERMINATED, c->config_file),
                          _T(PACKAGE_NAME),
                          MB_OK | MBOX_RTL_FLAGS,
                          GetGUILanguage());
            SendMessage(c->hwndStatus, WM_CLOSE, 0, 0);
            break;

        case resuming:
        case connecting:
        case reconnecting:
            /* We have failed to (re)connect */
            txt_id = c->state == reconnecting ? IDS_NFO_STATE_FAILED_RECONN : IDS_NFO_STATE_FAILED;
            msg_id = c->state == reconnecting ? IDS_NFO_RECONN_FAILED : IDS_NFO_CONN_FAILED;

            c->state = disconnecting;
            CheckAndSetTrayIcon();
            c->state = disconnected;
            EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), FALSE);
            EnableWindow(GetDlgItem(c->hwndStatus, ID_RESTART), FALSE);
            SetStatusWinIcon(c->hwndStatus, ID_ICO_DISCONNECTED);
            SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(txt_id));
            if (o.silent_connection == 0)
            {
                SetForegroundWindow(c->hwndStatus);
                ShowWindow(c->hwndStatus, SW_SHOW);
            }
            MessageBoxExW(c->hwndStatus,
                          LoadLocalizedString(msg_id, c->config_name),
                          _T(PACKAGE_NAME),
                          MB_OK | MBOX_RTL_FLAGS,
                          GetGUILanguage());
            SendMessage(c->hwndStatus, WM_CLOSE, 0, 0);
            break;

        case disconnecting:
            /* Shutdown was initiated by us */
            c->failed_psw_attempts = 0;
            c->failed_auth_attempts = 0;
            c->state = disconnected;
            if (c->flags & FLAG_DAEMON_PERSISTENT)
            {
                /* user initiated disconnection -- stay detached and do not auto-reconnect */
                c->auto_connect = false;
            }
            CheckAndSetTrayIcon();
            SetDlgItemText(
                c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_DISCONNECTED));
            SendMessage(c->hwndStatus, WM_CLOSE, 0, 0);
            break;

        case onhold:
        /* stop triggered while on hold -- possibly the daemon exited. Treat same as detaching */
        case detaching:
            c->state = disconnected;
            CheckAndSetTrayIcon();
            SendMessage(c->hwndStatus, WM_CLOSE, 0, 0);
            break;

        case suspending:
            c->state = suspended;
            CheckAndSetTrayIcon();
            SetDlgItemText(
                c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_SUSPENDED));
            break;

        default:
            break;
    }
}

/* Convert bytecount c to a human readable string.
 * Input c converted to a string of the form "nnn.. (xxx.y XiB)"
 * where X is K, M, G, T, P or E depending on the count. The result
 * is returned in buf up to max of len - 1 wide characters.
 * Return value: pointer to buf.
 */
static wchar_t *
format_bytecount(wchar_t *buf, size_t len, unsigned long long c)
{
    const char *suf[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", NULL };
    const char **s = suf;
    double x = c;

    if (c <= 1024)
    {
        swprintf(buf, len, L"%I64u B", c);
        buf[len - 1] = L'\0';
        return buf;
    }
    while (x > 1024 && *(s + 1))
    {
        x /= 1024.0;
        s++;
    }
    swprintf(buf, len, L"%I64u (%.1f %hs)", c, x, *s);
    buf[len - 1] = L'\0';

    return buf;
}

/*
 * Handle bytecount report from OpenVPN
 * Expect bytes-in,bytes-out
 */
void
OnByteCount(connection_t *c, char *msg)
{
    if (!msg || sscanf(msg, "%" SCNu64 ",%" SCNu64, &c->bytes_in, &c->bytes_out) != 2)
    {
        return;
    }
    wchar_t in[32], out[32];
    format_bytecount(in, _countof(in), c->bytes_in);
    format_bytecount(out, _countof(out), c->bytes_out);
    SetDlgItemTextW(
        c->hwndStatus, ID_TXT_BYTECOUNT, LoadLocalizedString(IDS_NFO_BYTECOUNT, in, out));
}

/*
 * Handle INFOMSG from OpenVPN. At the moment it handles
 * WEB_AUTH:<flags>:<url>, OPEN_URL:<url> and CR_TEXT:<flags>:<challenge-str> messages
 * used by two-step authentication.
 */
void
OnInfoMsg(connection_t *c, char *msg)
{
    PrintDebug(L"OnInfoMsg with msg = %hs", msg);

    char *p = NULL;
    wchar_t *url = NULL;

    if (strbegins(msg, "OPEN_URL:"))
    {
        url = Widen(msg + 9);
    }
    else if (strbegins(msg, "WEB_AUTH:") && (p = strchr(msg + 9, ':')) != NULL)
    {
        if (p != msg + 9)
        {
            /* flags not empty: log a message that we do not support any flags */
            *p = '\0';
            wchar_t *flags = Widen(msg + 9);
            if (flags)
            {
                WriteStatusLog(
                    c, L"GUI> Unsupported flags in WEB_AUTH request ignored: ", flags, false);
            }
            free(flags);
        }
        url = Widen(p + 1);
    }

    if (url)
    {
        if (o.use_qr_for_url)
        {
            OpenQRDialog(url, c->config_name);
        }
        else
        {
            if (!open_url(url))
            {
                WriteStatusLog(c, L"GUI> ", L"Error: failed to open url from info msg", false);
            }
        }
        free(url);
    }
    else if (strbegins(msg, "CR_TEXT:"))
    {
        auth_param_t *param = (auth_param_t *)calloc(1, sizeof(auth_param_t));
        if (!param)
        {
            WriteStatusLog(c, L"GUI> ", L"Error: Out of memory - ignoring CR_TEXT request", false);
            return;
        }
        param->c = c;

        if (!parse_crtext(msg + 8, param))
        {
            WriteStatusLog(c, L"GUI> ", L"Error parsing crtext string", FALSE);

            free_auth_param(param);
            return;
        }
        LocalizedDialogBoxParamEx(
            ID_DLG_CHALLENGE_RESPONSE, c->hwndStatus, GenericPassDialogFunc, (LPARAM)param);
    }
}

/*
 * Break a long line into shorter segments
 */
static WCHAR *
WrapLine(WCHAR *line)
{
    int i = 0;
    WCHAR *next = NULL;
    int len = 80;

    for (i = 0; *line; i++, ++line)
    {
        if ((*line == L'\r') || (*line == L'\n'))
        {
            *line = L' ';
        }
        if (next && i > len)
        {
            break;
        }
        if (iswspace(*line))
        {
            next = line;
        }
    }
    if (!*line)
    {
        next = NULL;
    }
    if (next)
    {
        *next = L'\0';
        ++next;
    }
    return next;
}

/*
 * Write a line to the status log window and optionally to the log file
 */
void
WriteStatusLog(connection_t *c, const WCHAR *prefix, const WCHAR *line, BOOL fileio)
{
    /* this can be called without connection (AS profile import), so do nothing in this case */
    if (!c)
    {
        return;
    }

    HWND logWnd = GetDlgItem(c->hwndStatus, ID_EDT_LOG);
    FILE *log_fd;
    time_t now;
    WCHAR datetime[26];

    time(&now);
    /* TODO: change this to use _wctime_s when mingw supports it */
    wcsncpy(datetime, _wctime(&now), _countof(datetime));
    datetime[24] = L' ';

    /* change text color if Warning or Error */
    COLORREF text_clr = 0;

    if (wcsstr(prefix, L"ERROR"))
    {
        text_clr = o.clr_error;
    }
    else if (wcsstr(prefix, L"WARNING"))
    {
        text_clr = o.clr_warning;
    }

    if (text_clr != 0)
    {
        CHARFORMAT cfm = {
            .cbSize = sizeof(CHARFORMAT),
            .dwMask = CFM_COLOR | CFM_BOLD,
            .dwEffects = 0,
            .crTextColor = text_clr,
        };
        SendMessage(logWnd, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfm);
    }


    /* Remove lines from log window if it is getting full */
    if (SendMessage(logWnd, EM_GETLINECOUNT, 0, 0) > MAX_LOG_LINES)
    {
        int pos = SendMessage(logWnd, EM_LINEINDEX, DEL_LOG_LINES, 0);
        SendMessage(logWnd, EM_SETSEL, 0, pos);
        SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM) _T(""));
    }
    /* Append line to log window */
    SendMessage(logWnd, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM)datetime);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM)prefix);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessage(logWnd, EM_REPLACESEL, FALSE, (LPARAM)L"\n");

    if (!fileio)
    {
        return;
    }

    log_fd = _tfopen(c->log_path, TEXT("at+,ccs=UTF-8"));
    if (log_fd)
    {
        fwprintf(log_fd, L"%ls%ls%ls\n", datetime, prefix, line);
        fclose(log_fd);
    }
}

#define IO_TIMEOUT 5000 /* milliseconds */

static void
CloseServiceIO(service_io_t *s)
{
    if (s->hEvent)
    {
        CloseHandle(s->hEvent);
    }
    s->hEvent = NULL;
    if (s->pipe && s->pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s->pipe);
    }
    s->pipe = NULL;
}

/*
 * Open the service pipe and initialize service I/O.
 * Failure is not fatal.
 */
static BOOL
InitServiceIO(service_io_t *s)
{
    DWORD dwMode =
        o.ovpn_engine == OPENVPN_ENGINE_OVPN3 ? PIPE_READMODE_BYTE : PIPE_READMODE_MESSAGE;

    CLEAR(*s);

    /* auto-reset event used for signalling i/o completion*/
    s->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!s->hEvent)
    {
        return FALSE;
    }

    s->pipe = CreateFile(o.ovpn_engine == OPENVPN_ENGINE_OVPN3 ? OPENVPN_SERVICE_PIPE_NAME_OVPN3
                                                               : OPENVPN_SERVICE_PIPE_NAME_OVPN2,
                         GENERIC_READ | GENERIC_WRITE,
                         0,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,
                         NULL);

    if (!s->pipe || s->pipe == INVALID_HANDLE_VALUE
        || !SetNamedPipeHandleState(s->pipe, &dwMode, NULL, NULL))
    {
        CloseServiceIO(s);
        return FALSE;
    }

    return TRUE;
}

/*
 * Read-completion routine for interactive service pipe. Call with
 * err = 0, bytes = 0 to queue a new read request.
 */
static void WINAPI
HandleServiceIO(DWORD err, DWORD bytes, LPOVERLAPPED lpo)
{
    service_io_t *s = (service_io_t *)lpo;
    int len, capacity;

    len = _countof(s->readbuf);
    capacity = (len - 1) * sizeof(*(s->readbuf));

    if (bytes > 0)
    {
        /* messages from the service are not nul terminated */
        int nchars = bytes / sizeof(s->readbuf[0]);
        s->readbuf[nchars] = L'\0';
        SetEvent(s->hEvent);
        return;
    }
    if (err)
    {
        _snwprintf_s(
            s->readbuf, len, _TRUNCATE, L"0x%08x\nInteractive Service disconnected\n", err);
        SetEvent(s->hEvent);
        return;
    }

    /* Otherwise queue next read request */
    ReadFileEx(s->pipe, s->readbuf, capacity, lpo, HandleServiceIO);
    /* Any error in the above call will get checked in next round */
}

static BOOL
ValidatePipe(connection_t *c)
{
    ULONG ppid = 0, spid = 0;

    if (!c->iserv.pipe)
    {
        return FALSE;
    }
    if (!GetNamedPipeServerProcessId(c->iserv.pipe, &ppid))
    {
        MsgToEventLog(EVENTLOG_ERROR_TYPE,
                      L"%hs:%d Failed to get pipe server process id: (error = 0x%08x)",
                      __func__,
                      __LINE__,
                      GetLastError());
        return FALSE;
    }
    spid = GetServicePid();

    return (ppid > 0) && (spid > 0) && (spid == ppid);
}

/*
 * Write size bytes in buf to the pipe with a timeout.
 * Retun value: TRUE on success FLASE on error
 */
static BOOL
WritePipe(HANDLE pipe, LPVOID buf, DWORD size)
{
    OVERLAPPED o;
    BOOL retval = FALSE;

    CLEAR(o);
    o.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (!o.hEvent)
    {
        return retval;
    }

    if (WriteFile(pipe, buf, size, NULL, &o) || GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(o.hEvent, IO_TIMEOUT) == WAIT_OBJECT_0)
        {
            retval = TRUE;
        }
        else
        {
            CancelIo(pipe);
        }
        /* TODO report error -- timeout */
    }

    CloseHandle(o.hEvent);
    return retval;
}

/*
 * Called when read from service pipe signals
 */
static void
OnService(connection_t *c, UNUSED char *msg)
{
    DWORD err = 0;
    DWORD pid = 0;
    WCHAR *p, *buf, *next;
    const WCHAR *prefix = L"IService> ";

    /*
     * Duplicate the read buffer and queue the next read request
     * by calling HandleServiceIO with err = 0, bytes = 0.
     */
    buf = wcsdup(c->iserv.readbuf);
    HandleServiceIO(0, 0, (LPOVERLAPPED)&c->iserv);

    if (buf == NULL)
    {
        return;
    }

    /* messages from the service are in the format "0x08x\n%s\n%s" */
    if (swscanf(buf, L"0x%08x\n", &err) != 1)
    {
        free(buf);
        return;
    }

    p = buf + 11;
    /* next line is the pid if followed by "\nProcess ID" */
    if (!err && wcsstr(p, L"\nProcess ID") && swscanf(p, L"0x%08x", &pid) == 1 && pid != 0)
    {
        PrintDebug(L"Process ID of openvpn started by IService: %d", pid);
        c->hProcess = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!c->hProcess)
        {
            PrintDebug(L"Failed to get process handle from pid of openvpn: error = %lu",
                       GetLastError());
        }
        free(buf);
        return;
    }

    while (iswspace(*p))
    {
        ++p;
    }

    while (p && *p)
    {
        next = WrapLine(p);
        WriteStatusLog(c, prefix, p, false);
        p = next;
    }
    free(buf);

    /* Error from iservice before management interface is connected */
    switch (err)
    {
        case 0:
            break;

        case ERROR_STARTUP_DATA:
            WriteStatusLog(c, prefix, L"OpenVPN not started due to previous errors", true);
            OnStop(c, NULL);
            break;

        case ERROR_OPENVPN_STARTUP:
            WriteStatusLog(c, prefix, L"Check the log file for details", false);
            OnStop(c, NULL);
            break;

        default:
            OnStop(c, NULL);
            break;
    }
}

/*
 * Called when the directly started openvpn process exits
 */
static void
OnProcess(connection_t *c, UNUSED char *msg)
{
    DWORD err;
    WCHAR tmp[256];

    if (!GetExitCodeProcess(c->hProcess, &err) || err == STILL_ACTIVE)
    {
        return;
    }

    _snwprintf_s(tmp,
                 _countof(tmp),
                 _TRUNCATE,
                 L"OpenVPN terminated with exit code %lu. "
                 L"See the log file for details",
                 err);
    WriteStatusLog(c, L"OpenVPN GUI> ", tmp, false);

    OnStop(c, NULL);
}

/*
 * Called when NEED-OK is received
 */
void
OnNeedOk(connection_t *c, char *msg)
{
    char *resp = NULL;
    WCHAR *wstr = NULL;
    auth_param_t *param = (auth_param_t *)calloc(1, sizeof(auth_param_t));

    if (!param)
    {
        WriteStatusLog(c,
                       L"GUI> ",
                       L"Error: out of memory while processing NEED-OK. Sending stop signal",
                       false);
        StopOpenVPN(c);
        return;
    }
    if (!parse_input_request(msg, param))
    {
        goto out;
    }

    /* allocate space for response : "needok param->id cancel/ok" */
    resp = malloc(strlen(param->id) + strlen("needok \' \' cancel"));
    wstr = Widen(param->str);

    if (!wstr || !resp)
    {
        WriteStatusLog(c,
                       L"GUI> ",
                       L"Error: out of memory while processing NEED-OK. Sending stop signal",
                       false);
        StopOpenVPN(c);
        goto out;
    }

    const char *fmt;
    if (MessageBoxExW(
            c->hwndStatus, wstr, L"" PACKAGE_NAME, MB_OKCANCEL | MBOX_RTL_FLAGS, GetGUILanguage())
        == IDOK)
    {
        fmt = "needok \'%s\' ok";
    }
    else
    {
        ManagementCommand(c, "auth-retry none", NULL, regular);
        fmt = "needok \'%s\' cancel";
    }

    sprintf(resp, fmt, param->id);
    ManagementCommand(c, resp, NULL, regular);

out:
    free_auth_param(param);
    free(wstr);
    free(resp);
}

/*
 * Called when NEED-STR is received
 */
void
OnNeedStr(connection_t *c, UNUSED char *msg)
{
    if (strstr(msg, "Need 'pkcs11-id-request'"))
    {
        msg = strstr(msg, "MSG:");
        OnPkcs11(c, msg ? msg + 4 : "");
        return;
    }
    WriteStatusLog(c, L"GUI> ", L"Error: Received NEED-STR message -- not implemented", false);
}

/* Stop the connection -- this sets the daemon to exit if
 * started by us, else instructs the daemon to disconnect and
 * and wait.
 */
static void
DisconnectDaemon(connection_t *c)
{
    if (c->flags & FLAG_DAEMON_PERSISTENT)
    {
        if (c->manage.connected > 1) /* connected and ready for input */
        {
            ManagementCommand(c, "forget-passwords", NULL, regular);
            ManagementCommand(c, "hold on", NULL, regular);
            ManagementCommand(c, "signal SIGHUP", NULL, regular);
        }
        else
        {
            OnStop(c, NULL);
        }
    }
    else
    {
        SetEvent(c->exit_event);
        SetTimer(c->hwndStatus, IDT_STOP_TIMER, 15000, NULL);
    }
}

/*
 * Close open handles
 */
static void
Cleanup(connection_t *c)
{
    CloseManagement(c);

    free_dynamic_cr(c);
    env_item_del_all(c->es);
    c->es = NULL;
    echo_msg_clear(c, true); /* clear history */
    pkcs11_list_clear(&c->pkcs11_list);

    if (c->hProcess)
    {
        CloseHandle(c->hProcess);
    }
    c->hProcess = NULL;

    if (c->iserv.hEvent)
    {
        CloseServiceIO(&c->iserv);
    }

    if (c->exit_event)
    {
        CloseHandle(c->exit_event);
    }
    c->exit_event = NULL;
    c->daemon_state[0] = '\0';
}
/*
 * Helper to position and scale widgets in status window using current dpi
 * Takes status window width and height in screen pixels as input
 */
void
RenderStatusWindow(HWND hwndDlg, UINT w, UINT h)
{
    MoveWindow(GetDlgItem(hwndDlg, ID_EDT_LOG),
               DPI_SCALE(20),
               DPI_SCALE(25),
               w - DPI_SCALE(40),
               h - DPI_SCALE(110),
               TRUE);
    MoveWindow(GetDlgItem(hwndDlg, ID_TXT_STATUS),
               DPI_SCALE(20),
               DPI_SCALE(5),
               w - DPI_SCALE(30),
               DPI_SCALE(15),
               TRUE);
    MoveWindow(GetDlgItem(hwndDlg, ID_TXT_IP),
               DPI_SCALE(20),
               h - DPI_SCALE(75),
               w - DPI_SCALE(30),
               DPI_SCALE(15),
               TRUE);
    MoveWindow(GetDlgItem(hwndDlg, ID_TXT_BYTECOUNT),
               DPI_SCALE(20),
               h - DPI_SCALE(55),
               w - DPI_SCALE(210),
               DPI_SCALE(15),
               TRUE);
    MoveWindow(GetDlgItem(hwndDlg, ID_TXT_VERSION),
               w - DPI_SCALE(180),
               h - DPI_SCALE(55),
               DPI_SCALE(170),
               DPI_SCALE(15),
               TRUE);
    MoveWindow(GetDlgItem(hwndDlg, ID_DISCONNECT),
               DPI_SCALE(20),
               h - DPI_SCALE(30),
               DPI_SCALE(110),
               DPI_SCALE(23),
               TRUE);
    MoveWindow(GetDlgItem(hwndDlg, ID_RESTART),
               DPI_SCALE(145),
               h - DPI_SCALE(30),
               DPI_SCALE(110),
               DPI_SCALE(23),
               TRUE);
    MoveWindow(GetDlgItem(hwndDlg, ID_DETACH),
               DPI_SCALE(270),
               h - DPI_SCALE(30),
               DPI_SCALE(110),
               DPI_SCALE(23),
               TRUE);
    MoveWindow(GetDlgItem(hwndDlg, ID_HIDE),
               w - DPI_SCALE(130),
               h - DPI_SCALE(30),
               DPI_SCALE(110),
               DPI_SCALE(23),
               TRUE);
}

/*
 * DialogProc for OpenVPN status dialog windows
 */
INT_PTR CALLBACK
StatusDialogFunc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    connection_t *c;

    switch (msg)
    {
        case WM_MANAGEMENT:
            /* Management interface related event */
            OnManagement(wParam, lParam);
            return TRUE;

        case WM_INITDIALOG:
            c = (connection_t *)lParam;

            /* Set window icon "disconnected" */
            SetStatusWinIcon(hwndDlg, ID_ICO_CONNECTING);

            /* Set connection for this dialog */
            if (!SetPropW(hwndDlg, cfgProp, (HANDLE)c))
            {
                MsgToEventLog(EVENTLOG_ERROR_TYPE,
                              L"%hs:%d SetProp failed (error = 0x%08x)",
                              __func__,
                              __LINE__,
                              GetLastError());
                DisconnectDaemon(c);
                DestroyWindow(hwndDlg);
                break;
            }

            /* Create log window */
            HWND hLogWnd =
                CreateWindowEx(0,
                               RICHEDIT_CLASS,
                               NULL,
                               WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL | ES_SUNKEN | ES_LEFT
                                   | ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL | ES_AUTOVSCROLL,
                               20,
                               25,
                               350,
                               160,
                               hwndDlg,
                               (HMENU)ID_EDT_LOG,
                               o.hInstance,
                               NULL);
            if (!hLogWnd)
            {
                ShowLocalizedMsgEx(MB_OK | MB_ICONERROR,
                                   c->hwndStatus,
                                   TEXT(PACKAGE_NAME),
                                   IDS_ERR_CREATE_EDIT_LOGWINDOW);
                /* We can't continue without a log window */
                StopOpenVPN(c);
                return FALSE;
            }

            /* Set font and fontsize of the log window */
            CHARFORMAT cfm = { .cbSize = sizeof(CHARFORMAT),
                               .dwMask = CFM_SIZE | CFM_FACE | CFM_BOLD,
                               .szFaceName = _T("Microsoft Sans Serif"),
                               .dwEffects = 0,
                               .yHeight = 160 };
            if (SendMessage(hLogWnd, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cfm) == 0)
            {
                ShowLocalizedMsgEx(
                    MB_OK | MB_ICONERROR, c->hwndStatus, TEXT(PACKAGE_NAME), IDS_ERR_SET_SIZE);
            }

            /* display version string as "OpenVPN GUI gui_version/core_version" */
            wchar_t version[256];
            _sntprintf_0(
                version, L"%hs %hs/%hs", PACKAGE_NAME, PACKAGE_VERSION_RESOURCE_STR, o.ovpn_version)
                SetDlgItemText(hwndDlg, ID_TXT_VERSION, version);

            /* Set size and position of controls */
            RECT rect;
            GetWindowRect(hwndDlg, &rect);
            /* Move the window by upto 100 random pixels to avoid all
             * status windows fall on top of each other.
             */
            SetWindowPos(hwndDlg,
                         HWND_TOP,
                         rect.left + rand() % 100,
                         rect.top + rand() % 100,
                         0,
                         0,
                         SWP_NOSIZE);
            GetClientRect(hwndDlg, &rect);
            RenderStatusWindow(hwndDlg, rect.right, rect.bottom);
            if (c->flags & FLAG_DAEMON_PERSISTENT && o.enable_persistent > 0)
            {
                EnableWindow(GetDlgItem(hwndDlg, ID_DETACH), TRUE);
            }
            else
            {
                ShowWindow(GetDlgItem(hwndDlg, ID_DETACH), SW_HIDE);
            }
            /* Set focus on the LogWindow so it scrolls automatically */
            SetFocus(hLogWnd);
            return FALSE;

        case WM_DPICHANGED:
            DpiSetScale(&o, HIWORD(wParam));
            RECT dlgRect;
            GetClientRect(hwndDlg, &dlgRect);
            RenderStatusWindow(hwndDlg, dlgRect.right, dlgRect.bottom);
            return FALSE;

        case WM_SIZE:
            RenderStatusWindow(hwndDlg, LOWORD(lParam), HIWORD(lParam));
            InvalidateRect(hwndDlg, NULL, TRUE);
            return TRUE;

        case WM_COMMAND:
            c = (connection_t *)GetProp(hwndDlg, cfgProp);
            switch (LOWORD(wParam))
            {
                case ID_DISCONNECT:
                    SetFocus(GetDlgItem(c->hwndStatus, ID_EDT_LOG));
                    StopOpenVPN(c);
                    return TRUE;

                case ID_HIDE:
                    if (c->state != disconnected)
                    {
                        ShowWindow(hwndDlg, SW_HIDE);
                    }
                    else
                    {
                        DestroyWindow(hwndDlg);
                    }
                    return TRUE;

                case ID_RESTART:
                    SetFocus(GetDlgItem(c->hwndStatus, ID_EDT_LOG));
                    RestartOpenVPN(c);
                    return TRUE;

                case ID_DETACH:
                    SetFocus(GetDlgItem(c->hwndStatus, ID_EDT_LOG));
                    DetachOpenVPN(c);
                    return TRUE;
            }
            break;

        case WM_SHOWWINDOW:
            if (wParam == TRUE)
            {
                SetFocus(GetDlgItem(hwndDlg, ID_EDT_LOG));
            }
            return FALSE;

        case WM_CLOSE:
            c = (connection_t *)GetProp(hwndDlg, cfgProp);
            if (c->state != disconnected && c->state != detached)
            {
                ShowWindow(hwndDlg, SW_HIDE);
            }
            else
            {
                DestroyWindow(hwndDlg);
            }
            return TRUE;

        case WM_NCDESTROY:
            KillTimer(hwndDlg, IDT_STOP_TIMER);
            RemoveProp(hwndDlg, cfgProp);
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        case WM_OVPN_RELEASE:
            c = (connection_t *)GetProp(hwndDlg, cfgProp);
            c->state = reconnecting;
            SetDlgItemText(
                c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_RECONNECTING));
            SetDlgItemTextW(c->hwndStatus, ID_TXT_IP, L"");
            SetStatusWinIcon(c->hwndStatus, ID_ICO_CONNECTING);
            OnHold(c, "");
            break;

        case WM_OVPN_STOP:
            c = (connection_t *)GetProp(hwndDlg, cfgProp);
            /* external messages can trigger when we are not ready -- check the state */
            if (!IsWindowEnabled(GetDlgItem(c->hwndStatus, ID_DISCONNECT)) || c->state == onhold)
            {
                break;
            }
            c->state = disconnecting;
            if (!(c->flags & FLAG_DAEMON_PERSISTENT))
            {
                RunDisconnectScript(c, false);
            }
            EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), FALSE);
            EnableWindow(GetDlgItem(c->hwndStatus, ID_RESTART), FALSE);
            SetMenuStatus(c, disconnecting);
            SetDlgItemText(
                c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_WAIT_TERM));
            DisconnectDaemon(c);
            break;

        case WM_OVPN_DETACH:
            c = (connection_t *)GetProp(hwndDlg, cfgProp);
            /* just stop the thread keeping openvpn.exe running */
            c->state = detaching;
            EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), FALSE);
            EnableWindow(GetDlgItem(c->hwndStatus, ID_RESTART), FALSE);
            OnStop(c, NULL);
            break;

        case WM_OVPN_SUSPEND:
            c = (connection_t *)GetProp(hwndDlg, cfgProp);
            c->state = suspending;
            EnableWindow(GetDlgItem(c->hwndStatus, ID_DISCONNECT), FALSE);
            EnableWindow(GetDlgItem(c->hwndStatus, ID_RESTART), FALSE);
            SetMenuStatus(c, disconnecting);
            SetDlgItemText(
                c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_WAIT_TERM));
            SetEvent(c->exit_event);
            SetTimer(hwndDlg, IDT_STOP_TIMER, 15000, NULL);
            break;

        case WM_TIMER:
            PrintDebug(L"WM_TIMER message with wParam = %lu", wParam);
            c = (connection_t *)GetProp(hwndDlg, cfgProp);
            if (wParam == IDT_STOP_TIMER)
            {
                /* openvpn failed to respond to stop signal -- terminate */
                TerminateOpenVPN(c);
                KillTimer(hwndDlg, IDT_STOP_TIMER);
                OnStop(c, NULL);
            }
            break;

        case WM_OVPN_RESTART:
            c = (connection_t *)GetProp(hwndDlg, cfgProp);
            /* external messages can trigger when we are not ready -- check the state */
            if (IsWindowEnabled(GetDlgItem(c->hwndStatus, ID_RESTART)))
            {
                c->state = reconnecting;
                ManagementCommand(c, "signal SIGHUP", NULL, regular);
                SetDlgItemText(
                    c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_RECONNECTING));
                SetDlgItemTextW(c->hwndStatus, ID_TXT_IP, L"");
                SetStatusWinIcon(c->hwndStatus, ID_ICO_CONNECTING);
            }
            if (!o.silent_connection)
            {
                SetForegroundWindow(c->hwndStatus);
                ShowWindow(c->hwndStatus, SW_SHOW);
            }
            break;
    }
    return FALSE;
}

/*
 * ThreadProc for OpenVPN status dialog windows
 */
static DWORD WINAPI
ThreadOpenVPNStatus(void *p)
{
    connection_t *c = p;
    TCHAR conn_name[200];
    MSG msg;
    HANDLE wait_event;

    CLEAR(msg);
    srand(c->threadId);

    /* Cut of extention from config filename. */
    _tcsncpy(conn_name, c->config_file, _countof(conn_name));
    conn_name[_tcslen(conn_name) - _tcslen(o.ext_string) - 1] = _T('\0');

    /* Create and Show Status Dialog */
    c->hwndStatus = CreateLocalizedDialogParam(ID_DLG_STATUS, StatusDialogFunc, (LPARAM)c);
    if (!c->hwndStatus)
    {
        /* kill daemon process if we started it */
        SetEvent(c->exit_event);
        Cleanup(c);
        c->state = disconnected;
        return 1;
    }

    CheckAndSetTrayIcon();
    SetMenuStatus(c, connecting);
    SetDlgItemText(c->hwndStatus, ID_TXT_STATUS, LoadLocalizedString(IDS_NFO_STATE_CONNECTING));
    SetWindowText(c->hwndStatus, LoadLocalizedString(IDS_NFO_CONNECTION_XXX, conn_name));

    if (!OpenManagement(c))
    {
        MessageBoxExW(c->hwndStatus,
                      L"Failed to open management",
                      _T(PACKAGE_NAME),
                      MB_OK | MB_SETFOREGROUND | MB_ICONERROR | MBOX_RTL_FLAGS,
                      GetGUILanguage());
        StopOpenVPN(c);
    }

    /* Start the async read loop for service and set it as the wait event */
    if (c->iserv.hEvent)
    {
        HandleServiceIO(0, 0, (LPOVERLAPPED)&c->iserv);
        wait_event = c->iserv.hEvent;
    }
    else
    {
        wait_event = c->hProcess;
    }

    /* For persistent connections, popup the status win only if we're connecting manually */
    BOOL show_status_win = (o.silent_connection == 0);
    if ((c->flags & FLAG_DAEMON_PERSISTENT) && c->state == resuming)
    {
        show_status_win = false;
    }
    ShowWindow(c->hwndStatus, show_status_win ? SW_SHOW : SW_HIDE);

    /* Load echo msg histroy from registry */
    echo_msg_load(c);

    /* Run the message loop for the status window */
    while (WM_QUIT != msg.message)
    {
        DWORD res;
        if (wait_event == NULL) /* for persistent connections there is no wait_event */
        {
            res = GetMessage(&msg, NULL, 0, 0);
            if (res == (DWORD)-1) /* log the error and continue */
            {
                MsgToEventLog(EVENTLOG_WARNING_TYPE,
                              L"GetMessage for <%ls> returned error (status=%lu)",
                              c->config_name,
                              GetLastError());
                continue;
            }
            else if (res == 0) /* WM_QUIT */
            {
                break;
            }
        }
        else if (!PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if ((res = MsgWaitForMultipleObjectsEx(
                     1, &wait_event, INFINITE, QS_ALLINPUT, MWMO_ALERTABLE))
                == WAIT_OBJECT_0)
            {
                if (wait_event == c->hProcess)
                {
                    OnProcess(c, NULL);
                }
                else if (wait_event == c->iserv.hEvent)
                {
                    OnService(c, NULL);
                }
            }
            continue;
        }

        if (IsDialogMessage(c->hwndStatus, &msg) == 0)
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    /* release handles etc.*/
    Cleanup(c);
    c->hwndStatus = NULL;
    return 0;
}

/*
 * Set priority based on the registry or cmd-line value
 */
static BOOL
SetProcessPriority(DWORD *priority)
{
    *priority = NORMAL_PRIORITY_CLASS;
    if (!_tcscmp(o.priority_string, _T("IDLE_PRIORITY_CLASS")))
    {
        *priority = IDLE_PRIORITY_CLASS;
    }
    else if (!_tcscmp(o.priority_string, _T("BELOW_NORMAL_PRIORITY_CLASS")))
    {
        *priority = BELOW_NORMAL_PRIORITY_CLASS;
    }
    else if (!_tcscmp(o.priority_string, _T("NORMAL_PRIORITY_CLASS")))
    {
        *priority = NORMAL_PRIORITY_CLASS;
    }
    else if (!_tcscmp(o.priority_string, _T("ABOVE_NORMAL_PRIORITY_CLASS")))
    {
        *priority = ABOVE_NORMAL_PRIORITY_CLASS;
    }
    else if (!_tcscmp(o.priority_string, _T("HIGH_PRIORITY_CLASS")))
    {
        *priority = HIGH_PRIORITY_CLASS;
    }
    else
    {
        ShowLocalizedMsgEx(MB_OK | MB_ICONERROR,
                           o.hWnd,
                           TEXT(PACKAGE_NAME),
                           IDS_ERR_UNKNOWN_PRIORITY,
                           o.priority_string);
        return FALSE;
    }
    return TRUE;
}

#ifdef ENABLE_OVPN3
inline static struct json_object *
json_object_new_utf16_string(wchar_t *utf16)
{
    DWORD input_size = WideCharToMultiByte(CP_UTF8, 0, utf16, -1, NULL, 0, NULL, NULL);
    struct json_object *jobj = NULL;
    char *utf8 = malloc(input_size);
    if (!utf8)
    {
        goto out;
    }

    WideCharToMultiByte(CP_UTF8, 0, utf16, -1, utf8, input_size, NULL, NULL);

    jobj = json_object_new_string(utf8);
    free(utf8);

out:
    return jobj;
}

static char *
PrepareStartJsonRequest(connection_t *c, wchar_t *exit_event_name)
{
    const char *request_header =
        "POST /start HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: %zd\r\n\r\n";
    json_object *jobj = json_object_new_object();

    json_object_object_add(jobj, "config_file", json_object_new_utf16_string(c->config_file));
    json_object_object_add(jobj, "config_dir", json_object_new_utf16_string(c->config_dir));
    json_object_object_add(jobj, "exit_event_name", json_object_new_utf16_string(exit_event_name));
    json_object_object_add(jobj, "management_password", json_object_new_string(c->manage.password));
    json_object_object_add(
        jobj, "management_host", json_object_new_string(inet_ntoa(c->manage.skaddr.sin_addr)));
    json_object_object_add(
        jobj, "management_port", json_object_new_int(ntohs(c->manage.skaddr.sin_port)));
    json_object_object_add(jobj, "log", json_object_new_utf16_string(c->log_path));
    json_object_object_add(jobj, "log-append", json_object_new_int(o.log_append));

    const char *body = json_object_to_json_string(jobj);

    int len = snprintf(NULL, 0, request_header, strlen(body)) + strlen(body) + 1;
    char *request = calloc(1, len);
    if (request == NULL)
    {
        goto out;
    }
    sprintf_s(request, len, request_header, strlen(body));
    strcat_s(request, len, body);

out:
    json_object_put(jobj);
    return request;
}
#endif /* ifdef ENABLE_OVPN3 */

/* If state is on hold -- release */
void
ReleaseOpenVPN(connection_t *c)
{
    if (c->state != onhold)
    {
        return;
    }
    PostMessage(c->hwndStatus, WM_OVPN_RELEASE, 0, 0);
}

/* Start a thread to monitor a connection and launch openvpn.exe if required */
BOOL
StartOpenVPN(connection_t *c)
{
    CLEAR(c->ip);

    if (c->hwndStatus)
    {
        if (c->state == onhold)
        {
            ReleaseOpenVPN(c);
            return true;
        }
        PrintDebug(L"Connection request when already started -- ignored");
        /* the tread can hang around after disconnect if user has not dismissed any popups */
        if (c->state == disconnected)
        {
            WriteStatusLog(c,
                           L"OpenVPN GUI> ",
                           L"Complete any pending dialog before starting a new connection",
                           false);
        }
        if (!o.silent_connection)
        {
            SetForegroundWindow(c->hwndStatus);
            ShowWindow(c->hwndStatus, SW_SHOW);
        }
        return FALSE;
    }
    else if (c->state != disconnected && c->state != detached)
    {
        return FALSE;
    }

    PrintDebug(L"Starting openvpn on config %ls", c->config_name);

    /* Create thread to show the connection's status dialog */
    HANDLE hThread = CreateThread(NULL, 0, ThreadOpenVPNStatus, c, CREATE_SUSPENDED, &c->threadId);
    if (hThread == NULL)
    {
        ShowLocalizedMsgEx(
            MB_OK | MB_ICONERROR, o.hWnd, TEXT(PACKAGE_NAME), IDS_ERR_CREATE_THREAD_STATUS);
        return false;
    }

    if (c->flags & FLAG_DAEMON_PERSISTENT)
    {
        if (!ParseManagementAddress(c))
        {
            /* Show an error if manually connecting */
            if (c->state == disconnected)
            {
                ShowLocalizedMsgEx(MB_OK | MB_ICONERROR,
                                   o.hWnd,
                                   TEXT(PACKAGE_NAME),
                                   IDS_ERR_PARSE_MGMT_OPTION,
                                   c->config_dir,
                                   c->config_file);
            }
            else
            {
                c->state = disconnected;
            }
            TerminateThread(hThread, 1);
            return false;
        }
    }
    /* Launch openvpn.exe using the service or directly */
    else if (!LaunchOpenVPN(c))
    {
        TerminateThread(hThread, 1);
        return false;
    }

    c->state = (c->state == suspended || c->state == detached) ? resuming : connecting;

    /* Start the status dialog thread */
    ResumeThread(hThread);

    if (hThread && hThread != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hThread);
    }

    return true;
}

/*
 * Launch an OpenVPN process
 */
static BOOL
LaunchOpenVPN(connection_t *c)
{
    TCHAR cmdline[1024];
    TCHAR *options = cmdline + 8;
    TCHAR exit_event_name[17];
    HANDLE hStdInRead = NULL, hStdInWrite = NULL;
    HANDLE hNul = NULL;
    DWORD written;
    BOOL retval = FALSE;
    DWORD passwd_len = 16; /* incuding NUL */

    if (passwd_len > sizeof(c->manage.password))
    {
        passwd_len = sizeof(c->manage.password);
    }

    RunPreconnectScript(c);

    /* Create an event object to signal OpenVPN to exit */
    _sntprintf_0(exit_event_name, _T("%x%08x"), GetCurrentProcessId(), c->threadId);
    c->exit_event = CreateEvent(NULL, TRUE, FALSE, exit_event_name);
    if (c->exit_event == NULL)
    {
        ShowLocalizedMsgEx(MB_OK | MB_ICONERROR,
                           o.hWnd,
                           TEXT(PACKAGE_NAME),
                           IDS_ERR_CREATE_EVENT,
                           exit_event_name);
        goto out;
    }

    /* Create a management interface password */
    GetRandomPassword(c->manage.password, passwd_len - 1);

    find_free_tcp_port(&c->manage.skaddr);

    /* Construct command line -- put log first */
    _sntprintf_0(
        cmdline,
        _T("openvpn --log%ls \"%ls\" --config \"%ls\" "
           "--setenv IV_GUI_VER \"%hs %hs\" --setenv IV_SSO openurl,webauth,crtext --service %ls 0 --auth-retry interact "
           "--management %hs %hd stdin --management-query-passwords %ls"
           "--management-hold"),
        (o.log_append ? _T("-append") : _T("")),
        c->log_path,
        c->config_file,
        PACKAGE_NAME,
        PACKAGE_VERSION_RESOURCE_STR,
        exit_event_name,
        inet_ntoa(c->manage.skaddr.sin_addr),
        ntohs(c->manage.skaddr.sin_port),
        (o.proxy_source != config ? _T("--management-query-proxy ") : _T("")));

    BOOL use_iservice = (o.iservice_admin && IsWindows7OrGreater()) || !IsUserAdmin();
    /* Try to open the service pipe */
    if (use_iservice && InitServiceIO(&c->iserv))
    {
        BOOL res = FALSE;

        if (!ValidatePipe(c))
        {
            CloseHandle(c->exit_event);
            CloseServiceIO(&c->iserv);
            goto out;
        }

        if (o.ovpn_engine == OPENVPN_ENGINE_OVPN3)
        {
#ifdef ENABLE_OVPN3
            char *request = PrepareStartJsonRequest(c, exit_event_name);

            res = (request != NULL) && WritePipe(c->iserv.pipe, request, strlen(request));
            free(request);
#endif
        }
        else
        {
            DWORD size = _tcslen(c->config_dir) + _tcslen(options) + passwd_len + 3;
            TCHAR startup_info[1024];

            if (!AuthorizeConfig(c))
            {
                CloseHandle(c->exit_event);
                CloseServiceIO(&c->iserv);
                goto out;
            }

            c->hProcess = NULL;
            c->manage.password[passwd_len - 1] = '\n';

            /* Ignore pushed route-method when service is in use */
            const wchar_t *extra_options = L" --pull-filter ignore route-method";
            size += wcslen(extra_options);

            _sntprintf_0(startup_info,
                         L"%ls%lc%ls%ls%lc%.*hs",
                         c->config_dir,
                         L'\0',
                         options,
                         extra_options,
                         L'\0',
                         passwd_len,
                         c->manage.password);
            c->manage.password[passwd_len - 1] = '\0';

            res = WritePipe(c->iserv.pipe, startup_info, size * sizeof(TCHAR));
        }

        if (!res)
        {
            ShowLocalizedMsgEx(
                MB_OK | MB_ICONERROR, o.hWnd, TEXT(PACKAGE_NAME), IDS_ERR_WRITE_SERVICE_PIPE);
            CloseHandle(c->exit_event);
            CloseServiceIO(&c->iserv);
            goto out;
        }
    }
#ifdef ENABLE_OVPN3
    else if (o.ovpn_engine == OPENVPN_ENGINE_OVPN3)
    {
        ShowLocalizedMsgEx(
            MB_OK | MB_ICONERROR, o.hWnd, TEXT(PACKAGE_NAME), IDS_ERR_WRITE_SERVICE_PIPE);
        CloseHandle(c->exit_event);
        CloseServiceIO(&c->iserv);
        goto out;
    }
#endif
    else
    {
        /* Start OpenVPN directly */
        DWORD priority;
        STARTUPINFO si;
        PROCESS_INFORMATION pi;
        SECURITY_DESCRIPTOR sd;

        /* Make I/O handles inheritable and accessible by all */
        SECURITY_ATTRIBUTES sa = { .nLength = sizeof(sa),
                                   .lpSecurityDescriptor = &sd,
                                   .bInheritHandle = TRUE };

        if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
        {
            ShowLocalizedMsgEx(
                MB_OK | MB_ICONERROR, o.hWnd, TEXT(PACKAGE_NAME), IDS_ERR_INIT_SEC_DESC);
            CloseHandle(c->exit_event);
            return FALSE;
        }
        if (!SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE))
        {
            ShowLocalizedMsgEx(
                MB_OK | MB_ICONERROR, o.hWnd, TEXT(PACKAGE_NAME), IDS_ERR_SET_SEC_DESC_ACL);
            CloseHandle(c->exit_event);
            return FALSE;
        }

        /* Set process priority */
        if (!SetProcessPriority(&priority))
        {
            CloseHandle(c->exit_event);
            return FALSE;
        }

        /* Get a handle of the NUL device */
        hNul = CreateFile(_T("NUL"), GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        if (hNul == INVALID_HANDLE_VALUE)
        {
            CloseHandle(c->exit_event);
            return FALSE;
        }

        /* Create the pipe for STDIN with only the read end inheritable */
        if (!CreatePipe(&hStdInRead, &hStdInWrite, &sa, 0))
        {
            ShowLocalizedMsgEx(
                MB_OK | MB_ICONERROR, o.hWnd, TEXT(PACKAGE_NAME), IDS_ERR_CREATE_PIPE_IN_READ);
            CloseHandle(c->exit_event);
            goto out;
        }
        if (!SetHandleInformation(hStdInWrite, HANDLE_FLAG_INHERIT, 0))
        {
            ShowLocalizedMsgEx(
                MB_OK | MB_ICONERROR, o.hWnd, TEXT(PACKAGE_NAME), IDS_ERR_DUP_HANDLE_IN_WRITE);
            CloseHandle(c->exit_event);
            goto out;
        }

        /* Fill in STARTUPINFO struct */
        GetStartupInfo(&si);
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hStdInRead;
        si.hStdOutput = hNul;
        si.hStdError = hNul;

        /* Create an OpenVPN process for the connection */
        if (!CreateProcess(o.exe_path,
                           cmdline,
                           NULL,
                           NULL,
                           TRUE,
                           priority | CREATE_NO_WINDOW,
                           NULL,
                           c->config_dir,
                           &si,
                           &pi))
        {
            ShowLocalizedMsgEx(MB_OK | MB_ICONERROR,
                               o.hWnd,
                               TEXT(PACKAGE_NAME),
                               IDS_ERR_CREATE_PROCESS,
                               o.exe_path,
                               cmdline,
                               c->config_dir);
            CloseHandle(c->exit_event);
            goto out;
        }

        CloseHandleEx(&hStdInRead);
        CloseHandleEx(&hNul);

        /* Pass management password to OpenVPN process */
        c->manage.password[passwd_len - 1] = '\n';
        WriteFile(hStdInWrite, c->manage.password, passwd_len, &written, NULL);
        c->manage.password[passwd_len - 1] = '\0';

        c->hProcess = pi.hProcess; /* Will be closed in the event loop on exit */
        CloseHandle(pi.hThread);
    }

    retval = TRUE;

out:
    if (hStdInWrite && hStdInWrite != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hStdInWrite);
    }
    if (hStdInRead && hStdInRead != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hStdInRead);
    }
    if (hNul && hNul != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hNul);
    }
    return retval;
}


/* Close the status thread without disconnecting the tunnel.
 * Meant to be used only on persistent connections which can
 * stay connected without the GUI tending to it.
 */
void
DetachOpenVPN(connection_t *c)
{
    /* currently supported only for persistent connections */
    if (c->flags & FLAG_DAEMON_PERSISTENT)
    {
        c->auto_connect = false;
        PostMessage(c->hwndStatus, WM_OVPN_DETACH, 0, 0);
    }
}

void
StopOpenVPN(connection_t *c)
{
    PostMessage(c->hwndStatus, WM_OVPN_STOP, 0, 0);
}

/* force-kill as a last resort */
static BOOL
TerminateOpenVPN(connection_t *c)
{
    DWORD exit_code = 0;
    BOOL retval = TRUE;

    if (!c->hProcess)
    {
        return retval;
    }
    if (!GetExitCodeProcess(c->hProcess, &exit_code))
    {
        PrintDebug(L"In TerminateOpenVPN: failed to get process status: error = %lu",
                   GetLastError());
        return FALSE;
    }
    if (exit_code == STILL_ACTIVE)
    {
        retval = TerminateProcess(c->hProcess, 1);
        if (retval)
        {
            PrintDebug(L"Openvpn Process for config '%ls' terminated", c->config_name);
        }
        else
        {
            PrintDebug(L"Failed to terminate openvpn Process for config '%ls'", c->config_name);
        }
    }
    else
    {
        PrintDebug(L"In TerminateOpenVPN: Process is not active");
    }

    return retval;
}

void
RestartOpenVPN(connection_t *c)
{
    if (c->state == onhold)
    {
        ReleaseOpenVPN(c);
    }
    else if (c->hwndStatus)
    {
        PostMessage(c->hwndStatus, WM_OVPN_RESTART, 0, 0);
    }
    else /* Not started: treat this as a request to connect */
    {
        StartOpenVPN(c);
    }
}

void
SetStatusWinIcon(HWND hwndDlg, int iconId)
{
    HICON hIcon = LoadLocalizedSmallIcon(iconId);
    if (!hIcon)
    {
        return;
    }
    HICON hIconBig = LoadLocalizedIcon(ID_ICO_APP);
    if (!hIconBig)
    {
        hIconBig = hIcon;
    }
    SendMessage(hwndDlg, WM_SETICON, (WPARAM)ICON_SMALL, (LPARAM)hIcon);
    SendMessage(hwndDlg, WM_SETICON, (WPARAM)ICON_BIG, (LPARAM)hIconBig);
}


/*
 * Read one line from OpenVPN's stdout.
 */
static BOOL
ReadLineFromStdOut(HANDLE hStdOut, char *line, DWORD size)
{
    DWORD len, read;

    while (TRUE)
    {
        if (!PeekNamedPipe(hStdOut, line, size, &read, NULL, NULL))
        {
            if (GetLastError() != ERROR_BROKEN_PIPE)
            {
                ShowLocalizedMsg(IDS_ERR_READ_STDOUT_PIPE);
            }
            return FALSE;
        }

        char *pos = memchr(line, '\r', read);
        if (pos)
        {
            len = pos - line + 2;
            if (len > size)
            {
                return FALSE;
            }
            break;
        }

        /* Line doesn't fit into the buffer */
        if (read == size)
        {
            return FALSE;
        }

        Sleep(100); /* called when no UI is yet initialized */
    }

    if (!ReadFile(hStdOut, line, len, &read, NULL) || read != len)
    {
        if (GetLastError() != ERROR_BROKEN_PIPE)
        {
            ShowLocalizedMsg(IDS_ERR_READ_STDOUT_PIPE);
        }
        return FALSE;
    }

    line[read - 2] = '\0';
    return TRUE;
}


BOOL
CheckVersion()
{
    HANDLE hStdOutRead = NULL;
    HANDLE hStdOutWrite = NULL;
    BOOL retval = FALSE;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    TCHAR cmdline[] = _T("openvpn --version");
    char match_version[] = "OpenVPN 2.";
    TCHAR pwd[MAX_PATH];
    char line[1024];
    TCHAR *p;

    CLEAR(si);
    CLEAR(pi);

    /* Make handles inheritable and accessible by all */
    SECURITY_DESCRIPTOR sd;
    SECURITY_ATTRIBUTES sa = { .nLength = sizeof(sa),
                               .lpSecurityDescriptor = &sd,
                               .bInheritHandle = TRUE };
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
    {
        ShowLocalizedMsg(IDS_ERR_INIT_SEC_DESC);
        return FALSE;
    }
    if (!SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE))
    {
        ShowLocalizedMsg(IDS_ERR_SET_SEC_DESC_ACL);
        return FALSE;
    }

    /* Create the pipe for STDOUT with inheritable write end */
    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0))
    {
        ShowLocalizedMsg(IDS_ERR_CREATE_PIPE_IN_READ);
        return FALSE;
    }
    if (!SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0))
    {
        ShowLocalizedMsg(IDS_ERR_DUP_HANDLE_IN_WRITE);
        goto out;
    }

    /* Construct the process' working directory */
    _tcsncpy(pwd, o.exe_path, _countof(pwd));
    p = _tcsrchr(pwd, _T('\\'));
    if (p != NULL)
    {
        *p = _T('\0');
    }

    /* Fill in STARTUPINFO struct */
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hStdOutWrite;
    si.hStdError = hStdOutWrite;

    /* Start OpenVPN to check version */
    bool success =
        CreateProcess(o.exe_path, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, pwd, &si, &pi);
    if (!success)
    {
        ShowLocalizedMsg(IDS_ERR_CREATE_PROCESS, o.exe_path, cmdline, pwd);
        goto out;
    }

    CloseHandleEx(&hStdOutWrite);
    CloseHandleEx(&pi.hThread);
    CloseHandleEx(&pi.hProcess);

    if (ReadLineFromStdOut(hStdOutRead, line, sizeof(line)))
    {
#ifdef DEBUG
        PrintDebug(_T("VersionString: %hs"), line);
#endif
        /* OpenVPN version 2.x */
        char *p = strstr(line, match_version);
        if (p)
        {
            retval = TRUE;
            p = strtok(p + 8, " ");
            strncpy(o.ovpn_version, p, _countof(o.ovpn_version) - 1);
            o.ovpn_version[_countof(o.ovpn_version) - 1] = '\0';
        }
    }

out:
    CloseHandleEx(&hStdOutWrite);
    CloseHandleEx(&hStdOutRead);
    return retval;
}

/* Delete saved passwords and reset the checkboxes to default */
void
ResetSavePasswords(connection_t *c)
{
    if (ShowLocalizedMsgEx(
            MB_OKCANCEL, o.hWnd, TEXT(PACKAGE_NAME), IDS_NFO_DELETE_PASS, c->config_name)
        == IDCANCEL)
    {
        return;
    }
    DeleteSavedPasswords(c->config_name);
    c->flags &= ~(FLAG_SAVE_KEY_PASS | FLAG_SAVE_AUTH_PASS);
    SetMenuStatus(c, c->state);
}

/* keep this array in same order as corresponding resource ids -- IDS_NFO_OVPN_STATE_xxxxx */
static const char *daemon_states[] = {
    "INITIAL", "CONNECTING", "ASSIGN_IP",  "ADD_ROUTES", "CONNECTED",   "RECONNECTING", "EXITING",
    "WAIT",    "AUTH",       "GET_CONFIG", "RESOLVE",    "TCP_CONNECT", "AUTH_PENDING", NULL
};

/*
 * Given an OpenVPN state as reported by the management interface
 * return the correspnding resource id for translated string.
 * Returns IDS_NFO_OVPN_STATE_UNKNOWN if no match found.
 */
int
daemon_state_resid(const char *state)
{
    int i = 0;
    while (daemon_states[i] && strcmp(daemon_states[i], state))
    {
        i++;
    }
    return daemon_states[i] ? IDS_NFO_OVPN_STATE_INITIAL + i : IDS_NFO_OVPN_STATE_UNKNOWN;
}
