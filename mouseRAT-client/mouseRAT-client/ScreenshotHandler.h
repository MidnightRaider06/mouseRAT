#include <Windows.h>
#include <vector>
#include <iostream>

using namespace std;

#pragma once
class ScreenshotHandler {

public:
	static HBITMAP captureScreenshot(void);
	static string bitmapToString(HBITMAP hBitmap);
};

