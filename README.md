fuzzydevice
===========

fuzzydevice is a tool to create random input devices through uinput.

This is a developer tool only, expect some rough edges. It's primary purpose
is testing libinput but it may be useful in finding bugs in the kernel or
other input stacks.

fuzzydevice initializes a uinput device with a set of random `EV_foo` bits
set and proceeds to send a random number of events before removing the
device again. It does that in a loop until cancelled or something blows up.

fuzzydevice creates a new libinput context for each device and calls
`libinput_dispatch()` for the events. The events themselves are discarded,
the goal is to find libinput segfaults.

**WARNING:** fuzzydevice will send truly random events, including key
presses, mouse button clicks and pointer movements. This may result in data
being deleted, applications closed, processes cancelled, etc. It is
recommended that fuzzydevice is run in a virtual machine where it can't do
any damage to the host.

Compilation and running
-----------------------

```
$ meson builddir
$ ninja -C builddir
$ sudo ./builddir/fuzzydevice
Testing fuzzydevice-023922 (seed 1548821192 random 2089556915)
```

Then wait until something crashes.

When it crashes, fuzzydevice leaves a `.evemu` and a `.libinput` file. These
can be used to analyse the device and event sequences that caused the crash.

If the crash cannot be reproduced with the evemu recording alone (timing
issues, etc.), try to reproduce the exact device with:

```
$ sudo ./builddir/fuzzydevice --seed 1548821192 --random 2089556915
```

Other commandline options are available, see `--help`.
