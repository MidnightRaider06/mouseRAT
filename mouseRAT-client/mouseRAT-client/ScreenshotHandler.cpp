#include "ScreenshotHandler.h"

using namespace std;

HBITMAP ScreenshotHandler::captureScreenshot(void) {
    int x1, y1, x2, y2, w, h;

    // get screen dimensions
    x1 = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y1 = GetSystemMetrics(SM_YVIRTUALSCREEN);
    x2 = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    y2 = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    w = x2 - x1;
    h = y2 - y1;

    // copy screen to bitmap
    HDC     hScreen = GetDC(NULL);
    HDC     hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, w, h);
    HGDIOBJ old_obj = SelectObject(hDC, hBitmap);
    BOOL    bRet = BitBlt(hDC, 0, 0, w, h, hScreen, x1, y1, SRCCOPY);

    // save bitmap to clipboard
    OpenClipboard(NULL);
    EmptyClipboard();
    SetClipboardData(CF_BITMAP, hBitmap);
    CloseClipboard();

    // clean up
    SelectObject(hDC, old_obj);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);
    DeleteObject(hBitmap);
}

string ScreenshotHandler::bitmapToString(HBITMAP hBitmap) {
    if (hBitmap == NULL) {
        return "";
    }

    BITMAP bm;
    if (!GetObject(hBitmap, sizeof(BITMAP), &bm)) {
        std::cerr << "GetObject failed.\n";
        return "";
    }

    // Calculate the stride (row size aligned to 4 bytes)
    int stride = ((bm.bmWidth * bm.bmBitsPixel + 31) / 32) * 4;
    DWORD dwBmpSize = stride * bm.bmHeight;

    // Setup BITMAPINFOHEADER
    BITMAPINFOHEADER bi = { 0 };
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = bm.bmWidth;
    bi.biHeight = bm.bmHeight; // positive for bottom-up DIB
    bi.biPlanes = 1;
    bi.biBitCount = bm.bmBitsPixel;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = dwBmpSize;

    // Buffer for pixel data
    std::vector<unsigned char> buffer(dwBmpSize);

    // Retrieve the actual bits
    HDC hdc = GetDC(NULL);
    if (!GetDIBits(hdc, hBitmap, 0, bm.bmHeight, buffer.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS)) {
        cerr << "GetDIBits failed.\n";
        ReleaseDC(NULL, hdc);
        return "";
    }
    ReleaseDC(NULL, hdc);

    // Create BITMAPFILEHEADER
    BITMAPFILEHEADER bf = { 0 };
    bf.bfType = 0x4D42; // 'BM'
    bf.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + dwBmpSize;
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    // Combine everything into one buffer
    std::vector<unsigned char> fullBitmapData;
    fullBitmapData.resize(bf.bfSize);

    memcpy(fullBitmapData.data(), &bf, sizeof(BITMAPFILEHEADER));
    memcpy(fullBitmapData.data() + sizeof(BITMAPFILEHEADER), &bi, sizeof(BITMAPINFOHEADER));
    memcpy(fullBitmapData.data() + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER), buffer.data(), dwBmpSize);

    // Encode to Base64
    //return base64_encode(fullBitmapData.data(), fullBitmapData.size());

	// Return empty string for now until base64_encode is implemented
    return "";
}