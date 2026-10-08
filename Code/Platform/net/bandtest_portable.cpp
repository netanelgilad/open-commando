// Bandwidth probe replacement: browsers can't open raw ICMP sockets, so the
// probe always reports that and the game uses the configured bandwidth.
#include "BandTest.h"

BANDTEST_API unsigned long Detect_Bandwidth(unsigned long, unsigned long, int, int &failure_code,
                                            unsigned long &downstream, unsigned long api_version,
                                            BandtestSettingsStruct *, char *) {
  downstream = 0;
  failure_code = api_version == BANDTEST_API_VERSION ? BANDTEST_NO_RAW_SOCKET_PERMISSION : BANDTEST_WRONG_API_VERSION;
  return 0;
}
