#include "CommandExecutor.h"
#include <sstream>
#include <fstream>
#include "base64.h"
#include <vector>

using namespace std;


CommandExecutor::CommandExecutor() {
    startTerminal();
}

// Create pipes for cmd.exe I/O
int CommandExecutor::initPipes() {
    hStdinRead = NULL;
    hStdinWrite = NULL;
    hStdoutRead = NULL;
    hStdoutWrite = NULL;

    SECURITY_ATTRIBUTES saAttr = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    // Create stdin pipe
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &saAttr, 0)) {
		cout << "Stdin pipe creation failed. Error: " << GetLastError() << endl;
        return 1;
    }

    // Create stdout pipe
    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &saAttr, 0)) {
		cout << "Stdout pipe creation failed. Error: " << GetLastError() << endl;
        return 1;
    }

    // Ensure the write handle to stdin and read handle to stdout are not inherited.
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);
	cout << "Pipes initialized successfully." << endl;
    return 0;
}

int CommandExecutor::startTerminal() {
    // Initialize pipes
    if (initPipes() != 0) {
        return 1; // Error initializing pipes
    }
	cout << "Trying to start command terminal..." << endl;
    // Create the command process
    STARTUPINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hStdinRead;
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStdoutWrite; // Redirect stderr to stdout

    wchar_t cmdLine[] = L"cmd.exe";

	cout << "Stage 2 of command terminal creation..." << endl;
    // Create the command process
    if (!CreateProcess(L"C:\\Windows\\System32\\cmd.exe", cmdLine, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        cout << "Failed to create process. Error: " << GetLastError() << endl;
        return 1; // Error creating process
    }

    cout << "cmd.exe started with PID: " << pi.dwProcessId << endl;
    Sleep(100);

    // Close handles that are not needed in the parent process
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    cout << "Command terminal started successfully." << endl;
    return 0; // Success
}

// Close the pipes and handles
int CommandExecutor::closePipes() {
    const char* exitCommand = "exit\n";
    DWORD bytesWritten;
    WriteFile(hStdinWrite, exitCommand, strlen(exitCommand), &bytesWritten, NULL);

    if (hStdinRead) CloseHandle(hStdinRead);
    if (hStdinWrite) CloseHandle(hStdinWrite);
    if (hStdoutRead) CloseHandle(hStdoutRead);
    if (hStdoutWrite) CloseHandle(hStdoutWrite);
    hStdinRead = NULL;
    hStdinWrite = NULL;
    hStdoutRead = NULL;
    hStdoutWrite = NULL;
    cout << "Pipes closed successfully." << endl;
    return 0;
}

string CommandExecutor::executeCommand(string command) {
    DWORD bytesWritten;

    if (command.find("screenshot") == 0) {
        return captureScreenshotAndEncode();
	}
	command += "\n"; // Make sure command is actually run
	cout << "Command: " << command << endl;
    BOOL success = WriteFile(hStdinWrite, command.c_str(), command.length(), &bytesWritten, NULL);
    if (!success) {
        cout << "WriteFile failed. Error: " << GetLastError() << endl;
    }
    cout << "WriteFile succeeded. Bytes written: " << bytesWritten << endl;

    Sleep(3000);
    char buffer[4096];
    DWORD bytesRead;
    string output;
    bool firstRead = true;

    // Read the output from the command
    while (true) {
        DWORD bytesAvailable = 0;
        if (!PeekNamedPipe(hStdoutRead, NULL, 0, NULL, &bytesAvailable, NULL)) {
            cout << "PeekNamedPipe failed. Error: " << GetLastError() << endl;
            break;
        }

        if (bytesAvailable == 0) {
            Sleep(50);
            continue;
        }

        if (!ReadFile(hStdoutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) || bytesRead == 0) {
            break;
        }

        buffer[bytesRead] = '\0';
        output += buffer;

        if (output.find("### END OF OUTPUT ###") != string::npos) {
            break;
        }
    }

    // Process output
    istringstream ss(output);
    string line;
    string finalOutput;

    if (firstRead) {
        while (getline(ss, line)) {
            // Skip lines containing Windows version info or copyright
            if (line.find("Microsoft Windows") != string::npos) continue;
            if (line.find("(c) Microsoft Corporation") != string::npos) continue;
            if (line.empty()) continue; // skip blank line after banner
            if (line.find(":\\") != string::npos && line.find(">") != string::npos) continue;

            finalOutput += line + "\n";
        }
        firstRead = false;
    }
    else {
        while (getline(ss, line)) {

            // Skip prompt lines like "C:\Users\abhinav>"
            if (line.find(":\\") != string::npos && line.find(">") != string::npos) continue;

            finalOutput += line + "\n";
        }
    }

    return finalOutput;
}

string CommandExecutor::captureScreenshotAndEncode() {
    // Capture screenshot and save as BMP
    int x = GetSystemMetrics(SM_CXSCREEN);
	int y = GetSystemMetrics(SM_CYSCREEN);

    HDC hScreen = GetDC(NULL);
	HDC hDC = CreateCompatibleDC(hScreen);
	HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, x, y);
	SelectObject(hDC, hBitmap);
	BitBlt(hDC, 0, 0, x, y, hScreen, 0, 0, SRCCOPY);

    // Save bitmap to file
    BITMAP bmp;
    BITMAPFILEHEADER bf;
    BITMAPINFOHEADER bi;

    // fill bf and bi
    // Assume hBitmap is your captured HBITMAP, x and y are width and height
    GetObject(hBitmap, sizeof(BITMAP), &bmp);

    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = bmp.bmWidth;
    bi.biHeight = bmp.bmHeight;
    bi.biPlanes = 1;
    bi.biBitCount = 24; // 24-bit bitmap
    bi.biCompression = BI_RGB;
    bi.biSizeImage = ((bmp.bmWidth * 3 + 3) & ~3) * bmp.bmHeight;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;

    bf.bfType = 0x4D42; // 'BM'
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bf.bfSize = bf.bfOffBits + bi.biSizeImage;
    bf.bfReserved1 = 0;
    bf.bfReserved2 = 0;

    // Get the bitmap data
    std::vector<BYTE> bmpData(bi.biSizeImage);
    HDC hDC = GetDC(NULL);
    BITMAPINFO binfo = { 0 };
    binfo.bmiHeader = bi;
    GetDIBits(hDC, hBitmap, 0, bmp.bmHeight, bmpData.data(), &binfo, DIB_RGB_COLORS);
    ReleaseDC(NULL, hDC);


    // Write headers and data to file
    ofstream file("screenshot.bmp", std::ios::binary);
    file.write(reinterpret_cast<const char*>(&bf), sizeof(bf));
    file.write(reinterpret_cast<const char*>(&bi), sizeof(bi));
    file.write(reinterpret_cast<const char*>(bmpData.data()), bmpData.size());
    file.close();

	ifstream bmpFile("screenshot.bmp", ios::in | ios::binary);
    vector<unsigned char> buffer((istreambuf_iterator<char>(bmpFile)), {});
    bmpFile.close();


    // Encode to base64
    string base64Data = base64_encode(buffer.data(), buffer.size());
    return base64Data;
}

CommandExecutor::~CommandExecutor() {
    closePipes(); // Clean up resources
}