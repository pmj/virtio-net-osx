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

So far, only the most basic device matching initialisation is working. The
driver will attach
itself to any PCI devices with the virtio device and vendor ID and the network
subsystem and device class IDs. It will then begin to initialise the device
and read its feature bits and log them to the system log.

## Next Steps

Device initialisation (as per the virtio spec) with minimal feature negotiation
needs to be completed. The virtio ring buffer data structure used for packet
transmission and receiving must be implemented and appropriately memory-mapped.
Then, we need to publish an `IOEthernetInterface` device and connect up the
appropriate methods to the virtio data structure. This also means introducing
a work loop which will handle the interrupts, etc.

## Binaries

I'll provide binary KEXTs once the driver is actually useful. Unless you're a
developer, there is currently *nothing* useful you can do with this driver.
Until it's done, you'll need to compile it yourself. This repository contains
an XCode 4 project with which the KEXT can be built in a single step for
Mac OS X 10.6 and 10.7. The code should also work without modification on earlier
SDKs, but you'll probably need to create a new project in XCode 3, which comes
with the older SDKs.

## License

I'm making the source code for this driver available under both the [zLib license][zlib]
and the the [3-clause BSD license][bsd3].

virtio: http://ozlabs.org/~rusty/virtio-spec/

bsd3: http://www.opensource.org/licenses/BSD-3-Clause

zlib: http://www.opensource.org/licenses/zLib