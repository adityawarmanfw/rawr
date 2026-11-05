#pragma once
#define PROP_VALUE_MAX 92
// No debug property override in the default host integration test.
inline int __system_property_get(const char*, char*) { return 0; }
