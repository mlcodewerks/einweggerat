#pragma once
#include "core.h"

// Only the exact two-value true/false pairs used by einweggerat become checkboxes.
bool core_option_boolean(const Option &option, size_t &enabled, size_t &disabled);
void draw_core_options(Core &core, bool &open);
