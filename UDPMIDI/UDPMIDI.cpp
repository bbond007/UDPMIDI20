//#define USE_ASYNC_SELECT
//#define _WINSOCK_DEPRECATED_NO_WARNINGS TRUE
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
#pragma comment(lib, "ComCtl32.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "msimg32.lib")
#define ABOUT_DLG_TXT "UDPMIDI c++ version\n\nTest release : 10/09/2021\n\nBinaryBond007@gmail.com"
#define WM_UDPSOCK_IN (WM_USER + 1)
#define WM_SHELL_ICO  (WM_USER + 2)

/* http://midi.teragonaudio.com/tech/lowmidi.htm */
static char MT32_Message[] = "<-UDPMIDI  VER 2.0->>";
static struct       sockaddr_in SenderAddr;
static struct       sockaddr_in OLDSenderAddr;
static struct       sockaddr_in client_addr;
static HICON        hIcon;
static HWND         hwndComboBoxMIDIDevice;
static HWND         hwndEditPacketsRX;
static HWND         hwndEditPacketsTX;
static HWND         hwndEditUDPPort;
static HWND         hwndEditLastErr;
static HWND         hwndEditLastErrStr;
static HWND         hwndEditLastErrLbl;
static HWND         hwndEditSenderIP;
static HMIDIOUT     handMIDIOut     = 0;
static HMIDIIN      handMIDIIn      = 0;
static unsigned int RXCount         = 0;
static unsigned int TXCount         = 0;
static HANDLE       handUDPinThread = 0;
static SOCKET       listenerSocket  = INVALID_SOCKET;
static SOCKET       clientSocket    = INVALID_SOCKET;
static int          SenderAddrSize  = sizeof(SenderAddr);

void SetEditBoxINT(HWND hwnd, int val)
{
    char buf[11];
    wchar_t text_wchar[0xff]; 
    //memset(buf, 0x00, sizeof(buf)); // ok compiler worning, i don't think this is necessacary, but whatever. 
    _itoa_s(val, buf, 11);
    size_t length = strlen(buf);      // TODO --> C6054 : string buf might not be 0 terminated? ok
    mbstowcs_s(&length, text_wchar, buf, length);
    SendMessage(hwnd, (UINT)WM_SETTEXT, (WPARAM)0, (LPARAM)text_wchar);
}

void SetEditBoxTXT(HWND hwnd, const char * val)
{
    wchar_t text_wchar[0xff];
    size_t length = strlen(val);
    mbstowcs_s(&length, text_wchar, val, length);
    SendMessage(hwnd, (UINT)WM_SETTEXT, (WPARAM)0, (LPARAM)text_wchar);
}

void ShoWErrorControls()
{
    ShowWindow(hwndEditLastErrStr, SW_SHOW);
    ShowWindow(hwndEditLastErr, SW_SHOW);
    ShowWindow(hwndEditLastErrLbl, SW_SHOW);
}

void SetLastErr(const char * err, int errNo)
{
    SetEditBoxINT(hwndEditLastErr, errNo);
    if (err != NULL)
        SetEditBoxTXT(hwndEditLastErrStr, err);
    ShoWErrorControls();
}

void SendMIDIShortMessage(HWND hwnd, u_int note)
{
    if (handMIDIOut != 0)
    {
        midiOutShortMsg(handMIDIOut, note);
    }
    else
        MessageBox(hwnd, TEXT("MIDI not started!"), TEXT("OOPS!"),
            MB_OK);
}

void SendMIDILongMessage(HWND hwnd, byte* buf, int bufLen)
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

void SetMT32_LCD(HWND hwnd, char * MT32Message)
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
    int checksum = 0;
    int MT32MessageIndex = 0;
    int MT32MessageLength = strlen(MT32Message);
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
    SendMIDILongMessage(hwnd, buf, sizeof(buf));
}
 
 void CloseMIDIDevice()
 {
     if (handMIDIOut != 0)
     {
         midiOutClose(handMIDIOut);
         handMIDIOut = 0;
         midiInClose(handMIDIIn);
         handMIDIIn = 0;
     }
}

int udpsock_client_connect(char* ipAddr, int port)
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

int udpsock_client_write(int sock, byte* buf, int bufLen)
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
    SetEditBoxINT(hwndEditPacketsTX, ++TXCount);
    return result;
}

void CALLBACK MIDIInCallback(HMIDIIN handle, UINT uMsg, DWORD dwInstance, DWORD dwParam1, DWORD dwParam2)
{
    byte shortBuf[3];
    //SetEditBoxINT(hwndEditPacketsTX, ++TXCount);
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
        udpsock_client_write(clientSocket, shortBuf, 3);
        break;

    case MIM_LONGDATA:
        break;

    case MIM_ERROR:
        break;
    case MIM_LONGERROR:
        break;
    }
}

void OpenMIDIDevice(HWND hwnd)
{
    int result;
    CloseMIDIDevice();
    int iMIDIDev = (int)SendMessage(hwndComboBoxMIDIDevice, CB_GETCURSEL, 0, 0);
    if (iMIDIDev != -1)
    {
        result = midiInOpen(&handMIDIIn, iMIDIDev, (DWORD)MIDIInCallback, 0, CALLBACK_FUNCTION);
        result = midiOutOpen(&handMIDIOut, iMIDIDev, 0, 0, 0);
        SetMT32_LCD(hwnd, MT32_Message);
    }
    else
        MessageBox(hwnd, TEXT("No MIDI device selected!"), TEXT("OOPS!"), MB_OK);
}

DWORD WINAPI UDPinThread(LPVOID lpParameter)
{
    HWND& hwnd = *((HWND*)lpParameter);
    byte buf[1024];
    int bufLen;
    while (listenerSocket != INVALID_SOCKET)
    {
        //bufLen = recv(listenerSocket, (char*)buf, sizeof(buf), 0);
        bufLen = recvfrom(listenerSocket, (char*)buf, sizeof(buf), 0, (SOCKADDR*)&SenderAddr, &SenderAddrSize);   //recvfrom gives us IP addr so we can send stuff back.  
        if (bufLen > 0)
        {
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

BOOL InitUDPListener(HWND hDlg)
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

void CloseUDPListner()
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

void AllNotesOff(HWND hwnd)
{
    return;
    for (byte channel = 0xb0; channel <= 0xbf; channel++)
    {
        SendMIDIShortMessage(hwnd, channel << 16 | 0x7b00);
        SendMIDIShortMessage(hwnd, channel << 16 | 0x4000);
    }
}

void VerticalGradient(HDC hDC, const RECT & rect, COLORREF rgbTop, COLORREF rgbBottom)
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

INT_PTR CALLBACK DialogProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    HDC hdc;
    RECT rect;
    PAINTSTRUCT ps;
#ifdef USE_ASYNC_SELECT
    byte buf[1024];
    int bufLen;
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
            OpenMIDIDevice(hDlg);
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
        case IDC_COMBO_MIDI_DEV:
            switch (HIWORD(wParam))
            {
            case CBN_SELCHANGE:
                int iMIDIDev = (int)SendMessage(hwndComboBoxMIDIDevice, CB_GETCURSEL, 0, 0);
                //SetLastErr("Open MIDI dev -->", iMIDIDev);
                CloseUDPListner();
                OpenMIDIDevice(hDlg);
                InitUDPListener(hDlg);
            }
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
        DrawIconEx(hdc, 100, 60, hIcon, 72, 78, 0, NULL, DI_NORMAL);
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
    hwndComboBoxMIDIDevice = GetDlgItem(hDlg, IDC_COMBO_MIDI_DEV);
    hwndEditPacketsRX      = GetDlgItem(hDlg, IDC_EDIT_PACKETS_RX);
    hwndEditPacketsTX      = GetDlgItem(hDlg, IDC_EDIT_PACKETS_TX);
    hwndEditUDPPort        = GetDlgItem(hDlg, IDC_EDIT_UDP_PORT);
    hwndEditLastErr        = GetDlgItem(hDlg, IDC_EDIT_LAST_ERR);
    hwndEditLastErrStr     = GetDlgItem(hDlg, IDC_EDIT_LAST_ERR_STR);
    hwndEditLastErrLbl     = GetDlgItem(hDlg, 1012);// IDC_LABEL_ERROR); why is that an error?
    hwndEditSenderIP       = GetDlgItem(hDlg, IDC_EDIT_IP_ADDR);
    int numDevs = midiOutGetNumDevs();
    if (numDevs > 0)
    {
        for (int i = 0; i < numDevs; i++)
        {
            MIDIOUTCAPS myCaps;
            int result2 = midiOutGetDevCaps(i, &myCaps, sizeof(myCaps));
            SendMessage(hwndComboBoxMIDIDevice, (UINT)CB_ADDSTRING, (WPARAM)0, (LPARAM)myCaps.szPname);
        }
        SendMessage(hwndComboBoxMIDIDevice, (UINT)CB_SETCURSEL, (WPARAM)0, (LPARAM)0);
        OpenMIDIDevice(hDlg);
        AllNotesOff(hDlg);
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
    CloseMIDIDevice();
    CloseUDPListner();
    WSACleanup();
    return 0;
}