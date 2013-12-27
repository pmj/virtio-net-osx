//
//  pj_name_prefix.h
//  virtio-osx
//
//  Created by Phil Jordan on 26/12/2013.
//
//

#ifndef virtio_osx_pj_name_prefix_h
#define virtio_osx_pj_name_prefix_h

#ifndef PJ_NAME_PREFIX
#error Must #define a unique, reverse-DNS-style PJ_NAME_PREFIX, e.g. com_example_driver_device_
#endif
#define PJ_PREFIXED_NAME_CONCAT2(prefix,name) prefix ## name
#define PJ_PREFIXED_NAME_CONCAT(prefix,name) PJ_PREFIXED_NAME_CONCAT2(prefix, name)
#define PJ_PREFIXED_NAME(name) PJ_PREFIXED_NAME_CONCAT(PJ_NAME_PREFIX, name)

#endif
