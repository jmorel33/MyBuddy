/**
 * @file mybuddy_version.h
 * @brief MyBuddy version macros — canonical source of truth.
 *
 * Bump only this file when releasing a new version.
 * All other MyBuddy headers include this file; never define version
 * macros elsewhere.
 *
 * The runtime string is available via MbdGetVersionString() declared in
 * mybuddy_api.h and implemented in mybuddy_impl.h.
 */
#ifndef MYBUDDY_VERSION_H
#define MYBUDDY_VERSION_H

#define MYBUDDY_VERSION_MAJOR       1
#define MYBUDDY_VERSION_MINOR       6
#define MYBUDDY_VERSION_PATCH       2
#define MYBUDDY_VERSION_REVISION    ""
#define MYBUDDY_VERSION_DESCRIPTION "Dedicated version header, MbdGetVersionString() API, full 3-OS audit"

#endif /* MYBUDDY_VERSION_H */
