// pyxis_test_hooks.h
//
// Header for the test-hook helpers defined in main.cpp under
// `-DPYXIS_TEST_HOOKS`. Declared at global scope (no namespace) so any
// translation unit can call them without ADL surprises.
//
// The implementations capture inbound messages so the harness on the
// Mac can poll T:RX and confirm round-trip. Without PYXIS_TEST_HOOKS
// these are not declared.

#pragma once

#ifdef PYXIS_TEST_HOOKS

#include <LXMF/LXMessage.h>

void pyxis_test_hook_record_rx(const ::LXMF::LXMessage& msg);

#endif
