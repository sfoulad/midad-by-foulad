#pragma once

// Stand-in for lib/hal/HalStorage.h. CrossPointSettings.h mentions HalFile only
// in the declaration of writeSettings(), which this test never calls, so an
// empty class is enough to keep the header self-contained on the host.
class HalFile {};
