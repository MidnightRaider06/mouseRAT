#include "CommandExecutor.h"
#include <sstream>

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

CommandExecutor::~CommandExecutor() {
    closePipes(); // Clean up resources
}