#pragma once

#include "framework.h"

#include <string>

// Writes debug-only diagnostic output.
void DebugLog(const wchar_t* format, ...);

// Writes informational log output.
void InfoLog(const wchar_t* format, ...);

// Writes warning log output.
void WarningLog(const wchar_t* format, ...);

// Writes error log output.
void ErrorLog(const wchar_t* format, ...);

