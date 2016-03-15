//
//  VirtioSCSIController.cpp
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 21/04/2015.
//
//

#include "VirtioSCSIController.h"
#include "../virtio-net/SSDCMultiSubrangeMemoryDescriptor.h"
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>



OSDefineMetaClassAndStructors(VirtioSCSIController, IOSCSIParallelInterfaceController);

namespace VirtioSCSIControllerFeatures
{
	enum VirtioSCSIControllerFeatureBits
	{
		VIRTIO_SCSI_F_INOUT = (1u << 0),
		VIRTIO_SCSI_F_HOTPLUG = (1u << 1),
		VIRTIO_SCSI_F_CHANGE = (1u << 2),
		VIRTIO_SCSI_F_T10_PI = (1u << 3),
	};
	
	static const uint32_t SUPPORTED_FEATURES =
		VirtioSCSIControllerFeatures::VIRTIO_SCSI_F_INOUT
		| VirtioSCSIControllerFeatures::VIRTIO_SCSI_F_HOTPLUG
		| VirtioDeviceGenericFeature::VIRTIO_F_RING_INDIRECT_DESC;
}

enum virtio_scsi_event_type
{
	VIRTIO_SCSI_T_TRANSPORT_RESET = 1,
	
};

enum virtio_scsi_reset_event_reason
{
	VIRTIO_SCSI_EVT_RESET_HARD = 0,
	VIRTIO_SCSI_EVT_RESET_RESCAN = 1,
	VIRTIO_SCSI_EVT_RESET_REMOVED = 2,
};

enum virtio_scsi_process_task_command
{
	VIRTIO_SCSI_T_TMF_ABORT_TASK = 0,
	VIRTIO_SCSI_T_TMF_ABORT_TASK_SET = 1,
	VIRTIO_SCSI_T_TMF_CLEAR_ACA = 2,
	VIRTIO_SCSI_T_TMF_CLEAR_TASK_SET = 3,
	VIRTIO_SCSI_T_TMF_I_T_NEXUS_RESET = 4,
	VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET = 5,
	VIRTIO_SCSI_T_TMF_QUERY_TASK = 6,
	VIRTIO_SCSI_T_TMF_QUERY_TASK_SET = 7,
};

enum virtio_scsi_command_specific_response_values
{
	VIRTIO_SCSI_S_FUNCTION_COMPLETE = 0,
	VIRTIO_SCSI_S_FUNCTION_SUCCEEDED = 10,
	VIRTIO_SCSI_S_FUNCTION_REJECTED = 11,

};

struct virtio_scsi_ctrl_tmf_dev_readable
{
	uint32_t type;
	uint32_t subtype;
	uint8_t lun[8];
	uint64_t id;
};
struct virtio_scsi_ctrl_tmf_dev_writeable
{
	uint8_t response;
};

struct virtio_scsi_event
{
	uint32_t event;
	uint8_t lun[8];
	uint32_t reason;
};

bool VirtioSCSIController::InitializeController(void)
{
	VirtioDevice* virtio = OSDynamicCast(VirtioDevice, this->GetProvider());
	if(virtio == nullptr)
	{
		return false;
	}
	
	virtio->resetDevice();
	//kprintf("VirtioBlockDevice::start() resetDevice\n");
	
	uint32_t dev_features = virtio->supportedFeatures();
	uint32_t use_features = dev_features & VirtioSCSIControllerFeatures::SUPPORTED_FEATURES;
	this->active_features = use_features;
	//kprintf("VirtioBlockDevice::start() use Features\n");

	bool ok = virtio->requestFeatures(use_features);
	if (!ok)
	{
		virtio->failDevice();
		return false;
	}
	//kprintf("VirtioBlockDevice::start() bool ok\n");


//read out max target field from config area
// negotiate hotplug features
//	read out max lun in config area
// read seg_max config field - determine size of indirect descriptor tables and set kiomaximumsegmentCountREad and write to seg max
// read max_sectors and use as kioMaximumBlockCountRead/ write
	
	
	/*static const unsigned CONFIG_NUM_QUEUES_OFFSET = 0;
	static const unsigned CONFIG_SEG_MAX_OFFSET = 4;
	static const unsigned CONFIG_MAX_SECTORS_OFFSET = 8;
	static const unsigned CONFIG_CMD_PER_LUN_OFFSET = 12;
	static const unsigned CONFIG_EVENT_INFO_SIZE_OFFSET = 16;
	static const unsigned CONFIG_SENSE_SIZE_OFFSET = 20;
	static const unsigned CONFIG_CDB_SIZE_OFFSET = 24;
	static const unsigned CONFIG_MAX_CHANNEL_OFFSET = 28;
	static const unsigned CONFIG_MAX_TARGET_OFFSET = 30;
	static const unsigned CONFIG_MAX_LUN_OFFSET = 32;*/
	
	uint32_t num_queues = virtio->readDeviceConfig32LETransitional(CONFIG_NUM_QUEUES_OFFSET);
	this->seg_max = virtio->readDeviceConfig32LETransitional(CONFIG_SEG_MAX_OFFSET);
	this->max_sectors = virtio->readDeviceConfig32LETransitional(CONFIG_MAX_SECTORS_OFFSET);
	uint32_t cmd_per_lun = virtio->readDeviceConfig32LETransitional(CONFIG_CMD_PER_LUN_OFFSET);
	uint32_t event_info_size = virtio->readDeviceConfig32LETransitional(CONFIG_EVENT_INFO_SIZE_OFFSET);
	uint32_t sense_size = virtio->readDeviceConfig32LETransitional(CONFIG_SENSE_SIZE_OFFSET);
	uint32_t cdb_size = virtio->readDeviceConfig32LETransitional(CONFIG_CDB_SIZE_OFFSET);
	uint16_t max_channel = virtio->readDeviceConfig16LETransitional(CONFIG_MAX_CHANNEL_OFFSET);
	this->max_target = virtio->readDeviceConfig16LETransitional(CONFIG_MAX_TARGET_OFFSET);
	this->max_lun = virtio->readDeviceConfig32LETransitional(CONFIG_MAX_LUN_OFFSET);


	IOLog("VirtioSCSIController::InitializeController num_queues = %u \nseg_max = %u \nmax_sectors = %u \ncmd_per_lun = %u \nevent_info_size = %u \nsense_size = %u \ncdb_size = %u \nmax_channel = %u \nmax_target = %u \nmax_lun = %u \n", num_queues, seg_max, max_sectors, cmd_per_lun, event_info_size, sense_size, cdb_size, max_channel, this->max_target, this->max_lun);
	virtio->writeDeviceConfig32LETransitional(CONFIG_CDB_SIZE_OFFSET, kSCSICDBSize_Maximum);

	unsigned queue_sizes[3] = {0,0,0};
	unsigned request_queue_segs = 2 + seg_max;
	if (use_features & VirtioSCSIControllerFeatures::VIRTIO_SCSI_F_INOUT)
	{
		request_queue_segs += seg_max;
	}
	const unsigned indirect_desc_per_request[3] = {2, 0, request_queue_segs};
	IOReturn result = virtio->setupVirtqueues(3, nullptr, queue_sizes, indirect_desc_per_request);
	if (result != kIOReturnSuccess)
	{
		virtio->failDevice();
		virtio->close(this);
		return false;
	}

	this->max_task_count = min(cmd_per_lun, queue_sizes[2]);
	
	this->virtio_dev = virtio;
	
	for(unsigned i = 0; i < queue_sizes[1]; i++)
	{
		IOBufferMemoryDescriptor* eventBuffer = IOBufferMemoryDescriptor::inTaskWithOptions(
			kernel_task, kIODirectionIn | kIOMemoryPhysicallyContiguous, event_info_size, alignof(uint32_t));
		VirtioCompletion completion = { &eventCompleted, this, eventBuffer };
		IOReturn res = virtio->submitBuffersToVirtqueue(1, nullptr, eventBuffer, completion);
		if(res != kIOReturnSuccess)
		{
			eventBuffer->release();
			break;
		}
	}
	this->multi_subrange_MD_pool = OSSet::withCapacity(this->max_task_count);
	this->single_subrange_MD_pool = OSSet::withCapacity(this->max_task_count);
	for(unsigned i = 0; i < this->max_task_count; i++)
	{
		SSDCMultiSubrangeMemoryDescriptor* multi_md =
			SSDCMultiSubrangeMemoryDescriptor::withDescriptorRanges(nullptr, 0, kIODirectionNone, false);
		this->multi_subrange_MD_pool->setObject(multi_md);
		multi_md->release();
		IOSubMemoryDescriptor* single_md = IOSubMemoryDescriptor::withSubRange(nullptr, 0, 0, kIODirectionNone);
		this->single_subrange_MD_pool->setObject(single_md);
		single_md->release();
	}
	
	this->max_ctrl_tmf = queue_sizes[0];
	this->ctrl_readable_pool = OSSet::withCapacity(this->max_ctrl_tmf);
	this->ctrl_writable_pool = OSSet::withCapacity(this->max_ctrl_tmf);

	for(unsigned i = 0; i < this->max_ctrl_tmf; i++)
	{
		IOBufferMemoryDescriptor* ctrl_readable = IOBufferMemoryDescriptor::inTaskWithOptions(
			kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionOut, sizeof(virtio_scsi_ctrl_tmf_dev_readable), alignof(virtio_scsi_ctrl_tmf_dev_readable));
		this->ctrl_readable_pool->setObject(ctrl_readable);
		IOBufferMemoryDescriptor* ctrl_writable = IOBufferMemoryDescriptor::inTaskWithOptions(
			kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionIn, sizeof(virtio_scsi_ctrl_tmf_dev_writeable), alignof(virtio_scsi_ctrl_tmf_dev_writeable));
		this->ctrl_writable_pool->setObject(ctrl_writable);
		ctrl_readable->release();
		ctrl_writable->release();
	}
	
	
	virtio->startDevice(nullptr, nullptr, this->GetWorkLoop());
	IOLog("VirtioSCSIController::InitializeController -> startDevice\n");
	return true;
}


void VirtioSCSIController::eventCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written)
{
	IOBufferMemoryDescriptor* eventBuffer = static_cast<IOBufferMemoryDescriptor*>(ref);
	VirtioSCSIController* controller = static_cast<VirtioSCSIController*>(target);
	
	if(device_reset)
	{
		IOLog("VirtioSCSIController::eventCompleted -> device reset\n");

		eventBuffer->release();
	}
	else
	{
		virtio_scsi_event* event = static_cast<virtio_scsi_event*>(eventBuffer->getBytesNoCopy());
		IOLog("VirtioSCSIController::eventCompleted -> event (%u)\n", event->event);

		if(event->event == VIRTIO_SCSI_T_TRANSPORT_RESET)
		{
			IOLog("VirtioSCSIController::eventCompleted -> event - transport reset, reason %u, lun %02x %02x %02x %02x  %02x %02x %02x %02x\n",
				event->reason,
				event->lun[0], event->lun[1], event->lun[2], event->lun[3],
				event->lun[4], event->lun[5], event->lun[6], event->lun[7]);

			if(event->reason == VIRTIO_SCSI_EVT_RESET_RESCAN)
			{
				controller->CreateTargetForID(event->lun[1]);
			}
			else if(event->reason == VIRTIO_SCSI_EVT_RESET_REMOVED)
			{
				controller->DestroyTargetForID(event->lun[1]);
			}
			else if(event->reason == VIRTIO_SCSI_EVT_RESET_HARD)
			{
				//controller
			}
		}
		VirtioCompletion completion = { &eventCompleted, controller, eventBuffer };
		IOReturn res = controller->virtio_dev->submitBuffersToVirtqueue(1, nullptr, eventBuffer, completion);
		if(res != kIOReturnSuccess)
		{
			eventBuffer->release();
		}
	}
}

SCSIInitiatorIdentifier	VirtioSCSIController::ReportInitiatorIdentifier(void)
{
	IOLog("VirtioSCSIController::ReportInitiatorIdentifier\n");

	return this->max_target + 1;
}

SCSIDeviceIdentifier VirtioSCSIController::ReportHighestSupportedDeviceID(void)
{
	IOLog("VirtioSCSIController::ReportHighestSupportedDeviceID\n");
	return this->max_target;
}


UInt32 VirtioSCSIController::ReportMaximumTaskCount(void)
{
	IOLog("VirtioSCSIController::ReportMaximumTaskCount\n");
	return this->max_task_count;
}

struct virtio_scsi_req_cmd_to_device
{
	uint8_t lun[8];
	uint64_t id;
	uint8_t task_attr;
	uint8_t prio;
	uint8_t crn;
	uint8_t cdb[kSCSICDBSize_Maximum];
}__attribute__((packed));
_Static_assert(sizeof(virtio_scsi_req_cmd_to_device) == 35, "");

struct virtio_scsi_req_cmd_from_device
{
	uint32_t sense_len;
	uint32_t residual;
	uint16_t status_qualifier;
	uint8_t status;
	uint8_t response;
	union
	{
		uint8_t sense_bytes[96];
		SCSI_Sense_Data sense_data;
	};
};

struct virtio_scsi_task
{
	IOSubMemoryDescriptor* sub_md;
	SSDCMultiSubrangeMemoryDescriptor* multi_md;
	SSDCMemoryDescriptorSubrange subrange[2];
	virtio_scsi_req_cmd_to_device to_device;
	virtio_scsi_req_cmd_from_device from_device;
};

UInt32 VirtioSCSIController::ReportHBASpecificTaskDataSize(void)
{
	//IOLog("VirtioSCSIController::ReportHBASpecificTaskDataSize\n");
	//cannot be 0
	
	UInt32 struct_size = sizeof(virtio_scsi_task);
	return struct_size;
}

UInt32 VirtioSCSIController::ReportHBASpecificDeviceDataSize(void)
{
	//IOLog("VirtioSCSIController::ReportHBASpecificDeviceDataSize\n");

	return 0;
}

bool VirtioSCSIController::DoesHBAPerformDeviceManagement(void)
{
	IOLog("VirtioSCSIController::DoesHBAPerformDeviceManagement\n");

	if (this->active_features & VirtioSCSIControllerFeatures::VIRTIO_SCSI_F_HOTPLUG)
	{
		//yes - return true we have to tell what devices available
		IOLog("VirtioSCSIController::DoesHBAPerformDeviceManagement hotplug enabled\n");
		return true;
	}

	IOLog("VirtioSCSIController::DoesHBAPerformDeviceManagement hotplug disabled\n");

	return false;
}


SCSILogicalUnitNumber VirtioSCSIController::ReportHBAHighestLogicalUnitNumber (void)
{
	IOLog("VirtioSCSIController::ReportHBAHighestLogicalUnitNumber\n");

	return this->max_lun;
}
bool VirtioSCSIController::DoesHBASupportSCSIParallelFeature (SCSIParallelFeature theFeature)
{
	IOLog("VirtioSCSIController::DoesHBASupportSCSIParallelFeature\n");

	return false;
}

bool VirtioSCSIController::StartController(void)
{
	IOLog("VirtioSCSIController::StartController\n");
	
	if (this->active_features & VirtioSCSIControllerFeatures::VIRTIO_SCSI_F_HOTPLUG)
	{
		for (unsigned i = 0; i < this->max_target; ++i)
		{
			this->CreateTargetForID(i);
		}
	}

	return true;
}

static void virtio_scsi_lun_bytes_from_target_lun(SCSILogicalUnitBytes* dest, SCSITargetIdentifier target, SCSILogicalUnitNumber lun);

static OSObject* item_from_pool (OSSet* pool)
{
	OSObject* item = pool->getAnyObject();
	if (item == nullptr)
		return nullptr;
	item->retain();
	pool->removeObject(item);
	return item;
}

static void return_item_to_pool (OSObject* item, OSSet* pool)
{
	pool->setObject(item);
	item->release();
}

SCSIServiceResponse VirtioSCSIController::ProcessParallelTask(SCSIParallelTaskIdentifier parallelRequest)
{
	IOCommandGate* gate = this->GetCommandGate();
	SCSIServiceResponse response;
	gate->runAction(ProcessParallelTaskInGate, parallelRequest, &response);
	return response;
}

IOReturn VirtioSCSIController::ProcessParallelTaskInGate(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
	VirtioSCSIController* me = static_cast<VirtioSCSIController*>(owner);
	*static_cast<SCSIServiceResponse*>(arg1) = me->ProcessParallelTaskInGate(static_cast<SCSIParallelTaskIdentifier>(arg0));
	return kIOReturnSuccess;
}

SCSIServiceResponse VirtioSCSIController::ProcessParallelTaskInGate(SCSIParallelTaskIdentifier parallelRequest)
{
	//IOLog("VirtioSCSIController::ProcessParallelTask\n");
	IOMemoryDescriptor* request_buffer = this->GetHBADataDescriptor(parallelRequest);
	virtio_scsi_task* task = static_cast<virtio_scsi_task*>(this->GetHBADataPointer(parallelRequest));
	
	SCSITargetIdentifier target = this->GetTargetIdentifier(parallelRequest);
	SCSILogicalUnitNumber lun = this->GetLogicalUnitNumber(parallelRequest);
	virtio_scsi_lun_bytes_from_target_lun(&task->to_device.lun, target, lun);
	
	task->to_device.id = this->GetTaggedTaskIdentifier(parallelRequest);
	task->to_device.task_attr = this->GetTaskAttribute(parallelRequest);
	task->to_device.prio = 0;
	task->to_device.crn = 0;
	this->GetCommandDescriptorBlock(parallelRequest, &task->to_device.cdb);
	/*
	kprintf("VirtioSCSIController::ProcessParallelTask LUN=%02x %02x %02x %02x  %02x %02x %02x %02x, id=%016llx\n",
		task->to_device.lun[0],
		task->to_device.lun[1],
		task->to_device.lun[2],
		task->to_device.lun[3],
		task->to_device.lun[4],
		task->to_device.lun[5],
		task->to_device.lun[6],
		task->to_device.lun[7],
		task->to_device.id);
	*/
	
	IOMemoryDescriptor* data_buffer = this->GetDataBuffer(parallelRequest);
	uint64_t buffer_offset = this->GetDataBufferOffset(parallelRequest);
	uint64_t data_size =
		min(
			this->GetRequestedDataTransferCount(parallelRequest),
			data_buffer == nullptr ? 0 : (data_buffer->getLength() - buffer_offset));
	uint8_t direction = this->GetDataTransferDirection(parallelRequest);
	
	SSDCMultiSubrangeMemoryDescriptor* multi_md = static_cast<SSDCMultiSubrangeMemoryDescriptor*>(item_from_pool(this->multi_subrange_MD_pool));
	
	IOSubMemoryDescriptor* sub_md = static_cast<IOSubMemoryDescriptor*>(item_from_pool(this->single_subrange_MD_pool));
	
	IOMemoryDescriptor* to_device;
	IOMemoryDescriptor* from_device;
	
	if(direction == kSCSIDataTransfer_FromInitiatorToTarget)
	{
		//Out
		task->subrange[0].offset = offsetof(virtio_scsi_task, to_device);
		task->subrange[0].md = request_buffer;
		task->subrange[0].length = sizeof(virtio_scsi_req_cmd_to_device);
		
		task->subrange[1].offset = buffer_offset;
		task->subrange[1].md = data_buffer;
		task->subrange[1].length = data_size;
		
		multi_md->initWithDescriptorRanges(task->subrange, 2, kIODirectionOut, false);
		to_device = multi_md;
		
		sub_md->initSubRange(request_buffer, offsetof(virtio_scsi_task, from_device), sizeof(virtio_scsi_req_cmd_from_device), kIODirectionIn);
		
		from_device = sub_md;
	}
	else if(direction == kSCSIDataTransfer_FromTargetToInitiator)
	{
		//In
		
		sub_md->initSubRange(request_buffer, offsetof(virtio_scsi_task, to_device), sizeof(virtio_scsi_req_cmd_to_device), kIODirectionOut);
		to_device = sub_md;
		
		task->subrange[0].offset = offsetof(virtio_scsi_task, from_device);
		task->subrange[0].md = request_buffer;
		task->subrange[0].length = sizeof(virtio_scsi_req_cmd_from_device);
		
		task->subrange[1].offset = buffer_offset;
		task->subrange[1].md = data_buffer;
		task->subrange[1].length = data_size;
		
		multi_md->initWithDescriptorRanges(task->subrange, 2, kIODirectionIn, false);
		from_device = multi_md;

	}
	//kSCSIDataTransfer_NoDataTransfer
	else
	{
		task->subrange[0].offset = offsetof(virtio_scsi_task, to_device);
		task->subrange[0].md = request_buffer;
		task->subrange[0].length = sizeof(virtio_scsi_req_cmd_to_device);
		
		multi_md->initWithDescriptorRanges(task->subrange, 1, kIODirectionOut, false);
		to_device = multi_md;
		
		sub_md->initSubRange(request_buffer, offsetof(virtio_scsi_task, from_device), sizeof(virtio_scsi_req_cmd_from_device), kIODirectionIn);
		
		from_device = sub_md;
	}
	
	task->sub_md = sub_md;
	task->multi_md = multi_md;
	
	VirtioCompletion my_completion = { &parallelTaskCompleted, this, parallelRequest };
	IOReturn result = this->virtio_dev->submitBuffersToVirtqueue(2, to_device, from_device, my_completion);
	
	if(result == kIOReturnSuccess)
	{
		return kSCSIServiceResponse_Request_In_Process;
	}
	
	multi_md->initWithDescriptorRanges(nullptr, 0, kIODirectionNone, false);
	return_item_to_pool(multi_md, this->multi_subrange_MD_pool);

	sub_md->initSubRange(nullptr, 0, 0, kIODirectionNone);
	return_item_to_pool(sub_md, this->single_subrange_MD_pool);
	
	return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
}
void VirtioSCSIController::parallelTaskCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written)
{
	VirtioSCSIController* me = static_cast<VirtioSCSIController*>(target);
	SCSIParallelTaskIdentifier parallelRequest = static_cast<SCSIParallelTaskIdentifier>(ref);
	me->parallelTaskCompleted(parallelRequest, device_reset);

}

bool VirtioSCSIController::DoesHBAPerformAutoSense()
{
	return true;
}

static const uint8_t VIRTIO_SCSI_S_OK = 0;
static const uint8_t VIRTIO_SCSI_S_BAD_TARGET = 3;

void VirtioSCSIController::parallelTaskCompleted(SCSIParallelTaskIdentifier parallelRequest, bool device_reset)
{
	virtio_scsi_task* task = static_cast<virtio_scsi_task*>(this->GetHBADataPointer(parallelRequest));
	
	task->multi_md->initWithDescriptorRanges(nullptr, 0, kIODirectionNone, false);
	return_item_to_pool(task->multi_md, this->multi_subrange_MD_pool);
	task->sub_md->initSubRange(nullptr, 0, 0, kIODirectionNone);
	return_item_to_pool(task->sub_md, this->single_subrange_MD_pool);

	SCSITaskStatus completionStatus;
	SCSIServiceResponse serviceResponse;
	if(!device_reset)
	{
		uint8_t response = task->from_device.response;
		completionStatus = static_cast<SCSITaskStatus>(task->from_device.status);
		
		uint64_t bytes_transferred = 0;
		if(response == VIRTIO_SCSI_S_OK)
		{
			serviceResponse = kSCSIServiceResponse_TASK_COMPLETE;
		
			//copying sense data back into the task
			this->SetAutoSenseData(parallelRequest, &task->from_device.sense_data, task->from_device.sense_len);
		
			if (completionStatus == kSCSITaskStatus_GOOD)
			{
				uint64_t data_size = this->GetRequestedDataTransferCount(parallelRequest);
				bytes_transferred = data_size - task->from_device.residual;
			}
		}
		else if (response == VIRTIO_SCSI_S_BAD_TARGET)
		{
			kprintf("VirtioSCSIController::parallelTaskCompleted Error response is %u, status %u - target does not exist\n", response, completionStatus);
			serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
			completionStatus = kSCSITaskStatus_DeviceNotPresent;
			this->DestroyTargetForID(this->GetTargetIdentifier(parallelRequest));
		}
		else
		{
			kprintf("VirtioSCSIController::parallelTaskCompleted Error response is %u, status %u\n", response, completionStatus);
			serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
		}
		this->SetRealizedDataTransferCount(parallelRequest, bytes_transferred);
	}
	else
	{
		kprintf("VirtioSCSIController::parallelTaskCompleted device reset\n");
		serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
		completionStatus = kSCSITaskStatus_DeviceNotPresent;
	}
	this->CompleteParallelTask(parallelRequest, completionStatus, serviceResponse);
}

static SCSITargetIdentifier virtio_scsi_target_from_lun_bytes(SCSILogicalUnitBytes lun_bytes)
{
	return lun_bytes[1];
}
static SCSILogicalUnitNumber virtio_scsi_lun_from_lun_bytes(SCSILogicalUnitBytes lun_bytes)
{
	return (static_cast<SCSILogicalUnitNumber>(lun_bytes[2]) << 8) | lun_bytes[3];
}

static void virtio_scsi_lun_bytes_from_target_lun(SCSILogicalUnitBytes* dest, SCSITargetIdentifier target, SCSILogicalUnitNumber lun)
{
	(*dest)[0] = 1;
	(*dest)[1] = static_cast<uint8_t>(target);
	(*dest)[2] = (lun >> 8) & 0xff;
	(*dest)[3] = lun & 0xff;
	bzero(&(*dest)[4], 4);
}

static const uint32_t VIRTIO_SCSI_T_TMF = 0;

struct virtio_scsi_management_task
{
	IOBufferMemoryDescriptor* readable;
	IOBufferMemoryDescriptor* writable;
	SCSITargetIdentifier theT;
	SCSILogicalUnitNumber theL;
	SCSITaggedTaskIdentifier theQ;
	unsigned virtio_scsi_process_task_command;
};

struct virtio_scsi_processTaskManagementFunction_args
{
	SCSITargetIdentifier theT;
	SCSILogicalUnitNumber theL;
	SCSITaggedTaskIdentifier theQ;
	unsigned virtio_scsi_process_task_command;
};

SCSIServiceResponse	VirtioSCSIController::processTaskManagementFunction (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ, unsigned virtio_scsi_process_task_command)
{
	IOCommandGate* gate = this->GetCommandGate();
	SCSIServiceResponse response;
	virtio_scsi_processTaskManagementFunction_args args = { theT, theL, theQ, virtio_scsi_process_task_command };
	gate->runAction(processTaskManagementFunctionInGate, &args, &response);
	return response;
}

IOReturn VirtioSCSIController::processTaskManagementFunctionInGate(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
	VirtioSCSIController* me = static_cast<VirtioSCSIController*>(owner);
	virtio_scsi_processTaskManagementFunction_args& args = *static_cast<virtio_scsi_processTaskManagementFunction_args*>(arg0);
	*static_cast<SCSIServiceResponse*>(arg1) =
		me->processTaskManagementFunctionInGate(args.theT, args.theL, args.theQ, args.virtio_scsi_process_task_command);
	return kIOReturnSuccess;
}

SCSIServiceResponse VirtioSCSIController::processTaskManagementFunctionInGate(SCSITargetIdentifier theT, SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ, unsigned virtio_scsi_process_task_command)
{
	virtio_scsi_management_task* task = static_cast<virtio_scsi_management_task*>(
		IOMallocAligned(sizeof(virtio_scsi_management_task), alignof(virtio_scsi_management_task)));
	
	IOBufferMemoryDescriptor* device_readable = static_cast<IOBufferMemoryDescriptor*>(item_from_pool(this->ctrl_readable_pool));
	IOBufferMemoryDescriptor* device_writable = static_cast<IOBufferMemoryDescriptor*>(item_from_pool(this->ctrl_writable_pool));
	
	virtio_scsi_ctrl_tmf_dev_readable* ctrl_tmf = static_cast<virtio_scsi_ctrl_tmf_dev_readable*>(device_readable->getBytesNoCopy());
	ctrl_tmf->type = VIRTIO_SCSI_T_TMF;
	ctrl_tmf->subtype = virtio_scsi_process_task_command;
	virtio_scsi_lun_bytes_from_target_lun(&ctrl_tmf->lun, theT, theL);
	ctrl_tmf->id = theQ;

	task->readable = device_readable;
	task->writable = device_writable;
	task->theT = theT;
	task->theL = theL;
	task->theQ = theQ;
	task->virtio_scsi_process_task_command = virtio_scsi_process_task_command;
	
	VirtioCompletion my_completion = { &processTaskManagementCompleted, this, task };
	IOReturn result = this->virtio_dev->submitBuffersToVirtqueue(0, device_readable, device_writable, my_completion);
	
	if(result == kIOReturnSuccess)
	{
		return kSCSIServiceResponse_Request_In_Process;
	}
	
	IOFreeAligned(task, sizeof(*task));
	return_item_to_pool(device_readable, this->ctrl_readable_pool);
	return_item_to_pool(device_writable, this->ctrl_writable_pool);

	return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
}

void VirtioSCSIController::processTaskManagementCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written)
{
	VirtioSCSIController* me = static_cast<VirtioSCSIController*>(target);
	virtio_scsi_management_task* task = static_cast<virtio_scsi_management_task*>(ref);
	me->taskManagementFunctionCompleted(task, device_reset, num_bytes_written);
}
void VirtioSCSIController::taskManagementFunctionCompleted(virtio_scsi_management_task* task, bool device_reset, uint32_t num_bytes_written)
{
	IOBufferMemoryDescriptor* device_writable = task->writable;
	virtio_scsi_ctrl_tmf_dev_writeable* writable_struct = static_cast<virtio_scsi_ctrl_tmf_dev_writeable*>(device_writable->getBytesNoCopy());
	
	SCSIServiceResponse scsi_response;
	
	
	if (device_reset)
	{
		scsi_response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
	}
	else
	{
		if(writable_struct->response == VIRTIO_SCSI_S_FUNCTION_COMPLETE || writable_struct->response == VIRTIO_SCSI_S_FUNCTION_SUCCEEDED)
		{
			scsi_response = kSCSIServiceResponse_FUNCTION_COMPLETE;
		}
		
		else if(writable_struct->response == VIRTIO_SCSI_S_FUNCTION_REJECTED)
		{
			scsi_response = kSCSIServiceResponse_FUNCTION_REJECTED;
		}
		else
		{
			scsi_response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
		}
	}

	return_item_to_pool(task->readable, this->ctrl_readable_pool);
	return_item_to_pool(device_writable, this->ctrl_writable_pool);
	
	switch (task->virtio_scsi_process_task_command)
	{
	case VIRTIO_SCSI_T_TMF_ABORT_TASK:
		this->CompleteAbortTask(task->theT, task->theL, task->theQ, scsi_response);
		break;
	
	case VIRTIO_SCSI_T_TMF_ABORT_TASK_SET:
		this->CompleteAbortTaskSet(task->theT, task->theL, scsi_response);
		break;
	
	case VIRTIO_SCSI_T_TMF_CLEAR_ACA:
		this->CompleteClearACA(task->theT, task->theL, scsi_response);
		break;
	
	case VIRTIO_SCSI_T_TMF_CLEAR_TASK_SET:
		this->CompleteClearTaskSet(task->theT, task->theL, scsi_response);
		break;
	
	case VIRTIO_SCSI_T_TMF_I_T_NEXUS_RESET:
		this->CompleteTargetReset(task->theT, scsi_response);
		break;
	
	case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
		this->CompleteLogicalUnitReset(task->theT, task->theL, scsi_response);
		break;
	

	default:
		break;
	}
	
	IOFreeAligned(task, sizeof(*task));

}

SCSIServiceResponse	VirtioSCSIController::AbortTaskRequest (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL, SCSITaggedTaskIdentifier theQ)
{
	IOLog("VirtioSCSIController::AbortTaskRequest\n");
	
	SCSIServiceResponse response = processTaskManagementFunction (theT, theL, theQ, VIRTIO_SCSI_T_TMF_ABORT_TASK);
	return response;

}

SCSIServiceResponse VirtioSCSIController::AbortTaskSetRequest (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
	IOLog("VirtioSCSIController::AbortTaskSetRequest\n");
	SCSIServiceResponse response = processTaskManagementFunction (theT, theL, 0, VIRTIO_SCSI_T_TMF_ABORT_TASK_SET);
	return response;

}

SCSIServiceResponse VirtioSCSIController::ClearACARequest (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
	IOLog("VirtioSCSIController::ClearACARequest\n");
	SCSIServiceResponse response = processTaskManagementFunction (theT, theL, 0, VIRTIO_SCSI_T_TMF_CLEAR_ACA);
	return response;


}

SCSIServiceResponse VirtioSCSIController::ClearTaskSetRequest (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
	IOLog("VirtioSCSIController::ClearTaskSetRequest\n");
	SCSIServiceResponse response = processTaskManagementFunction (theT, theL, 0, VIRTIO_SCSI_T_TMF_CLEAR_TASK_SET);
	return response;

}

SCSIServiceResponse VirtioSCSIController::LogicalUnitResetRequest (SCSITargetIdentifier theT, SCSILogicalUnitNumber theL)
{
	IOLog("VirtioSCSIController::LogicalUnitResetRequest\n");
	SCSIServiceResponse response = processTaskManagementFunction (theT, theL, 0, VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET);
	return response;

}

SCSIServiceResponse VirtioSCSIController::TargetResetRequest (SCSITargetIdentifier theT)
{
	IOLog("VirtioSCSIController::TargetResetRequest\n");
	SCSIServiceResponse response = processTaskManagementFunction (theT, 0, 0, VIRTIO_SCSI_T_TMF_I_T_NEXUS_RESET);
	return response;

}


void VirtioSCSIController::StopController(void)
{
	IOLog("VirtioSCSIController::StopController\n");

}

void VirtioSCSIController::TerminateController(void)
{
	IOLog("VirtioSCSIController::TerminateController\n");

	if(this->virtio_dev != nullptr)
	{
		this->virtio_dev->failDevice();
		this->virtio_dev = nullptr;
	}
	
	//clear pools
	OSSafeReleaseNULL(this->single_subrange_MD_pool);
	OSSafeReleaseNULL(this->multi_subrange_MD_pool);
	OSSafeReleaseNULL(this->ctrl_readable_pool);
	OSSafeReleaseNULL(this->ctrl_writable_pool);

}


IOInterruptEventSource*	VirtioSCSIController::CreateDeviceInterrupt(IOInterruptEventSource::Action action, IOFilterInterruptEventSource::Filter filter, IOService* provider)
{
	IOLog("VirtioSCSIController::CreateDeviceInterrupt\n");

	return nullptr;
}

bool VirtioSCSIController::InitializeTargetForID (SCSITargetIdentifier targetID)
{
	//IOLog("VirtioSCSIController::InitializeTargetForID\n");

	return true;
}


void VirtioSCSIController::ReportHBAConstraints(OSDictionary* constraints)
{
	IOLog("VirtioSCSIController::ReportHBAConstraints\n");

	uint32_t number64 = 64;
	uint32_t number1 = 1;
	
	constraints->setObject(kIOMaximumSegmentCountReadKey, OSNumber::withNumber(this->seg_max, 32));
	constraints->setObject(kIOMaximumSegmentCountWriteKey, OSNumber::withNumber(this->seg_max, 32));
	constraints->setObject(kIOMaximumSegmentByteCountReadKey, OSNumber::withNumber(UINT32_MAX, 32));
	constraints->setObject(kIOMaximumSegmentByteCountWriteKey, OSNumber::withNumber(UINT32_MAX, 32));

	constraints->setObject(kIOMinimumSegmentAlignmentByteCountKey, OSNumber::withNumber(number1, 32));
	constraints->setObject(kIOMaximumSegmentAddressableBitCountKey, OSNumber::withNumber(number64, 32));
	constraints->setObject(kIOMinimumHBADataAlignmentMaskKey, OSNumber::withNumber(UINT64_MAX, 64));
	constraints->setObject(kIOHierarchicalLogicalUnitSupportKey, kOSBooleanTrue);
	constraints->setObject(kIOMaximumBlockCountReadKey, OSNumber::withNumber(this->max_sectors, 32));
	constraints->setObject(kIOMaximumBlockCountWriteKey, OSNumber::withNumber(this->max_sectors, 32));
	
}

void VirtioSCSIController::HandleInterruptRequest(void)
{
	IOLog("VirtioSCSIController::HandleInterruptRequest\n");

}


