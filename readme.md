# Virtio-Net for Mac OS X

## Virtio and virtio-net

[*virtio*][virtio] is an open specification for virtualised "hardware" in
virtual machines. Historically, virtual machine developers have either emulated
popular real hardware devices in order to utilise existing drivers in operating
systems, or implemented their own virtualised devices with drivers for each
supported guest operating system. Neither approach is ideal. Emulating
real hardware in software is often unnecessarily inefficient: some of the
constraints of real hardware don't apply to virtualised hardware. VM-specific
virtualised devices can be fast, but require specific driver for each supported
guest operating system. Moreover, the VM developer usually maintains the drivers,
and the specs are often not published. This prevents development of drivers for
less popular guest operating systems.

An open specification presents an
opportunity for separating the responsibilities for implementing the virtual
hardware and the drivers, and also potentially allows for greater guest
portability across different virtualisation solution.

The virtio spec (Version 0.9 as of this writing) includes a specification for a
virtualised PCI ethernet network card. Implementations for such virtual hardware
are present in Linux' KVM virtualisation solution and also in newer versions of
VirtualBox. Drivers for guests exist for Linux (in the main tree) and for
Windows.

## Motivation

VirtualBox supports virtual machines running Mac OS X (when running
on Apple hardware), but so far I have not found any drivers. Virtual machines
are great for testing and debugging kernel extensions; I've so far been unable
to connect gdb to a Mac OS X kernel running inside a VirtualBox VM with
emulated "real" ethernet cards. This is an attempt to create a driver which
supports kernel debugging for the virtio network interface, in addition to
being a good choice for general VM networking. Performance is not a priority
initially but it will be once everything is working.

## Status

So far, receiving packets appears to be working. The driver will attach
itself to any PCI devices with the virtio device and vendor ID and the network
subsystem and device class IDs. It will then initialise the device, negotiate its
supported features and allocate memory for the transmit and receive queues. It
will also output various diagnostic information including the MAC address to the
kernel log.

The driver installs an interrupt handler with a filter for ignoring uninteresting
interrupts. It populates the receive queue with blank network packets. Finally,
it exposes an ethernet interface to the kernel, which can be enabled by the user,
at which point packets can be received, but currently not transmitted.

On unloading, most resources are freed, but unused packets in the receive queue
are currently leaked.

## Next Steps

Receiving packets appears to be working, but the output queue seems to be
incorrectly set up, so we're not yet getting any packets to transmit, for some
reason. This needs to be investigated, then those packets can be added to the
appropriate queue, and finally freed once the device is done with them.

The code is also becoming quite messy. Most of the things happening in `start()`
and `stop()` should be happening in `enable()` and `disable()`, respectively.
Some functions are also very long and should be split up. Code documentation
wouldn't hurt either.

Resource leaks must be fixed, along with any error handling.

We should probably gather network statistics and expose any status changes to
the system.

Finally, there are many, many optimisation opportunities, some easier to
implement than others.


## Binaries

I'll provide binary KEXTs once the driver is actually useful. Unless you're a
developer, there is currently *nothing* useful you can do with this driver.
Until it's done, you'll need to compile it yourself. This repository contains
an XCode 4 project with which the KEXT can be built in a single step. The KEXT
should work on versions 10.5 (Leopard) through 10.7 (Lion), but so far has only
been tested on Snow Leopard. Since XCode 4 only runs on Snow Leopard and up,
you'll need to create your own XCode 3 project if you want to compile it on
Leopard.

## License

I'm making the source code for this driver available under the [zLib license][zlib],
the [3-clause BSD license][bsd3] and the [MIT License][mit]. The `virtio_ring.h`
file is adapted from the virtio spec and 3-clause BSD licensed. I've added the
MIT license in the hope that this will help inclusion into VirtualBox proper.

[virtio]: http://ozlabs.org/~rusty/virtio-spec/

[bsd3]: http://www.opensource.org/licenses/BSD-3-Clause

[zlib]: http://www.opensource.org/licenses/zLib

[mit]: http://www.opensource.org/licenses/MIT
