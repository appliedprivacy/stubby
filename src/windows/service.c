/*
 * Copyright (c) 2020, NLNet Labs, Sinodun
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the names of the copyright holders nor the
 *   names of its contributors may be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Verisign, Inc. BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <stdarg.h>
#include <stdio.h>

#include <winsock2.h>
#include <windows.h>
#include <tchar.h>
#include <strsafe.h>

#include "configfile.h"
#include "log.h"
#include "server.h"

#include "service.h"

#include "windowsservice.h"

static void winerr(const TCHAR* operation, DWORD err)
{
        char msg[512];

        if ( FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM,
                           NULL,
                           err,
                           0,
                           msg,
                           sizeof(msg),
                           NULL) == 0 )
                fprintf(stderr, "Error: %s: errno=%d\n", operation, err);
        else
                fprintf(stderr, "Error: %s: %s\n", operation, msg);
        exit(EXIT_FAILURE);
}

static void winlasterr(const TCHAR* operation)
{
        winerr(operation, GetLastError());
}

// #pragma comment(lib, "advapi32.lib")

#define SVCNAME TEXT("Stubby")

SERVICE_STATUS          gSvcStatus;
SERVICE_STATUS_HANDLE   gSvcStatusHandle;
HANDLE                  ghSvcStopEvent = NULL;
int                     dnssec_validation = 0;

VOID SvcInstall(void);
VOID SvcRemove(void);
VOID SvcService(void);
VOID SvcStart(int loglevel);
VOID SvcStop(void);
VOID WINAPI SvcCtrlHandler( DWORD );
VOID WINAPI SvcMain( DWORD, LPTSTR * );

VOID ReportSvcStatus( DWORD, DWORD, DWORD );
VOID SvcInit( DWORD, LPTSTR * );
VOID SvcReportEvent( LPTSTR );


void windows_service_command(const TCHAR* arg, int loglevel)
{
        if ( lstrcmpi(arg, TEXT("install")) == 0 )
                SvcInstall();
        else if ( lstrcmpi(arg, TEXT("remove")) == 0 )
                SvcRemove();
        else if ( lstrcmpi(arg, TEXT("service")) == 0 )
                SvcService();
        else if ( lstrcmpi(arg, TEXT("start")) == 0 )
                SvcStart(loglevel);
        else if ( lstrcmpi(arg, TEXT("stop")) == 0 )
                SvcStop();
        else
        {
                fprintf(stderr, "Unknown Windows option '%s'\n", arg);
                exit(EXIT_FAILURE);
        }

        exit(EXIT_SUCCESS);
}

void report_verror(getdns_loglevel_type level, const char *fmt, va_list ap)
{
        char buf[256];
        HANDLE hEventSource;
        LPCTSTR lpszStrings[2];
        WORD eventType;
        DWORD eventId;

        hEventSource = RegisterEventSource(NULL, SVCNAME);
        if ( hEventSource == NULL )
                return;

        switch (level)
        {
        case GETDNS_LOG_EMERG:
                eventType = EVENTLOG_ERROR_TYPE;
                eventId = SVC_EMERGENCY;
                break;

        case GETDNS_LOG_ALERT:
                eventType = EVENTLOG_ERROR_TYPE;
                eventId = SVC_ALERT;
                break;

        case GETDNS_LOG_CRIT:
                eventType = EVENTLOG_ERROR_TYPE;
                eventId = SVC_CRITICAL;
                break;

        case GETDNS_LOG_ERR:
                eventType = EVENTLOG_ERROR_TYPE;
                eventId = SVC_ERROR;
                break;

        case GETDNS_LOG_WARNING:
                eventType = EVENTLOG_WARNING_TYPE;
                eventId = SVC_WARNING;
                break;

        case GETDNS_LOG_NOTICE:
                eventType = EVENTLOG_WARNING_TYPE;
                eventId = SVC_NOTICE;
                break;

        case GETDNS_LOG_INFO:
                eventType = EVENTLOG_INFORMATION_TYPE;
                eventId = SVC_INFO;
                break;

        default:
                eventType = EVENTLOG_INFORMATION_TYPE;
                eventId = SVC_DEBUG;
                break;

        }

        vsnprintf(buf, sizeof(buf), fmt, ap);

        lpszStrings[0] = SVCNAME;
        lpszStrings[1] = buf;

        ReportEvent(hEventSource,        // event log handle
                    eventType,           // event type
                    0,                   // event category
                    eventId,             // event identifier
                    NULL,                // no security identifier
                    2,                   // size of lpszStrings array
                    0,                   // no binary data
                    lpszStrings,         // array of strings
                    NULL);               // no binary data

        DeregisterEventSource(hEventSource);
}

void report_vlog(void *userarg, uint64_t system,
                 getdns_loglevel_type level,
                 const char *fmt, va_list ap)
{
        (void) userarg;
        (void) system;
        report_verror(level, fmt, ap);
}


VOID report_winerr(LPTSTR operation)
{
        char msg[512];
        DWORD err = GetLastError();

        if ( FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM,
                           NULL,
                           err,
                           0,
                           msg,
                           sizeof(msg),
                           NULL) == 0 )
                stubby_error("Error: %s: errno=%d\n", operation, err);
        else
                stubby_error("Error: %s: %s\n", operation, msg);
}

VOID report_getdnserr(LPTSTR operation)
{
        stubby_error("%s: %s", operation, stubby_getdns_strerror());
}

VOID SvcService()
{
        SERVICE_TABLE_ENTRY DispatchTable[] = {
                { SVCNAME, (LPSERVICE_MAIN_FUNCTION) SvcMain },
                { NULL, NULL }
        };

        // This call returns when the service has stopped.
        // The process should simply terminate when the call returns.
        if ( !StartServiceCtrlDispatcher(DispatchTable) )
        {
                report_winerr("StartServiceCtrlDispatcher");
        }
}

static void createRegistryEntries(const TCHAR* path)
{
        TCHAR buf[512];
        HKEY hkey;
        DWORD t;
        LSTATUS status;

        snprintf(buf, sizeof(buf), "SYSTEM\\CurrentControlSet\\Services"
                 "\\EventLog\\Application\\%s", SVCNAME);
        status = RegCreateKeyEx(
                HKEY_LOCAL_MACHINE,
                buf,       // Key
                0,         // Reserved
                NULL,      // Class
                REG_OPTION_NON_VOLATILE, // Info on file
                KEY_WRITE, // Access rights
                NULL,      // Security descriptor
                &hkey,     // Result
                NULL       // Don't care if it exists
                );
        if ( status != ERROR_SUCCESS )
                winerr("Create registry key", status);

        status = RegSetValueEx(
                hkey,                      // Key handle
                "EventMessageFile",        // Value name
                0,                         // Reserved
                REG_EXPAND_SZ,             // It's a string
                (const BYTE*) path,        // with this value
                strlen(path) + 1           // and this long
                );
        if ( status != ERROR_SUCCESS )
        {
                RegCloseKey(hkey);
                winerr("Set EventMessageFile", status);
        }

        status = RegSetValueEx(
                hkey,                      // Key handle
                "CategoryMessageFile",     // Value name
                0,                         // Reserved
                REG_EXPAND_SZ,             // It's a string
                (const BYTE*) path,        // with this value
                strlen(path) + 1           // and this long
                );
        if ( status != ERROR_SUCCESS )
        {
                RegCloseKey(hkey);
                winerr("Set CategoryMessageFile", status);
        }

        /* event types */
        t = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
        status = RegSetValueEx(
                hkey,                      // Key handle
                "TypesSupported",          // Value name
                0,                         // Reserved
                REG_DWORD,                 // It's a DWORD
                (const BYTE*) &t,          // with this value
                sizeof(t)                  // and this long
                );
        if ( status != ERROR_SUCCESS )
        {
                RegCloseKey(hkey);
                winerr("Set TypesSupported", status);
        }

        t = 1;
        status = RegSetValueEx(
                hkey,                      // Key handle
                "CategoryCount",           // Value name
                0,                         // Reserved
                REG_DWORD,                 // It's a DWORD
                (const BYTE*) &t,          // with this value
                sizeof(t)                  // and this long
                );
        if ( status != ERROR_SUCCESS )
        {
                RegCloseKey(hkey);
                winerr("Set TypesSupported", status);
        }
        RegCloseKey(hkey);
}

static void deleteRegistryEntries(void)
{
        HKEY hkey;
        DWORD t;
        LSTATUS status;

        status = RegCreateKeyEx(
                HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Services"
                "\\EventLog\\Application",
                0,         // Reserved
                NULL,      // Class
                REG_OPTION_NON_VOLATILE, // Info on file
                DELETE,    // Access rights
                NULL,      // Security descriptor
                &hkey,     // Result
                NULL       // Don't care if it exists
                );
        if ( status != ERROR_SUCCESS )
                winerr("Create registry key", status);

        status = RegDeleteKey(hkey, SVCNAME);
        if ( status != ERROR_SUCCESS )
        {
                RegCloseKey(hkey);
                winerr("Delete registry key", status);
        }
        RegCloseKey(hkey);
}

VOID SvcInstall()
{
        SC_HANDLE schSCManager;
        SC_HANDLE schService;
        TCHAR modpath[MAX_PATH];
        const TCHAR ARG[] = "-w service";
        TCHAR cmd[MAX_PATH + 3 + sizeof(ARG)];
        SERVICE_DESCRIPTION description;

        if( !GetModuleFileName(NULL, modpath, MAX_PATH) )
                winlasterr("GetModuleFileName");
        snprintf(cmd, sizeof(cmd), "\"%s\" %s", modpath, ARG);

        createRegistryEntries(modpath);

        schSCManager = OpenSCManager(
                NULL,                    // local computer
                NULL,                    // ServicesActive database
                SC_MANAGER_ALL_ACCESS);  // full access rights

        if (NULL == schSCManager)
                winlasterr("Open service manager");

        schService = CreateService(
                schSCManager,              // SCM database
                SVCNAME,                   // name of service
                "Stubby Secure DNS Proxy", // service name to display
                SERVICE_ALL_ACCESS,        // desired access
                SERVICE_WIN32_OWN_PROCESS, // service type
                SERVICE_DEMAND_START,      // start type
                SERVICE_ERROR_NORMAL,      // error control type
                cmd,                       // path to service's binary
                NULL,                      // no load ordering group
                NULL,                      // no tag identifier
                NULL,                      // no dependencies
                NULL,                      // LocalSystem account
                NULL);                     // no password

        if (schService == NULL)
        {
                CloseServiceHandle(schSCManager);
                winlasterr("Create service");
        }

        description.lpDescription = TEXT("Enable performing DNS name lookups over secure channels.");
        ChangeServiceConfig2(schService, SERVICE_CONFIG_DESCRIPTION,
                             (LPVOID) &description);

        printf("Service installed successfully\n");

        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
}

VOID SvcRemove()
{
        SC_HANDLE schSCManager;
        SC_HANDLE schService;

        schSCManager = OpenSCManager(
                NULL,                    // local computer
                NULL,                    // ServicesActive database
                SC_MANAGER_ALL_ACCESS);  // full access rights

        if (NULL == schSCManager)
                winlasterr("Open service manager");

        schService = OpenService(
                schSCManager,              // SCM database
                SVCNAME,                   // name of service
                DELETE);                   // intention

        if (schService == NULL)
        {
                CloseServiceHandle(schSCManager);
                winlasterr("Open service");
        }

        if ( DeleteService(schService) == 0 )
        {
                CloseServiceHandle(schService);
                CloseServiceHandle(schSCManager);
                winlasterr("Delete service");
        }

        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        deleteRegistryEntries();

        printf("Service removed successfully\n");
}

VOID SvcStart(int loglevel)
{
        SC_HANDLE schSCManager;
        SC_HANDLE schService;

        schSCManager = OpenSCManager(
                NULL,                    // local computer
                NULL,                    // ServicesActive database
                SC_MANAGER_ALL_ACCESS);  // full access rights

        if (NULL == schSCManager)
                winlasterr("Open service manager");

        schService = OpenService(
                schSCManager,              // SCM database
                SVCNAME,                   // name of service
                SERVICE_START);            // intention

        if (schService == NULL)
        {
                CloseServiceHandle(schSCManager);
                winlasterr("Open service");
        }

        TCHAR loglevelstr[2];
        loglevelstr[0] = '0' + loglevel;
        loglevelstr[1] = '\0';

        LPCTSTR args[2] = {
                SVCNAME,
                loglevelstr
        };

        if ( StartService(
                     schService,        // Service
                     2,                 // number of args
                     args               // args
                     ) == 0 )
        {
                CloseServiceHandle(schService);
                CloseServiceHandle(schSCManager);
                winlasterr("Start service");
        }

        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);

        printf("Service started successfully\n");
}

VOID SvcStop()
{
        SC_HANDLE schSCManager;
        SC_HANDLE schService;

        schSCManager = OpenSCManager(
                NULL,                    // local computer
                NULL,                    // ServicesActive database
                SC_MANAGER_ALL_ACCESS);  // full access rights

        if (NULL == schSCManager)
                winlasterr("Open service manager");

        schService = OpenService(
                schSCManager,              // SCM database
                SVCNAME,                   // name of service
                SERVICE_STOP);             // intention

        if (schService == NULL)
        {
                CloseServiceHandle(schSCManager);
                winlasterr("Open service");
        }

        SERVICE_STATUS st;

        if ( ControlService(
                     schService,                // service
                     SERVICE_CONTROL_STOP,      // action
                     &st                        // result
                     ) == 0 )
        {
                CloseServiceHandle(schService);
                CloseServiceHandle(schSCManager);
                winlasterr("Stop service");
        }

        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);

        printf("Service stopped successfully\n");
}

VOID WINAPI SvcMain(DWORD dwArgc, LPTSTR *lpszArgv)
{
        stubby_set_log_funcs(report_verror, report_vlog);

        gSvcStatusHandle = RegisterServiceCtrlHandler(
                SVCNAME,
                SvcCtrlHandler);

        if( !gSvcStatusHandle )
        {
                report_winerr("RegisterServiceCtrlHandler");
                return;
        }

        gSvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        gSvcStatus.dwServiceSpecificExitCode = 0;
        ReportSvcStatus(SERVICE_START_PENDING, 0, 3000);

        SvcInit(dwArgc, lpszArgv);
}

VOID SvcInit( DWORD dwArgc, LPTSTR *lpszArgv)
{
        getdns_context *context = NULL;
        getdns_return_t r;
        int more = 1;
        getdns_eventloop *eventloop;
        int validate_dnssec;

        ghSvcStopEvent = CreateEvent(
                NULL,    // default security attributes
                TRUE,    // manual reset event
                FALSE,   // not signaled
                NULL);   // no name

        if ( ghSvcStopEvent == NULL)
        {
                ReportSvcStatus(SERVICE_STOPPED, 1, 0);
                return;
        }

        ReportSvcStatus(SERVICE_START_PENDING, 0, 1000);
        if ( ( r = getdns_context_create(&context, 1) ) ) {
                stubby_error("Create context failed: %s", stubby_getdns_strerror(r));
                ReportSvcStatus(SERVICE_STOPPED, 1, 0);
                CloseHandle(ghSvcStopEvent);
                ghSvcStopEvent = NULL;
                return;
        }

        if ( dwArgc > 1 )
                stubby_set_getdns_logging(context, lpszArgv[1][0] - '0');

        init_config(context);
        ReportSvcStatus(SERVICE_START_PENDING, 0, 1010);
        if ( !read_config(context, NULL, &validate_dnssec) ) {
                ReportSvcStatus(SERVICE_STOPPED, 1, 0);
                goto tidy_and_exit;
        }
        ReportSvcStatus(SERVICE_START_PENDING, 0, 1020);
        if ( !server_listen(context, dnssec_validation) ) {
                ReportSvcStatus(SERVICE_STOPPED, 1, 0);
                goto tidy_and_exit;
        }

        ReportSvcStatus(SERVICE_START_PENDING, 0, 1030);
        if ( getdns_context_get_eventloop(context, &eventloop) ) {
                report_getdnserr("Get event loop");
                ReportSvcStatus(SERVICE_STOPPED, 1, 0);
                goto tidy_and_exit;
        }

        ReportSvcStatus(SERVICE_RUNNING, 0, 0);

        for(;;)
        {
                switch ( WaitForSingleObject(ghSvcStopEvent, 0) )
                {
                case WAIT_OBJECT_0:
                        break;

                case WAIT_FAILED:
                        more = 0;
                        report_winerr("WaitForSingleObject");
                        break;

                default:
                        more = 0;
                        stubby_debug("Stop object signalled");
                        break;
                }

                if ( !more )
                        break;

                eventloop->vmt->run_once(eventloop, 1);
        }
        ReportSvcStatus(SERVICE_STOPPED, 0, 0);

tidy_and_exit:
        getdns_context_destroy(context);
        delete_config();
        CloseHandle(ghSvcStopEvent);
        ghSvcStopEvent = NULL;
}

VOID ReportSvcStatus( DWORD dwCurrentState,
                      DWORD dwWin32ExitCode,
                      DWORD dwWaitHint)
{
        static DWORD dwCheckPoint = 1;

        gSvcStatus.dwCurrentState = dwCurrentState;
        gSvcStatus.dwWin32ExitCode = dwWin32ExitCode;
        gSvcStatus.dwWaitHint = dwWaitHint;

        if (dwCurrentState == SERVICE_START_PENDING)
                gSvcStatus.dwControlsAccepted = 0;
        else gSvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

        if ( (dwCurrentState == SERVICE_RUNNING) ||
             (dwCurrentState == SERVICE_STOPPED) )
                gSvcStatus.dwCheckPoint = 0;
        else gSvcStatus.dwCheckPoint = dwCheckPoint++;

        SetServiceStatus(gSvcStatusHandle, &gSvcStatus);
}

VOID WINAPI SvcCtrlHandler(DWORD dwCtrl)
{
        switch(dwCtrl)
        {
        case SERVICE_CONTROL_STOP:
                ReportSvcStatus(SERVICE_STOP_PENDING, 0, 0);
                SetEvent(ghSvcStopEvent);
                break;

        case SERVICE_CONTROL_INTERROGATE:
                break;

        default:
                break;
        }
}
