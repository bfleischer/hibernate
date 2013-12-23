/*
 * Copyright (c) 2011 Benjamin Fleischer. All rights reserved.
 *
 * Redistribution  and  use  in  source  and  binary  forms,  with  or   without
 * modification, are permitted provided that the following conditions  are  met:
 *
 * 1. Redistributions of source code must retain  the  above  copyright  notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce  the above copyright notice,
 *    this list of conditions and the following disclaimer in the  documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS  "AS  IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT  NOT  LIMITED  TO,  THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR  A  PARTICULAR  PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT  HOLDER  OR  CONTRIBUTORS  BE
 * LIABLE  FOR  ANY  DIRECT,  INDIRECT,  INCIDENTAL,  SPECIAL,   EXEMPLARY,   OR
 * CONSEQUENTIAL  DAMAGES  (INCLUDING,  BUT  NOT  LIMITED  TO,  PROCUREMENT   OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,  DATA,  OR  PROFITS;  OR  BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON  ANY  THEORY  OF  LIABILITY,  WHETHER  IN
 * CONTRACT, STRICT LIABILITY,  OR  TORT  (INCLUDING  NEGLIGENCE  OR  OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF  ADVISED  OF  THE
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
 * If the constant HIBERNATE_SLEEP is defined hibernate will sleep for
 * kSleepSeconds instead of initiating system sleep. This can be used to
 * simulate the processes of modifing the power management preferences.
 */
// #define HIBERNATE_SLEEP

#ifdef HIBERNATE_SLEEP
/* The time to sleep in seconds instead of initiating system sleep. */
# define kSleepSeconds 10
#endif

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
#define kPMAdaptPreferencesSuccess 0
/* Getting or setting the active power management profiles failed. */
#define kPMAdaptPreferencesErrorActiveProfiles 1
/* Getting or setting the custom power management preferences failed. */
#define kPMAdaptPreferencesErrorCustomPreferences 2
/* Getting information about the currently active power source failed. */
#define kPMAdaptPreferencesErrorPowerSource 3
/* Getting or setting the active power management preferences failed. */
#define kPMAdaptPreferencesErrorActivePreferences 4

int PMAdaptPreferences(CFDictionaryRef *activePMProfiles,
                       CFDictionaryRef *customPMPreferences) {
    CFStringRef feature = NULL;
    CFNumberRef value = NULL;
    IOReturn rc;

    // Get active power management profiles
    *activePMProfiles = IOPMCopyActivePowerProfiles();
    if (!*activePMProfiles) {
        return kPMAdaptPreferencesErrorActiveProfiles;
    }

    // Get custom power management preferences
    *customPMPreferences = IOPMCopyCustomPMPreferences();
    if (!*customPMPreferences) {
        CFRelease(*activePMProfiles);

        return kPMAdaptPreferencesErrorCustomPreferences;
    }

    // Get power source type
    CFTypeRef psInformantion = IOPSCopyPowerSourcesInfo();
    if (!psInformantion) {
        CFRelease(*activePMProfiles);
        CFRelease(*customPMPreferences);

        return kPMAdaptPreferencesErrorPowerSource;
    }
    CFStringRef psType = IOPSGetProvidingPowerSourceType(psInformantion);
    CFRetain(psType);

    CFRelease(psInformantion);
    if (!psType) {
        CFRelease(*activePMProfiles);
        CFRelease(*customPMPreferences);

        return kPMAdaptPreferencesErrorPowerSource;
    }

    // Get active power management preferences
    CFDictionaryRef activePMPreferences = IOPMCopyActivePMPreferences();
    if (!activePMPreferences) {
        CFRelease(*activePMProfiles);
        CFRelease(*customPMPreferences);
        CFRelease(psType);

        return kPMAdaptPreferencesErrorActivePreferences;
    }

    // Get active power management preferences for power source
    CFDictionaryRef activePMPreferencesPS = NULL;
    if (!CFDictionaryGetValueIfPresent(activePMPreferences,
                                       psType,
                                       (void *) &activePMPreferencesPS)) {
        CFRelease(*activePMProfiles);
        CFRelease(*customPMPreferences);
        CFRelease(psType);
        CFRelease(activePMPreferences);

        return kPMAdaptPreferencesErrorActivePreferences;
    }

    // Create mutable copy of active power managment preferences
    CFMutableDictionaryRef mcActivePMPreferences =
            CFDictionaryCreateMutableCopy(kCFAllocatorDefault,
                                          0,
                                          activePMPreferences);
    CFMutableDictionaryRef mcActivePMPreferencesPS =
            CFDictionaryCreateMutableCopy(kCFAllocatorDefault,
                                          0,
                                          activePMPreferencesPS);
    CFDictionarySetValue(mcActivePMPreferences,
                         psType,
                         mcActivePMPreferencesPS);
    CFRelease(activePMPreferences);

    // Set hibernate mode
    feature = CFSTR(kIOHibernateModeKey);
    if (IOPMFeatureIsAvailable(feature, psType)) {
        SInt32 hibernateMode = kHibernateMode;
        value = CFNumberCreate(kCFAllocatorDefault,
                               kCFNumberSInt32Type,
                               &hibernateMode);
        CFDictionarySetValue(mcActivePMPreferencesPS, feature, value);
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
        CFDictionarySetValue(mcActivePMPreferencesPS, feature, value);
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
        CFDictionarySetValue(mcActivePMPreferencesPS, feature, value);
        CFRelease(value);
    }
    CFRelease(feature);

    CFRelease(mcActivePMPreferencesPS);

    // Select custom power manamgment profile for power source
    CFMutableDictionaryRef mcPMActiveProfiles =
            CFDictionaryCreateMutableCopy(kCFAllocatorDefault,
                                          0,
                                          *activePMProfiles);
    int customProfile = kIOPMCustomPowerProfile;
    value = CFNumberCreate(kCFAllocatorDefault,
                           kCFNumberSInt32Type,
                           &customProfile);
    CFDictionarySetValue(mcPMActiveProfiles, psType, value);
    CFRelease(value);

    CFRelease(psType);

    // Activate adapted power management preferences
    rc = IOPMSetCustomPMPreferences(mcActivePMPreferences);
    CFRelease(mcActivePMPreferences);
    if (rc != kIOReturnSuccess) {
        CFRelease(mcPMActiveProfiles);

        return kPMAdaptPreferencesErrorCustomPreferences;
    }
    rc = IOPMSetActivePowerProfiles(mcPMActiveProfiles);
    CFRelease(mcPMActiveProfiles);
    if (rc != kIOReturnSuccess) {
        return kPMAdaptPreferencesErrorActiveProfiles;
    }

    return kPMAdaptPreferencesSuccess;
}

/* The power management preferences have been restored successfuly. */
#define kPMRestorePreferencesSuccess 0
/* The custom power manamgement preferences could not be resored. */
#define kPMRestorePreferencesErrorCustomPreferences 1
/* The active power manamgement profile could not be restored. */
#define kPMRestorePreferencesErrorActiveProfiles 2

/*
 * Restores the power management preferences to the state before the system
 * initiated sleep.
 */
int PMRestorePreferences(CFDictionaryRef activePMProfiles,
                         CFDictionaryRef customPMPreferences) {
    // Restore custom power management preferences
    if (IOPMSetCustomPMPreferences(customPMPreferences) != kIOReturnSuccess) {
        return kPMRestorePreferencesErrorCustomPreferences;
    }

    // Restore active profiles
    if (IOPMSetActivePowerProfiles(activePMProfiles) != kIOReturnSuccess) {
        return kPMRestorePreferencesErrorActiveProfiles;
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
#define kMainErrorPMAdaptPreferences 3
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
    CFDictionaryRef activePMProfiles = NULL;
    CFDictionaryRef customPMPreferences = NULL;
    rc = PMAdaptPreferences(&activePMProfiles, &customPMPreferences);
    if (rc != kPMAdaptPreferencesSuccess) {
        switch (rc) {
            case kPMAdaptPreferencesErrorActiveProfiles:
                perror("hibernate: setting active power management profiles "
                       "failed\n");
                break;
            case kPMAdaptPreferencesErrorCustomPreferences:
                perror("hiberate: setting custom power management preferences "
                       "failed\n");
                break;
            case kPMAdaptPreferencesErrorPowerSource:
                perror("hibernate: getting currently active power source type "
                       "failed\n");
                break;
            case kPMAdaptPreferencesErrorActivePreferences:
                perror("hibernate: getting active power management preferences "
                       "failed\n");
                break;
        }
        return kMainErrorPMAdaptPreferences;
    }

    // Connect to the IOPMrootDomain
    IONotificationPortRef port = NULL;
    io_object_t notifier;
    session = IORegisterForSystemPower(NULL,
                                       &port,
                                       IOPowerNotificationCallback,
                                       &notifier);
    if (!session) {
        CFRelease(customPMPreferences);
        CFRelease(activePMProfiles);

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

#ifdef HIBERNATE_SLEEP
    sleep(kSleepSeconds);
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
    rc = PMRestorePreferences(activePMProfiles, customPMPreferences);
    CFRelease(customPMPreferences);
    CFRelease(activePMProfiles);
    if (rc != kPMRestorePreferencesSuccess) {
        switch(rc) {
            case kPMRestorePreferencesErrorCustomPreferences:
                perror("hibernate: restoring custom power management "
                       "preferences failed\n");
                break;
            case kPMRestorePreferencesErrorActiveProfiles:
                perror("hibernate: restoring active power management profiles "
                       "failed\n");
                break;
        }
        return kMainErrorPMRestorePreferences;
    }

    return kMainSuccess;
}
