#include "ntddk.h"

#include "fwpsk.h"
#include "mstcpip.h"
#include "ntstrsafe.h"
#include "ndis.h"


#include "analyzer.h"
#include "windbgshark_drv.h"
#include "callout.h"
#include "register.h"
#include "utils.h"

#include "debug.h"

#define PACKET_EXTRA_SIZE 0x500

void FreePendedPacket(
   IN OUT PENDED_PACKET* packet)
{
	if (packet->mdl != NULL)
	{
		IoFreeMdl(packet->mdl);
		packet->mdl = NULL;
	}

	if(packet->data != NULL)
	{
		ExFreePoolWithTag(packet->data, TAG_PENDEDPACKETDATA);
		packet->data = NULL;
	}
	
	ExFreePoolWithTag(packet, TAG_PENDEDPACKET);
}

PENDED_PACKET*
AllocateAndInitializeStreamPendedPacket(
	IN const FWPS_INCOMING_VALUES0* inFixedValues,
	IN const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
	IN FLOW_DATA* flowContext,
	IN PLARGE_INTEGER localTime,
	IN OUT FWPS_STREAM_CALLOUT_IO_PACKET0* packet)
{
	PENDED_PACKET *pendedPacket;
	UINT index = 0;
	SIZE_T bytesCopied;

	ASSERT(packet != NULL && 
		packet->streamData != NULL);

	// pendedPacket gets deleted in FreePendedPacket
	#pragma warning( suppress : 803072 )
	pendedPacket = ExAllocatePoolWithTag(
						NonPagedPool,
						sizeof(PENDED_PACKET),
						TAG_PENDEDPACKET);
	   
	if (pendedPacket == NULL)
	{
		return NULL;
	}

	RtlZeroMemory(pendedPacket, sizeof(PENDED_PACKET));


	pendedPacket->flags = packet->streamData->flags;

	if(pendedPacket->flags & FWPS_STREAM_FLAG_SEND)
	{
		index = FWPS_FIELD_STREAM_V4_IP_LOCAL_ADDRESS;
		pendedPacket->ipv4SrcAddr = 
			RtlUlongByteSwap( /* host-order -> network-order conversion */
			inFixedValues->incomingValue[index].value.uint32);

		index = FWPS_FIELD_STREAM_V4_IP_REMOTE_ADDRESS;
		pendedPacket->ipv4DstAddr = 
			RtlUlongByteSwap( /* host-order -> network-order conversion */
			inFixedValues->incomingValue[index].value.uint32);

		index = FWPS_FIELD_STREAM_V4_IP_LOCAL_PORT;
		pendedPacket->srcPort = 
			inFixedValues->incomingValue[index].value.uint16;

		index = FWPS_FIELD_STREAM_V4_IP_REMOTE_PORT;
		pendedPacket->dstPort = 
			inFixedValues->incomingValue[index].value.uint16;
	}
	else if(pendedPacket->flags & FWPS_STREAM_FLAG_RECEIVE)
	{
		index = FWPS_FIELD_STREAM_V4_IP_REMOTE_ADDRESS;
		pendedPacket->ipv4SrcAddr = 
			RtlUlongByteSwap( /* host-order -> network-order conversion */
			inFixedValues->incomingValue[index].value.uint32);

		index = FWPS_FIELD_STREAM_V4_IP_LOCAL_ADDRESS;
		pendedPacket->ipv4DstAddr = 
			RtlUlongByteSwap( /* host-order -> network-order conversion */
			inFixedValues->incomingValue[index].value.uint32);

		index = FWPS_FIELD_STREAM_V4_IP_REMOTE_PORT;
		pendedPacket->srcPort = 
			inFixedValues->incomingValue[index].value.uint16;

		index = FWPS_FIELD_STREAM_V4_IP_LOCAL_PORT;
		pendedPacket->dstPort = 
			inFixedValues->incomingValue[index].value.uint16;
	}
	
	pendedPacket->localTime.HighPart = localTime->HighPart;
	pendedPacket->localTime.LowPart = localTime->LowPart;
	myRtlTimeToSecondsSince1970(localTime, &pendedPacket->timestamp);
	
	// Closing packets are only created at flowDeleteFn
	pendedPacket->close = FALSE;

	// Protocol analyzers will change this value if needed
	pendedPacket->permitted = TRUE;

	pendedPacket->mdl = NULL;
	
	pendedPacket->dataLength = (ULONG) packet->streamData->dataLength;
	pendedPacket->allocatedBytes = pendedPacket->dataLength + PACKET_EXTRA_SIZE;

	if(pendedPacket->dataLength > 0)
	{
		// data gets deleted in FreePendedPacket
		#pragma warning( suppress : 28197 )
		pendedPacket->data = ExAllocatePoolWithTag(
							NonPagedPool,
							pendedPacket->allocatedBytes,
							TAG_PENDEDPACKETDATA);

		if (pendedPacket->data == NULL)
		{
			FreePendedPacket(pendedPacket);
			return NULL;
		}
		
		RtlZeroMemory(pendedPacket->data, pendedPacket->allocatedBytes);

		FwpsCopyStreamDataToBuffer0(
			packet->streamData, 
			pendedPacket->data, 
			pendedPacket->dataLength, 
			&bytesCopied);
	}
	
	if(flowContext != NULL)
	{
		pendedPacket->flowContext = flowContext;
	}

	return pendedPacket;
}

NTSTATUS inspectPacket(PENDED_PACKET* windbgsharkPacket)
{
	if(windbgsharkPacket->flowContext == NULL)
	{
		// This means an error with callouts
		return STATUS_UNSUCCESSFUL;
	}
	
	if(windbgsharkPacket->close)
	{
		CleanupFlowContext(windbgsharkPacket->flowContext);
		windbgsharkPacket->permitted = FALSE;

		return STATUS_SUCCESS;
	}

	if(windbgsharkPacket->flags & FWPS_STREAM_FLAG_SEND)
	{
		windbgsharkPacket->sequenceNumber = windbgsharkPacket->flowContext->localCounter;
		windbgsharkPacket->acknowledgementNumber = windbgsharkPacket->flowContext->remoteCounter;
	}
	else if(windbgsharkPacket->flags & FWPS_STREAM_FLAG_RECEIVE)
	{
		windbgsharkPacket->sequenceNumber = windbgsharkPacket->flowContext->remoteCounter;
		windbgsharkPacket->acknowledgementNumber = windbgsharkPacket->flowContext->localCounter;
	}

	// If the payload is empty, no need for parsing
	if(windbgsharkPacket->dataLength == 0)
	{
		return STATUS_UNSUCCESSFUL;
	}

	// onpacketinspect
	nop();

	// onpacketinject
	nop();

	return STATUS_SUCCESS;
}

void NTAPI StreamInjectCompletionFn(
   IN void* context,
   IN OUT NET_BUFFER_LIST* netBufferList,
   IN BOOLEAN dispatchLevel)
{
	PENDED_PACKET *packet = (PENDED_PACKET*) context;

	UNREFERENCED_PARAMETER(dispatchLevel);

	FwpsFreeNetBufferList(netBufferList);

	FreePendedPacket(packet);
}

NTSTATUS
ReinjectPendedPacket(
	IN PENDED_PACKET *packet,
	IN FLOW_DATA *flowData)
{
	NTSTATUS status;
	UINT32 flags;
	NET_BUFFER_LIST* netBufferList = NULL;
	FLOW_DATA *flowCtx;
	ULONG dataLength;

	if(packet->dataLength == 0 || packet->data == NULL)
	{
		return STATUS_UNSUCCESSFUL;
	}

	packet->mdl = IoAllocateMdl(
		packet->data,
		packet->dataLength,
		FALSE,
		FALSE,
		NULL);
	
	if (packet->mdl == NULL)
	{
		status = STATUS_NO_MEMORY;
		goto Exit;
	}

	MmBuildMdlForNonPagedPool(packet->mdl);
	
	status = FwpsAllocateNetBufferAndNetBufferList(
						gNetBufferListPool,
						0,
						0,
						packet->mdl,
						0,
						packet->dataLength,
						&netBufferList);

	if(!NT_SUCCESS(status))
	{
		goto Exit;
	}

	flags = packet->flags;
	dataLength = packet->dataLength;
	flowCtx = packet->flowContext;

	status = FwpsStreamInjectAsync(
		gInjectionHandle,
		NULL,
		0,
		flowData->flowHandle,
		gStreamCalloutIdV4,
		FWPS_LAYER_STREAM_V4,
		flags, 
		netBufferList,
		packet->dataLength,
		StreamInjectCompletionFn,
		packet);

	if (!NT_SUCCESS(status))
	{
		goto Exit;
	}

	// Keep correct sequence numbers
	if(flags & FWPS_STREAM_FLAG_SEND)
	{
		flowCtx->localCounter += dataLength;
	}
	else if(flags & FWPS_STREAM_FLAG_RECEIVE)
	{
		flowCtx->remoteCounter += dataLength;
	}

	// Ownership transferred
	netBufferList = NULL;
	packet = NULL;

	

Exit:

	if (netBufferList != NULL)
	{
		FwpsFreeNetBufferList(netBufferList);
	}

	if (packet != NULL)
	{
		FreePendedPacket(packet);
	}

	return status;
}

void thAnalyzer(IN PVOID StartContext)
{
	KLOCK_QUEUE_HANDLE packetQueueLockHandle;
	NTSTATUS ntstatus;
	BOOLEAN permitted;

	UNREFERENCED_PARAMETER(StartContext);

	for(;;)
	{
		LIST_ENTRY *listEntry;
		PENDED_PACKET *pendedPacket;
		BOOLEAN brk = FALSE;

		KeWaitForSingleObject(
			&gWorkerEvent,
			Executive, 
			KernelMode, 
			FALSE, 
			NULL);

		if (gDriverUnloading)
		{
			break;
		}

		listEntry = NULL;

		KeAcquireInStackQueuedSpinLock(
			&gPacketQueueLock,
			&packetQueueLockHandle);
		
		if (gDriverUnloading)
		{
			brk = TRUE;
		}
		else if(!IsListEmpty(&gPacketQueue))
		{
			listEntry = RemoveHeadList(&gPacketQueue);

			pendedPacket = CONTAINING_RECORD(
						listEntry,
						PENDED_PACKET,
						listEntry);
		}

		// Clear event if list is empty 
		// either after we pulled a packet from it or we didn't
		if (IsListEmpty(&gPacketQueue))
		{
			KeClearEvent(&gWorkerEvent);
		}

		KeReleaseInStackQueuedSpinLock(&packetQueueLockHandle);

		if(brk)
		{
			break;
		}

		// This is what we build the whole driver for, mate
		inspectPacket(pendedPacket);

		if(pendedPacket->permitted)
		{
			// This function flushes out the whole packet, completely taking
			// ownership and responsibility of the pendedPacket and 
			// deallocating its resources
			ntstatus = ReinjectPendedPacket(
				pendedPacket, 
				pendedPacket->flowContext);

			// Ownership transferred
			pendedPacket = NULL;
		}

		if (pendedPacket != NULL)
		{
			FreePendedPacket(pendedPacket);
		}
	}

	ASSERT(gDriverUnloading);

	// Discard all the pended packets if driver is being unloaded.
	while (!IsListEmpty(&gPacketQueue))
	{
		PENDED_PACKET *packet = NULL;
		LIST_ENTRY* listEntry = NULL;

		KeAcquireInStackQueuedSpinLock(
			&gPacketQueueLock,
			&packetQueueLockHandle);

		if (!IsListEmpty(&gPacketQueue))
		{
			listEntry = RemoveHeadList(&gPacketQueue);

			packet = CONTAINING_RECORD(
							listEntry,
							PENDED_PACKET,
							listEntry);
		}

		KeReleaseInStackQueuedSpinLock(&packetQueueLockHandle);
      
		if (packet != NULL)
		{
			FreePendedPacket(packet);
		}
	}

Exit:

	PsTerminateSystemThread(STATUS_SUCCESS);
}