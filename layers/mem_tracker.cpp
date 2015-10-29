/*
 *
 * Copyright (C) 2015 LunarG, Inc.
 * Copyright (C) 2015 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <list>
#include <map>
#include <vector>
using namespace std;

#include "vk_loader_platform.h"
#include "vk_dispatch_table_helper.h"
#include "vk_struct_string_helper_cpp.h"
#include "mem_tracker.h"
#include "vk_layer_config.h"
#include "vk_layer_extension_utils.h"
#include "vk_layer_table.h"
#include "vk_layer_data.h"
#include "vk_layer_logging.h"
static LOADER_PLATFORM_THREAD_ONCE_DECLARATION(g_initOnce);

// WSI Image Objects bypass usual Image Object creation methods.  A special Memory
// Object value will be used to identify them internally.
static const VkDeviceMemory MEMTRACKER_SWAP_CHAIN_IMAGE_KEY = (VkDeviceMemory)(-1);

struct layer_data {
    debug_report_data *report_data;
    std::vector<VkDbgMsgCallback> logging_callback;
    VkLayerDispatchTable* device_dispatch_table;
    VkLayerInstanceDispatchTable* instance_dispatch_table;
    bool wsi_enabled;
    uint64_t currentFenceId;
    // Maps for tracking key structs related to MemTracker state
    unordered_map<VkCommandBuffer,    MT_CB_INFO>           cbMap;
    unordered_map<VkDeviceMemory, MT_MEM_OBJ_INFO>      memObjMap;
    unordered_map<VkFence,        MT_FENCE_INFO>        fenceMap;    // Map fence to fence info
    unordered_map<VkQueue,        MT_QUEUE_INFO>        queueMap;
    unordered_map<VkSwapchainKHR, MT_SWAP_CHAIN_INFO*>  swapchainMap;
    unordered_map<VkSemaphore,    MtSemaphoreState>     semaphoreMap;
    // Images and Buffers are 2 objects that can have memory bound to them so they get special treatment
    unordered_map<uint64_t, MT_OBJ_BINDING_INFO>        imageMap;
    unordered_map<uint64_t, MT_OBJ_BINDING_INFO>        bufferMap;

    layer_data() :
        report_data(nullptr),
        device_dispatch_table(nullptr),
        instance_dispatch_table(nullptr),
        wsi_enabled(false),
        currentFenceId(1)
    {};
};

static unordered_map<void *, layer_data *> layer_data_map;

static VkPhysicalDeviceMemoryProperties memProps;

// TODO : This can be much smarter, using separate locks for separate global data
static int globalLockInitialized = 0;
static loader_platform_thread_mutex globalLock;

#define MAX_BINDING 0xFFFFFFFF

static MT_OBJ_BINDING_INFO* get_object_binding_info(layer_data* my_data, uint64_t handle, VkDbgObjectType type)
{
    MT_OBJ_BINDING_INFO* retValue = NULL;
    switch (type)
    {
        case VK_OBJECT_TYPE_IMAGE:
        {
            auto it = my_data->imageMap.find(handle);
            if (it != my_data->imageMap.end())
                return &(*it).second;
            break;
        }
        case VK_OBJECT_TYPE_BUFFER:
        {
            auto it = my_data->bufferMap.find(handle);
            if (it != my_data->bufferMap.end())
                return &(*it).second;
            break;
        }
    }
    return retValue;
}

template layer_data *get_my_data_ptr<layer_data>(
        void *data_key,
        std::unordered_map<void *, layer_data *> &data_map);

// Add new queue for this device to map container
static void add_queue_info(layer_data* my_data, const VkQueue queue)
{
    MT_QUEUE_INFO* pInfo   = &my_data->queueMap[queue];
    pInfo->lastRetiredId   = 0;
    pInfo->lastSubmittedId = 0;
}

static void delete_queue_info_list(layer_data* my_data)
{
    // Process queue list, cleaning up each entry before deleting
    my_data->queueMap.clear();
}

static void add_swap_chain_info(layer_data* my_data,
    const VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR* pCI)
{
    MT_SWAP_CHAIN_INFO* pInfo = new MT_SWAP_CHAIN_INFO;
    memcpy(&pInfo->createInfo, pCI, sizeof(VkSwapchainCreateInfoKHR));
    my_data->swapchainMap[swapchain] = pInfo;
}

// Add new CBInfo for this cb to map container
static void add_cmd_buf_info(layer_data* my_data,
    const VkCommandBuffer cb)
{
    my_data->cbMap[cb].commandBuffer = cb;
}

// Return ptr to Info in CB map, or NULL if not found
static MT_CB_INFO* get_cmd_buf_info(layer_data* my_data,
    const VkCommandBuffer cb)
{
    auto item = my_data->cbMap.find(cb);
    if (item != my_data->cbMap.end()) {
        return &(*item).second;
    } else {
        return NULL;
    }
}

static void add_object_binding_info(layer_data* my_data, const uint64_t handle, const VkDbgObjectType type, const VkDeviceMemory mem)
{
    switch (type)
    {
        // Buffers and images are unique as their CreateInfo is in container struct
        case VK_OBJECT_TYPE_BUFFER:
        {
            auto pCI = &my_data->bufferMap[handle];
            pCI->mem = mem;
            break;
        }
        case VK_OBJECT_TYPE_IMAGE:
        {
            auto pCI = &my_data->imageMap[handle];
            pCI->mem = mem;
            break;
        }
    }
}

static void add_object_create_info(layer_data* my_data, const uint64_t handle, const VkDbgObjectType type, const void* pCreateInfo)
{
    // TODO : For any CreateInfo struct that has ptrs, need to deep copy them and appropriately clean up on Destroy
    switch (type)
    {
        // Buffers and images are unique as their CreateInfo is in container struct
        case VK_OBJECT_TYPE_BUFFER:
        {
            auto pCI = &my_data->bufferMap[handle];
            memset(pCI, 0, sizeof(MT_OBJ_BINDING_INFO));
            memcpy(&pCI->create_info.buffer, pCreateInfo, sizeof(VkBufferCreateInfo));
            break;
        }
        case VK_OBJECT_TYPE_IMAGE:
        {
            auto pCI = &my_data->imageMap[handle];
            memset(pCI, 0, sizeof(MT_OBJ_BINDING_INFO));
            memcpy(&pCI->create_info.image, pCreateInfo, sizeof(VkImageCreateInfo));
            break;
        }
        // Swap Chain is very unique, use my_data->imageMap, but copy in
        // SwapChainCreatInfo's usage flags and set the mem value to a unique key. These is used by
        // vkCreateImageView and internal MemTracker routines to distinguish swap chain images
        case VK_OBJECT_TYPE_SWAPCHAIN_KHR:
        {
            auto pCI = &my_data->imageMap[handle];
            memset(pCI, 0, sizeof(MT_OBJ_BINDING_INFO));
            pCI->mem = MEMTRACKER_SWAP_CHAIN_IMAGE_KEY;
            pCI->create_info.image.usage =
                const_cast<VkSwapchainCreateInfoKHR*>(static_cast<const VkSwapchainCreateInfoKHR *>(pCreateInfo))->imageUsageFlags;
            break;
        }
    }
}

// Add a fence, creating one if necessary to our list of fences/fenceIds
static VkBool32 add_fence_info(layer_data* my_data,
    VkFence   fence,
    VkQueue   queue,
    uint64_t *fenceId)
{
    VkBool32 skipCall = VK_FALSE;
    *fenceId = my_data->currentFenceId++;

    // If no fence, create an internal fence to track the submissions
    if (fence != VK_NULL_HANDLE) {
        my_data->fenceMap[fence].fenceId = *fenceId;
        my_data->fenceMap[fence].queue   = queue;
        // Validate that fence is in UNSIGNALED state
        VkFenceCreateInfo* pFenceCI = &(my_data->fenceMap[fence].createInfo);
        if (pFenceCI->flags & VK_FENCE_CREATE_SIGNALED_BIT) {
            skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_FENCE, (uint64_t) fence, 0, MEMTRACK_INVALID_FENCE_STATE, "MEM",
                           "Fence %#" PRIxLEAST64 " submitted in SIGNALED state.  Fences must be reset before being submitted", (uint64_t) fence);
        }
    } else {
        // TODO : Do we need to create an internal fence here for tracking purposes?
    }
    // Update most recently submitted fence and fenceId for Queue
    my_data->queueMap[queue].lastSubmittedId = *fenceId;
    return skipCall;
}

// Remove a fenceInfo from our list of fences/fenceIds
static void delete_fence_info(layer_data* my_data,
    VkFence fence)
{
    my_data->fenceMap.erase(fence);
}

// Record information when a fence is known to be signalled
static void update_fence_tracking(layer_data* my_data,
    VkFence fence)
{
    auto fence_item = my_data->fenceMap.find(fence);
    if (fence_item != my_data->fenceMap.end()) {
        MT_FENCE_INFO *pCurFenceInfo = &(*fence_item).second;
        VkQueue queue = pCurFenceInfo->queue;
        auto queue_item = my_data->queueMap.find(queue);
        if (queue_item != my_data->queueMap.end()) {
            MT_QUEUE_INFO *pQueueInfo = &(*queue_item).second;
            if (pQueueInfo->lastRetiredId < pCurFenceInfo->fenceId) {
                pQueueInfo->lastRetiredId = pCurFenceInfo->fenceId;
            }
        }
    }

    // Update fence state in fenceCreateInfo structure
    auto pFCI = &(my_data->fenceMap[fence].createInfo);
    pFCI->flags = static_cast<VkFenceCreateFlags>(pFCI->flags | VK_FENCE_CREATE_SIGNALED_BIT);
}

// Helper routine that updates the fence list for a specific queue to all-retired
static void retire_queue_fences(layer_data* my_data,
    VkQueue queue)
{
    MT_QUEUE_INFO *pQueueInfo = &my_data->queueMap[queue];
    // Set queue's lastRetired to lastSubmitted indicating all fences completed
    pQueueInfo->lastRetiredId = pQueueInfo->lastSubmittedId;
}

// Helper routine that updates all queues to all-retired
static void retire_device_fences(layer_data* my_data,
    VkDevice device)
{
    // Process each queue for device
    // TODO: Add multiple device support
    for (auto ii=my_data->queueMap.begin(); ii!=my_data->queueMap.end(); ++ii) {
        // Set queue's lastRetired to lastSubmitted indicating all fences completed
        MT_QUEUE_INFO *pQueueInfo = &(*ii).second;
        pQueueInfo->lastRetiredId = pQueueInfo->lastSubmittedId;
    }
}

// Helper function to validate correct usage bits set for buffers or images
//  Verify that (actual & desired) flags != 0 or,
//   if strict is true, verify that (actual & desired) flags == desired
//  In case of error, report it via dbg callbacks
static VkBool32 validate_usage_flags(layer_data* my_data, void* disp_obj, VkFlags actual, VkFlags desired,
                                     VkBool32 strict, uint64_t obj_handle, VkDbgObjectType obj_type,
                                     char const* ty_str, char const* func_name, char const* usage_str)
{
    VkBool32 correct_usage = VK_FALSE;
    VkBool32 skipCall      = VK_FALSE;
    if (strict)
        correct_usage = ((actual & desired) == desired);
    else
        correct_usage = ((actual & desired) != 0);
    if (!correct_usage) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, obj_type, obj_handle, 0, MEMTRACK_INVALID_USAGE_FLAG, "MEM",
                           "Invalid usage flag for %s %#" PRIxLEAST64 " used by %s. In this case, %s should have %s set during creation.",
                           ty_str, obj_handle, func_name, ty_str, usage_str);
    }
    return skipCall;
}

// Helper function to validate usage flags for images
// Pulls image info and then sends actual vs. desired usage off to helper above where
//  an error will be flagged if usage is not correct
static VkBool32 validate_image_usage_flags(layer_data* my_data, void* disp_obj, VkImage image, VkFlags desired, VkBool32 strict,
                                           char const* func_name, char const* usage_string)
{
    VkBool32 skipCall = VK_FALSE;
    MT_OBJ_BINDING_INFO* pBindInfo = get_object_binding_info(my_data, (uint64_t)image, VK_OBJECT_TYPE_IMAGE);
    if (pBindInfo) {
        skipCall = validate_usage_flags(my_data, disp_obj, pBindInfo->create_info.image.usage, desired, strict,
                                      (uint64_t) image, VK_OBJECT_TYPE_IMAGE, "image", func_name, usage_string);
    }
    return skipCall;
}

// Helper function to validate usage flags for buffers
// Pulls buffer info and then sends actual vs. desired usage off to helper above where
//  an error will be flagged if usage is not correct
static VkBool32 validate_buffer_usage_flags(layer_data* my_data, void* disp_obj, VkBuffer buffer, VkFlags desired, VkBool32 strict,
                                            char const* func_name, char const* usage_string)
{
    VkBool32 skipCall = VK_FALSE;
    MT_OBJ_BINDING_INFO* pBindInfo = get_object_binding_info(my_data, (uint64_t) buffer, VK_OBJECT_TYPE_BUFFER);
    if (pBindInfo) {
        skipCall = validate_usage_flags(my_data, disp_obj, pBindInfo->create_info.buffer.usage, desired, strict,
                                      (uint64_t) buffer, VK_OBJECT_TYPE_BUFFER, "buffer", func_name, usage_string);
    }
    return skipCall;
}

// Return ptr to info in map container containing mem, or NULL if not found
//  Calls to this function should be wrapped in mutex
static MT_MEM_OBJ_INFO* get_mem_obj_info(layer_data* my_data,
    const VkDeviceMemory mem)
{
    auto item = my_data->memObjMap.find(mem);
    if (item != my_data->memObjMap.end()) {
        return &(*item).second;
    } else {
        return NULL;
    }
}

static void add_mem_obj_info(layer_data* my_data,
    void*                    object,
    const VkDeviceMemory     mem,
    const VkMemoryAllocateInfo* pAllocateInfo)
{
    assert(object != NULL);

    memcpy(&my_data->memObjMap[mem].allocInfo, pAllocateInfo, sizeof(VkMemoryAllocateInfo));
    // TODO:  Update for real hardware, actually process allocation info structures
    my_data->memObjMap[mem].allocInfo.pNext = NULL;
    my_data->memObjMap[mem].object = object;
    my_data->memObjMap[mem].refCount = 0;
    my_data->memObjMap[mem].mem = mem;
}

// Find CB Info and add mem reference to list container
// Find Mem Obj Info and add CB reference to list container
static VkBool32 update_cmd_buf_and_mem_references(
    layer_data           *my_data,
    const VkCommandBuffer     cb,
    const VkDeviceMemory  mem,
    const char           *apiName)
{
    VkBool32 skipCall = VK_FALSE;

    // Skip validation if this image was created through WSI
    if (mem != MEMTRACKER_SWAP_CHAIN_IMAGE_KEY) {

        // First update CB binding in MemObj mini CB list
        MT_MEM_OBJ_INFO* pMemInfo = get_mem_obj_info(my_data, mem);
        if (!pMemInfo) {
            skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cb, 0, MEMTRACK_INVALID_MEM_OBJ, "MEM",
                           "In %s, trying to bind mem obj %#" PRIxLEAST64 " to CB %p but no info for that mem obj.\n    "
                           "Was it correctly allocated? Did it already get freed?", apiName, (uint64_t) mem, cb);
        } else {
            // Search for cmd buffer object in memory object's binding list
            VkBool32 found  = VK_FALSE;
            if (pMemInfo->pCommandBufferBindings.size() > 0) {
                for (list<VkCommandBuffer>::iterator it = pMemInfo->pCommandBufferBindings.begin(); it != pMemInfo->pCommandBufferBindings.end(); ++it) {
                    if ((*it) == cb) {
                        found = VK_TRUE;
                        break;
                    }
                }
            }
            // If not present, add to list
            if (found == VK_FALSE) {
                pMemInfo->pCommandBufferBindings.push_front(cb);
                pMemInfo->refCount++;
            }
            // Now update CBInfo's Mem reference list
            MT_CB_INFO* pCBInfo = get_cmd_buf_info(my_data, cb);
            // TODO: keep track of all destroyed CBs so we know if this is a stale or simply invalid object
            if (!pCBInfo) {
                skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cb, 0, MEMTRACK_INVALID_MEM_OBJ, "MEM",
                               "Trying to bind mem obj %#" PRIxLEAST64 " to CB %p but no info for that CB. Was CB incorrectly destroyed?", (uint64_t) mem, cb);
            } else {
                // Search for memory object in cmd buffer's reference list
                VkBool32 found  = VK_FALSE;
                if (pCBInfo->pMemObjList.size() > 0) {
                    for (auto it = pCBInfo->pMemObjList.begin(); it != pCBInfo->pMemObjList.end(); ++it) {
                        if ((*it) == mem) {
                            found = VK_TRUE;
                            break;
                        }
                    }
                }
                // If not present, add to list
                if (found == VK_FALSE) {
                    pCBInfo->pMemObjList.push_front(mem);
                }
            }
        }
    }
    return skipCall;
}

// Free bindings related to CB
static VkBool32 clear_cmd_buf_and_mem_references(layer_data* my_data,
    const VkCommandBuffer cb)
{
    VkBool32 skipCall = VK_FALSE;
    MT_CB_INFO* pCBInfo = get_cmd_buf_info(my_data, cb);
    if (!pCBInfo) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cb, 0, MEMTRACK_INVALID_CB, "MEM",
                     "Unable to find global CB info %p for deletion", cb);
    } else {
        if (pCBInfo->pMemObjList.size() > 0) {
            list<VkDeviceMemory> mem_obj_list = pCBInfo->pMemObjList;
            for (list<VkDeviceMemory>::iterator it=mem_obj_list.begin(); it!=mem_obj_list.end(); ++it) {
                MT_MEM_OBJ_INFO* pInfo = get_mem_obj_info(my_data, *it);
                pInfo->pCommandBufferBindings.remove(cb);
                pInfo->refCount--;
            }
        }
        pCBInfo->pMemObjList.clear();
    }
    return skipCall;
}

// Delete the entire CB list
static VkBool32 delete_cmd_buf_info_list(layer_data* my_data)
{
    VkBool32 skipCall = VK_FALSE;
    for (unordered_map<VkCommandBuffer, MT_CB_INFO>::iterator ii=my_data->cbMap.begin(); ii!=my_data->cbMap.end(); ++ii) {
        skipCall |= clear_cmd_buf_and_mem_references(my_data, (*ii).first);
    }
    my_data->cbMap.clear();
    return skipCall;
}

// For given MemObjInfo, report Obj & CB bindings
static VkBool32 reportMemReferencesAndCleanUp(layer_data* my_data,
    MT_MEM_OBJ_INFO* pMemObjInfo)
{
    VkBool32 skipCall = VK_FALSE;
    size_t cmdBufRefCount = pMemObjInfo->pCommandBufferBindings.size();
    size_t objRefCount    = pMemObjInfo->pObjBindings.size();

    if ((pMemObjInfo->pCommandBufferBindings.size() + pMemObjInfo->pObjBindings.size()) != 0) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t) pMemObjInfo->mem, 0, MEMTRACK_FREED_MEM_REF, "MEM",
                       "Attempting to free memory object %#" PRIxLEAST64 " which still contains %lu references",
                       (uint64_t) pMemObjInfo->mem, (cmdBufRefCount + objRefCount));
    }

    if (cmdBufRefCount > 0 && pMemObjInfo->pCommandBufferBindings.size() > 0) {
        for (list<VkCommandBuffer>::const_iterator it = pMemObjInfo->pCommandBufferBindings.begin(); it != pMemObjInfo->pCommandBufferBindings.end(); ++it) {
            // TODO : CommandBuffer should be source Obj here
            log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(*it), 0, MEMTRACK_FREED_MEM_REF, "MEM",
                    "Command Buffer %p still has a reference to mem obj %#" PRIxLEAST64, (*it), (uint64_t) pMemObjInfo->mem);
        }
        // Clear the list of hanging references
        pMemObjInfo->pCommandBufferBindings.clear();
    }

    if (objRefCount > 0 && pMemObjInfo->pObjBindings.size() > 0) {
        for (auto it = pMemObjInfo->pObjBindings.begin(); it != pMemObjInfo->pObjBindings.end(); ++it) {
            log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, it->type, it->handle, 0, MEMTRACK_FREED_MEM_REF, "MEM",
                    "VK Object %#" PRIxLEAST64 " still has a reference to mem obj %#" PRIxLEAST64, it->handle, (uint64_t) pMemObjInfo->mem);
        }
        // Clear the list of hanging references
        pMemObjInfo->pObjBindings.clear();
    }
    return skipCall;
}

static VkBool32 deleteMemObjInfo(layer_data* my_data,
    void* object,
    VkDeviceMemory mem)
{
    VkBool32 skipCall = VK_FALSE;
    auto item = my_data->memObjMap.find(mem);
    if (item != my_data->memObjMap.end()) {
        my_data->memObjMap.erase(item);
    } else {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t) mem, 0, MEMTRACK_INVALID_MEM_OBJ, "MEM",
                       "Request to delete memory object %#" PRIxLEAST64 " not present in memory Object Map", (uint64_t) mem);
    }
    return skipCall;
}

// Check if fence for given CB is completed
static VkBool32 checkCBCompleted(layer_data* my_data,
    const VkCommandBuffer  cb,
    VkBool32          *complete)
{
    VkBool32 skipCall = VK_FALSE;
    *complete = VK_TRUE;
    MT_CB_INFO* pCBInfo = get_cmd_buf_info(my_data, cb);
    if (!pCBInfo) {
        skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cb, 0,
                        MEMTRACK_INVALID_CB, "MEM", "Unable to find global CB info %p to check for completion", cb);
        *complete = VK_FALSE;
    } else if (pCBInfo->lastSubmittedQueue != NULL) {
        VkQueue queue = pCBInfo->lastSubmittedQueue;
        MT_QUEUE_INFO *pQueueInfo = &my_data->queueMap[queue];
        if (pCBInfo->fenceId > pQueueInfo->lastRetiredId) {
            log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)cb, 0,
                MEMTRACK_NONE, "MEM", "fence %#" PRIxLEAST64 " for CB %p has not been checked for completion",
                (uint64_t) pCBInfo->lastSubmittedFence, cb);
            *complete = VK_FALSE;
        }
    }
    return skipCall;
}

static VkBool32 freeMemObjInfo(layer_data* my_data,
    void*          object,
    VkDeviceMemory mem,
    bool           internal)
{
    VkBool32 skipCall = VK_FALSE;
    // Parse global list to find info w/ mem
    MT_MEM_OBJ_INFO* pInfo = get_mem_obj_info(my_data, mem);
    if (!pInfo) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t) mem, 0, MEMTRACK_INVALID_MEM_OBJ, "MEM",
                       "Couldn't find mem info object for %#" PRIxLEAST64 "\n    Was %#" PRIxLEAST64 " never allocated or previously freed?",
                       (uint64_t) mem, (uint64_t) mem);
    } else {
        if (pInfo->allocInfo.allocationSize == 0 && !internal) {
            skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t) mem, 0, MEMTRACK_INVALID_MEM_OBJ, "MEM",
                            "Attempting to free memory associated with a Persistent Image, %#" PRIxLEAST64 ", "
                            "this should not be explicitly freed\n", (uint64_t) mem);
        } else {
            // Clear any CB bindings for completed CBs
            //   TODO : Is there a better place to do this?

            VkBool32 commandBufferComplete = VK_FALSE;
            assert(pInfo->object != VK_NULL_HANDLE);
            list<VkCommandBuffer>::iterator it = pInfo->pCommandBufferBindings.begin();
            list<VkCommandBuffer>::iterator temp;
            while (pInfo->pCommandBufferBindings.size() > 0 && it != pInfo->pCommandBufferBindings.end()) {
                skipCall |= checkCBCompleted(my_data, *it, &commandBufferComplete);
                if (VK_TRUE == commandBufferComplete) {
                    temp = it;
                    ++temp;
                    skipCall |= clear_cmd_buf_and_mem_references(my_data, *it);
                    it = temp;
                } else {
                    ++it;
                }
            }

            // Now verify that no references to this mem obj remain and remove bindings
            if (0 != pInfo->refCount) {
                skipCall |= reportMemReferencesAndCleanUp(my_data, pInfo);
            }
            // Delete mem obj info
            skipCall |= deleteMemObjInfo(my_data, object, mem);
        }
    }
    return skipCall;
}

static const char *object_type_to_string(VkDbgObjectType type) {
    switch (type)
    {
        case VK_OBJECT_TYPE_IMAGE:
           return "image";
           break;
        case VK_OBJECT_TYPE_BUFFER:
           return "image";
           break;
        case VK_OBJECT_TYPE_SWAPCHAIN_KHR:
           return "swapchain";
           break;
        default:
           return "unknown";
    }
}

// Remove object binding performs 3 tasks:
// 1. Remove ObjectInfo from MemObjInfo list container of obj bindings & free it
// 2. Decrement refCount for MemObjInfo
// 3. Clear mem binding for image/buffer by setting its handle to 0
// TODO : This only applied to Buffer, Image, and Swapchain objects now, how should it be updated/customized?
static VkBool32 clear_object_binding(layer_data* my_data, void* dispObj, uint64_t handle, VkDbgObjectType type)
{
    // TODO : Need to customize images/buffers/swapchains to track mem binding and clear it here appropriately
    VkBool32 skipCall = VK_FALSE;
    MT_OBJ_BINDING_INFO* pObjBindInfo = get_object_binding_info(my_data, handle, type);
    if (pObjBindInfo) {
        MT_MEM_OBJ_INFO* pMemObjInfo = get_mem_obj_info(my_data, pObjBindInfo->mem);
        if (!pMemObjInfo) {
            skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_WARN_BIT, type, handle, 0, MEMTRACK_MEM_OBJ_CLEAR_EMPTY_BINDINGS, "MEM",
                           "Attempting to clear mem binding on %s obj %#" PRIxLEAST64 " but it has no binding.",
                           object_type_to_string(type), handle);
        } else {
            // This obj is bound to a memory object. Remove the reference to this object in that memory object's list, decrement the memObj's refcount
            // and set the objects memory binding pointer to NULL.
            VkBool32 clearSucceeded = VK_FALSE;
            for (auto it = pMemObjInfo->pObjBindings.begin(); it != pMemObjInfo->pObjBindings.end(); ++it) {
                if ((it->handle == handle) && (it->type == type)) {
                    pMemObjInfo->refCount--;
                    pMemObjInfo->pObjBindings.erase(it);
                    // TODO : Make sure this is a reasonable way to reset mem binding
                    pObjBindInfo->mem = VK_NULL_HANDLE;
                    clearSucceeded = VK_TRUE;
                    break;
                }
            }
            if (VK_FALSE == clearSucceeded ) {
                skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, type, handle, 0, MEMTRACK_INVALID_OBJECT, "MEM",
                                "While trying to clear mem binding for %s obj %#" PRIxLEAST64 ", unable to find that object referenced by mem obj %#" PRIxLEAST64,
                                 object_type_to_string(type), handle, (uint64_t) pMemObjInfo->mem);
            }
        }
    }
    return skipCall;
}

// For NULL mem case, output warning
// Make sure given object is in global object map
//  IF a previous binding existed, output validation error
//  Otherwise, add reference from objectInfo to memoryInfo
//  Add reference off of objInfo
//  device is required for error logging, need a dispatchable
//  object for that.
static VkBool32 set_mem_binding(layer_data* my_data,
    void*            dispatch_object,
    VkDeviceMemory   mem,
    uint64_t         handle,
    VkDbgObjectType  type,
    const char      *apiName)
{
    VkBool32 skipCall = VK_FALSE;
    // Handle NULL case separately, just clear previous binding & decrement reference
    if (mem == VK_NULL_HANDLE) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_WARN_BIT, type, handle, 0, MEMTRACK_INVALID_MEM_OBJ, "MEM",
                       "In %s, attempting to Bind Obj(%#" PRIxLEAST64 ") to NULL", apiName, handle);
    } else {
        MT_OBJ_BINDING_INFO* pObjBindInfo = get_object_binding_info(my_data, handle, type);
        if (!pObjBindInfo) {
            skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, type, handle, 0, MEMTRACK_MISSING_MEM_BINDINGS, "MEM",
                            "In %s, attempting to update Binding of %s Obj(%#" PRIxLEAST64 ") that's not in global list()",
                            object_type_to_string(type), apiName, handle);
        } else {
            // non-null case so should have real mem obj
            MT_MEM_OBJ_INFO* pMemInfo = get_mem_obj_info(my_data, mem);
            if (!pMemInfo) {
                skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t) mem,
                                0, MEMTRACK_INVALID_MEM_OBJ, "MEM", "In %s, while trying to bind mem for %s obj %#" PRIxLEAST64 ", couldn't find info for mem obj %#" PRIxLEAST64,
                                object_type_to_string(type), apiName, handle, (uint64_t) mem);
            } else {
                // TODO : Need to track mem binding for obj and report conflict here
                MT_MEM_OBJ_INFO* pPrevBinding = get_mem_obj_info(my_data, pObjBindInfo->mem);
                if (pPrevBinding != NULL) {
                    skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t) mem, 0, MEMTRACK_REBIND_OBJECT, "MEM",
                            "In %s, attempting to bind memory (%#" PRIxLEAST64 ") to object (%#" PRIxLEAST64 ") which has already been bound to mem object %#" PRIxLEAST64,
                            apiName, (uint64_t) mem, handle, (uint64_t) pPrevBinding->mem);
                }
                else {
                    MT_OBJ_HANDLE_TYPE oht;
                    oht.handle = handle;
                    oht.type = type;
                    pMemInfo->pObjBindings.push_front(oht);
                    pMemInfo->refCount++;
                    // For image objects, make sure default memory state is correctly set
                    // TODO : What's the best/correct way to handle this?
                    if (VK_OBJECT_TYPE_IMAGE == type) {
                        VkImageCreateInfo ici = pObjBindInfo->create_info.image;
                        if (ici.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
                            // TODO::  More memory state transition stuff.
                        }
                    }
                    pObjBindInfo->mem = mem;
                }
            }
        }
    }
    return skipCall;
}

// For NULL mem case, clear any previous binding Else...
// Make sure given object is in its object map
//  IF a previous binding existed, update binding
//  Add reference from objectInfo to memoryInfo
//  Add reference off of object's binding info
// Return VK_TRUE if addition is successful, VK_FALSE otherwise
static VkBool32 set_sparse_mem_binding(layer_data* my_data,
    void*            dispObject,
    VkDeviceMemory   mem,
    uint64_t         handle,
    VkDbgObjectType  type,
    const char      *apiName)
{
    VkBool32 skipCall = VK_FALSE;
    // Handle NULL case separately, just clear previous binding & decrement reference
    if (mem == VK_NULL_HANDLE) {
        skipCall = clear_object_binding(my_data, dispObject, handle, type);
    } else {
        MT_OBJ_BINDING_INFO* pObjBindInfo = get_object_binding_info(my_data, handle, type);
        if (!pObjBindInfo) {
            skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, type, handle, 0, MEMTRACK_MISSING_MEM_BINDINGS, "MEM",
                            "In %s, attempting to update Binding of Obj(%#" PRIxLEAST64 ") that's not in global list()", apiName, handle);
        }
        // non-null case so should have real mem obj
        MT_MEM_OBJ_INFO* pInfo = get_mem_obj_info(my_data, mem);
        if (!pInfo) {
            skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t) mem, 0, MEMTRACK_INVALID_MEM_OBJ, "MEM",
                            "In %s, While trying to bind mem for obj %#" PRIxLEAST64 ", couldn't find info for mem obj %#" PRIxLEAST64, apiName, handle, (uint64_t) mem);
        } else {
            // Search for object in memory object's binding list
            VkBool32 found  = VK_FALSE;
            if (pInfo->pObjBindings.size() > 0) {
                for (auto it = pInfo->pObjBindings.begin(); it != pInfo->pObjBindings.end(); ++it) {
                    if (((*it).handle == handle) && ((*it).type == type)) {
                        found = VK_TRUE;
                        break;
                    }
                }
            }
            // If not present, add to list
            if (found == VK_FALSE) {
                MT_OBJ_HANDLE_TYPE oht;
                oht.handle = handle;
                oht.type   = type;
                pInfo->pObjBindings.push_front(oht);
                pInfo->refCount++;
            }
            // Need to set mem binding for this object
            MT_MEM_OBJ_INFO* pPrevBinding = get_mem_obj_info(my_data, pObjBindInfo->mem);
            pObjBindInfo->mem = mem;
        }
    }
    return skipCall;
}

template <typename T>
void print_object_map_members(layer_data* my_data,
    void*            dispObj,
    T const&         objectName,
    VkDbgObjectType  objectType,
    const char      *objectStr)
{
    for (auto const& element : objectName) {
        log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, objectType, 0, 0, MEMTRACK_NONE, "MEM",
            "    %s Object list contains %s Object %#" PRIxLEAST64 " ", objectStr, objectStr, element.first);
    }
}

// For given Object, get 'mem' obj that it's bound to or NULL if no binding
static VkBool32 get_mem_binding_from_object(layer_data* my_data,
    void* dispObj, const uint64_t handle, const VkDbgObjectType type, VkDeviceMemory *mem)
{
    VkBool32 skipCall = VK_FALSE;
    *mem = VK_NULL_HANDLE;
    MT_OBJ_BINDING_INFO* pObjBindInfo = get_object_binding_info(my_data, handle, type);
    if (pObjBindInfo) {
        if (pObjBindInfo->mem) {
            *mem = pObjBindInfo->mem;
        } else {
            skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, type, handle, 0, MEMTRACK_MISSING_MEM_BINDINGS, "MEM",
                           "Trying to get mem binding for object %#" PRIxLEAST64 " but object has no mem binding", handle);
        }
    } else {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, type, handle, 0, MEMTRACK_INVALID_OBJECT, "MEM",
                       "Trying to get mem binding for object %#" PRIxLEAST64 " but no such object in %s list",
                       handle, object_type_to_string(type));
    }
    return skipCall;
}

// Print details of MemObjInfo list
static void print_mem_list(layer_data* my_data,
    void* dispObj)
{
    MT_MEM_OBJ_INFO* pInfo = NULL;

    // Early out if info is not requested
    if (!(my_data->report_data->active_flags & VK_DBG_REPORT_INFO_BIT)) {
        return;
    }

    // Just printing each msg individually for now, may want to package these into single large print
    log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
            "Details of Memory Object list (of size %lu elements)", my_data->memObjMap.size());
    log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
            "=============================");

    if (my_data->memObjMap.size() <= 0)
        return;

    for (auto ii=my_data->memObjMap.begin(); ii!=my_data->memObjMap.end(); ++ii) {
        pInfo = &(*ii).second;

        log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
            "    ===MemObjInfo at %p===", (void*)pInfo);
        log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
                "    Mem object: %#" PRIxLEAST64, (void*)pInfo->mem);
        log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
                "    Ref Count: %u", pInfo->refCount);
        if (0 != pInfo->allocInfo.allocationSize) {
            string pAllocInfoMsg = vk_print_vkmemoryallocateinfo(&pInfo->allocInfo, "MEM(INFO):         ");
            log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
                    "    Mem Alloc info:\n%s", pAllocInfoMsg.c_str());
        } else {
            log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
                    "    Mem Alloc info is NULL (alloc done by vkCreateSwapchainKHR())");
        }

        log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
                "    VK OBJECT Binding list of size %lu elements:", pInfo->pObjBindings.size());
        if (pInfo->pObjBindings.size() > 0) {
            for (list<MT_OBJ_HANDLE_TYPE>::iterator it = pInfo->pObjBindings.begin(); it != pInfo->pObjBindings.end(); ++it) {
                log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
                        "       VK OBJECT %p", (*it));
            }
        }

        log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
                "    VK Command Buffer (CB) binding list of size %lu elements", pInfo->pCommandBufferBindings.size());
        if (pInfo->pCommandBufferBindings.size() > 0)
        {
            for (list<VkCommandBuffer>::iterator it = pInfo->pCommandBufferBindings.begin(); it != pInfo->pCommandBufferBindings.end(); ++it) {
                log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
                        "      VK CB %p", (*it));
            }
        }
    }
}

static void printCBList(layer_data* my_data,
    void* dispObj)
{
    MT_CB_INFO* pCBInfo = NULL;

    // Early out if info is not requested
    if (!(my_data->report_data->active_flags & VK_DBG_REPORT_INFO_BIT)) {
        return;
    }

    log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
        "Details of CB list (of size %lu elements)", my_data->cbMap.size());
    log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
        "==================");

    if (my_data->cbMap.size() <= 0)
        return;

    for (auto ii=my_data->cbMap.begin(); ii!=my_data->cbMap.end(); ++ii) {
        pCBInfo = &(*ii).second;

        log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
                "    CB Info (%p) has CB %p, fenceId %" PRIx64", and fence %#" PRIxLEAST64,
                (void*)pCBInfo, (void*)pCBInfo->commandBuffer, pCBInfo->fenceId,
                (uint64_t) pCBInfo->lastSubmittedFence);

        if (pCBInfo->pMemObjList.size() <= 0)
            continue;
        for (list<VkDeviceMemory>::iterator it = pCBInfo->pMemObjList.begin(); it != pCBInfo->pMemObjList.end(); ++it) {
            log_msg(my_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, 0, 0, MEMTRACK_NONE, "MEM",
                    "      Mem obj %p", (*it));
        }
    }
}

static void init_mem_tracker(
    layer_data *my_data)
{
    uint32_t report_flags = 0;
    uint32_t debug_action = 0;
    FILE *log_output = NULL;
    const char *option_str;
    VkDbgMsgCallback callback;
    // initialize MemTracker options
    report_flags = getLayerOptionFlags("MemTrackerReportFlags", 0);
    getLayerOptionEnum("MemTrackerDebugAction", (uint32_t *) &debug_action);

    if (debug_action & VK_DBG_LAYER_ACTION_LOG_MSG)
    {
        option_str = getLayerOption("MemTrackerLogFilename");
        log_output = getLayerLogOutput(option_str, "MemTracker");
        layer_create_msg_callback(my_data->report_data, report_flags, log_callback, (void *) log_output, &callback);
        my_data->logging_callback.push_back(callback);
    }

    if (debug_action & VK_DBG_LAYER_ACTION_DEBUG_OUTPUT) {
        layer_create_msg_callback(my_data->report_data, report_flags, win32_debug_output_msg, NULL, &callback);
        my_data->logging_callback.push_back(callback);
    }

    if (!globalLockInitialized)
    {
        loader_platform_thread_create_mutex(&globalLock);
        globalLockInitialized = 1;
    }

    // Zero out memory property data
    memset(&memProps, 0, sizeof(VkPhysicalDeviceMemoryProperties));
}

// hook DestroyInstance to remove tableInstanceMap entry
VK_LAYER_EXPORT void VKAPI vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    // Grab the key before the instance is destroyed.
    dispatch_key key = get_dispatch_key(instance);
    layer_data *my_data = get_my_data_ptr(key, layer_data_map);
    VkLayerInstanceDispatchTable *pTable = my_data->instance_dispatch_table;
    pTable->DestroyInstance(instance, pAllocator);

    // Clean up logging callback, if any
    while (my_data->logging_callback.size() > 0) {
        VkDbgMsgCallback callback = my_data->logging_callback.back();
        layer_destroy_msg_callback(my_data->report_data, callback);
        my_data->logging_callback.pop_back();
    }

    layer_debug_report_destroy_instance(my_data->report_data);
    delete my_data->instance_dispatch_table;
    layer_data_map.erase(key);
    if (layer_data_map.empty()) {
        // Release mutex when destroying last instance
        loader_platform_thread_delete_mutex(&globalLock);
        globalLockInitialized = 0;
    }
}

VkResult VKAPI vkCreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                     pAllocator,
    VkInstance*                                 pInstance)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(*pInstance), layer_data_map);
    VkLayerInstanceDispatchTable *pTable = my_data->instance_dispatch_table;
    VkResult result = pTable->CreateInstance(pCreateInfo, pAllocator, pInstance);

    if (result == VK_SUCCESS) {
        my_data->report_data = debug_report_create_instance(
                                   pTable,
                                   *pInstance,
                                   pCreateInfo->enabledExtensionNameCount,
                                   pCreateInfo->ppEnabledExtensionNames);

        init_mem_tracker(my_data);
    }
    return result;
}

static void createDeviceRegisterExtensions(const VkDeviceCreateInfo* pCreateInfo, VkDevice device)
{
    layer_data *my_device_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkLayerDispatchTable *pDisp = my_device_data->device_dispatch_table;
    PFN_vkGetDeviceProcAddr gpa = pDisp->GetDeviceProcAddr;
    pDisp->GetSurfacePropertiesKHR = (PFN_vkGetSurfacePropertiesKHR) gpa(device, "vkGetSurfacePropertiesKHR");
    pDisp->GetSurfaceFormatsKHR = (PFN_vkGetSurfaceFormatsKHR) gpa(device, "vkGetSurfaceFormatsKHR");
    pDisp->GetSurfacePresentModesKHR = (PFN_vkGetSurfacePresentModesKHR) gpa(device, "vkGetSurfacePresentModesKHR");
    pDisp->CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR) gpa(device, "vkCreateSwapchainKHR");
    pDisp->DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR) gpa(device, "vkDestroySwapchainKHR");
    pDisp->GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR) gpa(device, "vkGetSwapchainImagesKHR");
    pDisp->AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR) gpa(device, "vkAcquireNextImageKHR");
    pDisp->QueuePresentKHR = (PFN_vkQueuePresentKHR) gpa(device, "vkQueuePresentKHR");
    my_device_data->wsi_enabled = false;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionNameCount; i++) {
        if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], VK_EXT_KHR_DEVICE_SWAPCHAIN_EXTENSION_NAME) == 0)
            my_device_data->wsi_enabled = true;
    }
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateDevice(
    VkPhysicalDevice          gpu,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice                 *pDevice)
{
    layer_data *my_device_data = get_my_data_ptr(get_dispatch_key(*pDevice), layer_data_map);
    VkLayerDispatchTable *pDeviceTable = my_device_data->device_dispatch_table;
    VkResult result = pDeviceTable->CreateDevice(gpu, pCreateInfo, pAllocator, pDevice);
    if (result == VK_SUCCESS) {
        layer_data *my_instance_data = get_my_data_ptr(get_dispatch_key(gpu), layer_data_map);
        my_device_data->report_data = layer_debug_report_create_device(my_instance_data->report_data, *pDevice);
        createDeviceRegisterExtensions(pCreateInfo, *pDevice);
    }
    return result;
}

VK_LAYER_EXPORT void VKAPI vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator)
{
    dispatch_key key = get_dispatch_key(device);
    layer_data *my_device_data = get_my_data_ptr(key, layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    log_msg(my_device_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE, (uint64_t)device, 0, MEMTRACK_NONE, "MEM",
        "Printing List details prior to vkDestroyDevice()");
    log_msg(my_device_data->report_data, VK_DBG_REPORT_INFO_BIT, VK_OBJECT_TYPE_DEVICE, (uint64_t)device, 0, MEMTRACK_NONE, "MEM",
        "================================================");
    print_mem_list(my_device_data, device);
    printCBList(my_device_data, device);
    skipCall = delete_cmd_buf_info_list(my_device_data);
    // Report any memory leaks
    MT_MEM_OBJ_INFO* pInfo = NULL;
    if (my_device_data->memObjMap.size() > 0) {
        for (auto ii=my_device_data->memObjMap.begin(); ii!=my_device_data->memObjMap.end(); ++ii) {
            pInfo = &(*ii).second;
            if (pInfo->allocInfo.allocationSize != 0) {
                skipCall |= log_msg(my_device_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t) pInfo->mem, 0, MEMTRACK_MEMORY_LEAK, "MEM",
                                 "Mem Object %p has not been freed. You should clean up this memory by calling "
                                 "vkFreeMemory(%p) prior to vkDestroyDevice().", pInfo->mem, pInfo->mem);
            }
        }
    }
    // Queues persist until device is destroyed
    delete_queue_info_list(my_device_data);
    layer_debug_report_destroy_device(device);
    loader_platform_thread_unlock_mutex(&globalLock);

#if DISPATCH_MAP_DEBUG
    fprintf(stderr, "Device: %p, key: %p\n", device, key);
#endif
    VkLayerDispatchTable *pDisp  = my_device_data->device_dispatch_table;
    if (VK_FALSE == skipCall) {
        pDisp->DestroyDevice(device, pAllocator);
    }
    delete my_device_data->device_dispatch_table;
    layer_data_map.erase(key);
}

VK_LAYER_EXPORT void VKAPI vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice                  physicalDevice,
    VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(physicalDevice), layer_data_map);
    VkLayerInstanceDispatchTable *pInstanceTable = my_data->instance_dispatch_table;
    pInstanceTable->GetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
    memcpy(&memProps, pMemoryProperties, sizeof(VkPhysicalDeviceMemoryProperties));
}

static const VkLayerProperties mtGlobalLayers[] = {
    {
        "MemTracker",
        VK_API_VERSION,
        VK_MAKE_VERSION(0, 1, 0),
        "Validation layer: MemTracker",
    }
};

VK_LAYER_EXPORT VkResult VKAPI vkEnumerateInstanceExtensionProperties(
        const char *pLayerName,
        uint32_t *pCount,
        VkExtensionProperties* pProperties)
{
    /* Mem tracker does not have any global extensions */
    return util_GetExtensionProperties(0, NULL, pCount, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI vkEnumerateInstanceLayerProperties(
        uint32_t *pCount,
        VkLayerProperties*    pProperties)
{
    return util_GetLayerProperties(ARRAY_SIZE(mtGlobalLayers),
                                   mtGlobalLayers,
                                   pCount, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI vkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice                            physicalDevice,
        const char*                                 pLayerName,
        uint32_t*                                   pCount,
        VkExtensionProperties*                      pProperties)
{
    /* Mem tracker does not have any physical device extensions */
    if (pLayerName == NULL) {
        layer_data *my_data = get_my_data_ptr(get_dispatch_key(physicalDevice), layer_data_map);
        VkLayerInstanceDispatchTable *pInstanceTable = my_data->instance_dispatch_table;
        return pInstanceTable->EnumerateDeviceExtensionProperties(
                                                    physicalDevice,
                                                    NULL,
                                                    pCount,
                                                    pProperties);
    } else {
        return util_GetExtensionProperties(0, NULL, pCount, pProperties);
    }
}

VK_LAYER_EXPORT VkResult VKAPI vkEnumerateDeviceLayerProperties(
        VkPhysicalDevice                            physicalDevice,
        uint32_t*                                   pCount,
        VkLayerProperties*                          pProperties)
{
    /* Mem tracker's physical device layers are the same as global */
    return util_GetLayerProperties(ARRAY_SIZE(mtGlobalLayers), mtGlobalLayers,
                                   pCount, pProperties);
}

VK_LAYER_EXPORT void VKAPI vkGetDeviceQueue(
    VkDevice  device,
    uint32_t  queueNodeIndex,
    uint32_t  queueIndex,
    VkQueue   *pQueue)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    my_data->device_dispatch_table->GetDeviceQueue(device, queueNodeIndex, queueIndex, pQueue);
    loader_platform_thread_lock_mutex(&globalLock);
    add_queue_info(my_data, *pQueue);
    loader_platform_thread_unlock_mutex(&globalLock);
}

VK_LAYER_EXPORT VkResult VKAPI vkQueueSubmit(
    VkQueue             queue,
    uint32_t            submitCount,
    const VkSubmitInfo *pSubmits,
    VkFence             fence)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(queue), layer_data_map);
    VkResult result = VK_ERROR_VALIDATION_FAILED;

    loader_platform_thread_lock_mutex(&globalLock);
    // TODO : Need to track fence and clear mem references when fence clears
    MT_CB_INFO* pCBInfo = NULL;
    uint64_t    fenceId = 0;
    VkBool32 skipCall = add_fence_info(my_data, fence, queue, &fenceId);

    print_mem_list(my_data, queue);
    printCBList(my_data, queue);
    for (uint32_t submit_idx = 0; submit_idx < submitCount; submit_idx++) {
        const VkSubmitInfo *submit = &pSubmits[submit_idx];
        for (uint32_t i = 0; i < submit->commandBufferCount; i++) {
            pCBInfo = get_cmd_buf_info(my_data, submit->pCommandBuffers[i]);
            pCBInfo->fenceId = fenceId;
            pCBInfo->lastSubmittedFence = fence;
            pCBInfo->lastSubmittedQueue = queue;
        }

        for (uint32_t i = 0; i < submit->waitSemaphoreCount; i++) {
            VkSemaphore sem = submit->pWaitSemaphores[i];

            if (my_data->semaphoreMap.find(sem) != my_data->semaphoreMap.end()) {
                if (my_data->semaphoreMap[sem] != MEMTRACK_SEMAPHORE_STATE_SIGNALLED) {
                    skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_SEMAPHORE, (uint64_t) sem,
                            0, MEMTRACK_NONE, "SEMAPHORE",
                            "vkQueueSubmit: Semaphore must be in signaled state before passing to pWaitSemaphores");
                }
                my_data->semaphoreMap[sem] = MEMTRACK_SEMAPHORE_STATE_WAIT;
            }
        }
        for (uint32_t i = 0; i < submit->signalSemaphoreCount; i++) {
            VkSemaphore sem = submit->pWaitSemaphores[i];

            if (my_data->semaphoreMap.find(sem) != my_data->semaphoreMap.end()) {
                if (my_data->semaphoreMap[sem] != MEMTRACK_SEMAPHORE_STATE_UNSET) {
                    skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_SEMAPHORE, (uint64_t) sem,
                            0, MEMTRACK_NONE, "SEMAPHORE",
                            "vkQueueSubmit: Semaphore must not be currently signaled or in a wait state");
                }
                my_data->semaphoreMap[sem] = MEMTRACK_SEMAPHORE_STATE_SIGNALLED;
            }
        }
    }

    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        result = my_data->device_dispatch_table->QueueSubmit(
            queue, submitCount, pSubmits, fence);
    }

    loader_platform_thread_lock_mutex(&globalLock);
    for (uint32_t submit_idx = 0; submit_idx < submitCount; submit_idx++) {
        const VkSubmitInfo *submit = &pSubmits[submit_idx];
        for (uint32_t i = 0; i < submit->waitSemaphoreCount; i++) {
            VkSemaphore sem = submit->pWaitSemaphores[i];

            if (my_data->semaphoreMap.find(sem) != my_data->semaphoreMap.end()) {
                my_data->semaphoreMap[sem] = MEMTRACK_SEMAPHORE_STATE_UNSET;
            }
        }
    }
    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkAllocateMemory(
    VkDevice                 device,
    const VkMemoryAllocateInfo *pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory          *pMemory)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
    // TODO : Track allocations and overall size here
    loader_platform_thread_lock_mutex(&globalLock);
    add_mem_obj_info(my_data, device, *pMemory, pAllocateInfo);
    print_mem_list(my_data, device);
    loader_platform_thread_unlock_mutex(&globalLock);
    return result;
}

VK_LAYER_EXPORT void VKAPI vkFreeMemory(
    VkDevice       device,
    VkDeviceMemory mem,
    const VkAllocationCallbacks* pAllocator)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    /* From spec : A memory object is freed by calling vkFreeMemory() when it is no longer needed. Before
     * freeing a memory object, an application must ensure the memory object is unbound from
     * all API objects referencing it and that it is not referenced by any queued command buffers
     */
    loader_platform_thread_lock_mutex(&globalLock);
    freeMemObjInfo(my_data, device, mem, false);
    print_mem_list(my_data, device);
    printCBList(my_data, device);
    loader_platform_thread_unlock_mutex(&globalLock);
    my_data->device_dispatch_table->FreeMemory(device, mem, pAllocator);
}

bool validateMemRange(
    VkDevice        device,
    VkDeviceMemory  mem,
    VkDeviceSize    offset,
    VkDeviceSize    size)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    bool skip_call = false;

    auto mem_element = my_data->memObjMap.find(mem);
    if (mem_element != my_data->memObjMap.end()) {
        if ((offset + size) > mem_element->second.allocInfo.allocationSize) {
            skip_call = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t) mem, 0,
                                MEMTRACK_INVALID_MAP, "MEM", "Mapping Memory from %" PRIu64 " to %" PRIu64 " with total array size %" PRIu64,
                                offset, size + offset, mem_element->second.allocInfo.allocationSize);
        }
    }
    return skip_call;
}

VK_LAYER_EXPORT VkResult VKAPI vkMapMemory(
    VkDevice         device,
    VkDeviceMemory   mem,
    VkDeviceSize     offset,
    VkDeviceSize     size,
    VkFlags          flags,
    void           **ppData)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    // TODO : Track when memory is mapped
    VkBool32 skipCall = VK_FALSE;
    VkResult result   = VK_ERROR_VALIDATION_FAILED;
    loader_platform_thread_lock_mutex(&globalLock);
    MT_MEM_OBJ_INFO *pMemObj = get_mem_obj_info(my_data, mem);
    if ((memProps.memoryTypes[pMemObj->allocInfo.memoryTypeIndex].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_DEVICE_MEMORY, (uint64_t) mem, 0, MEMTRACK_INVALID_STATE, "MEM",
                       "Mapping Memory without VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT set: mem obj %#" PRIxLEAST64, (uint64_t) mem);
    }
    skipCall |= validateMemRange(device, mem, offset, size);
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        result = my_data->device_dispatch_table->MapMemory(device, mem, offset, size, flags, ppData);
    }
    return result;
}

VK_LAYER_EXPORT void VKAPI vkUnmapMemory(
    VkDevice       device,
    VkDeviceMemory mem)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    // TODO : Track as memory gets unmapped, do we want to check what changed following map?
    //   Make sure that memory was ever mapped to begin with
    my_data->device_dispatch_table->UnmapMemory(device, mem);
}

VK_LAYER_EXPORT void VKAPI vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* pAllocator)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    loader_platform_thread_lock_mutex(&globalLock);
    delete_fence_info(my_data, fence);
    auto item = my_data->fenceMap.find(fence);
    if (item != my_data->fenceMap.end()) {
        my_data->fenceMap.erase(item);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    my_data->device_dispatch_table->DestroyFence(device, fence, pAllocator);
}

VK_LAYER_EXPORT void VKAPI vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    auto item = my_data->bufferMap.find((uint64_t)buffer);
    if (item != my_data->bufferMap.end()) {
        skipCall = clear_object_binding(my_data, device, (uint64_t)buffer, VK_OBJECT_TYPE_BUFFER);
        my_data->bufferMap.erase(item);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->DestroyBuffer(device, buffer, pAllocator);
    }
}

VK_LAYER_EXPORT void VKAPI vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    auto item = my_data->imageMap.find((uint64_t)image);
    if (item != my_data->imageMap.end()) {
        skipCall = clear_object_binding(my_data, device, (uint64_t)image, VK_OBJECT_TYPE_IMAGE);
        my_data->imageMap.erase(item);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->DestroyImage(device, image, pAllocator);
    }
}

VkResult VKAPI vkBindBufferMemory(
    VkDevice                                    device,
    VkBuffer                                    buffer,
    VkDeviceMemory                              mem,
    VkDeviceSize                                memoryOffset)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = VK_ERROR_VALIDATION_FAILED;
    loader_platform_thread_lock_mutex(&globalLock);
    // Track objects tied to memory
    VkBool32 skipCall = set_mem_binding(my_data, device, mem, (uint64_t)buffer, VK_OBJECT_TYPE_BUFFER, "vkBindBufferMemory");
    add_object_binding_info(my_data, (uint64_t)buffer, VK_OBJECT_TYPE_BUFFER, mem);
    print_mem_list(my_data, device);
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        result = my_data->device_dispatch_table->BindBufferMemory(device, buffer, mem, memoryOffset);
    }
    return result;
}

VkResult VKAPI vkBindImageMemory(
    VkDevice                                    device,
    VkImage                                     image,
    VkDeviceMemory                              mem,
    VkDeviceSize                                memoryOffset)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = VK_ERROR_VALIDATION_FAILED;
    loader_platform_thread_lock_mutex(&globalLock);
    // Track objects tied to memory
    VkBool32 skipCall = set_mem_binding(my_data, device, mem, (uint64_t)image, VK_OBJECT_TYPE_IMAGE, "vkBindImageMemory");
    add_object_binding_info(my_data, (uint64_t)image, VK_OBJECT_TYPE_IMAGE, mem);
    print_mem_list(my_data, device);
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        result = my_data->device_dispatch_table->BindImageMemory(device, image, mem, memoryOffset);
    }
    return result;
}

void VKAPI vkGetBufferMemoryRequirements(
    VkDevice                                    device,
    VkBuffer                                    buffer,
    VkMemoryRequirements*                       pMemoryRequirements)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    // TODO : What to track here?
    //   Could potentially save returned mem requirements and validate values passed into BindBufferMemory
    my_data->device_dispatch_table->GetBufferMemoryRequirements(device, buffer, pMemoryRequirements);
}

void VKAPI vkGetImageMemoryRequirements(
    VkDevice                                    device,
    VkImage                                     image,
    VkMemoryRequirements*                       pMemoryRequirements)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    // TODO : What to track here?
    //   Could potentially save returned mem requirements and validate values passed into BindImageMemory
    my_data->device_dispatch_table->GetImageMemoryRequirements(device, image, pMemoryRequirements);
}

VK_LAYER_EXPORT VkResult VKAPI vkQueueBindSparse(
    VkQueue                                     queue,
    uint32_t                                    bindInfoCount,
    const VkBindSparseInfo*                     pBindInfo,
    VkFence                                     fence)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(queue), layer_data_map);
    VkResult result = VK_ERROR_VALIDATION_FAILED;
    VkBool32 skipCall = VK_FALSE;

    loader_platform_thread_lock_mutex(&globalLock);

    for (uint32_t i = 0; i < bindInfoCount; i++) {
        // Track objects tied to memory
        for (uint32_t j = 0; j < pBindInfo[i].bufferBindCount; j++) {
            for (uint32_t k = 0; k < pBindInfo[i].pBufferBinds[j].bindCount; k++) {
                if (set_sparse_mem_binding(my_data, queue,
                            pBindInfo[i].pBufferBinds[j].pBinds[k].memory,
                            (uint64_t) pBindInfo[i].pBufferBinds[j].buffer,
                            VK_OBJECT_TYPE_BUFFER, "vkQueueBindSparse"))
                    skipCall = VK_TRUE;
            }
        }
        for (uint32_t j = 0; j < pBindInfo[i].imageOpaqueBindCount; j++) {
            for (uint32_t k = 0; k < pBindInfo[i].pImageOpaqueBinds[j].bindCount; k++) {
                if (set_sparse_mem_binding(my_data, queue,
                            pBindInfo[i].pImageOpaqueBinds[j].pBinds[k].memory,
                            (uint64_t) pBindInfo[i].pImageOpaqueBinds[j].image,
                            VK_OBJECT_TYPE_IMAGE, "vkQueueBindSparse"))
                    skipCall = VK_TRUE;
            }
        }
        for (uint32_t j = 0; j < pBindInfo[i].imageBindCount; j++) {
            for (uint32_t k = 0; k < pBindInfo[i].pImageBinds[j].bindCount; k++) {
                if (set_sparse_mem_binding(my_data, queue,
                            pBindInfo[i].pImageBinds[j].pBinds[k].memory,
                            (uint64_t) pBindInfo[i].pImageBinds[j].image,
                            VK_OBJECT_TYPE_IMAGE, "vkQueueBindSparse"))
                    skipCall = VK_TRUE;
            }
        }
    }

    print_mem_list(my_data, queue);
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        result = my_data->device_dispatch_table->QueueBindSparse(queue, bindInfoCount, pBindInfo, fence);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateFence(
    VkDevice                 device,
    const VkFenceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkFence                 *pFence)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->CreateFence(device, pCreateInfo, pAllocator, pFence);
    if (VK_SUCCESS == result) {
        loader_platform_thread_lock_mutex(&globalLock);
        MT_FENCE_INFO* pFI = &my_data->fenceMap[*pFence];
        memset(pFI, 0, sizeof(MT_FENCE_INFO));
        memcpy(&(pFI->createInfo), pCreateInfo, sizeof(VkFenceCreateInfo));
        loader_platform_thread_unlock_mutex(&globalLock);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkResetFences(
    VkDevice  device,
    uint32_t  fenceCount,
    const VkFence  *pFences)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result   = VK_ERROR_VALIDATION_FAILED;
    VkBool32 skipCall = VK_FALSE;

    loader_platform_thread_lock_mutex(&globalLock);
    // Reset fence state in fenceCreateInfo structure
    for (uint32_t i = 0; i < fenceCount; i++) {
        auto fence_item = my_data->fenceMap.find(pFences[i]);
        if (fence_item != my_data->fenceMap.end()) {
            // Validate fences in SIGNALED state
            if (!(fence_item->second.createInfo.flags & VK_FENCE_CREATE_SIGNALED_BIT)) {
                skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_FENCE, (uint64_t) pFences[i], 0, MEMTRACK_INVALID_FENCE_STATE, "MEM",
                        "Fence %#" PRIxLEAST64 " submitted to VkResetFences in UNSIGNALED STATE", (uint64_t) pFences[i]);
            }
            else {
                fence_item->second.createInfo.flags =
                    static_cast<VkFenceCreateFlags>(fence_item->second.createInfo.flags & ~VK_FENCE_CREATE_SIGNALED_BIT);
            }
        }
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        result = my_data->device_dispatch_table->ResetFences(device, fenceCount, pFences);
    }
    return result;
}

static inline VkBool32 verifyFenceStatus(VkDevice device, VkFence fence, const char* apiCall)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    auto pFenceInfo = my_data->fenceMap.find(fence);
    if (pFenceInfo != my_data->fenceMap.end()) {
        if (pFenceInfo->second.createInfo.flags & VK_FENCE_CREATE_SIGNALED_BIT) {
            skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_FENCE, (uint64_t) fence, 0, MEMTRACK_INVALID_FENCE_STATE, "MEM",
                "%s specified fence %#" PRIxLEAST64 " already in SIGNALED state.", apiCall, (uint64_t) fence);
        }
        if (!pFenceInfo->second.queue) { // Checking status of unsubmitted fence
            skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_FENCE, (uint64_t) fence, 0, MEMTRACK_INVALID_FENCE_STATE, "MEM",
                "%s called for fence %#" PRIxLEAST64 " which has not been submitted on a Queue.", apiCall, (uint64_t) fence);
        }
    }
    return skipCall;
}

VK_LAYER_EXPORT VkResult VKAPI vkGetFenceStatus(
    VkDevice device,
    VkFence  fence)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkBool32 skipCall = verifyFenceStatus(device, fence, "vkGetFenceStatus");
    if (skipCall)
        return VK_ERROR_VALIDATION_FAILED;
    VkResult result = my_data->device_dispatch_table->GetFenceStatus(device, fence);
    if (VK_SUCCESS == result) {
        loader_platform_thread_lock_mutex(&globalLock);
        update_fence_tracking(my_data, fence);
        loader_platform_thread_unlock_mutex(&globalLock);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkWaitForFences(
    VkDevice       device,
    uint32_t       fenceCount,
    const VkFence *pFences,
    VkBool32       waitAll,
    uint64_t       timeout)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    // Verify fence status of submitted fences
    for(uint32_t i = 0; i < fenceCount; i++) {
        skipCall |= verifyFenceStatus(device, pFences[i], "vkWaitForFences");
    }
    if (skipCall)
        return VK_ERROR_VALIDATION_FAILED;
    VkResult result = my_data->device_dispatch_table->WaitForFences(device, fenceCount, pFences, waitAll, timeout);
    loader_platform_thread_lock_mutex(&globalLock);

    if (VK_SUCCESS == result) {
        if (waitAll || fenceCount == 1) { // Clear all the fences
            for(uint32_t i = 0; i < fenceCount; i++) {
                update_fence_tracking(my_data, pFences[i]);
            }
        }
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkQueueWaitIdle(
    VkQueue queue)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(queue), layer_data_map);
    VkResult result = my_data->device_dispatch_table->QueueWaitIdle(queue);
    if (VK_SUCCESS == result) {
        loader_platform_thread_lock_mutex(&globalLock);
        retire_queue_fences(my_data, queue);
        loader_platform_thread_unlock_mutex(&globalLock);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkDeviceWaitIdle(
    VkDevice device)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->DeviceWaitIdle(device);
    if (VK_SUCCESS == result) {
        loader_platform_thread_lock_mutex(&globalLock);
        retire_device_fences(my_data, device);
        loader_platform_thread_unlock_mutex(&globalLock);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateBuffer(
    VkDevice                  device,
    const VkBufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkBuffer                 *pBuffer)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->CreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
    if (VK_SUCCESS == result) {
        loader_platform_thread_lock_mutex(&globalLock);
        add_object_create_info(my_data, (uint64_t)*pBuffer, VK_OBJECT_TYPE_BUFFER, pCreateInfo);
        loader_platform_thread_unlock_mutex(&globalLock);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateImage(
    VkDevice                 device,
    const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage                 *pImage)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->CreateImage(device, pCreateInfo, pAllocator, pImage);
    if (VK_SUCCESS == result) {
        loader_platform_thread_lock_mutex(&globalLock);
        add_object_create_info(my_data, (uint64_t)*pImage, VK_OBJECT_TYPE_IMAGE, pCreateInfo);
        loader_platform_thread_unlock_mutex(&globalLock);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateImageView(
    VkDevice                     device,
    const VkImageViewCreateInfo *pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView                 *pView)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->CreateImageView(device, pCreateInfo, pAllocator, pView);
    if (result == VK_SUCCESS) {
        loader_platform_thread_lock_mutex(&globalLock);
        // Validate that img has correct usage flags set
        validate_image_usage_flags(my_data, device, pCreateInfo->image,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                    false, "vkCreateImageView()", "VK_IMAGE_USAGE_[SAMPLED|STORAGE|COLOR_ATTACHMENT]_BIT");
        loader_platform_thread_unlock_mutex(&globalLock);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateBufferView(
    VkDevice                      device,
    const VkBufferViewCreateInfo *pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkBufferView                 *pView)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->CreateBufferView(device, pCreateInfo, pAllocator, pView);
    if (result == VK_SUCCESS) {
        loader_platform_thread_lock_mutex(&globalLock);
        // In order to create a valid buffer view, the buffer must have been created with at least one of the
        // following flags:  UNIFORM_TEXEL_BUFFER_BIT or STORAGE_TEXEL_BUFFER_BIT
        validate_buffer_usage_flags(my_data, device, pCreateInfo->buffer,
                    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
                    false, "vkCreateBufferView()", "VK_BUFFER_USAGE_[STORAGE|UNIFORM]_TEXEL_BUFFER_BIT");
        loader_platform_thread_unlock_mutex(&globalLock);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkAllocateCommandBuffers(
    VkDevice                     device,
    const VkCommandBufferAllocateInfo *pCreateInfo,
    VkCommandBuffer                 *pCommandBuffer)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->AllocateCommandBuffers(device, pCreateInfo, pCommandBuffer);
    // At time of cmd buffer creation, create global cmd buffer info for the returned cmd buffer
    loader_platform_thread_lock_mutex(&globalLock);
    if (*pCommandBuffer)
        add_cmd_buf_info(my_data, *pCommandBuffer);
    printCBList(my_data, device);
    loader_platform_thread_unlock_mutex(&globalLock);
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkBeginCommandBuffer(
    VkCommandBuffer                 commandBuffer,
    const VkCommandBufferBeginInfo *pBeginInfo)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkResult result            = VK_ERROR_VALIDATION_FAILED;
    VkBool32 skipCall          = VK_FALSE;
    VkBool32 commandBufferComplete = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    // This implicitly resets the Cmd Buffer so make sure any fence is done and then clear memory references
    skipCall = checkCBCompleted(my_data, commandBuffer, &commandBufferComplete);

    if (VK_FALSE == commandBufferComplete) {
        skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                        MEMTRACK_RESET_CB_WHILE_IN_FLIGHT, "MEM", "Calling vkBeginCommandBuffer() on active CB %p before it has completed. "
                        "You must check CB flag before this call.", commandBuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        result = my_data->device_dispatch_table->BeginCommandBuffer(commandBuffer, pBeginInfo);
    }
    loader_platform_thread_lock_mutex(&globalLock);
    clear_cmd_buf_and_mem_references(my_data, commandBuffer);
    loader_platform_thread_unlock_mutex(&globalLock);
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkEndCommandBuffer(
    VkCommandBuffer commandBuffer)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    // TODO : Anything to do here?
    VkResult result = my_data->device_dispatch_table->EndCommandBuffer(commandBuffer);
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkResetCommandBuffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkResult result            = VK_ERROR_VALIDATION_FAILED;
    VkBool32 skipCall          = VK_FALSE;
    VkBool32 commandBufferComplete = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    // Verify that CB is complete (not in-flight)
    skipCall = checkCBCompleted(my_data, commandBuffer, &commandBufferComplete);
    if (VK_FALSE == commandBufferComplete) {
        skipCall |= log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                        MEMTRACK_RESET_CB_WHILE_IN_FLIGHT, "MEM", "Resetting CB %p before it has completed. You must check CB "
                        "flag before calling vkResetCommandBuffer().", commandBuffer);
    }
    // Clear memory references as this point.
    skipCall |= clear_cmd_buf_and_mem_references(my_data, commandBuffer);
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        result = my_data->device_dispatch_table->ResetCommandBuffer(commandBuffer, flags);
    }
    return result;
}
// TODO : For any vkCmdBind* calls that include an object which has mem bound to it,
//    need to account for that mem now having binding to given commandBuffer
VK_LAYER_EXPORT void VKAPI vkCmdBindPipeline(
    VkCommandBuffer     commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline          pipeline)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
#if 0
    // TODO : If memory bound to pipeline, then need to tie that mem to commandBuffer
    if (getPipeline(pipeline)) {
        MT_CB_INFO *pCBInfo = get_cmd_buf_info(my_data, commandBuffer);
        if (pCBInfo) {
            pCBInfo->pipelines[pipelineBindPoint] = pipeline;
        } else {
                    "Attempt to bind Pipeline %p to non-existant command buffer %p!", (void*)pipeline, commandBuffer);
            layerCbMsg(VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, commandBuffer, 0, MEMTRACK_INVALID_CB, (char *) "DS", (char *) str);
        }
    }
    else {
                "Attempt to bind Pipeline %p that doesn't exist!", (void*)pipeline);
        layerCbMsg(VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_PIPELINE, pipeline, 0, MEMTRACK_INVALID_OBJECT, (char *) "DS", (char *) str);
    }
#endif
    my_data->device_dispatch_table->CmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

VK_LAYER_EXPORT void VKAPI vkCmdSetViewport(
    VkCommandBuffer                     commandBuffer,
    uint32_t                            viewportCount,
    const VkViewport*                   pViewports)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    MT_CB_INFO *pCmdBuf = get_cmd_buf_info(my_data, commandBuffer);
    if (!pCmdBuf) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                       MEMTRACK_INVALID_CB, "MEM", "Unable to find command buffer object %p, was it ever created?", (void*)commandBuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdSetViewport(commandBuffer, viewportCount, pViewports);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdSetScissor(
    VkCommandBuffer                     commandBuffer,
    uint32_t                            scissorCount,
    const VkRect2D*                     pScissors)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    MT_CB_INFO *pCmdBuf = get_cmd_buf_info(my_data, commandBuffer);
    if (!pCmdBuf) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                       MEMTRACK_INVALID_CB, "MEM", "Unable to find command buffer object %p, was it ever created?", (void*)commandBuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdSetScissor(commandBuffer, scissorCount, pScissors);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    MT_CB_INFO *pCmdBuf = get_cmd_buf_info(my_data, commandBuffer);
    if (!pCmdBuf) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                       MEMTRACK_INVALID_CB, "MEM", "Unable to find command buffer object %p, was it ever created?", (void*)commandBuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdSetLineWidth(commandBuffer, lineWidth);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdSetDepthBias(
    VkCommandBuffer                     commandBuffer,
    float                               depthBiasConstantFactor,
    float                               depthBiasClamp,
    float                               depthBiasSlopeFactor)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    MT_CB_INFO *pCmdBuf = get_cmd_buf_info(my_data, commandBuffer);
    if (!pCmdBuf) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                       MEMTRACK_INVALID_CB, "MEM", "Unable to find command buffer object %p, was it ever created?", (void*)commandBuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdSetDepthBias(commandBuffer, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdSetBlendConstants(
     VkCommandBuffer                        commandBuffer,
     const float                            blendConstants[4])
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    MT_CB_INFO *pCmdBuf = get_cmd_buf_info(my_data, commandBuffer);
    if (!pCmdBuf) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                       MEMTRACK_INVALID_CB, "MEM", "Unable to find command buffer object %p, was it ever created?", (void*)commandBuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdSetBlendConstants(commandBuffer, blendConstants);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdSetDepthBounds(
    VkCommandBuffer                     commandBuffer,
    float                               minDepthBounds,
    float                               maxDepthBounds)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    MT_CB_INFO *pCmdBuf = get_cmd_buf_info(my_data, commandBuffer);
    if (!pCmdBuf) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                       MEMTRACK_INVALID_CB, "MEM", "Unable to find command buffer object %p, was it ever created?", (void*)commandBuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdSetDepthBounds(commandBuffer, minDepthBounds, maxDepthBounds);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdSetStencilCompareMask(
    VkCommandBuffer                     commandBuffer,
    VkStencilFaceFlags                  faceMask,
    uint32_t                            compareMask)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    MT_CB_INFO *pCmdBuf = get_cmd_buf_info(my_data, commandBuffer);
    if (!pCmdBuf) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                       MEMTRACK_INVALID_CB, "MEM", "Unable to find command buffer object %p, was it ever created?", (void*)commandBuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdSetStencilCompareMask(commandBuffer, faceMask, compareMask);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdSetStencilWriteMask(
    VkCommandBuffer                     commandBuffer,
    VkStencilFaceFlags                  faceMask,
    uint32_t                            writeMask)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    MT_CB_INFO *pCmdBuf = get_cmd_buf_info(my_data, commandBuffer);
    if (!pCmdBuf) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                       MEMTRACK_INVALID_CB, "MEM", "Unable to find command buffer object %p, was it ever created?", (void*)commandBuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdSetStencilWriteMask(commandBuffer, faceMask, writeMask);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdSetStencilReference(
    VkCommandBuffer                     commandBuffer,
    VkStencilFaceFlags                  faceMask,
    uint32_t                            reference)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    MT_CB_INFO *pCmdBuf = get_cmd_buf_info(my_data, commandBuffer);
    if (!pCmdBuf) {
        skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)commandBuffer, 0,
                       MEMTRACK_INVALID_CB, "MEM", "Unable to find command buffer object %p, was it ever created?", (void*)commandBuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdSetStencilReference(commandBuffer, faceMask, reference);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdBindDescriptorSets(
    VkCommandBuffer        commandBuffer,
    VkPipelineBindPoint    pipelineBindPoint,
    VkPipelineLayout       layout,
    uint32_t               firstSet,
    uint32_t               setCount,
    const VkDescriptorSet *pDescriptorSets,
    uint32_t               dynamicOffsetCount,
    const uint32_t        *pDynamicOffsets)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    // TODO : Somewhere need to verify that all textures referenced by shaders in DS are in some type of *SHADER_READ* state
    my_data->device_dispatch_table->CmdBindDescriptorSets(
        commandBuffer, pipelineBindPoint, layout, firstSet, setCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}

VK_LAYER_EXPORT void VKAPI vkCmdBindVertexBuffers(
    VkCommandBuffer     commandBuffer,
    uint32_t            startBinding,
    uint32_t            bindingCount,
    const VkBuffer     *pBuffers,
    const VkDeviceSize *pOffsets)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    // TODO : Somewhere need to verify that VBs have correct usage state flagged
    my_data->device_dispatch_table->CmdBindVertexBuffers(commandBuffer, startBinding, bindingCount, pBuffers, pOffsets);
}

VK_LAYER_EXPORT void VKAPI vkCmdBindIndexBuffer(
    VkCommandBuffer  commandBuffer,
    VkBuffer     buffer,
    VkDeviceSize offset,
    VkIndexType  indexType)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    // TODO : Somewhere need to verify that IBs have correct usage state flagged
    my_data->device_dispatch_table->CmdBindIndexBuffer(commandBuffer, buffer, offset, indexType);
}

VK_LAYER_EXPORT void VKAPI vkCmdDrawIndirect(
    VkCommandBuffer   commandBuffer,
     VkBuffer     buffer,
     VkDeviceSize offset,
     uint32_t     count,
     uint32_t     stride)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    loader_platform_thread_lock_mutex(&globalLock);
    VkBool32 skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)buffer, VK_OBJECT_TYPE_BUFFER, &mem);
    skipCall          |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdDrawIndirect");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdDrawIndirect(commandBuffer, buffer, offset, count, stride);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdDrawIndexedIndirect(
    VkCommandBuffer  commandBuffer,
    VkBuffer     buffer,
    VkDeviceSize offset,
    uint32_t     count,
    uint32_t     stride)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    loader_platform_thread_lock_mutex(&globalLock);
    VkBool32 skipCall = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)buffer, VK_OBJECT_TYPE_BUFFER, &mem);
    skipCall         |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdDrawIndexedIndirect");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdDrawIndexedIndirect(commandBuffer, buffer, offset, count, stride);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdDispatchIndirect(
    VkCommandBuffer  commandBuffer,
    VkBuffer     buffer,
    VkDeviceSize offset)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    loader_platform_thread_lock_mutex(&globalLock);
    VkBool32 skipCall = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)buffer, VK_OBJECT_TYPE_BUFFER, &mem);
    skipCall         |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdDispatchIndirect");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdDispatchIndirect(commandBuffer, buffer, offset);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdCopyBuffer(
    VkCommandBuffer     commandBuffer,
    VkBuffer            srcBuffer,
    VkBuffer            dstBuffer,
    uint32_t            regionCount,
    const VkBufferCopy *pRegions)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    VkBool32       skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)srcBuffer, VK_OBJECT_TYPE_BUFFER, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdCopyBuffer");
    skipCall |= get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)dstBuffer, VK_OBJECT_TYPE_BUFFER, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdCopyBuffer");
    // Validate that SRC & DST buffers have correct usage flags set
    skipCall |= validate_buffer_usage_flags(my_data, commandBuffer, srcBuffer, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, "vkCmdCopyBuffer()", "VK_BUFFER_USAGE_TRANSFER_SRC_BIT");
    skipCall |= validate_buffer_usage_flags(my_data, commandBuffer, dstBuffer, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, "vkCmdCopyBuffer()", "VK_BUFFER_USAGE_TRANSFER_DST_BIT");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdCopyQueryPoolResults(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    startQuery,
    uint32_t                                    queryCount,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                destStride,
    VkQueryResultFlags                          flags)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    VkBool32       skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    skipCall |= get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)dstBuffer, VK_OBJECT_TYPE_BUFFER, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdCopyQueryPoolResults");
    // Validate that DST buffer has correct usage flags set
    skipCall |= validate_buffer_usage_flags(my_data, commandBuffer, dstBuffer, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, "vkCmdCopyQueryPoolResults()", "VK_BUFFER_USAGE_TRANSFER_DST_BIT");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdCopyQueryPoolResults(commandBuffer, queryPool, startQuery, queryCount, dstBuffer, dstOffset, destStride, flags);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdCopyImage(
    VkCommandBuffer    commandBuffer,
    VkImage            srcImage,
    VkImageLayout      srcImageLayout,
    VkImage            dstImage,
    VkImageLayout      dstImageLayout,
    uint32_t           regionCount,
    const VkImageCopy *pRegions)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    VkBool32       skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    // Validate that src & dst images have correct usage flags set
    skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)srcImage, VK_OBJECT_TYPE_IMAGE, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdCopyImage");
    skipCall |= get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)dstImage, VK_OBJECT_TYPE_IMAGE, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdCopyImage");
    skipCall |= validate_image_usage_flags(my_data, commandBuffer, srcImage, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true, "vkCmdCopyImage()", "VK_IMAGE_USAGE_TRANSFER_SRC_BIT");
    skipCall |= validate_image_usage_flags(my_data, commandBuffer, dstImage, VK_IMAGE_USAGE_TRANSFER_DST_BIT, true, "vkCmdCopyImage()", "VK_IMAGE_USAGE_TRANSFER_DST_BIT");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdCopyImage(
            commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdBlitImage(
    VkCommandBuffer        commandBuffer,
    VkImage            srcImage,
    VkImageLayout      srcImageLayout,
    VkImage            dstImage,
    VkImageLayout      dstImageLayout,
    uint32_t           regionCount,
    const VkImageBlit *pRegions,
    VkFilter        filter)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    VkBool32       skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    // Validate that src & dst images have correct usage flags set
    skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)srcImage, VK_OBJECT_TYPE_IMAGE, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdBlitImage");
    skipCall |= get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)dstImage, VK_OBJECT_TYPE_IMAGE, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdBlitImage");
    skipCall |= validate_image_usage_flags(my_data, commandBuffer, srcImage, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true, "vkCmdBlitImage()", "VK_IMAGE_USAGE_TRANSFER_SRC_BIT");
    skipCall |= validate_image_usage_flags(my_data, commandBuffer, dstImage, VK_IMAGE_USAGE_TRANSFER_DST_BIT, true, "vkCmdBlitImage()", "VK_IMAGE_USAGE_TRANSFER_DST_BIT");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdBlitImage(
            commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdCopyBufferToImage(
    VkCommandBuffer              commandBuffer,
    VkBuffer                 srcBuffer,
    VkImage                  dstImage,
    VkImageLayout            dstImageLayout,
    uint32_t                 regionCount,
    const VkBufferImageCopy *pRegions)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    VkBool32       skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)dstImage, VK_OBJECT_TYPE_IMAGE, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdCopyBufferToImage");
    skipCall |= get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)srcBuffer, VK_OBJECT_TYPE_BUFFER, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdCopyBufferToImage");
    // Validate that src buff & dst image have correct usage flags set
    skipCall |= validate_buffer_usage_flags(my_data, commandBuffer, srcBuffer, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, "vkCmdCopyBufferToImage()", "VK_BUFFER_USAGE_TRANSFER_SRC_BIT");
    skipCall |= validate_image_usage_flags(my_data, commandBuffer, dstImage, VK_IMAGE_USAGE_TRANSFER_DST_BIT, true, "vkCmdCopyBufferToImage()", "VK_IMAGE_USAGE_TRANSFER_DST_BIT");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdCopyBufferToImage(
        commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdCopyImageToBuffer(
    VkCommandBuffer              commandBuffer,
    VkImage                  srcImage,
    VkImageLayout            srcImageLayout,
    VkBuffer                 dstBuffer,
    uint32_t                 regionCount,
    const VkBufferImageCopy *pRegions)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    VkBool32       skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)srcImage, VK_OBJECT_TYPE_IMAGE, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdCopyImageToBuffer");
    skipCall |= get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)dstBuffer, VK_OBJECT_TYPE_BUFFER, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdCopyImageToBuffer");
    // Validate that dst buff & src image have correct usage flags set
    skipCall |= validate_image_usage_flags(my_data, commandBuffer, srcImage, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true, "vkCmdCopyImageToBuffer()", "VK_IMAGE_USAGE_TRANSFER_SRC_BIT");
    skipCall |= validate_buffer_usage_flags(my_data, commandBuffer, dstBuffer, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, "vkCmdCopyImageToBuffer()", "VK_BUFFER_USAGE_TRANSFER_DST_BIT");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdCopyImageToBuffer(
            commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdUpdateBuffer(
    VkCommandBuffer     commandBuffer,
    VkBuffer        dstBuffer,
    VkDeviceSize    dstOffset,
    VkDeviceSize    dataSize,
    const uint32_t *pData)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    VkBool32       skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)dstBuffer, VK_OBJECT_TYPE_BUFFER, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdUpdateBuffer");
    // Validate that dst buff has correct usage flags set
    skipCall |= validate_buffer_usage_flags(my_data, commandBuffer, dstBuffer, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, "vkCmdUpdateBuffer()", "VK_BUFFER_USAGE_TRANSFER_DST_BIT");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdUpdateBuffer(commandBuffer, dstBuffer, dstOffset, dataSize, pData);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdFillBuffer(
    VkCommandBuffer  commandBuffer,
    VkBuffer     dstBuffer,
    VkDeviceSize dstOffset,
    VkDeviceSize size,
    uint32_t     data)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkDeviceMemory mem;
    VkBool32       skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)dstBuffer, VK_OBJECT_TYPE_BUFFER, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdFillBuffer");
    // Validate that dst buff has correct usage flags set
    skipCall |= validate_buffer_usage_flags(my_data, commandBuffer, dstBuffer, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, "vkCmdFillBuffer()", "VK_BUFFER_USAGE_TRANSFER_DST_BIT");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdFillBuffer(commandBuffer, dstBuffer, dstOffset, size, data);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdClearColorImage(
    VkCommandBuffer                    commandBuffer,
    VkImage                        image,
    VkImageLayout                  imageLayout,
    const VkClearColorValue       *pColor,
    uint32_t                       rangeCount,
    const VkImageSubresourceRange *pRanges)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    // TODO : Verify memory is in VK_IMAGE_STATE_CLEAR state
    VkDeviceMemory mem;
    VkBool32       skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)image, VK_OBJECT_TYPE_IMAGE, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdClearColorImage");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdClearColorImage(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdClearDepthStencilImage(
    VkCommandBuffer                         commandBuffer,
    VkImage                             image,
    VkImageLayout                       imageLayout,
    const VkClearDepthStencilValue*     pDepthStencil,
    uint32_t                            rangeCount,
    const VkImageSubresourceRange*      pRanges)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    // TODO : Verify memory is in VK_IMAGE_STATE_CLEAR state
    VkDeviceMemory mem;
    VkBool32       skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)image, VK_OBJECT_TYPE_IMAGE, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdClearDepthStencilImage");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdClearDepthStencilImage(
            commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdResolveImage(
    VkCommandBuffer           commandBuffer,
    VkImage               srcImage,
    VkImageLayout         srcImageLayout,
    VkImage               dstImage,
    VkImageLayout         dstImageLayout,
    uint32_t              regionCount,
    const VkImageResolve *pRegions)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    loader_platform_thread_lock_mutex(&globalLock);
    VkDeviceMemory mem;
    skipCall  = get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)srcImage, VK_OBJECT_TYPE_IMAGE, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdResolveImage");
    skipCall |= get_mem_binding_from_object(my_data, commandBuffer, (uint64_t)dstImage, VK_OBJECT_TYPE_IMAGE, &mem);
    skipCall |= update_cmd_buf_and_mem_references(my_data, commandBuffer, mem, "vkCmdResolveImage");
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        my_data->device_dispatch_table->CmdResolveImage(
            commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdBeginQuery(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t    slot,
    VkFlags     flags)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    my_data->device_dispatch_table->CmdBeginQuery(commandBuffer, queryPool, slot, flags);
}

VK_LAYER_EXPORT void VKAPI vkCmdEndQuery(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t    slot)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    my_data->device_dispatch_table->CmdEndQuery(commandBuffer, queryPool, slot);
}

VK_LAYER_EXPORT void VKAPI vkCmdResetQueryPool(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t    startQuery,
    uint32_t    queryCount)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(commandBuffer), layer_data_map);
    my_data->device_dispatch_table->CmdResetQueryPool(commandBuffer, queryPool, startQuery, queryCount);
}

VK_LAYER_EXPORT VkResult VKAPI vkDbgCreateMsgCallback(
        VkInstance instance,
        VkFlags msgFlags,
        const PFN_vkDbgMsgCallback pfnMsgCallback,
        void* pUserData,
        VkDbgMsgCallback* pMsgCallback)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(instance), layer_data_map);
    VkLayerInstanceDispatchTable *pTable = my_data->instance_dispatch_table;
    VkResult res =  pTable->DbgCreateMsgCallback(instance, msgFlags, pfnMsgCallback, pUserData, pMsgCallback);
    if (res == VK_SUCCESS) {
        res = layer_create_msg_callback(my_data->report_data, msgFlags, pfnMsgCallback, pUserData, pMsgCallback);
    }
    return res;
}

VK_LAYER_EXPORT VkResult VKAPI vkDbgDestroyMsgCallback(
        VkInstance instance,
        VkDbgMsgCallback msgCallback)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(instance), layer_data_map);
    VkLayerInstanceDispatchTable *pTable = my_data->instance_dispatch_table;
    VkResult res =  pTable->DbgDestroyMsgCallback(instance, msgCallback);
    layer_destroy_msg_callback(my_data->report_data, msgCallback);

    return res;
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateSwapchainKHR(
    VkDevice                        device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    VkSwapchainKHR                 *pSwapchain)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->CreateSwapchainKHR(device, pCreateInfo, pSwapchain);

    if (VK_SUCCESS == result) {
        loader_platform_thread_lock_mutex(&globalLock);
        add_swap_chain_info(my_data, *pSwapchain, pCreateInfo);
        loader_platform_thread_unlock_mutex(&globalLock);
    }

    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkDestroySwapchainKHR(
    VkDevice                        device,
    VkSwapchainKHR swapchain)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkBool32 skipCall = VK_FALSE;
    VkResult result   = VK_ERROR_VALIDATION_FAILED;
    loader_platform_thread_lock_mutex(&globalLock);
    if (my_data->swapchainMap.find(swapchain) != my_data->swapchainMap.end()) {
        MT_SWAP_CHAIN_INFO* pInfo = my_data->swapchainMap[swapchain];

        if (pInfo->images.size() > 0) {
            for (auto it = pInfo->images.begin(); it != pInfo->images.end(); it++) {
                skipCall = clear_object_binding(my_data, device, (uint64_t)*it, VK_OBJECT_TYPE_SWAPCHAIN_KHR);
                auto image_item = my_data->imageMap.find((uint64_t)*it);
                if (image_item != my_data->imageMap.end())
                    my_data->imageMap.erase(image_item);
            }
        }
        delete pInfo;
        my_data->swapchainMap.erase(swapchain);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        result = my_data->device_dispatch_table->DestroySwapchainKHR(device, swapchain);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkGetSwapchainImagesKHR(
    VkDevice                device,
    VkSwapchainKHR          swapchain,
    uint32_t*               pCount,
    VkImage*                pSwapchainImages)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);

    if (result == VK_SUCCESS && pSwapchainImages != NULL) {
        const size_t count = *pCount;
        MT_SWAP_CHAIN_INFO *pInfo = my_data->swapchainMap[swapchain];

        if (pInfo->images.empty()) {
            pInfo->images.resize(count);
            memcpy(&pInfo->images[0], pSwapchainImages, sizeof(pInfo->images[0]) * count);

            if (pInfo->images.size() > 0) {
                for (std::vector<VkImage>::const_iterator it = pInfo->images.begin();
                     it != pInfo->images.end(); it++) {
                    // Add image object binding, then insert the new Mem Object and then bind it to created image
                    add_object_create_info(my_data, (uint64_t)*it, VK_OBJECT_TYPE_SWAPCHAIN_KHR, &pInfo->createInfo);
                }
            }
        } else {
            const size_t count = *pCount;
            MT_SWAP_CHAIN_INFO *pInfo = my_data->swapchainMap[swapchain];
            const bool mismatch = (pInfo->images.size() != count ||
                    memcmp(&pInfo->images[0], pSwapchainImages, sizeof(pInfo->images[0]) * count));

            if (mismatch) {
                log_msg(my_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_SWAPCHAIN_KHR, (uint64_t) swapchain, 0, MEMTRACK_NONE, "SWAP_CHAIN",
                        "vkGetSwapchainInfoKHR(%p, VK_SWAP_CHAIN_INFO_TYPE_PERSISTENT_IMAGES_KHR) returned mismatching data", swapchain);
            }
        }
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkAcquireNextImageKHR(
    VkDevice        device,
    VkSwapchainKHR  swapchain,
    uint64_t        timeout,
    VkSemaphore     semaphore,
    uint32_t       *pImageIndex)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result   = VK_ERROR_VALIDATION_FAILED;
    VkBool32 skipCall = VK_FALSE;

    loader_platform_thread_lock_mutex(&globalLock);
    if (my_data->semaphoreMap.find(semaphore) != my_data->semaphoreMap.end()) {
        if (my_data->semaphoreMap[semaphore] != MEMTRACK_SEMAPHORE_STATE_UNSET) {
            skipCall = log_msg(my_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_SEMAPHORE, (uint64_t)semaphore,
                               0, MEMTRACK_NONE, "SEMAPHORE",
                               "vkAcquireNextImageKHR: Semaphore must not be currently signaled or in a wait state");
        }
        my_data->semaphoreMap[semaphore] = MEMTRACK_SEMAPHORE_STATE_SIGNALLED;
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    if (VK_FALSE == skipCall) {
        result = my_data->device_dispatch_table->AcquireNextImageKHR(device,
                                    swapchain, timeout, semaphore, pImageIndex);
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateSemaphore(
    VkDevice                     device,
    const VkSemaphoreCreateInfo *pCreateInfo,
    const VkAllocationCallbacks*      pAllocator,
    VkSemaphore                 *pSemaphore)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkResult result = my_data->device_dispatch_table->CreateSemaphore(device, pCreateInfo, pAllocator, pSemaphore);
    loader_platform_thread_lock_mutex(&globalLock);
    if (*pSemaphore != VK_NULL_HANDLE) {
        my_data->semaphoreMap[*pSemaphore] = MEMTRACK_SEMAPHORE_STATE_UNSET;
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    return result;
}

VK_LAYER_EXPORT void VKAPI vkDestroySemaphore(
    VkDevice    device,
    VkSemaphore semaphore,
    const VkAllocationCallbacks* pAllocator)
{
    layer_data *my_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    loader_platform_thread_lock_mutex(&globalLock);
    auto item = my_data->semaphoreMap.find(semaphore);
    if (item != my_data->semaphoreMap.end()) {
        my_data->semaphoreMap.erase(item);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    my_data->device_dispatch_table->DestroySemaphore(device, semaphore, pAllocator);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI vkGetDeviceProcAddr(
    VkDevice         dev,
    const char      *funcName)
{
    if (dev == NULL)
        return NULL;

    layer_data *my_data;
    /* loader uses this to force layer initialization; device object is wrapped */
    if (!strcmp(funcName, "vkGetDeviceProcAddr")) {
        VkBaseLayerObject* wrapped_dev = (VkBaseLayerObject*) dev;
        my_data = get_my_data_ptr(get_dispatch_key(wrapped_dev->baseObject), layer_data_map);
        my_data->device_dispatch_table = new VkLayerDispatchTable;
        layer_initialize_dispatch_table(my_data->device_dispatch_table, wrapped_dev);
        return (PFN_vkVoidFunction) vkGetDeviceProcAddr;
    }
    if (!strcmp(funcName, "vkCreateDevice"))
        return (PFN_vkVoidFunction) vkCreateDevice;
    if (!strcmp(funcName, "vkDestroyDevice"))
        return (PFN_vkVoidFunction) vkDestroyDevice;
    if (!strcmp(funcName, "vkQueueSubmit"))
        return (PFN_vkVoidFunction) vkQueueSubmit;
    if (!strcmp(funcName, "vkAllocateMemory"))
        return (PFN_vkVoidFunction) vkAllocateMemory;
    if (!strcmp(funcName, "vkFreeMemory"))
        return (PFN_vkVoidFunction) vkFreeMemory;
    if (!strcmp(funcName, "vkMapMemory"))
        return (PFN_vkVoidFunction) vkMapMemory;
    if (!strcmp(funcName, "vkUnmapMemory"))
        return (PFN_vkVoidFunction) vkUnmapMemory;
    if (!strcmp(funcName, "vkDestroyFence"))
        return (PFN_vkVoidFunction) vkDestroyFence;
    if (!strcmp(funcName, "vkDestroyBuffer"))
        return (PFN_vkVoidFunction) vkDestroyBuffer;
    if (!strcmp(funcName, "vkDestroyImage"))
        return (PFN_vkVoidFunction) vkDestroyImage;
    if (!strcmp(funcName, "vkBindBufferMemory"))
        return (PFN_vkVoidFunction) vkBindBufferMemory;
    if (!strcmp(funcName, "vkBindImageMemory"))
        return (PFN_vkVoidFunction) vkBindImageMemory;
    if (!strcmp(funcName, "vkGetBufferMemoryRequirements"))
        return (PFN_vkVoidFunction) vkGetBufferMemoryRequirements;
    if (!strcmp(funcName, "vkGetImageMemoryRequirements"))
        return (PFN_vkVoidFunction) vkGetImageMemoryRequirements;
    if (!strcmp(funcName, "vkQueueBindSparse"))
        return (PFN_vkVoidFunction) vkQueueBindSparse;
    if (!strcmp(funcName, "vkCreateFence"))
        return (PFN_vkVoidFunction) vkCreateFence;
    if (!strcmp(funcName, "vkGetFenceStatus"))
        return (PFN_vkVoidFunction) vkGetFenceStatus;
    if (!strcmp(funcName, "vkResetFences"))
        return (PFN_vkVoidFunction) vkResetFences;
    if (!strcmp(funcName, "vkWaitForFences"))
        return (PFN_vkVoidFunction) vkWaitForFences;
    if (!strcmp(funcName, "vkCreateSemaphore"))
        return (PFN_vkVoidFunction) vkCreateSemaphore;
    if (!strcmp(funcName, "vkDestroySemaphore"))
        return (PFN_vkVoidFunction) vkDestroySemaphore;
    if (!strcmp(funcName, "vkQueueWaitIdle"))
        return (PFN_vkVoidFunction) vkQueueWaitIdle;
    if (!strcmp(funcName, "vkDeviceWaitIdle"))
        return (PFN_vkVoidFunction) vkDeviceWaitIdle;
    if (!strcmp(funcName, "vkCreateBuffer"))
        return (PFN_vkVoidFunction) vkCreateBuffer;
    if (!strcmp(funcName, "vkCreateImage"))
        return (PFN_vkVoidFunction) vkCreateImage;
    if (!strcmp(funcName, "vkCreateImageView"))
        return (PFN_vkVoidFunction) vkCreateImageView;
    if (!strcmp(funcName, "vkCreateBufferView"))
        return (PFN_vkVoidFunction) vkCreateBufferView;
    if (!strcmp(funcName, "vkAllocateCommandBuffers"))
        return (PFN_vkVoidFunction) vkAllocateCommandBuffers;
    if (!strcmp(funcName, "vkBeginCommandBuffer"))
        return (PFN_vkVoidFunction) vkBeginCommandBuffer;
    if (!strcmp(funcName, "vkEndCommandBuffer"))
        return (PFN_vkVoidFunction) vkEndCommandBuffer;
    if (!strcmp(funcName, "vkResetCommandBuffer"))
        return (PFN_vkVoidFunction) vkResetCommandBuffer;
    if (!strcmp(funcName, "vkCmdBindPipeline"))
        return (PFN_vkVoidFunction) vkCmdBindPipeline;
    if (!strcmp(funcName, "vkCmdSetViewport"))
        return (PFN_vkVoidFunction) vkCmdSetViewport;
    if (!strcmp(funcName, "vkCmdSetScissor"))
        return (PFN_vkVoidFunction) vkCmdSetScissor;
    if (!strcmp(funcName, "vkCmdSetLineWidth"))
        return (PFN_vkVoidFunction) vkCmdSetLineWidth;
    if (!strcmp(funcName, "vkCmdSetDepthBias"))
        return (PFN_vkVoidFunction) vkCmdSetDepthBias;
    if (!strcmp(funcName, "vkCmdSetBlendConstants"))
        return (PFN_vkVoidFunction) vkCmdSetBlendConstants;
    if (!strcmp(funcName, "vkCmdSetDepthBounds"))
        return (PFN_vkVoidFunction) vkCmdSetDepthBounds;
    if (!strcmp(funcName, "vkCmdSetStencilCompareMask"))
        return (PFN_vkVoidFunction) vkCmdSetStencilCompareMask;
    if (!strcmp(funcName, "vkCmdSetStencilWriteMask"))
        return (PFN_vkVoidFunction) vkCmdSetStencilWriteMask;
    if (!strcmp(funcName, "vkCmdSetStencilReference"))
        return (PFN_vkVoidFunction) vkCmdSetStencilReference;
    if (!strcmp(funcName, "vkCmdBindDescriptorSets"))
        return (PFN_vkVoidFunction) vkCmdBindDescriptorSets;
    if (!strcmp(funcName, "vkCmdBindVertexBuffers"))
        return (PFN_vkVoidFunction) vkCmdBindVertexBuffers;
    if (!strcmp(funcName, "vkCmdBindIndexBuffer"))
        return (PFN_vkVoidFunction) vkCmdBindIndexBuffer;
    if (!strcmp(funcName, "vkCmdDrawIndirect"))
        return (PFN_vkVoidFunction) vkCmdDrawIndirect;
    if (!strcmp(funcName, "vkCmdDrawIndexedIndirect"))
        return (PFN_vkVoidFunction) vkCmdDrawIndexedIndirect;
    if (!strcmp(funcName, "vkCmdDispatchIndirect"))
        return (PFN_vkVoidFunction)vkCmdDispatchIndirect;
    if (!strcmp(funcName, "vkCmdCopyBuffer"))
        return (PFN_vkVoidFunction)vkCmdCopyBuffer;
    if (!strcmp(funcName, "vkCmdCopyQueryPoolResults"))
        return (PFN_vkVoidFunction)vkCmdCopyQueryPoolResults;
    if (!strcmp(funcName, "vkCmdCopyImage"))
        return (PFN_vkVoidFunction) vkCmdCopyImage;
    if (!strcmp(funcName, "vkCmdCopyBufferToImage"))
        return (PFN_vkVoidFunction) vkCmdCopyBufferToImage;
    if (!strcmp(funcName, "vkCmdCopyImageToBuffer"))
        return (PFN_vkVoidFunction) vkCmdCopyImageToBuffer;
    if (!strcmp(funcName, "vkCmdUpdateBuffer"))
        return (PFN_vkVoidFunction) vkCmdUpdateBuffer;
    if (!strcmp(funcName, "vkCmdFillBuffer"))
        return (PFN_vkVoidFunction) vkCmdFillBuffer;
    if (!strcmp(funcName, "vkCmdClearColorImage"))
        return (PFN_vkVoidFunction) vkCmdClearColorImage;
    if (!strcmp(funcName, "vkCmdClearDepthStencilImage"))
        return (PFN_vkVoidFunction) vkCmdClearDepthStencilImage;
    if (!strcmp(funcName, "vkCmdResolveImage"))
        return (PFN_vkVoidFunction) vkCmdResolveImage;
    if (!strcmp(funcName, "vkCmdBeginQuery"))
        return (PFN_vkVoidFunction) vkCmdBeginQuery;
    if (!strcmp(funcName, "vkCmdEndQuery"))
        return (PFN_vkVoidFunction) vkCmdEndQuery;
    if (!strcmp(funcName, "vkCmdResetQueryPool"))
        return (PFN_vkVoidFunction) vkCmdResetQueryPool;
    if (!strcmp(funcName, "vkGetDeviceQueue"))
        return (PFN_vkVoidFunction) vkGetDeviceQueue;

    my_data = get_my_data_ptr(get_dispatch_key(dev), layer_data_map);
    if (my_data->wsi_enabled)
    {
        if (!strcmp(funcName, "vkCreateSwapchainKHR"))
            return (PFN_vkVoidFunction) vkCreateSwapchainKHR;
        if (!strcmp(funcName, "vkDestroySwapchainKHR"))
            return (PFN_vkVoidFunction) vkDestroySwapchainKHR;
        if (!strcmp(funcName, "vkGetSwapchainImagesKHR"))
            return (PFN_vkVoidFunction) vkGetSwapchainImagesKHR;
        if (!strcmp(funcName, "vkAcquireNextImageKHR"))
            return (PFN_vkVoidFunction)vkAcquireNextImageKHR;
    }

    VkLayerDispatchTable *pDisp = my_data->device_dispatch_table;
    if (pDisp->GetDeviceProcAddr == NULL)
        return NULL;
    return pDisp->GetDeviceProcAddr(dev, funcName);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI vkGetInstanceProcAddr(
    VkInstance       instance,
    const char       *funcName)
{
    PFN_vkVoidFunction fptr;
    if (instance == NULL)
        return NULL;

    layer_data *my_data;
    /* loader uses this to force layer initialization; instance object is wrapped */
    if (!strcmp(funcName, "vkGetInstanceProcAddr")) {
        VkBaseLayerObject* wrapped_inst = (VkBaseLayerObject*) instance;
        my_data = get_my_data_ptr(get_dispatch_key(wrapped_inst->baseObject), layer_data_map);
        my_data->instance_dispatch_table = new VkLayerInstanceDispatchTable;
        layer_init_instance_dispatch_table(my_data->instance_dispatch_table, wrapped_inst);
        return (PFN_vkVoidFunction) vkGetInstanceProcAddr;
    }
    my_data = get_my_data_ptr(get_dispatch_key(instance), layer_data_map);
    if (!strcmp(funcName, "vkDestroyInstance"))
        return (PFN_vkVoidFunction) vkDestroyInstance;
    if (!strcmp(funcName, "vkCreateInstance"))
        return (PFN_vkVoidFunction) vkCreateInstance;
    if (!strcmp(funcName, "vkGetPhysicalDeviceMemoryProperties"))
        return (PFN_vkVoidFunction) vkGetPhysicalDeviceMemoryProperties;
    if (!strcmp(funcName, "vkEnumerateInstanceLayerProperties"))
        return (PFN_vkVoidFunction) vkEnumerateInstanceLayerProperties;
    if (!strcmp(funcName, "vkEnumerateInstanceExtensionProperties"))
        return (PFN_vkVoidFunction) vkEnumerateInstanceExtensionProperties;
    if (!strcmp(funcName, "vkEnumerateDeviceLayerProperties"))
        return (PFN_vkVoidFunction) vkEnumerateDeviceLayerProperties;
    if (!strcmp(funcName, "vkEnumerateDeviceExtensionProperties"))
        return (PFN_vkVoidFunction) vkEnumerateDeviceExtensionProperties;

    fptr = debug_report_get_instance_proc_addr(my_data->report_data, funcName);
    if (fptr)
        return fptr;

    {
        VkLayerInstanceDispatchTable* pTable = my_data->instance_dispatch_table;
        if (pTable->GetInstanceProcAddr == NULL)
            return NULL;
        return pTable->GetInstanceProcAddr(instance, funcName);
    }
}
