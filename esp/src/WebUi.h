// Small web UI: upload / delete / run G-code, upload a new GBA ROM, live status.
#pragma once
#include "CncModule.h"

void webUiBegin(CncModule &cnc, Grbl &grbl, LinkMaster &link);
void webUiLoop();
