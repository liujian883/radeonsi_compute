#include <stdexcept>
#include <assert.h>
#include "compute_interface.hpp"

extern "C" {
#include "computesi.h"
};

ComputeInterface::ComputeInterface(std::string driName)
{
	context = compute_create_context(driName.c_str());
	
	if (!context)
	{
		throw std::runtime_error("Could not open DRI interface: " + driName);
	}
}

ComputeInterface::~ComputeInterface()
{
	compute_free_context(context);
}

gpu_buffer* ComputeInterface::bufferAlloc(size_t size)
{
	return compute_alloc_gpu_buffer(context, size, RADEON_DOMAIN_VRAM, 8*1024);
}

void ComputeInterface::bufferFree(gpu_buffer* buf)
{
	compute_free_gpu_buffer(buf);
}

void ComputeInterface::transferToGPU(gpu_buffer* buf, size_t offset, void* data, size_t size, EventDependence evd)
{
	compute_copy_to_gpu(buf, offset, data, size);
}

void ComputeInterface::transferFromGPU(gpu_buffer* buf, size_t offset, void* data, size_t size, EventDependence evd)
{
	compute_copy_from_gpu(buf, offset, data, size);
}

void ComputeInterface::launch(std::vector<uint32_t> userData, std::vector<size_t> threadOffset, std::vector<size_t> blockDim, std::vector<size_t> localSize, gpu_buffer* code, EventDependence evd)
{
	assert(localSize.size() == blockDim.size());
	assert(localSize.size() <= 3);
	assert(localSize.size() > 0);
	assert(userData.size() <= 16);
	
	localSize.resize(3, 1);
	blockDim.resize(3, 1);
	threadOffset.resize(3, 0);
	
	compute_state state;
	
	state.id = 0;
	state.user_data_length = userData.size();

	for (unsigned i = 0; i < userData.size(); i++)
	{
		state.user_data[i] = userData[i];
	}
	
	state.dim[0] = blockDim[0];
	state.dim[1] = blockDim[1];
	state.dim[2] = blockDim[2];
	state.start[0] = threadOffset[0];
	state.start[1] = threadOffset[1];
	state.start[2] = threadOffset[2];
	state.num_thread[0] = localSize[0];
	state.num_thread[1] = localSize[1];
	state.num_thread[2] = localSize[2];
	
	state.sgpr_num = 105;
	state.vgpr_num = 255;
	state.priority = 0;
	state.debug_mode = 0;
	state.ieee_mode = 0;
	state.scratch_en = 0;
	state.lds_size = 128; ///32K
	state.excp_en = 0;
	state.waves_per_sh = 1;
	state.thread_groups_per_cu = 1;
	state.lock_threshold = 0;
	state.simd_dest_cntl = 0;
	state.se0_sh0_cu_en = 0xFF;
	state.se0_sh1_cu_en = 0xFF;
	state.se1_sh0_cu_en = 0xFF;
	state.se1_sh1_cu_en = 0xFF;
	state.tmpring_waves = 0;
	state.tmpring_wavesize = 0;
	state.binary = code;

	int ret = compute_emit_compute_state(context, &state);

	if (ret != 0)
	{
		throw std::runtime_error("Error while running kernel: " + std::string(strerror(errno)));
	}
	
	compute_flush_caches(context);
}

