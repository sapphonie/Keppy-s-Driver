﻿/*
Keppy's Driver, a fork of BASSMIDI Driver
*/

#define STRICT

#if __DMC__
unsigned long _beginthreadex(void *security, unsigned stack_size,
	unsigned(__stdcall *start_address)(void *), void *arglist,
	unsigned initflag, unsigned *thrdaddr);
void _endthreadex(unsigned retval);
#endif

#define _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES 1
#define _CRT_SECURE_NO_WARNINGS 1
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <process.h>
#include <Shlwapi.h>
#include <mmddk.h>
#include <mmsystem.h>
#include <tchar.h>
#include <limits>
#include "stdafx.h"
#include <vector>
#include <signal.h>
#include <list>
#include <sstream>
#include <string>
#include <shlobj.h>
#include <fstream>
#include <iostream>
#include <cctype>
#include <comdef.h>

#define BASSDEF(f) (WINAPI *f)	// define the BASS/BASSMIDI/BASSENC/BASSWASAPI functions as pointers
#define BASSMIDIDEF(f) (WINAPI *f)	
#define BASSENCDEF(f) (WINAPI *f)	
#define BASSWASAPIDEF(f) (WINAPI *f)
#define LOADBASSFUNCTION(f) *((void**)&f)=GetProcAddress(bass,#f)
#define LOADBASSMIDIFUNCTION(f) *((void**)&f)=GetProcAddress(bassmidi,#f)
#define LOADBASSENCFUNCTION(f) *((void**)&f)=GetProcAddress(bassenc,#f)
#define LOADBASSWASAPIFUNCTION(f) *((void**)&f)=GetProcAddress(basswasapi,#f)

#include <bass.h>
#include <bassmidi.h>
#include <bassenc.h>
#include <basswasapi.h>

#include "sound_out.h"

#define MAX_DRIVERS 4
#define MAX_CLIENTS 8 // Per driver

#define SPFSTD 108

#ifndef _LOADRESTRICTIONS_OFF
#define _LOADRESTRICTIONS_ON
#endif

struct Driver_Client {
	int allocated;
	DWORD_PTR instance;
	DWORD flags;
	DWORD_PTR callback;
};

//Note: drivers[0] is not used (See OnDriverOpen).
struct Driver {
	int open;
	int clientCount;
	HDRVR hdrvr;
	struct Driver_Client clients[MAX_CLIENTS];
} drivers[MAX_DRIVERS + 1];

static int driverCount = 0;

static volatile int OpenCount = 0;
static volatile int modm_closed = 1;

static CRITICAL_SECTION mim_section;
static volatile int stop_thread = 0;
static volatile int reset_synth[4] = { 0, 0, 0, 0 };
static HANDLE hCalcThread = NULL;
static DWORD processPriority;
static HANDLE load_sfevent = NULL;

static unsigned int font_count[4] = { 0, 0, 0, 0 };
static HSOUNDFONT * hFonts[4] = { NULL, NULL, NULL, NULL };
static HSTREAM hStream[4] = { 0, 0, 0, 0 };

static BOOL com_initialized = FALSE;
static BOOL sound_out_float = FALSE;
static float sound_out_volume_float = 1.0;
static int sound_out_volume_int = 0x1000;
static int consent = 0; //This has been added to avoid fatal exceptions with the consent.exe program on Windows Vista and newer (DO NOT REMOVE)
static int frames = 0; //default
static int realtimeset = 0; //Real-time settings
static int midivoices = 0; //Max voices INT
static int maxcpu = 0; //CPU usage INT
static int softwaremode = 1; //Software rendering
static int preload = 0; //Soundfont preloading
static int availableports = 4; //How many ports are available
static int reverb = 0; //Reverb FX
static int chorus = 0; //Chorus FX
static int encmode = 0; //Reverb FX
static int transpose = 0; //Transpose FX
static int frequency = 0; //Audio frequency
static int sinc = 0; //Sinc
static int sysresetignore = 0; //Ignore sysex messages
static int debug = 0; //Debug mode
static int tracks = 0; //Tracks limit
static int volume = 0; //Volume limit
static int nofloat = 1; //Enable or disable the float engine
static int nofx = 0; //Enable or disable FXs
static int noteoff1 = 0; //Note cut INT
static int wasapimode = 0; //WASAPI MODE

static int decoded;
static int decoded2;
static int decoded3;
static int decoded4;

static sound_out * sound_driver = NULL;

static HINSTANCE bass = 0;				// bass handle
static HINSTANCE bassmidi = 0;			// bassmidi handle
static HINSTANCE bassenc = 0;			// bassenc handle
static HINSTANCE basswasapi = 0;		// basswasapi handle
//TODO: Can be done with: HMODULE GetDriverModuleHandle(HDRVR hdrvr);  (once DRV_OPEN has been called)
static HINSTANCE hinst = NULL;             //main DLL handle

static void DoStopClient();

class message_window
{
	HWND m_hWnd;
	ATOM class_atom;

public:
	message_window() {
		static const TCHAR * class_name = _T("keppymididrv message window");
		WNDCLASSEX cls = { 0 };
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = DefWindowProc;
		cls.hInstance = hinst;
		cls.lpszClassName = class_name;
		class_atom = RegisterClassEx(&cls);
		if (class_atom) {
			m_hWnd = CreateWindowEx(0, (LPCTSTR)class_atom, _T("keppymididrv"), 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hinst, NULL);
		}
		else {
			m_hWnd = NULL;
		}
	}

	~message_window()
	{
		if (IsWindow(m_hWnd)) DestroyWindow(m_hWnd);
		if (class_atom) UnregisterClass((LPCTSTR)class_atom, hinst);
	}

	HWND get_hwnd() const { return m_hWnd; }
};

message_window * g_msgwnd = NULL;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved){

	if (fdwReason == DLL_PROCESS_ATTACH){
		hinst = hinstDLL;
		DisableThreadLibraryCalls(hinstDLL);
		g_msgwnd = new message_window;
	}
	else if (fdwReason == DLL_PROCESS_DETACH){
		;
		DoStopClient();
		delete g_msgwnd;
	}
	return TRUE;
}

std::vector<HSOUNDFONT> _soundFonts[4];
std::vector<BASS_MIDI_FONTEX> presetList[4];

static void FreeFonts(UINT uDeviceID)
{
	unsigned i;
	if (_soundFonts[uDeviceID].size())
	{
		for (auto it = _soundFonts[uDeviceID].begin(); it != _soundFonts[uDeviceID].end(); ++it)
		{
			BASS_MIDI_FontFree(*it);
		}
		_soundFonts[uDeviceID].resize(0);
		presetList[uDeviceID].resize(0);
	}
}

static bool load_font_item(unsigned uDeviceID, const TCHAR * in_path)
{
	const DWORD bass_flags =
#ifdef UNICODE
		BASS_UNICODE
#else
		0
#endif
		;
	const TCHAR * ext = _T("");
	const TCHAR * dot = _tcsrchr(in_path, _T('.'));
	if (dot != 0) ext = dot + 1;
	if (!_tcsicmp(ext, _T("sf2"))
		|| !_tcsicmp(ext, _T("sf2pack"))
		|| !_tcsicmp(ext, _T("sfz"))
		)
	{
		HSOUNDFONT font = BASS_MIDI_FontInit(in_path, bass_flags);
		if (!font)
		{
			return false;
		}
		_soundFonts[uDeviceID].push_back(font);
		BASS_MIDI_FONTEX fex = { font, -1, -1, -1, 0, 0 };
		presetList[uDeviceID].push_back(fex);
		return true;
	}
	else if (!_tcsicmp(ext, _T("sflist")))
	{
		FILE * fl = _tfopen(in_path, _T("r, ccs=UTF-8"));
		if (fl)
		{
			TCHAR path[32768], temp[32768];
			TCHAR name[32768];
			TCHAR *nameptr;
			const TCHAR * slash = _tcsrchr(in_path, _T('\\'));
			if (slash != 0) _tcsncpy(path, in_path, slash - in_path + 1);
			while (!feof(fl))
			{
				std::vector<BASS_MIDI_FONTEX> presets;

				if (!_fgetts(name, 32767, fl)) break;
				name[32767] = 0;
				TCHAR * cr = _tcschr(name, _T('\n'));
				if (cr) *cr = 0;
				cr = _tcschr(name, _T('\r'));
				if (cr) *cr = 0;
				cr = _tcschr(name, '|');
				if (cr)
				{
					bool valid = true;
					TCHAR *endchr;
					nameptr = cr + 1;
					*cr = 0;
					cr = name;
					while (*cr && valid)
					{
						switch (*cr++)
						{
						case 'p':
						{
							// patch override - "p[db#,]dp#=[sb#,]sp#" ex. "p0,5=0,1"
							// may be used once per preset group
							long dbank = 0;
							long dpreset = _tcstol(cr, &endchr, 10);
							if (endchr == cr)
							{
								valid = false;
								break;
							}
							if (*endchr == ',')
							{
								dbank = dpreset;
								cr = endchr + 1;
								dpreset = _tcstol(cr, &endchr, 10);
								if (endchr == cr)
								{
									valid = false;
									break;
								}
							}
							if (*endchr != '=')
							{
								valid = false;
								break;
							}
							cr = endchr + 1;
							long sbank = -1;
							long spreset = _tcstol(cr, &endchr, 10);
							if (endchr == cr)
							{
								valid = false;
								break;
							}
							if (*endchr == ',')
							{
								sbank = spreset;
								cr = endchr + 1;
								spreset = _tcstol(cr, &endchr, 10);
								if (endchr == cr)
								{
									valid = false;
									break;
								}
							}
							if (*endchr && *endchr != ';' && *endchr != '&')
							{
								valid = false;
								break;
							}
							cr = endchr;
							BASS_MIDI_FONTEX fex = { 0, (int)spreset, (int)sbank, (int)dpreset, (int)dbank, 0 };
							presets.push_back(fex);
						}
						break;

						case '&':
						{
						}
						break;

						case ';':
							// separates preset items
							break;

						default:
							// invalid command character
							valid = false;
							break;
						}
					}
					if (!valid)
					{
						presets.clear();
						BASS_MIDI_FONTEX fex = { 0, -1, -1, -1, 0, 0 };
						presets.push_back(fex);
					}
				}
				else
				{
					BASS_MIDI_FONTEX fex = { 0, -1, -1, -1, 0, 0 };
					presets.push_back(fex);
					nameptr = name;
				}
				if ((isalpha(nameptr[0]) && nameptr[1] == _T(':')) || nameptr[0] == '\\')
				{
					_tcscpy(temp, nameptr);
				}
				else
				{
					_tcscat(temp, nameptr);
				}
				HSOUNDFONT font = BASS_MIDI_FontInit(temp, bass_flags);
				if (!font)
				{
					fclose(fl);
					return false;
				}
				for (auto it = presets.begin(); it != presets.end(); ++it)
				{
					if (preload)
						BASS_MIDI_FontLoad(font, it->spreset, it->sbank);
					it->font = font;
					presetList[uDeviceID].push_back(*it);
				}
				_soundFonts[uDeviceID].push_back(font);
			}
			fclose(fl);
			return true;
		}
	}
	return false;
}

void LoadFonts(UINT uDeviceID, const TCHAR * name)
{
	FreeFonts(uDeviceID);

	if (name && *name)
	{
		const TCHAR * ext = _tcsrchr(name, _T('.'));
		if (ext) ext++;
		if (!_tcsicmp(ext, _T("sf2")) || !_tcsicmp(ext, _T("sf2pack")) || !_tcsicmp(ext, _T("sfz")))
		{
			if (!load_font_item(uDeviceID, name))
			{
				FreeFonts(uDeviceID);
				return;
			}
		}
		else if (!_tcsicmp(ext, _T("sflist")))
		{
			if (!load_font_item(uDeviceID, name))
			{
				FreeFonts(uDeviceID);
				return;
			}
		}

		std::vector< BASS_MIDI_FONTEX > fonts;
		for (unsigned long i = 0, j = presetList[uDeviceID].size(); i < j; ++i)
		{
			fonts.push_back(presetList[uDeviceID][j - i - 1]);
		}
		BASS_MIDI_StreamSetFonts(hStream[uDeviceID], &fonts[0], (unsigned int)fonts.size() | BASS_MIDI_FONT_EX);
	}
}

LRESULT DoDriverLoad() {
	//The DRV_LOAD message is always the first message that a device driver receives. 
	//Notifies the driver that it has been loaded. The driver should make sure that any hardware and supporting drivers it needs to function properly are present.
	memset(drivers, 0, sizeof(drivers));
	driverCount = 0;
	return DRV_OK;

}

LRESULT DoDriverOpen(HDRVR hdrvr, LPCWSTR driverName, LONG lParam) {

	/*
	Remarks

	If the driver returns a nonzero value, the system uses that value as the driver identifier (the dwDriverId parameter)
	in messages it subsequently sends to the driver instance. The driver can return any type of value as the identifier.
	For example, some drivers return memory addresses that point to instance-specific information. Using this method of
	specifying identifiers for a driver instance gives the drivers ready access to the information while they are processing messages.
	*/

	/*
	When the driver's DriverProc function receives a
	DRV_OPEN message, it should:
	1. Allocate memory space for a structure instance.
	2. Add the structure instance to the linked list.
	3. Store instance data in the new list entry.
	4. Specify the entry's number or address as the return value for the DriverProc function.
	Subsequent calls to DriverProc will include the list entry's identifier as its dwDriverID
	argument
	*/
	int driverNum;
	if (driverCount == MAX_DRIVERS) {
		return 0;
	}
	else {
		for (driverNum = 1; driverNum < MAX_DRIVERS; driverNum++) {
			if (!drivers[driverNum].open) {
				break;
			}
		}
		if (driverNum == MAX_DRIVERS) {
			return 0;
		}
	}
	drivers[driverNum].open = 1;
	drivers[driverNum].clientCount = 0;
	drivers[driverNum].hdrvr = hdrvr;
	driverCount++;
	return driverNum;
}

LRESULT DoDriverClose(DWORD_PTR dwDriverId, HDRVR hdrvr, LONG lParam1, LONG lParam2) {
	int i;
	for (i = 0; i < MAX_DRIVERS; i++) {
		if (drivers[i].open && drivers[i].hdrvr == hdrvr) {
			drivers[i].open = 0;
			--driverCount;
			return DRV_OK;
		}
	}
	return DRV_CANCEL;
}

LRESULT DoDriverConfigure(DWORD_PTR dwDriverId, HDRVR hdrvr, HWND parent, DRVCONFIGINFO* configInfo) {
	return DRV_CANCEL;
}

/* INFO Installable Driver Reference: http://msdn.microsoft.com/en-us/library/ms709328%28v=vs.85%29.aspx */
/* The original header is LONG DriverProc(DWORD dwDriverId, HDRVR hdrvr, UINT msg, LONG lParam1, LONG lParam2);
but that does not support 64bit. See declaration of DefDriverProc to see where the values come from.
*/
STDAPI_(LRESULT) DriverProc(DWORD_PTR dwDriverId, HDRVR hdrvr, UINT uMsg, LPARAM lParam1, LPARAM lParam2)
{
	switch (uMsg) {
		/* Seems this is only for kernel mode drivers
		case DRV_INSTALL:
		return DoDriverInstall(dwDriverId, hdrvr, static_cast<DRVCONFIGINFO*>(lParam2));
		case DRV_REMOVE:
		DoDriverRemove(dwDriverId, hdrvr);
		return DRV_OK;
		*/
	case DRV_QUERYCONFIGURE:
		//TODO: Until it doesn't have a configuration window, it should return 0.
		return DRV_CANCEL;
	case DRV_CONFIGURE:
		return DoDriverConfigure(dwDriverId, hdrvr, reinterpret_cast<HWND>(lParam1), reinterpret_cast<DRVCONFIGINFO*>(lParam2));

		/* TODO: Study this. It has implications:
		Calling OpenDriver, described in the Win32 SDK. This function calls SendDriverMessage to
		send DRV_LOAD and DRV_ENABLE messages only if the driver has not been previously loaded,
		and then to send DRV_OPEN.
		� Calling CloseDriver, described in the Win32 SDK. This function calls SendDriverMessage to
		send DRV_CLOSE and, if there are no other open instances of the driver, to also send
		DRV_DISABLE and DRV_FREE.
		*/
	case DRV_LOAD:
		return DoDriverLoad();
	case DRV_FREE:
		//The DRV_FREE message is always the last message that a device driver receives. 
		//Notifies the driver that it is being removed from memory. The driver should free any memory and other system resources that it has allocated.
		return DRV_OK;
	case DRV_OPEN:
		return DoDriverOpen(hdrvr, reinterpret_cast<LPCWSTR>(lParam1), static_cast<LONG>(lParam2));
	case DRV_CLOSE:
		return DoDriverClose(dwDriverId, hdrvr, static_cast<LONG>(lParam1), static_cast<LONG>(lParam2));
	default:
		return DefDriverProc(dwDriverId, hdrvr, uMsg, lParam1, lParam2);
	}
}

HRESULT modGetCaps(UINT uDeviceID, MIDIOUTCAPS* capsPtr, DWORD capsSize) {
	MIDIOUTCAPSA * myCapsA;
	MIDIOUTCAPSW * myCapsW;
	MIDIOUTCAPS2A * myCaps2A;
	MIDIOUTCAPS2W * myCaps2W;
	CHAR synthName[] = "Keppy's";
	WCHAR synthNameW[] = L"Keppy's";

	CHAR synthPortA[] = " Driver\0";
	WCHAR synthPortAW[] = L" Driver\0";

	CHAR synthPortB[] = " (X)\0";
	WCHAR synthPortBW[] = L" (X)\0";

	CHAR synthPortC[] = " (XX)\0";
	WCHAR synthPortCW[] = L" (XX)\0";

	CHAR synthPortD[] = " (XXX)\0";
	WCHAR synthPortDW[] = L" (XXX)\0";

	PCHAR synthPorts[4] = { synthPortA, synthPortB, synthPortC, synthPortD };
	WCHAR * synthPortsW[4] = { synthPortAW, synthPortBW, synthPortCW, synthPortDW };

	switch (capsSize) {
	case (sizeof(MIDIOUTCAPSA)) :
		myCapsA = (MIDIOUTCAPSA *)capsPtr;
		myCapsA->wMid = 0xffff;
		myCapsA->wPid = 0xffff;
		memcpy(myCapsA->szPname, synthName, strlen(synthName));
		memcpy(myCapsA->szPname + strlen(synthName), synthPorts[uDeviceID], strlen(synthPorts[uDeviceID]) + 1);
		myCapsA->wTechnology = MOD_SYNTH;
		myCapsA->vDriverVersion = 0x0090;
		myCapsA->wVoices = 100000;
		myCapsA->wNotes = 0;
		myCapsA->wChannelMask = 0xffff;
		myCapsA->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	case (sizeof(MIDIOUTCAPSW)) :
		myCapsW = (MIDIOUTCAPSW *)capsPtr;
		myCapsW->wMid = 0xffff;
		myCapsW->wPid = 0xffff;
		memcpy(myCapsW->szPname, synthNameW, wcslen(synthNameW) * sizeof(wchar_t));
		memcpy(myCapsW->szPname + wcslen(synthNameW), synthPortsW[uDeviceID], (wcslen(synthPortsW[uDeviceID]) + 1) * sizeof(wchar_t));
		myCapsW->wTechnology = MOD_SYNTH;
		myCapsW->vDriverVersion = 0x0090;
		myCapsW->wVoices = 100000;
		myCapsW->wNotes = 0;
		myCapsW->wChannelMask = 0xffff;
		myCapsW->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	case (sizeof(MIDIOUTCAPS2A)) :
		myCaps2A = (MIDIOUTCAPS2A *)capsPtr;
		myCaps2A->wMid = 0xffff;
		myCaps2A->wPid = 0xffff;
		memcpy(myCaps2A->szPname, synthName, strlen(synthName));
		memcpy(myCaps2A->szPname + strlen(synthName), synthPorts[uDeviceID], strlen(synthPorts[uDeviceID]) + 1);
		myCaps2A->wTechnology = MOD_SYNTH;
		myCaps2A->vDriverVersion = 0x0090;
		myCaps2A->wVoices = 100000;
		myCaps2A->wNotes = 0;
		myCaps2A->wChannelMask = 0xffff;
		myCaps2A->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	case (sizeof(MIDIOUTCAPS2W)) :
		myCaps2W = (MIDIOUTCAPS2W *)capsPtr;
		myCaps2W->wMid = 0xffff;
		myCaps2W->wPid = 0xffff;
		memcpy(myCaps2W->szPname, synthNameW, wcslen(synthNameW) * sizeof(wchar_t));
		memcpy(myCaps2W->szPname + wcslen(synthNameW), synthPortsW[uDeviceID], (wcslen(synthPortsW[uDeviceID]) + 1) * sizeof(wchar_t));
		myCaps2W->wTechnology = MOD_SYNTH;
		myCaps2W->vDriverVersion = 0x0090;
		myCaps2W->wVoices = 100000;
		myCaps2W->wNotes = 0;
		myCaps2W->wChannelMask = 0xffff;
		myCaps2W->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	default:
		return MMSYSERR_ERROR;
	}
}


struct evbuf_t{
	UINT uDeviceID;
	UINT   uMsg;
	DWORD_PTR	dwParam1;
	DWORD_PTR	dwParam2;
	int exlen;
	unsigned char *sysexbuffer;
};

#define EVBUFF_SIZE 524280
static struct evbuf_t evbuf[EVBUFF_SIZE];
static UINT  evbwpoint = 0;
static UINT  evbrpoint = 0;
static volatile LONG evbcount = 0;
static UINT evbsysexpoint;

int bmsyn_buf_check(void){
	int retval;
	EnterCriticalSection(&mim_section);
	retval = evbcount;
	LeaveCriticalSection(&mim_section);
	return retval;
}

bool compare_nocase(const std::string& first, const std::string& second)
{
	unsigned int i = 0;
	while ((i<first.length()) && (i<second.length()))
	{
		if (tolower(first[i])<tolower(second[i])) return true;
		else if (tolower(first[i])>tolower(second[i])) return false;
		++i;
	}
	return (first.length() < second.length());
}

int bmsyn_play_some_data(void){
	UINT uDeviceID;
	UINT uMsg;
	DWORD_PTR	dwParam1;
	DWORD_PTR   dwParam2;

	UINT evbpoint;
	int exlen;
	unsigned char *sysexbuffer;
	int played;

	played = 0;
	if (!bmsyn_buf_check()){
		played = ~0;
		return played;
	}
	do{
		EnterCriticalSection(&mim_section);
		evbpoint = evbrpoint;
		if (++evbrpoint >= EVBUFF_SIZE)
			evbrpoint -= EVBUFF_SIZE;

		uDeviceID = evbuf[evbpoint].uDeviceID;
		uMsg = evbuf[evbpoint].uMsg;
		dwParam1 = evbuf[evbpoint].dwParam1;
		dwParam2 = evbuf[evbpoint].dwParam2;
		exlen = evbuf[evbpoint].exlen;
		sysexbuffer = evbuf[evbpoint].sysexbuffer;

		LeaveCriticalSection(&mim_section);
		switch (uMsg) {
		case MODM_DATA:
			dwParam2 = dwParam1 & 0xF0;
			exlen = (dwParam2 >= 0xF8 && dwParam2 <= 0xFF) ? 1 : ((dwParam2 == 0xC0 || dwParam2 == 0xD0) ? 2 : 3);
			BASS_MIDI_StreamEvents(hStream[uDeviceID], BASS_MIDI_EVENTS_RAW, &dwParam1, exlen);
			break;
		case MODM_LONGDATA:
#ifdef DEBUG
			FILE * logfile;
			logfile = fopen("c:\\dbglog2.log", "at");
			if (logfile != NULL) {
				for (int i = 0; i < exlen; i++)
					fprintf(logfile, "%x ", sysexbuffer[i]);
				fprintf(logfile, "\n");
			}
			fclose(logfile);
#endif
			BASS_MIDI_StreamEvents(hStream[uDeviceID], BASS_MIDI_EVENTS_RAW, sysexbuffer, exlen);
			free(sysexbuffer);
			break;
		}
	} while (InterlockedDecrement(&evbcount));
	return played;
}

void load_settings()
{
	HKEY hKey;
	long lResult;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(DWORD);
	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Keppy's Driver\\Settings", 0, KEY_ALL_ACCESS, &hKey);
	RegQueryValueEx(hKey, L"buflen", NULL, &dwType, (LPBYTE)&frames, &dwSize);
	RegQueryValueEx(hKey, L"polyphony", NULL, &dwType, (LPBYTE)&midivoices, &dwSize);
	RegQueryValueEx(hKey, L"tracks", NULL, &dwType, (LPBYTE)&tracks, &dwSize);
	RegQueryValueEx(hKey, L"preload", NULL, &dwType, (LPBYTE)&preload, &dwSize);
	RegQueryValueEx(hKey, L"volume", NULL, &dwType, (LPBYTE)&volume, &dwSize);
	RegQueryValueEx(hKey, L"cpu", NULL, &dwType, (LPBYTE)&maxcpu, &dwSize);
	RegQueryValueEx(hKey, L"encmode", NULL, &dwType, (LPBYTE)&encmode, &dwSize);
	RegQueryValueEx(hKey, L"wasapimode", NULL, &dwType, (LPBYTE)&wasapimode, &dwSize);
	RegQueryValueEx(hKey, L"frequency", NULL, &dwType, (LPBYTE)&frequency, &dwSize);
	RegQueryValueEx(hKey, L"sinc", NULL, &dwType, (LPBYTE)&sinc, &dwSize);
	RegQueryValueEx(hKey, L"debug", NULL, &dwType, (LPBYTE)&debug, &dwSize);
	RegQueryValueEx(hKey, L"realtimeset", NULL, &dwType, (LPBYTE)&realtimeset, &dwSize);
	RegCloseKey(hKey);



	sound_out_volume_float = (float)volume / 10000.0f;
	sound_out_volume_int = (int)(sound_out_volume_float * (float)0x1000);
}

BOOL load_bassfuncs()
{
	TCHAR installpath[MAX_PATH] = { 0 };
	TCHAR basspath[MAX_PATH] = { 0 };
	TCHAR bassmidipath[MAX_PATH] = { 0 };
	TCHAR bassencpath[MAX_PATH] = { 0 };
	TCHAR basswasapipath[MAX_PATH] = { 0 };
	TCHAR pluginpath[MAX_PATH] = { 0 };
	WIN32_FIND_DATA fd;
	HANDLE fh;
	int installpathlength;

	GetModuleFileName(hinst, installpath, MAX_PATH);
	PathRemoveFileSpec(installpath);

	lstrcat(basspath, installpath);
	lstrcat(basspath, L"\\bass.dll");
	if (!(bass = LoadLibrary(basspath))) {
		OutputDebugString(L"Failed to load BASS DLL!");
		return FALSE;
	}
	lstrcat(bassmidipath, installpath);
	lstrcat(bassmidipath, L"\\bassmidi.dll");
	if (!(bassmidi = LoadLibrary(bassmidipath))) {
		OutputDebugString(L"Failed to load BASSMIDI DLL!");
		return FALSE;
	}
	lstrcat(bassencpath, installpath);
	lstrcat(bassencpath, L"\\bassenc.dll");
	if (!(bassenc = LoadLibrary(bassencpath))) {
		OutputDebugString(L"Failed to load BASSENC DLL!");
		return FALSE;
	}
	lstrcat(basswasapipath, installpath);
	lstrcat(basswasapipath, L"\\basswasapi.dll");
	if (!(basswasapi = LoadLibrary(basswasapipath))) {
		OutputDebugString(L"Failed to load BASSWASAPI DLL!");
		return FALSE;
	}
	/* "load" all the BASS functions that are to be used */
	OutputDebugString(L"Loading BASS functions....");
	LOADBASSFUNCTION(BASS_ErrorGetCode);
	LOADBASSFUNCTION(BASS_SetConfig);
	LOADBASSFUNCTION(BASS_ChannelFlags);
	LOADBASSFUNCTION(BASS_Init);
	LOADBASSFUNCTION(BASS_Free);
	LOADBASSFUNCTION(BASS_GetInfo);
	LOADBASSFUNCTION(BASS_StreamFree);
	LOADBASSFUNCTION(BASS_PluginLoad);
	LOADBASSFUNCTION(BASS_SetVolume);
	LOADBASSFUNCTION(BASS_ChannelSetFX);
	LOADBASSFUNCTION(BASS_ChannelGetData);
	LOADBASSFUNCTION(BASS_ChannelRemoveFX);
	LOADBASSFUNCTION(BASS_ChannelSetAttribute);
	LOADBASSFUNCTION(BASS_ChannelGetAttribute);
	LOADBASSMIDIFUNCTION(BASS_MIDI_StreamCreate);
	LOADBASSMIDIFUNCTION(BASS_MIDI_FontInit);
	LOADBASSMIDIFUNCTION(BASS_MIDI_FontLoad);
	LOADBASSMIDIFUNCTION(BASS_MIDI_FontFree);
	LOADBASSMIDIFUNCTION(BASS_MIDI_StreamSetFonts);
	LOADBASSMIDIFUNCTION(BASS_MIDI_StreamEvents);
	LOADBASSMIDIFUNCTION(BASS_MIDI_StreamGetEvent);
	LOADBASSMIDIFUNCTION(BASS_MIDI_StreamEvent);
	LOADBASSMIDIFUNCTION(BASS_MIDI_StreamLoadSamples);
	LOADBASSENCFUNCTION(BASS_Encode_Start);
	LOADBASSENCFUNCTION(BASS_Encode_Stop);
	LOADBASSWASAPIFUNCTION(BASS_WASAPI_Init);
	LOADBASSWASAPIFUNCTION(BASS_WASAPI_Start);
	LOADBASSWASAPIFUNCTION(BASS_WASAPI_PutData);
	OutputDebugString(L"Done.");

	installpathlength = lstrlen(installpath) + 1;
	lstrcat(pluginpath, installpath);
	lstrcat(pluginpath, L"\\bass*.dll");
	fh = FindFirstFile(pluginpath, &fd);
	if (fh != INVALID_HANDLE_VALUE) {
		do {
			HPLUGIN plug;
			pluginpath[installpathlength] = 0;
			lstrcat(pluginpath, fd.cFileName);
			plug = BASS_PluginLoad((char*)pluginpath, BASS_UNICODE);
		} while (FindNextFile(fh, &fd));
		FindClose(fh);
	}
	return TRUE;
}

int IsFloatingPointEnabled(){
	return 1;
}

BOOL IsFloatingPointEnabledBool(){
	return TRUE;
}

int AreEffectsDisabled(){
	long lResult;
	HKEY hKey;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(DWORD);
	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Keppy's Driver\\Settings", 0, KEY_ALL_ACCESS, &hKey);
	RegQueryValueEx(hKey, L"nofx", NULL, &dwType, (LPBYTE)&nofx, &dwSize);
	return nofx;
}

int IsNoteOff1TurnedOn(){
	long lResult;
	HKEY hKey;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(DWORD);
	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Keppy's Driver\\Settings", 0, KEY_ALL_ACCESS, &hKey);
	RegQueryValueEx(hKey, L"noteoff", NULL, &dwType, (LPBYTE)&noteoff1, &dwSize);
	return noteoff1;
}

int IsSoftwareModeEnabled()
{
	return 1;
}

int IgnoreSystemReset()
{
	HKEY hKey;
	long lResult;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(DWORD);
	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Keppy's Driver\\Settings", 0, KEY_ALL_ACCESS, &hKey);
	RegQueryValueEx(hKey, L"sysresetignore", NULL, &dwType, (LPBYTE)&sysresetignore, &dwSize);
	RegCloseKey(hKey);
	return sysresetignore;
}

int check_sinc()
{
	HKEY hKey;
	long lResult;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(DWORD);
	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Keppy's Driver\\Settings", 0, KEY_ALL_ACCESS, &hKey);
	RegQueryValueEx(hKey, L"sinc", NULL, &dwType, (LPBYTE)&sinc, &dwSize);
	RegCloseKey(hKey);
	return sinc;
}

void debug_info() {
	HKEY hKey;
	long lResult;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(DWORD);
	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Keppy's Driver", 0, KEY_ALL_ACCESS, &hKey);
	float currentvoices0;
	float currentcpuusage0;
	int tempo;
	BASS_ChannelGetAttribute(hStream[0], BASS_ATTRIB_MIDI_VOICES_ACTIVE, &currentvoices0);
	BASS_ChannelGetAttribute(hStream[0], BASS_ATTRIB_CPU, &currentcpuusage0);
	int currentvoicesint0 = int(currentvoices0);
	int currentcpuusageint0 = int(currentcpuusage0);
	// Things
	RegSetValueEx(hKey, L"currentvoices0", 0, dwType, (LPBYTE)&currentvoicesint0, sizeof(currentvoicesint0));
	RegSetValueEx(hKey, L"currentcpuusage0", 0, dwType, (LPBYTE)&currentcpuusageint0, sizeof(currentcpuusageint0));
	// OTHER THINGS
	RegSetValueEx(hKey, L"int", 0, dwType, (LPBYTE)&decoded, sizeof(decoded));
	RegCloseKey(hKey);
}

void realtime_load_settings()
{
	HKEY hKey;
	long lResult;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(DWORD);
	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Keppy's Driver\\Settings", 0, KEY_ALL_ACCESS, &hKey);
	RegQueryValueEx(hKey, L"realtimeset", NULL, &dwType, (LPBYTE)&realtimeset, &dwSize);;
	RegQueryValueEx(hKey, L"polyphony", NULL, &dwType, (LPBYTE)&midivoices, &dwSize);
	RegQueryValueEx(hKey, L"tracks", NULL, &dwType, (LPBYTE)&tracks, &dwSize);
	RegQueryValueEx(hKey, L"volume", NULL, &dwType, (LPBYTE)&volume, &dwSize);
	RegQueryValueEx(hKey, L"noteoff", NULL, &dwType, (LPBYTE)&noteoff1, &dwSize);
	RegQueryValueEx(hKey, L"sinc", NULL, &dwType, (LPBYTE)&sinc, &dwSize);
	RegQueryValueEx(hKey, L"nofx", NULL, &dwType, (LPBYTE)&nofx, &dwSize);
	RegQueryValueEx(hKey, L"noteoff", NULL, &dwType, (LPBYTE)&noteoff1, &dwSize);
	RegQueryValueEx(hKey, L"sysresetignore", NULL, &dwType, (LPBYTE)&sysresetignore, &dwSize);
	RegQueryValueEx(hKey, L"cpu", NULL, &dwType, (LPBYTE)&maxcpu, &dwSize);
	RegCloseKey(hKey);
	//cake
	sound_out_volume_float = (float)volume / 10000.0f;
	sound_out_volume_int = (int)(sound_out_volume_float * (float)0x1000);
	//another cake
	int maxmidivoices = static_cast <int> (midivoices);
	float trackslimit = static_cast <int> (tracks);
	BASS_ChannelSetAttribute(hStream[0], BASS_ATTRIB_MIDI_CHANS, trackslimit);
	BASS_ChannelSetAttribute(hStream[0], BASS_ATTRIB_MIDI_VOICES, maxmidivoices);
	BASS_ChannelSetAttribute(hStream[0], BASS_ATTRIB_MIDI_CPU, maxcpu);
	if (noteoff1) {
		BASS_ChannelFlags(hStream[0], BASS_MIDI_NOTEOFF1, BASS_MIDI_NOTEOFF1);
	}
	else {
		BASS_ChannelFlags(hStream[0], 0, BASS_MIDI_NOTEOFF1);
	}

	if (nofx) {
		BASS_ChannelFlags(hStream[0], BASS_MIDI_NOFX, BASS_MIDI_NOFX);
	}
	else {
		BASS_ChannelFlags(hStream[0], 0, BASS_MIDI_NOFX);
	}
	if (sysresetignore) {
		BASS_ChannelFlags(hStream[0], BASS_MIDI_NOSYSRESET, BASS_MIDI_NOSYSRESET);
	}
	else {
		BASS_ChannelFlags(hStream[0], 0, BASS_MIDI_NOSYSRESET);
	}
	if (sinc) {
		BASS_ChannelFlags(hStream[0], BASS_MIDI_SINCINTER, BASS_MIDI_SINCINTER);
	}
	else {
		BASS_ChannelFlags(hStream[0], 0, BASS_MIDI_SINCINTER);
	}
}

BOOL ProcessBlackList(){
	// Blacklist system init
	TCHAR defaultstring[MAX_PATH];
	TCHAR userstring[MAX_PATH];
	TCHAR defaultblacklistdirectory[MAX_PATH];
	TCHAR userblacklistdirectory[MAX_PATH];
	TCHAR modulename[MAX_PATH];
	OSVERSIONINFO osvi;
	BOOL bIsWindowsXPorLater;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&osvi);
	// VirtualMIDISynth ban init
	TCHAR vmidisynthpath[MAX_PATH];
	SHGetFolderPath(NULL, CSIDL_SYSTEM, NULL, 0, vmidisynthpath);
	PathAppend(vmidisynthpath, _T("\\VirtualMIDISynth\\VirtualMIDISynth.dll"));
	try {
		if (osvi.dwMajorVersion == 5) {
			MessageBox(NULL, L"Windows XP is not supported by the driver.", L"Keppy's Driver", MB_OK | MB_ICONERROR);
			return 0x0;
		}
		else if (PathFileExists(vmidisynthpath)) {
			MessageBox(NULL, L"Please uninstall VirtualMIDISynth 1.x before using this driver.\n\nWhy this? Well, VirtualMIDISynth 1.x causes a DLL Hell while loading the BASS libraries, that's why you need to uninstall it before using my driver.\n\nYou can still use VirtualMIDISynth 2.x, since it doesn't load the DLLs directly into the MIDI application.", L"Keppy's Driver", MB_OK | MB_ICONERROR);
			return 0x0;
		}
		else {
			GetModuleFileName(NULL, modulename, MAX_PATH);
			PathStripPath(modulename);
			if (GetWindowsDirectory(defaultblacklistdirectory, MAX_PATH)) {
				_tcscat(defaultblacklistdirectory, L"\\keppymididrv.defaultblacklist");
				std::wifstream file(defaultblacklistdirectory);
				if (file) {
					// The default blacklist exists, continue
					OutputDebugString(defaultblacklistdirectory);
					while (file.getline(defaultstring, sizeof(defaultstring) / sizeof(*defaultstring)))
					{
						if (_tcscmp(modulename, defaultstring) == 0) {
							return 0x0;
						}
					}
				}
				else {
					MessageBox(NULL, L"The default blacklist is missing, or the driver is not installed properly!\nFatal error, can not continue!\n\nPress OK to quit.", L"Keppy's MIDI Driver - FATAL ERROR", MB_OK | MB_ICONERROR);
					exit(0);
				}
			}
			if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, userblacklistdirectory))) {
				PathAppend(userblacklistdirectory, _T("\\Keppy's Driver\\blacklist\\keppymididrv.blacklist"));
				std::wifstream file(userblacklistdirectory);
				OutputDebugString(userblacklistdirectory);
				while (file.getline(userstring, sizeof(userstring) / sizeof(*userstring)))
				{
					if (_tcscmp(modulename, userstring) == 0) {
						std::wstring modulenamelpcwstr(modulename);
						std::wstring concatted_stdstr = L"Keppy's Driver - " + modulenamelpcwstr + L" is blacklisted";
						LPCWSTR messageboxtitle = concatted_stdstr.c_str();
						MessageBox(NULL, L"This program has been manually blacklisted.\n\nThe driver will be automatically unloaded by WinMM.", messageboxtitle, MB_OK | MB_ICONEXCLAMATION);
						return 0x0;
					}
				}
			}
			return 0x1;
		}
	}
	catch (std::exception & e) {
		OutputDebugStringA(e.what());
		exit;
	}
}

void LoadSoundfont(DWORD whichsf){
	TCHAR config[MAX_PATH];
	BASS_MIDI_FONT * mf;
	FreeFonts(0);
	if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, config)))
	{
		if (whichsf == 1) {
			PathAppend(config, _T("\\Keppy's Driver\\lists\\keppymidi.sflist"));
		}
		else if (whichsf == 2) {
			PathAppend(config, _T("\\Keppy's Driver\\lists\\keppymidib.sflist"));
		}
		else if (whichsf == 3) {
			PathAppend(config, _T("\\Keppy's Driver\\lists\\keppymidic.sflist"));
		}
		else if (whichsf == 4) {
			PathAppend(config, _T("\\Keppy's Driver\\lists\\keppymidid.sflist"));
		}
	}
	LoadFonts(0, config);
	BASS_MIDI_StreamLoadSamples(hStream[0]);
}

void ResetSynth(){
	BASS_MIDI_StreamEvent(hStream[0], 0, MIDI_EVENT_SYSTEMEX, MIDI_SYSTEM_DEFAULT);
}

BOOL IsVistaOrNewer(){
	OSVERSIONINFOEX osvi;
	BOOL bOsVersionInfoEx;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*)&osvi);
	if (bOsVersionInfoEx == FALSE) return FALSE;
	if (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId &&
		osvi.dwMajorVersion > 5)
		return TRUE;
	return FALSE;
}

void ReloadSFList(DWORD whichsflist){
	if (consent == 1) {
		std::wstringstream ss;
		ss << "Do you want to (re)load list n°" << whichsflist << "?";
		std::wstring s = ss.str();
		const int result = MessageBox(NULL, s.c_str(), L"Keppy's Driver", MB_ICONINFORMATION | MB_YESNO);
		switch (result)
		{
		case IDYES:
			ResetSynth();
			LoadSoundfont(whichsflist);
			break;
		case IDNO:
			break;
		}
	}
	else {
		//NULL 
	}
}

void keybindings()
{
	if (GetAsyncKeyState(VK_MENU) & GetAsyncKeyState(0x31) & 0x8000) {
		ReloadSFList(1);
	}
	else if (GetAsyncKeyState(VK_MENU) & GetAsyncKeyState(0x32) & 0x8000) {
		ReloadSFList(2);
	}
	else if (GetAsyncKeyState(VK_MENU) & GetAsyncKeyState(0x33) & 0x8000) {
		ReloadSFList(3);
	}
	else if (GetAsyncKeyState(VK_MENU) & GetAsyncKeyState(0x34) & 0x8000) {
		ReloadSFList(4);
	}
	if (GetAsyncKeyState(VK_INSERT) & 1) {
		if (consent == 1) {
			ResetSynth();
		}
		else {
			//NULL 
		}
	}
}

std::wstring s2ws(const std::string& s)
{
	int len;
	int slength = (int)s.length() + 1;
	len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
	wchar_t* buf = new wchar_t[len];
	MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, buf, len);
	std::wstring r(buf);
	delete[] buf;
	return r;
}

unsigned __stdcall threadfunc(LPVOID lpV){
	unsigned i;
	int pot;
	int opend = 0;
	BASS_MIDI_FONT * mf;
	while (opend == 0 && stop_thread == 0) {
		if (!com_initialized) {
			if (FAILED(CoInitialize(NULL))) continue;
			com_initialized = TRUE;
		}
		load_settings();
		load_bassfuncs();
		float trackslimit = static_cast <int> (tracks);
		int maxmidivoices = static_cast <int> (midivoices);
		int frequencyvalue = static_cast <int> (frequency);
		BASS_SetConfig(BASS_CONFIG_MIDI_VOICES, 9999);
		OutputDebugString(L"Initializing the stream...");
		if (BASS_Init(0, frequencyvalue, 0, NULL, NULL)) {
			if (wasapimode == 0) {
				if (sound_driver == NULL) {
					sound_driver = create_sound_out_xaudio2();
					sound_out_float = IsVistaOrNewer();
					sound_driver->open(g_msgwnd->get_hwnd(), frequency + 100, 2, (IsFloatingPointEnabled() ? sound_out_float : nofloat), SPFSTD, frames);
					/* Why frequency + 100? There's a bug on XAudio that cause clipping when the MIDI driver's audio frequency is the same has the sound card's max audio frequency. */
				}
			}
			else {
				BASS_WASAPI_Init(-1, 0, 2, BASS_WASAPI_SHARED, (float)30 * 0.001, 0.005, NULL, NULL);
				BASS_WASAPI_Start();
			}
			consent = 1;
			hStream[0] = BASS_MIDI_StreamCreate(tracks, BASS_STREAM_DECODE | (IgnoreSystemReset() ? BASS_MIDI_NOSYSRESET : sysresetignore) | (IsSoftwareModeEnabled() ? BASS_SAMPLE_SOFTWARE : softwaremode) | (IsFloatingPointEnabled() ? BASS_SAMPLE_FLOAT : nofloat) | (IsNoteOff1TurnedOn() ? BASS_MIDI_NOTEOFF1 : noteoff1) | (AreEffectsDisabled() ? BASS_MIDI_NOFX : nofx) | (check_sinc() ? BASS_MIDI_SINCINTER : sinc), 0);
			if (!hStream[0]) {
				BASS_StreamFree(hStream[0]);
				hStream[0] = 0;
				continue;
			}
			// Encoder code
			if (encmode == 1) {
				typedef std::basic_string<TCHAR> tstring;
				TCHAR encpath[MAX_PATH];
				TCHAR buffer[MAX_PATH] = { 0 };
				TCHAR * out;
				DWORD bufSize = sizeof(buffer) / sizeof(*buffer);
				if (GetModuleFileName(NULL, buffer, bufSize) == bufSize) { }
				out = PathFindFileName(buffer);
				std::wstring stemp = tstring(out) + L" - Keppy's Driver Output File.wav";
				LPCWSTR result2 = stemp.c_str();
				if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_DESKTOP, NULL, 0, encpath)))
				{
					PathAppend(encpath, result2);
					
				}
				_bstr_t b(encpath);
				const char* c = b;
				const int result = MessageBox(NULL, L"You've enabled the \"Output to WAV\" mode.\n\nPress YES to confirm, or press NO to prevent the driver from outputting the audio to a WAV file.\n\n(The WAV file will be automatically saved to the desktop)", L"Keppy's Driver", MB_ICONINFORMATION | MB_YESNO);
				switch (result)
				{
				case IDYES:
					BASS_Encode_Start(hStream[0], c, BASS_ENCODE_PCM, NULL, 0);
					break;
				case IDNO:
					break;
				}
			}
			// Cake.
			BASS_MIDI_StreamEvent(hStream[0], 0, MIDI_EVENT_SYSTEM, MIDI_SYSTEM_DEFAULT);
			BASS_ChannelSetAttribute(hStream[0], BASS_ATTRIB_MIDI_CHANS, trackslimit);
			BASS_ChannelSetAttribute(hStream[0], BASS_ATTRIB_MIDI_VOICES, maxmidivoices);
			BASS_ChannelSetAttribute(hStream[0], BASS_ATTRIB_MIDI_CPU, maxcpu);
			LoadSoundfont(1);
			SetEvent(load_sfevent);
			opend = 1;
			reset_synth[0] = 0;
		}
	}
	while (stop_thread == 0){
		Sleep(100);
		if (reset_synth[0] != 0){
			reset_synth[0] = 0;
			load_settings();
			BASS_MIDI_StreamEvent(hStream[0], 0, MIDI_EVENT_SYSTEM, MIDI_SYSTEM_DEFAULT);
			BASS_MIDI_StreamLoadSamples(hStream[0]);
		}
		bmsyn_play_some_data();
		if (sound_out_float) {
			float sound_buffer[2][SPFSTD];
			decoded = BASS_ChannelGetData(hStream[0], sound_buffer[0], BASS_DATA_FLOAT + SPFSTD * sizeof(float));
			if (decoded < 0) {

			}
			else {
				for (unsigned i = 0, j = decoded / sizeof(float); i < j; i++) {
					float sample = sound_buffer[0][i];
					sample *= sound_out_volume_float;
					sound_buffer[0][i] = sample;
				}
				if (wasapimode == 0) {
					sound_driver->write_frame(sound_buffer[0], decoded / sizeof(float), false);
				}
				else {
					BASS_WASAPI_PutData(sound_buffer[0], decoded / sizeof(float));
				}	
			}
		}
		else {
			short sound_buffer[2][SPFSTD];
			decoded = BASS_ChannelGetData(hStream[0], sound_buffer[0], SPFSTD * sizeof(short));
			if (decoded < 0) {
			
			}
			else {
				for (unsigned i = 0, j = decoded / sizeof(short); i < j; i++) {
					int sample = sound_buffer[0][i];
					sample = (sample * sound_out_volume_int) >> 12;
					if ((sample + 0x8000) & 0xFFFF0000) sample = 0x7FFF ^ (sample >> 31);
					sound_buffer[0][i] = sample;
				}
				if (wasapimode == 0) {
					sound_driver->write_frame(sound_buffer[0], decoded / sizeof(float), false);
				}
				else {
					BASS_WASAPI_PutData(sound_buffer[0], SPFSTD * sizeof(short));
				}
			}
		}
		realtime_load_settings();
		keybindings();
		debug_info();
	}
	stop_thread = 0;
	if (hStream[0])
	{
		if (consent == 1) {
			ResetSynth();
		}
		else {
			//NULL
		}
		BASS_StreamFree(hStream[0]);
		hStream[1] = 0;
	}
	if (bassmidi) {
		if (consent == 1) {
			ResetSynth();
		}
		else {
			//NULL
		}
		FreeFonts(0);
		FreeLibrary(bassmidi);
		bassmidi = 0;
	}
	if (bass) {
		if (consent == 1) {
			ResetSynth();
		}
		else {
			//NULL
		}
		BASS_Free();
		FreeLibrary(bass);
		bass = 0;
	}
	if (sound_driver) {
		if (consent == 1) {
			ResetSynth();
		}
		else {
			//NULL
		}
		delete sound_driver;
		sound_driver = NULL;
	}
	if (com_initialized) {
		CoUninitialize();
		com_initialized = FALSE;
	}
	_endthreadex(0);
	return 0;
}

void DoCallback(int driverNum, int clientNum, DWORD msg, DWORD_PTR param1, DWORD_PTR param2) {
	struct Driver_Client *client = &drivers[driverNum].clients[clientNum];
	DriverCallback(client->callback, client->flags, drivers[driverNum].hdrvr, msg, client->instance, param1, param2);
}

void DoStartClient() {
	if (modm_closed == 1) {
		DWORD result;
		unsigned int thrdaddr;
		InitializeCriticalSection(&mim_section);
		processPriority = GetPriorityClass(GetCurrentProcess());
		SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
		load_sfevent = CreateEvent(
			NULL,               // default security attributes
			TRUE,               // manual-reset event
			FALSE,              // initial state is nonsignaled
			TEXT("SoundFontEvent")  // object name
			);
		hCalcThread = (HANDLE)_beginthreadex(NULL, 0, threadfunc, 0, 0, &thrdaddr);
		SetPriorityClass(hCalcThread, REALTIME_PRIORITY_CLASS);
		SetThreadPriority(hCalcThread, THREAD_PRIORITY_TIME_CRITICAL);
		result = WaitForSingleObject(load_sfevent, INFINITE);
		if (result == WAIT_OBJECT_0)
		{
			CloseHandle(load_sfevent);
		}
		modm_closed = 0;
	}
}

void DoStopClient() {
	HKEY hKey;
	long lResult;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(DWORD);
	int One = 0;
	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Keppy's Driver", 0, KEY_ALL_ACCESS, &hKey);
	RegSetValueEx(hKey, L"currentvoices0", 0, dwType, (LPBYTE)&One, 1);
	RegSetValueEx(hKey, L"currentcpuusage0", 0, dwType, (LPBYTE)&One, 1);
	RegSetValueEx(hKey, L"int", 0, dwType, (LPBYTE)&One, 1);
	if (consent == 1) {
		ResetSynth();
	}
	else {
		//NULL
	}
	RegCloseKey(hKey);
	if (modm_closed == 0){
		stop_thread = 1;
		WaitForSingleObject(hCalcThread, INFINITE);
		CloseHandle(hCalcThread);
		modm_closed = 1;
		SetPriorityClass(GetCurrentProcess(), processPriority);
	}
	DeleteCriticalSection(&mim_section);
}

void DoResetClient(UINT uDeviceID) {
	/*
	TODO : If the driver's output queue contains any output buffers (see MODM_LONGDATA) whose contents
	have not been sent to the kernel-mode driver, the driver should set the MHDR_DONE flag and
	clear the MHDR_INQUEUE flag in each buffer's MIDIHDR structure, and then send the client a
	MOM_DONE callback message for each buffer.
	*/
	ResetSynth();
	reset_synth[uDeviceID] = 1;
}

LONG DoOpenClient(struct Driver *driver, UINT uDeviceID, LONG* dwUser, MIDIOPENDESC * desc, DWORD flags) {
	/*	For the MODM_OPEN message, dwUser is an output parameter.
	The driver creates the instance identifier and returns it in the address specified as
	the argument. The argument is the instance identifier.
	CALLBACK_EVENT Indicates dwCallback member of MIDIOPENDESC is an event handle.
	CALLBACK_FUNCTION Indicates dwCallback member of MIDIOPENDESC is the address of a callback function.
	CALLBACK_TASK Indicates dwCallback member of MIDIOPENDESC is a task handle.
	CALLBACK_WINDOW Indicates dwCallback member of MIDIOPENDESC is a window handle.
	*/
	int clientNum;
	if (driver->clientCount == 0) {
		//TODO: Part of this might be done in DoDriverOpen instead.
		DoStartClient();
		DoResetClient(uDeviceID);
		clientNum = 0;
	}
	else if (driver->clientCount == MAX_CLIENTS) {
		return MMSYSERR_ALLOCATED;
	}
	else {
		int i;
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (!driver->clients[i].allocated) {
				break;
			}
		}
		if (i == MAX_CLIENTS) {
			return MMSYSERR_ALLOCATED;
		}
		clientNum = i;
	}
	driver->clients[clientNum].allocated = 1;
	driver->clients[clientNum].flags = HIWORD(flags);
	driver->clients[clientNum].callback = desc->dwCallback;
	driver->clients[clientNum].instance = desc->dwInstance;
	*dwUser = clientNum;
	driver->clientCount++;
	SetPriorityClass(GetCurrentProcess(), processPriority);
	//TODO: desc and flags

	DoCallback(uDeviceID, clientNum, MOM_OPEN, 0, 0);
	return MMSYSERR_NOERROR;
}

LONG DoCloseClient(struct Driver *driver, UINT uDeviceID, LONG dwUser) {
	/*
	If the client has passed data buffers to the user-mode driver by means of MODM_LONGDATA
	messages, and if the user-mode driver hasn't finished sending the data to the kernel-mode driver,
	the user-mode driver should return MIDIERR_STILLPLAYING in response to MODM_CLOSE.
	After the driver closes the device instance it should send a MOM_CLOSE callback message to
	the client.
	*/

	if (!driver->clients[dwUser].allocated) {
		return MMSYSERR_INVALPARAM;
	}

	driver->clients[dwUser].allocated = 0;
	driver->clientCount--;
	if (driver->clientCount <= 0) {
		DoResetClient(uDeviceID);
		driver->clientCount = 0;
	}
	DoCallback(uDeviceID, dwUser, MOM_CLOSE, 0, 0);
	return MMSYSERR_NOERROR;
}
/* Audio Device Messages for MIDI http://msdn.microsoft.com/en-us/library/ff536194%28v=vs.85%29 */
STDAPI_(DWORD) modMessage(UINT uDeviceID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dwParam1, DWORD_PTR dwParam2){
	MIDIHDR *IIMidiHdr;
	UINT evbpoint;
	struct Driver *driver = &drivers[uDeviceID];
	int exlen = 0;
	unsigned char *sysexbuffer = NULL;
	DWORD result = 0;
	switch (uMsg) {
	case MODM_OPEN:
		return DoOpenClient(driver, uDeviceID, reinterpret_cast<LONG*>(dwUser), reinterpret_cast<MIDIOPENDESC*>(dwParam1), static_cast<DWORD>(dwParam2));
	case MODM_PREPARE:
		/*If the driver returns MMSYSERR_NOTSUPPORTED, winmm.dll prepares the buffer for use. For
		most drivers, this behavior is sufficient.*/
		return MMSYSERR_NOTSUPPORTED;
	case MODM_UNPREPARE:
		return MMSYSERR_NOTSUPPORTED;
	case MODM_GETNUMDEVS:
		return ProcessBlackList();
	case MODM_GETDEVCAPS:
		return modGetCaps(uDeviceID, reinterpret_cast<MIDIOUTCAPS*>(dwParam1), static_cast<DWORD>(dwParam2));
	case MODM_LONGDATA:
		IIMidiHdr = (MIDIHDR *)dwParam1;
		if (!(IIMidiHdr->dwFlags & MHDR_PREPARED)) return MIDIERR_UNPREPARED;
		IIMidiHdr->dwFlags &= ~MHDR_DONE;
		IIMidiHdr->dwFlags |= MHDR_INQUEUE;
		exlen = (int)IIMidiHdr->dwBufferLength;
		if (NULL == (sysexbuffer = (unsigned char *)malloc(exlen * sizeof(char)))){
			return MMSYSERR_NOMEM;
		}
		else{
			memcpy(sysexbuffer, IIMidiHdr->lpData, exlen);
#ifdef DEBUG
			FILE * logfile;
			logfile = fopen("c:\\dbglog.log", "at");
			if (logfile != NULL) {
				fprintf(logfile, "sysex %d byete\n", exlen);
				for (int i = 0; i < exlen; i++)
					fprintf(logfile, "%x ", sysexbuffer[i]);
				fprintf(logfile, "\n");
			}
			fclose(logfile);
#endif
		}
		/*
		TODO: 	When the buffer contents have been sent, the driver should set the MHDR_DONE flag, clear the
		MHDR_INQUEUE flag, and send the client a MOM_DONE callback message.


		In other words, these three lines should be done when the evbuf[evbpoint] is sent.
		*/
		IIMidiHdr->dwFlags &= ~MHDR_INQUEUE;
		IIMidiHdr->dwFlags |= MHDR_DONE;
		DoCallback(uDeviceID, static_cast<LONG>(dwUser), MOM_DONE, dwParam1, 0);
		//fallthrough
	case MODM_DATA:
		EnterCriticalSection(&mim_section);
		evbpoint = evbwpoint;
		if (++evbwpoint >= EVBUFF_SIZE)
			evbwpoint -= EVBUFF_SIZE;
		evbuf[evbpoint].uDeviceID = uDeviceID;
		evbuf[evbpoint].uMsg = uMsg;
		evbuf[evbpoint].dwParam1 = dwParam1;
		evbuf[evbpoint].dwParam2 = dwParam2;
		evbuf[evbpoint].exlen = exlen;
		evbuf[evbpoint].sysexbuffer = sysexbuffer;
		LeaveCriticalSection(&mim_section);
		if (InterlockedIncrement(&evbcount) >= EVBUFF_SIZE) {
			do
			{

			} while (evbcount >= EVBUFF_SIZE);
		}
		return MMSYSERR_NOERROR;
	case MODM_GETVOLUME: {
		*(LONG*)dwParam1 = static_cast<LONG>(sound_out_volume_float * 0xFFFF);
		return MMSYSERR_NOERROR;
	}
	case MODM_SETVOLUME: {
		sound_out_volume_float = LOWORD(dwParam1) / (float)0xFFFF;
		sound_out_volume_int = (int)(sound_out_volume_float * (float)0x1000);
		return MMSYSERR_NOERROR;
	}
	case MODM_PAUSE: {
		reset_synth[uDeviceID] = 1;
		return MMSYSERR_NOERROR;
	}
	case MODM_STOP: {
		reset_synth[uDeviceID] = 1;
		return MMSYSERR_NOERROR;
	}
	case MODM_RESET:
		DoResetClient(uDeviceID);
		return MMSYSERR_NOERROR;
		/*
		MODM_GETPOS
		MODM_PAUSE
		//The driver must halt MIDI playback in the current position. The driver must then turn off all notes that are currently on.
		MODM_RESTART
		//The MIDI output device driver must restart MIDI playback at the current position.
		// playback will start on the first MODM_RESTART message that is received regardless of the number of MODM_PAUSE that messages were received.
		//Likewise, MODM_RESTART messages that are received while the driver is already in play mode must be ignored. MMSYSERR_NOERROR must be returned in either case
		MODM_STOP
		//Like reset, without resetting.
		MODM_PROPERTIES
		MODM_STRMDATA
		*/
	case MODM_CLOSE:
		return DoCloseClient(driver, uDeviceID, static_cast<LONG>(dwUser));
		break;

		/*
		MODM_CACHEDRUMPATCHES
		MODM_CACHEPATCHES
		*/

	default:
		return MMSYSERR_NOERROR;
		break;
	}
}