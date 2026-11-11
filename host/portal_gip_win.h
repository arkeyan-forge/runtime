// Windows (desktop): the Xbox One portal through Windows' own Xbox driver and Windows.Gaming.Input.Custom
// (host/portal_gip_win.cpp).
#pragma once
#include "host/portal_backend.h"
#include <memory>
#include <string>

// The backend, after waiting up to `waitMs` for Windows to hand over a portal (0: no wait); null when the route is
// unavailable (the log says why).
std::unique_ptr<PortalBackend> CreateGipPortal(unsigned waitMs);
// --portal-probe: waits for the portal, sends 'R' and waits for its answer; the status, and each step in `steps`.
std::string ProbeGipPortal(PortalProbe& probe, std::string& steps);
