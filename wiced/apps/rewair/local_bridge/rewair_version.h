#pragma once

/* This number is the single source of truth for firmware and release tags. */
#define REWAIR_FW_VERSION_NUMBER "0.7.0"

#if defined( REWAIR_RELEASE_BUILD )
#define REWAIR_FW_VERSION "rewair " REWAIR_FW_VERSION_NUMBER
#else
#define REWAIR_FW_VERSION "rewair " REWAIR_FW_VERSION_NUMBER "-dev"
#endif
