#pragma once

// Single source of truth for the AltaLux plugin version. The version resource
// (AltaLux.rc) and the version string reported to IrfanView (GetPlugInInfo)
// both derive from these defines; change the version here only.

#define ALTALUX_VERSION_MAJOR    3
#define ALTALUX_VERSION_MINOR    0
#define ALTALUX_VERSION_PATCH    0
#define ALTALUX_VERSION_REVISION 0

// Dotted form used in the version resource strings.
#define ALTALUX_VERSION_FILE_STRING "3.0.0.0"

// Major.minor form reported to IrfanView.
#define ALTALUX_VERSION_DISPLAY_STRING "3.00"
