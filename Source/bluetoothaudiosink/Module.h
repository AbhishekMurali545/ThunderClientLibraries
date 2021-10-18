#pragma once

#ifndef MODULE_NAME
#define MODULE_NAME ClientLibrary_BluetoothAudioSink
#endif

#include <com/com.h>
#include <core/core.h>
#include <plugins/plugins.h>
#include <tracing/tracing.h>

#if defined(__WINDOWS__) && defined(DEVICEINFO_EXPORTS)
#undef EXTERNAL
#define EXTERNAL EXTERNAL_EXPORT
#endif
