/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Queue submissions without command buffers that wait for a semaphore, on
	every queue family of the first AMD GPU. wgpu makes one whenever it
	submits nothing but its fence, waiting for the binary semaphore that
	orders its submissions. RADV merges such waits through sync files, which
	amdgpu_drm does not have, and reported the device lost (the Craft apps'
	direct presentation on the Framework 13).

	Each round: a copy that signals a binary semaphore and the timeline at n,
	an empty submission that waits for the binary semaphore and signals the
	timeline at n + 1, and one that waits for the timeline at n + 1 and
	signals it at n + 2 and a fence. The fence must signal and the timeline
	read n + 2.

	vkemptywait [rounds]		default 200

	Loads the Vulkan loader at run time, so it needs no Vulkan library to
	link; cross-built with the air/OS SDK and Mesa's include/vulkan. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define FUNCTIONS(F) \
	F(EnumeratePhysicalDevices) F(GetPhysicalDeviceProperties) \
	F(GetPhysicalDeviceMemoryProperties) \
	F(GetPhysicalDeviceQueueFamilyProperties) F(CreateDevice) \
	F(GetDeviceQueue) F(AllocateMemory) F(FreeMemory) F(CreateBuffer) \
	F(DestroyBuffer) F(BindBufferMemory) F(GetBufferMemoryRequirements) \
	F(CreateCommandPool) F(AllocateCommandBuffers) F(BeginCommandBuffer) \
	F(CmdCopyBuffer) F(EndCommandBuffer) F(QueueSubmit) F(QueueWaitIdle) \
	F(CreateSemaphore) F(DestroySemaphore) F(CreateFence) F(DestroyFence) \
	F(WaitForFences) F(ResetFences) F(GetSemaphoreCounterValue) \
	F(DestroyCommandPool) F(DestroyDevice) F(DestroyInstance)

#define DECLARE(name) static PFN_vk##name vk##name;
FUNCTIONS(DECLARE)


static int
test_family(VkPhysicalDevice physical, uint32_t family, VkQueueFlags flags,
	int rounds)
{
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkPhysicalDeviceVulkan12Features features12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.timelineSemaphore = VK_TRUE,
	};
	VkDeviceCreateInfo deviceInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features12,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueInfo,
	};
	VkDevice device;
	if (vkCreateDevice(physical, &deviceInfo, NULL, &device) != VK_SUCCESS) {
		printf("family %u: vkCreateDevice failed\n", family);
		return 1;
	}
	VkQueue queue;
	vkGetDeviceQueue(device, family, 0, &queue);

	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(physical, &memory);
	VkBufferCreateInfo bufferInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = 65536,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
			| VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	VkBuffer buffer;
	vkCreateBuffer(device, &bufferInfo, NULL, &buffer);
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, buffer, &requirements);
	uint32_t type = 0;
	while (type < memory.memoryTypeCount
		&& (requirements.memoryTypeBits & (1u << type)) == 0)
		type++;
	VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = type,
	};
	VkDeviceMemory bufferMemory;
	vkAllocateMemory(device, &allocateInfo, NULL, &bufferMemory);
	vkBindBufferMemory(device, buffer, bufferMemory, 0);

	VkCommandPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = family,
	};
	VkCommandPool pool;
	vkCreateCommandPool(device, &poolInfo, NULL, &pool);
	VkCommandBufferAllocateInfo commandInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands;
	vkAllocateCommandBuffers(device, &commandInfo, &commands);
	VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};
	vkBeginCommandBuffer(commands, &beginInfo);
	VkBufferCopy region = { .srcOffset = 0, .dstOffset = 32768, .size = 32768 };
	vkCmdCopyBuffer(commands, buffer, buffer, 1, &region);
	vkEndCommandBuffer(commands);

	VkSemaphoreCreateInfo binaryInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	VkSemaphore binary;
	vkCreateSemaphore(device, &binaryInfo, NULL, &binary);
	VkSemaphoreTypeCreateInfo timelineType = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};
	VkSemaphoreCreateInfo timelineInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &timelineType,
	};
	VkSemaphore timeline;
	vkCreateSemaphore(device, &timelineInfo, NULL, &timeline);
	VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	vkCreateFence(device, &fenceInfo, NULL, &fence);

	int failed = 0;
	uint64_t value = 0;
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	for (int round = 0; round < rounds && !failed; round++) {
		/* the copy: signals the binary semaphore and the timeline at n */
		uint64_t copyValues[2] = { 0, ++value };
		VkSemaphore copySignals[2] = { binary, timeline };
		VkTimelineSemaphoreSubmitInfo copyTimeline = {
			.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
			.signalSemaphoreValueCount = 2,
			.pSignalSemaphoreValues = copyValues,
		};
		VkSubmitInfo copy = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = &copyTimeline,
			.commandBufferCount = 1,
			.pCommandBuffers = &commands,
			.signalSemaphoreCount = 2,
			.pSignalSemaphores = copySignals,
		};
		/* nothing, after the binary semaphore: the timeline at n + 1 */
		uint64_t waitBinaryValue = 0, emptyValue = ++value;
		VkTimelineSemaphoreSubmitInfo emptyTimeline = {
			.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
			.waitSemaphoreValueCount = 1,
			.pWaitSemaphoreValues = &waitBinaryValue,
			.signalSemaphoreValueCount = 1,
			.pSignalSemaphoreValues = &emptyValue,
		};
		VkSubmitInfo empty = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = &emptyTimeline,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &binary,
			.pWaitDstStageMask = &stage,
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &timeline,
		};
		/* nothing, after the timeline at n + 1: n + 2 and the fence */
		uint64_t lastWait = emptyValue, lastValue = ++value;
		VkTimelineSemaphoreSubmitInfo lastTimeline = {
			.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
			.waitSemaphoreValueCount = 1,
			.pWaitSemaphoreValues = &lastWait,
			.signalSemaphoreValueCount = 1,
			.pSignalSemaphoreValues = &lastValue,
		};
		VkSubmitInfo last = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = &lastTimeline,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &timeline,
			.pWaitDstStageMask = &stage,
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &timeline,
		};

		VkResult result = vkQueueSubmit(queue, 1, &copy, VK_NULL_HANDLE);
		if (result == VK_SUCCESS)
			result = vkQueueSubmit(queue, 1, &empty, VK_NULL_HANDLE);
		if (result == VK_SUCCESS)
			result = vkQueueSubmit(queue, 1, &last, fence);
		if (result == VK_SUCCESS)
			result = vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull);
		uint64_t counter = 0;
		if (result == VK_SUCCESS)
			result = vkGetSemaphoreCounterValue(device, timeline, &counter);
		if (result != VK_SUCCESS || counter != lastValue) {
			printf("family %u (flags 0x%x): round %d: result %d, timeline %llu,"
				" wanted %llu\n", family, flags, round, result,
				(unsigned long long)counter, (unsigned long long)lastValue);
			failed = 1;
		}
		vkResetFences(device, 1, &fence);
	}
	if (!failed)
		printf("family %u (flags 0x%x): %d rounds, timeline %llu\n", family,
			flags, rounds, (unsigned long long)value);

	vkQueueWaitIdle(queue);
	vkDestroyFence(device, fence, NULL);
	vkDestroySemaphore(device, timeline, NULL);
	vkDestroySemaphore(device, binary, NULL);
	vkDestroyCommandPool(device, pool, NULL);
	vkDestroyBuffer(device, buffer, NULL);
	vkFreeMemory(device, bufferMemory, NULL);
	vkDestroyDevice(device, NULL);
	return failed;
}


int
main(int argc, char** argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 200;
	void* loader = dlopen("libvulkan.so.1", RTLD_NOW);
	if (loader == NULL) {
		fprintf(stderr, "no Vulkan loader: %s\n", dlerror());
		return 1;
	}
	PFN_vkGetInstanceProcAddr getProc
		= (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
	PFN_vkCreateInstance createInstance
		= (PFN_vkCreateInstance)getProc(NULL, "vkCreateInstance");

	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkemptywait",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo instanceInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	VkInstance instance;
	if (createInstance(&instanceInfo, NULL, &instance) != VK_SUCCESS) {
		fprintf(stderr, "vkCreateInstance failed\n");
		return 1;
	}
#define LOAD(name) vk##name = (PFN_vk##name)getProc(instance, "vk" #name);
	FUNCTIONS(LOAD)

	VkPhysicalDevice devices[8];
	uint32_t count = 8;
	vkEnumeratePhysicalDevices(instance, &count, devices);
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties properties;
	for (uint32_t i = 0; i < count; i++) {
		vkGetPhysicalDeviceProperties(devices[i], &properties);
		if (properties.vendorID == 0x1002) {
			physical = devices[i];
			break;
		}
	}
	if (physical == VK_NULL_HANDLE) {
		fprintf(stderr, "no AMD GPU\n");
		return 1;
	}
	printf("device: %s\n", properties.deviceName);

	VkQueueFamilyProperties families[16];
	uint32_t familyCount = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families);
	int failed = 0;
	for (uint32_t family = 0; family < familyCount; family++) {
		VkQueueFlags flags = families[family].queueFlags;
		if ((flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT
				| VK_QUEUE_TRANSFER_BIT)) == 0)
			continue;
		failed |= test_family(physical, family, flags, rounds);
	}
	vkDestroyInstance(instance, NULL);
	printf("%s\n", failed ? "FAIL" : "PASS");
	return failed;
}
