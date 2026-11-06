// Attribute-path settings are captured once when the native module loads.
// Later environment changes must not alter install/remove behavior.
#pragma once
#include <stdlib.h>
#include <string.h>
static int ci_attr_config_captured_311;
static int ci_attr_noexc_config_311;
static int ci_attr_diagnostics_config_311;
void Ci_AttrPathConfigSnapshot311(void) {
  if (ci_attr_config_captured_311) return;
  const char* enabled = getenv("ATTR_PATH_NOEXC");
  const char* diagnostics = getenv("ATTR_PATH_DIAGNOSTICS");
  ci_attr_noexc_config_311 = enabled != NULL && strcmp(enabled, "1") == 0;
  ci_attr_diagnostics_config_311 = diagnostics != NULL && strcmp(diagnostics, "1") == 0;
  ci_attr_config_captured_311 = 1;
}
