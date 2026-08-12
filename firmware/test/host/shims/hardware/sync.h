// Host-test shim for pico-sdk hardware/sync.h. Tests are single-threaded;
// the barrier is a no-op.
#pragma once
#ifndef __dmb
#define __dmb() ((void)0)
#endif
