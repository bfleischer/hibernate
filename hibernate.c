/*
 * Copyright (c) 2011-2017 Benjamin Fleischer. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/sysctl.h>

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFString.h>

#include <IOKit/IOTypes.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include "IOHibernatePrivate.h"
#include "IOPMLibPrivate.h"
#include "IOPowerSourcesPrivate.h"

/*
 * If the HIBERNATE_SIMULATE_SLEEP is enabled hibernate will sleep for
 * kSimulatedSleepSeconds instead of initiating system sleep. This can be used
 * to simulate the processes of modifing the power management preferences.
 */
#define HIBERNATE_SIMULATE_SLEEP 0

/* The time to sleep in seconds instead of initiating system sleep. */
# define kSimulatedSleepSeconds 10

/* The hibernate mode for system sleep. */
#define kHibernateMode kIOHibernateModeOn
/* The state of the standby feature during system sleep. */
#define kStandby 0
/* The state of the feature wake on local area network during system sleep. */
#define kWakeOnLAN 0
/* The time in seconds to wait before initating system sleep. */
#define kWaitBeforeSystemSleep 2
/* The time in seconds to wait after the system has powered on. */
#define kWaitAfterSystemSleep 8

/*
 * The IOPMrootDomain session used to initiate system sleep, receive sleep/wake
 * notifications and acknowledge them.
 */
io_connect_t session;
/* The run loop object. */
CFRunLoopRef loop;

/* The operating system release is supported. */
#define kCheckOSReleaseSupported 0
/* The operating system release is unsupported. */
#define kCheckOSReleaseUnsupported 1
/* The operating system release could not be determined due to an error. */
#define kCheckOSReleaseError 2

/*
 * Returns kCheckOSReleaseSupported if the operating system release is
 * supported, kCheckOSReleaseUnsupported if the release is not supported and
 * kCheckOSReleaseError if the release could not be determined due to an error.
 */
int CheckOSRelease() {
    int selector[2] = { CTL_KERN, KERN_OSRELEASE };
    size_t length;
    int release;

    // Get length of release string
    if (sysctl(selector, 2, NULL, &length, NULL, 0) == -1) {
        return kCheckOSReleaseError;
    }

    // Parse release string
    char *buffer = (char *) malloc(length * sizeof(char));
    if (sysctl(selector, 2, buffer, &length, NULL, 0) == -1) {
        return kCheckOSReleaseError;
    }
    sscanf(buffer, "%d.%*d.%*d", &release);
    free(buffer);

    // Check KERN_OSRELEASE
    if (release < 10) {
        return kCheckOSReleaseUnsupported;
    } else {
        return kCheckOSReleaseSupported;
    }
}

/* The power management preferences have been adapted to enable hibernation. */
#define kPMAlterPreferencesSuccess 0
/* Getting or setting the power management preferences failed. */
#define kPMAlterPreferencesErrorCustomPreferences 1
/* Getting information about the currently active power source failed. */
#define kPMAlterPreferencesErrorPowerSource 2
/* Getting or setting the active power management preferences failed. */
#define kPMAlterPreferencesErrorActivePreferences 3

int PMAlterPreferences(CFDictionaryRef *originalPMPreferences) {
    CFStringRef feature = NULL;
    CFNumberRef value = NULL;
    IOReturn rc;

    // Get power source type
    CFTypeRef psInformantion = IOPSCopyPowerSourcesInfo();
    if (!psInformantion) {
        return kPMAlterPreferencesErrorPowerSource;
    }
    CFStringRef psType = IOPSGetProvidingPowerSourceType(psInformantion);
    CFRetain(psType);

    CFRelease(psInformantion);
    if (!psType) {
        return kPMAlterPreferencesErrorPowerSource;
    }

    // Get active power management preferences
    CFDictionaryRef activePMPreferences = IOPMCopyPMPreferences();
    if (!activePMPreferences) {
        CFRelease(psType);

        return kPMAlterPreferencesErrorActivePreferences;
    }

    // Get active power management preferences for power source
    CFDictionaryRef activePMPreferencesPS = NULL;
    if (!CFDictionaryGetValueIfPresent(activePMPreferences,
                                       psType,
                                       (void *) &activePMPreferencesPS)) {
        CFRelease(psType);
        CFRelease(activePMPreferences);

        return kPMAlterPreferencesErrorActivePreferences;
    }

    // Create mutable copy of active power managment preferences
    CFMutableDictionaryRef mutableActivePMPreferences =
            CFDictionaryCreateMutableCopy(kCFAllocatorDefault,
                                          0,
                                          activePMPreferences);
    CFMutableDictionaryRef mutableActivePMPreferencesPS =
            CFDictionaryCreateMutableCopy(kCFAllocatorDefault,
                                          0,
                                          activePMPreferencesPS);
    CFDictionarySetValue(mutableActivePMPreferences,
                         psType,
                         mutableActivePMPreferencesPS);

    // Set hibernate mode
    feature = CFSTR(kIOHibernateModeKey);
    if (IOPMFeatureIsAvailable(feature, psType)) {
        SInt32 hibernateMode = kHibernateMode;
        value = CFNumberCreate(kCFAllocatorDefault,
                               kCFNumberSInt32Type,
                               &hibernateMode);
        CFDictionarySetValue(mutableActivePMPreferencesPS, feature, value);
        CFRelease(value);
    }
    CFRelease(feature);

    // Set standby
    feature = CFSTR(kIOPMDeepSleepEnabledKey);
    if (IOPMFeatureIsAvailable(feature, psType)) {
        SInt32 standby = kStandby;
        value = CFNumberCreate(kCFAllocatorDefault,
                               kCFNumberSInt32Type,
                               &standby);
        CFDictionarySetValue(mutableActivePMPreferencesPS, feature, value);
        CFRelease(value);
    }
    CFRelease(feature);

    // Set wake on local area network
    feature = CFSTR(kIOPMWakeOnLANKey);
    if (IOPMFeatureIsAvailable(feature, psType)) {
        SInt32 wakeOnLAN = kWakeOnLAN;
        value = CFNumberCreate(kCFAllocatorDefault,
                               kCFNumberSInt32Type,
                               &wakeOnLAN);
        CFDictionarySetValue(mutableActivePMPreferencesPS, feature, value);
        CFRelease(value);
    }
    CFRelease(feature);

    CFRelease(mutableActivePMPreferencesPS);
    CFRelease(psType);

    // Activate adapted power management preferences
    rc = IOPMSetPMPreferences(mutableActivePMPreferences);
    CFRelease(mutableActivePMPreferences);
    if (rc != kIOReturnSuccess) {
        CFRelease(activePMPreferences);
        return kPMAlterPreferencesErrorCustomPreferences;
    }

    *originalPMPreferences = activePMPreferences;
    return kPMAlterPreferencesSuccess;
}

/* The power management preferences have been restored successfuly. */
#define kPMRestorePreferencesSuccess 0
/* The custom power manamgement preferences could not be resored. */
#define kPMRestorePreferencesErrorCustomPreferences 1

/*
 * Restores the power management preferences to the state before the system
 * initiated sleep.
 */
int PMRestorePreferences(CFDictionaryRef customPMPreferences) {
    // Restore custom power management preferences
    if (IOPMSetPMPreferences(customPMPreferences) != kIOReturnSuccess) {
        return kPMRestorePreferencesErrorCustomPreferences;
    }

    return kPMRestorePreferencesSuccess;
}

/*
 * Receives sleep/wake notifications for the system from the IOPMrootDomain.
 */
void IOPowerNotificationCallback(void *context,
                                 io_service_t service,
                                 natural_t type,
                                 void *argument) {
    switch (type) {
        case kIOMessageSystemHasPoweredOn:
            CFRunLoopStop(loop);
            break;
        case kIOMessageSystemWillSleep:
            IOAllowPowerChange(session, (long) argument);
            break;
    }
}

/*
 * Requests that the system initiate sleep. Is invoked by the CFRunLoop before
 * entering the event processing loop. Requires root privileges.
 */
void RLObserverSleepSystem(CFRunLoopObserverRef observer,
                           CFRunLoopActivity activity,
                           void *context) {
    switch (IOPMSleepSystem(session)) {
        case kIOReturnSuccess:
            break;
        case kIOReturnNotPrivileged:
            perror("hibernate: must be run as root\n");
        default:
            perror("hibernate: failed to initiate system sleep\n");
            CFRunLoopStop(CFRunLoopGetCurrent());
            break;
    }
}

/* The system hibernation has been initiated successfuly. */
#define kMainSuccess 0
/* The operating system release could not be determined or is unsupported. */
#define kMainErrorOSRelease 1
/* Connection to the IOPMrootDomain failed. */
#define kMainErrorIOPMrootDomain 2
/*
 * The power management preferences could not be adapted to enable hibernation.
 */
#define kMainErrorPMAlterPreferences 3
/* The power manamgment preferences could not be restored after hibernation. */
#define kMainErrorPMRestorePreferences 4

/*
 * Initiates hibernation by adapting the power manamgement preferences,
 * initiating system sleep and restoring the previous power management
 * preferences after the system has powered on again.
 */
int main (int argc, const char *argv[]) {
    int rc;

    // Check operating system release
    rc = CheckOSRelease();
    if (rc != kCheckOSReleaseSupported) {
        switch (rc) {
            case kCheckOSReleaseUnsupported:
                perror("hibernate: operating system release unsupported\n");
                break;
            case kCheckOSReleaseError:
                perror("hibernate: getting operating system resease failed\n");
                break;
        }
        return kMainErrorOSRelease;
    }

    // Adapt power management preferences
    CFDictionaryRef originalPMPreferences = NULL;
    rc = PMAlterPreferences(&originalPMPreferences);
    if (rc != kPMAlterPreferencesSuccess) {
        switch (rc) {
            case kPMAlterPreferencesErrorCustomPreferences:
                perror("hiberate: setting custom power management preferences "
                       "failed\n");
                break;
            case kPMAlterPreferencesErrorPowerSource:
                perror("hibernate: getting currently active power source type "
                       "failed\n");
                break;
            case kPMAlterPreferencesErrorActivePreferences:
                perror("hibernate: getting active power management preferences "
                       "failed\n");
                break;
        }
        return kMainErrorPMAlterPreferences;
    }

    // Connect to the IOPMrootDomain
    IONotificationPortRef port = NULL;
    io_object_t notifier;
    session = IORegisterForSystemPower(NULL,
                                       &port,
                                       IOPowerNotificationCallback,
                                       &notifier);
    if (!session) {
        CFRelease(originalPMPreferences);

        perror("hibernate: connecting to the IOPMrootDomain failed\n");
        return kMainErrorIOPMrootDomain;
    }

    loop = CFRunLoopGetCurrent();

    // Add run loop observer to run loop to initiate sleep
    CFRunLoopObserverRef observer =
            CFRunLoopObserverCreate(kCFAllocatorDefault,
                                    kCFRunLoopEntry,
                                    false,
                                    0,
                                    RLObserverSleepSystem,
                                    NULL);
    CFRunLoopAddObserver(loop, observer, kCFRunLoopCommonModes);

    // Add run loop source to run loop to receive power notifications
    CFRunLoopSourceRef source = IONotificationPortGetRunLoopSource(port);
    CFRunLoopAddSource(loop, source, kCFRunLoopCommonModes);

    sleep(kWaitBeforeSystemSleep);

#if HIBERNATE_SIMULATE_SLEEP
    sleep(kSimulatedSleepSeconds);
#else
    // Run run loop
    CFRunLoopRun();
#endif

    sleep(kWaitAfterSystemSleep);

    // Clear run loop
    CFRunLoopRemoveObserver(loop, observer, kCFRunLoopCommonModes);
    CFRelease(observer);
    CFRunLoopRemoveSource(loop, source, kCFRunLoopCommonModes);

    // Disconnect from the IOPMrootDomain
    IODeregisterForSystemPower(&notifier);
    IOServiceClose(session);
    IONotificationPortDestroy(port);

    // Restore power management preferences
    rc = PMRestorePreferences(originalPMPreferences);
    CFRelease(originalPMPreferences);
    if (rc != kPMRestorePreferencesSuccess) {
        switch(rc) {
            case kPMRestorePreferencesErrorCustomPreferences:
                perror("hibernate: restoring custom power management "
                       "preferences failed\n");
                break;
        }
        return kMainErrorPMRestorePreferences;
    }

    return kMainSuccess;
}
