Hibernate
=========

Hibernate is a simple command line program to put Mac OS X 10.6 into hibernation mode. This is archieved by adapting the power management preferences and initiating system sleep. After the system has powered on, the power management preferences are restored to their previous state.

By default the following features are adapted:

* `kIOHibernateModeKey` is set to `kIOHibernateModeOn` to enable hibernation
* `kIOPMWakeOnLANKey` is set to `0` and is thereby disabled. This switch controls the "Wake on Demand" feature as detailed by Apple in [KB HT3774](http://support.apple.com/kb/HT3774).

Particularly with portable Macs not disabling the "Wake on Demand" feature, while in hibernation mode, will lead to problems, if the Mac supports the feature. Apple states:

> Portable Macs with Wake on Demand enabled will only wake on demand if they are plugged into power, and either the built-in display is open or an external display is attached.

But the above statement does not apply to hibernation mode. When in hibernation mode, a portable Mac will wake up in regular intervals to broadcast its Bonjour services to a local Bonjour proxy (even if there is none) despite not being plugged into power or the lid being closed. To prevent this from happening, the "Wake on Demand" feature is disabled while in hibernation mode.
