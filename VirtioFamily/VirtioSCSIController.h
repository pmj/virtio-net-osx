//
//  VirtioSCSIController.h
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 21/04/2015.
//
//

#ifndef __virtio_osx__VirtioSCSIController__
#define __virtio_osx__VirtioSCSIController__

#include "VirtioDevice.h"
#include <IOKit/scsi/spi/IOSCSIParallelInterfaceController.h>

#define VirtioSCSIController eu_dennis__jordan_driver_VirtioSCSIController

class IOBufferMemoryDescriptor;
struct virtio_scsi_management_task;
class VirtioSCSIController : public IOSCSIParallelInterfaceController
{
	OSDeclareDefaultStructors(VirtioSCSIController);
protected:
	static const unsigned CONFIG_NUM_QUEUES_OFFSET = 0;
	static const unsigned CONFIG_SEG_MAX_OFFSET = 4;
	static const unsigned CONFIG_MAX_SECTORS_OFFSET = 8;
	static const unsigned CONFIG_CMD_PER_LUN_OFFSET = 12;
	static const unsigned CONFIG_EVENT_INFO_SIZE_OFFSET = 16;
	static const unsigned CONFIG_SENSE_SIZE_OFFSET = 20;
	static const unsigned CONFIG_CDB_SIZE_OFFSET = 24;
	static const unsigned CONFIG_MAX_CHANNEL_OFFSET = 28;
	static const unsigned CONFIG_MAX_TARGET_OFFSET = 30;
	static const unsigned CONFIG_MAX_LUN_OFFSET = 32;

	uint32_t active_features;
	uint16_t max_target;
	uint32_t max_task_count;
	uint32_t max_lun;
	uint32_t seg_max;
	uint32_t max_sectors;
	VirtioDevice* virtio_dev;
	OSSet* multi_subrange_MD_pool;
	OSSet* single_subrange_MD_pool;
	OSSet* ctrl_readable_pool;
	OSSet* ctrl_writable_pool;
	uint32_t max_ctrl_tmf;
	
	virtual SCSIInitiatorIdentifier	ReportInitiatorIdentifier(void) override;
	virtual SCSIDeviceIdentifier ReportHighestSupportedDeviceID(void) override;
	virtual UInt32 ReportMaximumTaskCount(void) override;
	virtual UInt32 ReportHBASpecificTaskDataSize(void) override;
	virtual UInt32 ReportHBASpecificDeviceDataSize(void) override;
	virtual bool DoesHBAPerformDeviceManagement(void) override;
	virtual bool InitializeController(void) override;
	virtual void TerminateController(void) override;
	virtual bool StartController(void) override;
	virtual void StopController(void) override;
	virtual void HandleInterruptRequest(void) override;
	virtual SCSIServiceResponse ProcessParallelTask (SCSIParallelTaskIdentifier parallelRequest) override;
	static IOReturn ProcessParallelTaskInGate(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3);
	SCSIServiceResponse ProcessParallelTaskInGate(SCSIParallelTaskIdentifier parallelRequest);

	virtual IOInterruptEventSource*	CreateDeviceInterrupt (IOInterruptEventSource::Action action, IOFilterInterruptEventSource::Filter filter, IOService* provider) override;

	static void eventCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written);

	static void parallelTaskCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written);
	void parallelTaskCompleted(SCSIParallelTaskIdentifier parallelRequest, bool device_reset);
	SCSIServiceResponse processTaskManagementFunction(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ, unsigned virtio_scsi_process_task_command);
	static IOReturn processTaskManagementFunctionInGate(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3);
	SCSIServiceResponse processTaskManagementFunctionInGate(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ, unsigned virtio_scsi_process_task_command);
	static void processTaskManagementCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written);
	void taskManagementFunctionCompleted(virtio_scsi_management_task* task, bool device_reset, uint32_t num_bytes_written);
	
public:

	virtual SCSILogicalUnitNumber ReportHBAHighestLogicalUnitNumber (void) override;
	virtual bool DoesHBASupportSCSIParallelFeature (SCSIParallelFeature theFeature) override;
	virtual bool InitializeTargetForID (SCSITargetIdentifier targetID) override;
	virtual SCSIServiceResponse	AbortTaskRequest (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ) override;
	virtual	SCSIServiceResponse AbortTaskSetRequest (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) override;
	virtual	SCSIServiceResponse ClearACARequest (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) override;
	virtual	SCSIServiceResponse ClearTaskSetRequest (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) override;
	virtual	SCSIServiceResponse LogicalUnitResetRequest (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL) override;
	virtual	SCSIServiceResponse TargetResetRequest (SCSITargetIdentifier theT) override;
	virtual void ReportHBAConstraints(OSDictionary* constraints) override;
	virtual bool DoesHBAPerformAutoSense() override;
};
#endif /* defined(__virtio_osx__VirtioSCSIController__) */

	

