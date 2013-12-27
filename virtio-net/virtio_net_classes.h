//
//  virtio_net_classes.h
//  virtio-osx
//
//  Created by Phil Jordan on 26/12/2013.
//
//

#ifndef virtio_osx_virtio_net_classes_h
#define virtio_osx_virtio_net_classes_h

#define PJ_NAME_PREFIX eu_philjordan_driver_virtio_net_

#include "pj_name_prefix.h"

#define SSDCMultiSubrangeMemoryDescriptor PJ_PREFIXED_NAME(MultiSubrangeMemoryDescriptor)
#define PJMbufMemoryDescriptor PJ_PREFIXED_NAME(MbufMemoryDescriptor)
#define PJVirtioNet PJ_PREFIXED_NAME(VirtioEthernetController)

#ifdef __cplusplus
class SSDCMultiSubrangeMemoryDescriptor;
class PJMbufMemoryDescriptor;
class PJVirtioNet;
#endif

#endif
