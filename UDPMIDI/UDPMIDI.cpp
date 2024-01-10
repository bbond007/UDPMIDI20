//#define USE_ASYNC_SELECT
//#define _WINSOCK_DEPRECATED_NO_WARNINGS TRUE
//#define DEBUG_LOGGING
#include <Winsock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <CommCtrl.h>
#include <tchar.h>
#include <mmeapi.h>
#include <stdio.h>
#include <string.h>
#include <wingdi.h>
#include "resource.h"
#include "atlstr.h"

#pragma comment(lib, "ComCtl32.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "msimg32.lib")
#define ABOUT_DLG_TXT "UDPMIDI c++ version\n\n2.2 release : 01/10/2024\n\nBinaryBond007@gmail.com"
#define WM_UDPSOCK_IN (WM_USER + 1)
#define WM_SHELL_ICO  (WM_USER + 2)


static struct        sockaddr_in SenderAddr;
static struct        sockaddr_in OLDSenderAddr;
static struct        sockaddr_in client_addr;
static HICON         hIcon;
static HWND          hwndComboBoxMIDIOutDevice;
static HWND          hwndComboBoxMIDIInDevice;
static HWND          hwndEditPacketsRX;
static HWND          hwndEditPacketsTX;
static HWND          hwndEditUDPPort;
static HWND          hwndEditLastErr;
static HWND          hwndEditLastErrStr;
static HWND          hwndEditLastErrLbl;
static HWND          hwndEditSenderIP;
static HMIDIOUT      handMIDIOut               = 0;
static HMIDIIN       handMIDIIn                = 0;
static unsigned int  RXCount                   = 0;
static unsigned int  TXCount                   = 0;
static HANDLE        handUDPinThread           = 0;
static SOCKET        listenerSocket            = INVALID_SOCKET;
static SOCKET        clientSocket              = INVALID_SOCKET;
static int           SenderAddrSize            = sizeof(SenderAddr);
//static bool          bAddBuffer                = true;
static char          MT32_Message[]            = "<-UDPMIDI  VER 2.2->";
static char          defaultClientDeviceName[] = "mt32-pi";
static char          ClientDeviceName[30];
static MIDIHDR	     midiInHdr;
static unsigned char SysXBuffer[2048];
#ifdef DEBUG_LOGGING
static FILE*         logFile                   = 0;

static void debugDumpBuf(byte* buf, size_t bufLen)
{
    for (int i = 0; i < bufLen; i++)
        if (i == bufLen - 1)
            fprintf(logFile, "%02x\r\n", *buf++);
        else
            fprintf(logFile, "%02x:", *buf++);
}
#endif

static void SetEditBoxINT(HWND hwnd, int val)
{
    char buf[11];
    wchar_t text_wchar[0xff]; 
    //memset(buf, 0x00, sizeof(buf)); // ok compiler warning, i don't think this is necessacary, but whatever. 
    _itoa_s(val, buf, 11);
    size_t length = strlen(buf);      // TODO --> C6054 : string buf might not be 0 terminated? ok
    mbstowcs_s(&length, text_wchar, buf, length);
    SendMessage(hwnd, (UINT)WM_SETTEXT, (WPARAM)0, (LPARAM)text_wchar);
}

static void SetEditBoxTXT(HWND hwnd, const char * val)
{
    wchar_t text_wchar[0xff];
    size_t length = strlen(val);
    mbstowcs_s(&length, text_wchar, val, length);
    SendMessage(hwnd, (UINT)WM_SETTEXT, (WPARAM)0, (LPARAM)text_wchar);
}

static void ShoWErrorControls()
{
    ShowWindow(hwndEditLastErrStr, SW_SHOW);
    ShowWindow(hwndEditLastErr, SW_SHOW);
    ShowWindow(hwndEditLastErrLbl, SW_SHOW);
}

static void SetLastErr(const char * err, int errNo)
{
    SetEditBoxINT(hwndEditLastErr, errNo);
    if (err != NULL)
        SetEditBoxTXT(hwndEditLastErrStr, err);
    ShoWErrorControls();
}

static void SendMIDIShortMessage(HWND hwnd, u_int note)
{
    if (handMIDIOut != 0)
    {
        midiOutShortMsg(handMIDIOut, note);
    }
    else
        MessageBox(hwnd, TEXT("MIDI not started!"), TEXT("OOPS!"),
            MB_OK);
}

static void SendMIDILongMessage(HWND hwnd, byte* buf, size_t bufLen)
{
    MIDIHDR     midiHdr;
    UINT        err;
    if (handMIDIOut != 0)
    {
        /* Store pointer in MIDIHDR */
        midiHdr.lpData = (LPSTR)&buf[0];
        /* Store its size in the MIDIHDR */
        midiHdr.dwBufferLength = bufLen;
        /* Flags must be set to 0 */
        midiHdr.dwFlags = 0;

        /* Prepare the buffer and MIDIHDR */
        err = midiOutPrepareHeader(handMIDIOut, &midiHdr, sizeof(MIDIHDR));
        if (!err)
        {
            /* Output the long message */
            err = midiOutLongMsg(handMIDIOut, &midiHdr, sizeof(MIDIHDR));
            /*
            if (err)
            {
                char errMsg[200];
                MMRESULT get_res = midiOutGetErrorText(err, (LPWSTR)errMsg, sizeof(errMsg));
                SetLastErr(errMsg, err);
                MessageBox(hwnd, TEXT("WSAStartup failed!"), TEXT("OOPS!"),
                   MB_OK);
            }
            */
            /* Unprepare the buffer and MIDIHDR */
            while (MIDIERR_STILLPLAYING == midiOutUnprepareHeader(handMIDIOut, &midiHdr, sizeof(MIDIHDR)))
            {
                /* Should put a delay in here rather than a busy-wait - DONE :P*/
                Sleep(1000);
            }
        }
    }
    else
        MessageBox(hwnd, TEXT("MIDI not started!"), TEXT("OOPS!"), MB_OK);
}


static void GetMidiCloseError(int errorNo, char buf[], size_t bufLen)
{
    char tmp[30];
    switch (errorNo)
    {
    case MIDIERR_STILLPLAYING:
        strcat_s(buf, bufLen, "Buffers are still in the queue.");
        break;
    case MMSYSERR_INVALHANDLE:
        strcat_s(buf, bufLen, "The specified device handle is invalid.");
        break;
    case MMSYSERR_NOMEM:
        strcat_s(buf, bufLen, "The system is unable to allocate or lock memory.");
        break;
    default:
        sprintf_s(tmp, "Unknown error #%d", errorNo);
        strcat_s(buf, bufLen, tmp);
        break;
    }
}

static bool CloseMIDIOutDevice(HWND hwnd)
{
    char buf[100] = "CloseMIDIOutDevice() --> ";
    if (handMIDIOut != 0)
    {
        int result = midiOutClose(handMIDIOut);
        if (result == MMSYSERR_NOERROR)
            handMIDIOut = 0;
        else
        {
            GetMidiCloseError(result, buf, sizeof(buf));
            MessageBoxA(hwnd, buf, "ERROR:", MB_OK);
            return false;
        }
    }
    return true;
}

static bool CloseMIDIInDevice(HWND hwnd)
{
    char buf[100] = "CloseMIDIInDevice() --> ";
    if (handMIDIIn != 0)
    {
        //bAddBuffer = false;
        int result = midiInReset(handMIDIIn);
        //result = midiInUnprepareHeader(handMIDIIn, &midiInHdr, sizeof(MIDIHDR));
        //while ((result = midiInClose(handMIDIIn)) == MIDIERR_STILLPLAYING) Sleep(0);
        result = midiInClose(handMIDIIn);
        if (result == MMSYSERR_NOERROR)
            handMIDIIn = 0;
        else
        {
            GetMidiCloseError(result, buf, sizeof(buf));
            MessageBoxA(hwnd, buf, "ERROR:", MB_OK);
            return false;
        }
    }
    return true;
}

static void _CloseMIDIDevice(HWND hwnd)
{
    CloseMIDIOutDevice(hwnd);
    CloseMIDIInDevice(hwnd);
}

static int udpsock_client_connect(char* ipAddr, int port)
{
    SOCKET sock = 0;
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        SetLastErr("udpsock_client_connect", errno);
        return INVALID_SOCKET;
    }
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(port);
    //server_addr.sin_addr.s_addr = inet_addr(ipAddr);
    inet_pton(AF_INET, ipAddr, &client_addr.sin_addr);
    return sock;
}

static int udpsock_client_write(int sock, byte* buf, size_t bufLen)
{
    int result = 0;
    if (clientSocket != INVALID_SOCKET)
    {
        result = sendto(sock,
            (const char*)buf,
            bufLen,
            0, //MSG_CONFIRM,
            (const struct sockaddr*)&client_addr,
            sizeof(client_addr));
        if (result < 0)
            SetLastErr("udpsock_client_write", errno);
    }
    //SetEditBoxINT(hwndEditPacketsTX, ++TXCount);
    return result;
}

#ifdef USE_SET_MT32_LCD_OLD
static void SetMT32_LCD(HWND hwnd, char* MT32Message, bool client = false)
{
    byte buf[] =
    { 0xF0, 0x41, 0x10, 0x16, 0x12, 0x20, 0x00, 0x00,
                  0,0,0,0,0, //sysex character data
                  0,0,0,0,0, // "
                  0,0,0,0,0, // "
                  0,0,0,0,0, // "
                  0x00, /* checksum placedholder */
                  0xF7  /* end of sysex */
    };
    byte checksum = 0;
    int MT32MessageIndex = 0;
    size_t MT32MessageLength = strlen(MT32Message);
    for (int bufIndex = 5; bufIndex < sizeof(buf) - 2; bufIndex++)
    {
        if (bufIndex > 7)
        {
            if (MT32MessageIndex < MT32MessageLength)
                buf[bufIndex] = (byte)MT32Message[MT32MessageIndex++];
            else
                buf[bufIndex] = 0x20;
        }
        checksum += buf[bufIndex];
    }
    checksum = 128 - checksum % 128;
    buf[sizeof(buf) - 2] = (byte)(checksum);
    if (!client)
        SendMIDILongMessage(hwnd, buf, sizeof(buf));
    else
        udpsock_client_write(clientSocket, buf, sizeof(buf));
}
#else
static void SetMT32_LCD(HWND hwnd, char* MT32Message, bool client = false)
{

    byte hdr[] = { 0xF0, 0x41, 0x10, 0x16, 0x12, 0x20, 0x00, 0x00 };
    byte buf[255];
    byte iBuf = sizeof(hdr);
    memcpy(buf, hdr, sizeof(hdr)); 
    byte checksum = 0x20;
    for (int i = 0; i < strlen(MT32Message); i++)
    {
        checksum += MT32Message[i];
        buf[iBuf++] = MT32Message[i];
    } 
    checksum = 128 - checksum % 128;
    buf[iBuf++] = checksum;
    buf[iBuf++] = 0xf7;
    if (!client)
        SendMIDILongMessage(hwnd, buf, iBuf);
    else
        udpsock_client_write(clientSocket, buf, iBuf);
}
#endif

size_t GetMidiMessageLength(byte status_byte)
{
    if (status_byte < 0x80)
        return 0;
    if (0x80 <= status_byte && status_byte <= 0xbf)
        return 3;
    if (0xc0 <= status_byte && status_byte <= 0xdf)
        return 2;
    if (0xe0 <= status_byte && status_byte <= 0xef)
        return 3;
    if (status_byte == 0xf0)
        return 0;
    if (status_byte == 0xf1)
        return 2;
    if (status_byte == 0xf2)
        return 3;
    if (status_byte == 0xf3)
        return 2;
    if (0xf4 <= status_byte && status_byte <= 0xf6)
        return 1;
    if (status_byte == 0xf7)
        return 0;
    // 0xf8 <= status_byte && status_byte <= 0xff
    return 1;
}

static void CALLBACK MIDIInCallback(HMIDIIN handle, UINT uMsg, DWORD dwInstance, DWORD dwParam1, DWORD dwParam2)
{
    byte shortBuf[3];
    size_t shortBufLen;
    LPMIDIHDR hdr;
    MMRESULT result; 
 
    switch (uMsg)
    {

    case MIM_OPEN:
        break;

    case MIM_CLOSE:
        break;

    case MIM_DATA:
        shortBuf[0] = dwParam1 & 0xFF;
        shortBuf[1] = (dwParam1 >> 8) & 0xFF;
        shortBuf[2] = (dwParam1 >> 16) & 0xFF;
        shortBufLen = GetMidiMessageLength(shortBuf[0]);
        if (shortBufLen)
        {
            udpsock_client_write(clientSocket, shortBuf, shortBufLen);
            SetEditBoxINT(hwndEditPacketsTX, ++TXCount);
        }
        break;

    case MIM_LONGDATA:
        hdr = (LPMIDIHDR)dwParam1;
        if (hdr->dwBytesRecorded > 0)
        {
            udpsock_client_write(clientSocket, (byte*)hdr->lpData, hdr->dwBytesRecorded);
            result = midiInPrepareHeader(handle, hdr, sizeof(MIDIHDR));
            result = midiInAddBuffer(handle, hdr, sizeof(MIDIHDR));
        }
       /* else
            if (hdr && (hdr->dwFlags & MHDR_PREPARED) == MHDR_PREPARED)
                result = midiInUnprepareHeader(handle, hdr, sizeof(MIDIHDR));*/
        SetEditBoxINT(hwndEditPacketsTX, ++TXCount);
        break;

    case MIM_ERROR:
        break;

    case MIM_LONGERROR:
        break;

    case MIM_MOREDATA:
        break;
    }
}

static void GetMidiInOpenError(int errorNo, char buf[], size_t bufLen)
{
    char tmp[30];
    switch (errorNo)
    {
    case MMSYSERR_ALLOCATED:
        strcat_s(buf, bufLen, "The specified resource is already allocated.");
        break;
    case MMSYSERR_BADDEVICEID:
        strcat_s(buf, bufLen, "The specified device identifier is out of range.");
        break;
    case MMSYSERR_INVALFLAG:
        strcat_s(buf, bufLen, "The flags specified by dwFlags are invalid.");
        break;
    case MMSYSERR_INVALPARAM:
        strcat_s(buf, bufLen, "The specified pointer or structure is invalid.");
        break;
    case MMSYSERR_NOMEM:
        strcat_s(buf, bufLen, "The system is unable to allocate or lock memory.");
        break;
    default:
        sprintf_s(tmp, "Unknown error #%d", errorNo);
        strcat_s(buf, bufLen, tmp);
        break;
    }
}

static void GetMidiOutCloseError(int errorNo, char buf[], size_t bufLen)
{
    char tmp[30];
    switch (errorNo)
    {
    default:
        sprintf_s(tmp, "Unknown error #%d", errorNo);
        strcat_s(buf, bufLen, tmp);
        break;
    }
}

static void GetMidiOutOpenError(int errorNo, char buf[], size_t bufLen)
{
    char tmp[30];
    switch (errorNo)
    {
    case MIDIERR_NODEVICE:
        strcat_s(buf, bufLen, "No MIDI port was found.This error occurs only when the mapper is opened.");
        break;
    case MMSYSERR_ALLOCATED:
        strcat_s(buf, bufLen, "The specified resource is already allocated.");
        break;
    case MMSYSERR_BADDEVICEID:
        strcat_s(buf, bufLen, "The specified device identifier is out of range.");
        break;
    case MMSYSERR_INVALPARAM:
        strcat_s(buf, bufLen, "The specified pointer or structure is invalid.");
        break;
    case MMSYSERR_NOMEM:
        strcat_s(buf, bufLen, "The system is unable to allocate or lock memory.");
        break;
    default:
        sprintf_s(tmp, "Unknown error #%d", errorNo);
        strcat_s(buf, bufLen, tmp);
        break;
    }
}

static void OpenMIDIOutDevice(HWND hwnd)
{
    char buf[100];
    int result;
    CloseMIDIOutDevice(hwnd);
    int iMIDIOutDev = (int)SendMessage(hwndComboBoxMIDIOutDevice, CB_GETCURSEL, 0, 0);
    if (iMIDIOutDev != -1)
    {
        result = midiOutOpen(&handMIDIOut, iMIDIOutDev, 0, 0, 0);
        if (result != MMSYSERR_NOERROR)
        {
            sprintf_s(buf, "midiInOpen(%d) --> ", iMIDIOutDev);
            GetMidiOutOpenError(result, buf, sizeof(buf));
            MessageBoxA(hwnd, buf, "ERROR:", MB_OK);
        }
        else
            SetMT32_LCD(hwnd, MT32_Message);
    }
    else
        MessageBox(hwnd, TEXT("No MIDI device selected!"), TEXT("OOPS!"), MB_OK);
}

static void OpenMIDIInDevice(HWND hwnd)
{
    char buf[100];
    int result;
    CloseMIDIInDevice(hwnd);
    int iMIDIInDev = (int)SendMessage(hwndComboBoxMIDIInDevice, CB_GETCURSEL, 0, 0);
    if (iMIDIInDev != -1)
    {
        result = midiInOpen(&handMIDIIn, iMIDIInDev, (DWORD)(void*)MIDIInCallback, 0, CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR)
        {
            sprintf_s(buf, "midiInOpen(%d) --> ", iMIDIInDev);
            GetMidiInOpenError(result, buf, sizeof(buf));
            MessageBoxA(hwnd, buf, "ERROR:", MB_OK);
        }
        else
        {
            //bAddBuffer = true;
            midiInPrepareHeader(handMIDIIn, &midiInHdr, sizeof(MIDIHDR));
            midiInAddBuffer(handMIDIIn, &midiInHdr, sizeof(MIDIHDR));
            midiInStart(handMIDIIn);
        }
    }
    else
        MessageBox(hwnd, TEXT("No MIDI device selected!"), TEXT("OOPS!"), MB_OK);
}

static bool isValidIpAddress(char* ipAddress)
{
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ipAddress, &(sa.sin_addr));
    return result != 0;
}

static bool FindClientAndConnect()
{
    char iniFileName[MAX_PATH + 1];
    TCHAR iniFileNameTC[MAX_PATH + 1];
    bool res;
    if ((GetModuleFileNameA(0, iniFileName, MAX_PATH + 1) != 0) && (GetModuleFileName(0, iniFileNameTC, MAX_PATH + 1) != 0))
    {
        size_t len = strlen(iniFileName) - 1;
     /* iniFileNameTC[len] = */ iniFileName[len--] = 'I';
     /* iniFileNameTC[len] = */ iniFileName[len--] = 'N';
     /* iniFileNameTC[len] = */ iniFileName[len--] = 'I';
     /* TCHAR tmp[sizeof(ClientDeviceName)];
        size_t output_size;
       
        res = GetPrivateProfileString(_T("UDPMIDI"), _T("CLIENT"), _T(defaultClient), tmp, sizeof(tmp), iniFileNameTC);
        if (res)
            wcstombs_s(&output_size, ClientDeviceName, tmp, wcslen(tmp) + 1);
        else
      */    res = GetPrivateProfileStringA("UDPMIDI", "CLIENT", defaultClientDeviceName, ClientDeviceName, sizeof(ClientDeviceName), iniFileName);
    }

    struct addrinfo* addrInfoRes = NULL;
    char strIPAddr[INET_ADDRSTRLEN];
    if (isValidIpAddress(ClientDeviceName))
    {
        if (clientSocket != INVALID_SOCKET)
            closesocket(clientSocket);
        clientSocket = udpsock_client_connect(ClientDeviceName, 1999);
        SetEditBoxTXT(hwndEditSenderIP, ClientDeviceName);
        return true;
    }
    else
    {
        getaddrinfo(ClientDeviceName, "1999", 0, &addrInfoRes);
        if (addrInfoRes)
        {
            struct sockaddr_in* p = (struct sockaddr_in*)addrInfoRes->ai_addr;
            printf("%s\n", inet_ntop(AF_INET, &p->sin_addr, strIPAddr, sizeof(strIPAddr)));
            if (clientSocket != INVALID_SOCKET)
                closesocket(clientSocket);
            clientSocket = udpsock_client_connect(strIPAddr, 1999);
            SetEditBoxTXT(hwndEditSenderIP, strIPAddr);
            return true;
        }
    }
    return false;
}

static DWORD WINAPI UDPinThread(LPVOID lpParameter)
{
    HWND& hwnd = *((HWND*)lpParameter);
    byte buf[1024];
    size_t bufLen;
    while (listenerSocket != INVALID_SOCKET)
    {
        //bufLen = recv(listenerSocket, (char*)buf, sizeof(buf), 0);
        bufLen = recvfrom(listenerSocket, (char*)buf, sizeof(buf), 0, (SOCKADDR*)&SenderAddr, &SenderAddrSize);   //recvfrom gives us IP addr so we can send stuff back.  
        if (bufLen > 0)
        {
            //debugDumpBuf(buf, bufLen);
            SendMIDILongMessage(hwnd, buf, bufLen);
            SetEditBoxINT(hwndEditPacketsRX, ++RXCount);
            if (memcmp(&SenderAddr, &OLDSenderAddr, SenderAddrSize) != 0)
            {
                //SetEditBoxTXT(hwndEditSenderIP, inet_ntoa(SenderAddr.sin_addr));  // inet_ntoa depricated :( 
                inet_ntop(SenderAddr.sin_family, &SenderAddr.sin_addr, (PSTR)buf, 100);
                SetEditBoxTXT(hwndEditSenderIP, (char *) buf);
                OLDSenderAddr = SenderAddr;
                if (clientSocket != INVALID_SOCKET)
                    closesocket(clientSocket);
                clientSocket = udpsock_client_connect((char *)buf, 1999);
            }
        }
    }
    return 0;
}

static BOOL InitUDPListener(HWND hDlg)
{
    if (listenerSocket == INVALID_SOCKET)
    {    
        struct sockaddr_in serverAddr;
        short port = 1999;
        listenerSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (listenerSocket == INVALID_SOCKET)
        {
            //printf("socket failed with error %d\n", WSAGetLastError());
            SetLastErr("Socket Error", WSAGetLastError());
            MessageBox(hDlg, TEXT("socket failed!"), TEXT("OOPS!"),
                MB_OK);
            return FALSE;
        }
        // Bind the socket to any address and the specified port.
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);     
        serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        //OR, you can do 
        //serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
#ifdef USE_ASYNC_SELECT
        if (WSAAsyncSelect(listenerSocket, hDlg, WM_UDPSOCK_IN, FD_READ))
        {
            SetLastErr("WSAAsyncSelect Error", WSAGetLastError());
            MessageBox(hDlg, TEXT("WSAAsyncSelect failed!"), TEXT("OOPS!"),
                MB_OK);
            return FALSE;
        }
        else
#endif  
        if (bind(listenerSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)))
        {
            if (MessageBox(hDlg, TEXT("Socket Busy. Try again?"), TEXT("Close"),
                MB_ICONQUESTION | MB_YESNO) == IDYES)
            {
                SetLastErr("Socket Busy Error", WSAGetLastError());
                MessageBox(hDlg, TEXT("Bind failed!"), TEXT("OOPS!"),
                    MB_OK);
                return InitUDPListener(hDlg);
            }
            else
                return FALSE;
        } 
#ifndef USE_ASYNC_SELECT
        DWORD UDPinThreadID;
        handUDPinThread = CreateThread(0, 0, UDPinThread, &hDlg , 0, &UDPinThreadID);
#endif
    }
    return TRUE;
}

static void CloseUDPListner()
{
    if (listenerSocket != INVALID_SOCKET)
    {
#ifndef USE_ASYNC_SELECT
        CloseHandle(handUDPinThread);
#endif
        closesocket(listenerSocket);
        listenerSocket = INVALID_SOCKET; 
    }
}

static void AllNotesOff(HWND hwnd)
{
    return;
    for (byte channel = 0xb0; channel <= 0xbf; channel++)
    {
        SendMIDIShortMessage(hwnd, channel << 16 | 0x7b00);
        SendMIDIShortMessage(hwnd, channel << 16 | 0x4000);
    }
}

static void VerticalGradient(HDC hDC, const RECT & rect, COLORREF rgbTop, COLORREF rgbBottom)
{
    GRADIENT_RECT gradientRect = { 0, 1 };
    TRIVERTEX triVertext[2] = {
        rect.left - 1, rect.top - 1,
        GetRValue(rgbTop) << 8,
        GetGValue(rgbTop) << 8,
        GetBValue(rgbTop) << 8,
        0x0000,
        rect.right,
        rect.bottom,
        GetRValue(rgbBottom) << 8,
        GetGValue(rgbBottom) << 8,
        GetBValue(rgbBottom) << 8,
        0x0000
    };
    GradientFill(hDC, triVertext, 2, &gradientRect, 1, GRADIENT_FILL_RECT_V);
}

static INT_PTR CALLBACK DialogProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    HDC hdc;
    RECT rect;
    PAINTSTRUCT ps;
#ifdef USE_ASYNC_SELECT
    byte buf[1024];
    size_t bufLen;
#endif

    switch (uMsg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        
        case IDC_BTN_RESET:
            RXCount = 0;
            SetEditBoxINT(hwndEditPacketsRX, 0);
            SetEditBoxINT(hwndEditPacketsTX, 0);
            CloseUDPListner();
            CloseMIDIInDevice(hDlg);
            OpenMIDIInDevice(hDlg);
            CloseMIDIOutDevice(hDlg);
            OpenMIDIOutDevice(hDlg);
            InitUDPListener(hDlg);
            AllNotesOff(hDlg);
            break;
        case IDC_BTN_HIDE:
            ShowWindow(hDlg, SW_HIDE);
            return TRUE;
        case IDC_BTN_QUIT:
            SendMessage(hDlg, WM_CLOSE, 0, 0);
            return TRUE;
        case IDC_BTN_HELP:
            ShoWErrorControls();
            SendMIDIShortMessage(hDlg, 0x00403C90);
            MessageBox(hDlg, TEXT(ABOUT_DLG_TXT), TEXT("About"), MB_OK);
            break;
        case IDC_COMBO_MIDI_OUT_DEV:
            switch (HIWORD(wParam))
            {
            case CBN_SELCHANGE:
                CloseMIDIOutDevice(hDlg);
                int iMIDIOutDev = (int)SendMessage(hwndComboBoxMIDIOutDevice, CB_GETCURSEL, 0, 0);
                OpenMIDIOutDevice(hDlg);
            }
            break;
        case IDC_COMBO_MIDI_IN_DEV:
            switch (HIWORD(wParam))
            {
            case CBN_SELCHANGE:
                CloseMIDIInDevice(hDlg);
                int iMIDIInDev = (int)SendMessage(hwndComboBoxMIDIInDevice, CB_GETCURSEL, 0, 0);
                OpenMIDIInDevice(hDlg);
            }
            break;
        case IDC_BUTTON_PI:
            if(FindClientAndConnect())
                SetMT32_LCD(hDlg, MT32_Message, true);
            break;
        }
        break;

    case WM_SHELL_ICO:
        switch (lParam)
        {
        case WM_LBUTTONDOWN:
            ShowWindow(hDlg, SW_RESTORE);
            SetForegroundWindow(hDlg);
            break;
        }
        break;

    case WM_NOTIFY:
        break;

    case WM_PAINT:
        hdc = BeginPaint(hDlg, &ps);   
        GetClientRect(hDlg, &rect);
        VerticalGradient(hdc, rect, 0xff0000, 0x99999999);
        //TextOut(hdc, 0, 0, TEXT("BBond007"), 15);
        DrawIconEx(hdc, 100, 85, hIcon, 72, 78, 0, NULL, DI_NORMAL);
        EndPaint(hDlg, &ps);
        break;

#ifdef USE_ASYNC_SELECT
    case WM_UDPSOCK_IN:
        SetEditBoxINT(hwndEditPacketsRX, ++RXCount);
        bufLen = recv(listenerSocket, (char*)buf, sizeof(buf), 0);
        SendMIDILongMessage(hDlg, buf, bufLen);
        break;
#endif

    case WM_CLOSE:
        if (MessageBox(hDlg, TEXT("Close the program?"), TEXT("Close"),
            MB_ICONQUESTION | MB_YESNO) == IDYES)
        {
            DestroyWindow(hDlg);
        }
        return TRUE;

    case WM_DESTROY:
        PostQuitMessage(0);
        return TRUE;

    }
    return FALSE;
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd)
{
    HWND hDlg;
    NOTIFYICONDATA	niData;	// notify icon data
    MSG msg;
    BOOL ret;
    InitCommonControls();
    WSADATA wsaData;

    hDlg = CreateDialogParam(hInstance, MAKEINTRESOURCE(IDD_DIALOG1), 0, DialogProc, 0);
    hwndComboBoxMIDIOutDevice = GetDlgItem(hDlg, IDC_COMBO_MIDI_OUT_DEV);
    hwndComboBoxMIDIInDevice  = GetDlgItem(hDlg, IDC_COMBO_MIDI_IN_DEV);
    hwndEditPacketsRX         = GetDlgItem(hDlg, IDC_EDIT_PACKETS_RX);
    hwndEditPacketsTX         = GetDlgItem(hDlg, IDC_EDIT_PACKETS_TX);
    hwndEditUDPPort           = GetDlgItem(hDlg, IDC_EDIT_UDP_PORT);
    hwndEditLastErr           = GetDlgItem(hDlg, IDC_EDIT_LAST_ERR);
    hwndEditLastErrStr        = GetDlgItem(hDlg, IDC_EDIT_LAST_ERR_STR);
    hwndEditLastErrLbl        = GetDlgItem(hDlg, IDC_LABEL_ERROR); 
    hwndEditSenderIP          = GetDlgItem(hDlg, IDC_EDIT_IP_ADDR);  
    midiInHdr.lpData          = (LPSTR)&SysXBuffer[0];
    midiInHdr.dwBufferLength  = sizeof(SysXBuffer);
    
#ifdef DEBUG_LOGGING
    char logFileName[MAX_PATH + 1];   
    if (GetModuleFileNameA(0, logFileName, MAX_PATH + 1) != 0)
    {
        size_t len = strlen(logFileName) - 1;
        logFileName[len--] = 'G';
        logFileName[len--] = 'O';
        logFileName[len--] = 'L';
        fopen_s(&logFile, logFileName, "w");
    }
#endif

    wchar_t* wcstring = new wchar_t[sizeof(MT32_Message)];
    size_t convertedChars = 0;
    mbstowcs_s(&convertedChars, wcstring,sizeof(MT32_Message), MT32_Message, _TRUNCATE);
    SendMessage(hDlg, (UINT)WM_SETTEXT, (WPARAM)0, (LPARAM)wcstring);

    int numOutDevs = midiOutGetNumDevs();
    if (numOutDevs > 0)
    {
        for (int i = 0; i < numOutDevs; i++)
        {
            MIDIOUTCAPS myCaps;
            int result2 = midiOutGetDevCaps(i, &myCaps, sizeof(myCaps));
            SendMessage(hwndComboBoxMIDIOutDevice, (UINT)CB_ADDSTRING, (WPARAM)0, (LPARAM)myCaps.szPname);
        }
        SendMessage(hwndComboBoxMIDIOutDevice, (UINT)CB_SETCURSEL, (WPARAM)0, (LPARAM)0);
        OpenMIDIOutDevice(hDlg);
        AllNotesOff(hDlg);
    }

    int numInDevs = midiInGetNumDevs();
    if (numInDevs > 0)
    {
        for (int i = 0; i < numInDevs; i++)
        {
            MIDIINCAPS myCaps;
            int result2 = midiInGetDevCaps(i, &myCaps, sizeof(myCaps));
            SendMessage(hwndComboBoxMIDIInDevice, (UINT)CB_ADDSTRING, (WPARAM)0, (LPARAM)myCaps.szPname);
        }
        SendMessage(hwndComboBoxMIDIInDevice, (UINT)CB_SETCURSEL, (WPARAM)0, (LPARAM)0);
        OpenMIDIInDevice(hDlg);
        //AllNotesOff(hDlg);
    }

    SetEditBoxINT(hwndEditPacketsRX, 0);
    SetEditBoxINT(hwndEditPacketsTX, 0);
    SendMessage(hwndEditUDPPort, (UINT)WM_SETTEXT, (WPARAM)0, (LPARAM)TEXT("1999"));

    /* <<<--------------Tray icon-------------- >>> */
    ZeroMemory(&niData, sizeof(NOTIFYICONDATA));
    niData.cbSize = sizeof(NOTIFYICONDATA);
    niData.uID = IDI_ICON1;
    niData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    niData.hWnd = hDlg;
    niData.uCallbackMessage = WM_SHELL_ICO;
    hIcon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_ICON1),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR);
    niData.hIcon = hIcon;
    lstrcpyn(niData.szTip, _T("UDPMIDI"), sizeof(niData.szTip) / sizeof(TCHAR));
    /* ^^^--------------Tray icon--------------^^^ */
    ShowWindow(hDlg, nShowCmd);
    
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        SetLastErr("WSAStartup failed!", WSAGetLastError());
        MessageBox(hDlg, TEXT("WSAStartup failed!"), TEXT("OOPS!"),
            MB_OK);
    }
    else
        InitUDPListener(hDlg);
    
    Shell_NotifyIcon(NIM_ADD, &niData);
    //ShowWindow(hDlg, SW_RESTORE);
    
    InvalidateRect(hDlg, NULL, TRUE);
    while ((ret = GetMessage(&msg, 0, 0, 0)) != 0) 
    {
        if (ret == -1)
            return -1;
        if (!IsDialogMessage(hDlg, &msg)) 
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } 
    }
    //Cleanup trey icon...
    Shell_NotifyIcon(NIM_DELETE, &niData);
    CloseMIDIOutDevice(hDlg);
    CloseMIDIInDevice(hDlg);
    CloseUDPListner();
    WSACleanup();
#ifdef DEBUG_LOGGING
    fclose(logFile);
#endif
    return 0;
}